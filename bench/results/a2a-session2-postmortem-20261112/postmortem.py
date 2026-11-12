#!/usr/bin/env python3
"""A2-a session-2 postmortem: READ-ONLY analysis of existing telemetry (PR #117 final review).

No benchmark is run and nothing is measured here. Inputs, all already on disk:
  - ../a2a-storage-repro-r640/out/pilot.json, raw/*.server.log, state/*/snapshot.json (committed in PR #117)
  - raw/sa12 here: the sysstat archive for 2026-11-12, copied unchanged (sha256 in raw/sa12.sha256)
Method (stated so the limits are visible, see REPORT.md):
  sysstat sampled every 10 minutes (cron debian-sa1, SADC_OPTIONS "-S DISK"; no frequency, no interrupts).
  A session's load window (A1 start .. A1 end, about 16.4 min, 12 runs) overlaps 2 samples. A sample is the
  MEAN over its interval, so the load-window mean is estimated by overlap weighting with the idle-settle
  baseline subtracted from the non-overlapping part. Instantaneous columns (memory, queue, psi) are quoted as
  the sample values, not averaged. Run with: python3 -I -B postmortem.py
"""
import datetime as dt, json, os, re, statistics as st, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "a2a-storage-repro-r640", "out")
SA = os.path.join(HERE, "raw", "sa12")
KS = [1, 2, 3, 4, 5]


def utc(s):  # '2026-11-12T16:04:48Z'
    return dt.datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc).timestamp()


pj = json.load(open(os.path.join(OUT, "pilot.json")))
sess = {s["k"]: s for s in pj["results"]["A2a_validation"]["sessions"]}
stats = pj["results"]["A2a_validation"]["session_stats"]
runs = [r for r in pj["runs"] if r.get("phase") == "a2a"]
DEV = sess[1]["identity"]["kernel_disk"]          # kernel name of the approved SSD this boot (evidence only)
assert all(sess[k]["identity"]["kernel_disk"] == DEV for k in KS)

# ---- windows
W = {}
for k in KS:
    s = sess[k]
    W[k] = dict(trim=utc(s["trim"]["utc"]), load0=utc(s["start"]["utc"]), load1=utc(s["end"]["utc"]),
                pre=utc(s["pre_trim"]["utc"]))

# ---- sysstat samples (sadf -j; a sample covers (t - interval, t])
raw = subprocess.run(["sadf", "-j", SA, "--", "-A"], capture_output=True, text=True, check=True).stdout
samples = []
for x in json.loads(raw)["sysstat"]["hosts"][0]["statistics"]:
    ts = x["timestamp"]
    assert ts.get("utc") == 1
    t = dt.datetime.strptime(ts["date"] + " " + ts["time"], "%Y-%m-%d %H:%M:%S").replace(tzinfo=dt.timezone.utc).timestamp()
    f = {}          # interval-mean metrics
    inst = {}       # instantaneous metrics
    for c in x["cpu-load"]:
        n = c["cpu"]
        f[f"cpu.{n}.busy"] = c["usr"] + c["nice"] + c["sys"] + c["irq"] + c["soft"] + c["guest"] + c["gnice"]
        if n == "all":
            for m in ("usr", "sys", "iowait", "steal", "irq", "soft", "idle"):
                f[f"cpu.all.{m}"] = c[m]
    f["pcsw.cswch_s"] = x["process-and-context-switch"]["cswch"]
    f["pcsw.proc_s"] = x["process-and-context-switch"]["proc"]
    for m in ("fault", "majflt", "pgfree", "pgpgout", "pgscank", "pgscand", "pgsteal"):
        f[f"paging.{m}_s"] = x["paging"][m]
    f["io.tps"] = x["io"]["tps"]
    f["io.bwrtn_s"] = x["io"]["io-writes"]["bwrtn"]
    f["io.dtps"] = x["io"]["io-discard"]["dtps"]
    for d in x["disk"]:
        if d["disk-device"] == DEV:
            for m in ("tps", "wkB", "rkB", "dkB", "areq-sz", "aqu-sz", "await", "util-percent"):
                f[f"disk.{DEV}.{m}"] = d[m]
    for n in x["network"]["net-dev"]:
        if n["iface"] == "lo":
            f["net.lo.pck_s"] = n["rxpck"]
            f["net.lo.kB_s"] = n["rxkB"]
    for m, v in x["memory"].items():
        inst[f"mem.{m}"] = v
    for m in ("runq-sz", "plist-sz", "ldavg-1", "ldavg-5", "blocked"):
        inst[f"queue.{m}"] = x["queue"][m]
    inst["psi.cpu_some_avg60"] = x["psi"]["psi-cpu"]["some_avg60"]
    inst["psi.io_some_avg60"] = x["psi"]["psi-io"]["some_avg60"]
    inst["psi.io_full_avg60"] = x["psi"]["psi-io"]["full_avg60"]
    inst["psi.mem_some_avg60"] = x["psi"]["psi-mem"]["some_avg60"]
    for m, v in x["kernel"].items():
        inst[f"kernel.{m}"] = v
    samples.append(dict(t1=t, t0=t - ts["interval"], iv=ts["interval"], f=f, inst=inst))
