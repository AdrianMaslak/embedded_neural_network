/*
 * Assignment 2 - bare-metal application for the PYNQ-Z2, DFX version (bonus task)
 *
 * One reconfigurable partition at 0x4000_0000 holds either the CNN (cnn_rm) or the
 * SNN (snn_rm). Both modules have the same AXI-Lite register map and both read their
 * input from DDR through S_AXI_HP0. Each module writes its id (1 = CNN, 2 = SNN) to
 * the NET_ID register when it runs, so the firmware knows which one is loaded and
 * answers ST_NO_NET when the requested network is not in the partition.
 * Swap modules with a partial bitstream (dfx/firmware/swap.tcl) while this runs.
 *
 * Original description of the combined design follows.
 *
 *
 *   PC --UART--> CPU --(DDR, m_axi)--> CNN HLS IP --AXI-Lite--> CPU --UART--> PC
 *                    --(AXI BRAM)----> SNN HLS IP
 *
 * - Receives framed 60x80 8-bit images (see protocol.h), checks length + CRC.
 * - CNN: CPU converts pixels to Q8.8, writes them to a DDR buffer, flushes the
 *   D-cache and passes the buffer address; the IP fetches it over S_AXI_HP0.
 * - SNN: CPU computes the 512 edge-spike features, writes them into the AXI
 *   BRAM; the IP reads them through its m_axi port from the BRAM controller.
 * - Measures receive, preprocessing and accelerator time with the global timer.
 *
 * The networks present in the hardware are detected from xparameters.h, so
 * the same source builds for the combined design or for separate CNN / SNN
 * designs (absent networks answer ST_NO_NET).
 */
#include <stdint.h>
#include <string.h>

#include "xparameters.h"
#include "xil_io.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "xuartps.h"
#include "xuartps_hw.h"

/* Cortex-A9 global timer (64-bit, clocked at CPU_3x2x = CPU clock / 2).
 * Driven directly: the Vitis 2026.1 xiltimer BSP reads this timer but never
 * starts it when the program is loaded over JTAG, so XTime_GetTime() returns
 * a constant and every timeout and measurement breaks. */
#define GT_BASE      0xF8F00200u
#define GT_CNT_LO    (GT_BASE + 0x0)
#define GT_CNT_HI    (GT_BASE + 0x4)
#define GT_CTRL      (GT_BASE + 0x8)
#define TIMER_HZ     ((uint64_t)XPAR_CPU_CORE_CLOCK_FREQ_HZ / 2u)

typedef uint64_t XTime;

static void timer_start(void)
{
    if (!(Xil_In32(GT_CTRL) & 1u)) {
        Xil_Out32(GT_CTRL, 0);
        Xil_Out32(GT_CNT_LO, 0);
        Xil_Out32(GT_CNT_HI, 0);
        Xil_Out32(GT_CTRL, 1);          /* enable, prescaler 0 */
    }
}

static inline void XTime_GetTime(XTime *t)
{
    uint32_t hi, lo;
    do {                                /* re-read if the low word wrapped */
        hi = Xil_In32(GT_CNT_HI);
        lo = Xil_In32(GT_CNT_LO);
    } while (hi != Xil_In32(GT_CNT_HI));
    *t = ((uint64_t)hi << 32) | lo;
}

#include "protocol.h"
#include "preprocess.h"

#ifndef UART_BAUD
#define UART_BAUD 4000000u
#endif
#define IP_TIMEOUT_US 1000000u

/* ---------------- address map (SDT and classic xparameters names) -------- */
#if defined(XPAR_XUARTPS_0_BASEADDR)
#define UART_BASE XPAR_XUARTPS_0_BASEADDR
#else
#define UART_BASE XPAR_PS7_UART_0_BASEADDR
#endif

/* reconfigurable partition: fixed address set in dfx/vivado/build_dfx.tcl */
#define RP_BASE   0x40000000u
#define CNN_BASE  RP_BASE
#define SNN_BASE  RP_BASE
#define HAVE_CNN  1
#define HAVE_SNN  1

