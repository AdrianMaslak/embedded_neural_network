# CNN vs. SNN shape recognition on the PYNQ-Z2

Assignment 2 – Accelerated AI for Vision Applications (SDU, Embedded Systems).

Video: https://youtu.be/2bmPeDtzU-s · Report: [`report/report.pdf`](report/report.pdf)

A PC application sends 60×80 grayscale images over UART to a PYNQ-Z2. The ARM CPU on the board
preprocesses each image and runs it through a **CNN** or an **SNN** accelerator (both written in
Vitis HLS and placed in the same bitstream), then sends the class and timing back to the PC.

```
PC ──UART──► ARM Cortex-A9 (bare metal) ──AXI──► CNN IP  (reads image from DDR)
   ◄──UART──        preprocessing, control ──AXI──► SNN IP  (reads spike features from BRAM)
```

Classes: **Circle, Square, Triangle, Figure8**, one shape per image.

## Results (board, 1200 test images, 100 MHz)

| | CNN | SNN |
|---|---|---|
| Accuracy on the board | **100.0 %** | **97.92 %** |
| Accuracy of the PC reference | 100.0 % (Keras, float) | 97.92 % (integer model) |
| Accelerator time | 2 840 µs | 197 µs |
| CPU preprocessing | 289 µs | 1 827 µs |
| LUT / FF | 9 843 / 8 503 | 3 638 / 3 348 |
| BRAM36 eq. / DSP | 68 / 53 | 11 / 8 |
| Power (Vivado, post-route) | 0.266 W | 0.056 W |
| Energy per inference | 755 µJ | 11 µJ |

UART at 4 Mbaud: 12.0 ms to receive one image on the board, 30.7 ms PC round trip for both
networks (≈31 images/s). Timing is met at 100 MHz (WNS +0.257 ns). Raw data:
[`pc/results/both_20261116_193730/`](pc/results/both_20261116_193730/).

## Repository map

The repository holds both parts the assignment asks for. The build scripts refer to each other
with relative paths, so keep the folders where they are.

### Hardware / platform

| Path | Contents |
|---|---|
| [`hls/cnn/`](hls/cnn/) | CNN accelerator `shape_cnn` (Vitis HLS C++), testbench, weights, `run_hls.tcl` |
| [`hls/snn/`](hls/snn/) | SNN accelerator `snn_mlp` (Vitis HLS C++), testbench, weights, `run_hls.tcl` |
| [`vivado/`](vivado/) | `build.tcl` (block design → bitstream → `.xsa`), prebuilt `out/shape_system.bit` and `.xsa`, reports |

### Application

| Path | Contents |
|---|---|
| [`firmware/`](firmware/) | Bare-metal C for the ARM: UART protocol, preprocessing, IP control, timing; Vitis build script |
| [`pc/`](pc/) | PC application `app.py`: `info`, `echo`, `eval`, `demo`; software board model (`--sim`) |
| [`gen_shapes.py`](gen_shapes.py), [`dataset/`](dataset/) | Synthetic dataset generator; the test split used for all results |
| [`common.py`](common.py) | Class list and the reference preprocessing for both networks |
| [`cnn/`](cnn/), [`snn/`](snn/) | Training and export scripts, trained models, plots |
| [`tools/`](tools/) | Check that the firmware preprocessing equals the Python reference |
| [`report/`](report/) | Report (LaTeX + PDF) and diagrams |

## Requirements

* AMD Vivado + Vitis **2026.1** (Vitis HLS is part of Vitis), PYNQ-Z2 board files installed from
  the Vivado board store.
* Python 3.13 with `numpy opencv-python tensorflow keras torch snntorch scikit-learn matplotlib pyserial`.
* PYNQ-Z2 with the boot jumper on **JTAG** and the micro-USB cable connected.

The AMD tools fail on paths that contain spaces. If the repository sits in such a path, map it
to a drive letter first (this lasts until the next reboot):

```
subst W: "C:\path with spaces\assignment2"
W:
```

## Run on the board (prebuilt files)

