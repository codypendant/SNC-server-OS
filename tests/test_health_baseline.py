#!/usr/bin/env python3
"""HEALTH and failover baseline (docs/CONTROL_CONNECTIONS_DESIGN.md 5, A2,
and section 6): recorded before any persistent-connection change, so later
slices are compared with it.

Real processes, clustered as tests/test_control.py: three snc_witness
processes holding the placement map, the mint lease blocks and the group's
HA machine; one replication group, a primary and a sync standby, started by
NodeID from the map (--control). Promotion therefore runs the standby's
start barrier (PLACEMENT SYNC) and takes a fresh lease block (LEASES TAKE),
as in production. Witnesses poll with HEALTH, --failover-ms 1000,
--poll-ms 100. Every process runs with SNC_TRACE=1, so each control request
writes one "ha_ask" line (tools/ha.h: caller path, verb, port, conn).

Scenarios (each on a fresh cluster):
  kill      SIGKILL the primary: the witnesses suspect it and promote its
            standby.
  stop      SIGSTOP the primary: the same; after SIGCONT the old primary is
            no longer the group's primary.
  hold      DEBUG ACCEPT-HOLD failover_ms + 10 s on the primary (--debug-admin): its admin
            loop accepts nothing new and keeps serving what it holds, a
            stalled accept path. One-shot HEALTH goes silent: promotion.
  hold_short DEBUG ACCEPT-HOLD 400, under failover_ms: no promotion, the HA
            machine unchanged, HEALTH answers again after the hold.
  partition the witnesses reach the primary's admin port through a proxy
            (tests/test_health.py's Proxy), which is cut: their HEALTH is
            black-holed (the "witness to primary dropped" case of A2(b);
            T6 in test_health cuts every path on a lone pair).
  emfile    the primary's soft descriptor limit set to its lowest free
            descriptor (prlimit), so every accept fails with EMFILE: HEALTH
            goes silent, promotion; once the limit is restored the old
            primary's HEALTH shows adm_accept_fail > 0.

Checked in every scenario (A2(a)): every ha_ask line is conn=oneshot, and
the HA-path requests the design keeps one-shot are present in the kill
scenario's trace (watch HEALTH; the promotion's PLACEMENT SYNC and LEASES
TAKE; the witnesses' Paxos rounds under them; readiness). Recorded (A2(b)):
detection time (fault to the first witness_suspect trace line, both
CLOCK_MONOTONIC) and promotion time (fault to the standby reporting role p),
per trial, printed as one "baseline" line per scenario and, with
SNC_BASELINE_OUT=path, written as JSON. Bounds asserted unless
SNC_SANITIZED is set (bounds below); promotion within failover_ms + 8 s (as test_health T4). The
detection bound is failover_ms + 3 * poll_ms + 500 ms of scheduling slack:
the witness's silence clock starts at the last answered poll, which can
precede the fault by a poll interval, and a silent poll then takes up to
poll_ms before the next check (measured values are recorded; see
docs/HEALTH_BASELINE.md).

The partition case runs in tests/test_health.py T6 (lone pair, its proxy);
SIGKILL and SIGSTOP on a lone pair are T4 and T5 there.

Usage: test_health_baseline.py BUILD_DIR [SCENARIO ...]   (default: all)
       SNC_BASELINE_TRIALS=N   trials of each failover scenario (default 2)
"""
import json
import os
import random
import re
import resource
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from witness_state import first_start  # noqa: E402

FAILOVER_MS = 1000
POLL_MS = 100
SANITIZED = bool(os.environ.get("SNC_SANITIZED"))
TRIALS = int(os.environ.get("SNC_BASELINE_TRIALS", "2"))

