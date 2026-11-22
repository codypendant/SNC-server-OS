#!/usr/bin/env python3
"""net_bench's Track B checks (docs/DIRECTION_COMPARISON_PLAN.md 4.1), on a
real durable snc_server (2 shards):

  conserve   transfers, deposits and the settlement probe at 10^12 starting
             balances: --verify finds every account exact, no rejections,
             the probe sees every credit in time.
  rejected   balances of 0: transfers without funds are counted as rejected,
             not as errors, and --verify still holds (they never applied).
  mismatch   a wrong --start-balance on existing accounts: --verify fails
             and exits 1.
  late       the server stopped 500 ms mid-window: probes due in the stall
             miss, because each read must answer by intended send + SLO + g.
  flags      the probe needs --rate and an SLO.

Usage: test_net_bench_verify.py BUILD_DIR [scenario ...]
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_async_failover as af  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
ONLY = sys.argv[2:]
BIG = str(10**12)


class Server:
    def __init__(self):
        self.work = tempfile.mkdtemp(prefix="snc-verify-")
        self.port = af.free_ports(1)[0]
        self.proc = af.Proc([f"{BUILD}/snc_server", "--shards", "2", "--port", str(self.port), "--objects",
                             "40000", "--data-dir", os.path.join(self.work, "data")])
        assert af.wait_port(self.port) and af.wait_port(self.port + 1), self.proc.text()

    def bench(self, extra, ok=True, during=None):
        cmd = [f"{BUILD}/net_bench", "--port", str(self.port), "--threads", "2"] + extra
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        t = threading.Thread(target=during, daemon=True) if during else None
        if t:
            t.start()
        out, _ = p.communicate(timeout=180)
        if t:
            t.join()
        assert (p.returncode == 0) == ok, (cmd, p.returncode, out)
        return out

    def close(self):
        self.proc.stop()
        shutil.rmtree(self.work, ignore_errors=True)


def probe(out):
    m = re.search(r"probe: .* probed (\d+)  misses (\d+)  miss_pct [\d.]+  skipped (\d+)  reads (\d+)", out)
    assert m, out
    return dict(zip(["probed", "misses", "skipped", "reads"], map(int, m.groups())))


def rejected(out):
    return int(re.search(r"\n  rejected (\d+) ", out)[1])


def scenario_conserve(s):
    out = s.bench(["--objects", "4000", "--rate", "4000", "--seconds", "3", "--transfer", "40", "--reads", "50",
                   "--xfer-local", "0", "--start-balance", BIG, "--probe-pct", "5", "--probe-slo-us", "1000000",
                   "--verify"])
    assert " 0 mismatches " in out and ": ok" in out, out
    assert rejected(out) == 0, out
    p = probe(out)
    assert p["probed"] > 100 and p["misses"] == 0 and p["skipped"] == 0 and p["reads"] >= p["probed"], out
    print(f"conserve: 5024 accounts exact, {p['probed']} probes, 0 misses")


def scenario_rejected(s):
    out = s.bench(["--objects", "2000", "--rate", "3000", "--seconds", "1", "--transfer", "60", "--reads", "0",
                   "--start-balance", "0", "--verify"])
    assert rejected(out) > 100 and re.search(r"errors 0 ", out), out
    assert " 0 mismatches " in out, out
    print(f"rejected: {rejected(out)} transfers without funds counted apart; verify holds")


def scenario_mismatch(s):
    ids = os.path.join(s.work, "ids")
    s.bench(["--objects", "1000", "--ids", ids, "--seconds", "0"])
    out = s.bench(["--objects", "1000", "--ids", ids, "--rate", "1000", "--seconds", "1", "--reads", "0",
                   "--start-balance", "7", "--verify"], ok=False)
    assert "FAILED" in out, out
    print("mismatch: a wrong expectation fails --verify")


def scenario_late(s):
    def stall():
        time.sleep(1.5)
        s.proc.signal(signal.SIGSTOP)
        time.sleep(0.5)
        s.proc.signal(signal.SIGCONT)
    out = s.bench(["--objects", "4000", "--rate", "4000", "--seconds", "3", "--transfer", "50", "--reads", "0",
                   "--xfer-local", "0", "--start-balance", BIG, "--probe-pct", "10", "--probe-slo-us", "20000",
                   "--verify"], during=stall)
    p = probe(out)
    # About 0.3 s of 3 s of probes fall in or near the stall: they miss.
    assert p["misses"] >= 0.05 * p["probed"] and p["misses"] < p["probed"], out
    assert " 0 mismatches " in out, out
    print(f"late: {p['misses']} of {p['probed']} probes missed across a 500 ms stall")


def scenario_flags(s):
    for bad in (["--probe-pct", "1"], ["--probe-pct", "1", "--rate", "100"], ["--probe-pct", "101"],
                ["--start-balance", "-1"]):
        p = subprocess.run([f"{BUILD}/net_bench", "--port", str(s.port)] + bad, capture_output=True, text=True)
        assert p.returncode == 2, (bad, p.returncode, p.stdout, p.stderr)
    print("flags: bad probe settings refused")


def main():
    scenarios = [("flags", scenario_flags), ("conserve", scenario_conserve), ("rejected", scenario_rejected),
                 ("mismatch", scenario_mismatch), ("late", scenario_late)]
    s = Server()
    try:
        for name, fn in scenarios:
            if not ONLY or name in ONLY:
                fn(s)
    finally:
        s.close()


if __name__ == "__main__":
    main()
