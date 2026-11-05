#!/usr/bin/env python3
"""Pre-baseline tests T-Q3 and T-Q4 (docs/EXPERIMENTAL_BASELINE.md section 5)
for Profile B (SYNC HA): one group of a sync primary and its standby, one
snc_witness (--failover-ms 500, --poll-ms 50), real processes.

  q3_runtime   T-Q3, run time: the primary checkpoints every second under
               durable write load; the standby is SIGKILLed. Expected (the
               contract): the primary obtains DEGRADE from the witness,
               writes resume, and HEALTH main_ms stays small. A primary
               whose main thread is held by a checkpoint waiting for
               durability that needs the dead standby cannot ask for the
               grant: that is reported as WEDGED, with the main thread's
               stack (gdb) kept as evidence.
  q3_restart   T-Q3, restart (row 19c): the standby is gone; the primary
               restarts with another shard count, which runs a recovery
               checkpoint inside Engine::start. Expected: it starts, obtains
               DEGRADE, and serves writes.
  q4_normal    T-Q4: the standby is SIGSTOPped at full durable write load;
               the witness grants as normal. RESOURCES is sampled every
               100 ms at the primary: RSS, heap, held answers (waiters),
               ring overflow, follow-ups, replication retained bytes and
               unacknowledged WAL. Expected: growth stops at the grant and
               the held answers drain.
  q4_delayed   T-Q4: as q4_normal with the witness SIGSTOPped first, so the
               grant waits DELAY seconds (10, then 60): the growth rate and
               its plateau, and the time to a stated memory limit at that
               rate.

Every scenario prints one JSON line; with --json DIR it is also written
there. The assertions are the contract's; a scenario that finds the
hazard fails with its evidence (blockers are not fixed here: section 11 of
the baseline document).

Usage: test_baseline_ha.py BUILD_DIR [SCENARIO ...] [--json DIR]
"""
import json
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

ARGS = [a for a in sys.argv[1:]]
JSON_DIR = None
if "--json" in ARGS:
    i = ARGS.index("--json")
    JSON_DIR = ARGS[i + 1]
    del ARGS[i:i + 2]
BUILD = ARGS[0] if ARGS else "build"
SCENARIOS = ARGS[1:]
sys.argv = [sys.argv[0], BUILD]
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_async_failover as af  # noqa: E402
import test_fail_closed as fc  # noqa: E402
import test_health as th  # noqa: E402

# RSS per retained replication byte: sanitizer shadow memory and redzones
# (release measured 1.1x, ASan up to 3x, TSan 3.6x: TSan maps about 4x shadow
# for every application byte it touches).
RSS_PER_RETAINED = 6 if th.sanitizer(BUILD) == "thread" else 3

UUID = "b7" * 16
MEM_LIMIT_KB = 4 * 1024 * 1024  # the stated limit for "time to exhaustion": 4 GiB per process


def kv(line):
    return dict(x.split("=", 1) for x in (line or "").split() if "=" in x)


def deposit(port, oid, timeout=5):
    try:
        st, _, _, _ = fc.request(port, 1, fc.OP_DEPOSIT, fc.oid_bytes(oid), struct.pack("<qQ", 1, 1), timeout)
        return st
    except (OSError, RuntimeError):
        return None


def stack_of(pid):
    """Every thread's stack of a live process (gdb, batch), as evidence."""
    try:
        r = subprocess.run(["gdb", "-batch", "-p", str(pid), "-ex", "thread apply all bt 25"],
                           capture_output=True, text=True, timeout=120)
        return r.stdout[-60000:]
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"(gdb failed: {e})"


