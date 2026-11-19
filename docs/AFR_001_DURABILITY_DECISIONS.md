# AFR-001: durability owner-decision package (PROPOSED)

Status: **PROPOSED, for Codex and owner review.** Docs only. **Nothing here is an owner decision**, and nothing is
accepted, frozen or implemented. Each decision below needs the owner's explicit yes.

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
- **C1, a seed marker (E30).** Before the first fetched byte, the seed creates an empty `seeding` file and syncs the
  directory. Step 3 classifies "no valid record slot, and `seeding` present" as **SEED**: everything else in the
  directory came from the primary and this node never acknowledged any of it, so phase 2 removes it and fetches
  again. After the record write, phase 2 unlinks the marker and syncs the directory before the node serves. A valid
  record always wins over a stale marker. A lost marker create (FM-7) refuses as today, which a fault may do. This
  also covers the slot-epoch mismatch wipe-and-seed (E14′).
- **C2, creation order (E31).** Create the links generation durably before the first record write, on init and on
  seed, as V8-2 orders the watermark files. A missing file beside a valid record is then a fault.

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

**E-1, E-2, E-3 cannot modify or destroy production data, as now written** (round 3 found E-1 and E-2 unsafe as
#170 wrote them; the contract's section 9 now carries the safeguards) [INF, on paper]:
- **E-1:** file systems and `dm-delay` only on a loop device backed by a scratch file outside `/srv/snc-data`, the
  boot device and the old HDD; `mkfs` and `dmsetup` only on that loop device; disks named by serial or UUID; teardown
  and a `lsblk`/`df` check at the end. Booting another kernel writes `/boot` and is a separate authorization.
- **E-2:** files in a fresh, capped scratch directory, never the raw device; no SNC process running; removed at the
  end. It measures the device cost of one extra write only: **not X-2, not an HA_DURABLE or end-to-end result, and a
  pass is not acceptance.**
- **E-3:** an allowlist of read-only commands; `sdparm --set`, `hdparm -W` and sysfs writes forbidden.

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
