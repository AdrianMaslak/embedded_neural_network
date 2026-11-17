#!/usr/bin/env python3
"""
Train the spiking MLP (lecture 8 architecture) on the shape dataset.

Input encoding (common.py, mirrored in firmware/preprocess.c): integer Sobel
|Gx|+|Gy|, a pixel spikes when |G| > max|G|/3 (single time step, T = 1),
split into 2 orientation channels; spikes are binned on a 16x16 grid laid
over the shape's bounding square -> 512 features = min(count,4)*32.

(The lecture-8 encoding - Gx+Gy ON/OFF at the 90th percentile on a fixed
12x16 grid - gave 54 % float / 31 % int16 on this data set.)

Network: 512 -> 64 LIF -> 32 LIF -> 4 LIF, readout = output membrane
potential summed over time steps, prediction = argmax.

Outputs (snn/out/):
    models/snn_mlp.pt
    plots/  accuracy curve, confusion matrices (float + int reference)
    reports/metrics.json, classification_report_test.txt
    ../hls/snn/fpga_weights.h        int16 weights (x128) for HLS
    ../hls/snn/snn_test_vectors.txt  int16 features + label + int-ref pred/logits

The integer reference (`int_forward`) reproduces snn_mlp.cpp exactly, so the
HLS C-simulation must match it bit for bit.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import snntorch as snn
import torch
import torch.nn as nn
from snntorch import surrogate
from sklearn.metrics import ConfusionMatrixDisplay, classification_report, confusion_matrix
from torch.utils.data import DataLoader, TensorDataset

from common import (CLASSES, GRID, IMG_H, IMG_W, N_FEATURES, ROOT,
                    list_split, load_gray, snn_features_float, snn_features_int16)

SEED = 42
T_STEPS = 1
H1, H2 = 64, 32
EPOCHS = 60
BATCH = 32
LR = 1e-3
BETA = 0.9
SPIKE_PENALTY = 1e-5
WEIGHT_SCALE = 128
BETA_NUM = 115          # HLS leak: mem*115 >> 7 ~= 0.898

OUT = ROOT / "snn" / "out"
HLS_DIR = ROOT / "hls" / "snn"

torch.manual_seed(SEED)
np.random.seed(SEED)


class SNNMLP(nn.Module):
    def __init__(self, n_in=N_FEATURES, n_out=len(CLASSES)):
        super().__init__()
        g = surrogate.fast_sigmoid()
        self.fc1, self.lif1 = nn.Linear(n_in, H1), snn.Leaky(beta=BETA, spike_grad=g)
        self.fc2, self.lif2 = nn.Linear(H1, H2), snn.Leaky(beta=BETA, spike_grad=g)
        self.fc3, self.lif3 = nn.Linear(H2, n_out), snn.Leaky(beta=BETA, spike_grad=g)

    def forward(self, x):                      # x: (B, T, N)
        m1, m2, m3 = self.lif1.init_leaky(), self.lif2.init_leaky(), self.lif3.init_leaky()
        out, spikes = 0, 0
        for t in range(x.size(1)):
            s1, m1 = self.lif1(self.fc1(x[:, t]), m1)
            s2, m2 = self.lif2(self.fc2(s1), m2)
            _, m3 = self.lif3(self.fc3(s2), m3)
            out = out + m3
            spikes = spikes + s1.sum(1) + s2.sum(1)
        return out / x.size(1), spikes


# ---------------- integer model == snn_mlp.cpp ----------------
def quant(model):
    q = lambda t: np.round(t.detach().cpu().numpy() * WEIGHT_SCALE).astype(np.int64)
    return {n: q(getattr(model, n.split("_")[0]).weight if n.endswith("_w")
                 else getattr(model, n.split("_")[0]).bias)
            for n in ("fc1_w", "fc1_b", "fc2_w", "fc2_b", "fc3_w", "fc3_b")}


def lif(acc, mem):
    prev = mem > WEIGHT_SCALE
    mem = ((mem * BETA_NUM) >> 7) + acc
    mem = mem - prev * WEIGHT_SCALE
    return (mem > WEIGHT_SCALE).astype(np.int64), mem


def int_forward(qw, feats):
    """feats: (B, T, N) int16 features. Returns (pred, logits int)."""
    B = feats.shape[0]
    m1 = np.zeros((B, H1), np.int64); m2 = np.zeros((B, H2), np.int64)
    m3 = np.zeros((B, len(CLASSES)), np.int64); logits = np.zeros_like(m3)
    for t in range(feats.shape[1]):
        x = feats[:, t].astype(np.int64)
        # per-term (x*w)>>7 exactly as HLS (zero inputs skipped: contribute 0 anyway)
        a1 = qw["fc1_b"][None] + ((x[:, None, :] * qw["fc1_w"][None]) >> 7).sum(-1)
        s1, m1 = lif(a1, m1)
        s2, m2 = lif(qw["fc2_b"][None] + s1 @ qw["fc2_w"].T, m2)
        _, m3 = lif(qw["fc3_b"][None] + s2 @ qw["fc3_w"].T, m3)
        logits += m3
    return logits.argmax(1), logits      # argmax keeps first max == HLS strict '>'


def export_header(qw, path):
    lines = ["#ifndef FPGA_WEIGHTS_H", "#define FPGA_WEIGHTS_H", "", "#include <stdint.h>", "",
             f"#define WEIGHT_SCALE {WEIGHT_SCALE}", f"#define T_STEPS {T_STEPS}",
             f"#define IMG_H {IMG_H}", f"#define IMG_W {IMG_W}",
             f"#define GRID {GRID}",
             f"#define N_FEATURES {N_FEATURES}", f"#define HIDDEN1 {H1}", f"#define HIDDEN2 {H2}",
             f"#define NUM_CLASSES {len(CLASSES)}", ""]
    for n, a in qw.items():
        assert np.abs(a).max() < 32768, f"{n} overflows int16"
        if a.ndim == 2:
            body = ",\n".join("    {" + ", ".join(map(str, r)) + "}" for r in a)
            lines.append(f"static const int16_t {n}[{a.shape[0]}][{a.shape[1]}] = {{\n{body}\n}};\n")
        else:
            lines.append(f"static const int16_t {n}[{a.size}] = {{{', '.join(map(str, a))}}};\n")
    lines.append("#endif")
    path.write_text("\n".join(lines))


def plot_cm(y, p, path, title):
    fig, ax = plt.subplots(figsize=(5, 4.5))
    ConfusionMatrixDisplay(confusion_matrix(y, p, labels=range(len(CLASSES))),
                           display_labels=CLASSES).plot(ax=ax, cmap="Blues", colorbar=False)
    ax.set_title(title)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def load(split):
    items = list_split(split)
    imgs = [load_gray(p) for p, _ in items]
    xf = np.stack([snn_features_float(i) for i in imgs])[:, None, :]       # (B,1,N)
    xi = np.stack([snn_features_int16(i) for i in imgs])[:, None, :]
    y = np.array([l for _, l in items])
    return xf, xi, y


def evaluate(model, x):
    model.eval()
    with torch.no_grad():
        return model(torch.from_numpy(x))[0].argmax(1).numpy()


def main():
    for d in ("models", "plots", "reports"):
        (OUT / d).mkdir(parents=True, exist_ok=True)
    HLS_DIR.mkdir(parents=True, exist_ok=True)

    print("extracting features ...")
    xf_tr, _, y_tr = load("train")
    xf_va, xi_va, y_va = load("val")
    xf_te, xi_te, y_te = load("test")
    print(f"train {len(y_tr)}  val {len(y_va)}  test {len(y_te)}  "
          f"input density {100 * (xf_tr > 0).mean():.1f}% nonzero")

    model = SNNMLP()
    opt = torch.optim.Adam(model.parameters(), lr=LR)
    sched = torch.optim.lr_scheduler.ReduceLROnPlateau(opt, factor=0.5, patience=4)
    loader = DataLoader(TensorDataset(torch.from_numpy(xf_tr), torch.from_numpy(y_tr)),
                        batch_size=BATCH, shuffle=True)
    ce = nn.CrossEntropyLoss()

    best, best_state, wait, hist = -1, None, 0, {"train_acc": [], "val_acc": []}
    for ep in range(EPOCHS):
        model.train()
        correct = 0
        for xb, yb in loader:
            out, spk = model(xb)
            loss = ce(out, yb) + SPIKE_PENALTY * spk.mean()
            opt.zero_grad(); loss.backward(); opt.step()
            correct += (out.argmax(1) == yb).sum().item()
        tr_acc = correct / len(y_tr)
        va_acc = float((evaluate(model, xf_va) == y_va).mean())
        sched.step(-va_acc)
        hist["train_acc"].append(tr_acc); hist["val_acc"].append(va_acc)
        print(f"epoch {ep + 1:02d} train {tr_acc:.4f} val {va_acc:.4f}")
        if va_acc > best:
            best, wait = va_acc, 0
            best_state = {k: v.clone() for k, v in model.state_dict().items()}
        else:
            wait += 1
            if wait >= 10:
                break
    model.load_state_dict(best_state)
    torch.save(model.state_dict(), OUT / "models" / "snn_mlp.pt")

    fig = plt.figure(figsize=(5, 3.5))
    plt.plot(hist["train_acc"], label="train"); plt.plot(hist["val_acc"], label="val")
    plt.xlabel("epoch"); plt.ylabel("accuracy"); plt.grid(True); plt.legend(); plt.tight_layout()
    fig.savefig(OUT / "plots" / "snn_accuracy.png", dpi=150)
    plt.close(fig)

    # float model
    p_te = evaluate(model, xf_te)
    # integer (HLS-equivalent) model on quantised features
    qw = quant(model)
    pi_va, _ = int_forward(qw, xi_va)
    pi_te, logits_te = int_forward(qw, xi_te)

    plot_cm(y_te, p_te, OUT / "plots" / "snn_cm_test_float.png", "SNN (float) - test")
    plot_cm(y_te, pi_te, OUT / "plots" / "snn_cm_test_int.png", "SNN (int16 reference) - test")
    rep = classification_report(y_te, pi_te, target_names=CLASSES, digits=4)
    print(rep)
    (OUT / "reports" / "classification_report_test.txt").write_text(rep)

    cm = confusion_matrix(y_te, pi_te)
    metrics = {
        "T_steps": T_STEPS, "encoding": "Sobel ON/OFF, P90 threshold, 5x5 cell density",
        "val_acc_float": best, "test_acc_float": float((p_te == y_te).mean()),
        "val_acc_int": float((pi_va == y_va).mean()), "test_acc_int": float((pi_te == y_te).mean()),
        "float_int_agreement_test": float((p_te == pi_te).mean()),
        "test_per_class_acc_int": {c: float(cm[i, i] / cm[i].sum()) for i, c in enumerate(CLASSES)},
        "epochs_run": len(hist["val_acc"]),
        "params": int(sum(p.numel() for p in model.parameters())),
        "input_nonzero_pct": float(100 * (xi_te > 0).mean()),
    }
    (OUT / "reports" / "metrics.json").write_text(json.dumps(metrics, indent=2))
    print(json.dumps(metrics, indent=2))

    export_header(qw, HLS_DIR / "fpga_weights.h")
    # HLS test vectors: N, then per image "label ref_pred l0 l1 l2 l3 f0..f383"
    with open(HLS_DIR / "snn_test_vectors.txt", "w") as f:
        f.write(f"{len(y_te)}\n")
        for y, p, lg, x in zip(y_te, pi_te, logits_te, xi_te[:, 0]):
            f.write(f"{y} {p} " + " ".join(map(str, lg)) + " " + " ".join(map(str, x)) + "\n")
    print("done ->", OUT)


if __name__ == "__main__":
    main()
