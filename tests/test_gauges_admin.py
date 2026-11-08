#!/usr/bin/env python3
"""S0 observability, admin side (docs/CHECKPOINT_BACKPRESSURE_DESIGN.md 11).

One real snc_server (2 shards, durable, --checkpoint-s 1, --debug-admin)
under a short net_bench load.

  T8a HEALTH keeps every key it had, in the same order, as its prefix,
      and appends the checkpoint keys (ckpt, ckpt_age_ms, ckpt_last_ms,
      ckpt_done). Every token is key=value, so old parsers that read keys
      are unaffected.
  T8b GAUGES answers one key=value line with every published gauge of every
      shard; GAUGES 1 only shard 1's; GAUGES 9 is refused ("NO ...").
      STATUS and RESOURCES keep their keys.
  T8c GAUGES and HEALTH answer while a shard is stalled (DEBUG STALL 0
      2000): they read published values and never wait on a shard.

Usage: test_gauges_admin.py BUILD_DIR
"""
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
SANITIZED = bool(os.environ.get("SNC_SANITIZED"))

# Must match include/snc/gauges.h kGaugeNames.
GAUGE_NAMES = """refused_followup refused_ring refused_waiter refused_wal_buffer gather_overloaded
waiters_now waiters_peak waiter_bytes_now waiter_bytes_peak emits_now emits_peak
wal_present wal_direct wal_active_bytes_now wal_active_bytes_peak wal_buffer_cap_bytes
wal_next_lsn wal_durable_lsn wal_undurable wal_flushes wal_flush_ewma_us wal_flush_max_us wal_flush_since_ns
ckpt_phase ckpt_seq ckpt_started_ns ckpt_phase_since_ns ckpt_objects ckpt_bytes_issued ckpt_bytes_written
ckpt_writes_inflight ckpt_writes_inflight_peak ckpt_last_snapshot_us ckpt_last_walk_us ckpt_last_drain_us
ckpt_last_durable_wait_us ckpt_last_sync_us ckpt_last_total_us ckpt_last_result ckpt_completed
refused_pending pending_now pending_peak refused_replay
published_ns""".split()
DERIVED = "ckpt_phase_name ckpt_age_ms ckpt_phase_age_ms wal_flush_age_ms committed_start_lsn wal_replay_records consistent".split()
HEALTH_OLD = "role term uuid epoch leases shards stall_ms stall_shard main_ms adm_q adm_q_hw adm_busy adm_half adm_half_hw adm_evicted adm_timeouts".split()
HEALTH_NEW = "ckpt ckpt_age_ms ckpt_last_ms ckpt_done".split()
RESOURCES_KEYS = ("rss_kb hwm_kb heap_live waiters overflow emits_in_flight emits_held repl_retained wal_unacked "
                  "wal_undurable blocked degraded disk_readers disk_queued disk_queued_hwm disk_started "
                  "disk_stopped_on_cancel ckpt_running ckpts").split()


def log(*a):
    print(*a, flush=True)


def free_block(n):
    rnd = random.Random(os.getpid() * 7919 + time.time_ns())
    while True:
        base = rnd.randrange(20000, 31000)
        socks, ok = [], True
        for i in range(n):
            s = socket.socket()
            try:
                s.bind(("127.0.0.1", base + i))
            except OSError:
                ok = False
            socks.append(s)
        for s in socks:
            s.close()
        if ok:
            return base


