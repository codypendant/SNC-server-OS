#!/usr/bin/env python3
"""Post-hoc (not preregistered): where refusals and late ops fall relative to
the checkpoint phases, for the T-arm causal cells of ../run/raw.

Phases come from the gauges (snc::diag::mark_phase): 1 begin_checkpoint,
2 walk done, 3 chunk writes done and fdatasync submitted, 4 fdatasync done.
Each boundary is the latest shard's mark, so at 50 ms resolution the split
between adjacent phases is approximate. Each 50 ms client bucket is assigned
to the phase its timestamp falls in.

  python3 analysis/phases.py run/raw
"""
import collections, csv, gzip, json, os, sys

RAW = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "run", "raw")


def rows(name):
    return list(csv.DictReader(gzip.open(os.path.join(RAW, name), "rt")))


def main():
    out = {}
    for kind in ("r436646-ckpt", "r436646-nockpt", "r2183233-w60"):
        for r in range(3):
            n = f"T-{kind}-r{r}"
            T = json.load(open(os.path.join(RAW, n + ".times.json")))
            g, b, dk = rows(n + ".gauges.csv.gz"), rows(n + ".buckets.csv.gz"), rows(n + ".disk.csv.gz")
            ph = collections.defaultdict(list)
            for x in g:
                for k in ("ph_begin", "ph_walked", "ph_sync", "ph_done"):
                    if int(x[k]):
                        ph[k].append(int(x[k]))
            res = {}
            if not ph:
                refused = sum(int(x["exec_refused"]) + int(x["index_refused"]) for x in b)
                late = sum(int(x["late_over_1ms"]) for x in b)
                out[n] = dict(checkpoint=False, refused=refused, late_over_1ms=late)
                print(n, out[n])
                continue
            beg, walked, sync, done = (min(ph["ph_begin"]), max(ph["ph_walked"]), max(ph["ph_sync"]),
                                       max(ph["ph_done"]))
            edges = [("before", -1), ("walk", beg), ("write_drain", walked), ("fdatasync", sync),
                     ("recover_1s", done), ("after", done + 1e9)]
            ref, late = collections.Counter(), collections.Counter()
            for x in b:
                t, seg = int(x["t_ns"]), "before"
                for name, e in edges:
                    if t >= e:
                        seg = name
                ref[seg] += int(x["exec_refused"]) + int(x["index_refused"])
                late[seg] += int(x["late_over_1ms"])
            aw = []
            for a, c in zip(dk, dk[1:]):
                dw = int(c["w_ios"]) - int(a["w_ios"])
                if dw:
                    aw.append((int(c["t_ns"]), (int(c["w_ms"]) - int(a["w_ms"])) / dw))
            pre = sorted(v for t, v in aw if T["run_start"] < t < beg)
            fds = [v for t, v in aw if sync <= t <= done]
            res = dict(checkpoint=True, at_s=round((beg - T["run_start"]) / 1e9, 1),
                       walk_s=round((walked - beg) / 1e9, 2), write_drain_s=round((sync - walked) / 1e9, 2),
                       fdatasync_s=round((done - sync) / 1e9, 2), refused=dict(ref), late_over_1ms=dict(late),
                       wal_await_median_before_ms=round(pre[len(pre) // 2], 3) if pre else None,
                       wal_await_max_fdatasync_ms=round(max(fds), 2) if fds else None)
            out[n] = res
            print(n, json.dumps(res))
    json.dump(out, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "phases.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
