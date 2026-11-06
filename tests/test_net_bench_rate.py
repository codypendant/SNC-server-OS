#!/usr/bin/env python3
"""net_bench's open-loop --rate mode and exact --mix (PR #61 review), on a
real snc_server (memory, 2 shards). The schedule arithmetic itself is
tests/test_rate_schedule.cpp.

  stall_open     --rate, server SIGSTOPped 400 ms mid-window: every op due
                 during the stall is charged from its intended time, so p99
                 and max show the stall.
  stall_closed   the same stall in the old closed loop at depth 1: only the
                 one op in flight sees it, so p99 hides it (coordinated
                 omission; why the pilot may not set p99 from a closed loop).
  owed           --rate with --depth 4 and a stall across the window's end:
                 owed work waits for slots (issue lateness, end backlog), is
                 issued after the window, and still drains.
  undrained      a stall longer than --drain-s: "drained no" with the
                 remaining work counted, exit 0.
  idle_thread    a rate below the thread count: idle threads wait, not spin.
  mix            --mix shares are of total ops and are exact; the old
                 nested flags print their effective shares; bad mixes and
                 bad flag combinations are refused.

Usage: test_net_bench_rate.py BUILD_DIR [scenario ...]
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
OBJECTS = 20000
MIX = "read=70,deposit=15,transfer=10,index=5"


class Server:
    def __init__(self):
        self.work = tempfile.mkdtemp(prefix="snc-rate-")
        self.ids = os.path.join(self.work, "ids")
        while True:
            self.port = af.free_ports(1)[0]
            if self.port + 1 < 32000:
                break
        self.proc = af.Proc([f"{BUILD}/snc_server", "--shards", "2", "--port", str(self.port),
                             "--objects", str(OBJECTS * 2)])
        assert af.wait_port(self.port) and af.wait_port(self.port + 1), self.proc.text()
        out = self.bench(["--seconds", "0", "--mix", MIX])  # creates the accounts and the ids file
        assert "load: %d objects" % OBJECTS in out, out

    def bench(self, extra, during=None, timeout=120):
        cmd = [f"{BUILD}/net_bench", "--port", str(self.port), "--threads", "2", "--objects", str(OBJECTS),
               "--ids", self.ids] + extra
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        t = None
        if during:
            t = threading.Thread(target=during, daemon=True)
            t.start()
        out, _ = p.communicate(timeout=timeout)
        if t:
            t.join()
        assert p.returncode == 0, (cmd, p.returncode, out)
        return out

    def stall(self, after_s, for_s):
        def go():
            time.sleep(after_s)
            self.proc.signal(signal.SIGSTOP)
            time.sleep(for_s)
            self.proc.signal(signal.SIGCONT)
        return go

    def close(self):
        self.proc.stop()
        shutil.rmtree(self.work, ignore_errors=True)


def latency(out, what="total"):
    m = re.search(rf"\n  {what} latency ns  n (\d+)  p50 (\d+)  p95 (\d+)  p99 (\d+)  p99.9 (\d+)  max (\d+)", out)
    assert m, out
    return dict(zip(["n", "p50", "p95", "p99", "p999", "max"], map(int, m.groups())))


def rate_report(out):
    r = {}
    m = re.search(r"scheduled (\d+)  scheduled_in_window (\d+)  issued_by_end (\d+)  completed_in_window (\d+)  "
                  r"achieved (\d+) ops/s", out)
    assert m, out
    r.update(zip(["scheduled", "scheduled_in_window", "issued_by_end", "in_window", "achieved"], map(int, m.groups())))
    m = re.search(r"end_backlog (\d+) \(([\d.]+)% of scheduled_in_window\)  in_flight_at_end (\d+)", out)
    r.update(backlog=int(m[1]), backlog_pct=float(m[2]), in_flight=int(m[3]))
    m = re.search(r"issue lateness ns  p50 (\d+)  p99 (\d+)  p99.9 (\d+)  max (\d+)  late_over_1ms (\d+)", out)
    r.update(late_p99=int(m[2]), late_max=int(m[4]), late_1ms=int(m[5]))
    m = re.search(r"drained (yes|no)  remaining_unissued (\d+)  remaining_in_flight (\d+)", out)
    r.update(drained=m[1] == "yes", unissued=int(m[2]), left=int(m[3]))
    return r


MS = 1_000_000


def scenario_stall_open(s):
    # 2000 ops/s for 3 s after 0.5 s warm-up; stall 400 ms at about 1.5 s.
    # About 800 ops are due during the stall, over 10% of the window, so the
    # top 1% all waited most of it.
    out = s.bench(["--rate", "2000", "--warmup", "0.5", "--seconds", "3", "--mix", MIX],
                  during=s.stall(1.5, 0.4))
    lat, r = latency(out), rate_report(out)
    assert lat["max"] >= 380 * MS, out
    assert lat["p99"] >= 250 * MS, out
    assert r["drained"] and r["backlog"] == 0, out
    assert r["scheduled"] == 2000 * 3.5, out  # warm-up + window, exact
    assert r["scheduled_in_window"] == 2000 * 3, out  # the measured window only (review C1)
    assert lat["n"] == 6000, out  # every op due in the window was measured once
    print(f"stall_open: p99 {lat['p99'] / MS:.0f} ms, max {lat['max'] / MS:.0f} ms (stall 400 ms)")


def scenario_stall_closed(s):
    out = s.bench(["--depth", "1", "--seconds", "3", "--mix", MIX], during=s.stall(1.5, 0.4))
    lat = latency(out)
    assert lat["max"] >= 380 * MS, out
    assert lat["p99"] < 50 * MS, out  # the stall is one op among many
    print(f"stall_closed: p99 {lat['p99'] / 1000:.0f} us, max {lat['max'] / MS:.0f} ms: closed loop hides it")


def scenario_owed(s):
    # Depth 4 per thread: the slots fill within ms of the stall, then ops are
    # owed. The stall (2.6 s .. 3.4 s) crosses the window's end (3 s).
    out = s.bench(["--rate", "2000", "--depth", "4", "--seconds", "3", "--drain-s", "10", "--mix", MIX],
                  during=s.stall(2.6, 0.8))
    lat, r = latency(out), rate_report(out)
    assert r["late_max"] >= 300 * MS and r["late_1ms"] >= 300, out  # owed ops were issued late
    assert r["backlog"] >= 200, out  # owed and not yet issued when the window ended
    # No warm-up here, so both denominators are 6000; the share is of the window's ops.
    assert r["scheduled_in_window"] == 6000 and abs(r["backlog_pct"] - 100 * r["backlog"] / 6000) < 0.001, out
    assert r["drained"] and r["unissued"] == 0 and r["left"] == 0, out
    assert lat["n"] == 6000 and lat["max"] >= 300 * MS, out  # issued after the window, still measured
    print(f"owed: end backlog {r['backlog']}, issue lateness max {r['late_max'] / MS:.0f} ms, drained")


def scenario_undrained(s):
    out = s.bench(["--rate", "2000", "--depth", "4", "--seconds", "1", "--drain-s", "0.5", "--mix", MIX],
                  during=s.stall(0.7, 1.5))
    r = rate_report(out)
    assert not r["drained"] and r["unissued"] + r["left"] > 0, out
    print(f"undrained: remaining unissued {r['unissued']}, in flight {r['left']}")


def scenario_idle_thread(s):
    # 1 op/s over 2 threads: thread 1 has nothing to send, thread 0 one op
    # per second. Both must wait in 1 ms polls, not spin: a spinning thread
    # would use about 2 s of CPU in this 2 s run.
    out = os.path.join(s.work, "idle.out")
    with open(out, "w") as f:
        p = subprocess.Popen([f"{BUILD}/net_bench", "--port", str(s.port), "--threads", "2", "--objects",
                              str(OBJECTS), "--ids", s.ids, "--rate", "1", "--seconds", "2", "--mix", MIX],
                             stdout=f, stderr=subprocess.STDOUT)
        _, status, ru = os.wait4(p.pid, 0)
    text = open(out).read()
    assert os.waitstatus_to_exitcode(status) == 0, text
    r = rate_report(text)
    assert r["scheduled"] == 2 and r["drained"], text
    cpu = ru.ru_utime + ru.ru_stime
    assert cpu < 0.8, (cpu, text)
    print(f"idle_thread: 1 op/s on 2 threads used {cpu:.2f} s CPU in a 2 s run (no spin)")


def scenario_mix(s):
    out = s.bench(["--seconds", "1", "--mix", MIX])
    assert "mix of total ops (--mix): read 70.00%  deposit 15.00%  transfer 10.00%  index 5.00%" in out, out
    n = {k: latency(out, k)["n"] for k in ("read", "deposit", "transfer", "index")}
    total = sum(n.values())
    assert total == latency(out)["n"] and total > 20000, out
    for k, want in (("read", 70), ("deposit", 15), ("transfer", 10), ("index", 5)):
        got = 100 * n[k] / total
        assert abs(got - want) < 1.0, (k, got, out)
    # The draft's flags: "of the rest" nesting, so not 70/15/10/5.
    out = s.bench(["--seconds", "0.2", "--index", "5", "--transfer", "10", "--reads", "70"])
    assert "(nested flags): read 59.85%  deposit 25.65%  transfer 9.50%  index 5.00%" in out, out
    out = s.bench(["--seconds", "0.2", "--reads", "50"])  # no index: no index lines, as before
    assert "index ops/s" not in out and "\n  index latency" not in out, out
    for bad in (["--mix", "read=70,deposit=20"], ["--mix", "read=100,read=0"], ["--mix", "write=100"],
                ["--mix", "read=90,index=10", "--index", "5"], ["--warmup", "1"], ["--rate", "0"],
                ["--mix", "read=-5,deposit=105"], ["--rate", "1e6"], ["--rate", "-5"],
                ["--mix", "read=100", "--mix", "read=100"]):
        p = subprocess.run([f"{BUILD}/net_bench", "--port", str(s.port)] + bad, capture_output=True, text=True)
        assert p.returncode == 2, (bad, p.returncode, p.stdout, p.stderr)
    print("mix: exact shares of total; nested flags print 59.85/25.65/9.50/5.00; bad input refused")


def main():
    scenarios = [("mix", scenario_mix), ("idle_thread", scenario_idle_thread), ("stall_open", scenario_stall_open), ("stall_closed", scenario_stall_closed),
                 ("owed", scenario_owed), ("undrained", scenario_undrained)]
    s = Server()
    try:
        for name, fn in scenarios:
            if not ONLY or name in ONLY:
                fn(s)
    finally:
        s.close()
    print("test_net_bench_rate ok")


if __name__ == "__main__":
    main()