The bitstream and `.xsa` are in the repository, so only the firmware has to be built.

```
C:\AMDDesignTools\2026.1\Vitis\bin\vitis.bat -s firmware\build_sw.py
C:\AMDDesignTools\2026.1\Vitis\bin\xsdb.bat firmware\run_on_board.tcl

python pc\app.py --port COM4 info          # board alive, both networks present
python pc\app.py --port COM4 echo -n 50    # UART link test without the accelerators
python pc\app.py --port COM4 demo          # image grid: true label, CNN and SNN prediction
python pc\app.py --port COM4 eval          # full test set: accuracy, latency, UART timing
```

Replace `COM4` with the board's port (Device Manager → Ports). Every `pc\app.py` command also
runs without hardware with `--sim`.

## Rebuild everything

| Step | Command | Time |
|---|---|---|
| Dataset | `python gen_shapes.py` | 15 s |
| Train CNN, export weights | `python cnn\train_cnn.py` | ~5 min |
| Train SNN, export weights | `python snn\train_snn.py` | ~1 min |
| SNN HLS (C-sim, synthesis, IP) | `cd hls\snn` then `vitis-run --mode hls --tcl run_hls.tcl` | ~10 min |
| CNN HLS (synthesis, IP) | `cd hls\cnn`, `set HLS_NO_CSIM=1`, then `vitis-run --mode hls --tcl run_hls.tcl` | ~15 min |
| Hardware | `cd vivado` then `vivado -mode batch -source build.tcl` | ~15 min |
| Firmware | `vitis -s firmware\build_sw.py` | ~3 min |

The full CNN C-simulation of 1200 images takes about an hour and needs a lot of RAM; run it on
its own with `HLS_CSIM_ONLY=1`. Close other large programs while Vivado implements the design.

## Design summary

* **Dataset** – generated with fixed seeds; size, position, rotation, line thickness, fill,
  lighting, blur and noise vary. 1500/300/300 images per class for train/val/test, from
  independent random streams, identical for both networks.
* **CNN** – ShapeCNN, 4 × (Conv3×3 + BN + ReLU), max-pooling after the first three, global
  average pooling, dense 32 → 4; 8.6 k parameters. Q8.8 activations, 18-bit weights, each layer
  one loop pipelined at II = 1.
* **SNN** – 512–64–32–4 leaky integrate-and-fire network, T = 1 time step, int16 weights,
  membrane readout. Input encoding: Sobel edges, spike when |G| > max|G|/3, two orientation
  channels, 16×16 grid over the shape's bounding square, counts clipped at 4.
* **Memory** – CNN: the CPU writes the Q8.8 image to DDR and flushes the cache; the IP reads it
  through `S_AXI_HP0`. SNN: the CPU writes the features into an 8 KB AXI BRAM at `0x4002_0000`;
  the IP reads it through the CPU's AXI interconnect (slave port `S01`). Results come back in
  AXI-Lite registers.
* **UART** – PS UART0, 4 000 000 baud, 8N1; frames with sync bytes, command, length and
  CRC-16. See [`firmware/protocol.h`](firmware/protocol.h).

The lecture CNN reached about 32 % and the lecture spike encoding about 31 % on this dataset,
which is why both were replaced. The report explains why.

## Known issues and decisions

* **CNN C-simulation** ran on all 1200 test images in three chunks of 400 and matched Keras on
  1200/1200 (`hls/cnn/csim_results`); on the board 1200/1200 are correct too.
* **Vivado connection automation** leaves the SNN's `m_axi_gmem` on a dead-end interconnect
  because the BRAM controller has only one slave port. `build.tcl` therefore connects it to
  `S01_AXI` of the CPU interconnect by hand and stops with an error if the BRAM is not mapped
  for both the CPU and the SNN.
* **Timer** – the Vitis 2026.1 `xiltimer` library does not start the Cortex-A9 global timer
  when the program is loaded over JTAG, so the firmware starts and reads it directly.
* **DFX** (bonus) is done: CNN and SNN swap in one region at run time; see [`dfx/README.md`](dfx/README.md).