# HA-path request kinds the design keeps one-shot (section 4.5), as
# (process, caller path pattern, verb pattern); each must appear in the
# kill scenario's trace, so the instrument is known to see them.
HA_KINDS = [
    ("witness", r"^watch$", r"^HEALTH$"),
    ("witness", r"^watch$", r"^STATUS$"),
    ("witness", r"^watch$", r"^PROMOTE$"),
    ("witness", r"^watch/propose(/read_latest)?$", r"^(READ|PREPARE|ACCEPT|COMMIT) ha/0$"),
    ("server", r"^promote/placement_sync$", r"^PLACEMENT SYNC$"),
    ("server", r"^promote/lease_blocks$", r"^LEASES TAKE$"),
    ("witness", r"^serve_PLACEMENT_SYNC/read_linear(/read_latest)?$", r"^(READ|PREPARE|ACCEPT|COMMIT) placement$"),
    ("witness", r"^serve_LEASES_TAKE/propose(/read_latest)?$", r"^(READ|PREPARE|ACCEPT|COMMIT) leases$"),
    ("server", r"(^|/)ask_witness$", r"^(READY|NOTREADY|DEGRADE|SYNCED)"),
]

TRACE = re.compile(r"^T (\d+) (\d+) ha_ask caller=(\S+) verb=(.*) port=(\d+) conn=(\S+) ok=([01])$")
SUSPECT = re.compile(r"^T (\d+) \d+ witness_suspect ")


def free_ports(n, span=1):
    bases, taken = [], set()
    rnd = random.Random(os.getpid() * 7919 + time.time_ns())
    while len(bases) < n:
        b = rnd.randrange(20000, 32000 - span)
        if any(b + k in taken for k in range(span)):
            continue
        socks = []
        try:
            for k in range(span):
                s = socket.socket()
                socks.append(s)
                s.bind(("127.0.0.1", b + k))
            bases.append(b)
            taken.update(b + k for k in range(span))
        except OSError:
            pass
        finally:
            for s in socks:
                s.close()
    return bases


def ask(port, line, timeout=3):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout) as s:
            s.settimeout(timeout)
            s.sendall((line + "\n").encode())
            return s.makefile().readline().strip()
    except OSError:
        return None


def fields(reply):
    return dict(kv.split("=", 1) for kv in (reply or "").split() if "=" in kv)


def status(port, timeout=1):
    r = ask(port, "STATUS", timeout)
    return fields(r) if r else None


