#!/usr/bin/env python3
"""
PC application: sends shape images to the PYNQ-Z2 over UART, receives the CNN
and SNN predictions, and visualises / evaluates them.

    python pc/app.py --port COM5 info                 # board alive? which nets?
    python pc/app.py --port COM5 echo -n 50           # UART link test (no inference)
    python pc/app.py --port COM5 eval                 # full test set, both nets
    python pc/app.py --port COM5 eval --net cnn -n 100
    python pc/app.py --port COM5 demo                 # image grid: true vs CNN vs SNN
    python pc/app.py --port COM5 demo --image my_photo.png
    python pc/app.py --sim ...                        # software board, no hardware

UART: 8N1, no flow control, default 4 000 000 baud (must match firmware UART_BAUD).
Results go to pc/results/<net>_<timestamp>/.
"""
import argparse
import csv
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import CLASSES, IMG_H, IMG_W, ROOT, list_split, load_gray  # noqa: E402
from pc.protocol import (CMD_BOTH, CMD_CNN, CMD_ECHO, CMD_SNN, Link,  # noqa: E402
                         STATUS, uart_time_s)

NET_CMD = {"cnn": CMD_CNN, "snn": CMD_SNN, "both": CMD_BOTH}
REQ_OVERHEAD = 2 + 3 + 2          # sync + cmd/len + crc
RSP_OVERHEAD = 2 + 4 + 2


def open_link(args):
    if args.sim:
        from pc.sim_board import SimBoard
        return Link(SimBoard(args.baud))
    import serial
    port = serial.Serial(args.port, args.baud, timeout=0.5, write_timeout=5)
    time.sleep(0.1)
    return Link(port, timeout=args.timeout)


def stats(v):
    v = np.asarray(v, float)
    return dict(mean=float(v.mean()), std=float(v.std()), min=float(v.min()), max=float(v.max()))


# ---------------------------------------------------------------------------
def cmd_info(link, args):
    print(json.dumps(link.info(), indent=2))


def cmd_echo(link, args):
    """UART link test with image-sized random payloads, independent of the IPs."""
    rng = np.random.default_rng(0)
    rtts, errors = [], 0
    for i in range(args.n):
        payload = rng.integers(0, 256, IMG_H * IMG_W, dtype=np.uint8).tobytes()
        r = link.transact(CMD_ECHO, payload)
        if not r.ok or r.payload != payload:
            errors += 1
        rtts.append(r.rtt_s)
    n_bytes = 2 * (len(payload)) + REQ_OVERHEAD + RSP_OVERHEAD
    print(f"echo: {args.n} x {len(payload)} B, errors {errors}")
    print(f"round trip: {np.mean(rtts) * 1e3:.2f} ms (min {np.min(rtts) * 1e3:.2f}), "
          f"wire time @ {args.baud} Bd = {uart_time_s(n_bytes, args.baud) * 1e3:.2f} ms, "
          f"effective {n_bytes / np.mean(rtts) / 1e3:.1f} kB/s")


