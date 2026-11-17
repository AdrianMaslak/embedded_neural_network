"""
Shared definitions for Assignment 2: dataset paths, class list, and the
exact preprocessing used by training, HLS test vectors, and the PC app.

Everything that touches pixels on the board side must match these
functions bit-for-bit (see firmware/preprocess.c).
"""
from pathlib import Path

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parent
DATASET = ROOT / "dataset"

CLASSES = ["Circle", "Square", "Triangle", "Figure8"]
N_CLASSES = len(CLASSES)

IMG_H, IMG_W = 60, 80
N_PIXELS = IMG_H * IMG_W

# ---------------- SNN feature settings ----------------
GRID = 16                         # GRID x GRID cells over the shape's bounding square
N_FEATURES = GRID * GRID * 2      # 2 edge-orientation channels = 512
EDGE_DIV = 3                      # spike if |G| > max|G| / EDGE_DIV
COUNT_CLIP = 4                    # spikes per cell are clipped at 4 ...
FEATURE_SCALE = 128               # ... and mapped to 0..128 (128 == 1.0)


def list_split(split):
    """Return [(path, label)] for a dataset split, sorted for determinism."""
    items = []
    for label, cls in enumerate(CLASSES):
        for p in sorted((DATASET / split / cls).glob("*.png")):
            items.append((p, label))
    return items


def load_gray(path):
    img = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    if img is None:
        raise RuntimeError(f"cannot read {path}")
    if img.shape != (IMG_H, IMG_W):
        img = cv2.resize(img, (IMG_W, IMG_H), interpolation=cv2.INTER_LINEAR)
    return img


# ---------------- CNN input ----------------
def cnn_normalize(img_u8):
    """Float input used for Keras training: (x/255 - 0.5) / 0.25."""
    return (img_u8.astype(np.float32) / 255.0 - 0.5) / 0.25


def cnn_q88(img_u8):
    """Q8.8 int16 pixels as computed on the board (integer rounding)."""
    px = img_u8.astype(np.int32).reshape(-1)
    return ((1024 * px + 127) // 255 - 512).astype(np.int16)


# ---------------- SNN input ----------------
def sobel_int(img_u8):
    """3x3 Sobel Gx, Gy on raw 8-bit pixels (int32, BORDER_REFLECT_101 = cv2 default)."""
    gx = cv2.Sobel(img_u8, cv2.CV_16S, 1, 0, ksize=3).astype(np.int32)
    gy = cv2.Sobel(img_u8, cv2.CV_16S, 0, 1, ksize=3).astype(np.int32)
    return gx, gy


def snn_spikes(img_u8):
    """Edge spike maps, T = 1. Returns (spk_v, spk_h) bool (H, W).

    |G| = |Gx| + |Gy|; a pixel spikes when |G| * EDGE_DIV > max|G| (contrast
    adaptive, rejects sensor noise). The spike goes to channel 0 when the edge
    is vertical-ish (|Gx| >= |Gy|), else channel 1. All integer arithmetic.
    """
    gx, gy = sobel_int(img_u8)
    ax, ay = np.abs(gx), np.abs(gy)
    mag = ax + ay
    spk = mag * EDGE_DIV > mag.max()
    return spk & (ax >= ay), spk & (ax < ay)


def snn_bbox(spk):
    """Bounding square of all spikes: (ox, oy, S) with integer centring."""
    ys, xs = np.nonzero(spk)
    if xs.size == 0:
        return 0, 0, max(IMG_H, IMG_W)
    x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
    w, h = x1 - x0 + 1, y1 - y0 + 1
    S = max(w, h)
    return x0 - (S - w) // 2, y0 - (S - h) // 2, S


def snn_features_counts(img_u8):
    """Spike counts per cell of a GRID x GRID grid laid over the shape's
    bounding square (translation + scale normalisation).
    Order: (cy, cx, channel) -> (512,) int."""
    sv, sh = snn_spikes(img_u8)
    ox, oy, S = snn_bbox(sv | sh)
    counts = np.zeros((GRID, GRID, 2), np.int32)
    for ch, m in enumerate((sv, sh)):
        ys, xs = np.nonzero(m)
        cx = np.clip((xs - ox) * GRID // S, 0, GRID - 1)
        cy = np.clip((ys - oy) * GRID // S, 0, GRID - 1)
        np.add.at(counts, (cy, cx, ch), 1)
    return counts.reshape(-1)


def snn_features_int16(img_u8):
    """Features written to the SNN IP: min(count, 4) * 32  ->  0..128."""
    c = np.minimum(snn_features_counts(img_u8), COUNT_CLIP)
    return (c * (FEATURE_SCALE // COUNT_CLIP)).astype(np.int16)


def snn_features_float(img_u8):
    """Training input = int16 features / 128 (exactly representable)."""
    return snn_features_int16(img_u8).astype(np.float32) / FEATURE_SCALE