samples.sort(key=lambda s: s["t1"])
# exact interval start: previous sample's end when contiguous (sadf's interval is the true delta)


def overlap(s, a, b):
    return max(0.0, min(s["t1"], b) - max(s["t0"], a))


# a sample "touches" a session if it overlaps its load window; make sure none touches two
for s in samples:
    hit = [k for k in KS if overlap(s, W[k]["load0"], W[k]["load1"]) > 0]
    assert len(hit) <= 1, "a sample overlaps two load windows"

# idle baseline: samples with no load overlap and no trim inside (A2-a settle only)
busy_any = lambda s: any(overlap(s, W[k]["load0"], W[k]["load1"]) > 0 for k in KS)
idle = [s for s in samples if W[1]["pre"] - 1200 < s["t0"] and s["t1"] < W[5]["load1"] and not busy_any(s)
        and not any(W[k]["pre"] - 5 <= s["t1"] and s["t0"] <= W[k]["trim"] + 5 for k in KS)]
metrics = sorted(m for m in samples[0]["f"] if not m.endswith(".dkB") and m != "io.dtps")  # discard rows excluded: a sample can contain the NEXT session's fstrim; exact discards come from A1 diskstats
base = {m: st.median(s["f"][m] for s in idle) for m in metrics}

est, cover = {}, {}
for k in KS:
    hit = [(s, overlap(s, W[k]["load0"], W[k]["load1"])) for s in samples]
    hit = [(s, o) for s, o in hit if o > 0]
    tot_ov = sum(o for _, o in hit)
    cover[k] = dict(n_samples=len(hit), load_s=W[k]["load1"] - W[k]["load0"], overlap_s=tot_ov,
                    samples=[dict(end=dt.datetime.fromtimestamp(s["t1"], dt.timezone.utc).strftime("%H:%M:%S"),
                                  interval_s=s["iv"], overlap_s=round(o, 1)) for s, o in hit])
    est[k] = {}
    for m in metrics:
        num = sum(s["f"][m] * s["iv"] - base[m] * (s["iv"] - o) for s, o in hit)
        est[k][m] = num / tot_ov
# NB each estimate assumes the load window's own mean is what is wanted; it is the SESSION mean (12 runs).


def loo(m, i, tab):
    """leave-one-out score of session i for metric m: (x_i - median(others)) / max(range(others), floor).
    floor: 0.05 percentage points for per-CPU busy %, else 0.5 % of the others' median."""
    o = [tab[k][m] for k in KS if k != i]
    med = st.median(o)
    floor = 0.05 if m.startswith("cpu.") and m.endswith(".busy") else 0.005 * abs(med)   # resolution floor, see REPORT
    rng = max(max(o) - min(o), floor, 1e-12)
    return (tab[i][m] - med) / rng


# ---- instantaneous samples inside each load window
inst_rows = {}
for k in KS:
    rows = []
    for s in samples:
        if W[k]["load0"] <= s["t1"] <= W[k]["load1"]:
            rows.append(dict(at=dt.datetime.fromtimestamp(s["t1"], dt.timezone.utc).strftime("%H:%M:%S"), **s["inst"]))
    inst_rows[k] = rows

# ---- per-session benchmark telemetry
def runs_of(k, kind):
    return [r for r in runs if r.get("session") == k and r["slot"].startswith(kind)]


def server_stats(k, kind):
    out = {"frames_recv": [], "frames_send": [], "ring_per_req": []}
    for r in runs_of(k, kind):
        for line in open(os.path.join(OUT, "raw", f"A-a2a-k{k}-{r['slot']}.server.log")):
            m = re.match(r"total: requests (\d+) forwarded [0-9.]+% frames/recv ([0-9.]+) frames/send ([0-9.]+) ring msgs/request ([0-9.]+)", line)
            if m:
                out["frames_recv"].append(float(m.group(2))); out["frames_send"].append(float(m.group(3)))
                out["ring_per_req"].append(float(m.group(4)))
    return {a: st.median(b) for a, b in out.items()}


def ckpt_range(k, kind):
    v = []
    for r in runs_of(k, kind):
        p = os.path.join(OUT, "raw", f"A-a2a-k{k}-{r['slot']}.server.log")
        for line in open(p):
            m = re.match(r"checkpoint: \d+ objects in ([0-9.]+)s", line)
            if m:
                v.append(float(m.group(1)))
    return (min(v), max(v)) if v else None