class Group:
    def __init__(self, work, shards=2, checkpoint_s=0.0, objects=200000):
        self.work = work
        self.p_cli, self.s_cli, self.repl, self.p_adm, self.s_adm, self.wit = af.free_ports(6)
        self.shards, self.checkpoint_s, self.objects = shards, checkpoint_s, objects
        self.sb = self.prim = self.w = None

    def common(self, shards):
        return ["--space-uuid", UUID, "--objects", str(self.objects), "--shards", str(shards)]

    def start_standby(self):
        self.sb = af.Proc([f"{BUILD}/snc_server", "--port", str(self.s_cli), "--data-dir", f"{self.work}/s",
                           "--admin-port", str(self.s_adm), "--standby", str(self.repl)] + self.common(self.shards))
        assert af.wait_port(self.s_adm), "standby never opened its admin port\n" + self.sb.text()

    def start_primary(self, shards=None, wait=True):
        args = [f"{BUILD}/snc_server", "--port", str(self.p_cli), "--data-dir", f"{self.work}/p",
                "--admin-port", str(self.p_adm), "--replica-of", f"127.0.0.1:{self.repl}",
                "--replica-timeout-ms", "500", "--witness", f"127.0.0.1:{self.wit}",
                "--report-ms", "0"] + self.common(shards or self.shards)
        if self.checkpoint_s:
            args += ["--checkpoint-s", str(self.checkpoint_s)]
        self.prim = af.Proc(args)
        if wait:
            assert af.wait_port(self.p_adm), "primary never opened its admin port\n" + self.prim.text()

    def start_witness(self):
        self.w = af.Proc([f"{BUILD}/snc_witness", "--port", str(self.wit), "--primary", f"127.0.0.1:{self.p_adm}",
                          "--standby", f"127.0.0.1:{self.s_adm}", "--state", f"{self.work}/witness.state",
                          "--failover-ms", "500", "--poll-ms", "50"])
        assert self.w.wait_for(r"primary up", 15), "witness never saw the primary\n" + self.w.text()

    def start(self):
        self.start_standby()
        self.start_primary()
        self.start_witness()

    def stop(self):
        for p in (self.prim, self.sb, self.w):
            if p:
                p.stop()


class Load:
    """net_bench: durable deposits (no reads) at a fixed depth, for `seconds`."""

    def __init__(self, port, ids, objects, seconds, depth=32, threads=2):
        self.p = subprocess.Popen(
            [f"{BUILD}/net_bench", "--port", str(port), "--threads", str(threads), "--objects", str(objects),
             "--seconds", str(seconds), "--route", "owner", "--reads", "0", "--depth", str(depth), "--ids", ids],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    def stop(self):
        if self.p.poll() is None:
            self.p.kill()
        try:
            return self.p.communicate(timeout=10)[0]
        except subprocess.TimeoutExpired:
            return ""


class Sampler:
    """Polls HEALTH and RESOURCES at the primary on a thread (each sample
    timestamped from the scenario's t0)."""

    def __init__(self, port, t0, period=0.1):
        self.port, self.t0, self.period = port, t0, period
        self.health, self.res = [], []
        self.stop_ = False
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self):
        while not self.stop_:
            t = time.time() - self.t0
            h = af.ask(self.port, "HEALTH", 1)
            if h:
                self.health.append((round(t, 2), kv(h)))
            r = af.ask(self.port, "RESOURCES", 2)
            if r:
                self.res.append((round(t, 2), {k: int(v) for k, v in kv(r).items() if v.lstrip("-").isdigit()}))
            time.sleep(self.period)

    def stop(self):
        self.stop_ = True
        self.t.join(timeout=10)


def probe_writes(port, oid, until, out):
    """One durable deposit at a time until `until`: (t, ms, status) each."""
    while time.time() < until:
        t = time.time()
        st = deposit(port, oid, 3)
        out.append((t, round((time.time() - t) * 1000, 1), st))
        time.sleep(0.02)


def first_ok_after(samples, t):
    for ts, ms, st in samples:
        if ts >= t and st == fc.OK:
            return ts + ms / 1000
    return None


