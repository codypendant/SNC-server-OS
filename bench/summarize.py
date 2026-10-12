#!/usr/bin/env python3
"""Reduces bench/results/matrix.csv to medians per configuration and prints
them as JSON (for the report) or as Markdown tables (--markdown)."""
import csv, json, statistics, sys, collections

def load(path):
    groups = collections.OrderedDict()
    for r in csv.DictReader(open(path)):
        groups.setdefault((r["group"], r["engine"], r["flags"]), []).append(r)
    out = []
    for (group, engine, flags), rs in groups.items():
        def med(k):
            v = [float(r[k]) for r in rs if r[k] != ""]
            return statistics.median(v) if v else None
        ops = [int(r["ops_s"]) for r in rs]
        out.append(dict(group=group, engine=engine, flags=flags, runs=len(rs),
                        ops=statistics.median(ops), ops_min=min(ops), ops_max=max(ops),
                        p50=med("p50_ns"), p99=med("p99_ns"), p999=med("p999_ns"),
                        rss=med("rss_per_obj"), recovery=med("recovery_s"),
                        ckpt_load=med("ckpt_load_s"), wal_replay=med("wal_replay_s"),
                        checkpoint=med("checkpoint_s"), msgs=med("msgs_per_op"),
                        contended=med("contended_pct")))
    return out

def us(ns):
    return f"{ns / 1000:,.1f} µs" if ns < 1e6 else f"{ns / 1e6:,.2f} ms"

if __name__ == "__main__":
    rows = load(sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else "bench/results/matrix.csv")
    if "--markdown" in sys.argv:
        print("| Group | Engine | Flags | ops/s (median, min–max) | p50 | p99 | p99.9 |")
        print("| --- | --- | --- | --- | --- | --- | --- |")
        for r in rows:
            print(f"| {r['group']} | {r['engine']} | `{r['flags']}` | {r['ops'] / 1e6:.2f}M ({r['ops_min'] / 1e6:.2f}–{r['ops_max'] / 1e6:.2f}) "
                  f"| {us(r['p50'])} | {us(r['p99'])} | {us(r['p999'])} |")
    else:
        json.dump(rows, sys.stdout, indent=1)
