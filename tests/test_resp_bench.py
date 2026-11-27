#!/usr/bin/env python3
"""resp_bench's settlement probe on a real Garnet (bench/garnet, FileStream,
commit-waited AOF):

  conserve  transfers, deposits and the probe at 10^9 starting balances with
            a 1 s SLO: every probe settles, --verify finds every account
            (probe accounts included) exact.
  late      Garnet stopped 500 ms mid-window with a 20 ms SLO: probes due in
            the stall miss, because the reply and the read must both come
            back by intended send + SLO + g.
  unseen    negative control: probe accounts preset below the start balance
            (--no-load), so no deadline read can show the expected balance:
            every probe misses.
  flags     bad probe settings are refused.

Usage: test_resp_bench.py BUILD_DIR GARNET [scenario ...]
"""
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "bench", "durability"))
import snc  # noqa: E402

BUILD, GARNET = sys.argv[1], sys.argv[2]
ONLY = sys.argv[3:]
START = 10**9
COMMON = ["--threads", "2", "--conns", "2", "--objects", "2000", "--transfer", "50", "--reads", "20",
          "--start-balance", str(START), "--verify"]


class Garnet:
    def __init__(self):
        self.work = tempfile.mkdtemp(prefix="snc-resp-")
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        self.log = open(os.path.join(self.work, "garnet.log"), "w")
        self.proc = subprocess.Popen([GARNET, "--bind", "127.0.0.1", "--port", str(self.port), "--checkpointdir",
                                      os.path.join(self.work, "d"), "--aof", "--aof-commit-freq", "0",
                                      "--aof-commit-wait", "--device-type", "FileStream", "--memory", "1g",
                                      "--index", "64m"], stdout=self.log, stderr=self.log)
        assert snc.wait_port(self.port, self.proc, 120), open(self.log.name).read()

    def bench(self, extra, ok=True, during=None):
        cmd = [f"{BUILD}/resp_bench", "--port", str(self.port)] + COMMON + extra
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        t = threading.Thread(target=during, daemon=True) if during else None
        if t:
            t.start()
        out, _ = p.communicate(timeout=180)
        if t:
            t.join()
        assert (p.returncode == 0) == ok, (cmd, p.returncode, out)
        return out

    def set_balance(self, first, n, balance):
        v = struct.pack("<q", balance) + bytes(192)
        with socket.create_connection(("127.0.0.1", self.port), 10) as s:
            f = s.makefile("rb")
            s.sendall(b"".join(b"*3\r\n$3\r\nSET\r\n$16\r\n" + struct.pack("<QQ", 1, j + 1) + b"\r\n$200\r\n" + v
                               + b"\r\n" for j in range(first, first + n)))
            for _ in range(n):
                assert f.readline() == b"+OK\r\n"

    def close(self):
        self.proc.kill()
        self.proc.wait()
        shutil.rmtree(self.work, ignore_errors=True)


def probe(out):
    m = re.search(r"probe: .* probed (\d+)  misses (\d+)  miss_pct [\d.]+  skipped (\d+)  reads (\d+)", out)
    assert m, out
    return dict(zip(["probed", "misses", "skipped", "reads"], map(int, m.groups())))


def conserve(g):
    # A grace of 5 s rides out the 100-300 ms AOF stalls seen on CI VMs, so any
    # miss here is the probe's accounting, not the host.
    out = g.bench(["--rate", "2000", "--warmup", "1", "--seconds", "4", "--drain-s", "20", "--probe-pct", "10",
                   "--probe-slo-us", "1000000", "--probe-grace-us", "5000000", "--probe-accounts", "1024"])
    p = probe(out)
    assert p["probed"] > 200 and p["misses"] == 0 and p["skipped"] == 0 and p["reads"] >= p["probed"], out
    assert "verify: 3024 accounts, 0 mismatches" in out, out
    print(f"conserve: 3024 accounts exact, {p['probed']} probes, 0 misses")


def late(g):
    def stall():
        time.sleep(2.5)
        g.proc.send_signal(signal.SIGSTOP)
        time.sleep(0.5)
        g.proc.send_signal(signal.SIGCONT)
    out = g.bench(["--rate", "2000", "--warmup", "1", "--seconds", "4", "--probe-pct", "20", "--probe-slo-us",
                   "20000", "--probe-accounts", "1024"], during=stall)
    p = probe(out)
    assert p["misses"] >= 0.05 * p["probed"] and p["misses"] < p["probed"], out
    print(f"late: {p['misses']} of {p['probed']} probes missed across a 500 ms stall")


def unseen(g):
    g.set_balance(0, 2000, START)
    g.set_balance(2000, 64, START - 1000)
    out = g.bench(["--no-load", "--rate", "1000", "--seconds", "3", "--probe-pct", "20", "--probe-slo-us", "1000000",
                   "--probe-accounts", "64"], ok=False)
    p = probe(out)
    assert p["probed"] > 50 and p["misses"] == p["probed"], out
    assert "verify: 2064 accounts, 64 mismatches" in out, out
    print(f"unseen: {p['misses']} of {p['probed']} probes missed with credits hidden")


def flags(_):
    for bad in (["--rate", "100", "--probe-pct", "1"], ["--rate", "100", "--probe-pct", "101"],
                ["--rate", "100", "--probe-pct", "1", "--probe-slo-us", "20000000"],
                ["--rate", "100", "--threads", "4", "--probe-pct", "1", "--probe-slo-us", "1000",
                 "--probe-accounts", "2"]):
        p = subprocess.run([f"{BUILD}/resp_bench"] + bad, capture_output=True, text=True)
        assert p.returncode == 2, (bad, p.returncode, p.stderr)
    print("flags: bad probe settings refused")


def main():
    for name in ("flags", "conserve", "late", "unseen"):
        if ONLY and name not in ONLY:
            continue
        g = None if name == "flags" else Garnet()
        try:
            globals()[name](g)
        finally:
            if g:
                g.close()


if __name__ == "__main__":
    main()
