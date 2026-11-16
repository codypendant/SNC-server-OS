# SNC-AFR-001: safety analysis of the proposed authority, group and durability protocol

- **Status: PROPOSED analysis, not a proof.** Codex's review of PR #140
  (`docs/handoff/PR140_141_Codex_Review.md`) asked for this analysis before final architectural acceptance. Choosing
  a direction (`docs/AFR_001_CORE_DECISIONS.md`; cody chose leases for D-01) is separate from proving the protocol,
  and this document is a hand argument toward the second. Every step is an inference unless it is labelled
  otherwise. Section 6 says what would turn the argument into evidence.
- **Nothing here authorizes implementation.** AFR-D-02 and AFR-D-03 are not accepted.
- **Method.** This follows `docs/ENGINEERING_PRINCIPLES.md` (research before reinvention). Each claim carries a
  label:
  - **[FACT-SNC]**: verified in the code at `5338688`.
  - **[EXT]**: a guarantee another system documents, cited from its primary source.
  - **[INF]**: an SNC inference made here.
  - **[HYP]**: unverified.
- **Review.** An independent adversarial reviewer walked every hand-off case as a concrete interleaving:
  - the first draft failed both claims (cases B1 and B2 below), and three further defects were found (M1, M2, M4);
  - the rules below include the fixes;
  - cases with no rule that prevents them are marked **UNRESOLVED**.

## 0. Protocol under analysis

### Group record R

R is held by the witnesses and changed only by compare-and-set on `version`. It holds:
- the term `t`;
- the in-sync set `S` (the primary `p` is in S);
- the mode;
- the minimum set size `m`;
- `ready`.

It generalizes today's `ha/G` state (`snc_witness.cpp:112-138`, [FACT-SNC]). It follows PacificA's versioned
configuration, where the first accepted change wins [EXT: PacificA, MSR-TR-2008-25, section 2.2].

### Member state

Each member `i` keeps:
- a durable highest term `T_i`;
- a log of entries `(term, lsn)`;
- a durable term history `H_i`, mapping each term to its start LSN [EXT: Kafka KIP-101 leader-epoch file].

### Ack set A

The primary acknowledges entry `e` only when two conditions hold:
- every member of its ack set `A` has `e` durably (`p` included);
- `p` holds a valid lease at that moment.

### Lease

- **Anchor.** A grant from member `j` is anchored at `p`'s send time. `p` treats it as valid until
  `send_p + d·(1-ρ)` on its own clock.
- **p's lease.** p's lease is the minimum over grants from **every member of A**, other than `p`. A member of A
  that has not yet granted counts as an expired grant.
- **Clocks.** Clocks count through suspend (CLOCK_BOOTTIME). `ρ` bounds each clock's rate error against real time.

