# PC application

`app.py` talks to the board over the USB-UART (pyserial) and shows or evaluates the results.

```
python pc\app.py --port COM4 info                    # board alive? which networks?
python pc\app.py --port COM4 echo -n 50              # UART test without the accelerators
python pc\app.py --port COM4 demo                    # 8 test images: true label, CNN, SNN
python pc\app.py --port COM4 demo --image a.png b.png
python pc\app.py --port COM4 eval                    # full test set, both networks
python pc\app.py --port COM4 eval --net snn -n 100   # one network, random subset
python pc\app.py --sim eval                          # no hardware: software model of the board
```

Options: `--baud` (default 4 000 000, must match the firmware), `--timeout` (s), and for
`eval`/`demo` `--net cnn|snn|both`, `--split test|val|train`, `-n`.

| File | Contents |
|---|---|
| `app.py` | command line, evaluation, plots |
| `protocol.py` | framing, CRC, response parsing (mirror of `firmware/protocol.h`) |
| `sim_board.py` | software board: same protocol, networks computed in Python |

## Results

`eval` writes `pc/results/<net>_<date>_<time>/`:

* `summary.json` – accuracy per network and class, mean preprocessing and IP time, UART receive
  time, round trip, images per second
* `per_image.csv` – one row per image with prediction, scores and all timings
* `cm_CNN.png`, `cm_SNN.png` – confusion matrices

`both_20261116_193730/` is the reference run used in the report (1200 test images).

`demo` saves its figure to `pc/results/demo.png`. With `--image` the true label is unknown, so the
figure shows `true: ?` and no OK/X marks.
