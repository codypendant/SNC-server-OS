"""Writes DIRECTION_COMPARISON_TRACK_A_MATRIX.csv: one row per Track A run (plan section 3.4)."""
import csv
import sys

DELAYS_MS = [0, 1, 5, 50]
POINTS = [
    ("kill_at_receipt", "S-M", ["model", "T-I", "T-F", "T-N"], 2, lambda s: "none"),
    ("kill_after_reply", "S-M", ["model", "T-I", "T-F", "T-N"], 10, lambda s: f"delay_ms={DELAYS_MS[s % 4]}"),
    ("kill_at_approval", "S-AP", ["approver"], 10, lambda s: "deny" if s % 2 else "approve"),
    ("event_before_wait", "S-AP", ["approver"], 10, lambda s: "deny" if s % 2 else "approve"),
    ("kill_during_recovery", "S-M", ["T-I_then_first_after_recovery"], 2, lambda s: "none"),
    ("engine_restart_in_flight", "S-M", ["T-I", "T-N"], 2, lambda s: "none"),
    ("zombie_takeover", "S-M", ["model", "T-I", "T-F", "T-N"], 10, lambda s: "zombie_first" if s % 2 else "new_first"),
]

out = csv.writer(open(sys.argv[1], "w", newline=""))
out.writerow(["point", "kind", "scenario", "boundary", "seed", "varies"])
n = 0
for kind, scenario, boundaries, seeds, varies in POINTS:
    for boundary in boundaries:
        n += 1
        for seed in range(seeds):
            out.writerow([f"A{n:02d}", kind, scenario, boundary, seed, varies(seed)])
