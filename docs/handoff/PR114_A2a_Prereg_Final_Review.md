# PR #114 A2-a preregistration: final review (GO after merge)

Filed verbatim from cody's upload in the project thread, 2026-11-12 04:54 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #114 FINAL REVIEW

Repository:

"codypendant/SNC-server-OS"

PR:

"#114 — A0 done + A2-a storage-state reproducibility preregistration"

Reviewed head:

"01023e4ce7aa384d0ba8950bbac5348fe548d3b7"

Decision

PR #114: APPROVED.

MERGE AUTHORIZED.

A2-a STORAGE-STATE REPRODUCIBILITY VALIDATION: GO AFTER MERGE.

No further methodology or repository change before execution.

---

Accepted preregistration

Sessions

"K = 5"

Each physical session uses:

1. environment guard;
2. clear A2-a data root;
3. fail-closed pre-trim "sync -f";
4. validated A1 pre-trim snapshot;
5. explicit proven "fstrim";
6. fail-closed post-trim "sync -f";
7. 30-minute zero-I/O settle;
8. validated A1 start snapshot;
9. 12 benchmark runs;
10. validated A1 end snapshot.

No failed or incomplete session is replaced.

---

Per-session benchmark order

S0
F1
C1
F2
C2
F3
C3
F4
C4
F5
C5
S1

F and S:

- 654,569 ops/s historical anchor;
- open loop;
- depth 1024;
- 10 s warm-up;
- 60 s measurement;
- 10 s drain;
- Profile-A CP-1A configuration;
- memory measurement enabled.

C:

- closed loop;
- depth 512;
- 60 s;
- unchanged CP-1A capacity path.

---

Environment invariants

Before every session require:

- same boot ID;
- same HEAD;
- clean Git tree;
- "fstrim.timer" disabled;
- "fstrim.timer" inactive;
- "fstrim.service" inactive;
- "kernel.numa_balancing = 0";
- all governors = "performance";
- "/srv/snc-data" = "/dev/sdb1";
- ext4 mount options unchanged;
- physical data disk = "sdb";
- frozen plan SHA;
- frozen manifest SHA;
- frozen "snc_server" SHA;
- frozen "net_bench" SHA.

Any drift:

INCOMPLETE — STOP.

---

Explicit trim proof

Each session's trim requires:

- rc = 0;
- "/srv/snc-data" named in output;
- reported trimmed bytes > 0;
- "sdb" discard sectors increase.

Failure:

INCOMPLETE — STOP before settle.

---

Sync requirements

Pre-trim:

"sync -f /srv/snc-data"

must return 0 before A1 pre-trim and fstrim.

Post-trim:

"sync -f /srv/snc-data"

must return 0 before the settle.

Both rc/output are recorded.

Failure of either:

INCOMPLETE — STOP.

---

Settle requirement

After trim and post-trim sync:

1,800 seconds idle

Require over that interval:

sdb sectors written   = +0
sdb sectors discarded = +0

Any nonzero value:

INCOMPLETE — STOP.

The pre-data idle observation already demonstrated that this zero-I/O condition is achievable.

---

Statistical rules

Per session:

- median p99 from F1..F5;
- p99 CV from F1..F5;
- median capacity from C1..C5;
- throughput CV from C1..C5.

Across five sessions:

- CV p99 between;
- CV throughput between.

Compute:

N_raw =
3 × max(
  CV_p99_between,
  CV_thr_between,
  max_session_CV_p99,
  max_session_CV_thr
)

"N_raw" is a result only.

It does not relax any rule in this experiment.

---

Fixed stationarity sentinel

For every session:

max(P_end / P_start, P_start / P_end) <= 1.10

Equality passes.

Strictly greater than 1.10:

STOP.

This rule is independent of N_raw.

---

STOP rules

STOP if the complete experiment shows:

- an anchor/sentinel rep unsustained;
- invalid capacity result;
- any within-session p99 CV >10%;
- any within-session throughput CV >10%;
- between-session p99 CV >10%;
- between-session throughput CV >10%;
- automatic multimodal-gap signal;
- sentinel movement >10%.

Do not widen thresholds.

Do not replace sessions.

---

INCOMPLETE rules

INCOMPLETE includes:

- guard drift;
- sync failure;
- trim not proven;
- settle contamination;
- unusable A1 snapshot;
- client execution failure;
- unparsable run;
- server/load abort;
- missing run;
- fewer than five complete sessions.

Stop immediately.

---

Test status

Revision 3 mutation suite:

54 / 54 caught

including:

- both sync return-code checks;
- sync helper fail-closed behavior;
- removed sync calls;
- incorrect pre-trim sync ordering;
- "fstrim.service" guard;
- all prior sentinel/guard/trim/settle/A1/capacity checks.

Accepted.

---

Execution sequence

After PR #114 merges:

1. Pull the exact merge commit.
2. Make no further repository commit before execution.
3. Install the narrow sudoers file:

sudo install -m 0440 bench/results/a2a-storage-repro-r640/sudoers-snc-a2a /etc/sudoers.d/snc-a2a
sudo visudo -c

4. Record the "visudo -c" result.
5. Run the committed pre-data test:

PYTHONDONTWRITEBYTECODE=1 python3 -B bench/results/a2a-storage-repro-r640/test_a2a.py

6. If it passes, run:

python3 -B bench/results/a2a-storage-repro-r640/a2a.py run \
  /home/solo/a2a-storage-repro-r640 \
  --scratch /dev/shm/snc-a2a \
  --manifest bench/manifests/r640-main-20261111.txt \
  --build build-s6

If the driver refuses or returns INCOMPLETE:

STOP. Do not work around it.

---

After completion

Copy the output into the repository unchanged.

Open the result PR.

Then stop.

Do not begin the stable-peak bracket automatically.

---

Still unauthorized

- stable-peak boundary revalidation;
- duration Latin-square validation;
- CRITERIA freeze;
- competitor performance;
- REFINE #2;
- product optimization.

Final authorization:

PR #114 APPROVED.

MERGE IT.

THEN RUN THE FIVE-SESSION A2-a REPRODUCIBILITY VALIDATION EXACTLY AS PREREGISTERED.