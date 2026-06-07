#!/usr/bin/env python3
"""Run a Nix-generated perftest plan on two hosts.

Nix owns the benchmark matrix. This runner owns the live parts that Nix should
not do: SSH process supervision, per-row timeouts, result parsing, and telemetry
snapshots before and after each case.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import fnmatch
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


IMPORTANT_COUNTERS = [
    "verbs_qps",
    "verbs_mrs",
    "verbs_recv_wqes",
    "data_wr_send",
    "data_wr_op_send",
    "data_wr_op_send_imm",
    "data_wr_op_write",
    "data_wr_op_write_imm",
    "data_wr_op_unsupported",
    "data_wr_live",
    "data_wr_no_path",
    "data_wr_copied",
    "data_wr_zcopy",
    "data_wr_zcopy_fallback",
    "data_wr_zcopy_fallback_striping",
    "data_wr_zcopy_fallback_unsafe_sge",
    "data_wr_copy_error",
    "data_wr_no_recv_credit",
    "data_wr_path_send",
    "data_wr_path_send_error",
    "data_wr_retransmit",
    "data_wr_rnr_retransmit",
    "data_wr_retry_enqueue_error",
    "data_wr_retry_exhausted",
    "data_wr_rnr_retry_exhausted",
    "data_wr_timeout",
    "data_tx_accepted",
    "data_tx_posted",
    "data_tx_completed",
    "data_tx_canceled",
    "data_tx_errors",
    "data_tx_credit_stalls",
    "data_tx_credit_received",
    "data_rx_completed",
    "data_rx_canceled",
    "data_rx_credit_sent",
    "data_rx_credit_send_error",
    "data_rx_repost_failed",
    "data_rx_bad_frame",
    "data_rx_bad_header",
    "data_rx_bad_header_parse",
    "data_rx_bad_header_len",
    "data_rx_bad_header_path_credit",
    "data_rx_bad_header_opcode",
    "data_rx_bad_header_recv_credit",
    "data_rx_bad_header_ack",
    "data_rx_bad_header_write",
    "data_rx_bad_header_read_req",
    "data_rx_bad_header_mad",
    "data_rx_send",
    "data_rx_op_send",
    "data_rx_op_send_imm",
    "data_rx_op_write",
    "data_rx_op_write_imm",
    "data_rx_ack",
    "data_rx_ack_matched",
    "data_rx_ack_match_retried",
    "data_rx_ack_match_max_ms",
    "data_rx_ack_match_current_max_ms",
    "data_rx_ack_match_over_10ms",
    "data_rx_ack_match_over_64ms",
    "data_rx_ack_miss",
    "data_rx_late_ack",
    "data_rx_ack_cumulative",
    "data_tx_ack_ok",
    "data_tx_ack_rnr",
    "data_tx_ack_error",
    "data_tx_ack_send_error",
    "data_rx_ack_rnr",
    "data_rx_duplicate_ack",
    "data_rx_ack_history_miss",
    "data_tx_read_ack_ok",
    "data_tx_read_ack_retry",
    "data_tx_read_ack_error",
    "data_rx_read_ack_ok",
    "data_rx_read_ack_retry",
    "data_rx_read_ack_error",
    "data_read_resp_retransmit",
    "data_read_resp_drop",
    "data_rx_read_resp_duplicate",
    "data_rx_read_resp_gap",
    "data_rx_read_resp_remote_error",
    "data_rx_read_resp_bad_header",
    "data_rx_read_resp_copy_error",
    "data_rx_read_resp_short",
    "data_rx_read_req_no_access",
    "data_rx_read_req_no_mr",
    "data_rx_read_req_mr_access",
    "data_rx_read_req_too_large",
    "data_rx_read_req_bad_iova",
    "data_rx_read_req_alloc_error",
    "data_rx_read_req_resp_busy",
    "data_rx_read_req_resp_error",
    "data_rx_no_qp",
    "data_rx_bad_peer",
    "data_rx_unconnected_qp",
    "data_rx_qp_error",
    "data_rx_no_recv",
    "data_rx_copy_error",
    "data_rx_send_len_error",
    "data_rx_send_prot_error",
    "data_rx_send_cq_error",
    "data_rx_send_bad_fragment",
    "data_rx_send_sequence_error",
    "data_rx_active_timeout",
    "data_rx_reorder_buffered",
    "data_rx_reorder_delivered",
    "data_rx_reorder_dropped",
    "data_rx_reorder_timeout",
    "data_rx_reorder_window",
    "data_rx_pending_discarded",
    "data_cq_overflow",
]

PEER_COUNTERS = [
    "data_tx_enqueued",
    "data_tx_posted",
    "data_tx_completed",
    "data_tx_credit_stalls",
    "data_tx_credit_received",
    "data_rx_completed",
    "data_rx_canceled",
    "data_rx_credit_sent",
    "data_rx_credit_send_error",
    "data_rx_repost_failed",
    "rx_credit_pending",
]

FATAL_COUNTERS = [
    "data_wr_no_path",
    "data_wr_copy_error",
    "data_wr_path_send_error",
    "data_wr_retry_enqueue_error",
    "data_wr_retry_exhausted",
    "data_wr_rnr_retry_exhausted",
    "data_wr_timeout",
    "data_tx_errors",
    "data_tx_ack_error",
    "data_tx_ack_send_error",
    "data_tx_read_ack_error",
    "data_rx_canceled",
    "data_rx_credit_send_error",
    "data_rx_repost_failed",
    "data_rx_bad_frame",
    "data_rx_bad_header",
    "data_rx_bad_header_parse",
    "data_rx_bad_header_len",
    "data_rx_bad_header_path_credit",
    "data_rx_bad_header_opcode",
    "data_rx_bad_header_recv_credit",
    "data_rx_bad_header_ack",
    "data_rx_bad_header_write",
    "data_rx_bad_header_read_req",
    "data_rx_bad_header_mad",
    "data_rx_ack_miss",
    "data_rx_ack_history_miss",
    "data_rx_read_ack_error",
    "data_rx_read_resp_remote_error",
    "data_rx_read_resp_bad_header",
    "data_rx_read_resp_copy_error",
    "data_rx_read_resp_short",
    "data_rx_read_req_no_access",
    "data_rx_read_req_no_mr",
    "data_rx_read_req_mr_access",
    "data_rx_read_req_too_large",
    "data_rx_read_req_bad_iova",
    "data_rx_read_req_alloc_error",
    "data_rx_read_req_resp_error",
    "data_read_resp_drop",
    "data_rx_no_qp",
    "data_rx_bad_peer",
    "data_rx_unconnected_qp",
    "data_rx_qp_error",
    "data_rx_no_recv",
    "data_rx_copy_error",
    "data_rx_send_len_error",
    "data_rx_send_prot_error",
    "data_rx_send_cq_error",
    "data_rx_send_bad_fragment",
    "data_rx_send_sequence_error",
    "data_rx_active_timeout",
    "data_rx_reorder_dropped",
    "data_rx_reorder_timeout",
    "data_rx_reorder_window",
    "data_rx_pending_discarded",
    "data_cq_overflow",
]

CSV_FIELDS = [
    "timestamp_utc",
    "plan",
    "repeat_index",
    "repeat_count",
    "case",
    "tool",
    "verb",
    "kind",
    "server",
    "client",
    "kernel",
    "module_id",
    "module_id_source",
    "module_sha256",
    "module_ko_path",
    "iommu",
    "direction",
    "dev",
    "server_dev",
    "client_dev",
    "server_data_addr",
    "client_data_addr",
    "gid_index",
    "port",
    "status",
    "duration_s",
    "size_bytes",
    "qps",
    "iters",
    "tx_depth",
    "rx_depth",
    "connection",
    "bidirectional",
    "bw_peak_gbps",
    "bw_avg_gbps",
    "msg_rate_mpps",
    "lat_min_us",
    "lat_max_us",
    "lat_typical_us",
    "lat_avg_us",
    "lat_stdev_us",
    "lat_p99_us",
    "lat_p999_us",
    "server_ready_rails",
    "client_ready_rails",
    "server_rail_speeds",
    "client_rail_speeds",
    "server_error_delta",
    "client_error_delta",
    "server_zcopy_delta",
    "server_copy_delta",
    "client_zcopy_delta",
    "client_copy_delta",
    "server_path_send_error_delta",
    "client_path_send_error_delta",
    "error",
]

for prefix in ("server", "client"):
    for counter in IMPORTANT_COUNTERS:
        CSV_FIELDS.append(f"{prefix}_delta_{counter}")
    for counter in PEER_COUNTERS:
        CSV_FIELDS.append(f"{prefix}_delta_rail_{counter}")


def die(msg: str) -> None:
    print(f"tbv-perftest: {msg}", file=sys.stderr)
    raise SystemExit(1)


def require_env(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        die(f"{name} is not set")
    return value


RDMA_CORE = require_env("TBV_RDMA_CORE")
PERFTEST = require_env("TBV_PERFTEST")
PERFTEST_DARWIN = os.environ.get("TBV_PERFTEST_DARWIN", "")


# Set once per run by detect_systems(); maps hostname -> "Linux" / "Darwin".
HOST_SYSTEM: dict[str, str] = {}
SSH_CONFIG = ""
SSH_OPTIONS: list[str] = []


def perftest_for(host: str) -> str:
    return PERFTEST_DARWIN if HOST_SYSTEM.get(host) == "Darwin" else PERFTEST


def rdma_core_for(host: str) -> str:
    # Darwin uses dyld dynamic-lookup; no rdma-core build, no LD_LIBRARY_PATH.
    return "" if HOST_SYSTEM.get(host) == "Darwin" else RDMA_CORE


def ssh_args(target: str, command: str) -> list[str]:
    args = [
        "ssh",
    ]
    if SSH_CONFIG:
        args.extend(["-F", SSH_CONFIG])
    args.extend([
        "-o",
        "ConnectTimeout=8",
        "-o",
        "BatchMode=yes",
    ])
    for option in SSH_OPTIONS:
        args.extend(["-o", option])
    args.extend([
        target,
        "sudo -n bash -lc " + shlex.quote(command),
    ])
    return args


def run_local(args: list[str], *, check: bool = True, timeout: int | None = None) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
    )
    if check and proc.returncode != 0:
        raise RuntimeError(proc.stdout)
    return proc


def run_ssh(target: str, command: str, *, check: bool = True, timeout: int | None = None) -> subprocess.CompletedProcess[str]:
    return run_local(ssh_args(target, command), check=check, timeout=timeout)


def detect_system(host: str) -> str:
    """Return 'Linux' or 'Darwin' for the host via `uname -s`."""
    out = run_ssh(host, "uname -s", check=False, timeout=10).stdout.strip()
    return out or "Linux"


def copy_tools(host: str) -> None:
    if HOST_SYSTEM.get(host) == "Darwin":
        # Darwin closures are built natively (via remote builder); the
        # linux closure isn't substitutable here. The mac already has its
        # perftest path in its own nix store.
        return
    run_local(["nix-copy-closure", "--to", host, RDMA_CORE, PERFTEST], check=True)


def now_utc() -> str:
    return dt.datetime.now(dt.UTC).isoformat(timespec="seconds")


def parse_key_values(text: str) -> dict[str, int]:
    out: dict[str, int] = {}
    for raw in text.splitlines():
        match = re.match(r"\s*([A-Za-z0-9_]+):\s*(-?[0-9]+)\s*$", raw)
        if match:
            out[match.group(1)] = int(match.group(2))
    return out


def backend_matches(current: str | None, requested: str | None) -> bool:
    if not requested:
        return True
    if current is None:
        return False
    native_aliases = {"native", "native-linux", "tbverbs"}
    if requested in native_aliases:
        return current in native_aliases
    return current == requested


def normalize_rail_speed(value: str) -> str:
    value = value.strip()
    if re.fullmatch(r"[0-9]+", value):
        return f"{value}Gb/s"
    return value


def parse_peer_totals(text: str, backend: str | None) -> tuple[int, list[str], dict[str, int]]:
    current_backend = None
    ready_rails = 0
    speeds: set[str] = set()
    totals = {name: 0 for name in PEER_COUNTERS}
    for raw in text.splitlines():
        line = raw.strip()
        peer_match = re.match(r"peer\s+\S+\s+backend=([^ ]+)", line)
        if peer_match:
            current_backend = peer_match.group(1)
            continue
        if not backend_matches(current_backend, backend):
            continue
        if line.startswith("rail="):
            attrs = dict(re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([^ ]+)", line))
            if attrs.get("active") == "1" and attrs.get("data_ready") == "1":
                ready_rails += 1
                if attrs.get("link_speed"):
                    speeds.add(attrs["link_speed"])
        else:
            for name in PEER_COUNTERS:
                match = re.search(rf"{name}=(-?[0-9]+)", line)
                if match:
                    totals[name] += int(match.group(1))
    return ready_rails, sorted(speeds), totals


def collect_params(host: str) -> str:
    cmd = (
        "for f in /sys/module/thunderbolt_ibverbs/parameters/*; do "
        "[ -f \"$f\" ] || continue; "
        "printf '%s=' \"$(basename \"$f\")\"; cat \"$f\"; "
        "done 2>/dev/null || true"
    )
    return run_ssh(host, cmd, check=False, timeout=10).stdout


def parse_iommu_setting(cmdline: str) -> str:
    matches = re.findall(r"\b(?:intel_iommu|amd_iommu|iommu)=(\S+)", cmdline)
    if not matches:
        return "default"
    for preferred in ("off", "pt", "on"):
        if preferred in matches:
            return preferred
    return ",".join(matches)


def collect_host_identity(host: str) -> dict[str, str]:
    cmd = (
        "ko=$(modinfo -F filename thunderbolt_ibverbs 2>/dev/null); "
        "note=/sys/module/thunderbolt_ibverbs/notes/.note.gnu.build-id; "
        "echo kernel=$(uname -r); "
        "echo srcversion=$(cat /sys/module/thunderbolt_ibverbs/srcversion 2>/dev/null); "
        "echo taint=$(cat /sys/module/thunderbolt_ibverbs/taint 2>/dev/null); "
        "echo ko_path=$ko; "
        "echo module_sha256=$(sha256sum \"$ko\" 2>/dev/null | cut -c1-16); "
        "echo loaded_note_sha256=$(sha256sum \"$note\" 2>/dev/null | cut -c1-16); "
        "echo cmdline=$(tr -d '\\n' < /proc/cmdline 2>/dev/null)"
    )
    out = run_ssh(host, cmd, check=False, timeout=15).stdout
    fields: dict[str, str] = {}
    for line in out.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            fields[key.strip()] = value.strip()
    fields["iommu"] = parse_iommu_setting(fields.get("cmdline", ""))
    if fields.get("loaded_note_sha256"):
        fields["module_id"] = fields["loaded_note_sha256"]
        fields["module_id_source"] = "loaded-note-sha256"
    else:
        fields["module_id"] = fields.get("module_sha256", "")
        fields["module_id_source"] = "modinfo-ko-sha256"
    return fields


def format_identity(label: str, identity: dict[str, str]) -> str:
    return (
        f"tbv-perftest: {label} "
        f"kernel={identity.get('kernel', '?')} "
        f"module_id={identity.get('module_id') or 'unknown'} "
        f"module_id_source={identity.get('module_id_source') or '-'} "
        f"module_sha256={identity.get('module_sha256') or 'unknown'} "
        f"srcversion={identity.get('srcversion') or '-'} "
        f"taint={identity.get('taint') or '-'} "
        f"iommu={identity['iommu']}"
    )


def collect_telemetry(host: str, dev: str, backend: str | None, netdev: str | None, rdma_lib: str) -> dict[str, Any]:
    summary = run_ssh(
        host,
        "cat /sys/kernel/debug/thunderbolt_ibverbs/summary 2>/dev/null || true",
        check=False,
        timeout=10,
    ).stdout
    peers = run_ssh(
        host,
        "cat /sys/kernel/debug/thunderbolt_ibverbs/peers 2>/dev/null || true",
        check=False,
        timeout=10,
    ).stdout
    rdma_link = run_ssh(host, "rdma link show 2>&1 || true", check=False, timeout=10).stdout
    if rdma_lib:
        devinfo = run_ssh(
            host,
            f"LD_LIBRARY_PATH={shlex.quote(rdma_lib + '/lib')} "
            f"{shlex.quote(rdma_lib + '/bin/ibv_devinfo')} -d {shlex.quote(dev)} -v 2>&1 || true",
            check=False,
            timeout=15,
        ).stdout
    else:
        # No rdma-core path configured for this host (e.g. Darwin); ibv_devinfo
        # isn't available to invoke.
        devinfo = "(skipped: no rdma-core path)"
    params = collect_params(host)
    ip_stats = ""
    if netdev:
        ip_stats = run_ssh(host, f"ip -s link show dev {shlex.quote(netdev)} 2>&1 || true", check=False, timeout=10).stdout

    ready_rails, speeds, rail_totals = parse_peer_totals(peers, backend)
    return {
        "host": host,
        "timestamp_utc": now_utc(),
        "summary": summary,
        "summary_counters": parse_key_values(summary),
        "peers": peers,
        "ready_rails": ready_rails,
        "rail_speeds": speeds,
        "rail_totals": rail_totals,
        "rdma_link": rdma_link,
        "devinfo": devinfo,
        "params": params,
        "ip_stats": ip_stats,
    }


def delta_ints(after: dict[str, int], before: dict[str, int]) -> dict[str, int]:
    keys = set(before) | set(after)
    return {key: int(after.get(key, 0)) - int(before.get(key, 0)) for key in keys}


def fatal_counter_details(label: str, counters: dict[str, int]) -> list[str]:
    details = []
    for name in FATAL_COUNTERS:
        value = counters.get(name, 0)
        if value > 0:
            details.append(f"{label}.{name}+{value}")
    return details


def perftest_kind(case: dict[str, Any]) -> str:
    name = str(case["bin"])
    if name.endswith("_bw"):
        return "bw"
    if name.endswith("_lat"):
        return "lat"
    die(f"cannot classify perftest binary {name!r}")


def perftest_verb(case: dict[str, Any]) -> str:
    name = str(case["bin"])
    for verb in ("write", "read", "send", "atomic"):
        if f"_{verb}_" in name:
            return verb
    die(f"cannot classify perftest verb from binary {name!r}")


def parse_case_options(case: dict[str, Any]) -> dict[str, str]:
    out: dict[str, str] = {}
    argv = list(case.get("argv", []))
    flag_to_key = {
        "--size": "size_bytes",
        "--qp": "qps",
        "--iters": "iters",
        "--tx-depth": "tx_depth",
        "--rx-depth": "rx_depth",
        "--connection": "connection",
    }
    i = 0
    while i < len(argv):
        item = argv[i]
        if item in flag_to_key and i + 1 < len(argv):
            out[flag_to_key[item]] = str(argv[i + 1])
            i += 2
            continue
        if item == "--bidirectional":
            out["bidirectional"] = "1"
        i += 1
    out.setdefault("bidirectional", "0")
    return out


def strip_option(argv: list[str], flags: set[str]) -> list[str]:
    out: list[str] = []
    i = 0
    while i < len(argv):
        item = argv[i]
        if item in flags:
            if i + 1 < len(argv) and not argv[i + 1].startswith("-"):
                i += 2
            else:
                i += 1
            continue
        if any(item.startswith(flag + "=") for flag in flags):
            i += 1
            continue
        out.append(item)
        i += 1
    return out


def build_perftest_command(
    case: dict[str, Any],
    *,
    perftest_path: str,
    rdma_lib: str,
    pair_has_darwin: bool,
    dev: str,
    gid_index: int,
    port: int,
    timeout: int,
    remote_json: str,
    peer: str | None,
) -> str:
    argv = strip_option(
        [str(x) for x in case.get("argv", [])],
        {"--ib-dev", "-d", "--gid-index", "-x", "--port", "-p", "--out_json_file"},
    )
    argv.extend(["--ib-dev", dev, "--gid-index", str(gid_index), "--port", str(port), "-F"])
    if pair_has_darwin and "--use_old_post_send" not in argv:
        # Apple perftest is built with --disable-ibv_wr_api; force both sides
        # to use the legacy post-send flow so the negotiated ABI matches.
        argv.append("--use_old_post_send")
    if perftest_kind(case) == "bw":
        argv.append("--report_gbits")
    if perftest_kind(case) == "bw" and "--bidirectional" in argv and "--report-both" not in argv:
        argv.append("--report-both")
    argv.extend(["--out_json", f"--out_json_file={remote_json}"])
    if peer:
        argv.append(peer)

    bin_path = f"{perftest_path}/bin/{case['bin']}"
    quoted = " ".join(shlex.quote(str(arg)) for arg in argv)
    lib_prefix = f"LD_LIBRARY_PATH={shlex.quote(rdma_lib + '/lib')} " if rdma_lib else ""
    return (
        "set -euo pipefail; "
        f"mkdir -p {shlex.quote(str(Path(remote_json).parent))}; "
        f"cd {shlex.quote(str(Path(remote_json).parent))}; "
        f"{lib_prefix}"
        f"timeout {shlex.quote(str(timeout))} {shlex.quote(bin_path)} {quoted}"
    )


def parse_bw(stdout: str, bidirectional: bool) -> dict[str, Any]:
    rows = []
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) >= 5 and parts[0].isdigit() and parts[1].isdigit():
            try:
                rows.append({
                    "bytes": int(parts[0]),
                    "iters": int(parts[1]),
                    "peak_gbps": float(parts[2]),
                    "avg_gbps": float(parts[3]),
                    "msg_rate_mpps": float(parts[4]),
                })
            except ValueError:
                continue
    if not rows:
        raise ValueError("could not parse bandwidth summary")
    if bidirectional and len(rows) > 1:
        return {
            "bw_peak_gbps": f"{sum(row['peak_gbps'] for row in rows):.2f}",
            "bw_avg_gbps": f"{sum(row['avg_gbps'] for row in rows):.2f}",
            "msg_rate_mpps": f"{sum(row['msg_rate_mpps'] for row in rows):.6f}",
            "bw_rows": rows,
        }
    row = rows[-1]
    return {
        "bw_peak_gbps": f"{row['peak_gbps']:.2f}",
        "bw_avg_gbps": f"{row['avg_gbps']:.2f}",
        "msg_rate_mpps": f"{row['msg_rate_mpps']:.6f}",
        "bw_rows": rows,
    }


def parse_lat(stdout: str) -> dict[str, Any]:
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) >= 9 and parts[0].isdigit() and parts[1].isdigit():
            return {
                "lat_min_us": parts[2],
                "lat_max_us": parts[3],
                "lat_typical_us": parts[4],
                "lat_avg_us": parts[5],
                "lat_stdev_us": parts[6],
                "lat_p99_us": parts[7],
                "lat_p999_us": parts[8],
            }
    raise ValueError("could not parse latency summary")


def load_remote_json(host: str, path: str) -> Any:
    proc = run_ssh(host, f"cat {shlex.quote(path)} 2>/dev/null || true", check=False, timeout=10)
    text = proc.stdout.strip()
    if not text:
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return {"parse_error": text[:1000]}


def cleanup_remote_run(host: str, remote_dir: str) -> None:
    pattern = shlex.quote(remote_dir)
    run_ssh(
        host,
        "me=$$; "
        f"pgrep -f {pattern} | while read -r pid; do "
        "[ \"$pid\" = \"$me\" ] && continue; "
        "kill -TERM \"$pid\" 2>/dev/null || true; "
        "done; "
        "sleep 0.2; "
        f"pgrep -f {pattern} | while read -r pid; do "
        "[ \"$pid\" = \"$me\" ] && continue; "
        "kill -KILL \"$pid\" 2>/dev/null || true; "
        "done",
        check=False,
        timeout=5,
    )


def run_pair(
    case: dict[str, Any],
    *,
    run_id: str,
    server: str,
    client: str,
    server_perftest: str,
    client_perftest: str,
    server_rdma_lib: str,
    client_rdma_lib: str,
    server_dev: str,
    client_dev: str,
    peer_addr: str,
    gid_index: int,
    port: int,
    timeout: int,
    server_start_delay: float,
) -> tuple[str, dict[str, Any], str, dict[str, Any]]:
    remote_dir = f"/tmp/tbv-perftest/{run_id}"
    server_json = f"{remote_dir}/server.json"
    client_json = f"{remote_dir}/client.json"
    pair_has_darwin = "Darwin" in (HOST_SYSTEM.get(server), HOST_SYSTEM.get(client))
    server_cmd = build_perftest_command(
        case,
        perftest_path=server_perftest,
        rdma_lib=server_rdma_lib,
        pair_has_darwin=pair_has_darwin,
        dev=server_dev,
        gid_index=gid_index,
        port=port,
        timeout=timeout,
        remote_json=server_json,
        peer=None,
    )
    client_cmd = build_perftest_command(
        case,
        perftest_path=client_perftest,
        rdma_lib=client_rdma_lib,
        pair_has_darwin=pair_has_darwin,
        dev=client_dev,
        gid_index=gid_index,
        port=port,
        timeout=timeout,
        remote_json=client_json,
        peer=peer_addr,
    )

    server_proc = subprocess.Popen(
        ssh_args(server, server_cmd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(server_start_delay)
    client_proc = run_ssh(client, client_cmd, check=False, timeout=timeout + 20)
    server_reap_error = ""
    try:
        server_stdout, _ = server_proc.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        server_proc.terminate()
        try:
            server_stdout, _ = server_proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            server_proc.kill()
            server_stdout, _ = server_proc.communicate()
            cleanup_remote_run(server, remote_dir)
            server_reap_error = "server ssh cleanup timeout"

    raw = {
        "server_stdout": server_stdout,
        "client_stdout": client_proc.stdout,
        "server_returncode": server_proc.returncode,
        "client_returncode": client_proc.returncode,
        "server_json": load_remote_json(server, server_json),
        "client_json": load_remote_json(client, client_json),
    }
    if server_reap_error:
        return "fail", {}, f"{server_reap_error}: {server_stdout.strip()[-800:]}", raw
    if client_proc.returncode != 0:
        return "fail", {}, f"client rc={client_proc.returncode}: {client_proc.stdout.strip()[-800:]}", raw
    if server_proc.returncode != 0:
        return "fail", {}, f"server rc={server_proc.returncode}: {server_stdout.strip()[-800:]}", raw

    try:
        if perftest_kind(case) == "bw":
            metrics = parse_bw(client_proc.stdout, "--bidirectional" in case.get("argv", []))
        else:
            metrics = parse_lat(client_proc.stdout)
        return "ok", metrics, "", raw
    except ValueError as exc:
        combined = f"-- client --\n{client_proc.stdout}\n-- server --\n{server_stdout}"
        return "fail", {}, f"{exc}: {combined[-1200:]}", raw


def assert_topology(label: str, telemetry: dict[str, Any], expect_rails: int | None, expect_speed: str | None) -> None:
    if expect_rails is not None and telemetry["ready_rails"] != expect_rails:
        die(f"{label}: expected {expect_rails} ready rails, saw {telemetry['ready_rails']}")
    if expect_speed and expect_speed != "any":
        expected = normalize_rail_speed(expect_speed)
        speeds = telemetry["rail_speeds"]
        if not speeds or any(normalize_rail_speed(speed) != expected for speed in speeds):
            die(f"{label}: expected rail speed {expect_speed}, saw {','.join(speeds) or 'none'}")


def open_outputs(csv_path: Path, jsonl_path: Path):
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    jsonl_path.parent.mkdir(parents=True, exist_ok=True)
    csv_new = not csv_path.exists() or csv_path.stat().st_size == 0
    csv_handle = csv_path.open("a", newline="")
    jsonl_handle = jsonl_path.open("a")
    writer = csv.DictWriter(csv_handle, fieldnames=CSV_FIELDS, extrasaction="ignore")
    if csv_new:
        writer.writeheader()
    return csv_handle, jsonl_handle, writer


def case_selected(case: dict[str, Any], patterns: list[str]) -> bool:
    if not patterns:
        return True
    name = case["name"]
    return any(fnmatch.fnmatch(name, pattern) for pattern in patterns)


def direction_pairs(plan_direction: str, requested: str, server: str, client: str) -> list[tuple[str, str, str]]:
    direction = plan_direction if requested == "from-plan" else requested
    if direction == "forward":
        return [("forward", server, client)]
    if direction == "reverse":
        return [("reverse", client, server)]
    if direction == "both":
        return [("forward", server, client), ("reverse", client, server)]
    die(f"unknown direction '{direction}'")


def selected_runs(
    cases: list[dict[str, Any]],
    *,
    repeat_count: int,
    directions: str,
    server: str,
    client: str,
):
    for repeat_index in range(1, repeat_count + 1):
        for case in cases:
            case_directions = case.get("directions") or ["forward"]
            requested_directions = case_directions if directions == "from-plan" else [directions]
            for plan_dir in requested_directions:
                yield from (
                    (repeat_index, case, direction_name, run_server, run_client)
                    for direction_name, run_server, run_client
                    in direction_pairs(plan_dir, "from-plan", server, client)
                )


def parse_hosts(value: str) -> tuple[str, str]:
    parts = [part.strip() for part in re.split(r"[, ]+", value) if part.strip()]
    if len(parts) != 2:
        die("--hosts expects exactly two hosts, e.g. --hosts strix-1,strix-2")
    return parts[0], parts[1]


def parse_host_map(value: str) -> dict[str, str]:
    out: dict[str, str] = {}
    if not value:
        return out
    for part in re.split(r"[, ]+", value):
        if not part:
            continue
        host, sep, addr = part.partition("=")
        if not sep or not host or not addr:
            die("--data-addrs entries must look like host=addr")
        out[host] = addr
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a Nix-generated perftest benchmark plan.")
    parser.add_argument("--plan", required=True)
    parser.add_argument("--server")
    parser.add_argument("--client")
    parser.add_argument("--hosts", help="Shortcut for --server A --client B")
    parser.add_argument("--server-data-addr", help="address clients should use to connect to the server host")
    parser.add_argument("--client-data-addr", help="address clients should use to connect to the client host when directions reverse")
    parser.add_argument("--data-addrs", help="comma-separated host=addr map for perftest's data connection target")
    parser.add_argument("--ssh-config", help="ssh_config file passed to ssh with -F")
    parser.add_argument("--ssh-option", action="append", default=[], help="extra ssh -o option; may be repeated")
    parser.add_argument("--dev")
    parser.add_argument("--server-dev", help="override --dev on the server side (asymmetric pairs)")
    parser.add_argument("--client-dev", help="override --dev on the client side (asymmetric pairs)")
    parser.add_argument("--gid-index", type=int)
    parser.add_argument("--backend")
    parser.add_argument("--netdev")
    parser.add_argument("--expect-rails", type=int)
    parser.add_argument("--no-rail-check", action="store_true", help="skip the ready-rails / rail-speed topology check (e.g. RXE runs)")
    parser.add_argument("--expect-speed")
    parser.add_argument("--timeout", type=int)
    parser.add_argument("--base-port", type=int)
    parser.add_argument("--csv")
    parser.add_argument("--jsonl")
    parser.add_argument("--tag")
    parser.add_argument("--directions", choices=["from-plan", "forward", "reverse", "both"])
    parser.add_argument("--only", action="append", default=[], help="fnmatch pattern for case names; may repeat")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--no-copy-tools", action="store_true")
    parser.add_argument("--stop-on-fail", action="store_true")
    parser.add_argument(
        "--repeat",
        type=int,
        default=1,
        help="Repeat the selected benchmark case/direction sequence N times",
    )
    args = parser.parse_args()
    if args.repeat < 1:
        die("--repeat must be >= 1")
    global SSH_CONFIG, SSH_OPTIONS
    SSH_CONFIG = args.ssh_config or os.environ.get("TBV_SSH_CONFIG", "")
    SSH_OPTIONS = list(args.ssh_option or [])

    plan = json.loads(Path(args.plan).read_text())
    defaults = dict(plan.get("defaults", {}))
    if args.hosts:
        args.server, args.client = parse_hosts(args.hosts)

    server = args.server or defaults["server"]
    client = args.client or defaults["client"]
    dev = args.dev or defaults["dev"]
    # Pin device per host (not per role); directions swap server/client but
    # the host -> dev mapping stays fixed.
    host_dev = {
        server: args.server_dev or dev,
        client: args.client_dev or dev,
    }
    host_data_addr = {
        server: args.server_data_addr or server,
        client: args.client_data_addr or client,
    }
    host_data_addr.update(parse_host_map(args.data_addrs or ""))
    gid_index = args.gid_index if args.gid_index is not None else int(defaults["gidIndex"])
    backend = args.backend if args.backend is not None else defaults.get("backend")
    netdev = args.netdev or defaults.get("netdev")
    if args.no_rail_check:
        expect_rails = None
        expect_speed = None
    else:
        expect_rails = args.expect_rails if args.expect_rails is not None else defaults.get("expectRails")
        expect_speed = args.expect_speed if args.expect_speed is not None else defaults.get("expectSpeed")
    timeout = args.timeout or int(defaults["timeout"])
    base_port = args.base_port or int(defaults["basePort"])
    directions = args.directions or defaults.get("directions", "from-plan")
    copy = bool(defaults.get("copyTools", True)) and not args.no_copy_tools
    stop_on_fail = args.stop_on_fail or bool(defaults.get("stopOnFail", False))
    tag = args.tag or plan["name"]

    result_dir = Path(defaults.get("resultDir", "thunderbolt-ibverbs/results"))
    stamp = dt.datetime.now().strftime("%Y-%m-%d-%H%M%S")
    csv_path = Path(args.csv) if args.csv else result_dir / f"{stamp}-{tag}.csv"
    jsonl_path = Path(args.jsonl) if args.jsonl else result_dir / f"{stamp}-{tag}.jsonl"

    cases = [case for case in plan["cases"] if case_selected(case, args.only)]
    if args.list:
        for idx, case in enumerate(cases, start=1):
            print(f"{idx:03d} {case['name']} {case['bin']} {' '.join(case.get('argv', []))}")
        return 0

    if not args.dry_run:
        for host in sorted({server, client}):
            HOST_SYSTEM[host] = detect_system(host)
            print(f"tbv-perftest: {host} system={HOST_SYSTEM[host]}", flush=True)
        if "Darwin" in HOST_SYSTEM.values() and not PERFTEST_DARWIN:
            die("a host reports Darwin but TBV_PERFTEST_DARWIN is unset; the wrapper was built without a darwin perftest reference")

    if copy and not args.dry_run:
        for host in sorted({server, client}):
            copy_tools(host)

    if args.dry_run:
        runs = selected_runs(
            cases,
            repeat_count=args.repeat,
            directions=directions,
            server=server,
            client=client,
        )
        for idx, (repeat_index, case, direction_name, _, _) in enumerate(runs, start=1):
            print(
                f"{idx:03d} r={repeat_index}/{args.repeat} {direction_name} "
                f"{case['name']} {case['bin']} {' '.join(case.get('argv', []))}"
            )
        return 0

    csv_handle, jsonl_handle, writer = open_outputs(csv_path, jsonl_path)
    failures = 0
    run_index = 0
    try:
        identity_server = collect_host_identity(server)
        identity_client = collect_host_identity(client)
        print(format_identity(server, identity_server), flush=True)
        print(format_identity(client, identity_client), flush=True)
        for field in ("kernel", "module_id", "iommu"):
            if identity_server.get(field) != identity_client.get(field):
                print(
                    f"tbv-perftest: WARNING: server/client {field} mismatch",
                    file=sys.stderr,
                    flush=True,
                )

        initial_server = collect_telemetry(server, host_dev[server], backend, netdev, rdma_core_for(server))
        initial_client = collect_telemetry(client, host_dev[client], backend, netdev, rdma_core_for(client))
        assert_topology(server, initial_server, expect_rails, expect_speed)
        assert_topology(client, initial_client, expect_rails, expect_speed)
        print(
            "tbv-perftest: state ok: "
            f"{server} rails={initial_server['ready_rails']} speeds={','.join(initial_server['rail_speeds']) or 'n/a'}; "
            f"{client} rails={initial_client['ready_rails']} speeds={','.join(initial_client['rail_speeds']) or 'n/a'}",
            flush=True,
        )

        for repeat_index in range(1, args.repeat + 1):
            if args.repeat > 1:
                print(f"tbv-perftest: repeat {repeat_index}/{args.repeat}", flush=True)
            for case in cases:
                case_directions = case.get("directions") or ["forward"]
                if directions == "from-plan":
                    requested_directions = case_directions
                else:
                    requested_directions = [directions]
                for plan_dir in requested_directions:
                    for direction_name, run_server, run_client in direction_pairs(plan_dir, "from-plan", server, client):
                        run_index += 1
                        port = base_port + run_index
                        run_id = (
                            f"{tag}-r{repeat_index:02d}-"
                            f"{run_index:04d}-{case['name'].replace('.', '-')}-{direction_name}"
                        )
                        before_server = collect_telemetry(run_server, host_dev[run_server], backend, netdev, rdma_core_for(run_server))
                        before_client = collect_telemetry(run_client, host_dev[run_client], backend, netdev, rdma_core_for(run_client))
                        assert_topology(run_server, before_server, expect_rails, expect_speed)
                        assert_topology(run_client, before_client, expect_rails, expect_speed)

                        opts = parse_case_options(case)
                        row: dict[str, Any] = {
                            "timestamp_utc": now_utc(),
                            "plan": plan["name"],
                            "repeat_index": str(repeat_index),
                            "repeat_count": str(args.repeat),
                            "case": case["name"],
                            "tool": case["bin"],
                            "verb": perftest_verb(case),
                            "kind": perftest_kind(case),
                            "server": run_server,
                            "client": run_client,
                            "kernel": identity_server.get("kernel", ""),
                            "module_id": identity_server.get("module_id", ""),
                            "module_id_source": identity_server.get("module_id_source", ""),
                            "module_sha256": identity_server.get("module_sha256", ""),
                            "module_ko_path": identity_server.get("ko_path", ""),
                            "iommu": identity_server.get("iommu", ""),
                            "direction": direction_name,
                            "dev": (
                                host_dev[run_server]
                                if host_dev[run_server] == host_dev[run_client]
                                else f"{host_dev[run_server]}->{host_dev[run_client]}"
                            ),
                            "server_dev": host_dev[run_server],
                            "client_dev": host_dev[run_client],
                            "server_data_addr": host_data_addr.get(run_server, run_server),
                            "client_data_addr": host_data_addr.get(run_client, run_client),
                            "gid_index": str(gid_index),
                            "port": str(port),
                            "status": "",
                            "duration_s": "",
                            "error": "",
                            **opts,
                        }

                        print(f"tbv-perftest: {direction_name} {case['name']}", flush=True)
                        start = time.monotonic()
                        status, metrics, error, raw = run_pair(
                            case,
                            run_id=run_id,
                            server=run_server,
                            client=run_client,
                            server_perftest=perftest_for(run_server),
                            client_perftest=perftest_for(run_client),
                            server_rdma_lib=rdma_core_for(run_server),
                            client_rdma_lib=rdma_core_for(run_client),
                            server_dev=host_dev[run_server],
                            client_dev=host_dev[run_client],
                            peer_addr=host_data_addr.get(run_server, run_server),
                            gid_index=gid_index,
                            port=port,
                            timeout=timeout,
                            server_start_delay=float(defaults["serverStartDelay"]),
                        )
                        elapsed = time.monotonic() - start
                        after_server = collect_telemetry(run_server, host_dev[run_server], backend, netdev, rdma_core_for(run_server))
                        after_client = collect_telemetry(run_client, host_dev[run_client], backend, netdev, rdma_core_for(run_client))
                        server_delta = delta_ints(after_server["summary_counters"], before_server["summary_counters"])
                        client_delta = delta_ints(after_client["summary_counters"], before_client["summary_counters"])
                        server_rail_delta = delta_ints(after_server["rail_totals"], before_server["rail_totals"])
                        client_rail_delta = delta_ints(after_client["rail_totals"], before_client["rail_totals"])

                        fatal_details = (
                            fatal_counter_details("server", server_delta)
                            + fatal_counter_details("client", client_delta)
                        )
                        if status == "ok" and fatal_details:
                            status = "fail"
                            error = "fatal counter delta: " + ", ".join(fatal_details)
                        row.update(metrics)
                        row.update({
                            "status": status,
                            "duration_s": f"{elapsed:.3f}",
                            "error": error.replace("\n", " "),
                            "server_ready_rails": str(after_server["ready_rails"]),
                            "client_ready_rails": str(after_client["ready_rails"]),
                            "server_rail_speeds": "|".join(after_server["rail_speeds"]),
                            "client_rail_speeds": "|".join(after_client["rail_speeds"]),
                            "server_error_delta": str(sum(server_delta.get(name, 0) for name in FATAL_COUNTERS)),
                            "client_error_delta": str(sum(client_delta.get(name, 0) for name in FATAL_COUNTERS)),
                            "server_zcopy_delta": str(server_delta.get("data_wr_zcopy", 0)),
                            "server_copy_delta": str(server_delta.get("data_wr_copied", 0)),
                            "client_zcopy_delta": str(client_delta.get("data_wr_zcopy", 0)),
                            "client_copy_delta": str(client_delta.get("data_wr_copied", 0)),
                            "server_path_send_error_delta": str(server_delta.get("data_wr_path_send_error", 0)),
                            "client_path_send_error_delta": str(client_delta.get("data_wr_path_send_error", 0)),
                        })
                        for prefix, counters, rail_delta in (
                            ("server", server_delta, server_rail_delta),
                            ("client", client_delta, client_rail_delta),
                        ):
                            for counter in IMPORTANT_COUNTERS:
                                row[f"{prefix}_delta_{counter}"] = str(counters.get(counter, 0))
                            for counter in PEER_COUNTERS:
                                row[f"{prefix}_delta_rail_{counter}"] = str(rail_delta.get(counter, 0))

                        writer.writerow(row)
                        csv_handle.flush()
                        json_record = {
                            "row": row,
                            "case": case,
                            "raw": raw,
                            "host_identity": {
                                "server": identity_server,
                                "client": identity_client,
                            },
                            "telemetry": {
                                "before_server": before_server,
                                "before_client": before_client,
                                "after_server": after_server,
                                "after_client": after_client,
                                "server_delta": server_delta,
                                "client_delta": client_delta,
                                "server_rail_delta": server_rail_delta,
                                "client_rail_delta": client_rail_delta,
                            },
                        }
                        jsonl_handle.write(json.dumps(json_record, sort_keys=True) + "\n")
                        jsonl_handle.flush()

                        if status != "ok":
                            failures += 1
                            if stop_on_fail:
                                print(f"tbv-perftest: stopping after failure: {error}", file=sys.stderr)
                                return 1
    finally:
        csv_handle.close()
        jsonl_handle.close()

    print(f"tbv-perftest: wrote {csv_path}", flush=True)
    print(f"tbv-perftest: wrote {jsonl_path}", flush=True)
    if failures:
        print(f"tbv-perftest: {failures} row(s) failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
