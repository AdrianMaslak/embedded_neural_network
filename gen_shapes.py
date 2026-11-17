#!/usr/bin/env python3
"""
Synthetic shape dataset generator for Assignment 2 (CNN vs SNN on PYNQ-Z2).

Produces 60x80 (H x W) 8-bit grayscale PNGs, one shape per image:
    0 Circle, 1 Square, 2 Triangle, 3 Figure8

Variations: size, position, rotation, line thickness, outline vs filled,
background level + lighting gradient, contrast, blur, sensor noise.

Splits are generated from independent random streams (same base seed,
different stream index), so train / val / test never share an image and
the dataset is fully reproducible. Both networks must use these folders.

Layout:
    <out>/train/<Class>/<class>_00000.png
    <out>/val/...
    <out>/test/...
    <out>/labels.csv     (every image + its generation parameters)
    <out>/preview.png    (sample grid for the report)

Usage:
    python gen_shapes.py                        # defaults below
    python gen_shapes.py --n-train 2000 --out dataset_big
"""
import argparse
import csv
from pathlib import Path

import cv2
import numpy as np

IMG_H, IMG_W = 60, 80
SS = 4  # supersampling factor for anti-aliased drawing

CLASSES = ["Circle", "Square", "Triangle", "Figure8"]
SPLITS = ["train", "val", "test"]

# Shape extent (largest bbox side, final pixels) and line thickness (final pixels)
SIZE_RANGE = (16, 54)
THICK_RANGE = (1.0, 4.0)
FILLED_PROB = 0.25
MARGIN = 2  # px kept free at the image border


# ---------------------------------------------------------------------------
# Shape geometry (unit scale, roughly centred at origin). Returns list of
# closed polylines, each an (N, 2) float array of (x, y).
# ---------------------------------------------------------------------------
def circle_pts(cx=0.0, cy=0.0, r=1.0, n=96):
    t = np.linspace(0, 2 * np.pi, n, endpoint=False)
    return np.stack([cx + r * np.cos(t), cy + r * np.sin(t)], axis=1)


def make_shape(cls, rng, filled=False):
    if cls == "Circle":
        return [circle_pts()]

    if cls == "Square":
        return [np.array([[-1, -1], [1, -1], [1, 1], [-1, 1]], dtype=np.float64)]

    if cls == "Triangle":
        # Equilateral base with mild vertex jitter (still clearly a triangle)
        ang = np.deg2rad([90, 210, 330])
        pts = np.stack([np.cos(ang), np.sin(ang)], axis=1)
        pts += rng.uniform(-0.12, 0.12, size=pts.shape)
        return [pts]

    if cls == "Figure8":
        # filled lemniscate looks like a bowtie, so filled 8s use two discs
        if filled or rng.random() < 0.5:
            # Two stacked loops, touching / slightly overlapping / slight gap
            k = rng.uniform(0.88, 1.05)
            r2 = rng.uniform(0.8, 1.0)  # lower loop may be a bit bigger
            return [circle_pts(0, -k * r2, r2), circle_pts(0, k, 1.0)]
        # Lemniscate of Gerono, vertical: x = sin t cos t, y = sin t
        t = np.linspace(0, 2 * np.pi, 160, endpoint=False)
        w = rng.uniform(0.8, 1.2)
        return [np.stack([w * np.sin(t) * np.cos(t), np.sin(t)], axis=1)]

    raise ValueError(cls)