/* HLS register map of cnn_rm / snn_rm (identical, see x*_rm_hw.h) */
#define AP_CTRL            0x00
#define AP_START           0x01
#define AP_DONE            0x02
#define REG_INPUT_L        0x10
#define REG_INPUT_H        0x14
#define REG_PRED           0x1c
#define REG_LOGITS         0x30
#define REG_NET_ID         0x40

/* ---------------- buffers ---------------- */
static uint8_t rx_payload[MAX_PAYLOAD];
static uint8_t tx_payload[MAX_PAYLOAD];
static int16_t cnn_in[IMG_BYTES] __attribute__((aligned(64)));   /* DDR, read by CNN m_axi */
static int16_t snn_feat[SNN_N_FEATURES] __attribute__((aligned(64)));  /* DDR, read by SNN m_axi */

/* ---------------- timing ---------------- */
static inline uint32_t us_since(XTime t0)
{
    XTime t1;
    XTime_GetTime(&t1);
    return (uint32_t)(((t1 - t0) * 1000000ULL) / TIMER_HZ);
}

/* ---------------- UART ---------------- */
static XUartPs uart;

static void uart_init(void)
{
#ifdef SDT
    XUartPs_Config *cfg = XUartPs_LookupConfig(UART_BASE);
#else
    XUartPs_Config *cfg = XUartPs_LookupConfig(XPAR_XUARTPS_0_DEVICE_ID);
#endif
    XUartPs_CfgInitialize(&uart, cfg, cfg->BaseAddress);
    XUartPs_SetOperMode(&uart, XUARTPS_OPER_MODE_NORMAL);
    XUartPs_SetBaudRate(&uart, UART_BAUD);
}

static inline uint8_t rx_byte(void) { return XUartPs_RecvByte(UART_BASE); }
static inline void tx_byte(uint8_t b) { XUartPs_SendByte(UART_BASE, b); }

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put_u32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void send_response(uint8_t cmd, uint8_t status, const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[4] = {cmd, status, (uint8_t)len, (uint8_t)(len >> 8)};
    uint16_t crc = crc16_ccitt(0xFFFF, hdr, 4);
    crc = crc16_ccitt(crc, payload, len);
    tx_byte(SYNC_RSP_0);
    tx_byte(SYNC_RSP_1);
    for (int i = 0; i < 4; i++) tx_byte(hdr[i]);
    for (int i = 0; i < len; i++) tx_byte(payload[i]);
    tx_byte((uint8_t)crc);
    tx_byte((uint8_t)(crc >> 8));
}

/* ---------------- accelerators ---------------- */
static int wait_done(UINTPTR base)
{
    XTime t0;
    XTime_GetTime(&t0);
    while (!(Xil_In32(base + AP_CTRL) & AP_DONE))
        if (us_since(t0) > IP_TIMEOUT_US) return -1;
    return 0;
}

/* start the module in the partition on a DDR buffer; returns 0 or ST_TIMEOUT */
static int rp_run(const void *buf, uint32_t *t_ip)
{
    XTime t0;
    XTime_GetTime(&t0);
    Xil_Out32(RP_BASE + REG_INPUT_L, (uint32_t)(UINTPTR)buf);
    Xil_Out32(RP_BASE + REG_INPUT_H, 0);
    Xil_Out32(RP_BASE + AP_CTRL, AP_START);
    if (wait_done(RP_BASE)) return ST_TIMEOUT;
    *t_ip = us_since(t0);
    return ST_OK;
}

static int loaded_net(void) { return (int)(Xil_In32(RP_BASE + REG_NET_ID) & 0xff); }

