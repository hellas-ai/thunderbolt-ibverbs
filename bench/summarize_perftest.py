#!/usr/bin/env python3
"""Summarize tbv-perftest CSV output.

The runner writes large JSONL files with full stdout/stderr and telemetry
snapshots. This script intentionally consumes only the compact CSV sidecar so
benchmark reports can be checked into Git without carrying those raw artifacts.
"""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path


def num(row: dict[str, str], key: str) -> float | None:
    value = row.get(key, "")
    if value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


def load_rows(paths: list[Path]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for path in paths:
        with path.open(newline="") as handle:
            for row in csv.DictReader(handle):
                row["_source"] = str(path)
                rows.append(row)
    return rows


def print_row_counts(rows: list[dict[str, str]]) -> None:
    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        grouped[row["plan"]].append(row)

    print("## Row counts")
    for plan in sorted(grouped):
        plan_rows = grouped[plan]
        failures = sum(1 for row in plan_rows if row["status"] != "ok")
        print(f"- {plan}: {len(plan_rows)} rows, {failures} non-ok")
    print()


def print_bandwidth(rows: list[dict[str, str]]) -> None:
    best: dict[tuple[str, str, str], dict[str, str]] = {}
    for row in rows:
        if row["status"] != "ok" or row["kind"] != "bw":
            continue
        key = (row["plan"], row["verb"], row["direction"])
        value = num(row, "bw_avg_gbps")
        if value is None:
            continue
        old = best.get(key)
        if old is None or value > (num(old, "bw_avg_gbps") or 0):
            best[key] = row

    print("## Peak bandwidth")
    for key in sorted(best):
        row = best[key]
        print(
            "- {plan} {direction} ib_{verb}_bw: {bw:.2f} Gb/s "
            "({case}, size={size}, qps={qps})".format(
                plan=row["plan"],
                direction=row["direction"],
                verb=row["verb"],
                bw=num(row, "bw_avg_gbps") or 0,
                case=row["case"],
                size=row["size_bytes"],
                qps=row["qps"],
            )
        )
    print()


def print_latency(rows: list[dict[str, str]]) -> None:
    latency = [
        row
        for row in rows
        if row["status"] == "ok" and row["kind"] == "lat"
    ]

    print("## Latency")
    for plan in sorted({row["plan"] for row in latency}):
        print(f"### {plan}")
        print("| size | direction | verb | typical us | avg us | p99 us | p99.9 us | max us |")
        print("|---:|---|---|---:|---:|---:|---:|---:|")
        for row in sorted(
            (row for row in latency if row["plan"] == plan),
            key=lambda row: (
                int(row["size_bytes"]),
                row["direction"],
                row["verb"],
            ),
        ):
            print(
                "| {size} | {direction} | {verb} | {typ:.2f} | {avg:.2f} | "
                "{p99:.2f} | {p999:.2f} | {maxv:.2f} |".format(
                    size=row["size_bytes"],
                    direction=row["direction"],
                    verb=row["verb"],
                    typ=num(row, "lat_typical_us") or 0,
                    avg=num(row, "lat_avg_us") or 0,
                    p99=num(row, "lat_p99_us") or 0,
                    p999=num(row, "lat_p999_us") or 0,
                    maxv=num(row, "lat_max_us") or 0,
                )
            )
        print()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", nargs="+", type=Path)
    args = parser.parse_args()

    rows = load_rows(args.csv)
    print("# Perftest Summary")
    print()
    print_row_counts(rows)
    print_bandwidth(rows)
    print_latency(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
