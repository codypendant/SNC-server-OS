"""B3 reproduction (release): a standby that catches up from disk and keeps
missing the ack timeout. Node 1's RSS vs its retained replication bytes.

Usage (from a build dir): python3 ../bench/results/baseline-readiness-20261105/b3_repro.py . 16 stall|nostall
(16 = --replica-retain-mib, so the rejoined standby catches up from disk).
docs/BASELINE_READINESS.md B3."""
import sys, time, signal, json
build = sys.argv[1]; retain = sys.argv[2]; stall = sys.argv[3] == "stall"
sys.argv = [sys.argv[0], build]
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tests"))
import test_baseline_retry as br
th = br.th
out = {"retain_mib": retain, "standby_stalled": stall, "series": []}
def res(pr, k):
    r, _ = th.ask(pr.adm[k], "RESOURCES", 3)
    return th.fields(r)
def body(pr):
    b = br.Bench(pr, 60, threads=2, depth=64); pr.bench = b
    time.sleep(2)
    pr.nodes[0].signal(signal.SIGKILL); pr.nodes[0].p.wait(timeout=30)
    assert br.wait_state(pr, 1, 15)
    time.sleep(8)  # node 1 alone: records leave its memory, the rejoin must read them from disk
    pr.old_nodes.append(pr.nodes[0])
    pr.nodes[0] = th.Proc([f"{br.BUILD}/snc_server"] + pr.args[0])
    assert pr.nodes[0].wait_for(r"standby, listening", 30)
    t0 = time.time()
    while time.time() - t0 < 30:
        if stall:
            pr.nodes[0].signal(signal.SIGSTOP); time.sleep(0.7); pr.nodes[0].signal(signal.SIGCONT); time.sleep(0.3)
        else:
            time.sleep(1)
        f = res(pr, 1)
        out["series"].append({"t": round(time.time() - t0, 1), "rss_mib": int(f.get("rss_kb", 0)) // 1024,
                              "retained_mib": int(f.get("repl_retained", 0)) >> 20, "unacked": f.get("wal_unacked")})
    out["stopped_acking_lines"] = sum("stopped acking" in l for l in pr.nodes[1].lines)
    out["disk_catchup_lines"] = [l for l in pr.nodes[1].lines if "disk" in l or "reseed" in l][:5]
    return {}, None
br.run("b3", body, extra=("--replica-retain-mib", retain))
print(json.dumps(out))