def ask(port, line, timeout=5):
    with socket.create_connection(("127.0.0.1", port), timeout) as s:
        s.settimeout(timeout)
        s.sendall((line + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        return buf.decode().strip()


def kv(line):
    out = []
    for tok in line.split():
        assert "=" in tok, f"token without '=': {tok!r} in {line!r}"
        k, v = tok.split("=", 1)
        out.append((k, v))
    return out


def wait_port(port, proc, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return True
        except OSError:
            if proc.poll() is not None:
                return False
            time.sleep(0.05)
    return False


def main():
    work = tempfile.mkdtemp(prefix="snc-gauges-admin-")
    base = free_block(4)
    cli, adm = base, base + 3  # client ports base, base+1 (2 shards)
    srv = subprocess.Popen([f"{BUILD}/snc_server", "--shards", "2", "--port", str(cli), "--data-dir", f"{work}/d",
                            "--admin-port", str(adm), "--objects", "100000", "--checkpoint-s", "1",
                            "--debug-admin"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        assert wait_port(adm, srv), "snc_server did not start"
        r = subprocess.run([f"{BUILD}/net_bench", "--port", str(cli), "--threads", "2", "--objects", "20000",
                            "--seconds", "1", "--reads", "50"], capture_output=True, text=True, timeout=300)
        assert r.returncode == 0, r.stdout + r.stderr
        # Wait for at least one timed checkpoint after the load.
        t0 = time.time()
        while True:
            h = dict(kv(ask(adm, "HEALTH")))
            if int(h["ckpt_done"]) >= 2:
                break
            assert time.time() - t0 < 60, h
            time.sleep(0.2)

        # T8a
        line = ask(adm, "HEALTH")
        keys = [k for k, _ in kv(line)]
        # The old keys are still the line's prefix, in order; the new ones follow.
        assert keys[:len(HEALTH_OLD)] == HEALTH_OLD, f"HEALTH old keys changed: {keys}"
        assert keys[len(HEALTH_OLD):len(HEALTH_OLD) + len(HEALTH_NEW)] == HEALTH_NEW, line
        h = dict(kv(line))
        assert h["ckpt"] in ("idle", "shards", "commit", "reclaim"), h
        log(f"T8a HEALTH: old keys unchanged, new {HEALTH_NEW}: {line}")

        # T8b
        g = dict(kv(ask(adm, "GAUGES")))
        assert g["shards"] == "2", g
        for s in range(2):
            for name in GAUGE_NAMES + DERIVED:
                assert f"s{s}.{name}" in g, f"GAUGES lacks s{s}.{name}"
            assert g[f"s{s}.consistent"] == "1", g
            assert g[f"s{s}.wal_present"] == "1", g
            assert g[f"s{s}.wal_direct"] in ("0", "1"), g
            assert int(g[f"s{s}.ckpt_completed"]) >= 2, g
            assert g[f"s{s}.ckpt_last_result"] == "1", g
            assert int(g[f"s{s}.ckpt_bytes_written"]) == int(g[f"s{s}.ckpt_bytes_issued"]) or \
                g[f"s{s}.ckpt_phase_name"] != "idle", g
            assert int(g[f"s{s}.waiters_peak"]) > 0, g
        assert int(g["ckpt_done"]) >= 2, g
        n_keys = len(g)
        g1 = dict(kv(ask(adm, "GAUGES 1")))
        assert all(not k.startswith("s0.") for k in g1) and "s1.wal_direct" in g1, g1
        bad = ask(adm, "GAUGES 9")
        assert bad.startswith("NO "), bad
        st = dict(kv(ask(adm, "STATUS")))
        assert "role" in st and "shards" in st and "unproven" in st, st
        res = [k for k, _ in kv(ask(adm, "RESOURCES"))]
        assert res == RESOURCES_KEYS, res
        log(f"T8b GAUGES: {n_keys} keys (wal_direct s0={g['s0.wal_direct']}, ckpt_last_ms={g['ckpt_last_ms']}); "
            f"GAUGES 1 scoped; GAUGES 9 refused; STATUS and RESOURCES unchanged")

        # T8c
        assert ask(adm, "DEBUG STALL 0 2000").startswith("OK")
        time.sleep(0.2)  # the stall is running on shard 0's thread
        lat = []
        for _ in range(10):
            for verb in ("GAUGES", "HEALTH"):
                t = time.time()
                rep = ask(adm, verb, 2)
                lat.append((verb, time.time() - t))
                assert rep and "=" in rep, rep
            time.sleep(0.05)
        worst = max(d for _, d in lat)
        st_ms = int(dict(kv(ask(adm, "HEALTH")))["stall_ms"])
        log(f"T8c during a 2000 ms stall of shard 0: GAUGES/HEALTH worst {worst * 1000:.1f} ms, stall_ms {st_ms}")
        if not SANITIZED:
            assert worst < 0.5, lat
        time.sleep(2.0)
        log("test_gauges_admin: all passed")
    finally:
        srv.kill()
        srv.wait()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
