#!/usr/bin/env python3
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


HOST_ALIASES = {
    "strix-1": "192.168.23.136",
    "strix-2": "192.168.23.192",
}


CSV_FIELDS = [
    "timestamp_utc",
    "tag",
    "server",
    "client",
    "direction",
    "test",
    "size_bytes",
    "qps",
    "iters",
    "port",
    "status",
    "duration_s",
    "server_rails",
    "client_rails",
    "server_rail_speeds",
    "client_rail_speeds",
    "server_ib_speed",
    "client_ib_speed",
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
    "error",
]


def die(msg: str) -> None:
    print(f"tbv-rdma-sweep: {msg}", file=sys.stderr)
    raise SystemExit(1)


def require_env(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        die(f"{name} is not set")
    return value


RDMA_CORE = require_env("TBV_RDMA_CORE")
PERFTEST = require_env("TBV_PERFTEST")


def parse_csv_ints(value: str) -> list[int]:
    out = []
    for item in value.split(","):
        item = item.strip()
        if item:
            out.append(int(item, 0))
    if not out:
        raise argparse.ArgumentTypeError("empty list")
    return out


def resolve_host(host: str) -> str:
    return HOST_ALIASES.get(host, host)


def ssh_args(host: str, command: str) -> list[str]:
    return [
        "ssh",
        "-o",
        "ConnectTimeout=8",
        "-o",
        "BatchMode=yes",
        "-o",
        "StrictHostKeyChecking=accept-new",
        f"root@{host}",
        "bash -lc " + shlex.quote(command),
    ]


def run_local(args: list[str], *, check: bool = True, timeout: int | None = None) -> subprocess.CompletedProcess:
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


def run_ssh(host: str, command: str, *, check: bool = True, timeout: int | None = None) -> subprocess.CompletedProcess:
    return run_local(ssh_args(host, command), check=check, timeout=timeout)


def copy_tools(host: str) -> None:
    run_local(
        [
            "nix",
            "copy",
            "--no-check-sigs",
            "--to",
            f"ssh-ng://root@{host}",
            RDMA_CORE,
            PERFTEST,
        ],
        check=True,
    )


def speed_gbps(value: str) -> float | None:
    if not value or value.lower() == "any":
        return None
    match = re.search(r"([0-9]+(?:\.[0-9]+)?)", value)
    if not match:
        return None
    return float(match.group(1))


def parse_attrs(line: str) -> dict[str, str]:
    return dict(re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([^ ]+)", line))


def collect_state(host: str, dev: str, backend: str) -> dict[str, object]:
    peers = run_ssh(
        host,
        "cat /sys/kernel/debug/thunderbolt_ibverbs/peers 2>/dev/null || true",
        check=False,
    ).stdout
    devinfo = run_ssh(
        host,
        f"LD_LIBRARY_PATH={shlex.quote(RDMA_CORE + '/lib')} "
        f"{shlex.quote(RDMA_CORE + '/bin/ibv_devinfo')} -d {shlex.quote(dev)} -v 2>&1",
        check=False,
    ).stdout

    current_backend = None
    rails = []
    for raw in peers.splitlines():
        line = raw.strip()
        peer_match = re.match(r"peer\s+\S+\s+backend=([^ ]+)", line)
        if peer_match:
            current_backend = peer_match.group(1)
            continue
        if current_backend != backend or not line.startswith("rail="):
            continue
        attrs = parse_attrs(line)
        if attrs.get("active") == "1" and attrs.get("data_ready") == "1":
            rails.append(attrs)

    ib_speed = ""
    ib_width = ""
    port_state = ""
    for raw in devinfo.splitlines():
        line = raw.strip()
        if line.startswith("active_speed:"):
            ib_speed = line.split(":", 1)[1].strip()
        elif line.startswith("active_width:"):
            ib_width = line.split(":", 1)[1].strip()
        elif line.startswith("state:"):
            port_state = line.split(":", 1)[1].strip()

    speeds = sorted({rail.get("link_speed", "") for rail in rails if rail.get("link_speed")})
    return {
        "host": host,
        "rails": len(rails),
        "rail_speeds": speeds,
        "ib_speed": ib_speed,
        "ib_width": ib_width,
        "port_state": port_state,
        "peers": peers,
        "devinfo": devinfo,
    }


def assert_state(state: dict[str, object], expect_rails: int, expect_speed: str) -> None:
    host = state["host"]
    rails = int(state["rails"])
    if rails != expect_rails:
        die(f"{host}: expected {expect_rails} ready rails, saw {rails}")

    expected = speed_gbps(expect_speed)
    speeds = list(state["rail_speeds"])
    if expected is not None:
        observed = [speed_gbps(speed) for speed in speeds]
        bad = [speed for speed, parsed in zip(speeds, observed) if parsed != expected]
        if bad or len(observed) == 0:
            die(f"{host}: expected rail speed {expect_speed}, saw {','.join(speeds) or 'none'}")

    if "PORT_ACTIVE" not in str(state["port_state"]):
        die(f"{host}: RDMA port is not active: {state['port_state']}")


def state_for_csv(state: dict[str, object]) -> tuple[str, str, str]:
    return (
        str(state["rails"]),
        "|".join(state["rail_speeds"]),
        str(state["ib_speed"]),
    )


def parse_bw(stdout: str) -> dict[str, str]:
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) >= 5 and parts[0].isdigit() and parts[1].isdigit():
            return {
                "bw_peak_gbps": parts[2],
                "bw_avg_gbps": parts[3],
                "msg_rate_mpps": parts[4],
            }
    raise ValueError("could not parse bandwidth summary")


def parse_lat(stdout: str) -> dict[str, str]:
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


def perftest_command(bin_name: str, args: argparse.Namespace, size: int, iters: int, qps: int, port: int, peer: str | None) -> str:
    parts = [
        f"LD_LIBRARY_PATH={shlex.quote(RDMA_CORE + '/lib')}",
        "timeout",
        shlex.quote(str(args.timeout)),
        shlex.quote(f"{PERFTEST}/bin/{bin_name}"),
        "-F",
        "-d",
        shlex.quote(args.dev),
        "-x",
        shlex.quote(str(args.gid_index)),
        "-s",
        shlex.quote(str(size)),
        "-n",
        shlex.quote(str(iters)),
        "-q",
        shlex.quote(str(qps)),
        "-p",
        shlex.quote(str(port)),
        "--report_gbits",
    ]
    if peer:
        parts.append(shlex.quote(peer))
    return " ".join(parts)


def run_pair(bin_name: str, parser, args: argparse.Namespace, server_host: str, client_host: str, size: int, iters: int, qps: int, port: int) -> tuple[str, dict[str, str], str]:
    server_cmd = perftest_command(bin_name, args, size, iters, qps, port, None)
    client_cmd = perftest_command(bin_name, args, size, iters, qps, port, resolve_host(server_host))

    server_proc = subprocess.Popen(
        ssh_args(server_host, server_cmd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(args.server_start_delay)

    client_proc = run_ssh(client_host, client_cmd, check=False, timeout=args.timeout + 10)
    try:
        server_stdout, _ = server_proc.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        server_proc.terminate()
        server_stdout, _ = server_proc.communicate(timeout=5)

    combined = f"-- client --\n{client_proc.stdout}\n-- server --\n{server_stdout}"
    if client_proc.returncode != 0:
        return "fail", {}, f"client rc={client_proc.returncode}: {client_proc.stdout.strip()[-500:]}"
    if server_proc.returncode != 0:
        return "fail", {}, f"server rc={server_proc.returncode}: {server_stdout.strip()[-500:]}"

    try:
        return "ok", parser(client_proc.stdout), ""
    except ValueError as exc:
        return "fail", {}, f"{exc}: {combined[-1000:]}"


def open_csv(path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    new_file = not path.exists() or path.stat().st_size == 0
    handle = path.open("a", newline="")
    writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
    if new_file:
        writer.writeheader()
    return handle, writer


def base_row(
    args: argparse.Namespace,
    direction: str,
    server_label: str,
    client_label: str,
    test: str,
    size: int,
    qps: int,
    iters: int,
    port: int,
    server_state: dict[str, object],
    client_state: dict[str, object],
) -> dict[str, str]:
    server_rails, server_speeds, server_ib_speed = state_for_csv(server_state)
    client_rails, client_speeds, client_ib_speed = state_for_csv(client_state)
    return {
        "timestamp_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
        "tag": args.tag,
        "server": server_label,
        "client": client_label,
        "direction": direction,
        "test": test,
        "size_bytes": str(size),
        "qps": str(qps),
        "iters": str(iters),
        "port": str(port),
        "status": "",
        "duration_s": "",
        "server_rails": server_rails,
        "client_rails": client_rails,
        "server_rail_speeds": server_speeds,
        "client_rail_speeds": client_speeds,
        "server_ib_speed": server_ib_speed,
        "client_ib_speed": client_ib_speed,
        "bw_peak_gbps": "",
        "bw_avg_gbps": "",
        "msg_rate_mpps": "",
        "lat_min_us": "",
        "lat_max_us": "",
        "lat_typical_us": "",
        "lat_avg_us": "",
        "lat_stdev_us": "",
        "lat_p99_us": "",
        "lat_p999_us": "",
        "error": "",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a Thunderbolt ibverbs RDMA benchmark sweep and write CSV results.")
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--csv", default=f"tbv-rdma-sweep-{dt.datetime.now().strftime('%Y%m%d-%H%M%S')}.csv")
    parser.add_argument("--tag", default="")
    parser.add_argument("--expect-rails", type=int, default=2)
    parser.add_argument("--expect-speed", default="10Gb/s", help="Per-ready-rail link speed to require, or 'any'.")
    parser.add_argument("--backend", default="native")
    parser.add_argument("--directions", choices=["forward", "reverse", "both"], default="forward")
    parser.add_argument("--tests", choices=["bw", "lat", "both"], default="both")
    parser.add_argument("--bw-sizes", type=parse_csv_ints, default=parse_csv_ints("1024,4096,16384,65536,262144,1048576"))
    parser.add_argument("--lat-sizes", type=parse_csv_ints, default=parse_csv_ints("64,256,1024,4096,16384,65536"))
    parser.add_argument("--qps", type=parse_csv_ints, default=None, help="Bandwidth QP list shorthand. Latency stays qps=1 unless --lat-qps is set.")
    parser.add_argument("--bw-qps", type=parse_csv_ints, default=None)
    parser.add_argument("--lat-qps", type=parse_csv_ints, default=None)
    parser.add_argument("--bw-iters", type=int, default=1000)
    parser.add_argument("--lat-iters", type=int, default=1000)
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--base-port", type=int, default=18515)
    parser.add_argument("--server-start-delay", type=float, default=0.7)
    parser.add_argument("--dev", default="usb4_rdma0")
    parser.add_argument("--gid-index", type=int, default=1)
    parser.add_argument("--reload", action="store_true")
    parser.add_argument("--stop-on-fail", action="store_true")
    args = parser.parse_args()
    bw_qps = args.bw_qps or args.qps or parse_csv_ints("1,2,4")
    lat_qps = args.lat_qps or parse_csv_ints("1")
    if any(qps != 1 for qps in lat_qps):
        die("ib_write_lat only supports qps=1; use --bw-qps/--qps for multi-QP bandwidth sweeps")

    server_host = resolve_host(args.server)
    client_host = resolve_host(args.client)

    for host in {server_host, client_host}:
        copy_tools(host)
        if args.reload:
            run_ssh(host, "TBV_WAIT_SECS=10 thunderbolt-ibverbs-reload-system", check=True, timeout=30)

    server_state = collect_state(server_host, args.dev, args.backend)
    client_state = collect_state(client_host, args.dev, args.backend)
    assert_state(server_state, args.expect_rails, args.expect_speed)
    assert_state(client_state, args.expect_rails, args.expect_speed)

    print(
        "tbv-rdma-sweep: state ok: "
        f"{args.server} rails={server_state['rails']} speeds={','.join(server_state['rail_speeds'])}; "
        f"{args.client} rails={client_state['rails']} speeds={','.join(client_state['rail_speeds'])}",
        flush=True,
    )

    directions: list[tuple[str, str, str, str, str]]
    if args.directions == "forward":
        directions = [("forward", server_host, client_host, args.server, args.client)]
    elif args.directions == "reverse":
        directions = [("reverse", client_host, server_host, args.client, args.server)]
    else:
        directions = [
            ("forward", server_host, client_host, args.server, args.client),
            ("reverse", client_host, server_host, args.client, args.server),
        ]

    csv_path = Path(args.csv)
    handle, writer = open_csv(csv_path)
    failures = 0
    run_index = 0
    try:
        for direction_name, run_server, run_client, server_label, client_label in directions:
            if args.tests in ("bw", "both"):
                for qps in bw_qps:
                    for size in args.bw_sizes:
                        run_index += 1
                        port = args.base_port + run_index
                        server_state = collect_state(run_server, args.dev, args.backend)
                        client_state = collect_state(run_client, args.dev, args.backend)
                        assert_state(server_state, args.expect_rails, args.expect_speed)
                        assert_state(client_state, args.expect_rails, args.expect_speed)

                        row = base_row(
                            args,
                            direction_name,
                            server_label,
                            client_label,
                            "ib_write_bw",
                            size,
                            qps,
                            args.bw_iters,
                            port,
                            server_state,
                            client_state,
                        )
                        print(f"tbv-rdma-sweep: {direction_name} bw size={size} qps={qps}", flush=True)
                        start = time.monotonic()
                        status, metrics, error = run_pair("ib_write_bw", parse_bw, args, run_server, run_client, size, args.bw_iters, qps, port)
                        row["duration_s"] = f"{time.monotonic() - start:.3f}"
                        row["status"] = status
                        row.update(metrics)
                        row["error"] = error.replace("\n", " ")
                        writer.writerow(row)
                        handle.flush()
                        if status != "ok":
                            failures += 1
                            if args.stop_on_fail:
                                return 1

            if args.tests in ("lat", "both"):
                for qps in lat_qps:
                    for size in args.lat_sizes:
                        run_index += 1
                        port = args.base_port + run_index
                        server_state = collect_state(run_server, args.dev, args.backend)
                        client_state = collect_state(run_client, args.dev, args.backend)
                        assert_state(server_state, args.expect_rails, args.expect_speed)
                        assert_state(client_state, args.expect_rails, args.expect_speed)

                        row = base_row(
                            args,
                            direction_name,
                            server_label,
                            client_label,
                            "ib_write_lat",
                            size,
                            qps,
                            args.lat_iters,
                            port,
                            server_state,
                            client_state,
                        )
                        print(f"tbv-rdma-sweep: {direction_name} lat size={size} qps={qps}", flush=True)
                        start = time.monotonic()
                        status, metrics, error = run_pair("ib_write_lat", parse_lat, args, run_server, run_client, size, args.lat_iters, qps, port)
                        row["duration_s"] = f"{time.monotonic() - start:.3f}"
                        row["status"] = status
                        row.update(metrics)
                        row["error"] = error.replace("\n", " ")
                        writer.writerow(row)
                        handle.flush()
                        if status != "ok":
                            failures += 1
                            if args.stop_on_fail:
                                return 1
    finally:
        handle.close()

    print(f"tbv-rdma-sweep: wrote {csv_path}", flush=True)
    if failures:
        print(f"tbv-rdma-sweep: {failures} benchmark rows failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
