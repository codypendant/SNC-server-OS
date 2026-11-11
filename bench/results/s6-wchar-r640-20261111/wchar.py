#!/usr/bin/env python3
"""S6 write-window causal characterization (docs/handoff/PR104_S6_Result_Review.md,
"NEXT AUTHORIZED TASK"; PREREG.md here). Result-only, diagnostic. It is NOT
an S6 run and judges no S6 rule.

Cells W1 and W2 are exactly S6's P cell with one change, `--ckpt-window 1`
or `--ckpt-window 2` appended to the current server's command. Everything
else is bench/s6.py's frozen code, imported unchanged:
- preflight and postflight identity (machine, storage, binaries, isolation,
  clean tree);
- commands, the process driver, the 100 ms sampler and the disk observer;
- the checkpoint-window proof and the VOID/rerun policy.

The data root is /srv/snc-data/s6w (the same SSD) so S6's own path stays
untouched. W4 is the frozen S6 P reps, extracted read-only, never rerun.

  wchar.py run OUT --scratch DIR   # the 6 reps (W1, W2 rep-major, 3 each)
  wchar.py w4 S6_OUT               # metrics of S6's P reps, same extraction
  wchar.py table OUT S6_OUT        # the comparison table W1 / W2 / W4
"""
import json, os, re, sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "bench"))
import s6  # noqa: E402  (frozen at runner commit 63440b1; not modified)

WINDOWS = {"W1": 1, "W2": 2}
REPS = 3
SHARDS = s6.SHARDS
GAUGES = ["ckpt_write_lat_ewma_us", "ckpt_write_lat_max_us", "ckpt_window_full_steps", "ckpt_chunks",
          "ckpt_window", "ckpt_last_snapshot_us", "ckpt_last_walk_us", "ckpt_last_total_us",
          "ckpt_catchup_steps", "ckpt_step_max_ns", "emits_peak", "wal_flush_ewma_us", "wal_flush_max_us"]


def setup():
    """The only changes to s6's module state, all in this process."""
    for cell in WINDOWS:
        s6.CELLS[cell] = s6.CELLS["P"]          # ("current", 436646, True): exactly P
    s6.ORDER = list(WINDOWS)                     # rep-major: W1, W2 in each rep
    s6.REPS = REPS
    s6.DATA_ROOT = "/srv/snc-data/s6w"
    base = s6.commands

    def commands(cell, rep, port, admin, scratch, frozen):
        c = base(cell, rep, port, admin, scratch, frozen)
        c["server"] = c["server"] + ["--ckpt-window", str(WINDOWS[cell])]
        return c
    s6.commands = commands


def per_kind(client_log):
    """{kind: {p99, p999, max}} from net_bench's run section (after '--- run')."""
    run = client_log.split("--- run", 1)[-1]
    out = {}
    for kind in ("total", "read", "deposit", "transfer", "index"):
        m = re.search(rf"^\s+{kind} latency ns\s+n (\d+)\s+p50 \d+\s+p95 \d+\s+p99 (\d+)\s+p99\.9 (\d+)\s+max (\d+)",
                      run, re.M)
        out[kind] = None if not m else dict(n=int(m[1]), p99=int(m[2]), p999=int(m[3]), max=int(m[4]))
    return out


def rep_metrics(raw, tag, rec):
    """Everything PREREG.md section 4 lists for one rep, from its raw files."""
    fg = s6.parse_kv(open(os.path.join(raw, tag + ".final_gauges.txt")).read().strip())
    shard = {g: s6.shard_values(fg, g) for g in GAUGES}
    c = rec["client"]
    late = c.get("late_over_1ms")
    sched = c.get("scheduled_in_window")
    return dict(
        tag=tag, cell=rec["cell"], rep=rec["rep"], void=rec.get("void"),
        kinds=per_kind(open(os.path.join(raw, tag + ".client.log")).read()),
        overloaded=c.get("overloaded"), index_refused=c.get("index_refused"),
        late_over_1ms=late, scheduled_in_window=sched,
        late_fraction=None if not sched else str(Fraction(late, sched)),
        errors=c.get("errors"), stale=c.get("stale"), index_errors=c.get("index_errors"), drained=c.get("drained"),
        client_rc=rec.get("client_rc"), server_rc=rec.get("server_rc"),
        checkpoints=[(o, str(t)) for o, t in rec.get("checkpoints", [])],
        controller=rec.get("controller"), shards=shard)


