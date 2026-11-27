# AR-1 G3 freeze record

- **Authorization:** cody's G3 go, 2026-11-23 00:21 UTC ([AR1_G3_Owner_Decision.md](AR1_G3_Owner_Decision.md)), under
  prereg revision 3.
- **Frozen commit:** `575f49304d0766ff1445e7d442cb7024c3928bbc`. The VM checks out this commit; `ar1/g3.py` refuses any other HEAD or a dirty tree.
- **Frozen trees:**

| Part | Tree |
|---|---|
| `snc_rt` (experiments/ar1/ar1) | `1a6aef5a75fc699190b1792c29bb3c12649ad54f` |
| `ar1_check` (experiments/ar1/check) | `461b8da20f36bf0f85782d0d9e7a87f1de6e7e6e` |
| plain DBOS step 1 | `0d5012cacf8ed05e66c4b05a9dd0ee56f7787c8f` |
| plain DBOS step 2 | `4695e61930499ba28e2cea78492b9bee5b43895f` |

- **Counted seeds:** `seed(point, i, "counted:575f49304d0766ff1445e7d442cb7024c3928bbc")`, i = 0..19. They are fixed by the commit and were not chosen.
  At C-b2, each point gets every release order its outcome needs:

| Point | before | after | after-outcome |
|---|---|---|---|
| C-b2/model | 10 | 10 | - |
| C-b2/T-I | 11 | 9 | - |
| C-b2/T-F | 10 | 3 | 7 |
| C-b2/T-N | 14 | 2 | 4 |

## Evidence on the frozen code (uncounted, discarded as data)

- **SNC bring-up:** 256 of 256 runs pass.
- **Checker adequacy:** 14 of 14 mutations; every one of the 265 runs is flagged with the clause it breaks.
- **Plain DBOS step 2 bring-up:** 256 of 256 runs pass. Its only unexercised clauses are 4.gateway and 3.late_marked,
  which the driver treats as not applicable to plain DBOS: 4.gateway checks SNC's own gateway log, and
  3.late_marked is the other branch of item 3. Plain step 1 fails as expected: 148 runs show violations, and 108
  runs miss a point.
- **Line count (owner rule, [AR1_G3_Line_Count_Decision.md](AR1_G3_Line_Count_Decision.md)):** step 1 to step 2 adds
  284 lines. The count covers the black-formatted .py files only. L is 300.
- **Driver smoke test:** a clean clone of the frozen commit ran every phase end to end with 1 seed per point and 3
  performance tasks per arm, and exited normally:
  - Phase 0: pass;
  - adequacy: no missed mutation;
  - SNC: 128 of 128 pass;
  - plain step 2: 128 of 128 pass;
  - B: about 3 ms per commit at p50.

  Its outcome was UNRESOLVED, as expected at 1 seed, because three C-b2 clauses went unexercised. None of these
  numbers is a result.
- **Performance timing:** about 3 s per task, so 2,000 tasks fit inside the 4 h cap.

## Driver rules beyond the prereg text (readings, applied before any result)

- **B's denominator** is SNC's consequential commits per task (section 4 lists 16 for S-M), counted as commit-token
  rows. The number of WAL commit records is reported as well.
- **Missing points:** a missing point on SNC's arm is UNRESOLVED. On a plain arm it counts as an unmet item.
- **NARROW and GO:** plain step 2 at or under L that has not met items 1-7 gives UNRESOLVED, not GO. The prereg's GO
  needs "plain DBOS needs more than L lines", and that has not been shown in this case.
- **Invalid runs:** an invalid run is repeated once with the same seed. A second invalid run at a point makes the
  campaign UNRESOLVED. A violation seen in an invalid run still counts (section 7.1).

## Hardware deviation (recorded 2026-11-27, before the run)

- **Prereg section 2 (PROPOSED):** one isolated cloud VM with 4 vCPU, 16 GB and a local SSD.
- **Actual:** cody's Vultr VM, Ubuntu 24.04, with 6 vCPU, 16 GB and 320 GB of local NVMe. Vultr does not offer a 4 vCPU,
  16 GB size with local SSD. The driver's `manifest.txt` records the exact CPU, memory, kernel and filesystem.
- **Effect:**
  - The verdicts do not depend on the CPU count. The driver still runs 2 workers, as it did in the smoke test.
  - The extra cores and the faster disk leave more headroom under the per-run and per-phase caps.
  - B compares SNC and plain step 2 interleaved on the same machine, so both arms see the same hardware.
  - No CPU pinning, card change or code change. The frozen commit stays `575f493`.