def scenario_q3_runtime(work, trials=3):
    out = {"trials": []}
    for trial in range(trials):
        g = Group(f"{work}/t{trial}", checkpoint_s=1.0, objects=400000)
        os.makedirs(g.work)
        load = None
        try:
            g.start()
            ids = f"{g.work}/ids"
            err, txt = af.bench_ids(g.p_cli, ids, 100000)
            assert err == 0, txt
            probe_id = fc.create_here(g.p_cli)
            load = Load(g.p_cli, ids, 100000, 60)
            time.sleep(2.0 + 0.37 * (trial % 3))  # checkpoints running; vary the phase
            # Kill the standby while a checkpoint is in flight (RESOURCES
            # ckpt_running, set around Engine::checkpoint on the main loop).
            ckpt_at_kill, t_look = None, time.time() + 10
            while time.time() < t_look:
                rk = kv(af.ask(g.p_adm, "RESOURCES", 1))
                if rk.get("ckpt_running") == "1":
                    ckpt_at_kill = rk
                    break
                time.sleep(0.002)
            assert ckpt_at_kill, "no checkpoint in flight to kill the standby under"
            t_kill = time.time()
            g.sb.signal(signal.SIGKILL)
            writes = []
            probe_writes(g.p_cli, probe_id, t_kill + 15, writes)
            granted = g.prim.wait_for(r"witness: granted", 0.1)
            h = kv(af.ask(g.p_adm, "HEALTH", 2))
            st = af.ask(g.p_adm, "STATUS", 3)
            res = af.ask(g.p_adm, "RESOURCES", 3)
            resume = first_ok_after(writes, t_kill + 0.6)
            # Every acknowledged probe deposit (1 each, from 1,000,000) is there.
            okn = sum(1 for _, _, s in writes if s == fc.OK)
            try:
                bst, bal = fc.balance(g.p_cli, probe_id)
            except (OSError, RuntimeError):
                bst, bal = None, None  # no answer: a wedged primary
            r = {"trial": trial, "granted": granted is not None, "main_ms_after_15s": int(h["main_ms"]) if "main_ms" in h else None,
                 "status": st, "resources": res,
                 "writes_ok_after_kill": sum(1 for _, _, s in writes if s == fc.OK),
                 "writes_failed_after_kill": sum(1 for _, _, s in writes if s != fc.OK),
                 "resume_s": round(resume - t_kill, 2) if resume else None,
                 "checkpoint_in_flight_at_kill": True, "ckpts_at_kill": int(ckpt_at_kill.get("ckpts", 0)),
                 "ckpts_after": int(kv(res).get("ckpts", 0)) if res else None,
                 "probe_ok": okn, "probe_balance": bal if bst == fc.OK else None,
                 "acked_writes_kept": bst == fc.OK and bal >= 1000000 + okn,
                 "checkpoint_lines": [l for l in g.prim.lines if l.startswith("checkpoint:")][-3:]}
            # No HEALTH answer counts as wedged, not as a small main_ms.
            r["wedged"] = not r["granted"] and (r["main_ms_after_15s"] is None or r["main_ms_after_15s"] > 5000)
            if r["wedged"] or not r["granted"]:
                ev = f"{work}/q3_runtime_t{trial}_stack.txt"
                with open(ev, "w") as f:
                    f.write(stack_of(g.prim.p.pid))
                r["stack_file"] = ev
                r["main_thread_stack"] = main_frames(open(ev).read())
            out["trials"].append(r)
        finally:
            if load:
                load.stop()
            g.stop()
            shutil.rmtree(g.work, ignore_errors=True)  # 20 trials of data dirs fill a disk
    out["wedged_trials"] = sum(t["wedged"] for t in out["trials"])
    # Checkpoints go on after the kill (ckpts grows), so none is wedged.
    out["pass"] = all(t["granted"] and t["resume_s"] is not None and t["main_ms_after_15s"] is not None
                      and t["main_ms_after_15s"] < 1000 and t["acked_writes_kept"] and
                      (t["ckpts_after"] or 0) > t["ckpts_at_kill"] for t in out["trials"])
    return out


def main_frames(text):
    """The frames of the main thread (Thread 1) from a gdb dump, abridged."""
    m = re.search(r"Thread 1 \(.*?\n(.*?)(?:\n\nThread |\Z)", text, re.S)
    frames = (m[1] if m else "").splitlines()
    return [re.sub(r"\s+", " ", f)[:160] for f in frames if f.startswith("#")][:14]


