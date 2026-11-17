# Firmware (bare-metal, ARM Cortex-A9)

| File | Contents |
|---|---|
| `main.c` | UART framing and CRC, command loop, CNN/SNN control, timing |
| `preprocess.c/.h` | CNN Q8.8 conversion and SNN spike features, bit-identical to `common.py` |
| `protocol.h` | UART protocol, shared with `pc/protocol.py` |
| `build_sw.py` | creates the Vitis platform from `vivado/out/shape_system.xsa` and builds the app |
| `run_on_board.tcl` | programs the bitstream over JTAG, initialises the PS, loads and starts the ELF |

## Build and run

```
vitis -s firmware\build_sw.py               # -> firmware/ws/a2_app/build/a2_app.elf
xsdb firmware\run_on_board.tcl              # board in JTAG boot mode
```

`build_sw.py` deletes and recreates `firmware/ws`. If it fails with a locked `.lock` file, close
Vitis and delete `firmware/ws` by hand. The workspace can be opened in the Vitis GUI
(`vitis -w firmware\ws`) to build, run and debug from there.

## What one inference request does

1. Receive the frame byte by byte from PS UART0 (4 Mbaud) and check length and CRC-16.
2. **CNN**: convert the 4800 pixels to Q8.8 into a DDR buffer, flush the D-cache, write the
   buffer address to the IP, start it, poll `ap_done`, read class and 4 logits.
3. **SNN**: compute the 512 spike features, write them into the AXI BRAM at `0x4002_0000`, pass
   that address to the IP, start it, poll `ap_done`, read class and 4 scores.
4. Send back receive time, preprocessing time and IP time per network (µs) with the results.

An IP that does not finish within 1 s returns status `ST_TIMEOUT` instead of hanging.

## Timing

Times come from the Cortex-A9 global timer at 325 MHz (CPU clock / 2). The firmware starts the
timer itself: the Vitis 2026.1 `xiltimer` library reads it but never enables it when the
program is loaded over JTAG, which leaves `XTime_GetTime` constant.

## UART protocol

```
request  : AA 55 | cmd u8 | len u16 | payload | crc u16
response : 55 AA | cmd u8 | status u8 | len u16 | payload | crc u16
```

Little-endian, CRC-16/CCITT-FALSE over everything after the sync bytes. Commands:
`0x01` CNN, `0x02` SNN, `0x03` both (payload = 4800-byte image), `0x10` echo, `0x11` info.
Full details in `protocol.h`.

The baud rate is set by `UART_BAUD` (default 4 000 000). If a PC adapter cannot keep up, build
with `-DUART_BAUD=921600` and run the PC app with `--baud 921600`.