def run(out, scratch):
    setup()
    frozen = json.load(open(s6.FROZEN_PATH))
    if os.path.exists(out):
        raise SystemExit(f"{out} exists: the characterization runs once")
    os.makedirs(os.path.join(out, "raw"))
    os.makedirs(scratch, exist_ok=True)
    os.makedirs(s6.DATA_ROOT, exist_ok=True)
    ok, rec = s6.preflight(out, scratch, frozen, real=False)   # real=False: no S6 GO applies; identity checks do
    json.dump(rec, open(os.path.join(out, "preflight.json"), "w"), indent=1, default=str)
    if not ok:
        raise SystemExit("REFUSED before any run: " + "; ".join(rec["problems"]))
    d = s6.Driver(out, scratch, frozen, rec["machine_values"]["data_disk"])

    def post():
        mc, mv = s6.machine_identity(frozen, set(rec["disks"]["data"]))
        bc, bv = s6.build_identity(frozen)
        json.dump(dict(machine_checks=mc, machine_values=mv, build_checks=bc, build_values=bv),
                  open(os.path.join(out, "postflight.json"), "w"), indent=1, default=str)
        why = [f"machine {k}" for k, v in mc.items() if not v] + [f"build {k}" for k, v in bc.items() if not v]
        if mv != rec["machine_values"]:
            why.append("machine values changed")
        return not why, why

    reps, void = s6.run_session(d.execute, post)
    res = [rep_metrics(os.path.join(out, "raw"), r["tag"], r) for r in reps if r.get("tag") and not r.get("why")]
    json.dump(dict(session_void=void, reps=res), open(os.path.join(out, "char.json"), "w"), indent=1, default=str)
    print("SESSION", "VOID: " + "; ".join(void) if void else "complete", flush=True)
    return 0


def w4(s6_out):
    recs = [r for r in json.load(open(os.path.join(s6_out, "summary.json"))) if r["cell"] == "P"]
    for r in recs:
        r["checkpoints"] = [(o, Fraction(t)) for o, t in r["checkpoints"]]
    return [rep_metrics(os.path.join(s6_out, "raw"), r["tag"], r) for r in recs]


def table(out, s6_out):
    rows = json.load(open(os.path.join(out, "char.json")))["reps"] + json.loads(json.dumps(w4(s6_out), default=str))
    ms = lambda ns: f"{ns / 1e6:.2f}"   # noqa: E731
    lines = ["| cell | rep | void | total p99 | total p99.9 | max | read p99.9 | deposit p99.9 | transfer p99.9 | "
             "index p99.9 | refusals | late % | WAL EWMA peak | WAL>high worst | write EWMA (shards) | "
             "write max (shards) | window-full steps (shards) | chunks | snapshot / walk / total (s0) |",
             "|" + "---|" * 19]
    for r in sorted(rows, key=lambda r: (r["cell"], r["rep"])):
        k, sh, cs = r["kinds"], r["shards"], r["controller"] or {}
        rng = lambda g: f"{min(x for x in sh[g] if x is not None)}-{max(x for x in sh[g] if x is not None)}"  # noqa: E731
        wh = cs.get("wal_above_high") or {}
        lines.append(
            f"| {r['cell'] if r['cell'] != 'P' else 'W4 (S6 P)'} | {r['rep']} | {r['void'] or ''} | "
            f"{ms(k['total']['p99'])} | {ms(k['total']['p999'])} | {ms(k['total']['max'])} | "
            f"{ms(k['read']['p999'])} | {ms(k['deposit']['p999'])} | {ms(k['transfer']['p999'])} | "
            f"{ms(k['index']['p999'])} | {r['overloaded']}/{r['index_refused']} | "
            f"{float(Fraction(r['late_fraction'])) * 100:.3f} | {cs.get('wal_ewma_peak_us')} us | "
            f"{wh.get('max_shard_ms', 0) / 1000:.1f} s | {rng('ckpt_write_lat_ewma_us')} us | "
            f"{rng('ckpt_write_lat_max_us')} us | {rng('ckpt_window_full_steps')} | {rng('ckpt_chunks')} | "
            f"{sh['ckpt_last_snapshot_us'][0] / 1000:.1f} ms / {sh['ckpt_last_walk_us'][0] / 1e6:.2f} s / "
            f"{sh['ckpt_last_total_us'][0] / 1e6:.2f} s |")
    print("\n".join(lines))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["run"] and len(a) == 4 and a[2] == "--scratch":
        sys.exit(run(a[1], a[3]))
    if a[:1] == ["w4"] and len(a) == 2:
        print(json.dumps(w4(a[1]), indent=1, default=str))
        sys.exit(0)
    if a[:1] == ["table"] and len(a) == 3:
        table(a[1], a[2])
        sys.exit(0)
    raise SystemExit(__doc__)
