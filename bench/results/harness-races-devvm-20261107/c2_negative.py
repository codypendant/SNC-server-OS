"""Negative paths for c2_restart_held: the grounding condition deliberately
absent. Usage: c2_negative.py BUILD MODE (absent | stale). Expects c2 to FAIL
with grounded false, within the bounded wait."""
import json, sys, time
build, mode = sys.argv[1], sys.argv[2]
sys.argv = ["test_ready_order.py", build, "c2_restart_held"]
sys.path.insert(0, "/home/claude/snc-server-os/tests")
import test_ready_order as t
if mode == "absent":   # the new process never prints its READY line
    t.first_ready_line = lambda lines: None
elif mode == "late":   # the race, made deterministic: the line appears 3 s late
    real, first = t.first_ready_line, []
    def late(lines):
        first.append(time.time()) if not first else None
        return real(lines) if time.time() - first[0] >= 3 else None
    t.first_ready_line = late
else:                  # its first READY line names an older NOT READY
    t.first_ready_line = lambda lines: "witness: standby back in sync (READY after v=1)"
t0 = time.time()
r = t.SCEN["c2_restart_held"]()
out = {k: r.get(k) for k in ("pass", "ready_again", "new_ready_line", "grounded", "refused", "error")}
out["seconds"] = round(time.time() - t0, 1)
print(mode, json.dumps(out, default=str))
if mode == "late":
    print("LATE_OK" if r.get("pass") is True and r.get("grounded") is True else "LATE_UNEXPECTED")
    sys.exit(0)
print("NEGATIVE_OK" if r.get("pass") is False and r.get("grounded") is False else "NEGATIVE_UNEXPECTED")
