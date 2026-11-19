# AFR-001: durability owner-decision package (PROPOSED)

Status: **PROPOSED, for Codex and owner review.** Docs only. Nothing is frozen or implemented.

**[2026-11-19, owner decisions recorded in `docs/handoff/PR171_Owner_Decisions.md`:** Y5 yes (OD-10, recorded
revision of R9-A); Y6a, Y6b, Y8 yes as design directions (OD-11, OD-12, OD-14); Y6c-1 yes as the initial direction
(OD-13); **Y6c-2 CONDITIONAL, not accepted**, pending the I2-I5 re-argument (`docs/AFR_001_R65_INVARIANTS.md`); Y4
and Y7 HOLD. Codex's targeted final review of this text is section 7. After Codex's #172 review
(`docs/handoff/PR172_Codex_Review.md`): premise P's evidence is section 8, Y9 is `docs/AFR_001_REPORT_IDENTITY.md`
(Y9-T recommended), round 5 is section 9. After Codex's #173 review (`docs/handoff/PR173_Codex_Review.md`): round 6, the code
changes P-2, P-3 and E30 need, and the updated owner questions are section 10. After Codex's #174 review
(`docs/handoff/PR174_Codex_Review.md`): round 7, the buffered-WAL answers, a convergence table and the current owner
questions are section 11, which supersedes 10.2. After Codex's #175 review
(`docs/handoff/PR175_Codex_Review.md`): the preregistered model plan is `docs/AFR_001_REPORT_MODEL_PREREG.md`, the
`RWF_DSYNC` analysis is section 12, and the current owner questions are section 13. After Codex's #176 review
(`docs/handoff/PR176_Codex_Review.md`): the Y10 verification list on the target kernel is section 14. Y6c-2 and Y9 are not
accepted.**]**

Authority: Codex's review of #170 (`docs/handoff/PR170_Codex_Review.md`), section 4: the focused re-review, the G-48
initial-seed question, and "a concise final owner-decision package" in five parts. Codex's section 3 items are
recommendations, not approvals.

Unchanged: D-02 (HOLD), R-23 and O-FLOOR open, no final D-06 freeze, `m = 2` and every existing guarantee. Not done
and not authorized: X-2, R640, a model campaign, any experiment, C++, Rust, migration.

**The contract** means `docs/AFR_001_DURABILITY_CONTRACT.md` as corrected by round 3 (section 6 here). Labels as
there: FACT-SNC, FACT-MODEL, INF, HYP.

## 1. The decisions

| # | Decision asked | Recommendation | Codex (not an approval) |
|---|---|---|---|
| **Y5** | R9-A′: a replica reports an entry only once it has applied it **and its durable watermark covers it** (contract 5.1), with the word and meaning changes of 5.2, and recovery's sync then raise (2.2 step 9) | yes, as a recorded revision of frozen R9-A | support; defer until this re-review |
| **Y6a** | V8-1 (replace the newest segment after every restart); V8-L (a lock on every descriptor that carries an async `O_DIRECT` write; recovery passes each lock before reading); V8-3; the supported platform of 2.7 | yes; V8-R stays the fallback | support, conditional on platform verification |
| **Y6b** | the file name spaces, HeaderFirst spare recovery (note B.4), refusal on `EEXIST` | yes, as OD-8's realization | support |
| **Y6c-1** | whole-node in-place reseed as the initial direction, with contract 6.5's amended steps | yes | support |
| **Y6c-2** | the R6.5 revision (contract 6.6): the flag clears at RS-6's publication; `k` is then positioned by an ordinary R6.2 round and until then sends no report, holds no lease and is not admitted to S | yes, **after** I2-I5 are re-argued for the new state (section 5, B3) | support, with review of the frozen implications |
| **Y8** (new) | correct the two crash-only refusals (section 2): a seed marker (E30), and the links generation before the first record write (E31). Not: record them as G-48 exceptions | correct them, as design directions | not yet reviewed |
| Y7 | per-node detector versus an industry-level fault model for the first version | HOLD for a replicated measurement | HOLD |
| Y4 | the D-06 revision inventory E1-E31 | HOLD until Y5, Y6 and Y8 are recorded | HOLD |

Also for the owner, not Y items: whether "disk filled from outside SNC" is a G-48 exception (contract 2.5); whether
FM-8's scope closes R-23 (contract 10 step 6); E-1, E-2, E-3 and M-1, each a separate authorization (section 4).

## 2. G-48 and the initial seed (Codex 2B)

**The exact texts [FACT-SNC]:**
- G-48, `docs/SNC_GUARANTEES.md` 242: "After a covered fault heals, service resumes without operator action, within a
  stated bound. Exception: any refusal under G-14(a) (detected damage, including a damaged term history) needs an
  operator." Its other items are a restarted sole copy (open) and a sole member that crashes before READY (proposed).
  **No seed exception is listed.**
- G-14(a), line 139: covers content that is "damaged or cannot interpret".
- `docs/AFR_001_D02_CRASH_DESIGN.md` section 0, requirement 2: "A crash with no media fault always recovers without an
  operator (G-48). A refusal is allowed only when a media fault occurred."
- `docs/AFR_001_D06_BOUNDARY.md` rule 5 (PROPOSED): "A crash before the record write leaves no superblock but leaves
  data, so the next start refuses … That is fail-closed and needs the operator. It is today's behaviour".
- `docs/AFR_001_D02_ACCEPTANCE_REVIEW.md` 175, 329 (B10): listed as open.
- No owner decision in `docs/handoff/` mentions the seed. PR160's acceptance of D-06 items 4-14 is silent on it.
- Code: `seed.cpp:234-235` creates `ckpt/<seq>` before the transfer; `:273` writes the record last; a restart with
  `--seed-from` then throws at `engine.cpp:946` ("has data but no superblock; clear it first"); the comment at
  `seed.cpp:264-265` ("the next start fetches again") is wrong, and so was `docs/REPLICATION.md` (corrected on site).

**Classification: an unresolved violation**, of proposed G-48 and of design requirement 2. It is not a G-14(a)
detection (nothing is damaged), and no accepted text makes it an exception. #170 called it "an existing exception …
kept" and marked it as damage; both are corrected (contract section 3, row C1).

**A second one, found in round 3** (rr1 M2) [FACT-SNC]: `engine.cpp:1074` commits the first record and `:1080` then
creates the links generation with `fresh = true`. A crash between them leaves a valid record and no links generation;
every later start throws at `links_generation.cpp:85-90` until the operator passes `--links-generation-min`
(cluster nodes only). Row C2.

**Smallest corrections (Y8; recommended over an exception)** [INF]:
- **C1, a seed marker (E30).** Before anything else of the seed, it creates an empty `seeding` file and syncs the
  directory. Step 3 classifies "no valid record slot, and `seeding` present" as **SEED**: the seed's own files
  (`ckpt/`, stream directories, record files) came from the primary and this node never acknowledged any of them, so
  phase 2 removes those, keeps the node's local files (`links_generation*`, `role`, the lock), and fetches again.
  After the record write, step 8 unlinks the marker and syncs the directory before the node serves. A valid record
  always wins over a stale marker. A lost marker create refuses as today; FM-7's scope is not widened. In the
  slot-epoch mismatch wipe-and-seed (E14′) it covers the seed only once the operator's wipe has finished; V7 stays
  open. [Corrected in round 4: the first text removed "everything else", which would also have removed a kept
  `links_generation`.]