def cmd_eval(link, args):
    items = list_split(args.split)
    if args.n:
        rng = np.random.default_rng(1)
        items = [items[i] for i in sorted(rng.choice(len(items), args.n, replace=False))]
    nets = ["CNN", "SNN"] if args.net == "both" else [args.net.upper()]
    out = ROOT / "pc" / "results" / f"{args.net}_{time.strftime('%Y%m%d_%H%M%S')}"
    out.mkdir(parents=True, exist_ok=True)

    rows, y_true, y_pred = [], [], {n: [] for n in nets}
    t_pre, t_ip = {n: [] for n in nets}, {n: [] for n in nets}
    t_rx, rtt = [], []
    t_start = time.perf_counter()
    for k, (path, label) in enumerate(items):
        r = link.infer(load_gray(path), NET_CMD[args.net])
        if not r.ok:
            raise RuntimeError(f"{path.name}: board status {STATUS.get(r.status, r.status)}")
        y_true.append(label)
        t_rx.append(r.t_rx_us); rtt.append(r.rtt_s * 1e6)
        row = dict(file=path.name, label=CLASSES[label], rtt_us=round(r.rtt_s * 1e6), t_rx_us=r.t_rx_us)
        for res in r.results:
            y_pred[res.net].append(res.pred)
            t_pre[res.net].append(res.t_pre_us); t_ip[res.net].append(res.t_ip_us)
            row.update({f"{res.net}_pred": CLASSES[res.pred], f"{res.net}_t_pre_us": res.t_pre_us,
                        f"{res.net}_t_ip_us": res.t_ip_us,
                        f"{res.net}_scores": " ".join(f"{s:.3f}" for s in res.scores)})
        rows.append(row)
        if k % 100 == 0 or k == len(items) - 1:
            msg = "  ".join(f"{n} acc {np.mean(np.array(y_pred[n]) == np.array(y_true)):.3f}" for n in nets)
            print(f"[{k + 1}/{len(items)}] {msg}")
    wall = time.perf_counter() - t_start

    with open(out / "per_image.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader(); w.writerows(rows)

    from sklearn.metrics import confusion_matrix
    y_true = np.array(y_true)
    req_bytes = IMG_H * IMG_W + REQ_OVERHEAD
    rsp_bytes = 4 + 26 * len(nets) + RSP_OVERHEAD
    summary = dict(split=args.split, images=len(items), baud=args.baud, sim=args.sim,
                   request_bytes=req_bytes, response_bytes=rsp_bytes,
                   uart_wire_time_us=uart_time_s(req_bytes + rsp_bytes, args.baud) * 1e6,
                   board_rx_time_us=stats(t_rx), round_trip_us=stats(rtt),
                   images_per_s_end_to_end=len(items) / wall, nets={})
    for n in nets:
        yp = np.array(y_pred[n])
        cm = confusion_matrix(y_true, yp, labels=range(len(CLASSES)))
        summary["nets"][n] = dict(
            accuracy=float((yp == y_true).mean()),
            per_class=dict(zip(CLASSES, (cm.diagonal() / cm.sum(1)).round(4).tolist())),
            confusion=cm.tolist(),
            t_pre_us=stats(t_pre[n]), t_ip_us=stats(t_ip[n]),
            ip_throughput_img_per_s=1e6 / float(np.mean(t_ip[n])) if np.mean(t_ip[n]) > 0 else None)
        plot_cm(cm, out / f"cm_{n}.png", f"{n} on board - {args.split} set")
    # communication overhead = round trip minus everything the board computes
    compute = sum(np.array(t_pre[n]) + np.array(t_ip[n]) for n in nets)
    summary["uart_overhead_us"] = stats(np.array(rtt) - compute)
    (out / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps({k: v for k, v in summary.items() if k != "nets"}, indent=2))
    for n, s in summary["nets"].items():
        print(f"{n}: acc {s['accuracy']:.4f}  per-class {s['per_class']}  "
              f"IP {s['t_ip_us']['mean']:.1f} us  pre {s['t_pre_us']['mean']:.1f} us")
    print("saved ->", out)


def plot_cm(cm, path, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from sklearn.metrics import ConfusionMatrixDisplay
    fig, ax = plt.subplots(figsize=(5, 4.5))
    ConfusionMatrixDisplay(cm, display_labels=CLASSES).plot(ax=ax, cmap="Blues", colorbar=False)
    ax.set_title(title); fig.tight_layout(); fig.savefig(path, dpi=150); plt.close(fig)


def cmd_demo(link, args):
    """Show each image with its true label and both predictions."""
    import matplotlib.pyplot as plt
    if args.image:
        items = [(Path(p), None) for p in args.image]
    else:
        all_items = list_split(args.split)
        rng = np.random.default_rng(args.seed)
        items = [all_items[i] for i in rng.choice(len(all_items), args.n, replace=False)]

    cols = min(4, len(items))
    rows = int(np.ceil(len(items) / cols))
    fig, axes = plt.subplots(rows, cols, figsize=(3.6 * cols, 3.2 * rows), squeeze=False)
    for ax, (path, label) in zip(axes.flat, items):
        img = load_gray(path)
        r = link.infer(img, NET_CMD[args.net])
        ax.imshow(img, cmap="gray", vmin=0, vmax=255)
        lines = [f"true: {CLASSES[label] if label is not None else '?'}"]
        ok_all = True
        for res in r.results:
            ok = label is None or res.pred == label
            ok_all &= ok
            lines.append(f"{res.net}: {CLASSES[res.pred]} {'OK' if ok else 'X'}  ({res.t_ip_us} us)")
        ax.set_title("\n".join(lines), fontsize=9, color="green" if ok_all else "red")
        ax.axis("off")
        print(f"{path.name}: " + " | ".join(lines) + f" | rtt {r.rtt_s * 1e3:.1f} ms")
    for ax in axes.flat[len(items):]:
        ax.axis("off")
    fig.suptitle(f"PC -> UART -> PYNQ-Z2 -> CNN/SNN -> UART -> PC {'(SIM)' if args.sim else ''}")
    fig.tight_layout()
    out = ROOT / "pc" / "results"
    out.mkdir(parents=True, exist_ok=True)
    fig.savefig(out / "demo.png", dpi=150)
    if not args.no_show:
        plt.show()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--baud", type=int, default=4_000_000)
    ap.add_argument("--timeout", type=float, default=3.0)
    ap.add_argument("--sim", action="store_true", help="use the software board model")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    p = sub.add_parser("echo"); p.add_argument("-n", type=int, default=20)
    p = sub.add_parser("eval")
    p.add_argument("--net", choices=NET_CMD, default="both")
    p.add_argument("--split", default="test")
    p.add_argument("-n", type=int, default=0, help="random subset size (0 = all)")
    p = sub.add_parser("demo")
    p.add_argument("--net", choices=NET_CMD, default="both")
    p.add_argument("--split", default="test")
    p.add_argument("-n", type=int, default=8)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--image", nargs="*", help="custom image files instead of the test set")
    p.add_argument("--no-show", action="store_true")
    args = ap.parse_args()

    link = open_link(args)
    try:
        {"info": cmd_info, "echo": cmd_echo, "eval": cmd_eval, "demo": cmd_demo}[args.cmd](link, args)
    finally:
        link.port.close()


if __name__ == "__main__":
    main()