/* run the given network on one image, fill one 26-byte result block */
static int run_net(int net, const uint8_t *img, uint8_t *blk)
{
    XTime t0;
    XTime_GetTime(&t0);
    const void *buf;
    if (net == NET_CNN) {
        cnn_preprocess(img, cnn_in);
        Xil_DCacheFlushRange((UINTPTR)cnn_in, sizeof(cnn_in));
        buf = cnn_in;
    } else {
        snn_preprocess(img, snn_feat);
        Xil_DCacheFlushRange((UINTPTR)snn_feat, sizeof(snn_feat));
        buf = snn_feat;
    }
    uint32_t t_pre = us_since(t0), t_ip = 0;

    int st = rp_run(buf, &t_ip);
    if (st != ST_OK) return st;
    if (loaded_net() != net) return ST_NO_NET;      /* the other module is loaded */

    blk[0] = (uint8_t)net;
    blk[1] = (uint8_t)Xil_In32(RP_BASE + REG_PRED);
    put_u32(blk + 2, t_pre);
    put_u32(blk + 6, t_ip);
    for (int i = 0; i < 4; i++)
        put_u32(blk + 10 + 4 * i, Xil_In32(RP_BASE + REG_LOGITS + 4 * i));
    return ST_OK;
}

static int run_cnn(const uint8_t *img, uint8_t *blk) { return run_net(NET_CNN, img, blk); }
static int run_snn(const uint8_t *img, uint8_t *blk) { return run_net(NET_SNN, img, blk); }

/* which module is in the partition: run it once on a zero buffer and read NET_ID */
static int probe_net(void)
{
    uint32_t t;
    memset(cnn_in, 0, sizeof(cnn_in));
    Xil_DCacheFlushRange((UINTPTR)cnn_in, sizeof(cnn_in));
    if (rp_run(cnn_in, &t) != ST_OK) return 0;
    return loaded_net();
}

/* ---------------- main loop ---------------- */
int main(void)
{
    Xil_DCacheEnable();
    timer_start();
    uart_init();
    xil_printf("\r\nA2 shape classifier fw %x (DFX)  loaded net=%d  baud=%d\r\n",
               FW_VERSION, probe_net(), UART_BAUD);

    for (;;) {
        /* sync */
        if (rx_byte() != SYNC_REQ_0) continue;
        if (rx_byte() != SYNC_REQ_1) continue;

        uint8_t hdr[3];
        for (int i = 0; i < 3; i++) hdr[i] = rx_byte();
        uint8_t cmd = hdr[0];
        uint16_t len = hdr[1] | (hdr[2] << 8);
        if (len > MAX_PAYLOAD) {
            send_response(cmd, ST_BAD_LEN, 0, 0);
            continue;
        }

        XTime t_rx0;
        XTime_GetTime(&t_rx0);
        for (int i = 0; i < len; i++) rx_payload[i] = rx_byte();
        uint16_t crc_rx = rx_byte();
        crc_rx |= (uint16_t)rx_byte() << 8;
        uint32_t t_rx = us_since(t_rx0);

        uint16_t crc = crc16_ccitt(0xFFFF, hdr, 3);
        crc = crc16_ccitt(crc, rx_payload, len);
        if (crc != crc_rx) {
            send_response(cmd, ST_BAD_CRC, 0, 0);
            continue;
        }

        switch (cmd) {
        case CMD_ECHO:
            send_response(cmd, ST_OK, rx_payload, len);
            break;

        case CMD_INFO: {
            int id = probe_net();                   /* reflects the current partial bitstream */
            tx_payload[0] = (id == NET_CNN);
            tx_payload[1] = (id == NET_SNN);
            put_u32(tx_payload + 2, UART_BAUD);
            put_u32(tx_payload + 6, (uint32_t)TIMER_HZ);
            put_u16(tx_payload + 10, FW_VERSION);
            send_response(cmd, ST_OK, tx_payload, 12);
            break;
        }

        case CMD_CNN:
        case CMD_SNN:
        case CMD_BOTH: {
            if (len != IMG_BYTES) { send_response(cmd, ST_BAD_LEN, 0, 0); break; }
            put_u32(tx_payload, t_rx);
            uint16_t n = 4;
            int st = ST_OK;
            if (cmd != CMD_SNN && st == ST_OK) { st = run_cnn(rx_payload, tx_payload + n); n += RESULT_BYTES; }
            if (cmd != CMD_CNN && st == ST_OK) { st = run_snn(rx_payload, tx_payload + n); n += RESULT_BYTES; }
            send_response(cmd, (uint8_t)st, tx_payload, st == ST_OK ? n : 0);
            break;
        }

        default:
            send_response(cmd, ST_BAD_CMD, 0, 0);
        }
    }
    return 0;
}