def scenario_q3_restart(work):
    g = Group(f"{work}/r", shards=2)
    os.makedirs(g.work)
    out = {}
    try:
        g.start()
        ids = f"{g.work}/ids"
        err, txt = af.bench_ids(g.p_cli, ids, 20000)
        assert err == 0, txt
        g.sb.signal(signal.SIGKILL)
        g.sb.p.wait(timeout=30)
        time.sleep(1.0)
        g.prim.signal(signal.SIGKILL)  # a crash, so the restart recovers
        g.prim.p.wait(timeout=30)
        t0 = time.time()
        g.start_primary(shards=4, wait=False)
        up = af.wait_port(g.p_adm, 20)
        out["admin_port_open_s"] = round(time.time() - t0, 2) if up else None
        cli = af.wait_port(g.p_cli, 1) if up else False
        granted = g.prim.wait_for(r"witness: granted", 10 if up else 0.1)
        out["granted"] = granted is not None
        st = None
        if up:
            try:
                oid = fc.create_here(g.p_cli)
                st = deposit(g.p_cli, oid, 10)
            except (OSError, RuntimeError, AssertionError) as e:
                st = f"create failed: {e}"
        out["write_status"] = fc.STATUS.get(st, st)
        out["client_port_open"] = bool(cli)
        out["log_tail"] = g.prim.lines[-12:]
        if not up or st != fc.OK:
            ev = f"{work}/q3_restart_stack.txt"
            with open(ev, "w") as f:
                f.write(stack_of(g.prim.p.pid))
            out["stack_file"] = ev
            out["main_thread_stack"] = main_frames(open(ev).read())
        # B1: the recovery checkpoint (shard count 2 -> 4) ran with the
        # standby gone, and the start still completed.
        out["recovery_checkpoint"] = next((l for l in g.prim.lines if l.startswith("recovery: checkpoint")), None)
        out["pass"] = bool(up and out["granted"] and st == fc.OK and out["recovery_checkpoint"])
    finally:
        g.stop()
    return out


def growth(res, t_a, t_b, key):
    pts = [(t, r[key]) for t, r in res if t_a <= t <= t_b and key in r]
    if len(pts) < 2:
        return None
    (ta, va), (tb, vb) = pts[0], pts[-1]
    return {"from": va, "to": vb, "max": max(v for _, v in pts), "rate_per_s": round((vb - va) / max(tb - ta, 1e-3), 1)}


KEYS = ("rss_kb", "heap_live", "waiters", "overflow", "emits_in_flight", "emits_held", "repl_retained",
        "wal_unacked", "wal_undurable")


