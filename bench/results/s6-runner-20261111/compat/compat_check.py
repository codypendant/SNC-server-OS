#!/usr/bin/env python3
"""NOT S6 EVIDENCE. Compatibility smoke check required before S6 GO
(docs/handoff/PR99_S6_Pacing_CLI_Final_Review.md, "Capacity comparator rule"):
the current net_bench drives the 389b623 snc_server on the single-node
Profile-A workload (5,000,000 objects, the S6 mix, owner routing, 8 shards,
8 client threads), through bench/s6.py's own Driver.

Shortened so it is no measurement: a 5 s window and a checkpoint every 8 s
so one checkpoint happens, data on the boot disk (not the data SSD).
Runs C0 (pre-S1 server) and C (current server), one rep each.

  compat_check.py OUT
"""
import json, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "bench"))
import s6

out = os.path.abspath(sys.argv[1])
os.makedirs(os.path.join(out, "raw"), exist_ok=True)
s6.WINDOW_S, s6.CHECKPOINT_S = 5, 8
s6.DATA_ROOT = os.path.join(out, "data")          # boot disk, not /srv/snc-data
frozen = json.load(open(s6.FROZEN_PATH))
ok_b, got = s6.build_identity(frozen)
assert all(ok_b.values()), got
d = s6.Driver(out, "/dev/shm/snc-s6-compat", frozen, "sda")
os.makedirs("/dev/shm/snc-s6-compat", exist_ok=True)
res = {}
for cell in ("C0", "C"):
    r = d.execute(cell, 0)
    cs = r.get("controller") or {}
    res[cell] = dict(
        void=r.get("void"), why=r.get("why"), server=r["server_cmd"][r["server_cmd"].index("--shards") - 1],
        net_bench=r["run_cmd"][r["run_cmd"].index("--port") - 1], client=r.get("client"),
        correctness_ok=s6.correctness_ok(r, open_loop=False), checkpoints=[(o, str(s)) for o, s in r.get("checkpoints", [])],
        final_emits_peak=r.get("final_emits_peak"), final_degraded=r.get("final_degraded"),
        samples=cs.get("samples"), failed_samples=cs.get("failed_samples"), max_interval_ms=cs.get("max_interval_ms"),
        catchup=cs.get("catchup"), walk_budget_ns_range=cs.get("walk_budget_ns_range"),
        wal_above_high=cs.get("wal_above_high"), wal_ewma_peak_us=cs.get("wal_ewma_peak_us"),
        backlog_above_25pct=cs.get("backlog_above_25pct"))
os.rmdir("/dev/shm/snc-s6-compat")
json.dump(dict(build=got, cells=res), open(os.path.join(out, "compat.json"), "w"), indent=1, default=str)
print(json.dumps(res, indent=1, default=str))
