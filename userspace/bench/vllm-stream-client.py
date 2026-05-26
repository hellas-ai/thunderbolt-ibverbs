#!/usr/bin/env python3
"""Streaming vLLM benchmark client.

This measures what the OpenAI-compatible completions API exposes directly:
time-to-first-token and total request time. In the CSV, prefill time is the
TTFT proxy: it includes queueing plus prompt prefill plus the first decode
step. Decode time is total time after TTFT.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import os
import socket
import statistics
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any


FIELDS = [
    "timestamp_utc",
    "run_id",
    "host",
    "transport",
    "parallelism",
    "model",
    "concurrency",
    "max_tokens",
    "prompt_chars",
    "prompt_tokens_per_req",
    "requests_ok",
    "requests_failed",
    "status",
    "skip_reason",
    "wall_s",
    "total_prompt_tokens",
    "total_completion_tokens",
    "total_tps",
    "ttft_mean_s",
    "ttft_p50_s",
    "ttft_p95_s",
    "prefill_mean_s",
    "decode_mean_s",
    "decode_tps_mean",
    "output_tps_mean",
    "endpoint",
    "socket_ifname",
    "rdma_hca",
    "vllm_extra_args",
    "server_log",
    "errors",
]


def now_utc() -> str:
    return dt.datetime.now(dt.UTC).isoformat(timespec="seconds")


def percentile(values: list[float], pct: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (pct / 100.0) * (len(ordered) - 1)
    lo = math.floor(rank)
    hi = math.ceil(rank)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (rank - lo)


def fmt(value: float | int | str | None) -> str:
    if value is None:
        return ""
    if isinstance(value, float):
        if math.isnan(value) or math.isinf(value):
            return ""
        return f"{value:.6f}"
    return str(value)


def base_row(args: argparse.Namespace, prompt: str) -> dict[str, str]:
    return {
        "timestamp_utc": now_utc(),
        "run_id": args.run_id,
        "host": socket.gethostname(),
        "transport": args.transport,
        "parallelism": args.parallelism,
        "model": args.model,
        "concurrency": str(args.concurrency),
        "max_tokens": str(args.max_tokens),
        "prompt_chars": str(len(prompt)),
        "endpoint": args.endpoint,
        "socket_ifname": args.socket_ifname,
        "rdma_hca": args.rdma_hca,
        "vllm_extra_args": args.vllm_extra_args,
        "server_log": args.server_log,
    }


def append_csv(path: Path, row: dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    exists = path.exists() and path.stat().st_size > 0
    with path.open("a", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELDS, extrasaction="ignore")
        if not exists:
            writer.writeheader()
        writer.writerow({field: row.get(field, "") for field in FIELDS})


def stream_one(
    *,
    endpoint: str,
    model: str,
    prompt: str,
    max_tokens: int,
    temperature: float,
    ignore_eos: bool,
    timeout_s: float,
    barrier: threading.Barrier,
    request_index: int,
) -> dict[str, Any]:
    payload = {
        "model": model,
        "prompt": prompt,
        "max_tokens": max_tokens,
        "temperature": temperature,
        "ignore_eos": ignore_eos,
        "stream": True,
        "stream_options": {"include_usage": True},
    }

    barrier.wait()
    start = time.perf_counter()
    first_token: float | None = None
    usage: dict[str, Any] = {}
    text_parts: list[str] = []

    try:
        req = urllib.request.Request(
            endpoint,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            for raw in resp:
                line = raw.decode("utf-8", errors="replace").strip()
                if not line or not line.startswith("data:"):
                    continue

                data = line[5:].strip()
                if data == "[DONE]":
                    break

                obj = json.loads(data)
                if obj.get("usage") is not None:
                    usage = obj["usage"]

                for choice in obj.get("choices", []):
                    piece = choice.get("text") or ""
                    if piece:
                        if first_token is None:
                            first_token = time.perf_counter()
                        text_parts.append(piece)

        end = time.perf_counter()
        completion_tokens = usage.get("completion_tokens")
        prompt_tokens = usage.get("prompt_tokens")
        duration = end - start
        ttft = None if first_token is None else first_token - start
        decode = None if ttft is None else max(0.0, duration - ttft)

        return {
            "request_index": request_index,
            "ok": True,
            "start": start,
            "end": end,
            "duration_s": duration,
            "ttft_s": ttft,
            "decode_s": decode,
            "prompt_tokens": prompt_tokens,
            "completion_tokens": completion_tokens,
            "text_bytes": len("".join(text_parts).encode("utf-8")),
            "error": "",
        }
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")[:4000]
        return {
            "request_index": request_index,
            "ok": False,
            "start": start,
            "end": time.perf_counter(),
            "error": f"HTTP {exc.code}: {body}",
        }
    except Exception as exc:  # noqa: BLE001 - benchmark row should record failures.
        return {
            "request_index": request_index,
            "ok": False,
            "start": start,
            "end": time.perf_counter(),
            "error": repr(exc),
        }


def aggregate(args: argparse.Namespace, prompt: str) -> dict[str, str]:
    barrier = threading.Barrier(args.concurrency)
    wall_start = time.perf_counter()
    rows: list[dict[str, Any]] = []

    with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        futures = [
            pool.submit(
                stream_one,
                endpoint=args.endpoint,
                model=args.model,
                prompt=prompt,
                max_tokens=args.max_tokens,
                temperature=args.temperature,
                ignore_eos=args.ignore_eos,
                timeout_s=args.timeout,
                barrier=barrier,
                request_index=i,
            )
            for i in range(args.concurrency)
        ]
        for fut in as_completed(futures):
            rows.append(fut.result())

    wall_s = time.perf_counter() - wall_start
    ok_rows = [row for row in rows if row.get("ok")]
    bad_rows = [row for row in rows if not row.get("ok")]
    prompt_tokens = [
        int(row["prompt_tokens"])
        for row in ok_rows
        if row.get("prompt_tokens") is not None
    ]
    completion_tokens = [
        int(row["completion_tokens"])
        for row in ok_rows
        if row.get("completion_tokens") is not None
    ]
    ttfts = [
        float(row["ttft_s"])
        for row in ok_rows
        if row.get("ttft_s") is not None
    ]
    decodes = [
        float(row["decode_s"])
        for row in ok_rows
        if row.get("decode_s") is not None
    ]
    decode_tps = []
    output_tps = []
    for row in ok_rows:
        ctok = row.get("completion_tokens")
        duration = row.get("duration_s")
        decode_s = row.get("decode_s")
        if ctok is None or duration is None:
            continue
        ctok_i = int(ctok)
        if duration > 0:
            output_tps.append(ctok_i / duration)
        if decode_s and decode_s > 0:
            decode_tps.append(max(0, ctok_i - 1) / decode_s)

    total_completion = sum(completion_tokens)
    total_prompt = sum(prompt_tokens)
    errors = " | ".join(str(row.get("error", ""))[:300] for row in bad_rows)

    row = base_row(args, prompt)
    row.update(
        {
            "prompt_tokens_per_req": fmt(statistics.median(prompt_tokens) if prompt_tokens else None),
            "requests_ok": str(len(ok_rows)),
            "requests_failed": str(len(bad_rows)),
            "status": "ok" if ok_rows and not bad_rows else ("partial" if ok_rows else "failed"),
            "skip_reason": "",
            "wall_s": fmt(wall_s),
            "total_prompt_tokens": str(total_prompt),
            "total_completion_tokens": str(total_completion),
            "total_tps": fmt(total_completion / wall_s if wall_s > 0 else None),
            "ttft_mean_s": fmt(statistics.mean(ttfts) if ttfts else None),
            "ttft_p50_s": fmt(percentile(ttfts, 50)),
            "ttft_p95_s": fmt(percentile(ttfts, 95)),
            "prefill_mean_s": fmt(statistics.mean(ttfts) if ttfts else None),
            "decode_mean_s": fmt(statistics.mean(decodes) if decodes else None),
            "decode_tps_mean": fmt(statistics.mean(decode_tps) if decode_tps else None),
            "output_tps_mean": fmt(statistics.mean(output_tps) if output_tps else None),
            "errors": errors,
        }
    )
    return row


def parse_args() -> argparse.Namespace:
    def parse_bool(value: str) -> bool:
        lowered = value.lower()
        if lowered in {"1", "true", "yes", "on"}:
            return True
        if lowered in {"0", "false", "no", "off"}:
            return False
        raise argparse.ArgumentTypeError(f"invalid boolean: {value}")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", required=True, type=Path)
    parser.add_argument("--endpoint", default="http://127.0.0.1:8000/v1/completions")
    parser.add_argument("--model", required=True)
    parser.add_argument("--transport", required=True)
    parser.add_argument("--parallelism", required=True)
    parser.add_argument("--concurrency", required=True, type=int)
    parser.add_argument("--max-tokens", required=True, type=int)
    parser.add_argument("--prompt-file", type=Path)
    parser.add_argument("--prompt", default="")
    parser.add_argument("--temperature", default=0.0, type=float)
    parser.add_argument("--ignore-eos", default=True, type=parse_bool)
    parser.add_argument("--timeout", default=900.0, type=float)
    parser.add_argument("--run-id", default="")
    parser.add_argument("--socket-ifname", default="")
    parser.add_argument("--rdma-hca", default="")
    parser.add_argument("--vllm-extra-args", default="")
    parser.add_argument("--server-log", default="")
    parser.add_argument("--skip-reason", default="")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.prompt_file:
        prompt = args.prompt_file.read_text()
    else:
        prompt = args.prompt

    if args.skip_reason:
        row = base_row(args, prompt)
        row.update(
            {
                "requests_ok": "0",
                "requests_failed": "0",
                "status": "skipped",
                "skip_reason": args.skip_reason,
            }
        )
        append_csv(args.csv, row)
        return 0

    if args.concurrency < 1:
        print("--concurrency must be >= 1", file=sys.stderr)
        return 2

    row = aggregate(args, prompt)
    append_csv(args.csv, row)
    print(
        "  "
        f"conc={args.concurrency} max={args.max_tokens}: "
        f"status={row['status']} total_tps={row['total_tps']} "
        f"ttft_p50={row['ttft_p50_s']} decode_tps_mean={row['decode_tps_mean']}"
    )
    return 0 if row["status"] in {"ok", "partial"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
