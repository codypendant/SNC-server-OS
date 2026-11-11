import sys, json
sys.argv = [sys.argv[0]] + sys.argv[1:]
build, out = sys.argv[1], sys.argv[2]
sys.path.insert(0, "/home/claude/snc-server-os/tests")
sys.argv = [sys.argv[0], build, "q4_normal"]
import test_baseline_ha as t
orig = t.drain_rule
def dump(samples, window):
    r = orig(samples, window)
    with open(out, "a") as f:
        f.write(json.dumps({"rule": r, "samples": [{k: s[k] for k in ("undurable", "waiters", "skew_ms")} for s in samples]}) + "\n")
    return r
t.drain_rule = dump
sys.exit(t.main())
