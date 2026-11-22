# Bonus: Dynamic Function eXchange (DFX)

One reconfigurable partition (RP) holds either the CNN or the SNN. The modules are
swapped at run time with a partial bitstream while the firmware keeps running.

## Results (PYNQ-Z2, 1200 test images)

| | CNN in RP | SNN in RP |
|---|---|---|
| Accuracy on the board | 100 % (1200/1200) | 97.92 % |
| Accelerator time | 2840 us | 197 us |
| Module LUT / FF | 10 221 / 8 810 | 3 948 / 3 447 |
| Module BRAM36 / DSP | 68 / 53 | 14.5 / 8 |
| Timing at 100 MHz (WNS) | +0.477 ns | +0.332 ns |
| Power total / RP | 1.715 W / 0.293 W | 1.487 W / 0.078 W |

- RP = lower four clock regions (36 400 LUT, 72 800 FF, 100 BRAM tiles, 160 DSP);
  static part about 865 LUT.
- Partial bitstream 4 213 030 bytes, loaded over JTAG in about 2.55 s.
- `pr_verify`: the two configurations are compatible.
- Board log: `results/board_test.log` (CNN, swap to SNN, SNN, swap back, CNN).

## How it works

- `hls/cnn_rm.cpp`, `hls/snn_rm.cpp` wrap the unchanged accelerators from `hls/cnn`
  and `hls/snn` so both have the same ports and AXI-Lite map: input address 0x10,
  prediction 0x1C, logits 0x30, module id 0x40 (1 = CNN, 2 = SNN). Both read their
  input from DDR through S_AXI_HP0 (the SNN no longer uses the AXI BRAM).
- `vivado/build_dfx.tcl` builds the block design with the RP as a DFX block design
  container (variants `rm_cnn`, `rm_snn`), synthesizes, writes the pblock with
  `pblock_gen.tcl`, implements both configurations and writes all bitstreams and
  reports to `vivado/out`.
- `firmware/main.c` is the normal firmware with one RP base address (0x4000_0000).
  It reads the module id after each run and answers "not loaded" for the other network.
- After a partial reconfiguration the PL is reset (`reset_pl.tcl`, FPGA_RST_CTRL):
  the RP outputs toggle while it is rewritten, which otherwise locks the AXI
  interconnect. A DFX Decoupler would be the cleaner fix.

## Build (Windows, paths without spaces)

Needs a Vivado licence that includes DFX (Enterprise / university licence).

```
cd dfx\hls
vitis-run --mode hls --tcl run_hls_cnn.tcl
vitis-run --mode hls --tcl run_hls_snn.tcl
cd ..\vivado
set PYNQZ2_BOARD_DIR=<pynq-z2 board files, path without spaces>
vivado -mode batch -source build_dfx.tcl
cd ..\..
vitis -s dfx\firmware\build_sw.py
```

## Run on the board

```
xsdb dfx\firmware\run_on_board.tcl cnn
python pc\app.py --port COM4 info
python pc\app.py --port COM4 eval --net cnn
xsdb dfx\firmware\swap.tcl snn
xsdb dfx\firmware\reset_pl.tcl
python pc\app.py --port COM4 info
python pc\app.py --port COM4 eval --net snn
```