- **C2, creation order (E31).** Create the links generation durably before the first record write, on every init
  and seed, cluster member or not, as V8-2 orders the watermark files. A missing file beside a valid record is then a
  fault, or a directory created before this rule (its own row).

Each is one file and one ordering rule; no new protocol. **The alternative** is an owner decision adding both to
G-48's exception list ("a crash during a node's initial seed, or between its first record write and its links
generation, needs an operator"). Not recommended: G-48 would then admit an operator step after crashes alone.

## 3. Effects on accepted or frozen contracts

| Decision | Frozen or accepted text affected | Effect |
|---|---|---|
| Y5 | R9-A (conditionally frozen, PR155) | condition strengthened; every other clause kept |
| Y5 | R9-B, G-30 (A)/(B) | **words** change ("applied position" becomes the R9-A′ report), because the old words would be literally false |
| Y5 | R9-C, R9-F/G-30 (F), R2′ rule 2 and S5, R6.3′ item 1, G-11, the HA_DURABLE definition, D-03 | meaning moves: a copy counts once watermarked; `ack_p` and `c_m` lag |
| Y5 | P5's excluded silent-loss clause | **not closed**: R9-A′ is necessary, not sufficient; a live promoted member with a lost write, and FM-9, stay uncovered (contract 5.3) |
| Y6a, Y6b | OD-7, OD-8, OD-9; D-06 items 4-14 (conditionally accepted) | realized, not replaced; E26 widens V8-2 |
| Y6c-1 | R6.4's scope; D-06 4.1 "two streams reseeding clear independently" | whole node; E28 |
| Y6c-2 | R6.5's first sentence (treated as frozen); through it R2′ rule 3, R6.3′ item 1, P6, R2′'s promotion evidence, I2-I5 | the flag no longer means "positioned"; a window in which R7 may promote `k` outside S (G-35) |
| Y8 | G-48 (proposed), design requirement 2 | upheld instead of weakened; boundary rule 5's "fail-closed" behaviour is replaced |

`m = 2`, R9-D, R9-E, P1-P4, P6's attestation, P7 and S1-S4 are unchanged by every decision.

## 4. Supporting evidence, and E-1 to E-3

| Claim | Evidence |
|---|---|
| record, watermark, binding, R-CHAIN, completion | bounded TLC runs (`Completion.tla` rev 3, `MetaWatermark.tla` revs 2-4, `SegChain.tla` rev 5) [FACT-MODEL]; not re-run in round 3 |
| V8-L's premise (lock released only after the bio completes) | Linux v6.8 and v6.12 source reading (note A.1, F.2); **no experiment** |
| R9-A′, the R6.5 revision, the twelve-step order, Y8's corrections | argument only (section 5) |
| prior art | CTRL (FAST 2018) and etcd v3.5.17's WAL chain: support the direction, replace no mechanism (contract 8) |

**E-1, E-2, E-3 are designed not to modify or destroy production data, once a run card keeps every safeguard of
contract section 9** [INF, on paper; round 3 found E-1 and E-2 unsafe as #170 wrote them, round 4 made the
safeguards exact]:
- **E-1:** a capped scratch file on the root file system (never `/srv/snc-data`, `/boot` or the old HDD), attached as
  a loop device by the printed path; `modprobe`, `dm-delay`, `mkfs`, the mount and the tracepoint only on it and all
  reverted; disks named by model, serial or UUID; a before-and-after check. Another kernel is E-1b, a separate
  authorization.
- **E-2:** files in a `mktemp -d` directory on the data SSD's file system after its identity is checked, at most
  4 GiB, never the raw device; no SNC process before each phase; only that directory removed. It measures the device cost of one extra write only: **not X-2, not an HA_DURABLE or end-to-end result, and a
  pass is not acceptance.**
- **E-3:** an allowlist of read-only commands; `sdparm --set`, `hdparm -W`, sysfs writes and `/etc/apt` changes
  forbidden.

None was run. Each still needs its own authorization (and an R640 authorization if run there).

## 5. Remaining assumptions, risks and blockers

**Assumptions not verified [INF/HYP]:**
- the R640 SSD has power-loss protection or honours FLUSH/FUA (`WCE 1`, `fua=1` seen; PLP **not verified**);
- Ubuntu `6.8.0-149`'s patches do not change the V8 paths (upstream read, Ubuntu not);
- device timeouts bound the V8-L wait (not read; G-48 bound open);
- 512-byte sector atomicity; ext4 `data=ordered` with barriers.