[EXT: PacificA section 2.3, where the primary holds a lease from each secondary, and the paper assumes zero drift; TiKV
anchors the lease at proposal time, https://tikv.org/blog/lease-read/]

**Amendment to CORE_DECISIONS 5.1.** That document takes the lease from every member of S. It must be every member of
A, because A ⊇ S. Case B2 shows the S-only version admits two holders.

### Rules

**R1. Ack set ordering (a procedure, not only an invariant).**
1. To add `c`, `p` puts `c` in A first, waits until `c` has caught up, then proposes the compare-and-set adding `c`
   to S.
2. `p` may drop a member from A only after one of two things:
   - a shrink compare-and-set that removes it has been chosen; or
   - a linearizable read of R shows it is not in S.
   A lost reply to a grow is therefore never treated as a failure (fixes B1).

[EXT: Raft adds non-voting members that catch up first (thesis 4.2.1) and uses joint consensus (thesis 4.3);
PacificA candidates catch up first (section 2.4).]

**R2. Promotion and READY.**
- **Promotion** is a compare-and-set setting `t+1` and a new primary `j`. Four conditions apply:
  - `j` is in S at the chosen version;
  - `ready` is true;
  - `j` is in sync (E1-E11 as restated);
  - the new S is `{j}`, which is today's behavior (`snc_witness.cpp:871`, [FACT-SNC]).
- **READY** at term `t'` may be recorded only after the primary of `t'` has finished its R3 wait. This fixes B2.

**R3. Stop, then wait.**
- **Stop.** A member stops granting to a term when it is named in a promotion, or when it learns any higher term.
- **Wait after promotion.** A member promoted to `t'` does nothing that needs authority until `d·(1+ρ)/(1-ρ)` has
  passed on its own clock. That covers acknowledging writes, answering reads, granting standby leases and
  recording READY. The wait counts from its last grant to **any** primary of a lower term.
- **Wait after restart.** A restarted member waits the same amount, measured from the restart, before it accepts a
  promotion. It may grant at once, which is harmless because adding a grant can only shorten `p`'s lease.

**R4. Durable fencing.** A member that learns `t' > T_i` persists `t'` before anything else. From then on:
- it refuses records, grants and acknowledgements for lower terms;
- as a holder, it stops serving.

**R5. Answer-time check.** For a read, `p` captures the state first, then checks the lease, then sends. A check made
before the capture is not enough.

**R6. Reconciliation.**
- **The new primary's history.** A new primary `j` writes its term start into `H_j` only after its write pipeline
  has drained.
- **Divergence point.** A rejoining member `k` finds where its log diverges by iterating, as KIP-279 does:
  1. `k` asks `j` for the largest term `t' ≤ t_k` present in `H_j`, and for that term's end.
  2. `k` truncates to the minimum of that end and its own end of `t'`.
  3. `k` repeats until the terms match.
  [EXT: KIP-279 fixed exactly this divergence in KIP-101.]
- **Durable truncation.** The truncation is first written as a durable, term-stamped intent, and recovery re-applies
  the intent [EXT: Aurora's durable, epoch-versioned truncation ranges].
- **Reseed instead of truncate.** If the divergence point is below `k`'s checkpoint maximum LSN (`shard.cpp:2710-2716`,
  [FACT-SNC]: checkpoints are local-only), `k` is reseeded.
- **Incomplete reseed.** A member with a reseed in progress is recorded as "incomplete" and cannot be promoted.

**R7. Operator promotion.**
- It carries the expected term.
- It waits `d·(1+ρ)/(1-ρ)` after the compare-and-set.
- It may name a member outside S only with an operator assertion that every member of S is fenced or down
  (STONITH; [EXT: PostgreSQL warm-standby failover docs]).

**R8. Degraded mode (`S = {p}`).** `p`'s lease is held from a majority of witnesses, so its authority stays bounded
(G-33). The witnesses stop granting once the term changes.
- **Cost (UNMEASURED).** Consensus renewals happen in every post-failover and degraded window. X-3 measures this.
- **Alternative for cody.** Unbounded degraded authority, made safe only by R7's operator assertion. This violates
  G-33.

**R9. Standby read leases.** A standby read lease that `p` grants never outlasts `p`'s own primary lease. This fixes
M2.

## 1. Claim L: at most one node holds a valid lease (Codex point 1)

The numbers in the cases below assume `d` = 2 s, `ρ` = 0.1 and a failover timeout of 1 s.

### The drift condition

Safety requires `r_j / r_p ≤ (1+ρ)/(1-ρ)`, about 1.22, where `r_j` and `r_p` are the two clocks' real rates [INF].
- At exactly ρ the margin holds.
- Beyond the condition the two leases overlap, and nothing detects it. For example, with `r_p = 0.85` and
  `r_j = 1.15` there are about 0.2 s with two holders.
- This is an assumption, not a guarantee (G-46). [EXT: Ongaro thesis 6.4.1 and etcd raft both state the drift
  dependence; CockroachDB self-fences on observed offset.] A self-fence is [HYP] future work.

### Cases

| # | Case | Verdict | The step that prevents it |
|---|---|---|---|
| 1 | Old primary paused past expiry, resumed with requests already admitted | PREVENTED | R5: capture, then check, then send. p's lease ends by 2.0 s real; j serves from at least 2.005 s real. Any write p tries to acknowledge needs j's copy, which j refuses (R4) |
| 2a | Grantor restart | PREVENTED | R3 restart wait |
| 2b | Holder restart | PREVENTED | R4 persisted term; new grants required |
| 2c | Promoted member restarts before it serves | PREVENTED | R3: the wait counts from its last grant, and the restart wait covers a lost record of that grant |
| 3a | Set change racing a promotion, both from one version | PREVENTED | Compare-and-set exclusion (today: `snc_witness.cpp:423-513`, `:852-872`, TESTED `test_witness.py:240-256`, [FACT-SNC]) |
| 3b | Grow chosen, reply lost, `c` dropped from A, `c` promoted (B1) | PREVENTED by the fix | R1.2: dropping a member needs a chosen shrink or a linearizable read |
| 3c | Fast double promotion, `t+1` then `t+2`, the second to a member that never granted to the old primary (B2) | PREVENTED by the fix | R2 READY rule plus R3 "any lower term": `t+2` needs READY at `t+1`, which needs `j`'s full wait |
| 4 | Drift at ρ / beyond the condition | SAFE / out of contract | The drift condition above |
| 5a | Operator promotion of a member inside S during a valid lease | PREVENTED | R7 wait (and R3) |
| 5b | Operator promotion outside S, degraded | PREVENTED only with R8 | R8 witness lease. Without R8 it is **UNRESOLVED**: two primaries indefinitely if the assertion is wrong |
| 5c | Operator promotion outside S, `S = {p, j}`, `j` partitioned but still granting | **UNRESOLVED** | No rule. A "down" `j` that still grants keeps `p`'s lease alive. Safety rests on the operator's assertion |
| 6 | After promotion, a third member still serves reads under a standby lease from the old primary (M2) | PREVENTED by the fix | R9 plus R3: the standby lease ends with `p`'s lease |

**Residual assumptions:**
- the drift bound;
- a clock that counts through suspend (today's CLOCK_MONOTONIC does not; `runtime.cpp:38-41`, [FACT-SNC]);
- a correct operator assertion (5c).

## 2. Claim D: every acknowledged write survives (Codex point 2)

**This is an inference, not a proof.** The steps, with what each one assumes:

1. **Within a term.** When `e` is acknowledged, every member of A has `e`.
   - By R1, A ⊇ every S chosen during the term.
   - Grows enter A before S. Drops from A follow a chosen shrink or a linearizable read.
   - [INF] This assumes a definition of "caught up". Proposed: `c`'s durable LSN is at least `p`'s acknowledged LSN
     at the moment of the compare-and-set, and `c` stays in A from then on. Under that definition, every later
     member of S holds `e`.
2. **Promotion.** The new primary `j` is in S (R2), so it holds `e`. Its log is the reference for its term.
   - [INF] This assumes that `j`'s term start is recorded after its pipeline drains (R6).
3. **No truncation of e.**
   - A member truncates only beyond the divergence point found by R6's iteration. `e` is in `j`'s log, so `e` is
     never beyond that point.
   - Fixed by M1: without the iteration, a member could keep a same-LSN entry from a different term. That entry
     would later count as a copy of the wrong entry.
   - [INF] This assumes truncation is durable and re-applied after a crash (R6 intent).
4. **Induction.** Each later term starts from a primary that holds `e`.

**Unacknowledged suffixes.**
- Entries `j` holds become committed once the new set acknowledges them [EXT: PacificA section 2.4].
- Entries `j` lacks are discarded.
- In both cases no client received an OK, so the write is in-doubt (G-58).

**Where Claim D does not hold:**
- every copy of `e` is damaged at once;
- a single-copy window (section 3);
- R7 is applied outside S;
- an implementation breaks R1's procedure (it needs a deterministic test, section 6).

**Cost of R2's `{j}`.** At N = 3, R2 discards `k`'s intact copy from S. `k` must re-earn membership. Keeping
members whose log is a prefix of `j`'s would shorten single-copy windows, but it is not analysed here [HYP].

## 3. Single-copy windows and the limits of G-14(b) (Codex point 3)

A window occurs when `|A| = 1`. This happens in three situations:
- degraded mode;
- after every promotion, until the group is READY again;
- in a configuration with no replica.

If the minimum set size `m` is 1, the guarantees during a window are:

| Guarantee | During the window |
|---|---|
| G-11 class | LOCAL_DURABLE, reported (G-12). G-11 must be tightened: HA_DURABLE requires `\|S\| ≥ 2`, because "every automatically promotable replica" is vacuously true when S is `{p}` |
| G-13 | Writes acknowledged in the window do not survive loss of the host or disk. **HA_DURABLE writes acknowledged before the window are also down to one copy** |
| G-14(a) | Holds only after AFR-F-01 is fixed (AFR-S-1). It is VIOLATED today |
| G-14(b) | **No.** A silently lost newest flush is not detected. A per-flush watermark on the same device does not catch a device that drops both writes |
| Claim L | Holds with R8 (section 1, case 5b) |
| Claim D for earlier writes | They survive only as long as the one remaining copy does |

If `m = 2`, writes are refused during a window, every guarantee holds, and availability is what is lost.

[EXT: Kafka's `min.insync.replicas` makes the same trade.] SNC never promotes "uncleanly" on its own (R2).

**The owner decides:**
- the default for `m`;
- whether a window is reported per answer (that waits for AFR-D-14) or only on the status surface.

## 4. Experiments X-1 to X-3 (Codex point 4)

**No latency target exists in any SNC document.** Every threshold below is a proposal that cody sets before any run,
per AGENTS.md preregistration. Runs use the R640 with the frozen manifest.

| Experiment | Depends on | Runnable before implementation? | Proposed acceptance |
|---|---|---|---|
| **X-1** slowest-member cost | Today's pairs. A primary and a standby in separate network namespaces joined by veth, with `tc netem` delay on the standby link | Yes for N = 2; N = 3 needs implementation | Keep model (A) if healthy-pair p99 write latency meets the target cody sets. With the standby slowed by δ, report the extra p99 latency at queue depths 1, 16 and 128 (queueing makes it larger than δ), and the time to remove the slow member |
| **X-2** watermark and nonce cost | Nothing: a micro-benchmark of dsync writes on the R640 data disk | Yes | A per-flush watermark is the single-node default only if its throughput and p99 cost stays within cody's bound (proposed: 10%). Nonce-formatting costs one write per segment and is accepted if per-flush cost is unchanged |
| **X-3** lease overhead and failover delay | A lease prototype, R3 to R9, which needs an implementation go | No | Renewal and heartbeat overhead within cody's bound (proposed: 1%); R8 witness renewals counted separately. Compare against today's **partition** failover time (7.7 to 8.5 s, `REAL_MACHINES.md:239`), not the kill case (1.00 s, `:204`). The lease is shorter than the failover timeout, so it adds about nothing to partition failover. R2's READY wait adds `d·(1+ρ)/(1-ρ)` to every single-copy window. Measure that |

Order: X-2 and the pair part of X-1 can run once they are preregistered and authorized. X-3 needs an implementation
go.

## 5. What this establishes

**Established [INF]:** with R1 to R9 and the residual assumptions, the hand argument finds no interleaving that
breaks Claim L or Claim D, except case 5c (UNRESOLVED) and the stated out-of-contract cases.

**Not established:**
- that no interleaving was missed (that needs the model check in section 6);
- that the C++ implements any of this (it has no primary lease, no ack-set rule for N ≥ 3, and no term-history
  reconciliation);
- the cost of R8 and of the READY wait.

## 6. Validation plan

1. **TLA+ model of R1 to R9**, with no SNC code needed. It must include:
   - lost compare-and-set replies;
   - a member that is in A but not in S;
   - three terms with an operator step;
   - standby leases;
   - a crash in the middle of a truncation;
   - clock skew bounded by ρ.

   Without these the model would not find B1, B2, M1 or M2. [EXT: Ongaro's thesis gives Raft's TLA+ specification in
   Appendix B.2, with its proof in B.3; PacificA publishes proofs.]
2. **Deterministic tests**, once implemented: a kill at each step of R1 and R6, the R3 waits, and the scenarios in
   CORE_DECISIONS 5.2.
3. **The history checker** from SNC_GUARANTEES section 15: intervals, terms, epochs and authority evidence.
4. **Experiments X-1 to X-3**, as in section 4.

## Sources

**Opened by the reviewer:**
- PacificA, sections 2.2 to 2.4: https://www.microsoft.com/en-us/research/wp-content/uploads/2008/02/tr-2008-25.pdf
- Ongaro's thesis, chapter 4, section 6.4.1 and Appendix B: https://raw.githubusercontent.com/ongardie/dissertation/master/online.pdf

**Opened in the PR #140 survey** (`AFR_001_CORE_DECISIONS.md` section 7):
- KIP-101: https://cwiki.apache.org/confluence/spaces/KAFKA/pages/67634337/KIP-101+-+Alter+Replication+Protocol+to+use+Leader+Epoch+rather+than+High+Watermark+for+Truncation
- Aurora: https://pdos.csail.mit.edu/6.824/papers/aurora.pdf
- TiKV lease read: https://tikv.org/blog/lease-read/
- PostgreSQL failover: https://www.postgresql.org/docs/current/warm-standby-failover.html
- Vertical Paxos: https://microsoft.com/en-us/research/uploads/prod/2009/08/Vertical-Paxos-and-Primary-Backup-Replication-.pdf
- etcd raft: https://pkg.go.dev/go.etcd.io/raft/v3
- CockroachDB replication layer: https://docs.cockroachlabs.com/docs/stable/architecture/replication-layer

**Located by search, page not read:**
- KIP-279: https://cwiki.apache.org/confluence/spaces/KAFKA/pages/75978352/KIP-279+Fix+log+divergence+between+leader+and+follower+after+fast+leader+fail+over

  Its title confirms the divergence fix. The details used in R6 are [HYP] until the page is read.
