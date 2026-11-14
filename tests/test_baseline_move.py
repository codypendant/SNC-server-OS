#!/usr/bin/env python3
"""Pre-baseline tests T-Q2 and T-Q10 (docs/EXPERIMENTAL_BASELINE.md section 5)
on the cluster of tests/test_fail_closed.py made Profile B: two replication
groups of a sync primary and its standby, three snc_witness processes
holding placement v2 and promoting (HA), every node with --witness (the
DEGRADE gate). Real processes; the client is the shipped ClusterClient
(snc_load run: transfers with sessions, request timeout 3000 ms, following
failovers and slot moves). At the end, once every transfer has settled
(AUDIT pending 0 on both primaries), snc_load check compares every account,
read from the primary serving it, with the acknowledged transfers: each
account served exactly once, its balance the start plus its acknowledged
transfers plus a subset of the in-doubt ones.

  q2_lost_move   T-Q2: the answers of the source primary to sequenced
                 requests are discarded (DEBUG DROP-ANSWERS: the requests
                 run, the answers are "lost"); while they are, the slot
                 those transfers come from moves to the other group. Then
                 answers flow again. The client's resends of the lost
                 requests reach the source (same session, same sequence)
                 after the move: answered from the source's session
                 (kRespReplayed), not run again and not E_MOVED; its new
                 requests get E_MOVED, refresh and go to the destination.
                 Checked: replays > 0, moves > 0, the client's epoch past
                 the move, every transfer settled, exact balances with depth
                 1 (nothing in doubt: each resend is its session's latest).
  q10_<step>     T-Q10: every node checkpoints every 0.3 s. A move under
                 load is driven to <step> (copied, sealed, staged,
                 committed, activated) and stopped there; once both
                 primaries have checkpointed after the stop, every node is
                 SIGKILLed (standbys first) and each primary restarts from
                 its directory (recovery from the checkpoint plus the WAL),
                 then its standby. Checked: before the commit, the slot's
                 phase and serving state at both primaries after the
                 restart equal those before it; after the commit, both
                 have adopted the committed placement (destination ACTIVE
                 and serving, source sealed), never both serving; a new driver finishes the
                 move (DONE); exact balances, every transfer settled, the
                 placement epoch 2.

Every scenario prints one JSON line (and writes it to --json DIR).

Usage: test_baseline_move.py BUILD_DIR [SCENARIO ...] [--json DIR]
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ARGS = list(sys.argv[1:])
JSON_DIR = None
if "--json" in ARGS:
    i = ARGS.index("--json")
    JSON_DIR = ARGS[i + 1]
    del ARGS[i:i + 2]
BUILD = ARGS[0] if ARGS else "build"
SCENARIOS = ARGS[1:]
sys.argv = [sys.argv[0], BUILD]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_fail_closed as fc  # noqa: E402

START = 1000000


class BCluster(fc.Cluster):
    """Profile B: sync, witnesses promote, every node gated by them;
    `ckpt` adds --checkpoint-s to every node."""

    def __init__(self, work, ckpt=None):
        self.ckpt = ckpt
        super().__init__(work, groups=2, ha=True, async_replica=False, v2=True)

    def start(self, k, standby_of=None, extra=(), bin_dir=None):
        more = ["--witness", self.control]
        if self.ckpt:
            more += ["--checkpoint-s", str(self.ckpt)]
        super().start(k, standby_of=standby_of, extra=list(extra) + more, bin_dir=bin_dir)


def snc_load(*args, timeout=600):
    r = subprocess.run([f"{BUILD}/snc_load"] + [str(a) for a in args], capture_output=True, text=True,
                       timeout=timeout)
    return r.returncode, r.stdout + r.stderr


class Load:
    def __init__(self, c, ids, seconds, threads, depth):
        self.prefix = f"{c.work}/load"
        self.p = subprocess.Popen([f"{BUILD}/snc_load", "run", "--connect", f"127.0.0.1:{c.client[1]}", "--ids", ids,
                                   "--seconds", str(seconds), "--threads", str(threads), "--depth", str(depth),
                                   "--hot-pct", "100", "--report-ms", "500", "--out", self.prefix],
                                  stdout=open(f"{c.work}/load.log", "w"), stderr=subprocess.STDOUT)

    def wait(self, timeout=300):
        try:
            rc = self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            rc = self.p.wait()
        s = {}
        try:
            for line in open(self.prefix + ".summary"):
                k, _, v = line.strip().partition(" ")
                s[k] = v
        except OSError:
            s["error"] = "no summary"
        s["rc"] = rc
        return s

    def stop(self):
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait()


def primaries(c):
    return [k for k in sorted(c.server) if c.server[k].poll() is None and c.role(k) in ("p", "n")]


def settle_and_check(c, ids, res, timeout=120):
    """Waits until every transfer has settled, then checks the balances."""
    t_end = time.time() + timeout
    pending = None
    while time.time() < t_end:
        pending = {}
        for k in primaries(c):
            r = c.ask(k, "AUDIT", 30)
            pending[k] = int(r.split()[3]) if r and r.startswith("AUDIT") else None
        if pending and all(v == 0 for v in pending.values()):
            break
        time.sleep(0.5)
    res["pending_at_end"] = pending
    files = []
    for k in primaries(c):
        path = f"{c.work}/bal.{k}"
        r = c.ask(k, f"AUDIT {path}", 60)
        if r and r.startswith("AUDIT") and int(r.split()[1]) > 0:
            files.append(path)
    rc, out = snc_load("check", "--ids", ids, "--start", START, "--balances", ",".join(files),
                       "--runs", f"{c.work}/load")
    res["check"] = out.strip().splitlines()[-3:]
    return rc == 0 and all(v == 0 for v in pending.values())


def make(c, accounts=200, in_slot=200):
    ids = f"{c.work}/ids"
    rc, out = snc_load("create", "--connect", f"127.0.0.1:{c.client[1]}", "--accounts", accounts, "--in-slot",
                       in_slot, "--ids", ids)
    assert rc == 0, out
    m = re.search(r" in slot (\d+)", out)
    assert m, out
    return ids, int(m[1])


def owner_primary(c, slot):
    return 2 * c.info(1, slot)["map_group"] + 1


def q2_lost_move(work):
    c = BCluster(work)
    res = {}
    load = None
    try:
        ids, slot = make(c)
        src = owner_primary(c, slot)
        dst = 3 if src == 1 else 1
        res.update(slot=slot, source=src, dest=dst)
        load = Load(c, ids, 30, threads=4, depth=1)
        time.sleep(3)
        assert c.ask(src, "DEBUG DROP-ANSWERS 1").startswith("OK")
        time.sleep(1.5)  # requests run at the source, their answers are lost
        t = time.time()
        out, err = c.ctl("move", slot, c.group_of(dst), timeout=300)
        res["move"] = out
        res["move_s"] = round(time.time() - t, 2)
        r = c.ask(src, "DEBUG DROP-ANSWERS 0")
        res["answers_dropped"] = int(r.split("=")[1]) if r and "=" in r else None
        res["dest_info_after"] = {k: v for k, v in c.info(dst, slot).items() if k in ("phase", "serving", "map_epoch")}
        s = load.wait()
        res["load"] = {k: s.get(k) for k in ("rc", "acked", "replayed", "unknown", "moved", "migrating", "refreshes",
                                              "failovers", "resends", "max_resend_age_ms", "epoch", "error")}
        ok = settle_and_check(c, ids, res)
        ok = ok and out.startswith("DONE") and s["rc"] == 0 and int(s["replayed"]) > 0 and int(s["moved"]) > 0
        ok = ok and int(s["unknown"]) == 0 and int(s["epoch"]) >= 2 and (res["answers_dropped"] or 0) > 0
        res["pass"] = ok
        return res
    finally:
        if load:
            load.stop()
        keep(c, "q2_lost_move", res)
        c.stop()


def checkpoints_after(c, k, t):
    """Count of 'checkpoint:' lines node k logged; the log is appended to,
    so compare counts before and after."""
    try:
        return open(f"{c.work}/n{k}.log").read().count("checkpoint:")
    except OSError:
        return 0


def q10(step):
    def run(work):
        c = BCluster(work, ckpt=0.3)
        res = {"step": step}
        load = None
        try:
            ids, slot = make(c)
            src = owner_primary(c, slot)
            dst = 3 if src == 1 else 1
            res.update(slot=slot, source=src, dest=dst)
            load = Load(c, ids, 40, threads=2, depth=8)
            # DIAG (never merge): every 0.5 s, each node's HEALTH and shard-0 GAUGES (both non-blocking).
            import threading
            diag_stop = threading.Event()
            def diag_sampler():
                t0 = time.time()
                dumped = []
                last_dump = {}
                with open(f"{c.work}/zsampler.log", "w") as f:
                    while not diag_stop.is_set():
                        for k in sorted(c.admin):
                            if k == 0:
                                continue
                            h = c.ask(k, "HEALTH", 1)
                            g = c.ask(k, "GAUGES 0", 1)
                            fa = re.search(r" s0\.wal_flush_age_ms=(\d+)", g or "")
                            # DIAG: a WAL write in flight > 1.5 s: kernel-side evidence, no ptrace (sysrq-w
                            # puts every blocked task's kernel stack in dmesg). At most 4 captures per node.
                            if fa and int(fa[1]) > 1500 and dumped.count(k) < 4 and time.time() - last_dump.get(k, 0) > 3:
                                dumped.append(k)
                                last_dump[k] = time.time()
                                rep = dumped.count(k)
                                pids = {kk: c.server[kk].pid for kk in c.server if c.server[kk].poll() is None}
                                sh = ("echo === flush_age_ms=%s node=%s pids=%s; date +%%s.%%N; cat /proc/pressure/io; "
                                      "for b in /sys/block/*; do echo $b $(cat $b/inflight); done; grep -E 'Dirty|Writeback:' /proc/meminfo; "
                                      "for p in %s; do for t in /proc/$p/task/*; do echo \"-- $p $(basename $t) "
                                      "$(cat $t/comm) $(awk '{print $3}' $t/stat) wchan=$(cat $t/wchan)\"; cat $t/stack; done; done; "
                                      "echo '-- D-state tasks system-wide'; ps -eLo pid,tid,stat,comm,wchan:32 | awk '$3 ~ /D/'; "
                                      "for t in $(ps -eLo tid,stat,comm | awk '$2 ~ /D/ || $3 ~ /jbd2|kworker.*ext4|iou-wrk/ {print $1}'); do "
                                      "echo \"-- task $t $(cat /proc/$t/comm 2>/dev/null)\"; cat /proc/$t/stack 2>/dev/null; done; "
                                      "echo w > /proc/sysrq-trigger; sleep 0.2; dmesg | tail -n 400"
                                      % (fa[1], k, pids, " ".join(str(v) for v in pids.values())))
                                try:
                                    r = subprocess.run(["sudo", "-n", "sh", "-c", sh], capture_output=True, text=True, timeout=30)
                                    open(f"{c.work}/zkstack_n{k}_{rep}.log", "w").write(r.stdout + r.stderr)
                                except Exception as e:  # noqa: BLE001
                                    open(f"{c.work}/zkstack_n{k}_{rep}.log", "w").write(repr(e))
                            f.write(f"{time.time() - t0:.1f} n{k} H {h}\n{time.time() - t0:.1f} n{k} G {g}\n")
                        f.flush()
                        diag_stop.wait(0.5)
            diag_th = threading.Thread(target=diag_sampler, daemon=True)
            diag_th.start()
            res["diag_stop"] = diag_stop
            time.sleep(2)
            out, err = c.ctl("move", "--stop-after", step, slot, c.group_of(dst), timeout=300)
            assert out == f"STOPPED {step}", out + err
            before = {k: c.info(k, slot) for k in (src, dst)}
            n_ck = {k: checkpoints_after(c, k, 0) for k in (src, dst)}
            fc.wait(lambda: all(checkpoints_after(c, k, 0) >= n_ck[k] + 2 for k in (src, dst)),
                    "both primaries checkpointed after the stop", 30)
            res["checkpoints_after_stop"] = {k: checkpoints_after(c, k, 0) - n_ck[k] for k in (src, dst)}
            # Every node down (standbys first, so the witnesses have no one
            # to promote), then each primary restarts from its directory and
            # then its standby.
            for k in (c.peer(src), c.peer(dst), src, dst):
                c.kill(k)
            for k in (src, dst):
                c.start(k)
            for k in (src, dst):
                fc.wait(lambda: c.role(k) in ("p", "n"), f"node {k} restarted as primary", 60)
            for k in (src, dst):
                c.start(c.peer(k), standby_of=k)
            after = {k: c.info(k, slot) for k in (src, dst)}
            keys = ("phase", "move", "epoch", "serving", "map_group", "map_epoch")
            res["before"] = {k: {x: before[k][x] for x in keys} for k in before}
            res["after"] = {k: {x: after[k][x] for x in keys} for k in after}
            # Before the commit, every node restarts in the state it had.
            # Once the placement is committed (epoch 2), a node that had not
            # yet adopted it does so at restart (its start barrier): the
            # destination ACTIVE and serving, the source sealed with the
            # destination's map. Never both serving, nothing undone.
            if step in ("committed", "activated"):
                want = {src: {"phase": 2, "move": before[src]["move"], "epoch": before[src]["epoch"], "serving": 0,
                              "map_group": c.group_of(dst), "map_epoch": 2},
                        dst: {"phase": 1, "move": before[dst]["move"], "epoch": before[dst]["epoch"], "serving": 1,
                              "map_group": c.group_of(dst), "map_epoch": 2}}
                same = all(after[k][x] == want[k][x] for k in want for x in keys)
            else:
                same = all(before[k][x] == after[k][x] for k in before for x in keys)
            res["state_as_expected"] = same
            t = time.time()
            out, err = c.ctl("move", slot, c.group_of(dst), timeout=300)
            res["resume"] = out
            res["resume_s"] = round(time.time() - t, 2)
            s = load.wait()
            res["load"] = {k: s.get(k) for k in ("rc", "acked", "replayed", "unknown", "moved", "migrating", "failovers",
                                                  "resends", "max_resend_age_ms", "epoch", "error")}
            ok = settle_and_check(c, ids, res)
            res["placement"] = c.placement()
            ok = ok and same and out.startswith("DONE") and s["rc"] == 0 and int(s["epoch"]) == 2
            res["pass"] = ok
            return res
        finally:
            if res.get("diag_stop"):
                res.pop("diag_stop").set()
                time.sleep(0.6)
            if load:
                load.stop()
            keep(c, f"q10_{step}", res)
            c.stop()
    return run


def keep(c, name, res):
    """On a failure, every node's log and the client's, as evidence."""
    if not JSON_DIR or (res.get("pass") and not any(f.startswith("zkstack") for f in os.listdir(c.work))):
        return
    os.makedirs(JSON_DIR, exist_ok=True)
    with open(f"{JSON_DIR}/{name}_logs.txt", "w") as f:
        for fn in sorted(os.listdir(c.work)):
            if fn.endswith(".log"):
                f.write(f"==== {fn} ====\n" + open(f"{c.work}/{fn}", errors="replace").read()[-30000:] if not fn.startswith("z") else open(f"{c.work}/{fn}", errors="replace").read() + "\n")


SCEN = {"q2_lost_move": q2_lost_move}
for _s in ("copied", "sealed", "staged", "committed", "activated"):
    SCEN[f"q10_{_s}"] = q10(_s)


def main():
    names = SCENARIOS or list(SCEN)
    failed = []
    for name in names:
        work = tempfile.mkdtemp(prefix=f"snc-bm-{name}-")
        t = time.time()
        try:
            r = SCEN[name](work)
        except Exception as e:  # noqa: BLE001
            r = {"pass": False, "error": f"{type(e).__name__}: {e}"}
        r["scenario"], r["seconds"] = name, round(time.time() - t, 1)
        print(("PASS " if r.get("pass") else "FAIL ") + name + " " + json.dumps(r)[:3000], flush=True)
        if JSON_DIR:
            os.makedirs(JSON_DIR, exist_ok=True)
            with open(f"{JSON_DIR}/{name}.json", "w") as f:
                json.dump(r, f, indent=1)
        if not r.get("pass"):
            failed.append(name)
        shutil.rmtree(work, ignore_errors=True)
    print("test_baseline_move:", "FAILED " + str(failed) if failed else "OK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
