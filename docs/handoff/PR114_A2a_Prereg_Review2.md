# PR #114 A2-a preregistration: review 2 (one blocking correction)

Filed verbatim from cody's upload in the project thread, 2026-11-12 04:33 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #114 REVIEW — REVISION 2

Reviewed head:

"57eaa37ea1404a72c3efaf998cab6bed441fd485"

Decision

ONE BLOCKING CORRECTION REMAINS.

DO NOT RUN A2-a YET.

All prior review corrections are otherwise accepted.

---

Required correction

Both uses of:

sync -f /srv/snc-data

must be fail-closed.

Current code calls "sh(...)" and ignores the return code.

Required behavior

Pre-trim sync

After removing:

"/srv/snc-data/a2a"

run:

sync -f /srv/snc-data

Require:

"rc == 0"

If nonzero:

INCOMPLETE — STOP BEFORE A1 PRE-TRIM SNAPSHOT AND BEFORE FSTRIM.

Record stdout/stderr and return code.

Reason:

the deletion/free-space metadata must be committed before the explicit trim defines the starting state.

---

Post-trim sync

After the explicit trim succeeds and is proven, run:

sync -f /srv/snc-data

Require:

"rc == 0"

If nonzero:

INCOMPLETE — STOP BEFORE THE 30-MINUTE SETTLE.

Record stdout/stderr and return code.

---

Suggested implementation

Use a small helper such as:

sync_fs(stage)

that:

1. executes "sync -f /srv/snc-data";
2. records rc/output;
3. raises "Incomplete" on nonzero rc.

Keep the evidence in the session record.

Do not simply assert.

---

Required tests

Add deterministic cases proving:

- pre-trim sync failure → INCOMPLETE;
- no A1/trim occurs after pre-trim sync failure;
- post-trim sync failure → INCOMPLETE;
- no settle occurs after post-trim sync failure.

Add mutation coverage showing that ignoring either sync return code is caught.

---

Optional but recommended in the same tiny patch

Add "fstrim.service" to the per-session environment guard:

Require:

"systemctl is-active fstrim.service == inactive"

before each session.

The timer is already disabled, but this closes the remaining route where a manually started fstrim service could already be running when a controlled session begins.

This does not replace the explicit trim proof.

---

Everything else accepted

Accepted from revision 2:

- K = 5;
- A2-a explicit trim;
- 30-minute zero-I/O settle;
- fixed historical anchor 654,569;
- S0/F1/C1/.../C5/S1 order;
- five fixed reps;
- five capacity reps;
- fixed 10% symmetric sentinel;
- N_raw as result only;
- max within-session CV;
- between-session CV;
- «10% stop rules;»
- supplementary multimodal rule;
- per-session guard;
- exact mount/device checks;
- A1 snapshots;
- trim proof;
- stronger capacity validity;
- no replacement sessions;
- STABLE / STOP / INCOMPLETE semantics;
- narrow sudoers design;
- A0 timer disablement;
- 47/47 current mutation suite.

---

After this correction

Return the updated PR.

If the sync paths are fail-closed and tests cover them, I expect this preregistration to be ready for:

A2-a REPRODUCIBILITY RUN — GO

No other methodology redesign is requested.

---

Still unauthorized:

- first explicit experimental trim;
- A2-a performance sessions;
- stable-peak bracket;
- duration Latin square;
- CRITERIA;
- competitors;
- REFINE #2.