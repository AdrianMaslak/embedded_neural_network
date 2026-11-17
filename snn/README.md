# SNN training (512–64–32–4 LIF)

```
python snn\train_snn.py        # ~1 min
```

Encodes every image with the spike features from `common.py`, trains a leaky
integrate-and-fire network in snnTorch (β = 0.9, threshold 1, subtractive reset, fast-sigmoid
surrogate gradient, T = 1, readout = output membrane potential), quantises the weights to int16
(×128) and exports:

* `hls/snn/fpga_weights.h` – weights and constants for the HLS kernel
* `hls/snn/snn_test_vectors.txt` – features and expected integer results for the testbench
* `out/models/snn_mlp.pt`, `out/plots/`, `out/reports/`

It also runs an integer model in Python that reproduces the HLS kernel exactly; the HLS
C-simulation is checked against it.

Test accuracy: 98.58 % (float), 97.92 % (integer, same as on the board). Training log:
`../snn_train.log`.

## Input encoding

1. Integer Sobel, |G| = |Gx| + |Gy|.
2. A pixel spikes if |G| > max|G| / 3, into channel 0 if |Gx| ≥ |Gy|, otherwise channel 1.
3. Spikes are counted on a 16×16 grid laid over the bounding square of all spikes, which
   removes position and size.
4. Counts are clipped at 4 and scaled to 0…128 → 16·16·2 = 512 features.

The same steps run in C on the board (`firmware/preprocess.c`); `tools/check_preprocess.c`
confirms the two give identical features on all test images.
