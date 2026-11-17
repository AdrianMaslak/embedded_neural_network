# Vivado hardware

`build.tcl` creates the whole design in batch mode: block design, synthesis, implementation,
bitstream, reports and the `.xsa` for Vitis.

```
cd vivado
vivado -mode batch -source build.tcl        # ~15 min, from a path without spaces
```

It needs the two HLS IPs built first (`hls/*/…_prj/sol1/impl/ip`) and the PYNQ-Z2 board files.
These are looked up in `%APPDATA%\Xilinx\Vivado\2026.1\xhub\board_store\…`; set
`PYNQZ2_BOARD_DIR` to override.

## Outputs (`out/`, kept in the repository)

| File | Contents |
|---|---|
| `shape_system.bit` | bitstream |
| `shape_system.xsa` | hardware platform for Vitis (includes the bitstream) |
| `system_bd.tcl` | exported block design (can recreate the diagram) |
| `system_bd.svg`, `system_bd.pdf` | block design picture (`export_bd.tcl`, needs GUI mode) |
| `utilization.rpt`, `utilization_hier.rpt` | resources, total and per IP |
| `timing.rpt` | timing summary (all constraints met, WNS +0.257 ns) |
| `power.rpt` | power estimate (1.757 W on chip; CNN 0.266 W, SNN 0.056 W) |

## Block design

```
PS7  M_AXI_GP0 ─► AXI Interconnect (ps_axi_periph) ─┬─► shape_cnn_0  s_axi_control
                    S00: CPU                         ├─► snn_mlp_0    s_axi_CTRL
                    S01: snn_mlp_0 m_axi_gmem        └─► axi_bram_ctrl_0 ─► 8 KB BRAM
shape_cnn_0 m_axi_gmem ─► AXI Interconnect ─► PS7 S_AXI_HP0 ─► DDR
```

All of the PL runs on `FCLK_CLK0` = 100 MHz.

| Segment | Address | Size | Seen by |
|---|---|---|---|
| `shape_cnn_0` registers | `0x4000_0000` | 64 KB | CPU |
| `snn_mlp_0` registers | `0x4001_0000` | 64 KB | CPU |
| feature BRAM | `0x4002_0000` | 8 KB | CPU and SNN (same address) |
| DDR via `S_AXI_HP0` | `0x0000_0000` | 512 MB | CNN |

The SNN master is connected by hand: Vivado's connection automation puts it on its own
interconnect with no output because the BRAM controller has only one slave port, and the SNN
then waits for its reads forever. The script checks the BRAM mapping and stops if it is missing.

## Open in the GUI

```
vivado build\a2.xpr
```

opens the project created by `build.tcl` with the block design, address editor and
implemented design.

## Resources (post-implementation, xc7z020)

| | LUT | FF | BRAM36 eq. | DSP |
|---|---|---|---|---|
| whole design | 15 584 (29 %) | 14 264 (13 %) | 81 (58 %) | 61 (28 %) |
| `shape_cnn_0` | 9 843 | 8 503 | 68 | 53 |
| `snn_mlp_0` | 3 638 | 3 348 | 11 | 8 |
