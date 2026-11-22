> **Status:** implemented and tested on the PYNQ-Z2, see [dfx/README.md](../dfx/README.md).

# DFX bonus: swapping CNN and SNN in one region

Status: **not started.** The current design has both accelerators in the fabric at the same
time, which already meets the base assignment. This file is the plan for the bonus task: one
reconfigurable partition (RP) that holds either the CNN or the SNN, swapped at run time.

Work on a branch (for example `dfx`) and keep `vivado/out/` of `main` untouched, so the
measured results in the report stay reproducible.

## 1. Give both networks the same interface

A reconfigurable module (RM) must have exactly the same ports as every other RM for that
partition. Today the two IPs differ in where they read their input:

| | control | input |
|---|---|---|
| CNN | AXI-Lite `s_axi_control` | `m_axi gmem` → `S_AXI_HP0` → DDR |
| SNN | AXI-Lite `s_axi_CTRL` | `m_axi gmem` → CPU interconnect → BRAM |

Simplest option: let the SNN read from DDR as well.

* Firmware: write the 512 features to a DDR buffer (like `cnn_in`), flush the D-cache and pass
  that address instead of `0x4002_0000`. The BRAM controller can then be removed.
* Make both HLS tops expose identical ports: same AXI-Lite bundle name and register map width,
  same `m_axi` bundle name, data width and address width, both with `ap_ctrl_hs`.
  Compare `hls/*/…_prj/sol1/impl/ip/component.xml` before going further.
* Keep the register offsets used by the firmware (`AP_CTRL`, input address `0x10/0x14`,
  prediction `0x1c`, scores `0x30`) or put both maps behind one firmware table.

## 2. Vivado DFX project

* Turn on *Dynamic Function eXchange* for the project
  (`set_property PR_FLOW 1 [current_project]`).
* Put the accelerator into a block design container (or an RTL wrapper) that is the RP. Add
  the CNN and the SNN as two reconfigurable modules of it.
* Static part: PS7, AXI interconnects, reset, and **DFX Decoupler** on the AXI-Lite and AXI
  master interfaces so the static logic sees no garbage while the RP is being reconfigured.
  Optionally add the *DFX AXI Shutdown Manager*.
* Draw one Pblock for the RP, sized for the CNN (the bigger module): about 10 k LUT, 9 k FF,
  68 BRAM36 equivalents, 53 DSP, plus margin. Check that it does not cross clock-region rules
  for 7-series (`SNAPPING_MODE ON`).
* Implement one configuration per RM (CNN, SNN). Run `pr_verify` on the two routed
  checkpoints, then write the full bitstream and the two partial bitstreams
  (`write_bitstream -cell …`).
* Check first that the installed licence includes DFX for the xc7z020 (it may not with the
  free licence).

## 3. Loading a partial bitstream

Easiest, for the demo: over JTAG from xsdb, while the firmware keeps running:

```
connect
targets -set -nocase -filter {name =~ "*xc7z020*"}
fpga -partial cnn_partial.bit       ;# or snn_partial.bit
```

More complete: let the CPU load it through PCAP (`XDcfg` driver in the standalone BSP). The
partial bitstream (≈ 0.5–1 MB) has to reach DDR first, for example with `dow -data` from xsdb,
or over UART (about 2–3 s at 4 Mbaud) with a new protocol command.

Before and after loading: assert the decoupler, load, release the decoupler, reset the RP.

## 4. Firmware and PC app

* Add a way to read which RM is loaded (for example a constant ID register in each HLS top),
  and answer `ST_NO_NET` (already in `protocol.h`) when the requested network is not loaded.
* `info` should report the loaded network; the PC app can then swap and run `eval --net cnn`
  and `eval --net snn` back to back.
* Measure reconfiguration time; it belongs in the report next to the inference times.

## 5. What to report

Static vs. RP resource use, partial bitstream size, reconfiguration time, and whether
accuracy and IP times stay the same as in the combined build (CNN 2 840 µs, SNN 197 µs).
