/*
 * Assignment 2 - bare-metal application for the PYNQ-Z2 (Cortex-A9, standalone)
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

#if defined(XPAR_SHAPE_CNN_0_BASEADDR)
#define CNN_BASE XPAR_SHAPE_CNN_0_BASEADDR
#elif defined(XPAR_SHAPE_CNN_0_S_AXI_CONTROL_BASEADDR)
#define CNN_BASE XPAR_SHAPE_CNN_0_S_AXI_CONTROL_BASEADDR
#elif defined(XPAR_XSHAPE_CNN_0_BASEADDR)
#define CNN_BASE XPAR_XSHAPE_CNN_0_BASEADDR
#endif

#if defined(XPAR_SNN_MLP_0_BASEADDR)
#define SNN_BASE XPAR_SNN_MLP_0_BASEADDR
#elif defined(XPAR_SNN_MLP_0_S_AXI_CTRL_BASEADDR)
#define SNN_BASE XPAR_SNN_MLP_0_S_AXI_CTRL_BASEADDR
#elif defined(XPAR_XSNN_MLP_0_BASEADDR)
#define SNN_BASE XPAR_XSNN_MLP_0_BASEADDR
#endif

#if defined(XPAR_AXI_BRAM_CTRL_0_BASEADDR)
#define FEAT_BRAM_BASE XPAR_AXI_BRAM_CTRL_0_BASEADDR
#elif defined(XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR)
#define FEAT_BRAM_BASE XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR
#endif

#if defined(CNN_BASE)
#define HAVE_CNN 1
#else
#define HAVE_CNN 0
#endif
#if defined(SNN_BASE) && defined(FEAT_BRAM_BASE)
#define HAVE_SNN 1
#else
#define HAVE_SNN 0
#endif

/* HLS register maps (copied from the generated x*_hw.h drivers) */
#define AP_CTRL            0x00
#define AP_START           0x01
#define AP_DONE            0x02
#define CNN_REG_INPUT_L    0x10
#define CNN_REG_INPUT_H    0x14
#define CNN_REG_PRED       0x1c
#define CNN_REG_LOGITS     0x30
#define SNN_REG_FEAT_L     0x10
#define SNN_REG_FEAT_H     0x14
#define SNN_REG_PRED       0x1c
#define SNN_REG_LOGITS     0x30

/* ---------------- buffers ---------------- */
static uint8_t rx_payload[MAX_PAYLOAD];
static uint8_t tx_payload[MAX_PAYLOAD];
static int16_t cnn_in[IMG_BYTES] __attribute__((aligned(64)));   /* DDR, read by CNN m_axi */
static int16_t snn_feat[SNN_N_FEATURES];

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

/* fills one 26-byte result block; returns status */
static int run_cnn(const uint8_t *img, uint8_t *blk)
{
#if HAVE_CNN
    XTime t0;
    XTime_GetTime(&t0);
    cnn_preprocess(img, cnn_in);
    Xil_DCacheFlushRange((UINTPTR)cnn_in, sizeof(cnn_in));
    uint32_t t_pre = us_since(t0);

    XTime_GetTime(&t0);
    Xil_Out32(CNN_BASE + CNN_REG_INPUT_L, (uint32_t)(UINTPTR)cnn_in);
    Xil_Out32(CNN_BASE + CNN_REG_INPUT_H, 0);
    Xil_Out32(CNN_BASE + AP_CTRL, AP_START);
    if (wait_done(CNN_BASE)) return ST_TIMEOUT;
    uint32_t t_ip = us_since(t0);

    blk[0] = NET_CNN;
    blk[1] = (uint8_t)Xil_In32(CNN_BASE + CNN_REG_PRED);
    put_u32(blk + 2, t_pre);
    put_u32(blk + 6, t_ip);
    for (int i = 0; i < 4; i++)
        put_u32(blk + 10 + 4 * i, Xil_In32(CNN_BASE + CNN_REG_LOGITS + 4 * i));
    return ST_OK;
#else
    (void)img; (void)blk;
    return ST_NO_NET;
#endif
}

static int run_snn(const uint8_t *img, uint8_t *blk)
{
#if HAVE_SNN
    XTime t0;
    XTime_GetTime(&t0);
    snn_preprocess(img, snn_feat);
    for (int i = 0; i < SNN_N_FEATURES; i += 2)      /* 2 features per 32-bit word */
        Xil_Out32(FEAT_BRAM_BASE + 2 * i,
                  (uint16_t)snn_feat[i] | ((uint32_t)(uint16_t)snn_feat[i + 1] << 16));
    uint32_t t_pre = us_since(t0);

    XTime_GetTime(&t0);
    Xil_Out32(SNN_BASE + SNN_REG_FEAT_L, FEAT_BRAM_BASE);
    Xil_Out32(SNN_BASE + SNN_REG_FEAT_H, 0);
    Xil_Out32(SNN_BASE + AP_CTRL, AP_START);
    if (wait_done(SNN_BASE)) return ST_TIMEOUT;
    uint32_t t_ip = us_since(t0);

    blk[0] = NET_SNN;
    blk[1] = (uint8_t)Xil_In32(SNN_BASE + SNN_REG_PRED);
    put_u32(blk + 2, t_pre);
    put_u32(blk + 6, t_ip);
    for (int i = 0; i < 4; i++)
        put_u32(blk + 10 + 4 * i, Xil_In32(SNN_BASE + SNN_REG_LOGITS + 4 * i));
    return ST_OK;
#else
    (void)img; (void)blk;
    return ST_NO_NET;
#endif
}

/* ---------------- main loop ---------------- */
int main(void)
{
    Xil_DCacheEnable();
    timer_start();
    uart_init();
    xil_printf("\r\nA2 shape classifier fw %x  CNN=%d SNN=%d  baud=%d\r\n",
               FW_VERSION, HAVE_CNN, HAVE_SNN, UART_BAUD);

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

        case CMD_INFO:
            tx_payload[0] = HAVE_CNN;
            tx_payload[1] = HAVE_SNN;
            put_u32(tx_payload + 2, UART_BAUD);
            put_u32(tx_payload + 6, (uint32_t)TIMER_HZ);
            put_u16(tx_payload + 10, FW_VERSION);
            send_response(cmd, ST_OK, tx_payload, 12);
            break;

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
