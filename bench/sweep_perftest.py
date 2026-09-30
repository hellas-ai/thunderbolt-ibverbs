#!/usr/bin/env python3
"""Bandwidth and latency sweep with both hosts' CPU load, for plot_perftest.py.

Runs perftest between this host (client) and a server host over SSH, one case
at a time, and samples /proc/stat on both hosts while the case runs. The CPU
load is system-wide, because thunderbolt_ibverbs does most of its work in
kernel workers that a per-process measurement (perftest --cpu_util) misses.

    sweep_perftest.py --server misty --dev usb4_rdma_p1r0 --gid 1 \\
        --label "usb4_rdma, 2 cables" --csv tbv.csv

Appends one row per case: label, test, size, gbps, lat_us, client_cores,
server_cores (busy cores averaged over the sample window).
"""
import argparse
import csv
import os
import re
import shlex
import subprocess
import threading
import time

BW_SIZES = [2**k for k in range(6, 23, 2)]  # 64 B .. 4 MiB
LAT_SIZES = [2**k for k in range(6, 21, 2)]  # 64 B .. 1 MiB


def stat_cmd(seconds: float) -> str:
    # Busy and total jiffies before and after the window, and the CPU count.
    return (f"head -1 /proc/stat; sleep {seconds}; head -1 /proc/stat; nproc")


def busy_cores(out: str) -> float:
    lines = out.split("\n")
    a = [int(x) for x in lines[0].split()[1:]]
    b = [int(x) for x in lines[1].split()[1:]]
    ncpu = int(lines[2])
    d = [y - x for x, y in zip(a, b)]
    total = sum(d[:8])
    idle = d[3] + d[4]
    return ncpu * (total - idle) / total if total else 0.0


def run_case(args, test: str, size: int, port: int, bidir: bool):
    lat = test.endswith("_lat")
    flags = f"-d {args.dev} -x {args.gid} -s {size} -D {args.seconds} -F -p {port}"
    if not lat:
        flags += " --report_gbits"
        if bidir:
            flags += " -b"
    env = f"RDMAV_DRIVERS={args.rdmav_drivers} " if args.rdmav_drivers else ""
    server = subprocess.Popen(
        ["ssh", args.server, f"{env}timeout {args.seconds + 30} {test} {flags}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    window = args.seconds - 2
    samples = {}

    def sample(where):
        cmd = stat_cmd(window)
        if where == "server":
            cmd = ["ssh", args.server, cmd]
        else:
            cmd = ["sh", "-c", cmd]
        time.sleep(1.5)
        samples[where] = busy_cores(subprocess.run(cmd, capture_output=True,
                                                   text=True).stdout)

    threads = [threading.Thread(target=sample, args=(w,)) for w in ("client", "server")]
    for t in threads:
        t.start()
    client = subprocess.run(
        shlex.split(f"timeout {args.seconds + 30} {test} {flags} {args.server}"),
        capture_output=True, text=True,
        env=dict(os.environ, **({"RDMAV_DRIVERS": args.rdmav_drivers} if args.rdmav_drivers else {})))
    for t in threads:
        t.join()
    server.wait()
    gbps = lat_us = ""
    for line in client.stdout.splitlines():
        f = line.split()
        if len(f) >= 4 and re.fullmatch(r"\d+", f[0]) and re.fullmatch(r"\d+", f[1]):
            if lat:
                lat_us = f[2]  # t_avg in duration mode
            else:
                gbps = f[3]  # BW average
    return gbps, lat_us, samples.get("client", ""), samples.get("server", "")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--server", required=True)
    p.add_argument("--dev", required=True)
    p.add_argument("--gid", type=int, default=0)
    p.add_argument("--label", required=True)
    p.add_argument("--csv", required=True)
    p.add_argument("--seconds", type=int, default=6)
    p.add_argument("--port", type=int, default=19600)
    p.add_argument("--rdmav-drivers", default=os.environ.get("RDMAV_DRIVERS", ""))
    p.add_argument("--only", default="", help="substring filter on test names")
    p.add_argument("--max-size", action="append", default=[], metavar="TEST=BYTES",
                   help="skip larger sizes of a test, e.g. ib_read_bw=524288")
    args = p.parse_args()

    cases = [(t, s, False) for t in ("ib_write_bw", "ib_read_bw", "ib_send_bw") for s in BW_SIZES]
    cases += [("ib_write_bw", s, True) for s in BW_SIZES]
    cases += [(t, s, False) for t in ("ib_write_lat", "ib_read_lat", "ib_send_lat") for s in LAT_SIZES]
    max_size = {k: int(v) for k, v in (m.split("=", 1) for m in args.max_size)}
    new = not os.path.exists(args.csv)
    with open(args.csv, "a", newline="") as fh:
        w = csv.writer(fh)
        if new:
            w.writerow(["label", "test", "size", "gbps", "lat_us", "client_cores", "server_cores"])
        for i, (test, size, bidir) in enumerate(cases):
            name = test + ("_bidir" if bidir else "")
            if args.only and args.only not in name:
                continue
            if size > max_size.get(name, size):
                continue
            gbps, lat_us, cc, sc = run_case(args, test, size, args.port + i % 50, bidir)
            row = [args.label, name, size, gbps, lat_us,
                   f"{cc:.2f}" if cc != "" else "", f"{sc:.2f}" if sc != "" else ""]
            w.writerow(row)
            fh.flush()
            print(" ".join(str(x) for x in row), flush=True)


if __name__ == "__main__":
    main()
