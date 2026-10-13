#!/usr/bin/env python3
"""Automatic failover with snc_witness (docs/REPLICATION.md).

Primary snc_server on cores 0-1 (sync, --replica-timeout-ms 500,
--witness); standby on core 2; snc_witness (--failover-ms 500, --poll-ms
50); one net_bench thread on core 3. Four cases, each checked:

  kill:      the primary is SIGKILLed after a loaded run. Measures the time
             from the kill to the witness's promotion and checks that the
             promoted node serves.
  degraded:  the standby is stopped (SIGSTOP) under load; the primary asks
             the witness and continues without it. The standby is resumed
             and catches up (the primary tells the witness), then stopped
             again (a second grant). Then the primary is killed and the
             standby resumed: the witness must not promote it, since it
             lacks acknowledged writes.
  partition: the primary is stopped (SIGSTOP), as if cut off; the witness
             promotes the standby. The old primary is resumed: it must
             acknowledge no write (its standby refuses its term and the
             witness refuses to let it continue alone).
  rearm:     two failovers in a row. The primary is killed and the standby
             promoted; it runs alone under a grant and creates accounts.
             The old primary is restarted with its original command line
             (--rejoin): it moves its data aside and rejoins as the new
             primary's standby, seeded from it. Once it is back in sync the
             new primary is killed, the witnesses promote the old one again,
             and every account created in between must be there.

--witnesses 3 runs three replicated witnesses (Paxos among them) instead of
one; --down 1 also kills that many of them (witness 0 first, which the
primary lists first) once the primary is up, so every decision is made by
the rest.

Prints CSV rows; usage: run_failover.py [--reps N] [--witnesses N]
[--down K] [--out FILE].
"""
import argparse
import csv
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

BUILD = "build"
WORK = "/tmp/snc-failover-bench"
UUID = "5a" * 16
WITNESSES = 1  # --witnesses
DOWN = 0       # --down


def wait_port(port, timeout=60, proc=None):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    sys.exit(f"port {port} never opened" + ("\n" + proc.text() if proc else ""))


class Proc:
    """A process with its output lines collected, with arrival times."""

    def __init__(self, args):
        self.p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.lines = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.append((time.time(), line.rstrip()))

    def wait_for(self, pattern, timeout):
        t_end = time.time() + timeout
        while True:
            for t, l in list(self.lines):
                if re.search(pattern, l):
                    return t, l
            if time.time() >= t_end:
                return None, None
            time.sleep(0.005)

    def text(self):
        return "\n".join(l for _, l in self.lines)

    def kill(self, sig=signal.SIGKILL):
        if self.p.poll() is None:
            self.p.send_signal(sig)

    def wait(self, timeout):
        self.p.wait(timeout=timeout)


class Witnesses:
    """The witnesses, looked at as one: a line from any of them counts."""

    def __init__(self, procs):
        self.procs = procs

    def wait_for(self, pattern, timeout):
        t_end = time.time() + timeout
        while time.time() < t_end:
            for w in self.procs:
                t, l = w.wait_for(pattern, 0)
                if t is not None:
                    return t, l
            time.sleep(0.005)
        return None, None

    def text(self):
        return "\n".join(f"[witness {i}] " + w.text() for i, w in enumerate(self.procs))

    def kill(self, sig=signal.SIGKILL):
        for w in self.procs:
            w.kill(sig)

    def wait(self, timeout):
        for w in self.procs:
            w.wait(timeout)


