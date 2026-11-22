# PR #186 revision 3: focused independent review of the settlement probe

One reviewer (Opus), told to break the revision 3 settlement probe in `docs/DIRECTION_COMPARISON_PLAN.md` section 4.1
after Codex's HOLD of revision 2 (`PR186_Codex_Review_rev2.md`). The reviewer edited nothing. The report follows
verbatim. Every finding is applied in the plan (section 8, revision 3); the UNRESOLVED point on B2 under Zipf is
settled by making B2's gated point uniform and Zipf reported only.

I broke section 4.1 as asked and edited no files. All the arithmetic checks out. I found no blocker: 4 MAJOR issues, 4 MINOR ones, and 1 point I cannot settle.

**Arithmetic, recomputed:**
- f = B/x: 0.002/0.2 = 1%; 0.002/1.0 = 0.2%.
- Overhead: 0.2 × 0.01 = 0.2%; 1.0 × 0.002 = 0.2%.
- n_min = 3,000 at a true miss fraction of 1%: about 30 expected misses, standard deviation √29.7 = 5.45, Wilson 95% interval 0.70% to 1.42%.
- Thresholds: 3,000/(0.002 × 120) = 12,500 ops/s; 3,000/(0.002 × 600) = 2,500 ops/s. Example: 0.002 × 600,000 × 120 = 144,000.
- Pool: the 1,024 floor always binds (for example 4,000 probes/s × 2.2 ms × 10 = 88).
- "p99 ≤ SLO iff misses ≤ 1%" holds exactly with a nearest-rank p99 (misses ≤ floor(0.01n)).

**MAJOR 1: the reused probe account's "earlier balance" can turn a late credit into a false hit.**
- Quoted: "the deadline read shows the target's balance equal to its earlier balance plus the amount"; "each has at most one probed transfer outstanding, which lasts SLO + g".
- Why it breaks: an account is freed after SLO + g even when its probe missed and the credit is still pending. If "earlier balance" means the last observed balance, the old credit can land just before the next probe's deadline read. The read then shows "earlier + amount" while the new credit is absent, and counts a hit. This happens exactly near overload, the region bisection searches, so it favours whichever system misses.
- Smallest fix: define the earlier balance as the start balance plus every earlier probe credit sent to that account, missed ones included. The risk is a small double-count of misses, which is conservative and the same for every system. Or retire a missed account, and size the pool as 1,024 + 2% of the expected n (144,000 probes give about 1,440 misses, more than 1,024).

**MAJOR 2: grace is not measured from the intended deadline, so a lagging client relaxes the test.**
- Quoted: "issued at the transfer's intended send time + SLO … Its reply must arrive within a grace g".
- Why it breaks: if g runs from the actual issue time, a client running behind gives the credit extra time. That is coordinated omission on the probe. The C3 check bounds client lag only loosely.
- Smallest fix: "reply by intended send + SLO + g, measured on the intended timeline." This also makes the leniency the same for all systems. SNC's read waits only for the object's own last LSN (`docs/AFR_001_INVENTORY.md`:192), Restate's exclusive handler queues behind the credit, and atomic systems use ack ≤ deadline + g. All of them then settle by SLO + g at most.

**MAJOR 3: the durable-settlement boundary is not equal for the B2 outbox, and no read-implies-durable property is verified.**
- Quoted: "PostgreSQL's visibility after the local WAL flush is UNVERIFIED, so a read alone never proves durability there". That rule applies only to atomic systems (B1). In B2 the PostgreSQL outbox's credit is a worker commit checked by a read alone.
- Why it breaks: the same unverified PostgreSQL property is rejected in B1 and accepted in B2. Separately, the `dm-log-writes` check covers acknowledgements only. Nothing tests that a credit seen by a probe read survives a crash, for SNC, Restate or the outbox, and all three are judged on exactly that. This touches Codex item 5.
- Smallest fix: one rule, "for any PostgreSQL commit, its issuer (client or worker) has the commit return by deadline + g". Also add to the durability check: probe reads are logged on the same clock, and at each mark every credit a read showed before the mark must be in the recovered image.