def scenario_q4(work, delay):
    g = Group(f"{work}/d{delay}", objects=400000)
    os.makedirs(g.work)
    out = {"grant_delay_s": delay}
    load = None
    try:
        g.start()
        ids = f"{g.work}/ids"
        err, txt = af.bench_ids(g.p_cli, ids, 100000)
        assert err == 0, txt
        hold = delay + 30
        t0 = time.time()
        sampler = Sampler(g.p_adm, t0)
        load = Load(g.p_cli, ids, 100000, hold + 10, depth=64, threads=4)
        time.sleep(3.0)
        t_stop = time.time() - t0
        if delay:
            g.w.signal(signal.SIGSTOP)
        g.sb.signal(signal.SIGSTOP)
        if delay:
            time.sleep(delay)
            g.w.signal(signal.SIGCONT)
        granted = g.prim.wait_for(r"witness: granted", 30)
        t_grant = None
        if granted:
            # The grant's log line has no timestamp: take the first RESOURCES
            # sample that shows degraded=1.
            deadline = time.time() + 5
            while time.time() < deadline and t_grant is None:
                for t, r in list(sampler.res):
                    if t > t_stop and r.get("degraded", 0) > 0:
                        t_grant = t
                        break
                time.sleep(0.1)
        # Long enough after the grant for the replication retention to reach
        # its cap (--replica-retain-mib, default 1 GiB per shard) at full load.
        time.sleep(30.0)
        t_end = time.time() - t0
        sampler.stop()
        bench_out = load.stop()
        load = None
        g.sb.signal(signal.SIGCONT)
        res = sampler.res
        out["t_standby_stopped"] = round(t_stop, 2)
        out["t_grant"] = t_grant
        out["granted"] = granted is not None
        before = [r for t, r in res if t < t_stop]
        out["before"] = {k: before[-1].get(k) for k in KEYS} if before else None
        upto = t_grant if t_grant else t_end
        out["while_blocked"] = {k: growth(res, t_stop, upto, k) for k in KEYS}
        out["after_grant"] = ({k: growth(res, t_grant, t_end, k) for k in KEYS} if t_grant else None)
        last = res[-1][1] if res else {}
        out["at_end"] = {k: last.get(k) for k in KEYS}
        rate = (out["while_blocked"].get("rss_kb") or {}).get("rate_per_s") or 0
        rss_end = (out["while_blocked"].get("rss_kb") or {}).get("to") or 0
        out["time_to_4GiB_at_blocked_rate_s"] = (round((MEM_LIMIT_KB - rss_end) / rate) if rate > 0 else None)
        out["bench_tail"] = bench_out.strip().splitlines()[-2:] if bench_out else []
        out["series"] = [(t, {k: r.get(k) for k in ("rss_kb", "waiters", "overflow", "wal_unacked", "repl_retained",
                                                      "blocked", "degraded")}) for t, r in res][::5]
        # The contract: the grant comes; after it the held answers go back to
        # the load's own level (they are bounded by the clients' depth) and
        # memory stops growing: the replication retention is capped per shard
        # (Engine::Config::replica_retain_bytes, 1 GiB by default).
        out["retain_cap_bytes"] = g.shards * (1 << 30)
        out["last_5s"] = {k: growth(res, t_end - 5, t_end, k) for k in ("rss_kb", "repl_retained", "waiters")}
        # Held answers are bounded by what the clients keep in flight:
        # threads x depth (net_bench's --depth is per thread).
        out["client_in_flight_max"] = 4 * 64
        rss_rate = (out["last_5s"]["rss_kb"] or {}).get("rate_per_s", 1e9) * 1024  # bytes/s
        ret_rate = (out["last_5s"]["repl_retained"] or {}).get("rate_per_s", 0)
        end, window = out["at_end"], out["client_in_flight_max"]
        # Drained: the held answers and the not-yet-durable records are back
        # below the clients' window (while blocked they sit at it), nothing
        # waits behind full rings, no follow-up is held. Memory: what still
        # grows in the last 5 s is the capped replication retention (it may
        # still be filling on a slow host): at most RSS_PER_RETAINED times
        # its rate, plus 5 MiB/s of slack.
        out["checks"] = {
            "granted": bool(out["granted"] and t_grant is not None),
            "waiters_drained": end["waiters"] is not None and end["waiters"] < window,
            "undurable_drained": end["wal_undurable"] is not None and end["wal_undurable"] < window,
            "no_overflow": end["overflow"] == 0,
            "no_held_follow_ups": end["emits_held"] == 0,
            "retention_capped": (end["repl_retained"] or 0) <= out["retain_cap_bytes"],
            "memory_growth_is_retention": rss_rate < RSS_PER_RETAINED * max(ret_rate, 0) + 5 * (1 << 20),
        }
        out["pass"] = all(out["checks"].values())
    finally:
        if load:
            load.stop()
        g.stop()
    return out


SCEN = {
    "q3_runtime": scenario_q3_runtime,
    "q3_runtime20": lambda work: scenario_q3_runtime(work, trials=20),
    "q3_restart": scenario_q3_restart,
    "q4_normal": lambda w: scenario_q4(w, 0),
    "q4_delayed10": lambda w: scenario_q4(w, 10),
    "q4_delayed60": lambda w: scenario_q4(w, 60),
}


def main():
    names = SCENARIOS or list(SCEN)
    failed = []
    for name in names:
        work = tempfile.mkdtemp(prefix=f"snc-base-{name}-")
        t = time.time()
        try:
            r = SCEN[name](work)
        except Exception as e:  # noqa: BLE001
            r = {"pass": False, "error": f"{type(e).__name__}: {e}"}
        r["scenario"], r["seconds"] = name, round(time.time() - t, 1)
        line = json.dumps(r)
        print(("PASS " if r.get("pass") else "FAIL ") + name + " " + line[:4000], flush=True)
        if JSON_DIR:
            os.makedirs(JSON_DIR, exist_ok=True)
            with open(f"{JSON_DIR}/{name}.json", "w") as f:
                json.dump(r, f, indent=1)
            for fn in os.listdir(work):
                if fn.endswith("_stack.txt"):
                    shutil.copy(f"{work}/{fn}", f"{JSON_DIR}/{fn}")
        if not r.get("pass"):
            failed.append(name)
        shutil.rmtree(work, ignore_errors=True)
    print("test_baseline_ha:", "FAILED " + str(failed) if failed else "OK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