bench = {}
for k in KS:
    C = runs_of(k, "C")
    F = runs_of(k, "F")
    S = runs_of(k, "S")
    sn = {t: json.load(open(os.path.join(OUT, "state", f"k{k}-{i}-{t}", "snapshot.json")))
          for i, t in ((0, "pre-trim"), (1, "start"), (2, "end"))}
    ops = sum(r["exec_ops"] + r["index_ok"] for r in S + F + C)
    wsec = sn["end"]["sectors_written"] - sn["start"]["sectors_written"]
    bench[k] = dict(
        anchor_p99_median_ns=stats[str(k)]["median_p99_ns"],
        capacity_thr_median=st.median(r["total_ops_s"] for r in C),
        capacity_cpu_us_per_op=st.median(r["cpu_us_per_op"] for r in C),
        capacity_write_B_per_op=st.median(r["write_bytes_per_op"] for r in C),
        capacity_p99_ns=st.median(r["p99_ns"] for r in C),
        anchor_cpu_us_per_op=st.median(r["cpu_us_per_op"] for r in F),
        ckpt_capacity_s=ckpt_range(k, "C"),
        ckpt_anchor_s=ckpt_range(k, "F"),
        trim_bytes=sess[k]["trim"]["bytes"],
        temp_pre_start_end=[sn[t]["smart"]["temperature_c"] for t in ("pre-trim", "start", "end")],
        scan_pct_pre_start_end=[sn[t]["smart"]["bms_progress_pct"] for t in ("pre-trim", "start", "end")],
        units_started=sn["start"]["units_started"],
        session_ops=ops, session_written_bytes=wsec * 512, session_written_B_per_op=wsec * 512 / ops,
        session_run_s=sum(r["run_s"] for r in S + F + C),
        discard_sectors_in_load=sn["end"]["sectors_discarded"] - sn["start"]["sectors_discarded"],
        cap_server=server_stats(k, "C"), anchor_server=server_stats(k, "F"),
        cap_thr_reps=[r["total_ops_s"] for r in C],
    )
    bench[k].update({f"cap_{a}": v for a, v in bench[k]["cap_server"].items()})
    bench[k].update({f"anchor_{a}": v for a, v in bench[k]["anchor_server"].items()})

# ---- leave-one-out table over every metric
loo_tab = {k: {m: loo(m, k, est) for m in metrics} for k in KS}
benchm = ["cap_frames_recv", "cap_frames_send", "cap_ring_per_req", "anchor_frames_recv", "anchor_frames_send", "anchor_ring_per_req", "capacity_thr_median", "capacity_cpu_us_per_op", "capacity_write_B_per_op", "capacity_p99_ns",
          "anchor_p99_median_ns", "anchor_cpu_us_per_op", "session_written_B_per_op", "trim_bytes"]
loo_bench = {k: {m: loo(m, k, bench) for m in benchm} for k in KS}

res = dict(device_kernel_name=DEV, windows={k: {a: dt.datetime.fromtimestamp(v, dt.timezone.utc).strftime("%H:%M:%S")
                                                for a, v in W[k].items()} for k in KS},
           idle_baseline_samples=len(idle), coverage=cover, bench=bench, instantaneous=inst_rows,
           estimates=est, loo=loo_tab, loo_bench=loo_bench,
           sysstat_activities_unavailable=["-m FREQ/TEMP/CPU frequency", "-I interrupts", "-n SOFT per-CPU softirq"])
json.dump(res, open(os.path.join(HERE, "postmortem.json"), "w"), indent=1, sort_keys=True)

# ---- tables
L = []
P = L.append
P("## Sampling coverage (10-minute sysstat samples; mean over interval)")
P("k | load window | samples overlapping (end time: overlap s / interval s)")
for k in KS:
    c = cover[k]
    P(f"{k} | {res['windows'][k]['load0']}-{res['windows'][k]['load1']} ({c['load_s']:.0f}s) | " +
      "; ".join(f"{s['end']}: {s['overlap_s']:.0f}/{s['interval_s']}" for s in c["samples"]))
P(f"idle-baseline samples (no load, no trim inside): {len(idle)}")
P("")
P("## Consolidated per-session table (benchmark + host telemetry)")
cols = [("anchor p99 median ns", "anchor_p99_median_ns", "{:,.0f}"), ("cap thr median", "capacity_thr_median", "{:,.0f}"),
        ("cap CPU us/op", "capacity_cpu_us_per_op", "{:.3f}"), ("cap wr B/op", "capacity_write_B_per_op", "{:.1f}"),
        ("cap p99 ns", "capacity_p99_ns", "{:,.0f}"), ("anchor CPU us/op", "anchor_cpu_us_per_op", "{:.3f}"),
        ("session wr B/op (all 12 runs)", "session_written_B_per_op", "{:.2f}"), ("trim bytes", "trim_bytes", "{:,}")]