**MAJOR 4: window length varies by system, which biases B2 and leaves sessions over budget.**
- Quoted: "T = max(120 s, n_min / (B × R)), capped at 600 s"; section 7: "B2 runs | 0.5 day | R640, one session of about 5 h".
- Why it breaks:
  - **Bias:** if SNC stays above 12,500 ops/s, only Restate or the outbox (about 2.5k to 12.5k ops/s) gets windows up to 5x longer. Those windows contain more RocksDB compaction, checkpoints, autovacuum and outbox bloat. That favours SNC and is not in the gap table.
  - **Budget:** in B2, two slow systems × (about 8 bisection steps + 3 confirmations) × up to 600 s plus warm-up and drain is about 4 h or more, before tuning screens and recovery tests. The stop-rule cap could then end B2 as UNRESOLVED instead of with a result.
- Smallest fix: confirmation runs at a point use one T for every system (the largest any system needs). Recompute the section 7 session time with 600 s windows. Or allow bisection steps at 120 s with n ≥ 600, with only confirmations needing n_min.

**MINOR 1: the rule for rates below 2,500 ops/s is ambiguous and can grant SNC a wrong PASS.**
- Quoted: "In a ratio it counts as 2,500 ops/s, which favours that system. If the verdict holds anyway, it stands".
- Why it breaks: if SNC is the slow system, 2,500 sits in the numerator and favours SNC, so a PASS would not "hold anyway". If both systems are below 2,500, the ratio is undefined.
- Smallest fix: "the verdict stands only if it holds for every value of the low system's capacity in [0, 2,500); otherwise UNRESOLVED."

**MINOR 2: a bisection step that ends UNRESOLVED leaves bisection undefined.**
- Quoted: "It is rerun once; a second invalid run makes that point UNRESOLVED."
- Why it breaks: "point" can mean a bisection rate, and nothing says how bisection continues from an UNRESOLVED rate.
- Smallest fix: "an UNRESOLVED bisection step counts as failing; if the verdict would change had it passed, the benchmark is UNRESOLVED."

**MINOR 3: the B1 2 ms SLO justification still uses polling-era reasoning.**
- Quoted: "It is wider because a transfer touches two accounts, and the deadline read has its grace g."
- Why it breaks: g now sits on top of the SLO and loosens the test. It no longer consumes part of it, so it is no reason for a wider SLO. The two-account reason still holds: an SNC cross-shard saga needs two serial group commits. The B2 10 ms reasoning still holds.
- Smallest fix: delete "and the deadline read has its grace g". In 4.1, state that the effective bound is "settled by SLO + g at most, equal for all systems".

**MINOR 4: stale and ambiguous text remains.**
- Line 1 still says "revision 2", and section 8 has no revision 3 entry.
- Line 713 still says "the probe's poll grid, pool and cost cap", which is the old design.
- "B ± 5%" should say relative to B, counted against intended (offered) operations. At n = 3,000, binomial noise is about 1.8%, so ±5% gives roughly a 1% chance of a false invalid run per run. Acceptable.
- Say whether the 1,024 probe accounts are part of the 1M or added to it, and that the workload never targets them.

**UNRESOLVED: B2 under Zipf.** Probe targets are cold by construction ("receive nothing else"). That is representative only if B2's credit targets are uniform ("a random entity B" suggests they are). If targets follow Zipf, the probe misses contended-target settlement, which favours Restate and PostgreSQL (per-object or row queueing). Also, B2 lists "uniform and Zipf 0.99" without saying whether Zipf gates.
- Fix: state that B2 targets are uniform, or make Zipf in B2 non-gating.

**Checked and fine:**
- Leaving probe reads out of served operations and out of the read p99 gate is consistent: the share is the same B for every system, and it counts toward errors and C3.
- The probe reads are equal in count across systems. Restate's journaled read costing more is already labelled "favours SNC" and is negligible at 0.2%.
- The B1 0% point has no probes, so it runs at 120 s.
- Section 9's "Shared" numbers match 4.1.

**The five resolved Codex items:**
- Track A arithmetic, section 6 scope and the Rust/Python classification are not affected.
- Full settlement as the deciding boundary still holds; MAJOR 2 makes its effective bound SLO + g uniform.
- Durability verification needs only the read-durability addition in MAJOR 3. Its earlier design is not reopened.
