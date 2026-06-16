#!/usr/bin/env python3
"""Run paired uc_oneway stress cases over SSH.

This runner exists for the Apple-compat SEND fragmentation/window tests that
perftest does not cover. It keeps sender depth independent from receiver
receive-post depth, preserves full logs per run, and writes one CSV row per
case.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path


CSV_FIELDS = [
    "timestamp_utc",
    "tag",
    "direction",
    "receiver",
    "sender",
    "receiver_dev",
    "sender_dev",
    "receiver_gid_index",
    "sender_gid_index",
    "size_bytes",
    "count",
    "send_depth",
    "send_slots",
    "recv_depth",
    "recv_posts",
    "mtu",
    "repeat_index",
    "repeat_count",
    "port",
    "connect_host",
    "hold_before_destroy_ms",
    "status",
    "duration_s",
    "sender_rc",
    "receiver_rc",
    "sender_done",
    "receiver_done",
    "sender_mbps",
    "receiver_mbps",
    "sender_elapsed_s",
    "receiver_elapsed_s",
    "receiver_wc_status",
    "receiver_wc_opcode",
    "receiver_wc_byte_len",
    "sender_log",
    "receiver_log",
    "error",
]

PROGRESS_RE = re.compile(
    r"\b(?P<role>send|recv|bidi-recv) progress done="
    r"(?P<done>\d+)/(?P<total>\d+).*?\brate="
    r"(?P<rate>[0-9.]+) Mbit/s elapsed=(?P<elapsed>[0-9.]+) s"
)
WC_ERROR_RE = re.compile(
    r"recv wc error wr_id=\d+ status=(?P<status>\d+) "
    r"opcode=(?P<opcode>\d+) byte_len=(?P<byte_len>\d+)"
)
PORT_ACTIVE_RE = re.compile(r"\bstate:\s+PORT_ACTIVE\b")
APPLE_RDMA_DEV_PREFIX = "rdma_"
APPLE_RDMA_HOLD_BEFORE_DESTROY_MS = 1000
APPLE_UC_QUEUE_DEPTH = 32
SSH_CONFIG = ""
SSH_OPTIONS: list[str] = []


def die(msg: str) -> None:
    print(f"tbv-uc-stress: {msg}", file=sys.stderr)
    raise SystemExit(1)


def parse_csv_ints(value: str) -> list[int]:
    out: list[int] = []
    for item in value.split(","):
        item = item.strip()
        if item:
            out.append(int(item, 0))
    if not out:
        raise argparse.ArgumentTypeError("empty list")
    return out


def ssh_command(host: str, command: str) -> list[str]:
    if host in ("", "local", "localhost"):
        return ["bash", "-lc", command]
    args = [
        "ssh",
        "-o",
        "ConnectTimeout=8",
        "-o",
        "BatchMode=yes",
        "-n",
    ]
    if SSH_CONFIG:
        args.extend(["-F", SSH_CONFIG])
    for option in SSH_OPTIONS:
        args.extend(["-o", option])
    args.extend([host, command])
    return args


def shell_join(args: list[str]) -> str:
    return " ".join(shlex.quote(arg) for arg in args)


def safe_name(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_.-]+", "-", value.strip())
    value = value.strip("-")
    return value or "run"


def apple_rdma_pair(receiver_dev: str, sender_dev: str) -> bool:
    return receiver_dev.startswith(APPLE_RDMA_DEV_PREFIX) or sender_dev.startswith(
        APPLE_RDMA_DEV_PREFIX
    )


def apple_queue_depth(
    value: int, receiver_dev: str, sender_dev: str, apple_limit: int
) -> int:
    if apple_limit and apple_rdma_pair(receiver_dev, sender_dev):
        return min(value, apple_limit)
    return value


def hold_before_destroy_ms(
    args: argparse.Namespace, receiver_dev: str, sender_dev: str
) -> int:
    if args.hold_before_destroy_ms >= 0:
        return args.hold_before_destroy_ms
    if apple_rdma_pair(receiver_dev, sender_dev):
        return APPLE_RDMA_HOLD_BEFORE_DESTROY_MS
    return 0


def host_tool(args: argparse.Namespace, host: str, role: str) -> str:
    if host == args.server and args.server_tool:
        return args.server_tool
    if host == args.client and args.client_tool:
        return args.client_tool
    if role == "recv" and args.receiver_tool:
        return args.receiver_tool
    if role == "send" and args.sender_tool:
        return args.sender_tool
    return args.tool


def uc_command(
    tool: str,
    *,
    role: str,
    dev: str,
    gid_index: int,
    port: int,
    size: int,
    count: int,
    depth: int,
    mtu: int,
    check: bool,
    check_any_order: bool,
    send_slots: int | None = None,
    recv_posts: int | None = None,
    connect: str | None = None,
    hold_before_destroy_ms: int = 0,
) -> str:
    parts = [
        tool,
        "--role",
        role,
        "--dev",
        dev,
        "--gid-index",
        str(gid_index),
        "--port",
        str(port),
        "--size",
        str(size),
        "--count",
        str(count),
        "--depth",
        str(depth),
        "--mtu",
        str(mtu),
    ]
    if recv_posts is not None:
        parts.extend(["--recv-posts", str(recv_posts)])
    if send_slots is not None:
        parts.extend(["--send-slots", str(send_slots)])
    if connect is not None:
        parts.extend(["--connect", connect])
    if hold_before_destroy_ms:
        parts.extend(["--hold-before-destroy-ms", str(hold_before_destroy_ms)])
    if check_any_order:
        parts.append("--check-any-order")
    elif check:
        parts.append("--check")
    return shell_join(parts)


def run_with_timeout(cmd: list[str], timeout: float) -> tuple[int, str]:
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        stdout, _ = proc.communicate(timeout=timeout)
        return proc.returncode, stdout
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, _ = proc.communicate()
        return 124, stdout
    except BaseException:
        proc.kill()
        proc.communicate()
        raise


def run_host_capture(
    host: str, command: str, *, timeout: float = 15.0
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ssh_command(host, command),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
        timeout=timeout,
    )


def connect_host_for_receiver(args: argparse.Namespace, receiver: str) -> str:
    if args.connect_host:
        return args.connect_host
    if receiver == args.server:
        return args.server_connect_host or receiver
    if receiver == args.client:
        return args.client_connect_host or receiver
    return receiver


def preflight_device(host: str, dev: str) -> list[str]:
    command = "\n".join(
        [
            "if ! command -v ibv_devices >/dev/null 2>&1; then",
            "  echo 'missing ibv_devices in PATH'",
            "  exit 127",
            "fi",
            "if ! command -v ibv_devinfo >/dev/null 2>&1; then",
            "  echo 'missing ibv_devinfo in PATH'",
            "  exit 127",
            "fi",
            "echo __TBV_IBV_DEVICES__",
            "ibv_devices",
            "echo __TBV_IBV_DEVINFO__",
            f"ibv_devinfo -d {shlex.quote(dev)}",
        ]
    )
    errors: list[str] = []
    try:
        proc = run_host_capture(host, command)
    except subprocess.TimeoutExpired:
        return [f"{host}:{dev}: preflight timed out"]

    output = proc.stdout or ""
    if proc.returncode:
        tail = output.strip()[-500:]
        return [f"{host}:{dev}: preflight command failed rc={proc.returncode}: {tail}"]
    if not re.search(rf"(^|\n)\s*{re.escape(dev)}\s", output):
        errors.append(f"{host}:{dev}: device not listed by ibv_devices")
    if not PORT_ACTIVE_RE.search(output):
        errors.append(f"{host}:{dev}: ibv_devinfo did not report PORT_ACTIVE")
    return errors


def run_preflight(args: argparse.Namespace, directions: list[tuple]) -> None:
    print("tbv-uc-stress: preflight active", flush=True)
    errors: list[str] = []
    checked: set[tuple[str, str]] = set()

    for (
        direction,
        receiver,
        sender,
        receiver_dev,
        sender_dev,
        _receiver_gid,
        _sender_gid,
    ) in directions:
        connect_host = connect_host_for_receiver(args, receiver)
        print(
            "tbv-uc-stress: preflight "
            f"{direction}: receiver={receiver}:{receiver_dev} "
            f"sender={sender}:{sender_dev} connect_host={connect_host}",
            flush=True,
        )
        for host, dev in ((receiver, receiver_dev), (sender, sender_dev)):
            key = (host, dev)
            if key in checked:
                continue
            checked.add(key)
            errors.extend(preflight_device(host, dev))

    if errors:
        die("preflight failed:\n  - " + "\n  - ".join(errors))

    print("tbv-uc-stress: preflight OK", flush=True)


def cleanup_port(host: str, tool_basename: str, port: int) -> None:
    pattern = (
        rf"(^|/){re.escape(tool_basename)}([[:space:]]|$)"
        rf".*--port[[:space:]]+{port}([[:space:]]|$)"
    )
    subprocess.run(
        ssh_command(host, f"pkill -f {shlex.quote(pattern)} 2>/dev/null || true"),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
        timeout=10,
    )


def last_progress(stdout: str, role: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for match in PROGRESS_RE.finditer(stdout):
        if match.group("role") != role:
            continue
        out = {
            "done": match.group("done"),
            "mbps": match.group("rate"),
            "elapsed_s": match.group("elapsed"),
        }
    return out


def wc_error(stdout: str) -> dict[str, str]:
    match = WC_ERROR_RE.search(stdout)
    if not match:
        return {}
    return match.groupdict()


def open_csv(path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    new_file = not path.exists() or path.stat().st_size == 0
    handle = path.open("a", newline="")
    writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
    if new_file:
        writer.writeheader()
    return handle, writer


def run_case(
    args: argparse.Namespace,
    *,
    direction: str,
    receiver: str,
    sender: str,
    receiver_dev: str,
    sender_dev: str,
    receiver_gid_index: int,
    sender_gid_index: int,
    size: int,
    send_depth: int,
    mtu: int,
    repeat_index: int,
    port: int,
    log_dir: Path,
) -> dict[str, str]:
    sender_tool = host_tool(args, sender, "send")
    receiver_tool = host_tool(args, receiver, "recv")
    connect_host = connect_host_for_receiver(args, receiver)
    hold_ms = hold_before_destroy_ms(args, receiver_dev, sender_dev)
    recv_depth = apple_queue_depth(
        args.recv_depth, receiver_dev, sender_dev, args.apple_queue_depth
    )
    recv_posts = apple_queue_depth(
        args.recv_posts, receiver_dev, sender_dev, args.apple_queue_depth
    )
    send_depth = apple_queue_depth(
        send_depth, receiver_dev, sender_dev, args.apple_queue_depth
    )
    send_slots = args.send_slots or send_depth
    send_slots = apple_queue_depth(
        send_slots, receiver_dev, sender_dev, args.apple_queue_depth
    )
    if args.check and send_slots < send_depth:
        send_slots = send_depth
    log_prefix = f"{safe_name(args.tag)}-" if args.tag else ""
    receiver_log = log_dir / (
        f"{log_prefix}{direction}-port{port}-size{size}-sd{send_depth}-mtu{mtu}-"
        f"rep{repeat_index}-recv.log"
    )
    sender_log = log_dir / (
        f"{log_prefix}{direction}-port{port}-size{size}-sd{send_depth}-mtu{mtu}-"
        f"rep{repeat_index}-send.log"
    )

    recv_cmd = uc_command(
        receiver_tool,
        role="recv",
        dev=receiver_dev,
        gid_index=receiver_gid_index,
        port=port,
        size=size,
        count=args.count,
        depth=recv_depth,
        recv_posts=recv_posts,
        mtu=mtu,
        check=args.check,
        check_any_order=args.check_any_order,
        hold_before_destroy_ms=hold_ms,
    )
    send_cmd = uc_command(
        sender_tool,
        role="send",
        dev=sender_dev,
        gid_index=sender_gid_index,
        port=port,
        size=size,
        count=args.count,
        depth=send_depth,
        send_slots=send_slots,
        mtu=mtu,
        check=args.check,
        check_any_order=False,
        connect=connect_host,
        hold_before_destroy_ms=hold_ms,
    )

    start = time.monotonic()
    recv_proc = subprocess.Popen(
        ssh_command(receiver, recv_cmd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        time.sleep(args.start_delay)

        sender_rc, sender_stdout = run_with_timeout(
            ssh_command(sender, send_cmd),
            args.timeout,
        )
        try:
            receiver_stdout, _ = recv_proc.communicate(
                timeout=args.receiver_drain_timeout
            )
            receiver_rc = recv_proc.returncode
        except subprocess.TimeoutExpired:
            recv_proc.kill()
            receiver_stdout, _ = recv_proc.communicate()
            receiver_rc = 124

        if sender_rc == 124 or receiver_rc == 124:
            cleanup_port(sender, os.path.basename(sender_tool), port)
            cleanup_port(receiver, os.path.basename(receiver_tool), port)
    except BaseException:
        if recv_proc.poll() is None:
            recv_proc.kill()
            recv_proc.communicate()
        cleanup_port(sender, os.path.basename(sender_tool), port)
        cleanup_port(receiver, os.path.basename(receiver_tool), port)
        raise

    sender_log.write_text(sender_stdout)
    receiver_log.write_text(receiver_stdout)

    send_progress = last_progress(sender_stdout, "send")
    recv_progress = last_progress(receiver_stdout, "recv")
    recv_wc = wc_error(receiver_stdout)
    ok = sender_rc == 0 and receiver_rc == 0
    error = ""
    if not ok:
        error = (
            f"sender rc={sender_rc} tail={sender_stdout.strip()[-300:]} "
            f"receiver rc={receiver_rc} tail={receiver_stdout.strip()[-300:]}"
        )

    return {
        "timestamp_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
        "tag": args.tag,
        "direction": direction,
        "receiver": receiver,
        "sender": sender,
        "receiver_dev": receiver_dev,
        "sender_dev": sender_dev,
        "receiver_gid_index": str(receiver_gid_index),
        "sender_gid_index": str(sender_gid_index),
        "size_bytes": str(size),
        "count": str(args.count),
        "send_depth": str(send_depth),
        "send_slots": str(send_slots),
        "recv_depth": str(recv_depth),
        "recv_posts": str(recv_posts),
        "mtu": str(mtu),
        "repeat_index": str(repeat_index),
        "repeat_count": str(args.repeats),
        "port": str(port),
        "connect_host": connect_host,
        "hold_before_destroy_ms": str(hold_ms),
        "status": "ok" if ok else "fail",
        "duration_s": f"{time.monotonic() - start:.3f}",
        "sender_rc": str(sender_rc),
        "receiver_rc": str(receiver_rc),
        "sender_done": send_progress.get("done", ""),
        "receiver_done": recv_progress.get("done", ""),
        "sender_mbps": send_progress.get("mbps", ""),
        "receiver_mbps": recv_progress.get("mbps", ""),
        "sender_elapsed_s": send_progress.get("elapsed_s", ""),
        "receiver_elapsed_s": recv_progress.get("elapsed_s", ""),
        "receiver_wc_status": recv_wc.get("status", ""),
        "receiver_wc_opcode": recv_wc.get("opcode", ""),
        "receiver_wc_byte_len": recv_wc.get("byte_len", ""),
        "sender_log": str(sender_log),
        "receiver_log": str(receiver_log),
        "error": error.replace("\n", " "),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run uc_oneway stress cases and write CSV results."
    )
    parser.add_argument("--server", required=True, help="Forward-direction receiver host.")
    parser.add_argument("--client", required=True, help="Forward-direction sender host.")
    parser.add_argument("--server-dev", required=True)
    parser.add_argument("--client-dev", required=True)
    parser.add_argument("--server-gid-index", type=int, default=1)
    parser.add_argument("--client-gid-index", type=int, default=1)
    parser.add_argument("--tool", default="uc_oneway")
    parser.add_argument("--server-tool", default="")
    parser.add_argument("--client-tool", default="")
    parser.add_argument("--receiver-tool", default="")
    parser.add_argument("--sender-tool", default="")
    parser.add_argument(
        "--connect-host",
        default="",
        help="Name/IP sender passes to --connect; defaults to receiver host.",
    )
    parser.add_argument(
        "--server-connect-host",
        default="",
        help="Name/IP senders use when the server side is receiving.",
    )
    parser.add_argument(
        "--client-connect-host",
        default="",
        help="Name/IP senders use when the client side is receiving.",
    )
    parser.add_argument(
        "--csv",
        default=f"tbv-uc-stress-{dt.datetime.now().strftime('%Y%m%d-%H%M%S')}.csv",
    )
    parser.add_argument("--log-dir", default="")
    parser.add_argument("--tag", default="")
    parser.add_argument("--directions", choices=["forward", "reverse", "both"], default="forward")
    parser.add_argument("--sizes", type=parse_csv_ints, default=parse_csv_ints("32768"))
    parser.add_argument("--send-depths", type=parse_csv_ints, default=parse_csv_ints("16,32"))
    parser.add_argument("--send-slots", type=int, default=0)
    parser.add_argument("--recv-depth", type=int, default=APPLE_UC_QUEUE_DEPTH)
    parser.add_argument("--recv-posts", type=int, default=APPLE_UC_QUEUE_DEPTH)
    parser.add_argument("--mtus", type=parse_csv_ints, default=parse_csv_ints("4096"))
    parser.add_argument("--count", type=int, default=100000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument(
        "--apple-queue-depth",
        type=int,
        default=APPLE_UC_QUEUE_DEPTH,
        help=(
            "Maximum requested depth for cases involving macOS rdma_* devices; "
            "0 disables this software cap."
        ),
    )
    parser.add_argument(
        "--apple-gate",
        action="store_true",
        help=(
            "Run the Apple compatibility merge-gate matrix: both directions, "
            "4 KiB and 64 KiB checked SENDs, depth/count 256, strict order, "
            "and fail-fast."
        ),
    )
    parser.add_argument(
        "--preflight",
        dest="preflight",
        action="store_true",
        default=None,
        help="Check that selected RDMA devices exist and are PORT_ACTIVE before running.",
    )
    parser.add_argument(
        "--no-preflight",
        dest="preflight",
        action="store_false",
        help="Disable the automatic preflight used by --apple-gate.",
    )
    parser.add_argument(
        "--preflight-only",
        action="store_true",
        help="Run preflight checks and exit without starting uc_oneway.",
    )
    parser.add_argument(
        "--ssh-config",
        default=os.environ.get("TBV_SSH_CONFIG", ""),
        help="ssh_config file passed to ssh with -F.",
    )
    parser.add_argument(
        "--ssh-option",
        action="append",
        default=[],
        help="extra ssh -o option; may be repeated.",
    )
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--receiver-drain-timeout", type=float, default=10.0)
    parser.add_argument("--start-delay", type=float, default=1.0)
    parser.add_argument(
        "--hold-before-destroy-ms",
        type=int,
        default=-1,
        help=(
            "Pass uc_oneway --hold-before-destroy-ms; -1 selects 1000 ms for "
            "Apple rdma_* devices and 0 otherwise."
        ),
    )
    parser.add_argument("--base-port", type=int, default=24000)
    parser.add_argument("--stop-on-fail", action="store_true")
    parser.add_argument("--no-check", dest="check", action="store_false")
    parser.add_argument("--no-check-any-order", dest="check_any_order", action="store_false")
    parser.add_argument("--strict-order", dest="check_any_order", action="store_false")
    parser.set_defaults(check=True, check_any_order=True)
    args = parser.parse_args()
    global SSH_CONFIG, SSH_OPTIONS
    SSH_CONFIG = args.ssh_config or ""
    SSH_OPTIONS = list(args.ssh_option or [])

    if args.apple_gate:
        args.directions = "both"
        args.sizes = [4096, 65536]
        args.send_depths = [256]
        args.send_slots = 256
        args.recv_depth = 256
        args.recv_posts = 256
        args.mtus = [4096]
        args.count = 256
        args.repeats = 1
        args.apple_queue_depth = 256
        args.timeout = max(args.timeout, 120.0)
        args.receiver_drain_timeout = max(args.receiver_drain_timeout, 30.0)
        args.start_delay = max(args.start_delay, 2.0)
        args.hold_before_destroy_ms = max(args.hold_before_destroy_ms, 1500)
        args.stop_on_fail = True
        args.check = True
        args.check_any_order = False
        if not args.tag:
            args.tag = "apple-gate"
        print(
            "tbv-uc-stress: apple gate preset active "
            "(both directions, sizes=4096,65536, depth=256, count=256, "
            "strict-order)",
            flush=True,
        )

    if args.preflight is None:
        args.preflight = args.apple_gate
    if args.preflight_only:
        args.preflight = True

    if args.count <= 0:
        die("--count must be positive")
    if args.repeats <= 0:
        die("--repeats must be positive")
    if args.recv_depth <= 0 or args.recv_posts <= 0:
        die("--recv-depth and --recv-posts must be positive")
    if any(depth <= 0 for depth in args.send_depths):
        die("--send-depths must be positive")
    if args.send_slots < 0:
        die("--send-slots must be non-negative")
    if args.apple_queue_depth < 0:
        die("--apple-queue-depth must be non-negative")
    if args.hold_before_destroy_ms < -1:
        die("--hold-before-destroy-ms must be -1 or non-negative")

    if args.directions == "forward":
        directions = [
            (
                "forward",
                args.server,
                args.client,
                args.server_dev,
                args.client_dev,
                args.server_gid_index,
                args.client_gid_index,
            )
        ]
    elif args.directions == "reverse":
        directions = [
            (
                "reverse",
                args.client,
                args.server,
                args.client_dev,
                args.server_dev,
                args.client_gid_index,
                args.server_gid_index,
            )
        ]
    else:
        directions = [
            (
                "forward",
                args.server,
                args.client,
                args.server_dev,
                args.client_dev,
                args.server_gid_index,
                args.client_gid_index,
            ),
            (
                "reverse",
                args.client,
                args.server,
                args.client_dev,
                args.server_dev,
                args.client_gid_index,
                args.server_gid_index,
            ),
        ]

    if args.preflight:
        run_preflight(args, directions)
        if args.preflight_only:
            return 0

    csv_path = Path(args.csv)
    log_dir = Path(args.log_dir) if args.log_dir else csv_path.with_suffix("")
    log_dir.mkdir(parents=True, exist_ok=True)

    handle, writer = open_csv(csv_path)
    failures = 0
    run_index = 0
    try:
        for (
            direction,
            receiver,
            sender,
            receiver_dev,
            sender_dev,
            receiver_gid,
            sender_gid,
        ) in directions:
            for size in args.sizes:
                for mtu in args.mtus:
                    for send_depth in args.send_depths:
                        for repeat in range(1, args.repeats + 1):
                            run_index += 1
                            port = args.base_port + run_index
                            effective_send_depth = apple_queue_depth(
                                send_depth,
                                receiver_dev,
                                sender_dev,
                                args.apple_queue_depth,
                            )
                            effective_recv_posts = apple_queue_depth(
                                args.recv_posts,
                                receiver_dev,
                                sender_dev,
                                args.apple_queue_depth,
                            )
                            print(
                                "tbv-uc-stress: "
                                f"{direction} size={size} "
                                f"send_depth={effective_send_depth} "
                                f"recv_posts={effective_recv_posts} mtu={mtu} "
                                f"repeat={repeat}/{args.repeats}",
                                flush=True,
                            )
                            row = run_case(
                                args,
                                direction=direction,
                                receiver=receiver,
                                sender=sender,
                                receiver_dev=receiver_dev,
                                sender_dev=sender_dev,
                                receiver_gid_index=receiver_gid,
                                sender_gid_index=sender_gid,
                                size=size,
                                send_depth=send_depth,
                                mtu=mtu,
                                repeat_index=repeat,
                                port=port,
                                log_dir=log_dir,
                            )
                            writer.writerow(row)
                            handle.flush()
                            if row["status"] != "ok":
                                failures += 1
                                print(
                                    f"tbv-uc-stress: failed: {row['error'][-500:]}",
                                    file=sys.stderr,
                                    flush=True,
                                )
                                if args.stop_on_fail:
                                    return 1
    finally:
        handle.close()

    print(f"tbv-uc-stress: wrote {csv_path}", flush=True)
    if failures:
        print(f"tbv-uc-stress: {failures} case(s) failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
