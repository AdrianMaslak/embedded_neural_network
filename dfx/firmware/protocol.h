/*
 * PC <-> PYNQ-Z2 UART protocol (shared by firmware/main.c and pc/protocol.py)
 *
 * UART: PS UART0 (USB-UART), 8N1, no flow control, default 4 000 000 baud.
 * All multi-byte fields little-endian. CRC = CRC-16/CCITT-FALSE
 * (poly 0x1021, init 0xFFFF) - Python: binascii.crc_hqx(data, 0xFFFF).
 *
 * Request  (PC -> board):
 *   0xAA 0x55 | cmd u8 | len u16 | payload[len] | crc u16 (over cmd..payload)
 * Response (board -> PC):
 *   0x55 0xAA | cmd u8 | status u8 | len u16 | payload[len] | crc u16 (over cmd..payload)
 *
 * Commands
 *   CMD_CNN  0x01  payload = 4800 B image (60x80, 8-bit gray, row-major)
 *   CMD_SNN  0x02  same image, run SNN
 *   CMD_BOTH 0x03  same image, run CNN then SNN
 *   CMD_ECHO 0x10  payload echoed back unchanged (UART link test)
 *   CMD_INFO 0x11  no payload; returns info block
 *
 * Inference response payload:
 *   t_rx_us u32                (board-side receive time of the request payload)
 *   then per network run, a 26-byte result block:
 *     net u8 (1 = CNN, 2 = SNN) | pred u8 | t_pre_us u32 | t_ip_us u32 | logits i32[4]
 *   CNN logits are Q8.8 (divide by 256), SNN logits are summed LIF membrane
 *   potentials (scale 128).
 *
 * INFO payload: have_cnn u8 | have_snn u8 | baud u32 | timer_hz u32 | version u16
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#define SYNC_REQ_0   0xAA
#define SYNC_REQ_1   0x55
#define SYNC_RSP_0   0x55
#define SYNC_RSP_1   0xAA

#define CMD_CNN      0x01
#define CMD_SNN      0x02
#define CMD_BOTH     0x03
#define CMD_ECHO     0x10
#define CMD_INFO     0x11

#define ST_OK        0
#define ST_BAD_CRC   1
#define ST_BAD_LEN   2
#define ST_BAD_CMD   3
#define ST_NO_NET    4
#define ST_TIMEOUT   5

#define NET_CNN      1
#define NET_SNN      2

#define IMG_H        60
#define IMG_W        80
#define IMG_BYTES    (IMG_H * IMG_W)
#define MAX_PAYLOAD  IMG_BYTES
#define RESULT_BYTES 26

#define FW_VERSION   0x0100

#endif
