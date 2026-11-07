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
               Drained is judged on the published gauges over the last 5 s
               (drain_rule below), not on one RESOURCES sample.
  h1_measurement  Self-test of that measurement (no server): summing per-
               shard counts read one after another can exceed the clients'
               window though no instant does; drain_rule passes a drained
               but sync-bound series and fails blocked, stalled, growing
               and too-short ones.
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
                          "--failover-ms", "500", "--poll-ms", "50"] +
                         ([] if os.path.exists(f"{self.work}/witness.state") else ["--new-state", "1"]))
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
        self.health, self.res, self.gauges = [], [], []
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
            g = af.ask(self.port, "GAUGES", 2)
            if g:
                self.gauges.append((round(time.time() - self.t0, 2), gauge_sums(kv(g))))
            time.sleep(self.period)

    def stop(self):
        self.stop_ = True
        self.t.join(timeout=10)


def probe_writes(port, oid, until, out):
    """One durable deposit at a time until `until`: (t, ms, status) each."""
    while time.time() < until:
        t = time.time()
        st = deposit(port, oid, 3)  # 1 cent; 3 s timeout
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
            # Probe deposits acknowledged with the standby in sync (HA_DURABLE).
            pre = []
            probe_writes(g.p_cli, probe_id, time.time() + 1.0, pre)
            time.sleep(1.0 + 0.37 * (trial % 3))  # checkpoints running; vary the phase
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
            # main_ms is how long ago the main loop last turned. It also
            # turns slowly while it cuts a checkpoint (Engine::checkpoint runs
            # on the main loop, every 1 s here; ~1.3 s per cut under TSan), so
            # one sample can land inside a cut. Take the smallest of several
            # over 3 s: a wedged loop stays high in all of them.
            hs = []
            for _ in range(15):
                hk = kv(af.ask(g.p_adm, "HEALTH", 2))
                if "main_ms" in hk:
                    hs.append(int(hk["main_ms"]))
                time.sleep(0.2)
            h = {"main_ms": min(hs)} if hs else {}
            st = af.ask(g.p_adm, "STATUS", 3)
            res = af.ask(g.p_adm, "RESOURCES", 3)
            resume = first_ok_after(writes, t_kill + 0.6)
            # Every acknowledged probe deposit (1 cent each, from 1,000,000),
            # before the kill (HA_DURABLE) and after (under the grant), is
            # there; an unacknowledged one may or may not be.
            okn = sum(1 for _, _, s in pre + writes if s == fc.OK)
            tried = len(pre) + len(writes)
            try:
                bst, bal = fc.balance(g.p_cli, probe_id)
            except (OSError, RuntimeError):
                bst, bal = None, None  # no answer: a wedged primary
            # And after a crash and restart, recovered from the checkpoints
            # cut on local durability plus the WAL: the same balance.
            bal_restart = None
            if bst == fc.OK:
                g.prim.signal(signal.SIGKILL)
                g.prim.p.wait(timeout=30)
                g.start_primary()
                t_end = time.time() + 30
                while time.time() < t_end and bal_restart is None:
                    try:
                        rst, rb = fc.balance(g.p_cli, probe_id)
                        if rst == fc.OK:
                            bal_restart = rb
                    except (OSError, RuntimeError, AssertionError):
                        pass
                    time.sleep(0.2)
            r = {"trial": trial, "granted": granted is not None, "main_ms_after_15s": int(h["main_ms"]) if "main_ms" in h else None,
                 "main_ms_samples": hs,
                 "status": st, "resources": res,
                 "writes_ok_after_kill": sum(1 for _, _, s in writes if s == fc.OK),
                 "writes_failed_after_kill": sum(1 for _, _, s in writes if s != fc.OK),
                 "resume_s": round(resume - t_kill, 2) if resume else None,
                 "checkpoint_in_flight_at_kill": True, "ckpts_at_kill": int(ckpt_at_kill.get("ckpts", 0)),
                 "ckpts_after": int(kv(res).get("ckpts", 0)) if res else None,
                 "probe_ok": okn, "probe_tried": tried, "probe_ok_before_kill": sum(1 for _, _, s in pre if s == fc.OK),
                 "probe_balance": bal if bst == fc.OK else None, "probe_balance_after_restart": bal_restart,
                 "acked_writes_kept": bst == fc.OK and 1000000 + okn <= bal <= 1000000 + tried
                 and bal_restart == bal,
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


def gauge_sums(g):
    """One GAUGES reply (S0 published blocks, read without running a task on
    any shard). Each shard's block is one consistent copy taken at its own
    publish instant, so per-shard differences are coherent; sums across
    shards are not a snapshot (skew_ms: how far apart the instants were).

    excess: answers held beyond local durability, summed per shard as
    max(0, waiters_now - wal_undurable). Each q4 deposit is one record with
    one waiting answer, so while answers wait only for the local WAL,
    waiters <= undurable on every shard; answers held for the standby wait
    on records that are already durable here, and show as excess."""
    n = int(g.get("shards", 0))
    pub = [int(g.get(f"s{s}.published_ns", 0)) for s in range(n)]
    w = [int(g.get(f"s{s}.waiters_now", 0)) for s in range(n)]
    u = [int(g.get(f"s{s}.wal_undurable", 0)) for s in range(n)]
    return {
        "waiters": sum(w),
        "undurable": sum(u),
        "excess": sum(max(0, a - b) for a, b in zip(w, u)),
        "durable_lsn": sum(int(g.get(f"s{s}.wal_durable_lsn", 0)) for s in range(n)),
        "shard_waiters": w,
        "shard_durable_lsn": [int(g.get(f"s{s}.wal_durable_lsn", 0)) for s in range(n)],
        "skew_ms": round((max(pub) - min(pub)) / 1e6, 1) if n and min(pub) > 0 else None,
        "consistent": n > 0 and all(g.get(f"s{s}.consistent") == "1" for s in range(n)),
    }


# drain_rule, preregistered for q4 (docs/handoff/PR72_S1_Review.txt 9).
#
# What "drained" means: after the grant, no client answer is held behind
# the stopped standby; answers wait only for the primary's own WAL, which
# keeps syncing. It does NOT mean few answers wait: under a closed-loop
# load bounded by sync speed, nearly every client slot is waiting for the
# next local sync at any instant, so the count of waiters sits near the
# clients' window both when drained and when blocked. What differs is what
# they wait for. Blocked, answers wait on records already durable here
# (excess > 0, about the window); drained, they wait on undurable records
# (excess 0).
#
# Rule, over the observation interval (the last DRAIN_WINDOW_S seconds, at
# least DRAIN_MIN_SAMPLES consistent gauge samples):
#   drained   = median excess == 0, at most DRAIN_MAX_EXCESS_FRAC of the
#               samples show any excess, and the durable LSN advanced on
#               EVERY shard that had waiting answers in the interval (one
#               shard whose own sync stalls shows no excess; the others'
#               progress must not hide it);
#   bounded   = the summed undurable count has median <= the window and
#               does not grow: its last-half mean <= first-half mean + 10%
#               of the window.
# One sample is never a verdict (see h1_measurement).
DRAIN_WINDOW_S = 5.0
DRAIN_MIN_SAMPLES = 10            # sampler runs ~10/s in Release; >= 2/s under sanitizers
DRAIN_MAX_EXCESS_FRAC = 0.2


def median(vals):
    v = sorted(vals)
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2


def drain_rule(samples, window):
    """samples: gauge_sums dicts in time order."""
    n = len(samples)
    if n < DRAIN_MIN_SAMPLES:
        return {"n": n, "drained": False, "bounded": False, "why": "too few samples"}
    ex = [r["excess"] for r in samples]
    und = [r["undurable"] for r in samples]
    with_excess = sum(1 for e in ex if e > 0)
    half = n // 2
    first, last = sum(und[:half]) / half, sum(und[half:]) / (n - half)
    first_s, last_s = samples[0], samples[-1]
    if "shard_durable_lsn" in first_s:
        ns = len(first_s["shard_durable_lsn"])
        busy = [i for i in range(ns) if any(r["shard_waiters"][i] > 0 for r in samples)]
        stalled = [i for i in busy if last_s["shard_durable_lsn"][i] <= first_s["shard_durable_lsn"][i]]
        progressed = bool(busy) and not stalled
    else:
        stalled = []
        progressed = last_s["durable_lsn"] > first_s["durable_lsn"]
    out = {"n": n, "excess_median": median(ex), "excess_max": max(ex),
           "frac_with_excess": round(with_excess / n, 3), "durable_progressed": progressed, "stalled_shards": stalled,
           "undurable_median": median(und), "undurable_max": max(und),
           "undurable_first_half_mean": round(first, 1), "undurable_last_half_mean": round(last, 1)}
    out["drained"] = median(ex) == 0 and with_excess / n <= DRAIN_MAX_EXCESS_FRAC and progressed
    out["bounded"] = median(und) <= window and last <= first + 0.1 * window
    return out


def scenario_h1_measurement(work):
    """No server. Two shards keep exactly WINDOW requests in flight between
    them; requests finish on one shard and new ones start on the other.
    Reading the shards one after another, as RESOURCES does, can sum to more
    than WINDOW though the true total never does; and a single sample near
    the window says nothing about whether answers are held."""
    window = 256
    out = {}
    # 1. The old method's impossible sum, deterministically.
    s = [200, 56]                      # true total 256
    read0 = s[0]                       # RESOURCES reads shard 0 ...
    s[0] -= 30                         # ... 30 answers leave shard 0, 30 new requests reach shard 1
    s[1] += 30
    read1 = s[1]                       # ... then reads shard 1
    out["sequential_sum"] = read0 + read1
    out["true_total"] = sum(s)

    def sample(i, waiters, undurable, lsn1=None):
        # Two shards, the load split evenly; shard 1's durable LSN can be pinned.
        w = [waiters // 2, waiters - waiters // 2]
        u = [undurable // 2, undurable - undurable // 2]
        lsn = [1000 + 25 * i, 1000 + 25 * i if lsn1 is None else lsn1]
        return {"waiters": waiters, "undurable": undurable,
                "excess": sum(max(0, a - b) for a, b in zip(w, u)),
                "durable_lsn": sum(lsn), "shard_waiters": w, "shard_durable_lsn": lsn,
                "skew_ms": 0.3, "consistent": True}

    # 2. Drained but sync-bound: waiters near the window, every one on an
    # undurable record; the last sample at 270 (the failed ASan run's shape).
    drained = [sample(i, 240 + i % 17, 240 + i % 17) for i in range(49)] + [sample(49, 270, 270)]
    # 3. Blocked: the same counts, but the records are already durable here.
    blocked = [sample(i, 256, i % 9) for i in range(50)]
    # 4. Stalled: no excess, but nothing becomes durable.
    stalled = [dict(sample(i, 256, 256), durable_lsn=2000, shard_durable_lsn=[1000, 1000]) for i in range(50)]
    # 5. Growing undurable backlog.
    growing = [sample(i, 3 * i, 3 * i) for i in range(50)]
    # 6. One shard's own sync stalls: no excess there, the summed LSN still rises.
    one_stalled = [sample(i, 250, 250, lsn1=1000) for i in range(50)]
    out["old_rule_on_drained"] = drained[-1]["waiters"] < window       # one end sample
    out["rule"] = {k: drain_rule(v, window) for k, v in
                   (("drained", drained), ("blocked", blocked), ("stalled", stalled), ("growing", growing), ("one_shard_stalled", one_stalled),
                    ("short", drained[:5]))}
    r = out["rule"]
    out["checks"] = {
        "sequential_sum_exceeds_window": out["sequential_sum"] > window and out["true_total"] == window,
        "old_rule_fails_drained_series": out["old_rule_on_drained"] is False,
        "rule_passes_drained_series": r["drained"]["drained"] and r["drained"]["bounded"],
        "rule_fails_blocked_series": r["blocked"]["drained"] is False,
        "rule_fails_stalled_series": r["stalled"]["drained"] is False,
        "rule_fails_growing_series": r["growing"]["bounded"] is False,
        "rule_fails_one_shard_stalled": r["one_shard_stalled"]["drained"] is False,
        "rule_needs_enough_samples": r["short"]["drained"] is False,
    }
    out["pass"] = all(out["checks"].values())
    return out


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
        # Drained (drain_rule): over the last 5 s of published gauges, the
        # held answers and the not-yet-durable records are not pinned at the
        # clients' window (while blocked they sit at it); nothing waits
        # behind full rings, no follow-up is held. The last RESOURCES sample
        # is kept for the record only. Memory: what still grows in the last
        # 5 s is the capped replication retention (it may still be filling on
        # a slow host): at most RSS_PER_RETAINED times its rate, plus 5 MiB/s
        # of slack.
        tail = [r for t, r in sampler.gauges if t >= t_end - DRAIN_WINDOW_S and r["consistent"]]
        out["drain"] = drain_rule(tail, window)
        out["drain"]["skew_ms_max"] = max((r["skew_ms"] or 0) for r in tail) if tail else None
        out["drain"]["end_sample_waiters"] = end["waiters"]
        out["drain"]["end_sample_undurable"] = end["wal_undurable"]
        blocked = [r for t, r in sampler.gauges if t_stop + 0.3 <= t <= (t_grant or t_end) and r["consistent"]]
        if len(blocked) >= DRAIN_MIN_SAMPLES:
            out["while_blocked_drain"] = drain_rule(blocked, window)
        out["checks"] = {
            "granted": bool(out["granted"] and t_grant is not None),
            "answers_drained": out["drain"]["drained"],
            "undurable_bounded": out["drain"]["bounded"],
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
    "h1_measurement": scenario_h1_measurement,
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
