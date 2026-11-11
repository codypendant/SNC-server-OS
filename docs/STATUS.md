# Prototype status

What the code covers today, measured against the
[SNC v0.1 Object & Execution Specification](https://claude.ai/code/artifact/8a25e96e-09ae-43ef-9e42-d6763e1eebd9)
and the handoff's development sequence (§24).

## Next milestone: experimental validation / falsification

**Experimental Validation / Falsification Milestone: planned after S1 and
the required correctness fixes** ([EXPERIMENTAL_VALIDATION.md](EXPERIMENTAL_VALIDATION.md)).
Decided by cody on 2026-11-04.

- **Scope freeze after S1.** No major platform expansion until the
  milestone's GO / REFINE / NARROW / STOP decision. That covers native
  Documents, generic Events/Timers, WaitSets, Activation, the Agent
  Runtime, an MMO framework, a plugin platform, security/capabilities,
  multi-tenancy, a Rust rewrite, and DPDK/VPP.
- **Exceptions:** work a validation workload explicitly requires, and
  fixes for defects that would invalidate an experiment (R08, R11 are
  candidates).
- **Order:**
  1. S1 (approved, PR #44).
  2. The baseline readiness review
     ([EXPERIMENTAL_BASELINE.md](EXPERIMENTAL_BASELINE.md)): approved
     with corrections (PR #45).
  3. The pre-baseline tests and any blocker fixes they force: done and
     approved (PRs #47 to #53); the correctness gate is clear.
  4. **Now: campaign preparation** ([CAMPAIGN_PREP.md](CAMPAIGN_PREP.md)).
     The SNC-only R640 pilot (CP-1A) ran on 2026-11-06 and aborted under
     its rule (no stable peak). Step 1 found a benchmark defect (the ids file on the
     data SSD), fixed by scratch isolation (approved). The corrected proof passed and the
     causal cells show the unpaced checkpoint is the refusal source (accepted:
     engine design gap). The checkpoint/backpressure design
     ([CHECKPOINT_BACKPRESSURE_DESIGN.md](CHECKPOINT_BACKPRESSURE_DESIGN.md))
     was accepted as direction (PR #67) and its revision 2 approved (PR #68).
     S0 observability (published gauges, no behaviour change) was approved
     in architecture and scope (PR #69). S0.1 (seqlock memory order) was
     approved (PR #70), so S0 is complete. The two known HA test-harness
     races were fixed (PR #71, approved). S1 resource-specific admission
     ([S1_ADMISSION.md](S1_ADMISSION.md), PR #72) was not approved as
     complete ([handoff/PR72_S1_Review.txt](handoff/PR72_S1_Review.txt));
     the S1.1 corrections ([S1_1_CORRECTIONS.md](S1_1_CORRECTIONS.md),
     PR #73) resolved its blockers but left replay waiters outside the
     budget ([handoff/PR73_S11_Review.txt](handoff/PR73_S11_Review.txt));
     S1.2 ([S1_2_REPLAY.md](S1_2_REPLAY.md), PR #74) closed it and was
     approved, so S1 is complete
     ([handoff/PR74_S12_Review.txt](handoff/PR74_S12_Review.txt)). S2, the
     paced checkpoint walk ([S2_PACING.md](S2_PACING.md), PR #75), was
     approved and is complete
     ([handoff/PR75_S2_Review.txt](handoff/PR75_S2_Review.txt)). S3-D
     ([S3D_WRITEBACK.md](S3D_WRITEBACK.md), PR #76) chose C3 and was
     approved with conditions
     ([handoff/PR76_S3D_Review.txt](handoff/PR76_S3D_Review.txt)). S3, the
     bounded writeback ([S3_WRITEBACK.md](S3_WRITEBACK.md), PR #77), had
     its core approved ([handoff/PR77_S3_Review.txt](handoff/PR77_S3_Review.txt)).
     S3.1, the engine lifecycle fix for teardown with checkpoint I/O in
     flight (section 10 there, PR #78), was approved
     ([handoff/PR78_S31_Review.txt](handoff/PR78_S31_Review.txt)). S3.2,
     one lifecycle gate over every shard-submitting Engine method
     ([ENGINE_LIFECYCLE.md](ENGINE_LIFECYCLE.md), PR #79), and S3.3, the
     data-plane lease and promote/checkpoint serialization (sections 9-11
     there, PR #80), were approved, and S3 is complete
     ([handoff/PR80_S33_Review_S4_Authorization.txt](handoff/PR80_S33_Review_S4_Authorization.txt)).
     S4, the staggered final sync with K node tokens
     ([S4_SYNC_TOKENS.md](S4_SYNC_TOKENS.md), PR #81), had its core
     approved ([handoff/PR81_S4_Review.txt](handoff/PR81_S4_Review.txt));
     K is a hypothesis, default 0 (unlimited). S4.1 (PR #82: admin replies
     sent whole or cut visibly; the fairness wording) approved, and **S4 is
     COMPLETE** ([handoff/PR82_S41_Review.txt](handoff/PR82_S41_Review.txt)).
     Required before S6: follow-up intake backpressure (S1.1 section 3).
     Its design only is authorized:
     [FOLLOWUP_INTAKE_DESIGN.md](FOLLOWUP_INTAKE_DESIGN.md) (option D,
     credit-bounded destination staging) was approved as the base
     architecture in cody's review (handoff/PR83_Design_Review.txt), with R1
     rejected. As ordered, the `WalStream` active-buffer u32 overflow is
     fixed first (PR #84: checked growth, 1 GiB maximum, fail-stop;
     bench/results/walcap-validation-devvm-20261109). Rev2 of the design
     (reply-only follow-ups and enforced `max_emits`, an exchange budget X
     that makes B2 structural intra-node, cross-node end frames as D.2) is
     approved with conditions (handoff/PR85_Design_Review.txt); rev2.1 adds
     them (per-invocation emit reservation, coalesced paged resume), and
     D.1 (intra-node) was authorized. D.1 is implemented (PR #87): its core
     is APPROVED and its completion was held on a closure PR
     (handoff/PR87_D1_Review.txt). **The closure PR (witness "primary up"
     fix, D.1 GAUGES, P1 matrix) merged as PR #89 and was APPROVED: **D.1
     is COMPLETE** (handoff/PR89_D1_Closure_Review.md; section 3.9.7
     there; bench/results/intake-d1-closure-devvm-20261110). **D.2 is
     authorized** as the next slice. Its design rev1 (PR #91) got DESIGN
     DIRECTION APPROVED, IMPLEMENTATION HOLD
     (handoff/PR91_D2_Design_Review.md): 4 blockers plus proof terms.
     Design rev2 (PR #92: exact Out-to-instance binding, quarantine with
     explicit bounded debt, per-member entries, drop to resume) got
     SUBSTANTIALLY APPROVED WITH TWO CORRECTNESS BLOCKERS, IMPLEMENTATION
     HOLD (handoff/PR92_D2_Design_Rev2_Review.md). Design rev3 (PR #93:
     HelloAck identity split from the exact binding SYNC, FINAL removed,
     current-connection SYNC only, fencing before silent retirement,
     blocked-primary timing) got REV3 CORE PROTOCOL APPROVED,
     IMPLEMENTATION HOLD ON TWO NARROW ITEMS
     (handoff/PR93_D2_Design_Rev3_Review.md). Design rev4 (PR #94: SYNC accounting
     barrier, persistent monotonic incarnation generation) is APPROVED and
     **D.2 implementation is AUTHORIZED**
     (handoff/PR94_D2_Design_Rev4_Review.md), in the section 9 commit
     order, as one PR returned for independent review. **D.2 is
     COMPLETE: PR #95 APPROVED and merged** (final review
     handoff/PR95_D2_Implementation_Final_Review.md; validated at
     2bccdbe, product code b77937e; evidence
     bench/results/xnode-d2-review-fixes-devvm-20261110): wire v2, per-(link, member)
     entries, HelloAck plus exact SYNC binding, END frames, quarantine and
     retirement, durable links generation, blocked-primary leave and
     rejoin, heartbeats and silence detection, D.2 GAUGES; X1-X32 with
     X19b, the negatives and P2 are in
     bench/results/xnode-d2-validation-devvm-20261110. The independent
     review of PR #95 ruled CHANGES REQUIRED
     (handoff/PR95_D2_Implementation_Review.md): an explicit generation
     file is now locked (X33), a blocked-bit clear stays pending under a
     full queue (X34), the END layout is specified as implemented and
     checked byte for byte (X35), and ND10 and ND24 are closed by proofs
     I1 and I2 (design 2.9.1; X12c catches ND24). The fixes and the
     rerun gate at 2bccdbe (3 suites 47/47, negatives 42/42) are in
     bench/results/xnode-d2-review-fixes-devvm-20261110. Carried as
     observations (no root cause, not D.2 findings): one paxos c2b
     deadline failure and one F8 links-up timeout; investigate on site
     if either recurs.
     **R640 manifest e reviewed and APPROVED (2026-11-11):**
     bench/manifests/r640-main-20261111.txt, 12 × 32 GB with six of six
     memory channels per socket (R640_TOPOLOGY 2d). Manifest d is retired
     for future evidence; its CP-1A and diagnosis results stay historical.
     bench/pilot_plan.json pins manifest e (that pin is the only change;
     PILOT_PLAN 2 says why). PR #97 APPROVED 2026-11-11 (review at head
     f9c2151, [handoff/PR97_Manifest_Closure_Final_Review.md](handoff/PR97_Manifest_Closure_Final_Review.md)):
     the manifest-baseline gate is CLOSED. R640 S2 pacing re-derivation
     done at merged main 0dbd4c7
     ([bench/results/r640-s2-pacing-20261111/](../bench/results/r640-s2-pacing-20261111/README.md)):
     the legacy 256-ID step is 171.6 us p50 (about 670 ns per ID), the
     batch of 16 is 13.3 us p50 (16.4 p99). The derived values are min 20,
     max 65 and ceiling 155 us, batch 16, step 5 us. The dev-VM max (100)
     and ceiling (250) are not supported on the R640. snc_server has no
     flag for the walk values, so a flags-only change needs review before
     S6. The S6 preregistration, [S6_PREREG.md](S6_PREREG.md), is
     **for review; no S6 data, not GO**. PR #98 merged after its final review
     ([handoff/PR98_S2_Pacing_S6_Prereg_Final_Review.md](handoff/PR98_S2_Pacing_S6_Prereg_Final_Review.md)):
     the characterization and the 20/65/155/5/batch-16 values are APPROVED,
     as are max age 60,000 ms, the pre-S1 comparator 389b623 (run with the
     current net_bench) and 100 ms controller sampling. **S6 NOT GO.** Its
     section 7, the snc_server walk flags, is done
     ([bench/results/s6-pacing-cli-20261111/](../bench/results/s6-pacing-cli-20261111/README.md)).
     PR #99 APPROVED and merged (fbf9ca9;
     [handoff/PR99_S6_Pacing_CLI_Final_Review.md](handoff/PR99_S6_Pacing_CLI_Final_Review.md)):
     S6 pacing CLI plumbing COMPLETE; the TSan waiver is accepted for this
     parser-only slice. The S6 runner `bench/s6.py` (tests/test_s6.py,
     19/19 mutations caught) and the final [S6_PREREG.md](S6_PREREG.md) are
     done and returned for independent review
     ([bench/results/s6-runner-20261111/](../bench/results/s6-runner-20261111/README.md)).
     The current net_bench drives the 389b623 server (a compatibility
     check, not evidence). The runner refuses a real run until a docs-only
     freeze names this PR's merge SHA and a filed GO. **S6 still NOT GO.**
     No CP-1A, comparators or CRITERIA freeze before that.
     D.2 (cross-node) is its own reviewed slice, required before S6
     ([handoff/CP1A_Pilot_Abort_For_Review.md](handoff/CP1A_Pilot_Abort_For_Review.md)),
     then the SNC Experimental Baseline: a fixed commit, machine manifests,
     and pre-registered failure criteria.
  5. The falsification program.
  6. The decision.
- **Readiness review (approved with corrections, PR #45).** It set:
  - profiles A (LOCAL_DURABLE), B (SYNC HA), C (memory ceiling) and an
    optional D (async, manual failover);
  - R08 handled by excluding automatic async failover;
  - R11 handled by a bounded retry horizon proven by a test, with the
    TTL left at its default;
  - seven pre-baseline tests;
  - pre-registered GO / REFINE / NARROW / STOP rules.

  It found no blocker for these profiles. Two items (checkpoint × lost
  standby, blocked sync × memory) become blockers for Profile B's
  failure runs if their tests reproduce the hazard.
- **Pre-baseline test results (2026-11-05,
  [BASELINE_READINESS.md](BASELINE_READINESS.md)).**
  - **Pass:** T-Q1, T-Q2, T-Q4, T-Q10 and T-H (`bench/profile_check.py`,
    used by `lab.py`).
  - **Three Profile B BLOCKERS were reproduced (B1, B2, B3), and all three
    are FIXED (2026-11-05,
    [BASELINE_READINESS.md](BASELINE_READINESS.md) section 10), awaiting
    cody's and Codex's review:**
    - B1: a checkpoint waits only for local WAL durability, so a sync
      primary whose standby dies no longer wedges (q3_runtime20: 20/20).
    - B2: a standby is promotable only once proven in sync. The witnesses
      record NOT READY on learn, promotion, grant, reseed, regression and
      every primary start; only READY from the current term, naming the
      NOT READY it follows, clears it (section 11)
      ([SYNC_READINESS.md](SYNC_READINESS.md)). There is no witness state
      format change.
    - B3: a cancelled catch-up reader stops producing, and stops scanning
      too. The ack timeout no longer counts the primary's own disk reads
      or pacing, which also fixes F5 (catch-up below 2 MiB/s livelocked).
    - Fresh suites on the final code: release, ASan+UBSan and TSan each
      28/28 (release after a test-only probe fix), zero sanitizer reports.
  - **Findings:**
    - F1: replication retention after a grant must be declared (to be
      measured on the R640).
    - F2: done. The R11 contract is "every in-doubt resend before session
      expiry", enforced per run by `net_bench --retry-claim-ttl-s`.
    - F6: degraded mode has no back-pressure, so a group can stay NOT
      READY (unavailable for failover, never unsafe).
    - F7: with a stopped witness, readiness takes longer (about 4.5 s
      per READY attempt), and all of a witness's machines share one
      proposal queue with no expiry. Liveness only (SYNC_READINESS 7.5).
    - F8 (characterized 2026-11-05, [PAXOS_BALLOTS.md](PAXOS_BALLOTS.md)):
      a restarted witness does reuse ballots (clock stepped back), but the
      acceptors' persisted promises refuse them; no second value can be
      chosen (proof section 4, tests C1 to C3). **Approved (PR #51
      review): a non-blocking liveness note**; C1 to C3 stay as
      regressions.
    - **F9: a witness that loses its acceptor state** (state file
      deleted or restored from an older copy, restarted in place) **can
      let two values be chosen** in one instance. For the initial
      Profile B it is a configuration/recovery exclusion after the
      hardening of PAXOS_BALLOTS section 9 (G1 `--new-state`, G2
      campaign guard, F10, F11, fatal persistence; approved, PRs #52
      and #53). **Not solved for enterprise:** witness restore and
      replacement need the recovering-acceptor design (section 7, not
      built).
  - **The delayed-SYNCED window (PR #49 review) is CLOSED (2026-11-05,
    [BASELINE_READINESS.md](BASELINE_READINESS.md) section 11), approved
    (PR #50).** Every NOT READY is a fresh chosen version; READY names it
    (`after=N`) and is chosen only on exactly that state; SYNCED is
    refused. Characterized before the fix (2,000/2,000 accounts lost
    acknowledged deposits), V1 to V10 pass, no witness state format
    change. Mixed versions: new nodes never become READY through old witnesses
    only (fail safe); the protected claim needs every node and witness
    upgraded, witnesses first.
  - **X1 implemented:** wire kind `INDEX` exposes `index_lookup` with its
    semantics unchanged (EXPERIMENTAL_BASELINE section 4).
  - **The delayed-SYNCED window fix (PR #50) is approved.** F8/F9 were
    decided in the PR #51 review; the witness hardening (PR #52) and its
    corrections (PR #53: G2 at campaign boundaries, a clean full Release
    run 32 / 32) are approved.
  - **CORRECTNESS BASELINE: READY FOR CAMPAIGN PREPARATION (PR #53
    review, 2026-11-06). FULL EXPERIMENTAL BASELINE: NOT YET FROZEN**
    (EXPERIMENTAL_BASELINE section 11). The main R640 is described in
    [R640_TOPOLOGY.md](R640_TOPOLOGY.md) (manifests a-c; d, with the
    `performance` governor, still to capture).
  - **CP-1A, corrected per the PR #61 review:**
    - `net_bench --rate` (open loop, latency from the intended send time)
      and `--mix` (exact shares of total ops);
    - `bench/pilot.py` runs an open-loop rate sweep with the exact rule of
      [PILOT_PLAN.md](PILOT_PLAN.md), with explicit PILOT-L/PILOT-X
      allocations.
    - The R640 draft's stopped run is quarantined as invalid.
    - The plan, manifest d and the dev-VM evidence are APPROVED (PR #61
      final review, with C1-C3 applied). **The real pilot still needs an
      explicit GO.**
    - No comparative benchmarks before `CRITERIA.md`.
- **Future architecture vision (not authorized):** cody's and Codex's
  v2 vision is filed with a core review in
  [FUTURE_ARCHITECTURE.md](FUTURE_ARCHITECTURE.md).
- **Agent-SNC v0.1 architecture (design input, not authorized):**
  cody's synthesis of Agent-SNC (agents as durable state; events move
  execution, documents move knowledge; WorkOrders, channels, the Event
  Fabric, no-lost-wakeup waits, activation leases; Rust direction; ACB
  migration) is filed verbatim in
  [handoff/AGENT_SNC_ARCHITECTURE_V0_1.md](handoff/AGENT_SNC_ARCHITECTURE_V0_1.md).
  It says itself that it is "design input, not uncontrolled
  implementation authorization"; it starts no Agent-SNC or Rust work.
- **State-native computing lineage (research, not authorized):**
  [STATE_NATIVE_COMPUTING_LINEAGE.md](STATE_NATIVE_COMPUTING_LINEAGE.md)
  records prior art and frames the minimal durable state-machine contract
  (DSM-1..DSM-10) as hypotheses. It authorizes no branching, replay or DSM
  implementation.
- **Evidence rules:** every benchmark host has a manifest in
  [bench/manifests/](../bench/manifests/README.md); raw results go under
  `bench/results/<campaign>/` with the exact configuration and commit
  SHA. Numbers from this project's 4-vCPU development VM are regression
  checks only.

## Handoff §24 steps

| Step | State | Where |
| --- | --- | --- |
| 1. Development Linux | Done: Ubuntu 24.04, GCC 13, CMake | `CMakeLists.txt` |
| 2. Runtime abstraction | Done: native backend, plus a Seastar backend behind the same interface (`-DSNC_SEASTAR=ON`). Seastar is 0.45-0.92x of native in memory; native stays ([SEASTAR.md](SEASTAR.md)) | `include/snc/sri.h`, `src/native/runtime.cpp`, `src/seastar/runtime.cc` |
| 3. Dedicated per-core workers | Done: pinned threads, run-to-completion loop, eventfd idle | `src/native/runtime.cpp` |
| 4. Stable ObjectID | Done | `include/snc/object_id.h` |
| 5. In-memory object directory | Done: slot map + per-shard local map | `include/snc/slot_map.h`, `include/snc/local_map.h` |
| 6. Shard ownership | Done | `src/core/shard.cpp` |
| 7. `execute(ObjectID, op)` | Done: two-phase validate/apply, create, delete | `src/core/shard.cpp`, `include/snc/snc.h` |
| 8. Cross-shard dispatch | Done: 128-byte `Msg`, SPSC ring mesh, external buffers, `BUF_RETURN` | `include/snc/msg.h`, `src/core/shard.cpp` |
| 9. Secondary index | Done: shard-local `Account.email`, broadcast lookup, durable-only results | `src/core/shard.cpp`, `include/snc/registry.h` |
| 10. WAL/persistence | Done: per-shard streams, after-images, group commit over io_uring, durable acks | `include/snc/wal_format.h`, `src/core/storage.cpp` |
| 11. Checkpoint/recovery | Done: fuzzy per-shard checkpoint, A/B superblock, replay, remap on shard-count change | `src/core/engine.cpp`, `src/core/shard.cpp` |
| 12. Locality benchmarks | Done: SNC vs a thread-pool baseline, in memory and durable; report in `docs/BENCHMARKS.md` | `bench/`, `docs/BENCHMARKS.md` |

## After v0.1

| Step | State | Where |
| --- | --- | --- |
| Object lookup cost | Done: local map entries carry their hash; batch prefetch on socket and ring ingress (`snc::prefetch`). Measured against a baseline with the same map and prefetch: SNC leads when ops reach the owner and durable, trails on cross-core ops in process. Ring messages are one cache line | `include/snc/local_map.h`, `src/core/shard.cpp`, [PREFETCH.md](PREFETCH.md) |
| Durable tail latency | Done: shards yield every 50 µs while disk I/O is in flight so io_uring workers can complete WAL writes (`RuntimeConfig::io_yield_us`) | `src/native/runtime.cpp`, [ROUTING.md](ROUTING.md#durable-p99-the-cause) |
| Hot-key (Zipf) workloads | Done: `--zipf THETA` in all benchmarks; SNC overtakes the baseline at high skew (θ 1.5), and over TCP at θ 0.99 too; leads durable at every skew | `bench/zipf.h`, [SKEW.md](SKEW.md) |
| Comparison with a conventional stack | Done: same workload on PostgreSQL 16 and Redis 7, same cores; SNC about 10× Redis and 35-50× PostgreSQL | `bench/conventional/`, [CONVENTIONAL.md](CONVENTIONAL.md) |
| Two-object ops (transfers) | Done: durable follow-ups (`OpCtx::emit`, `resume`) across shards, one op with an atomic WAL group (`OpCtx::local`) within a shard; `create_near` places related objects in one slot; conserved across restart, remap, torn groups and SIGKILL. Random pairs: 1.7x slower than the locked baseline in memory, within 10% durable; all pairs on one shard: 1.5x faster in memory, tied durable. 7-9x Redis, 15-35x PostgreSQL | [TRANSFERS.md](TRANSFERS.md) |
| Cross-core handoff | Done: inline ID hash, messages written in place in the ring, one fence per loop, batched requests (`MULTI_REQ`, 0.5 messages per remote op). In process 100% remote at depth 32: 0.60x -> 0.82x the baseline; at depth 512: 1.20x | [CROSSCORE.md](CROSSCORE.md) |
| Huge pages (handoff §10, "Later" list) | Done: `RuntimeConfig::pages` puts each shard's local map and object heap (a mimalloc arena) on THP or reserved HugeTLB pages, the rest of the process on 4 KiB. 3-10% in memory; the baseline gains as much from system-wide THP, so the gaps between them barely move. Default stays 4 KiB | `include/snc/pages.h`, [HUGEPAGES.md](HUGEPAGES.md) |
| Replication (handoff "Later" list) | Done (v1): one primary ships each shard's WAL to a standby's shard over TCP; sync by default (durable acks wait for the standby's fsync), degrading to async after a timeout; catch-up from memory; manual promotion. Round 2: a new standby seeds from the primary's checkpoint over the network and catches up from its WAL files (about 5 s for 1M accounts when idle); divergence after a primary crash is detected. Round 3: a standby is promoted in place (about 50 ms for 1M accounts; it keeps its index live) and the old primary is fenced by term; catch-up from WAL files can be rate-limited; `snc_witness` fails over automatically (about 0.5 s, mostly detection) without losing acknowledged writes in sync mode (an async pair does lose the writes acked during the lag: `test_async_failover`, [EVIDENCE.md](EVIDENCE.md) section 12), and can run as three or five witnesses that decide by Paxos, so failover survives a minority of them down; a standby can serve reads under leases from its primary (1.1M reads/s on one core, every acknowledged write visible); clients fail over and resend in-flight writes exactly once ([RETRIES.md](RETRIES.md)). Sync keeps 0.54-0.60x of no-standby throughput on this one-disk loopback VM (0.98-1.05x across real machines, [REAL_MACHINES.md](REAL_MACHINES.md)), about 22x PostgreSQL with sync streaming replication | `src/core/replication.cpp`, [REPLICATION.md](REPLICATION.md) |
| Multiple nodes | A slot map across replication groups (placement epoch 1), each owning a contiguous share of slots; ops on another group's objects are refused (`E_WRONG_NODE`); transfers between groups settle over node links (TCP, no acks; every reconnect re-runs resume hooks), which find each group's primary among its members. Failover inside a group changes no placement. Checked with three groups of two members, one primary SIGKILLed mid-run and its standby promoted: every acknowledged transfer present, money conserved, all settled, other groups kept working, epoch unchanged; a primary left running while its standby is promoted withdraws from the data plane (links closed, clients answered `E_STANDBY`, in-doubt ops `E_FENCED`) and the other groups' links find the new primary; and three single-member groups, one killed and restarted, also with the map from the control plane (witnesses' `placement` machine, `snc_ctl init`, nodes by NodeID, restart from the stored map with the control plane down); misrouted ops get `E_MOVED {epoch, group}`; mint leases in cluster-wide blocks from the control quorum; one witness quorum running an HA machine per group (one group failed over by it, the other group, placement and epoch untouched). Slots move between groups (`snc_ctl move`): durable seal at the source, digest-checked staged copy at the destination, epoch raised only at commit, abort with a linearized unseal; nodes and clients refresh the map on a newer epoch or `E_MOVED`. Crash-tested at every step with each party killed (67 scenarios): never two serving groups, every acknowledged transfer kept, IDs unchanged, all settled | `src/core/node_link.cpp`, [CLUSTER.md](CLUSTER.md), [CONTROL_PLANE.md](CONTROL_PLANE.md), [MIGRATION.md](MIGRATION.md) |
| Real-machine lab | Done (first run, 2026-11-04): three Vultr VPSs (2 vCPU, one datacentre, RTT ~0.34 ms) ran P0-P6 and a 20-minute soak. Sync replication costs ~1.0x across machines (0.54-0.60x on loopback); kill/stop fail over in ~1.5 s, a silent partition in ~7.7-8.5 s (cause of the difference not yet attributed); slot moves keep a 0.1-0.2 s `E_MIGRATING` window; the largest slot (about 31K objects, reported as 100K because of a `snc_load` bug, since fixed; a real 100K slot moved in 57 s on loopback, where the move also caused a false failover of the destination: EVIDENCE section 12) moved in 11.3 s at `--chunk 256` but failed at `--chunk 8` (needs the chunked stream). The partition test found a double apply (resend refused `E_OVERLOADED` before its session was checked), fixed in PR #28 with a test that fails without it; 24 of 24 partition rounds on fresh hosts then passed (corroborating, not proof). Fresh hosts: `bench/lab/vultr_setup.sh` | `bench/lab/`, `bench/snc_load.cpp`, `bench/results/lab-20261104/`, [REAL_MACHINES.md](REAL_MACHINES.md), [EVIDENCE.md](EVIDENCE.md) |
| ScyllaDB comparison (handoff §14) | Done: about 50K ops/s with counter deposits, 10K with LWT, on the same 2 cores; SNC is about 50x and 20x that | [CONVENTIONAL.md](CONVENTIONAL.md) |
| Request routing to the owning shard | Done: per-shard TCP listeners, wire protocol v0, owner-aware client, owner-local minting for creates; measured against the baseline behind the same front end | `src/net/`, `include/snc/wire.h`, `tools/snc_server.cpp`, [ROUTING.md](ROUTING.md) |

## Known deviations from the spec

- **Recovery after a remap routes records on the control thread.** With an
  unchanged slot map, every shard reads its own checkpoint part and WAL
  stream in parallel. After a shard-count change (or before the first
  checkpoint exists) records are read on the control thread and routed to
  their new owners; a checkpoint follows, so this happens once per remap.
- **Recovery reads the whole segment that holds the checkpoint's start
  LSN**, CRC-checking records it then skips. There is no seek index.
- **Segments are allocated ahead, not zeroed ahead.** A preparer thread
  keeps one `fallocate`d, synced spare per stream; the shard renames it
  into place at rollover and writes the header with its first flush. The
  rename is not directory-synced on the shard; recovery adopts a spare
  whose header is valid instead.
- **A hole inside a segment ends it.** Anything after the first invalid or
  out-of-sequence record in a segment is treated as an unacknowledged tail,
  which is right for crashes (one flush in flight per stream) but cannot
  tell mid-segment media corruption apart. A gap *between* segments still
  stops recovery (C4).
- **Creates always wait for durability**, whatever `ExecOptions` says, so a
  new ID is never handed out before its record is on disk.
- **New registry types are accepted** at recovery even when the manifest's
  type catalog does not list them; only types that disappeared or changed
  schema stop recovery.
- **Local map growth rehashes in one step**, not incrementally. Engine
  pre-sizes maps from `expected_objects` to keep rehash out of runs.
- **Handler time budget** (20 µs, spec section 5) is not measured yet.
- **`BUF_RETURN` is sent per buffer**, not batched.
- **Ring stats are read racily while running** (benchmark only); they are
  exact after `stop()`.

## How durability is tested

- `test_persistence`: create, deposit, re-key email, delete; clean restart
  from the WAL alone; checkpoint then more writes; restart with 3 shards
  (slot remap, orphan stream folded into a forced checkpoint) and back to 4;
  a garbage block written after the last record of a segment (torn tail).
  Every phase checks every object's version and balance and samples the
  email index from all shards. Also checks that IDs minted after a restart
  use fresh leases (C12) and that a missing superblock without `init` is
  refused (C13).
- `test_crash`: a child process creates accounts and deposits into them,
  reporting each acknowledged result to the parent, which kills it with
  SIGKILL at a different point each round (4 rounds on one directory).
  A fresh process recovers and checks that every acknowledged version
  survived and that every account's balance matches its version. A process
  kill does not tear writes the kernel already accepted, so torn tails are
  covered by `test_persistence`, not here; power loss is not tested.
- `test_net`: the TCP front end in memory and durable: topology, creates
  that stay on their entry shard, requests to the owner and forwarded
  requests (flagged), reads, unknown targets and ops, a client that
  disconnects with 2,000 requests in flight, and a malformed frame.
- All 16 ctest tests pass in the RelWithDebInfo build (2026-11-04).
- Sanitizers (2026-11-04, Debug): ASan+UBSan 16/16, with 0 reports from
  server child processes of the Python suites; TSan 16/16 after fixing a
  data race in `test_cluster`'s own acknowledgement counter (a test
  defect). Details: [STABILIZATION_REPORT.md](STABILIZATION_REPORT.md),
  "Sanitizer baseline".
- Contracts, what each is checked by, and what is still uncertain
  (retry contract, acknowledgement stages, failure detection):
  [EVIDENCE.md](EVIDENCE.md).
- Core stabilization (2026-11-04, characterization only, no behavior
  change): [CORE_CONTRACTS.md](CORE_CONTRACTS.md) (draft contracts),
  [FAILURE_MODEL.md](FAILURE_MODEL.md), [INTERACTION_MATRIX.md](INTERACTION_MATRIX.md),
  [TRACING.md](TRACING.md) (`SNC_TRACE=1` stopgap tracing, proposed
  rings), [STABILIZATION_REPORT.md](STABILIZATION_REPORT.md) (the 100K
  false failover explained, resource inventory, Baseline 0.2 criteria).
- HA health decoupled from shard work (2026-11-04, option B):
  [HEALTH_DECOUPLING.md](HEALTH_DECOUPLING.md) section 10. Witnesses
  poll `HEALTH`, answered without waiting on shard threads; a busy shard
  no longer causes a failover (`tests/test_health.py`; the 100K move ran
  with no promotion). Shard stall is reported, never acted on: a shard
  wedged forever is not failed over (accepted until work is bounded).
  Effective only when every witness of a group polls `HEALTH`.
- Bounded/resumable migration staging (2026-11-04, **design accepted with
  modifications, not built**): [MIGRATION_STAGING.md](MIGRATION_STAGING.md).
  The final stage of a big slot still holds the destination shard for
  seconds (4.8 s at 100K objects).
- Fail-closed migration serving, D1/D7 (2026-11-04, **implemented and
  approved**): [MIGRATION_FAIL_CLOSED.md](MIGRATION_FAIL_CLOSED.md)
  section 20. Before it (reproduced: `bench/results/failclosed-20261104/`),
  a lossy promotion (async, admin `PROMOTE`, or restarting a standby's
  directory as a primary) let a moved slot be served from a partial or
  stale copy, and a source that lost its seal serve and lose writes. Now,
  with placement v2 (`--placement-v2`), a slot serves only with local
  proof of the move that last gave it to the group; promotion and a
  former standby's start re-seal before serving (`DIR/role`); the commit
  waits for the source group. Not covered: a second primary the driver
  cannot reach (R08), slots moved under v1 until backfilled; an unproven
  slot stays unavailable (no repair yet).
- Reference architecture research lane (2026-11-04, **program added, no
  review started**): [research/README.md](research/README.md). Styx,
  Restate, Orleans, SpacetimeDB and DBOS, compared mechanism by mechanism
  against SNC's contracts; runs alongside Core work and changes nothing by
  itself.
- S1 bounded destination staging (2026-11-04, **approved** after the
  review of PR #44): [MIGRATION_STAGING.md](MIGRATION_STAGING.md) section 18. Every
  STAGE (final and copy ahead) and its reply report run on B's shard as a
  chain of bounded units with guards between them (fence, token,
  placement, mutation, rehash), a durability window, and a driver resend
  instead of HAVE/EXPORT loops; `--move-units off` is the one-task
  rollback. At 100K objects:
  - **the longest non-preemptible unit falls from 2979 ms to 3.8 ms**;
  - STATUS max during the final falls from 2986 ms to 16 ms;
  - foreground deposit max falls from 1.6 s to 32 ms.

  The costs are the total move (+45%) and the client-visible `E_MIGRATING`
  window (+37% shared email, +10% unique). The shared-email index costs
  about 0.2 ms per put: per object, within the unit bound, a separate
  primitive. Tests: `test_stage_chain`, `test_s1` (S1-1..S1-18). Raw
  results: `bench/results/s1-20261104/`.
- Legacy role upgrade hardening (2026-11-04, **approved** after the review
  of PR #41): [LEGACY_ROLE.md](LEGACY_ROLE.md). A data
  directory from before role files no longer starts as a primary by
  default: it is a standby (`--standby`) or is adopted once
  (`--adopt-role primary`), refused if a group member serves, is silent or
  is at a newer term, and started through the barrier.

## Benchmark numbers

Routing over TCP, and the cause of the durable p99 gap, are in
[ROUTING.md](ROUTING.md). See [BENCHMARKS.md](BENCHMARKS.md) for the step 12 comparison against a
conventional thread-pool baseline, including durable throughput, latency,
memory and restart time.
