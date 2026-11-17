# CNN training (ShapeCNN)

```
python cnn\train_cnn.py        # ~5 min on a CPU
```

Trains the network in Keras on `dataset/train` (validation on `dataset/val`, early stopping),
folds batch-norm into the convolutions and exports:

* `hls/cnn/cnn_weights.h` – fixed-point weights for the HLS kernel
* `hls/cnn/cnn_test_vectors.txt` – test images in Q8.8 plus the Keras predictions for the testbench
* `out/models/shape_cnn.keras`, `out/plots/` (accuracy, loss, confusion matrices),
  `out/reports/` (classification report, metrics)

Architecture: input 60×80×1 → 4 × (Conv3×3 + BN + ReLU) with 8/16/16/32 channels, 2×2 max-pool
after the first three → global average pooling → dense 32 → 4. 8.6 k parameters, about 38 px
receptive field. Test accuracy 100 %. Training log: `../cnn_train.log`.
