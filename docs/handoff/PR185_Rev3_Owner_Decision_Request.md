# AR-1 prereg revision 3: owner decision request

These items need cody's explicit yes or no before G3 can be reviewed. The full text of each amendment is in
`docs/AR1_PREREG.md` section 16, and revision 2 is preserved in `docs/AR1_PREREG_REV2.md`. A yes accepts the
amendment into the prereg. It does not authorize counted runs: G3 needs its own GO.

Codex supported A-1 and A-2 in its review of PR #185 (`docs/handoff/PR185_Codex_Review.md`). In its review of #185
and #187-#191 it accepted the corrected G2 evidence and asked for this decision. Codex's support records nothing on
its own.

| Item | What it changes | Why | Recommendation |
|---|---|---|---|
| A-1, commit end | A commit's end is its own record end (XactLastRecEnd), not the next record's start. | It is what SyncRep waits for. Under revision 2's definition, the only bound that passes can wait forever on an idle server. | **Yes.** |
| A-2, the bound | The bound subtracts the page header when the insert position sits exactly at a page start (24 bytes) or a segment start (40 bytes). | Without it, 23 of 10,000 waits stalled for 4.7 to 24.4 s. C-7b checks every boundary case, and its negative control fires. | **Yes.** |
| A-3, points Ac and P | Adds a cancelled-SyncRep dispatch point (Ac) and a foreign-writer policy point (P). | Without them, nothing independent of the gateway could catch M1, and nothing could exercise M5. | **Yes.** |
| A-4, after-outcome release | Adds a release order where the old epoch records a stale completed outcome at T-F and T-N. | M13's obligation cannot arise under the revision 2 orders at those tools. The old orders stay. | **Yes.** |
| A-5, M5 proof | States why the revision 2 points cannot expose M5 in S1, and moves M5 to point P. | It replaces a silent waiver with a proof and a working schedule. | **Yes.** |
| A-6, provider request ids | Each physical model request gets a fresh random id, and 3.no_regenerate checks it. | It lets the checker tell a regenerated reply from the original (Codex 2E). | **Yes.** |
| A-7, delivery | Receivers log `delivered` after flushing a reply, and "received" uses it. | It makes "received" observable instead of inferred. | **Yes.** |
| A-8, M11 at C-b2 | M11 is counted only on seeds where the reply is released after the takeover. | A "before" release leaves no outstanding call, so M11 cannot be observed there. | **Yes.** |
| A-9, 1.upper | States when 1.upper counts as exercised (an unestablished commit at a pre-resume cut, with a state request started before resume). | The cut taken after the answer can never show it for a correct runtime. | **Yes.** |
| A-10, pre-resume cut | The cut is invalid unless the walreceiver kept its pid and stayed stopped. | Closes U-9. | **Yes.** |
| A-11, run cap | The 5-minute cap covers setup through the end of the check, and every run records its wall time. | Closes U-12. | **Yes.** |
| A-12, environment | Every package is pinned by sha256, and setup installs only from the locks. | Closes U-11. A clean reinstall reproduced the environment exactly. | **Yes.** |
| U-7, checkpoint replay | Ruling: model output replayed from a DBOS step checkpoint is the same output, not a regeneration. | No second physical request goes out, and A-6 verifies that the recorded output carries the first request's id and hash. It is consistent with prereg section 3 and E-11. | **Yes.** |

## What a yes does and does not do

- **Yes to all:** revision 3 becomes the accepted prereg, and the G2 report becomes final evidence for G3 review.
- **No to any item:** that item goes back to revision 2's wording. Any G2 result that depends on it is reported as
  UNRESOLVED.
- **Either way:**
  - G3 counted runs stay on HOLD until cody gives a separate GO;
  - counted runs go on the dedicated VM, installed from the locks.