**Cross-mechanism invariants supported only by argument** (Codex 2D; no model, no test):
1. phase 1 writes nothing (needs E27's code moves; atime aside), and the same record generation is chosen before or
   after step 5;
2. V8-L: "lock granted" means the dead process has no request left on that file;
3. step 9's sync and raise hide no loss and add no acknowledgement;
4. phase-2 deletions destroy no evidence; only a fault produces a phase-2 refusal; orphans hold no evidence;
5. PENDING: `L < retain_from − 1` arises only from damage; `max_lsn = start_lsn − 1 = L` (rests on a proposed
   `max_lsn`); at `L = retain_from − 1` FL-floor verifies nothing independent;
6. completion is idempotent (I5); B.4 adoption matches phase 1's "read as adopted";
7. a lost watermark write costs no coverage;
8. lock order: no deadlock, including an in-process restart;
9. space: one segment per stream of headroom suffices (2.5); A.6 block reuse;
10. R9-A′ is sufficient for its purpose; the primary's own copy is covered (write-path rule 3);
11. I2-I5 hold for a member that is complete and unpositioned after RS-6;
12. a crash during a reseed never refuses; `next_lease` never reissues a lease ID;
13. Y8's seed marker is safe: a node with the marker present never served.

None of the three models covers the twelve-step order (`MetaWatermark.tla:22`: "Recovery is terminal").

**Blockers to architectural acceptance:**
- **B1** D-02 on HOLD, R-23 open, O-FLOOR open (M-1 not authorized).
- **B2** V8-L unverified beyond source (E-1, E-3).
- **B3** I2-I5 not re-argued for the post-RS-6 state (Y6c-2 cannot be accepted before).
- **B4** the two crash-only refusals (Y8), until corrected or excepted.
- **B5** reseed: pins (E29), the lease-block case at RS-6, no bound on reseed loops under term changes, a remap on the
  primary mid-transfer, whether reports carry `(term, lsn)`.
- **B6** the buffered fallback after a write error (FM-12); refusal exit semantics under a supervisor (contract 3).
- **B7** today's code nonconformances, recorded not fixed: the `links_generation` retry, the read-`EIO` loop, the
  early writes (E27).
- **B8** no G-48 number; the watermark's cost (E-2) and Y7.
- **B9** (round 4) premise P: a primary's active checkpoint equals its current log applied through `max_lsn`; the
  reseed's Lemma C depends on it, and it rests on D-02.

## 6. Round 3: focused re-review of the corrected contract

Three reviewers, independent, each read the revised text at `1558aa6` (not the earlier summaries) and was told to
break it: **rr1** recovery classification, locks and the fault budget (contract 1-3); **rr2** R9-A′, reseed and step 12
(5, 6, the note's C); **rr3** whole-document consistency, the G-48 seed texts, and E-1 to E-3's data safety. No
BLOCKER; 12 MAJOR. Every MAJOR was fixed on site in the contract or the note; **these fixes are not re-reviewed.**

| Rev | MAJOR finding | Fixed |
|---|---|---|
| rr1 | step 9's raise relied on `RWF_DSYNC`, which is off with `wal_sync=none` and absent in the buffered fallback: `W > E` after a power loss | step 9 syncs `(W, E]` first; 12.1 and 12.2 corrected; buffered case UNRESOLVED |
| rr1 | crash between `engine.cpp:1074` and `:1080` refuses until an operator acts | section 3 row C2; E31; Y8 |
| rr1 | orphan removal undefined with no checkpoint; could delete acknowledged WAL | step 8: by stream count, only with a checkpoint; otherwise refuse |
| rr1 | FM-10 "Group: yes" exceeded the budget (FM-3 on the survivor) | FM-10 conditional; 1.1 says FM-9 is uncovered |
| rr2 | I2-I5 not argued for "complete, unpositioned" | 6.6 states the gap and the intended argument; B3; Y6c-2 conditioned |
| rr2 | 6.6 dropped "or reseeding" from R6.3′ item 1 | quoted in full; both words mean the flag |
| rr2 | the revision cited R2′ rule 2 and R1 for protections they do not give; LSN-only reports | revision text: `k` sends no report until positioned; `(term, lsn)` UNRESOLVED |
| rr2, rr3 | the note's C.2/C.3 still said RS-6 positions and checked "not in A" | note amended on site |
| rr3 | the seed refusal was marked as damage and called an accepted exception | section 3 kind C, rows C1-C2; section 2 here; markers in boundary rule 5, design 0 and `docs/REPLICATION.md` |
| rr3 | the state machine had no seed path | step 3 notes it; E30 adds SEED |
| rr3 | section 9 claimed the composition was covered by models | rewritten: the twelve-step order is unmodelled |
| rr3 | E-1 and E-2 could touch real devices | contract 9 safeguards; section 4 here |

MINOR findings fixed on site: R9-B and G-30 word changes; 5.2 rows (R9-F, HA_DURABLE definition, SA section 3, the
primary's copy); boot read before the reseed (step 11); step 8 skips RESEEDING streams; step 12's TEXT ONLY basis;
RS-1/RS-6 failure branches; RS-6's `next_lease` includes `k`'s own; pin end at "done"; phase-2 refusal list;
FL-floor at `L = retain_from − 1`; read `EIO` on void bytes; INIT's `init` dependency and phase 2; missing watermark
for PENDING and RESEEDING streams; "one outcome"; descriptor wording; migration precondition; refusal exit
semantics; FM-7's O-FLOOR exception; section 4 statuses (rows 3 and 7 partial); citations (`storage.cpp:52`,
`seed.cpp:271`, `engine.cpp:943-946`, `:1533`).

**Checked and found sound:** no safety claim exceeds the budget; the watermark is never said to protect against FM-9;
FM-8 file-level rollback is never claimed; 5.3 does not claim R9-A′ closes P5's clause; record generation selection
and restart convergence in phase 2; the open-lock-close barrier for spares, `seg.tmp`, `wm.tmp` and io-wq requests;
the PENDING arithmetic; RS-1's compare, RS-0's lease drop, RS-2's discard, RS-6 as one write, crashes at RS-1 to RS-6;
the model citations and their stated limits.

**UNRESOLVED from round 3:** everything in section 5's blockers, plus whether the owner prefers the exception to
Y8's corrections.

## 7. Round 4: Codex's targeted final review (after the owner decisions)

Codex's #171 review, section 2, asked for a narrowly scoped final review of the corrected text on seven points, with
remaining counterexamples recorded. Three independent reviewers, each told to break one part: **f1** step 9, no
active checkpoint, the seed marker and links-generation order, the fault model's independence; **f2** the state
after RS-6 (`docs/AFR_001_R65_INVARIANTS.md`, written for Codex's first priority) and the recorded R9-A′; **f3** the
experiment safeguards, the decision record and the public-repository rules. No BLOCKER; 17 MAJOR. Each was fixed on
site as below, **not re-reviewed**. Nothing changes an accepted decision: the fixes sit inside the accepted
directions (OD-10 to OD-14) or in proposals; the fault model is narrowed, never widened.

**Counterexamples found, and what now stops each** [INF]:

| # | Counterexample | Now |
|---|---|---|
| 1 | step 9's sync fails with `EIO`; the process restarts; the kernel reports the writeback error only once, so the next sync "succeeds" over cached bytes not on media, and the raise covers them (f1) | a failed step-9 sync is a named refusal, not restarted (contract 2.2 step 9, section 3) |
| 2 | that case on node A plus FM-10 on node B loses an HA_DURABLE write within each node's budget (f1) | FM-10's proviso counts such a write error as A's media fault |
| 3 | a checkpoint exists but its manifest's stream count differs from the record's: an orphan holding acknowledged WAL is neither removed, replayed nor refused (f1) | step 5 refuses every orphan step 8 may not remove; section 3 row |
| 4 | `wal_sync=none`: the old boot already raised `W` over dirty pages, so step 9's sync of `(W, E]` repairs nothing; and the "buffered fallback lacks `RWF_DSYNC`" premise was wrong (`runtime.cpp:231`) (f1) | step 9's sync is a guard only; `wal_sync=none` stays outside every guarantee |
| 5 | SEED removed "everything else", including a `links_generation` the operator kept for V7: incarnation regresses, peers refuse (f1) | SEED removes only the seed's own files |
| 6 | a primary that positioned `k` before an in-place reseed still counts it positioned in U (no boot write, so no new incarnation) (f2) | U4: RS-1 takes a new incarnation (contract 6.5) |
| 7 | `j`'s checkpoint, taken at a fuzzy `max_lsn` not durable on `j`, then a crash and a new `(t′, y)`: the copied state and copied WAL disagree (f2) | premise P stated; Lemma C depends on D-02 (new blocker B9) |
| 8 | E-1 on the R640 had no permitted place, and `losetup -D` or `/dev/sdX` could hit system or production devices (f3) | contract 9: exact place, caps, identity checks, named teardown, before-and-after check, E-1b |

**Other MAJOR fixes:** Y9 restated as defence in depth (G-12 keeps a second, positioned copy in S, so G-11 holds;
f2); U defined across restarts (f2); "R6.2 truncates the lost tail" corrected to "R6.4 reseeds again" (f2); the
record no longer states step 9 as decided in Codex's Y5 wording (f3); G-30 (A)/(B) keep their earlier words (f2, f3);
section 4's safety claim made conditional (f3); this section written (the dangling pointer, f3).

**MINOR fixes:** E31 for non-cluster directories; the marker in step 3's lists and its unlink in step 8; FM-7 not
widened for the marker; in-place pointers at R9-B, R9-F, G-11, the HA_DURABLE definition and R6.3′; SA's freeze
note; the M_ack citation's scope; the contract header; verbatim quotes in OD-12 and OD-14; "the one" labelled as the
recorder's reading; R9-A′'s freeze status and its dependence on D-02 noted; E-3 fetches the kernel source elsewhere;
the liveness bound for a member in A before positioning marked UNRESOLVED.

**Checked and found sound:** the decision record matches Codex's wording and strength, with Y6c-2 accepted nowhere;
the Codex filing has no paste damage; R9-A′ keeps every clause but the condition; nothing claims P5's clause closed,
FM-9 covered or FM-8 file-level covered; with no checkpoint, nothing deletes acknowledged WAL; the marker never wipes
a node that served, within the budget; E31's order closes the `engine.cpp:1074`/`:1080` window; within U, I1, I2
(for the WAL slice), I3, membership, promotion inside S, leases, RS-6's atomicity and U3 hold; no model names,
secrets or new personal names.

**New blocker B9:** premise P, that a primary's active checkpoint is its current log applied through `max_lsn`,
rests on D-02. **Y6c-2 recommendation:** accept, with U3 and U4 added to its text, once P is part of D-02's
acceptance.

**UNRESOLVED from round 4:** premise P; ext4's writeback-error reporting on v6.8 and Ubuntu 6.8.0-149 (not read);
whether a record can name a stream count different from the active manifest's; the slot-epoch wipe order (V7);
whether the seed's record files are preallocated before the first slot write; whether `block_rq_complete` times
completion correctly under a loop-backed `dm-delay`; no model covers U, R1.1's add before positioning, or R7 outside
S; no bound on reseed loops; whether reports should carry `(term, lsn)`.

## 8. Premise P: the exact evidence needed (Codex #172, section 5 item 2)

**P:** a primary's active checkpoint is its current log applied through the manifest's `max_lsn` for each stream.
`docs/AFR_001_R65_INVARIANTS.md` (Lemma C) and so Y6c-2 depend on it (B9). It decomposes into four rules; P holds if
all four hold [INF]:

| # | Rule | Where it stands | Evidence that discharges it |
|---|---|---|---|
| P-1 | `max_lsn` is the stream's last assigned LSN when the part's walk ends, so the part reflects nothing above it | D-06 4.3 step 1 and boundary rule 7: proposed, inside the conditionally accepted D-06 items 4-14 | text: already argued (boundary review finding 1); final with Y4 |
| P-2 | the part is published only once every record up to P-1's `max_lsn` is locally durable (C5) | **does not hold today**: C5 waits on today's `max_lsn`, the highest visited image's `last_lsn` (`shard.cpp:2654`), which misses deletes (`:2637`; boundary rule 7) [FACT, round 5], **and on a standby misses every replicated record**: `apply_replicated` installs each image with `last_lsn = 0` (`shard.cpp:2189-2192`, `:1812`), so a standby's C5 waits on nothing, as does any node for objects untouched since boot [FACT, round 6] | a C5 change to wait on P-1's value (an implementation change, not authorized), then review; with `wal_sync=none` it never holds (outside every guarantee) |
| P-3 | recovery refuses when a stream's valid end `E` is below the active checkpoint's `max_lsn` | **not written**: contract 2.4's NONE check reads the part but does not compare `E` with `max_lsn` | the check in contract 2.4, with `E = max(last valid LSN, retain_from − 1)` (round 6: an idle stream has no last LSN), which needs D-06 section 7's manifest `max_lsn` (proposed; stored nowhere today), P-2, and a seed that copies the WAL through `max_lsn` before its record write (E30, round 5: otherwise a freshly seeded node refuses with no fault); then review. With those, the refusal follows only a fault (G-14(a)), never a silently shorter log |
| P-4 | no entry at or below `max_lsn` is replaced while that checkpoint is active: a primary never truncates its own log (SA I4 step 2), and a member whose truncation point is below `max_lsn` is reseeded instead (R6.4; D-06 4.3 step 1, `L ≥ max_lsn`) | frozen (R6.4) and proposed (D-06 4.3) | text: holds now |

**The composition.** P-1 and P-2's defects were facts about the code, not interleavings (boundary review), so a
code and text check finds them, not a model. The interleavings (publication against crashes, truncation and reclaim)
would need M-1 **extended** with object state, the fuzzy walk and deletes (`Completion.tla` has none): a change of
M-1's scope, needing its own authorization. Not proposed now.

**So P is discharged by:** P-1 accepted (Y4), P-2 written as a C5 rule and later implemented, P-3's check and
E30's WAL copy reviewed, P-4 as it stands, and D-02's acceptance including them; a model only if the owner extends
M-1. Until then Y6c-2 stays conditional. Nothing here is run.

## 9. Round 5: focused review of the round-4 corrections, Y9 and premise P

Codex's #172 review asked for one focused review of the round-4 corrections, reporting only new substantive
findings. Three independent reviewers: **g1** the corrected contract sections; **g2** Y9
(`docs/AFR_001_REPORT_IDENTITY.md`); **g3** premise P's evidence and package consistency. **2 BLOCKER, 11 MAJOR**,
all new; each fixed on site as below, **not re-reviewed**.

| # | Finding | Now |
|---|---|---|
| g1 B | the new `E ≥ max_lsn` check refuses a freshly seeded node with no fault: the seed copies no WAL | E30: the seed copies the WAL through `max_lsn` before its record write; recovery restarts at step 1 after a SEED or INIT record write |
| g3 B | P-2 does not hold today: C5 waits on today's `max_lsn`, which misses deletes | P-2 marked "does not hold today", a C5 change (not authorized); the 2.4 check names D-06's proposed manifest field |
| g1 | the orphan refusal followed crashes alone (a remap's crash before its record write; a boot write that changes `n_streams`) | orphans with no record are removed; `n_streams` changes only with the remap's checkpoint; a record-holding orphan is a configuration refusal, never after crashes alone |
| g1 | RS-1's new incarnation changes conditionally accepted D-06 item 7 without saying so | E32, an owner decision with Y6c-2 |
| g1 | the budget counted FM-12 in one place and not another | budget paragraph and FM-12 row: an error that can leave a write only in the page cache is the node's media fault |
| g1 | E-1's scratch file on `/` writes the boot device | `tmpfs` for the run; no disk written |
| g2 | **a delayed report from a node that then reseeds, counted by a primary that missed the shrink, breaks G-11**; positioning alone and Y9-B both fail | RS-0: `k` stops granting and RS-2 waits `W` after its last grant (contract 6.5) |
| g2 | Y9-T's soundness relied on R3 fencing `p` first, which is false | replaced by the A ⊇ S argument |
| g2, g3 | Y9-T for a node in U depends on premise P; U1's report clause must stay | U1 kept verbatim; dependency stated |
| g2, g3 | positioning alone was shown broken without a stated severity | under `m = 2` it breaks I4 through a Grow (REPORT_IDENTITY section 2) |
| g3 | "one more property in M-1" understated the scope | M-1 would need extending (objects, the walk, deletes); not proposed |

**MINOR fixes:** status lines in this package and the R6.5 analysis; the media-fault bullet marked D-02-dependent; no
check with no checkpoint.

**UNRESOLVED from round 5:** what `n_streams` today's boot write carries during a remap; whether the links generation
must advance when RS-2 discards memory in place; whether a reporter must have persisted `p`'s term; P-1 for a
checkpoint taken while the node was a replica in an earlier term; writeback-error reporting on Ubuntu 6.8.0-149; RS-0's
grant wait adds `W` to every reseed.

**Updated owner questions (none recorded):**
- **Y9 (proposed: Y9-T).** Replace R9-A′'s report clause as in `docs/AFR_001_REPORT_IDENTITY.md` section 3: a report
  names its entry `(term, lsn)`; `p` counts it only if its own log holds that entry; no report while an intent is
  pending or the flag is set. **Recommendation: yes**, after Codex's review; argument only.
- **Y6c-2 (still conditional).** Its text now carries U3, U4 (E32), and RS-0's grant wait. **Recommendation: hold
  until premise P is discharged** (section 8), as Codex requires.
- Y4, Y7: HOLD. The V8-L experiment (E-1, E-1b), the watermark-cost experiment (E-2) and E-3 stay proposals with exact
  safeguards (contract 9), each for separate authorization; not started.

## 10. Round 6: targeted review of the round-5 corrections (Codex #173)

Codex's #173 review (`docs/handoff/PR173_Codex_Review.md`) asked for one targeted independent review of six areas.
Three reviewers, each told to break the work: **h1** Y9-T and the RS-0 grant wait; **h2** premise P (P-2, P-3) and
the seed's WAL ordering, with the exact code changes; **h3** orphan directories, the RS-1 incarnation and the write-
error budget. **4 BLOCKER, 6 MAJOR**, all new; each fixed on site as below, **not re-reviewed**. [INF unless marked.]

| # | Finding | Now |
|---|---|---|
| h1 B | a report sent before an in-place reseed counts once `k` is grown back and grants again; `(term, lsn)` alone does not stop it, and U4 had no effect on counting (REPORT_IDENTITY B-6a) | the report carries the reporter's incarnation; `p` counts it only under the incarnation it positioned (Y9-T revised) |
| h1 B, h3 B | RS-0's wait does not survive a crash: the grant time is in memory, and step 11 discarded at once (B-6b) | RS-1 is written only after `W` from the later of the last grant and the boot; RESEEDING grants nothing; granting resumes after RS-6 and R6.2 |
| h3 B | a grow remap's crash refused after crashes alone: `finish_recovery` (writers, re-seal, resume, links) runs before the remap checkpoint (`engine.cpp:1235-1254`, `shard.cpp:1844-1853` [FACT]), so a follow-up record lands in a new stream; "start with the old shard count" did not clear it | new step 9a: no record reaches any stream until the record write naming the remap's checkpoint; the orphan refusal is reachable only by FM-15; today's order recorded as a nonconformance |
| h2 | P-2 understated: a standby's C5 waits on nothing (`last_lsn = 0` on every replicated install [FACT]) | P-2 row; tests must cover a standby checkpoint |
| h2 | `E` undefined for a stream with no retained record: the 2.4 check, read literally, refused a clean restart of an idle stream | `E = max(last valid LSN, retain_from − 1)` |
| h2 | boundary rule 5 and D-06 4.2 ("an empty segment `start_lsn`") contradicted E30 | E30 states the segment, the manifest and a contiguity check; both texts marked amended |
| h2 | RS-6's `max_lsn` had no source with no writer open (0 would pass `L ≥ max_lsn`) | RS-6: at least RS-5's copied end |
| h3 | the buffered WAL spent the one-fault budget silently: after a write error, a restart reads cached pages, step 9's sync succeeds (the dead process consumed the error), and the node reports them | the buffered WAL is outside every guarantee, as `wal_sync = none` is, proposed refused at start for a group member; the round-5 "counts as the media fault" clause removed |
| h1 | RS-0 named no end for "stops granting" | granting resumes after RS-6 and R6.2 in the new incarnation |

**MINOR fixes:** E30 says "durably"; no checkpoint before step 9 ends for every stream; `shard.cpp:2637`, not
`:2636`; `runtime.cpp:231` is the synchronous fallback, not the buffered path; E32 lists R6.3′ item 1 and SA's
"boot incarnation" and restarts the request number; stale "closed" claims in the R6.5 analysis and REPORT_IDENTITY
corrected; `ENOSPC` on `O_DIRECT` spends no budget.

**Partly resolved:** round 5's "P-1 for a checkpoint taken as a replica". LSNs are assigned on the owning shard's
thread (`include/snc/storage.h:201` [FACT]), a standby appends before it installs (`shard.cpp:2189-2192` [FACT]), and deletes
are logged, so with P-1 defined as `next_lsn − 1` sampled when the walk ends, the part reflects nothing above it
[INF]. Its validity after a promotion stays with P-4.

**UNRESOLVED from round 6:** whether "caught up" is judged from reports (then the incarnation check applies there
too); whether a resume follow-up always writes before today's remap checkpoint (h3's B holds if any record path runs
in that window); which field defines "covers the remap"; whether ext4 can fall back from `O_DIRECT` to buffered
within a write (would put the buffered case on the default path) [HYP]; adding `max_lsn` to the manifest bumps
`kFormatVersion`, and `read_manifest` rejects other versions (`storage.cpp:332` [FACT]; D-06 section 6). Carried:
everything in section 9's list not resolved above; no model covers U, Y9-T or delayed reports.

### 10.1 The code changes P-2, P-3 and E30 will need (identified, not written, not authorized)

P-2 (C5 waits on the last assigned LSN):
1. `shard.cpp:2654` uses the largest image `last_lsn`. Sample `c.max_lsn = wal_ ? wal_->next_lsn() − 1 : 0` once
   where the walk ends (`c.visited = true`, `shard.cpp:2686`); keep the image maximum only as an assertion `≤` it; do
   not re-sample at the wait.
2. The wait at `shard.cpp:2727` (`lsn_local_durable(c.max_lsn)`) keeps its form. [Round 7: a checkpoint taken with
   no writer open (step 9a, RS-6) takes `start_lsn` and `max_lsn` as parameters, `E + 1` and `E`, not from `wal_`;
   `begin_checkpoint`'s `c.start_lsn = wal_ ? … : 0` (`shard.cpp:2319`) changes too.]
3. `CheckpointResult` (`shard.h:316-318`) gains `max_lsn`, filled at `shard.cpp:2560`.
4. `Manifest` (`include/snc/storage.h:55-72`) gains a per-stream `max_lsn` (with D-06 section 7's fields), written and read in
   `write_manifest`/`read_manifest` (`storage.cpp:286-358`) with a `kFormatVersion` change, filled at
   `engine.cpp:1490-1495`.
5. Tests: a delete during the walk with its WAL flush held, asserting no SYNC until `durable_lsn` reaches it; the
   same on a standby; a manifest round trip.

P-3 (recovery refuses `E < max_lsn`):
6. `E_s = max(last valid LSN of the H pass from retain_from, retain_from_s − 1)` (round 7: not the replay scan of
   `engine.cpp:1124-1129`, which starts at `start_lsn`; and not `from_lsn − 1`, which assumes records it never
   read); refuse by name when `E_s` is below the manifest's `max_lsn[s]`. It
   runs in phase 1, before any write, so the order before `engine.cpp:1074` moves (E27).
7. Tests (`test_crash.cpp`): a WAL tail cut below `max_lsn` refuses; an idle stream left with a header-only segment
   `start_lsn` recovers; a standby checkpoint then a cut tail.

E30 (seed copies the WAL through `max_lsn`):
8. Sender (`seed.cpp:141-160`): after the parts, send records `start_lsn..max_lsn` per stream (`read_stream_range`,
   `storage.cpp:439`).
9. Receiver (`seed.cpp:236-273`), before `write_superblock` (`:273`): the marker; segment `start_lsn` with the
   records, `fdatasync` and directory `fsync`; last LSN equals `max_lsn`; watermark and links-generation files; the
   manifest keeps `max_lsn`. The SEED outcome replaces `engine.cpp:943-946`.
10. Test: kill after the record write and before the marker unlink; the restart proceeds with `E ≥ max_lsn`.

Step 9a (remap order) and the buffered-WAL refusal are further code changes of the same kind, also not written.

### 10.2 Owner questions (none recorded)

- **Y9 (proposed: Y9-T as revised in round 6).** Replace R9-A′'s report clause with
  `docs/AFR_001_REPORT_IDENTITY.md` section 3: a report names its entry `(term, lsn)` and carries the reporter's
  incarnation; `p` counts it only if its own log holds that entry and the incarnation is the one `p` positioned; no
  report while an intent is pending or the flag is set. Together with RS-0's revised wait (contract 6.5), which is
  part of Y6c-2's text. **Recommendation: yes, as a design direction, after Codex's review of round 6.** It revises
  frozen R9-A′ (OD-10) and the replication `Ack` frame (a wire-format item); argument only, no model.
- **Buffered WAL outside every guarantee (new, round 6).** **Recommendation: yes**: it removes a silent budget
  spend and costs nothing on the default configuration (`wal_direct = true`).
- **Y6c-2:** still **hold until premise P is discharged** (section 8), as Codex requires. Its text now also carries
  RS-0's revised wait and the RS-6 `max_lsn` rule.
- Y4, Y7: HOLD. E-1, E-1b, E-2, E-3 and any M-1 change stay proposals, each needing separate authorization.

## 11. Round 7: re-review of the round-6 fixes (Codex #174)

Codex's #174 review asked for one focused adversarial review of six areas. Three reviewers, each told to break the
work: **k1** reporter incarnation and RS-0 timing; **k2** P-2, P-3, E30 and step 9a; **k3** the buffered WAL,
answering Codex's five questions. **2 BLOCKER, 6 MAJOR**, all new; each fixed on site, **not re-reviewed**. [INF
unless marked.]

| # | Finding | In text from | Now |
|---|---|---|---|
| k2 B | the step-9a and RS-6 checkpoints, taken with no writer, record `start_lsn = 0` (`shard.cpp:2319` [FACT]); the next clean boot reads every old segment and stops on "record in the wrong shard's stream" (`engine.cpp:1104` [FACT]); reclaim never runs; `retain_from − 1` wraps | round 6 (9a, RS-6) | `start_lsn = E + 1`, `max_lsn = E`, set explicitly; 10.1 item 2 |
| k2 B | a grow remap's record names new streams that have no segment until step 10; a crash in between makes FL-floor refuse | round 6 (9a) | each added stream gets its watermark file and an empty segment 1 durably first, with its floor values |
| k1 | a delayed report counts across two term changes within one incarnation (truncated against `j′`, counted by `j″` after an R7 promotion); round 6's "the reporter's term is not needed" was false | round 6 (Y9-T) | the report carries the reporter's durable term; counted only if equal to `p`'s (REPORT_IDENTITY B-7a) |
| k1 | the incarnation is not unique across a wipe and a fresh seed, which restart the generation | older (D-06 4.1), load-bearing since round 6 | incarnation = (node nonce drawn at every init and seed, generation); E32 and D-06 4.1 marked |
| k2 | `E`'s "last valid LSN" came from the replay scan, which starts at `start_lsn`: an idle stream with a pinned old segment refused a clean restart | round 6 (2.4) | from the H pass over every retained segment |
| k2 | the step-9a checkpoint runs session expiry, whose deletes go unlogged with no writer (`shard.cpp:533-534` [FACT]): primary and standby diverge | round 6 (9a) | step 9a's checkpoint mutates nothing and reclaims nothing |
| k3 | "the `O_DIRECT` WAL leaves nothing cached" is false when the kernel falls back to buffered inside the call (`-ENOTBLK`, ext4 and xfs, v6.8 [FACT-KERNEL, read by the reviewer]) | round 6 (budget) | named as a residual premise (rare [HYP], needs cached pages of the active segment, which catch-up reads create) |
| k3 | "refused at start" refused nothing that can happen: the server cannot set `wal_direct`, the `EINVAL` fallback is silent, no file-system check exists [FACT] | round 6 (budget, 2.7) | a concrete proposed gate (2.7): statfs check, `EINVAL` fatal, latched counter |

**MINOR fixes:** RS-0's "grant only after R6.2" (unenforceable after a crash, and not needed: U4 and R4) replaced;
"current boot" defined as the directory lock's time on `CLOCK_BOOTTIME`; the errseq wording; `ENOSPC` with a
preallocated segment; the `pwritev2` fallback's other triggers; RS-6's durability rests on RS-5's sync; step 9a's
triggers; reclaim skipped in step 9a.

**Recorded, not fixed:** `tools/control.h:153-156` ignores a directory `fsync` result (nonconformance [FACT]); E30's
"fetch again" depends on E29's pins (`engine.cpp:1516`), so a seed of a busy primary can loop.

**UNRESOLVED from round 7:** whether acknowledgements in terms after an R7 promotion outside S fall under I4 (decides
B-7a's severity; the field is added either way); whether watermark files are written `O_DIRECT`; the kernel
`-ENOTBLK` path's real frequency; what field defines "covers the remap"; R-23 can still reuse a generation. G-48 is
**not** closed: step 9a and E30 are argued, not checked.

### 11.1 The buffered WAL: Codex's five questions

| Question | Answer |
|---|---|
| Can every supported platform guarantee direct I/O? | **No, not fully.** Alignment holds (4096-byte blocks and buffers, `include/snc/wal_format.h:9`, `storage.cpp:30` [FACT]). But ext4 and xfs complete an `O_DIRECT` write through the page cache when invalidation fails (`-ENOTBLK`), and ext4 also for data=journal, inline data, verity or some encryption [FACT-KERNEL]. Only the first can occur on the supported defaults, and only with cached pages of the active segment [HYP: rare] |
| Can the code fall back silently? | **Yes.** `EINVAL` at open reopens buffered with no log and no counter (`storage.cpp:543-550`, spares `:779-784`); a spare can be buffered while the segment is direct; the gauge shows only the current segment [FACT]. The kernel's own fallback is invisible to the code |
| Is an unsupported configuration refused at start? | **No.** No file-system check exists; the server does not expose `wal_direct` or `wal_sync` and always runs the defaults (`tools/snc_server.cpp`, `include/snc/snc.h:373-374`); only the bench and tests change them [FACT] |
| Does the `pwritev2` fallback keep the flags? | **Yes**: the same `RWF_DSYNC` and kernel write flags as io_uring (`runtime.cpp:213`, `:231`); short or failed writes are fatal (`storage.cpp:821-824`); headers are written and `fdatasync`ed; padding is in the same write [FACT] |
| Does the exclusion change an accepted guarantee? | **No accepted guarantee mentions buffered I/O** [FACT, search]. SNC_GUARANTEES 1.3 already requires a weaker configuration to be refused at start unless the operator explicitly accepts it; the proposed gate applies that rule. It narrows no product guarantee [INF]; earlier tmpfs runs were never durability evidence |

So the exclusion is **stated as an owner decision**, not applied silently: today's text is a proposal, and nothing
in the code enforces it.

### 11.2 Is the contract converging? Findings per review round

Counts are from each round's own table. "From the previous round's fixes" means the finding sits in text written to
fix the round before; "new this round" means a proposal first written in that round; "older" means earlier text or
the code. Rounds 1-4 were not classified at the time and are not reclassified here.

| Round (PR) | Scope | BLOCKER | MAJOR | From previous fixes | New this round | Older |
|---|---|---|---|---|---|---|
| 1 (#170) | first draft of the contract, the corrected reseed note | 0 | 10 | not classified | | |
| 2 (#170) | the integrated contract | 0 | 23 | not classified | | |
| 3 (#171) | round-2 fixes | 0 | 12 | not classified (by scope, mostly fix text) | | |
| 4 (#172) | corrected sections after the owner decisions, the R6.5 analysis | 0 | 17 | not classified | | |
| 5 (#173) | round-4 fixes, Y9, premise P | 2 | 11 | 4 | 7 (1 B) | 2 (1 B) |
| 6 (#174) | round-5 fixes | 4 | 6 | 7 (3 B) | 0 | 3 (1 B) |
| 7 (this) | round-6 fixes | 2 | 6 | 7 (2 B) | 0 | 1 |

**Reading [INF, the recorder's].** The MAJOR count is falling (23, 12, 17, 11, 6, 6), but BLOCKERs appeared only once
reviews were narrowed to counterexamples (rounds 5-7), and in rounds 6 and 7 most findings sit in text the previous
round added. Each fix to a protocol interaction (the grant wait, the incarnation, the report's fields) or to the
recovery order (step 9a) has added a rule with its own surface. On the storage side the defects were facts about
the code that a reader found once pointed at them; on the replication side they were interleavings (delayed
messages, crashes, term changes) that prose review finds one at a time. **The contract is not converging by text
review alone in the report/reseed/grant area.**

### 11.3 Owner questions (none recorded; supersedes 10.2)

- **Y9 (proposed: Y9-T as revised in rounds 6 and 7).** A report names its entry `(term, lsn)` and carries the
  reporter's incarnation (node nonce and generation) and durable term; `p` counts it only if its own log holds the
  entry, the incarnation is the one `p` positioned, and the terms are equal; no report while an intent is pending or
  the flag is set; applied and watermarked as R9-A′ requires. With RS-0's revised wait (part of Y6c-2's text).
  **Recommendation: hold the yes until the bounded model below has run**, because rounds 6 and 7 each found a
  counterexample in the previous round's version. It revises frozen R9-A′ (OD-10) and the `Ack` wire format; no
  wire change is made.
- **A bounded model of the report/reseed protocol (new, my recommendation for the next step).** Extend the existing
  log and authority models only as far as Y9-T needs: reports as delayed and duplicated messages carrying (entry,
  incarnation, term), the RS-0 wait and RS-1, R6.2 positioning, a Grow, crashes, and an R7 promotion; small bounds
  (3-4 nodes, terms ≤ 3, LSN ≤ 3), preregistered, with mutations that must fail (no incarnation, no term, no wait).
  It would settle whether the rule set is complete, which three rounds of prose have not. **Needs your explicit
  go** (no model is authorized). Storage-side items (step 9a, `E`, `start_lsn`, E30) are code facts, better settled
  later by the 10.1 changes under crash tests, also needing a go.
- **Buffered WAL outside every guarantee, with the gate in 2.7.** **Recommendation: yes**, as a design direction:
  it applies SNC_GUARANTEES 1.3 and narrows no accepted guarantee; the kernel `-ENOTBLK` path stays a named residual
  premise.
- **E32 (incarnation = node nonce and generation of the boot or RS-1 write).** With Y6c-2.
- **Y6c-2:** still **hold until premise P is discharged** (section 8).
- Y4, Y7: HOLD. E-1, E-1b, E-2, E-3 and any M-1 change stay proposals.

## 12. Can the contract rely on `RWF_DSYNC` whatever path the kernel takes? (Codex #175, section 4)

Codex asked whether the kernel's internal fallback from direct to buffered I/O can be handled by relying on
`RWF_DSYNC` semantics independent of the I/O path, and not to claim that a start gate proves every write stayed
direct. One reviewer read the Linux v6.8 sources (`git tag v6.8`, files fetched from the kernel repository); every
row below is **[FACT-KERNEL]** at v6.8 unless marked, and Ubuntu's 6.8.0-149 patches are **not** checked [HYP that
they do not change these paths].

| Question | Answer | Source (v6.8) |
|---|---|---|
| Does a **successful** `RWF_DSYNC` write reach stable media on the fallback path too? | **Yes.** `RWF_DSYNC` sets `IOCB_DSYNC`; `generic_write_sync` then calls `vfs_fsync_range` and returns its error instead of the byte count. ext4 and xfs both reach it after a `-ENOTBLK` fallback; their fsync writes the data, forces the journal or log, and flushes the device cache | `include/linux/fs.h:2731-2742`; `fs/ext4/file.c:579-580`, `:285-305`; `fs/xfs/xfs_file.c:849-861`; `fs/ext4/fsync.c:158-176` |
| And on the direct path into a preallocated (unwritten) extent? | **Yes.** No FUA shortcut for unwritten extents; the sync runs after `end_io`, which converts the extent, and `fdatasync` commits that transaction on ext4 (xfs: forces the log; the detail that the conversion is counted is HYP) | `fs/iomap/direct-io.c:617-631`, `:90-126`; `fs/ext4/extents.c:3871-3876` |
| After a **failed** write on the fallback path, what is cached? | the folios stay **uptodate and clean** in the page cache with bytes not on media; the error goes to the dying descriptor and is marked seen; a **new** descriptor's `fdatasync` then returns 0 and writes nothing | `fs/ext4/page-io.c:118-146`; `fs/iomap/buffered-io.c:1456-1470`; `mm/filemap.c:726-750`; `fs/open.c:912`; `lib/errseq.c:123-131` |
| Does an `O_DIRECT` **read** return the device's contents? | **Yes** on xfs and on ext4 files that allow direct I/O: it writes back dirty pages in the range, then reads the device; clean cached pages are never the source | `fs/iomap/direct-io.c:584-594`; `mm/filemap.c:2677-2690` |
| Are there silent fallbacks with no `-ENOTBLK`? | **Yes, on ext4:** a file with verity, inline data, some encryption, or `data=journal` uses buffered I/O for `O_DIRECT` writes and reads alike | `fs/ext4/file.c:55-67`, `:81-91`, `:534-539`; `fs/ext4/inode.c:5487-5500` |

**So [INF from the FACT rows]:**
- **The success path is path-independent.** A write that returned success with `RWF_DSYNC` is on stable media whichever
  path the kernel took, given the device honours FLUSH and FUA and barriers are on (FM-9's premise, unchanged).
  The contract can rely on that.
- **The failure path is not.** After a fallback write fails, a restart without a reboot can read bytes from the cache
  that are not on media, and step 9's sync reports nothing. That is the round-6 buffered-WAL hazard, on a descriptor
  opened `O_DIRECT`.
- **Smallest rule that makes the contract path-independent (proposed, not accepted):** recovery's scan and every
  catch-up read of a WAL segment use `O_DIRECT` reads, so what recovery trusts is what the device holds, whatever path
  any earlier write took; and a WAL file on which ext4 would not do direct I/O at all (verity, inline data, that
  encryption, `data=journal`) is refused. With that rule:
  - the round-6 exclusion of the buffered WAL is no longer needed for the failure path;
  - the start gate (contract 2.7) remains useful as detection, not as proof;
  - the `-ENOTBLK` residual premise (contract section 1) is discharged.
- **Residual premises:**
  - aligned reads (the code already aligns to 4096 bytes);
  - WAL checksums must reject stale or zero bytes, because ext4 may mark an extent written after a failed writeback
    (`fs/ext4/page-io.c:175-196` does not check the bio status) [consequence HYP];
  - no other reader caches a live segment.
- **Weaker alternatives:** `posix_fadvise(DONTNEED)` is advisory [HYP that it drops these folios]; reboot before
  restart works but costs operations; a mount-option check alone does not stop `-ENOTBLK`.

**Code facts this rule touches (not changed):** the scan reads through buffered `read_file` (`storage.cpp:37`,
`:360-420`), and catch-up reads use `read_stream_range` (`storage.cpp:439`, `replication.cpp:163`). Both would read
with `O_DIRECT`. That is an implementation change, not authorized.

**Owner question (none recorded): Y10, rely on `RWF_DSYNC` plus `O_DIRECT` recovery reads.** Recommendation:
**yes, as a design direction, in place of the round-6 "buffered WAL outside every guarantee" wording.** It narrows no
platform. It keeps a buffered-WAL configuration correct on the success path and safe on the failure path, given the
residual premises. Contract 2.7 already lists `data=journal` as not supported; refusing WAL files with verity, inline
data or that encryption would add to that list, a platform restriction that needs its own yes.

## 13. Owner questions now (none recorded; supersedes 11.3)

- **Run the report/reseed model plan** (`docs/AFR_001_REPORT_MODEL_PREREG.md`): writing the model, one fidelity
  review, uncounted sizing runs, 17 counted launches within the stated budget. **Recommendation: yes.** It is the
  step Codex recommends, and it settles whether Y9-T's rule set is complete. Planning alone already found two text
  gaps, W-1 (operator wipe window) and W-2 (counted coverage must end with its positioning record), recorded in the
  plan's section 6 and proposed only.
- **Y9 (Y9-T as revised in rounds 6-7, plus W-2):** hold the yes until the model has run.
- **Y10, rely on `RWF_DSYNC` plus `O_DIRECT` recovery and catch-up reads (section 12):** [Superseded by section 14:
  conditional direction only; the buffered-WAL exclusion stays until the owner decides.] **recommendation yes**, as a
  design direction, replacing round 6's "buffered WAL outside every guarantee". It narrows no platform. Refusing ext4
  WAL files with verity, inline data or that encryption is a separate platform restriction, needing its own yes.
- **E32**, **Y6c-2** (hold until premise P is discharged), **Y4**, **Y7**: unchanged.

## 14. Y10 verification list (Codex #176, section 3)

Codex supports Y10 only as a conditional direction and asked for six checks before acceptance. One reviewer read
the sources. **The buffered-WAL exclusion (contract section 1, round 6) stays in place;** Y10 does not replace it
until the owner decides.

**Target kernel source.** git.launchpad.net is blocked here, so the reviewer used Ubuntu's source package for
`6.8.0-149.149` from `archive.ubuntu.com` (`linux_6.8.0-149.149.diff.gz`). Its SHA-256 matches the `.dsc`; the
`.dsc` signature was **not** verified. The diff was applied to the v6.8 files, and every hunk applied without offset
or fuzz. Rows marked (U149) are that tree [INF that it equals the original tarball for these files].

| Check | Result |
|---|---|
| 1. A successful `RWF_DSYNC` write, direct and fallback | **Durable on both paths; unchanged on the target.** `generic_write_sync` (fs.h:2750-2761), `iomap_dio_complete` (direct-io.c:83-126), the `-ENOTBLK` source (:653-660), the ext4 fallback ending in `generic_write_sync` (file.c:285-313, :586, :610), the xfs paths (:816, :845-854) and the ext4 journal fsync path (fsync.c:170-183) are identical to v6.8 apart from an added `inode_dio_wait` (ordering only) [FACT (U149)] |
| 2. Can a failed buffered write leave clean, non-durable cached data? | **Yes, on the target too.** `ext4_finish_bio` (page-io.c:118-146) and iomap (buffered-io.c:1495-1510) end writeback without redirtying; errseq unchanged (filemap.c:726-750, open.c:911) [FACT (U149)]. **One target difference:** ext4 does not convert an unwritten extent after a failed writeback (`EXT4_IO_END_FAILED`, page-io.c:189-196, :358), so a failed write into preallocated WAL space reads back as zeros from the device. That removes section 12's "extent marked written" residual for preallocated segments; xfs already skipped it |
| 3. Do `O_DIRECT` reads bypass that cache on the supported configuration? | **Yes**, for ext4 `data=ordered` with extents and no verity, inline data, encryption or per-inode `chattr +j`, and for xfs defaults: the read goes to the device; clean cached pages are never the source (ext4 file.c:55-95, inode.c:5647-5660; xfs_file.c:248-269) [FACT (U149)]. Misaligned reads fail with `EINVAL`, never fall back (direct-io.c:302-304). Per-inode `+j` and encryption are invisible to mount options, so a runtime check is needed (for example `statx` `STATX_DIOALIGN`) |
| 4. Does a direct read start writeback of dirty pages, and what does that do to read-only classification? | **Yes:** it writes back and waits on dirty pages in the range, then tests and clears the mapping's error (direct-io.c:597-609, filemap.c:667-692, :344-355) [FACT (U149)]. A failed writeback makes the read return `EIO`, which today is fatal (`storage.cpp:48-52`); nothing stale is returned silently. **When can pages be dirty at recovery?** Only with `wal_sync = none`, through a buffered or fallback descriptor, or a writable shared mapping by another process; a `RWF_DSYNC` write syncs inside the same call [INF; the window under SIGKILL is HYP]. **Classification:** the writeback is not a write by recovery, and it writes only bytes the kernel had already accepted, which the background flusher would write within about 30 s anyway, so phase 1 is no less read-only than today's buffered scan [INF]. **UNRESOLVED (a design choice):** whether phase 1 must require "no dirty pages", for example by refusing after a `wal_sync = none` run without a reboot, or by flushing before phase 1 |
| 5. Every WAL recovery and catch-up read path | All read through the page cache today [FACT]: `read_file` (storage.cpp:37-56, unaligned buffer); `scan_stream` (:363-436, from engine.cpp:1124, :1187); `read_stream_range` (:439-451, from replication.cpp:163); **`adopt_spare`'s header read** (:797-812, from engine.cpp:1087), which section 12 missed. The seed sender sends checkpoint files only (seed.cpp:70-90, :156-163); no tool reads segments; the only mmap is anonymous (pages.cpp:34-56). Under `O_DIRECT`, `read_file` needs an aligned buffer (`aligned_block`, storage.cpp:30) |
| 6. Actual target-kernel behaviour | Items 1-4 are confirmed from source only. **Not confirmable without running:** that the device honours FLUSH and FUA, that the R640 runs exactly this build with no livepatch, the WAL directory's file system and per-inode flags, and whether `-ENOTBLK` fallbacks happen there. A minimal later experiment (described only, **not authorized**) would use a dm-flakey or dm-error scratch loop device on the target kernel: write through the fallback path, inject an error, kill, restart without reboot, compare buffered and `O_DIRECT` reads; repeat with dirty pages; check `STATX_DIOALIGN`; trace `iomap_dio_invalidate_fail` |

**So Y10 as it now stands [INF, proposal only]:**
- the recovery scan, catch-up reads and `adopt_spare`'s header read use aligned `O_DIRECT` reads;
- a WAL file on which ext4 would not do direct I/O, checked at runtime, is refused;
- the success path relies on `RWF_DSYNC`, whatever the I/O path.

**Open before acceptance:**
- the phase-1 dirty-page choice in check 4;
- the target-system facts in check 6;
- the `.dsc` signature.

**Recommendation:** keep Y10 a conditional design direction, as Codex says. Ask the owner for the decision only
after the phase-1 choice is written, and keep the buffered-WAL exclusion until then.