def wait(cond, what, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        v = cond()
        if v:
            return v
        time.sleep(0.02)
    raise AssertionError(f"timed out: {what}")


class Cluster:
    """Three witnesses and one group (node 1 primary, node 2 sync standby)."""

    def __init__(self, build, work, proxy_primary=False):
        self.procs, self.proxy = [], None
        try:
            self._start_all(build, work, proxy_primary)
        except BaseException:
            self.close()  # nothing started is left running
            raise

    def _start_all(self, build, work, proxy_primary):
        self.build, self.work = build, work
        self.notes = []  # what a scenario observed, for its baseline line
        p = free_ports(3 + 2 * 4)
        self.wports = p[:3]
        nodes = [p[3 + 4 * i: 3 + 4 * (i + 1)] for i in range(2)]
        self.control = ",".join(f"127.0.0.1:{w}" for w in self.wports)
        self.admin = {i + 1: nodes[i][0] for i in range(2)}
        link = {i + 1: nodes[i][1] for i in range(2)}
        client = {i + 1: nodes[i][2] for i in range(2)}
        self.repl = nodes[1][3]
        env = dict(os.environ, SNC_TRACE="1")
        self.env = env
        # The witnesses' path to node 1's admin port: direct, or a proxy the
        # partition scenario cuts.
        watch1 = self.admin[1]
        if proxy_primary:
            from test_health import Proxy
            watch1 = free_ports(1)[0]
            self.proxy = Proxy(watch1, self.admin[1])
        for i, w in enumerate(self.wports):
            args = [f"{build}/snc_witness", "--port", str(w), "--state", f"{work}/w{i}", "--id", str(i),
                    "--peers", self.control, "--failover-ms", str(FAILOVER_MS), "--poll-ms", str(POLL_MS),
                    "--group", f"0=127.0.0.1:{watch1},127.0.0.1:{self.admin[2]}"]
            # stderr (the SNC_TRACE lines) apart from stdout, which is block
            # buffered and could split a trace line.
            self.procs.append(subprocess.Popen(first_start(args), stdout=open(f"{work}/w{i}.log", "w"),
                                               stderr=open(f"{work}/w{i}.trace", "w"), env=env))
        time.sleep(0.3)
        r = subprocess.run([f"{build}/snc_ctl", "--control", self.control, "init", "--nodes",
                            f"1@127.0.0.1:{self.admin[1]},2@127.0.0.1:{self.admin[2]}", "--groups",
                            f"2:1/{link[1]}/{client[1]}+2/{link[2]}/{client[2]}"],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        self.servers = {}
        self.start(2, standby=True)
        wait(lambda: status(self.admin[2]), "standby up")
        self.start(1, standby=False)
        st = {i: wait(lambda: status(self.admin[i]), f"node {i} up") for i in (1, 2)}
        assert [st[1]["role"], st[2]["role"]] == ["p", "s"], st
        wait(lambda: (self.read("ha/0") or "").split()[1:2] != ["0"], "the primary learned")
        # READY (degraded 0): until then no standby may be promoted.
        wait(lambda: (self.read("ha/0") or "").split()[4:5] == ["0"], "the group READY")

    def start(self, i, standby):
        args = [f"{self.build}/snc_server", "--shards", "2", "--objects", "10000", "--data-dir",
                f"{self.work}/n{i}", "--control", self.control, "--node-id", str(i), "--admin-port",
                str(self.admin[i]), "--witness", self.control, "--debug-admin"]
        if standby:
            args += ["--standby", str(self.repl)]
        else:
            args += ["--replica-of", f"127.0.0.1:{self.repl}", "--replica-timeout-ms", "300"]
        self.servers[i] = subprocess.Popen(args, stdout=open(f"{self.work}/n{i}.log", "w"),
                                           stderr=open(f"{self.work}/n{i}.trace", "w"), env=self.env)
        self.procs.append(self.servers[i])

    def read(self, m):
        for w in self.wports:
            r = ask(w, f"READ {m}", 1)
            if r:
                return r
        return None

    def promoted(self):
        s = status(self.admin[2], 0.5)
        return s if s and s.get("role") in ("p", "n") else None

    def close(self):
        if self.proxy:
            self.proxy.heal()
        for p in self.procs:
            if p.poll() is None:
                try:
                    p.send_signal(signal.SIGCONT)
                except OSError:
                    pass
                p.kill()
            p.wait()

    def traces(self):
        """(process kind, caller, verb, conn) of every ha_ask line, and the
        CLOCK_MONOTONIC ns of every witness_suspect line."""
        asks, suspects = [], []
        for name in sorted(os.listdir(self.work)):
            if not name.endswith(".trace"):
                continue
            kind = "witness" if name.startswith("w") else "server"
            with open(os.path.join(self.work, name), errors="replace") as f:
                for line in f:
                    m = TRACE.match(line.rstrip("\n"))
                    if m:
                        asks.append((kind, m.group(3), m.group(4), m.group(6)))
                        continue
                    m = SUSPECT.match(line)
                    if m:
                        suspects.append(int(m.group(1)))
        return asks, suspects


def check_oneshot(asks, what):
    # True by construction today (ha::ask writes conn=oneshot): a tripwire
    # for later slices, which label pooled requests otherwise.
    bad = [a for a in asks if a[3] != "oneshot"]
    assert not bad, f"{what}: requests not one-shot: {bad[:5]}"
    assert asks, f"{what}: no ha_ask trace lines (is SNC_TRACE honored?)"


def failover(c, fault_ns):
    """Waits for the promotion; returns (detect_ms, promote_ms)."""
    wait(c.promoted, "the standby promoted", timeout=FAILOVER_MS / 1000 + 12)
    promote_ms = (time.monotonic_ns() - fault_ns) / 1e6
    _, suspects = c.traces()
    after = [t for t in suspects if t >= fault_ns]
    assert after, "no witness_suspect line after the fault"
    detect_ms = (min(after) - fault_ns) / 1e6
    return detect_ms, promote_ms


def check_bounds(name, detect_ms, promote_ms):
    if SANITIZED:
        return
    assert detect_ms <= FAILOVER_MS + 3 * POLL_MS + 500, f"{name}: detection {detect_ms:.0f} ms"
    assert promote_ms <= FAILOVER_MS + 8000, f"{name}: promotion {promote_ms:.0f} ms"


def run_failover(build, name, fault, after=None):
    work = tempfile.mkdtemp(prefix=f"snc-hbase-{name}-")
    c = None
    try:
        c = Cluster(build, work, proxy_primary=name == "partition")
        fault_ns = fault(c)
        detect_ms, promote_ms = failover(c, fault_ns)
        st = c.promoted()
        assert st["term"] == "1", st
        if after:
            after(c)
        asks, _ = c.traces()
        check_oneshot(asks, name)
        check_bounds(name, detect_ms, promote_ms)
        return detect_ms, promote_ms, asks
    finally:
        if c:
            c.close()
        if os.environ.get("SNC_KEEP"):
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


def kill_fault(c):
    t = time.monotonic_ns()
    c.servers[1].send_signal(signal.SIGKILL)
    c.servers[1].wait()
    return t


def stop_fault(c):
    t = time.monotonic_ns()
    c.servers[1].send_signal(signal.SIGSTOP)
    return t


def stop_after(c):
    # The old primary resumes; its role afterwards is recorded, not asserted
    # (test_health T5 asserts the fencing on a lone pair).
    c.servers[1].send_signal(signal.SIGCONT)
    st = wait(lambda: status(c.admin[1]), "the old primary answers after SIGCONT")
    c.notes.append(f"old primary after SIGCONT: role={st.get('role')} term={st.get('term')}")


def partition_fault(c):
    t = time.monotonic_ns()
    c.proxy.cut()
    return t


def hold_fault(c):
    # Held well past the promotion (detection about failover_ms), so a slow
    # host still sees the promotion before HEALTH answers again.
    assert ask(c.admin[1], f"DEBUG ACCEPT-HOLD {FAILOVER_MS + 10000}") == "OK"
    return time.monotonic_ns()


def lowest_free_fd(pid):
    fds = {int(x) for x in os.listdir(f"/proc/{pid}/fd")}
    k = 0
    while k in fds:
        k += 1
    return k


def emfile_fault(c):
    pid = c.servers[1].pid
    soft, hard = resource.prlimit(pid, resource.RLIMIT_NOFILE)
    c.saved_limit = (soft, hard)
    # The lowest free descriptor: no new one can be allocated unless the
    # process closes one below it (then that hole is reused; accept_fail
    # still counts the failures). Not 0: poll() fails with EINVAL once its
    # set is larger than the limit, so the admin loop would go silent
    # through poll, not accept (measured; a different path).
    low = lowest_free_fd(pid)
    resource.prlimit(pid, resource.RLIMIT_NOFILE, (low, hard))
    c.notes.append(f"primary descriptor limit {soft} -> {low} (its lowest free descriptor)")
    return time.monotonic_ns()


def emfile_after(c):
    pid = c.servers[1].pid
    resource.prlimit(pid, resource.RLIMIT_NOFILE, c.saved_limit)
    g = wait(lambda: fields(ask(c.admin[1], "HEALTH", 1)) or None, "the old primary's HEALTH after the limit")
    assert int(g.get("adm_accept_fail", "0")) > 0, {k: v for k, v in g.items() if k.startswith("adm_")}
    c.notes.append(f"old primary adm_accept_fail={g['adm_accept_fail']} adm_evicted={g.get('adm_evicted')}")


def hold_short(build):
    work = tempfile.mkdtemp(prefix="snc-hbase-hold_short-")
    c = None
    try:
        c = Cluster(build, work)
        before = c.read("ha/0")
        hold_ms = FAILOVER_MS * 4 // 10
        assert ask(c.admin[1], f"DEBUG ACCEPT-HOLD {hold_ms}") == "OK"
        t0 = time.monotonic()
        # During the hold a new connection's HEALTH is not answered.
        assert ask(c.admin[1], "HEALTH", hold_ms / 1000 / 4) is None
        time.sleep(max(0.0, hold_ms / 1000 - (time.monotonic() - t0)) + 0.05)
        assert ask(c.admin[1], "HEALTH", 1) is not None, "HEALTH after the hold"
        time.sleep(3 * FAILOVER_MS / 1000)
        after = c.read("ha/0")
        assert after == before, (before, after)
        st1, st2 = status(c.admin[1]), status(c.admin[2])
        assert st1["role"] == "p" and st2["role"] == "s", (st1, st2)
        asks, suspects = c.traces()
        check_oneshot(asks, "hold_short")
        return hold_ms, len(suspects)
    finally:
        if c:
            c.close()
        shutil.rmtree(work, ignore_errors=True)


def table(asks):
    n = Counter((k, c, v) for k, c, v, _ in asks)
    return sorted(n.items(), key=lambda kv: -kv[1])


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build"
    want = sys.argv[2:] or ["kill", "stop", "partition", "hold", "hold_short", "emfile"]
    results = {}
    scenarios = {
        "kill": (kill_fault, None, TRIALS),
        "stop": (stop_fault, stop_after, TRIALS),
        "hold": (hold_fault, None, TRIALS),
        "partition": (partition_fault, None, TRIALS),
        "emfile": (emfile_fault, emfile_after, TRIALS),
    }
    kill_asks = None
    for name in want:
        if name == "hold_short":
            hold_ms, n_suspect = hold_short(build)
            results[name] = {"hold_ms": hold_ms, "promoted": False, "suspect_lines": n_suspect}
            print(f"baseline hold_short: accept held {hold_ms} ms (< failover_ms {FAILOVER_MS}): no promotion, "
                  f"HA machine unchanged, HEALTH answered after it; witness_suspect lines {n_suspect}")
            continue
        fault, after, trials = scenarios[name]
        rows, notes = [], []

        def after_note(c, after=after):
            if after:
                after(c)
            notes.extend(c.notes)

        for _ in range(trials):
            detect_ms, promote_ms, asks = run_failover(build, name, fault, after_note)
            rows.append({"detect_ms": round(detect_ms, 1), "promote_ms": round(promote_ms, 1)})
            if name == "kill" and kill_asks is None:
                kill_asks = asks
        results[name] = {"trials": rows, "notes": notes}
        d = ", ".join(f"{r['detect_ms']:.0f}/{r['promote_ms']:.0f}" for r in rows)
        print(f"baseline {name}: detection/promotion ms per trial: {d}"
              + (f"; {'; '.join(notes)}" if notes else ""))
    if kill_asks is not None:
        for proc, cpat, vpat in HA_KINDS:
            assert any(k == proc and re.search(cpat, c) and re.search(vpat, v) for k, c, v, _ in kill_asks), \
                f"no {proc} request {cpat} {vpat} in the kill trace"
        unlabeled = Counter(v for k, c, v, _ in kill_asks if c == "-")
        print(f"baseline kill: requests with no caller label: {dict(unlabeled) or 'none'}")
        results["kill_unlabeled"] = dict(unlabeled)
        results["kill_requests"] = [{"proc": k, "caller": c, "verb": v, "n": n} for (k, c, v), n in table(kill_asks)]
        print("baseline kill: control requests by process, caller, verb (all one-shot):")
        for (k, c, v), n in table(kill_asks):
            print(f"  {n:6d}  {k:7s} {c:45s} {v}")
    out = os.environ.get("SNC_BASELINE_OUT")
    if out:
        with open(out, "w") as f:
            json.dump({"failover_ms": FAILOVER_MS, "poll_ms": POLL_MS, "trials": TRIALS, "cpus": os.cpu_count(),
                       "sanitized": SANITIZED, "results": results}, f, indent=1)
    print("test_health_baseline: ok")


if __name__ == "__main__":
    main()
