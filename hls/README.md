# HLS accelerators

Two Vitis HLS components, each exported as a Vivado IP (`sdu.dk:hls:<name>:1.0`). Both use an
AXI-Lite control port and one AXI master port for their input.

| | `cnn/` – `shape_cnn` | `snn/` – `snn_mlp` |
|---|---|---|
| Top function | `shape_cnn` in `shape_cnn.cpp` | `snn_mlp` in `snn_mlp.cpp` |
| Input | 60×80 Q8.8 image (`int16`) over `m_axi gmem` | 512 spike features (`int16`) over `m_axi gmem` |
| Output (AXI-Lite) | class + 4 logits (Q8.8) | class + 4 membrane scores (×128) |
| Weights | `cnn_weights.h` (from `cnn/train_cnn.py`) | `fpga_weights.h` (from `snn/train_snn.py`) |
| Testbench | `shape_cnn_tb.cpp` vs. Keras predictions | `snn_mlp_tb.cpp` vs. the Python integer model |
| Test vectors | `cnn_test_vectors.txt` | `snn_test_vectors.txt` |
| Latency @ 100 MHz | 283 902 cycles (2.84 ms) | 19 812 cycles (0.198 ms) |

## Build

Run from a path without spaces (see the main README).

```
cd hls\snn
vitis-run --mode hls --tcl run_hls.tcl

cd hls\cnn
set HLS_NO_CSIM=1                 (PowerShell: $env:HLS_NO_CSIM="1")
vitis-run --mode hls --tcl run_hls.tcl
```

The IP lands in `<name>_prj/sol1/impl/ip/`, where `vivado/build.tcl` picks it up.

Environment switches in `run_hls.tcl`:

| Variable | Effect |
|---|---|
| `HLS_NO_CSIM=1` | skip C-simulation (CNN only) |
| `HLS_CSIM_ONLY=1` | only C-simulation, in a separate project `cnn_csim` (CNN only) |
| `HLS_COSIM=1` | also run C/RTL co-simulation |

## Verification status

* **SNN** – C-simulation of all 1200 test images matches the Python integer model bit for bit
  (prediction and all four scores): 1175/1200 = 97.92 %. See `snn/hls_run.log`.
* **CNN** – C-simulation of all 1200 test images, run in three chunks of 400, matches Keras on 1200/1200; see `cnn/csim_results/`.
  On the board all 1200 predictions are correct and equal to Keras.

## Reports

After a build the synthesis report (latency, II per loop, resources) is in
`<name>_prj/sol1/syn/report/<name>_csynth.rpt`.
