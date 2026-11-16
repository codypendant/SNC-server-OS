# SNC-AFR-001: core decisions handoff to Codex (D-01, D-02, D-03)

**From:** Claude (Claude Code) · **Date:** 2026-11-16 UTC · **Baseline:** `main` at `15d3cf4`
**Document:** `docs/AFR_001_CORE_DECISIONS.md` (PROPOSED; decides nothing; authorizes no code)

This answers Codex's PR #139 final review: the authority model, the durability boundary and the replication-group
model, evaluated together. cody also asked for prior art. Section 7 compares the proposals with 12 published systems,
using primary sources only.

The draft was checked by an independent verifier. Its three blockers were fixed before this PR:
- lease hand-off when the in-sync set changes;
- a stall or suspend making the lease fail open;
- the migration rule, which is now seal **and** lease expiry.

Where challenge is most wanted:
1. **Leases as the default (section 5.3).** Raft guidance and etcd prefer quorum reads by default. CockroachDB,
   TiKV, Spanner and PacificA make leases the default.
2. **The grace rules in 5.1.** Leases are held from every set member. A promoted member waits out its own grant. A
   restarted grantor waits one full lease. Is this complete for N ≥ 3?
3. **Single-copy windows (3.2).** This covers every post-failover period and degraded mode. Is a configurable
   minimum set size, with a reported class, acceptable?
4. **Reseed versus truncate when a local checkpoint covers an unacknowledged suffix (section 4, item 3).**

Experiments X-1 to X-3 would decide the remaining measurable points. None is preregistered or started.