def place(polys, size, angle_deg, rng):
    """Rotate, scale to `size` px, and translate to a random in-bounds centre."""
    a = np.deg2rad(angle_deg)
    rot = np.array([[np.cos(a), -np.sin(a)], [np.sin(a), np.cos(a)]])
    polys = [p @ rot.T for p in polys]

    allp = np.vstack(polys)
    lo, hi = allp.min(0), allp.max(0)
    ext = (hi - lo).max()
    centre = (lo + hi) / 2
    polys = [(p - centre) * (size / ext) for p in polys]

    allp = np.vstack(polys)
    lo, hi = allp.min(0), allp.max(0)
    w, h = hi - lo
    # shrink if it would not fit
    fit = min((IMG_W - 2 * MARGIN) / w, (IMG_H - 2 * MARGIN) / h, 1.0)
    if fit < 1.0:
        polys = [p * fit for p in polys]
        lo, hi = lo * fit, hi * fit

    cx = rng.uniform(MARGIN - lo[0], IMG_W - MARGIN - hi[0])
    cy = rng.uniform(MARGIN - lo[1], IMG_H - MARGIN - hi[1])
    return [p + [cx, cy] for p in polys], (cx, cy)


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------
def render(cls, rng):
    size = rng.uniform(*SIZE_RANGE)
    angle = 0.0 if cls == "Circle" else rng.uniform(0, 360)
    thick = rng.uniform(*THICK_RANGE)
    filled = rng.random() < FILLED_PROB

    polys, (cx, cy) = place(make_shape(cls, rng, filled), size, angle, rng)

    # Mask at supersampled resolution
    mask = np.zeros((IMG_H * SS, IMG_W * SS), np.uint8)
    pts = [np.round(p * SS).astype(np.int32) for p in polys]
    if filled:
        for p in pts:  # each loop filled separately (8 -> two solid lobes)
            cv2.fillPoly(mask, [p], 255, lineType=cv2.LINE_AA)
    else:
        t = max(1, int(round(thick * SS)))
        cv2.polylines(mask, pts, True, 255, thickness=t, lineType=cv2.LINE_AA)
    m = cv2.resize(mask, (IMG_W, IMG_H), interpolation=cv2.INTER_AREA)
    m = m.astype(np.float32) / 255.0

    # Background: base level + linear lighting gradient
    bg = rng.uniform(150, 235)
    gx, gy = rng.uniform(-25, 25, size=2)
    yy, xx = np.mgrid[0:IMG_H, 0:IMG_W].astype(np.float32)
    background = bg + gx * (xx / IMG_W - 0.5) + gy * (yy / IMG_H - 0.5)

    # Dark shape, at least ~60 grey levels contrast
    fg = rng.uniform(0, max(10.0, bg - 60))
    img = background * (1 - m) + fg * m

    blur = rng.random() < 0.3
    if blur:
        img = cv2.GaussianBlur(img, (3, 3), rng.uniform(0.3, 0.9))
    noise = rng.uniform(0, 8)
    img = img + rng.normal(0, noise, img.shape)
    img = np.clip(np.round(img), 0, 255).astype(np.uint8)

    params = dict(size=round(size, 2), angle=round(angle, 1), thickness=round(thick, 2),
                  filled=int(filled), cx=round(cx, 2), cy=round(cy, 2),
                  bg=round(bg, 1), fg=round(fg, 1), blur=int(blur), noise=round(noise, 2))
    return img, params


# ---------------------------------------------------------------------------
def make_preview(out, per_class=10, scale=3):
    rows = []
    for c in CLASSES:
        files = sorted((out / "train" / c).glob("*.png"))[:per_class]
        tiles = [cv2.imread(str(f), cv2.IMREAD_GRAYSCALE) for f in files]
        tiles = [cv2.copyMakeBorder(t, 1, 1, 1, 1, cv2.BORDER_CONSTANT, value=255) for t in tiles]
        rows.append(np.hstack(tiles))
    grid = np.vstack(rows)
    grid = cv2.resize(grid, None, fx=scale, fy=scale, interpolation=cv2.INTER_NEAREST)
    cv2.imwrite(str(out / "preview.png"), grid)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(__file__).parent / "dataset"))
    ap.add_argument("--n-train", type=int, default=1500, help="images per class")
    ap.add_argument("--n-val", type=int, default=300)
    ap.add_argument("--n-test", type=int, default=300)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    out = Path(args.out)
    counts = {"train": args.n_train, "val": args.n_val, "test": args.n_test}

    out.mkdir(parents=True, exist_ok=True)
    with open(out / "labels.csv", "w", newline="") as f:
        writer = None
        for si, split in enumerate(SPLITS):
            for ci, cls in enumerate(CLASSES):
                # independent stream per (split, class) -> reproducible, disjoint
                rng = np.random.default_rng([args.seed, si, ci])
                d = out / split / cls
                d.mkdir(parents=True, exist_ok=True)
                for i in range(counts[split]):
                    img, p = render(cls, rng)
                    name = f"{cls.lower()}_{i:05d}.png"
                    cv2.imwrite(str(d / name), img)
                    row = dict(split=split, label=ci, cls=cls, file=f"{split}/{cls}/{name}", **p)
                    if writer is None:
                        writer = csv.DictWriter(f, fieldnames=list(row))
                        writer.writeheader()
                    writer.writerow(row)
            print(f"[{split}] {counts[split]} x {len(CLASSES)} classes")

    make_preview(out)
    print(f"Done -> {out.resolve()}")


if __name__ == "__main__":
    main()