def net_bench(port, objects, seconds, timeout=120):
    try:
        out = subprocess.run(
            [f"{BUILD}/net_bench", "--port", str(port), "--threads", "1", "--cpus", "3",
             "--objects", str(objects), "--seconds", str(seconds), "--route", "owner",
             "--reads", "50", "--depth", "256"], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    m = re.search(r"ops/s (\d+)", out.stdout)
    return float(m[1]) if m else None


def witness_list(port):
    return ",".join(f"127.0.0.1:{port + 30 + i}" for i in range(WITNESSES))


def node_args(port, node, rearm):
    """snc_server flags for node 0 (starts as primary) or 1 (starts as
    standby). With rearm, each is also configured for the other role: it
    replicates to the other node once promoted, under the witnesses, and
    serves seeds."""
    cli, adm, repl, seed = ((port, port + 40, port + 60, port + 70) if node == 0 else
                            (port + 10, port + 50, port + 20, port + 80))
    o_repl = port + 20 if node == 0 else port + 60
    a = ["--port", str(cli), "--data-dir", f"{WORK}/{'ps'[node]}", "--space-uuid", UUID,
         "--admin-port", str(adm), "--objects", "200000"]
    if node == 0 or rearm:
        a += ["--replica-of", f"127.0.0.1:{o_repl}", "--replica-timeout-ms", "500",
              "--witness", witness_list(port)]
    if rearm:
        a += ["--seed-port", str(seed)]
        if node == 0:
            # Restarted with this same command line, it rejoins as node 1's
            # standby if node 1 has been promoted.
            a += ["--rejoin", f"{repl},127.0.0.1:{port + 50},127.0.0.1:{port + 80}"]
    return a, repl, adm


def start(port, rearm=False):
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    p_cli, s_cli, wit, p_adm, s_adm = (port + i for i in (0, 10, 30, 40, 50))
    a, repl, _ = node_args(port, 1, rearm)
    sb = Proc([f"{BUILD}/snc_server", "--cpus", "2,2", "--standby", str(repl)] + a)
    wait_port(s_adm, proc=sb)
    a, _, _ = node_args(port, 0, rearm)
    prim = Proc([f"{BUILD}/snc_server", "--cpus", "0,1"] + a)
    wait_port(p_adm, proc=prim)
    peers = ",".join(f"127.0.0.1:{wit + i}" for i in range(WITNESSES))
    w = Witnesses([
        Proc([f"{BUILD}/snc_witness", "--port", str(wit + i), "--primary", f"127.0.0.1:{p_adm}",
              "--standby", f"127.0.0.1:{s_adm}", "--state", f"{WORK}/witness{i}.state",
              "--failover-ms", "500", "--poll-ms", "50"] +
             (["--id", str(i), "--peers", peers] if WITNESSES > 1 else []))
        for i in range(WITNESSES)])
    if w.wait_for(r"primary up", 10)[0] is None:
        sys.exit("witness never saw the primary\n" + w.text())
    for i in range(DOWN):
        w.procs[i].kill()
    return prim, sb, w, p_cli, s_cli


def stop_all(*procs):
    for p in procs:
        p.kill()
        p.wait(timeout=60)


def case_kill(port):
    prim, sb, w, p_cli, s_cli = start(port)
    before = net_bench(p_cli, 100000, 3)
    t_kill = time.time()
    prim.kill()
    t, line = w.wait_for(r"failover:", 15)
    assert t is not None, "no failover\n" + w.text()
    promote_s = float(re.search(r"in ([\d.]+)s", line)[1])
    after = net_bench(s_cli, 4000, 2)
    assert after and after > 0, "the promoted node does not serve"
    stop_all(sb, w)
    return dict(case="kill", ok=1, before_ops_s=round(before), kill_to_promoted_s=round(t - t_kill, 3),
                promote_s=promote_s, after_ops_s=round(after))


def case_degraded(port):
    prim, sb, w, p_cli, s_cli = start(port)
    res = {}
    th = threading.Thread(target=lambda: res.update(ops=net_bench(p_cli, 100000, 6)))
    th.start()
    time.sleep(4.5)  # about 3 s of loading, then the run
    t_stop = time.time()
    sb.kill(signal.SIGSTOP)
    t, _ = prim.wait_for(r"witness: granted", 10)
    assert t is not None, "the primary was not granted\n" + prim.text() + w.text()
    th.join()
    assert res.get("ops"), "the clients stalled for good"
    # Back in sync: the primary tells the witness, which may fail over again.
    sb.kill(signal.SIGCONT)
    assert prim.wait_for(r"back in sync", 10)[0] is not None, "never back in sync\n" + prim.text()
    assert w.wait_for(r"SYNCED", 5)[0] is not None
    # Stopped again, under a little load: a second grant.
    th = threading.Thread(target=lambda: net_bench(p_cli, 2000, 3))
    th.start()
    time.sleep(0.5)
    sb.kill(signal.SIGSTOP)
    deadline = time.time() + 10
    while time.time() < deadline and len(re.findall(r"witness: granted", prim.text())) < 2:
        time.sleep(0.01)
    assert len(re.findall(r"witness: granted", prim.text())) == 2, "no second grant\n" + prim.text()
    th.join()
    prim.kill()
    sb.kill(signal.SIGCONT)
    t2, _ = w.wait_for(r"failover:", 3)
    assert t2 is None, "promoted a standby that lacks acknowledged writes\n" + w.text()
    stop_all(sb, w)
    return dict(case="degraded", ok=1, before_ops_s=round(res["ops"]),
                stall_to_grant_s=round(t - t_stop, 3))


def case_partition(port):
    prim, sb, w, p_cli, s_cli = start(port)
    before = net_bench(p_cli, 20000, 1)
    t_stop = time.time()
    prim.kill(signal.SIGSTOP)
    t, _ = w.wait_for(r"failover:", 15)
    assert t is not None, "no failover\n" + w.text()
    prim.kill(signal.SIGCONT)
    # The old primary: a fresh client's creates must never be acknowledged.
    old = net_bench(p_cli, 1000, 1, timeout=5)
    assert old is None, f"the old primary acknowledged writes ({old} ops/s)"
    t_ref, _ = prim.wait_for(r"witness: refused", 5)
    assert t_ref is not None, "the old primary was not refused\n" + prim.text()
    after = net_bench(s_cli, 4000, 2)
    assert after and after > 0, "the promoted node does not serve"
    stop_all(prim, sb, w)
    return dict(case="partition", ok=1, before_ops_s=round(before),
                kill_to_promoted_s=round(t - t_stop, 3), after_ops_s=round(after))


def bench_ids(port, ids, objects, seconds):
    """net_bench with --ids: creates the accounts and lists them in `ids`,
    or, if the file exists, uses them (a missing one counts as an error).
    Returns (ops/s, errors)."""
    out = subprocess.run(
        [f"{BUILD}/net_bench", "--port", str(port), "--threads", "1", "--cpus", "3",
         "--objects", str(objects), "--seconds", str(seconds), "--route", "owner",
         "--reads", "50", "--depth", "64", "--ids", ids], capture_output=True, text=True,
        timeout=120).stdout
    m, e = re.search(r"ops/s (\d+)", out), re.search(r"errors (\d+)", out)
    return (float(m[1]) if m else None), (int(e[1]) if e else None)


def case_rearm(port):
    """Two failovers in a row. The primary (node 0) is killed and node 1
    promoted; node 1 runs alone (granted) and creates accounts. Node 0 is
    restarted with its original command line: it finds node 1 primary,
    moves its data aside and rejoins as node 1's standby, seeded from it.
    Once it is back in sync, node 1 is killed: the witnesses promote node 0
    again, and every account node 1 created is there."""
    prim, sb, w, p_cli, s_cli = start(port, rearm=True)
    net_bench(p_cli, 20000, 1)
    prim.kill()
    prim.wait(60)
    t1, _ = w.wait_for(r"failover:.*term 1 ", 15)
    assert t1 is not None, "no first failover\n" + w.text()
    ids = f"{WORK}/ids"
    ops, err = bench_ids(s_cli, ids, 20000, 2)
    assert ops and err == 0, f"node 1 alone: {ops} ops/s, {err} errors\n" + sb.text()
    assert sb.wait_for(r"witness: granted", 1)[0] is not None, "node 1 ran without a grant"
    # Node 0 restarts as it first started, and rejoins as a standby.
    a, _, adm = node_args(port, 0, True)
    t_join = time.time()
    prim = Proc([f"{BUILD}/snc_server", "--cpus", "0,1"] + a)
    wait_port(adm, proc=prim)
    assert prim.wait_for(r"rejoin: starting as its standby", 5)[0] is not None, \
        "node 0 did not rejoin\n" + prim.text()
    assert any(d.startswith("p.stale-") for d in os.listdir(WORK)), "old data not kept aside"
    t_sync, _ = sb.wait_for(r"witness: standby back in sync", 60)
    assert t_sync is not None, "node 0 never back in sync\n" + sb.text() + prim.text()
    # The second failover.
    t_kill = time.time()
    sb.kill()
    sb.wait(60)
    t2, line = w.wait_for(r"failover:.*term 2 ", 15)
    assert t2 is not None, "no second failover\n" + w.text()
    ops2, err2 = bench_ids(p_cli, ids, 20000, 2)
    assert ops2 and err2 == 0, f"node 0 after the second failover: {ops2} ops/s, {err2} errors"
    stop_all(prim, w)
    return dict(case="rearm", ok=1, rejoin_to_sync_s=round(t_sync - t_join, 3),
                kill_to_promoted_s=round(t2 - t_kill, 3), after_ops_s=round(ops2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--cases", default="kill,degraded,partition,rearm")
    ap.add_argument("--witnesses", type=int, default=1)
    ap.add_argument("--down", type=int, default=0)
    ap.add_argument("--out", default="bench/results/failover.csv")
    a = ap.parse_args()
    global WITNESSES, DOWN
    WITNESSES, DOWN = a.witnesses, a.down
    rows, port = [], 23000  # below the ephemeral range: a client socket could hold a port
    cases = dict(kill=case_kill, degraded=case_degraded, partition=case_partition,
                 rearm=case_rearm)
    for rep in range(a.reps):
        for c in a.cases.split(","):
            port += 100
            r = cases[c](port)
            r["rep"] = rep
            r["witnesses"], r["down"] = WITNESSES, DOWN
            rows.append(r)
            print(r, flush=True)
    fields = ["case", "witnesses", "down", "rep", "ok", "before_ops_s", "kill_to_promoted_s", "promote_s",
              "stall_to_grant_s", "rejoin_to_sync_s", "after_ops_s"]
    with open(a.out, "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=fields)
        wr.writeheader()
        for r in rows:
            wr.writerow({k: r.get(k) for k in fields})
    shutil.rmtree(WORK, ignore_errors=True)


if __name__ == "__main__":
    main()