P("k | " + " | ".join(c[0] for c in cols) + " | ckpt cap s (min-max) | ckpt anchor s | SSD C pre/start/end | scan % pre/start/end | units started")
for k in KS:
    b = bench[k]
    P(f"{k} | " + " | ".join(c[2].format(b[c[1]]) for c in cols) +
      f" | {b['ckpt_capacity_s'][0]:.2f}-{b['ckpt_capacity_s'][1]:.2f} | {b['ckpt_anchor_s'][0]:.2f}-{b['ckpt_anchor_s'][1]:.2f}"
      f" | {'/'.join(map(str, b['temp_pre_start_end']))} | {'/'.join(f'{x:.2f}' for x in b['scan_pct_pre_start_end'])} | {', '.join(b['units_started'])}")
P("")
P("## Batching (server log 'total:' line), median over runs")
P("k | cap frames/recv | cap frames/send | cap ring msgs/req | anchor frames/recv | anchor frames/send | anchor ring msgs/req | discard sectors in load window (A1 diskstats)")
for k in KS:
    b = bench[k]
    P(f"{k} | {b['cap_frames_recv']:.2f} | {b['cap_frames_send']:.2f} | {b['cap_ring_per_req']:.3f} | {b['anchor_frames_recv']:.2f} | {b['anchor_frames_send']:.2f} | {b['anchor_ring_per_req']:.3f} | {b['discard_sectors_in_load']}")
P("")
P("## Host telemetry, load-window mean estimate (overlap-weighted, idle subtracted); LOO = (k - median(others)) / range(others)")
show = ["cpu.all.usr", "cpu.all.sys", "cpu.all.iowait", "cpu.all.steal", "cpu.all.irq", "cpu.all.soft", "pcsw.cswch_s", "pcsw.proc_s",
        f"disk.{DEV}.tps", f"disk.{DEV}.wkB", f"disk.{DEV}.areq-sz", f"disk.{DEV}.aqu-sz", f"disk.{DEV}.await", f"disk.{DEV}.util-percent",
        "net.lo.pck_s", "net.lo.kB_s", "paging.fault_s", "paging.pgfree_s", "paging.pgscank_s", "paging.pgscand_s"]
P("metric | " + " | ".join(f"k{k}" for k in KS) + " | LOO k2 | max |LOO| other sessions")
for m in show:
    others = max(abs(loo_tab[k][m]) for k in KS if k != 2)
    P(f"{m} | " + " | ".join(f"{est[k][m]:.4g}" for k in KS) + f" | {loo_tab[2][m]:+.2f} | {others:.2f}")
P("")
P("## Per-CPU busy % (usr+sys+irq+soft) estimate: group means")
srv, cli = [2, 4, 6, 8, 10, 12, 14, 16], [18, 20, 22, 24, 26, 28, 30, 32]
rest = [c for c in range(72) if c not in srv + cli]
P("group | " + " | ".join(f"k{k}" for k in KS))
for name, g in (("server cpus 2..16 even", srv), ("client cpus 18..32 even", cli), ("all other cpus", rest)):
    P(name + " | " + " | ".join(f"{st.mean(est[k][f'cpu.{c}.busy'] for c in g):.3f}" for k in KS))
P("")
P("## LOO outliers: every metric (72 per-CPU + ~40 aggregate) where |LOO| > 1, count per session")
cnt = {k: [m for m in metrics if abs(loo_tab[k][m]) > 1] for k in KS}
P("k | n metrics with |LOO|>1 (of %d) | largest |LOO| metrics" % len(metrics))
for k in KS:
    top = sorted(metrics, key=lambda m: -abs(loo_tab[k][m]))[:4]
    P(f"{k} | {len(cnt[k])} | " + ", ".join(f"{m} {loo_tab[k][m]:+.2f}" for m in top))
P("")
P("## LOO on benchmark-side quantities")
P("metric | " + " | ".join(f"k{k}" for k in KS))
for m in benchm:
    P(f"{m} | " + " | ".join(f"{loo_bench[k][m]:+.2f}" for k in KS))
P("")
P("## Instantaneous samples inside each load window (not averaged)")
im = ["mem.memfree", "mem.cached", "mem.buffers", "mem.dirty", "mem.slab", "mem.anonpg", "queue.runq-sz", "queue.ldavg-1",
      "queue.blocked", "psi.cpu_some_avg60", "psi.io_some_avg60", "psi.io_full_avg60", "psi.mem_some_avg60"]
P("k | at | " + " | ".join(im))
for k in KS:
    for r in inst_rows[k]:
        P(f"{k} | {r['at']} | " + " | ".join(str(r[m]) for m in im))
open(os.path.join(HERE, "tables.txt"), "w").write("\n".join(L) + "\n")
print("\n".join(L))
