#!/usr/bin/env python3
"""Plot sweep_perftest.py CSVs: bandwidth and latency with the CPU load they cost.

    plot_perftest.py sweep.csv [more.csv ...] --out docs/img

Writes bw_vs_size.svg (write, read, send, bidirectional write) and
lat_vs_size.svg (write, read, send). Each has the measure on top and the busy
CPU cores of both hosts below: solid for the client (the sender, except for
READ, where it is the reader), dashed for the server.
"""
import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FuncFormatter  # noqa: E402

plt.rcParams["svg.hashsalt"] = "tbv"  # deterministic ids
plt.rcParams["svg.fonttype"] = "none"


def size_label(v, _pos=None):
    v = int(v)
    for unit, div in (("MiB", 1 << 20), ("KiB", 1 << 10)):
        if v >= div:
            return f"{v // div} {unit}"
    return f"{v} B"


def load(paths):
    data = defaultdict(lambda: defaultdict(list))  # test -> label -> rows
    labels = []
    for path in paths:
        with open(path, newline="") as fh:
            for row in csv.DictReader(fh):
                if row["label"] not in labels:
                    labels.append(row["label"])
                data[row["test"]][row["label"]].append(row)
    return data, labels


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def figure(data, labels, tests, key, ylabel, title, out, logy=False):
    fig, axes = plt.subplots(2, len(tests), figsize=(4.2 * len(tests), 6.2),
                             sharex=True, gridspec_kw={"height_ratios": [2, 1]},
                             squeeze=False)
    colors = plt.rcParams["axes.prop_cycle"].by_key()["color"]
    for col, (test, name) in enumerate(tests):
        top, bottom = axes[0][col], axes[1][col]
        for i, label in enumerate(labels):
            rows = sorted(data.get(test, {}).get(label, []), key=lambda r: int(r["size"]))
            pts = [(int(r["size"]), num(r[key]), num(r["client_cores"]), num(r["server_cores"]))
                   for r in rows if num(r[key]) is not None]
            if not pts:
                continue
            x = [p[0] for p in pts]
            c = colors[i % len(colors)]
            top.plot(x, [p[1] for p in pts], marker="o", ms=3, color=c, label=label)
            bottom.plot(x, [p[2] for p in pts], color=c, ls="-", marker="o", ms=2)
            bottom.plot(x, [p[3] for p in pts], color=c, ls="--", marker="o", ms=2)
        top.set_title(name)
        top.set_xscale("log", base=2)
        if logy:
            top.set_yscale("log")
        top.grid(True, which="both", alpha=0.3)
        bottom.grid(True, alpha=0.3)
        bottom.xaxis.set_major_formatter(FuncFormatter(size_label))
        bottom.tick_params(axis="x", labelrotation=45)
        bottom.set_xlabel("message size")
        if col == 0:
            top.set_ylabel(ylabel)
            bottom.set_ylabel("busy CPU cores\n(— client, - - server)")
        bottom.set_ylim(bottom=0)
        if not logy:
            top.set_ylim(bottom=0)
    handles, names = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, names, loc="upper center", ncol=len(names), frameon=False,
               bbox_to_anchor=(0.5, 1.0))
    fig.suptitle(title, y=1.045)
    fig.tight_layout()
    fig.savefig(out, bbox_inches="tight", metadata={"Date": None})
    plt.close(fig)
    print(f"wrote {out}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv", nargs="+")
    p.add_argument("--out", default="docs/img")
    args = p.parse_args()
    data, labels = load(args.csv)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    figure(data, labels,
           [("ib_write_bw", "RDMA WRITE"), ("ib_read_bw", "RDMA READ"),
            ("ib_send_bw", "SEND"), ("ib_write_bw_bidir", "RDMA WRITE, both ways")],
           "gbps", "Gbit/s (1 QP)", "Bandwidth by message size", out / "bw_vs_size.svg")
    figure(data, labels,
           [("ib_write_lat", "RDMA WRITE"), ("ib_read_lat", "RDMA READ"),
            ("ib_send_lat", "SEND")],
           "lat_us", "average latency (µs)", "One-way latency by message size, 1 QP",
           out / "lat_vs_size.svg", logy=True)


if __name__ == "__main__":
    main()
