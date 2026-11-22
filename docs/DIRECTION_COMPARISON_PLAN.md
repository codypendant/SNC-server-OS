# SNC direction comparison plan (PROPOSED revision 2, authorizes nothing)

## 0. Status, authority and labels

- **Source.** cody's directive "SNC — Next Research and Testing Direction", 2026-11-22 02:09 UTC, written with Codex.
  It is filed verbatim in `docs/handoff/SNC_Next_Research_Direction_20261122.txt`.
- **Revision 2** applies Codex's HOLD review of revision 1 (`docs/handoff/PR186_Codex_Review.md`), items 1 to 6, and
  a focused review told to break the corrections (section 8).
- **What this is.** A bounded plan for owner and Codex approval. It is not a result and not a decision.
- **What it authorizes.** Nothing. These boundaries hold throughout:
  - no harness code, no runs, and no dependency adoption;
  - no production dependency selection;
  - no change to PR #185 or `experiments/ar1/`. PR #185's G2 adequacy gaps remain open;
  - AR-1's G3 stays on HOLD. Track A runs are not AR-1 counted runs and say nothing toward G3;
  - Track A cannot execute until its prerequisites are met and cody authorizes it. Track B needs its own
    authorization;
  - no raft-rs integration and no custom replication work;
  - no change to SNC's engine;
  - no implicit decision to abandon the original OS, whatever the results (section 6).
- **Gates.** Each needs cody's explicit yes (section 9):
  - **P-G1:** approve this plan and set the PROPOSED numbers;
  - **P-B:** start Track B. Each R640 session needs cody's hands;
  - **P-A:** start Track A. It also waits until PR #185's G2 gaps (U-1 to U-13, amendments A-1 and A-2) are closed
    and reviewed.
- **The direction stays open.** Nothing here assumes a full OS, an agent runtime, an SDK or a hybrid. Section 6 says,
  before any result exists, which results support which direction.
- **Labels** (`docs/ENGINEERING_PRINCIPLES.md`):
  - **FACT:** read in this repository;
  - **EXT:** external, cited;
  - **EXT-SRC:** read in source code, cited;
  - **INF:** inference;
  - **HYP:** untested;
  - **UNVERIFIED:** not checked;
  - **PROPOSED:** a number for cody to set before any run.
- **Sources.** Pinned primary sources are in `docs/research/DIRECTION_COMPARISON_SOURCES.md`. No competitor
  performance figure appears in this plan or in that file. Thresholds go into
  `bench/results/<campaign>/CRITERIA.md`, committed before the first comparative run
  (`docs/EXPERIMENTAL_VALIDATION.md` section 4).

## 1. Recommendation

Approve this plan (P-G1). Then run three bounded experiments:

1. **B1, durable object execution** (Track B, R640, one session). SNC against Garnet running the same logic next to
   the data with per-operation commit wait. PostgreSQL and our own `baseline_server` run alongside as checks. This is
   the cheapest test that could falsify the tested owner-local execution approach.
2. **B2, durable messaging between entities** (Track B, R640, one session). SNC's existing transfer saga against
   Restate Virtual Objects and a PostgreSQL transactional outbox.
3. **Track A, agent runtime** (cloud VM). Up to four idiomatic competitor arms and SNC's AR-1 arm, judged only on
   observable behaviour. It starts only after PR #185's G2 gaps are closed and reviewed, and cody gives P-A. It can
   run alongside Track B because the hardware differs.

One report then answers the five criteria (section 5), and the fixed rule in section 6 turns the results into the
evidence each direction can cite. The decisions stay cody's.

Track B goes first because SNC's engine needs no change for it, and it answers the original idea on its own terms.
Track A answers a separate question. SNC's agent arm is built on DBOS and PostgreSQL, not on the state-native core,
so neither track's result counts for or against the other track's direction.

## 2. What the evidence already says, and does not

| Evidence | What it shows | What it does not show |
|---|---|---|
| In process, 4 vCPU VM, against our own striped-lock map [FACT `docs/BENCHMARKS.md`, `docs/HUGEPAGES.md`] | Memory, all local: 1.47x (1.25x with huge pages on both). Durable, all local: 1.39x over the 4-log baseline. All remote: **0.87x** in memory and **0.91x** durable | The baseline is ours. AGENTS.md invariant 9 asks for competent alternatives |
| Over TCP against our own shared-memory server [FACT `docs/ROUTING.md` "What the numbers say"] | Owner-routed: **1.30x** in memory and **1.23x** durable. "The network dominates the per-op cost" | This is the closest prior to B1. It predicts a small margin over sockets |
| Over TCP against PostgreSQL, Redis and ScyllaDB [FACT `docs/CONVENTIONAL.md`] | 9-50x | Not like-for-like: it measures the application/database boundary. No server-side logic on the baselines |
| R640, Profile A [FACT `docs/STATUS.md`] | An objective rate of 654,569 ops/s at p99 about 0.93-1.0 ms | No competitor ran on the R640. CRITERIA is on HOLD |
| Sync standby [FACT `docs/REAL_MACHINES.md` P2] | 0.54-0.60x of non-replicated on loopback, but **0.98-1.05x** on separate hosts. The loopback figure was CPU contention | Not used: Track B has no replication |
| Transfers [FACT `docs/TRANSFERS.md`] | A cross-shard transfer is a saga with an outbox (three records, three messages), acknowledged at the durable debit | |
| AR-1 G2 [FACT PR #185 `experiments/ar1/results/G2_REPORT.md`] | SNC's arm passed 246 of 246 bring-up runs, and Phase 0 passed at bring-up scale | Uncounted. SNC's arm runs on DBOS and PostgreSQL, so it says nothing about the core |
| Plain DBOS arm, step 1 (same report) | 148 violations and 98 unreachable points in 246 runs | Not evidence about DBOS (below) |

**The plain DBOS step 1 result, read honestly** [INF]. It was written "as an application would normally write it"
(AR1_PREREG section 8), not written to meet items 1-7.
- Its largest clause, 4.mapped (473 firings), checks for an SNC dispatched-attempt record.
- All 98 unreachable runs sit at SNC-specific commit points (`*-prep`, `*-disp`, `I-acc`).
- Some failures are real behaviour of that code: 2.no_repeat (66) and 5.TI_one_effect (22) are effects repeated after
  a crash. But DBOS documents the tools that prevent them: datasource transactions and idempotency keys [EXT].

So step 1 is a baseline that was not written to the same goal. It is not evidence that DBOS cannot provide the
guarantee, and Track A does not use it as such.

**What could falsify the original idea** [INF]. The in-house margins are 1.23-1.47x on the best case, and below 1.0x
on remote work, against baselines we wrote. A production engine with server-side logic removes the boundary cost
behind the 9-50x figures. **The prior, written down before any run: B1 most likely ends FAIL or NARROW at the
proposed thresholds** (ROUTING.md's 1.23x durable sits below B1's 1.3x FAIL line). B1 is chosen because it can say
so with a competent baseline. B2 tests the tested saga-messaging approach: whether owning state on a core makes durable
work between entities cheaper than an engine built for it.

## 3. Track A: agent runtime

### 3.1 Question

What do DBOS, Restate, Temporal and LangGraph already give a persistent agent when written the way their docs
recommend? What does SNC's S1 layer add, measured only by behaviour anyone can observe?

### 3.2 Scenario and arms

Each arm runs AR-1's scenarios (`docs/AR1_PREREG.md` section 4), restated so that no SNC concept is required:

- **S-M:** one agent and one task: a model call, then three tools (T-I, T-F, T-N), one memory write, and completion.
  **S-AP** adds one policy decision that needs an approval; odd seeds deny it.
- **Deterministic causality.** Each tool request's arguments and the memory value are fixed functions of the stub
  reply id and earlier tool replies. So the checker can tell from the logs alone which model reply every later step
  used.
- **Request ids.** Each external request carries an id the arm chooses. The checker uses it only to correlate
  attempts across logs.
- **The services** (reused from AR-1, specified generically):
  - **T-I:** dedupes on an idempotency key the arm supplies.
  - **T-F:** takes an integer token, and rejects any token below the highest it has seen for that agent. The arm may
    derive the token from anything: a database sequence, a workflow attempt number, a state version.
  - **T-N:** no key, no status query.
  - **The approver** is asynchronous. It logs the request, then posts its decision to the arm through an ingress the
    arm chooses (a signal, a durable promise, a message, `Command(resume=...)`).
  - **The owner** is a service the arm must call when it cannot know a T-N outcome, after a timeout of its own
    choosing.
  - The services are shared harness code and are not counted. The arm's own calls to them, its token derivation and
    its owner call are counted.

| Arm | Suggested idiom (the implementer and the strengthening reviewer may change it) | Language |
|---|---|---|
| **SNC** | AR-1's `snc_rt` as closed in PR #185, plus an adapter for the read interface below. Its takeover is its documented recovery entry point, a new epoch | Python |
| **DBOS** | Agent row in an app table; one short workflow per task; datasource transactions for state; a partitioned queue with `partition_concurrency` 1 for one writer per agent; `recv` for the approval | Python |
| **Restate** | One Virtual Object per agent (K/V state, one writer per key); `ctx.run` for the model and tools; a durable promise or awakeable for the approval | **Rust SDK** (0.13, "might break across releases" [EXT-SRC]) |
| **Temporal** | Entity workflow per agent, or one workflow per task; activities for the model and tools; a Signal for the approval; Continue-As-New | Python |
| **LangGraph** | OSS library; Postgres checkpointer with durability `sync`; one node or `@task` per side effect; `interrupt()` for the approval; the Store for memory | Python |

- **Rust.** Restate's arm uses the Rust SDK because the directive names it and SNC-ADR-RUST-001 makes Rust maturity a
  real question. DBOS Rust 0.6 is "pre-production" [EXT], so it is recorded from the docs, not built.
- **LangGraph** is compared as the OSS library. Agent Server needs Redis and a licence key [EXT], so it is described
  from its docs only.
- **Durability.** Every competitor stores state with fsync before acknowledgement on one host, with a single copy:
  - PostgreSQL 18 with `synchronous_commit = on` and `fsync = on` and no standby, for DBOS, Temporal's persistence and
    LangGraph's checkpointer;
  - Restate's RocksDB log with fsync on, which is the default [EXT-SRC].

  Only SNC's arm keeps AR-1's same-host synchronous standby, because its design needs it. Its cost is charged to SNC
  in the latency and operations rows. No matrix point kills a disk, so a second copy changes no correctness outcome.

### 3.3 Observable guarantees

The checker reads only:
- the logs of the stub, tools, approver and owner;
- two calls that each arm implements however it likes: `state(agent)` and `reconstruct(task)`.

| Guarantee | Pass condition, from the logs | Directive dimension |
|---|---|---|
| **O-1 Recovery** | The task completes, or it stops only on a T-N call with no logged outcome after calling the owner. Every downstream request uses one model reply id. The model is called again only if no downstream request used the earlier reply | recovery |
| **O-2 Memory and one writer** | After the run ends and a quiesce period (10 s, PROPOSED), `state(agent)` equals the value derived from the replies that the downstream requests used. No tool log holds a request outside the set a single uninterrupted execution would send, except a T-F request the tool rejected | persistent memory |
| **O-3 Effects** | T-I: exactly one effect per logical call. T-N: no second request after an unknown outcome unless the owner said it did not happen. T-F: every stale token is rejected. A denied approval leaves no tool request | tool-effect safety |
| **O-4 Reconstruction** | For every tool and stub request, and for every policy decision including denials, `reconstruct(task)` returns: the exact model input bytes (the checker hashes them), the policy rule version, the approval, every attempt, and the outcome. Each answer matches the logs. The arm's retention setting is recorded | decision reconstruction |
| **O-5 Events** | An approval posted while the agent's worker is down, or before the arm has durably reached its wait, is delivered once, and the task proceeds on it | durable events |

**Rule (from the directive).** A missing SNC record is never a failure. Any engine history counts as a source for
`reconstruct`: Temporal's Event History, Restate's journal or state, DBOS step outputs, LangGraph's checkpoints, or
app tables. Storing what the engine does not keep is legitimate added code; DBOS, for example, records step outputs,
not step inputs [EXT]. If a default retention deletes history (Restate 24 h, Temporal namespace retention [EXT]),
the arm raises it by configuration, and the value is reported.

### 3.4 Crash points at the shared boundaries

AR-1's points are SNC's internal commits, so other arms cannot reach most of them (98 unreachable runs in G2).
Track A uses only boundaries every arm shares. The receiving service triggers the fault, so no arm needs hooks. After
every kill, the harness restarts the arm's workers at once, through each arm's documented recovery entry point:
- DBOS: restart, which recovers `PENDING` workflows;
- Restate and Temporal: start a worker;
- LangGraph: re-invoke the `thread_id`;
- SNC: its new epoch.

| Points | What happens | Scenario | Points | Seeds per point | Runs |
|---|---|---|---|---|---|
| A01-A04 Kill at receipt | The model stub or a tool (model, T-I, T-F, T-N) gets a request; the arm's workers are `SIGKILL`ed before the reply | S-M | 4 | 2 (nothing varies) | 8 |
| A05-A08 Kill after reply | As above, after the reply is flushed, with a delay of `[0, 1, 5, 50][seed mod 4]` ms (so 0 and 1 ms get three seeds, 5 and 50 ms two; the imbalance is accepted) | S-M | 4 | 10 | 40 |
| A09 Kill at approval | The approval request is logged, then a kill, then the decision is posted | S-AP | 1 | 10 (approve or deny) | 10 |
| A10 Event before wait | The decision is posted before the arm durably reaches its wait, then a kill | S-AP | 1 | 10 (approve or deny) | 10 |
| A11 Kill during recovery | A first kill at receipt of T-I (as A02), then a second kill at the first boundary the recovered run reaches | S-M | 1 | 2 (nothing varies) | 2 |
| A12-A13 Engine restart in flight | The Temporal or Restate server, or PostgreSQL for library arms, restarts at receipt of T-I and of T-N | S-M | 2 | 2 | 4 |
| A14-A17 Zombie takeover | The worker is `SIGSTOP`ped at receipt (model, T-I, T-F, T-N); the harness starts one fresh worker; the arm's own mechanism takes the task over (timeouts may be lowered by configuration, and are reported). The seed sets the release order. `zombie_first`: the zombie resumes right after the new worker starts. `new_first`: it resumes at the first of: the new worker passing the same boundary, the new worker stopping in doubt, or a fixed time T (PROPOSED 60 s). No takeover within T is UNRESOLVED, not a failure | S-M | 4 | 10 (release order) | 40 |
| **Total** | | 94 S-M runs, 20 S-AP runs | **17** | | **114** |

That is **17 points and 114 runs per arm.** Every run is listed, one row each, in
`docs/DIRECTION_COMPARISON_TRACK_A_MATRIX.csv`, written by `docs/DIRECTION_COMPARISON_TRACK_A_MATRIX.py` from the
point definitions. A reviewer recounted this table by hand and the CSV with their own script, and found 17 points,
114 runs, 94 S-M and 20 S-AP in both (section 8). CRITERIA.md freezes that file. Run time is set
mostly by the takeover timeouts. The estimate is 1-2 hours per arm [HYP]; bring-up measures it before any counted
run.

**Out of scope, with reasons:**
- cancellation (AR-1 D): it needs an SNC-defined cancel race;
- AR-1's internal kill-before-send points: the after-reply delays and the zombie points cover the observable cases;
- concurrency beyond the takeover pair, and scale: these are S2.

### 3.5 Idiomatic implementations, written fairly

- **Step 1.** A separate implementer per arm writes the scenario from that system's docs. A second reviewer is told to
  make the arm **stronger**: more idiomatic, shorter, using any documented feature. Step 1 is then **frozen before
  any matrix run**.
- **Step 2.** If step 1 fails a guarantee, the smallest additions that make O-1 to O-5 pass are written. Reviewers may
  submit shorter correct versions, and the shortest correct version counts.
- **Counting.** Logical lines after a pinned formatter (`black` for Python, `rustfmt` for Rust): not blank, not
  comment, and not brace-only or punctuation-only. A line modified between step 1 and step 2 counts once. Every count
  is reported **by language and by class**, for step 1, step 2 and the whole arm, with SNC's own runtime alongside.
  The shared services and harness are never counted.
- **One preregistered distinction: N (new infrastructure) or not N.** A line is **N** if it builds a mechanism the
  engine does not provide, in order to meet O-1 to O-5:
  - it declares, migrates, reads or writes a persistent structure the arm creates for that purpose (attempt or
    dispatch logs, dedupe tables, fencing records, stored model inputs, a history store);
  - or it implements a loop, scan, timer or background task outside the engine's APIs;
  - or it is in a function whose only purpose is one of these.

  Everything else is **not N**: the scenario's own application data (agent memory and task state), engine features
  used as documented, configuration, and scenario logic.
- **Worked examples**, frozen in CRITERIA.md before any arm is written:

  | Example | Class |
  |---|---|
  | Agent memory as a DBOS app-table row, a Restate Virtual Object key, a LangGraph Store item or a Temporal workflow variable | not N (application data) |
  | Workflow ids, step outputs, journals, Restate idempotency keys, Temporal Update ids, DBOS `send`/`recv` and events, used as the engine provides them | not N (engine feature) |
  | A Temporal memo or search attribute; an engine queue or scheduled workflow used as a poller | not N (engine feature) |
  | A table or key that records attempts, dispatches, fencing tokens or model inputs for `reconstruct` | N |
  | A thread, cron or loop outside the engine that scans for unfinished work | N |
  | SQL DDL, YAML or a server manifest that creates an N structure | N, counted at a factor of 1.0 against the host language's L, and reported in its own row |

- **Classifiers.** Two classifiers label every line, independently. They label after the arm's code is frozen and
  before any cross-arm comparison is seen. On disagreement the line is **not N**, which is the competitor's favour.
  If they agree on fewer than 90% of the lines either one calls N (PROPOSED), that arm's criterion-3 result is
  UNRESOLVED. Both label sets are published.
- **The gate, set before any code exists** (all PROPOSED, for cody):
  - **N lines in the whole arm** (step 1 plus step 2): at most **300** in Python, at most **450** in Rust. The factor
    of 1.5 is cody's choice; there is no measured basis for it [HYP];
  - **and total lines in the whole arm**, all classes: at most **1,000** in Python, at most **1,500** in Rust. That
    way a very large glue layer does not count as "readily reproduced". For scale, SNC's AR-1 runtime is 907 Python
    lines [FACT, G2 report].

  "Within L" in sections 5 to 7 means within both limits.
- **This L is this plan's own threshold.** It is separate from AR-1's L for plain DBOS.
- **Configuration only, never engine changes.** If a guarantee needs a change to a competitor's source, it counts as
  "not readily reproducible" for criterion 3.
- **The research file lists, per system, the usual ways a comparison is unfair to it.** Each is a review checklist
  item. Examples: LangGraph on `async` durability; Temporal without Continue-As-New; Restate code changed under a live
  deployment; DBOS on SQLite.

### 3.6 Performance and operations (reported, not gated)

- **Per task and per external call** (the same denominators for every arm): added latency, net of stub and tool time,
  at p50 and p99, for 1 agent and for 100 concurrent agents. Also bytes stored per task, and recovery time from a
  kill to the next external request. Stub latency is 500 ms (AR-1's performance setting).
- **Operations rubric:**
  - processes and services needed;
  - configuration keys changed from defaults;
  - deployment steps for single-node durability;
  - operator actions after a crash and after a code change;
  - the licence of every part used.

  These are counted, not scored.

## 4. Track B: two decisive benchmarks

Both run SNC in Profile A (LOCAL_DURABLE, `docs/EXPERIMENTAL_BASELINE.md`): one node, no standby, no replication, no
raft-rs. SNC's engine is unchanged. Each baseline is an established system used through documented features only.

### 4.1 Rules shared by B1 and B2

- **Hardware.** The R640, allocation PILOT-L (`docs/PILOT_PLAN.md`): the system under test and its load generator on
  disjoint node-0 cores, labelled loopback. Each system gets the **same 8 physical server cores**, with their
  hyperthread siblings idle, and the same data SSD. Garnet's .NET GC heaps and thread affinity are pinned to those
  cores. The manifest is captured first (`bench/manifest.sh`).
- **Clients.** Every load client uses the shared open-loop scheduler (`bench/rate_schedule.h`), measures latency from
  the intended send time, and runs on the same client cores. The pilot's client-limit check (C3) applies to every
  system. A client found limited makes the point invalid, not slow.
- **Capacity at SLO.**
  - It is the highest offered rate at which three 120 s runs each meet all of:
    - p99 at or below the SLO;
    - zero errors;
    - application rejections (no funds, account busy) at or below 0.1% of operations;
    - a bounded backlog: work owed plus in flight at the end of the window at most depth × threads, and the run
      drains.
  - The rate is found by bisection to 5% steps.
  - Three runs are made only at gated points. Points that do not gate run once.
  - The warm-up and all bounds are written into CRITERIA.md.
- **Funds.** Accounts start with a balance large enough that no transfer fails for lack of funds (PROPOSED 10^12).
  Every baseline runs the same funds check and reports its rejections.
- **Correctness.** Each run ends with a conservation check:
  - wait until nothing is in flight: for SNC, the `emits_now` gauge reads 0 on every shard and no account has outbox
    entries, in two quiet rounds in a row (a retried credit can make one sample read 0);
  - then check that the sum of balances equals the start total plus the acknowledged deposits, with in-doubt deposits
    as a bound.

  `net_bench --verify` does not cover transfers today [FACT `bench/net_bench.cpp`], so this is new harness code.
  A run that fails the check is invalid.
- **Durable acknowledgement is verified through the device and storage stack, for every system, SNC included.**
  - **Device.** Bring-up runs on a cloud VM with the R640's kernel, file system and mount options. The data
    directory sits on `dm-log-writes`, a device-mapper target that records every write and flush in order. Under it
    is a loop device set to `queue/write_cache = write back` and `queue/fua = 0`. The block layer then turns every
    FUA into a write plus a flush, so a flush is the only durability point in the log. Both sysfs values go into the
    manifest. The file system is created through the logged device, with a mark "mkfs", as Linux's crash-consistency
    tests do (xfstests generic/482). Building `replay-log` from xfstests `src/log-writes` on that VM is UNRESOLVED
    until bring-up.
  - **Controls, before any system runs.** `fio --direct=1 --fsync=1` must pass the check below. `fio --direct=1` with
    no sync must fail it. If either control misbehaves, the test is invalid on that host.
  - **Run.** Each system runs at about 25% of its capacity, so group commit is exercised. The harness reads
    `CLOCK_MONOTONIC`, then inserts a mark (`dmsetup message`) every 0.5 s. The client logs every acknowledgement on
    the same clock.
  - **Check.** For each of 20 marks per configuration (PROPOSED), the log is replayed from "mkfs" to the last flush
    before the mark, which models losing every volatile cache at that instant. The system recovers from that image.
    Then, for every account, the recovered balance is at least the start plus every deposit acknowledged before the
    mark, and at most that plus the deposits in doubt. Transfers are checked the same way on both accounts, and
    money is conserved.
  - **Pass** means no violation at any mark.
  - **Order with tuning.** Only configurations that passed this test may be screened or counted. So every candidate
    configuration of the tuning budget is tested on the VM before the R640 session.
  - **On the R640.** Each counted run must use the binary hash, configuration-file hash and command line of a
    configuration that passed. A 10 s `blktrace` sample per configuration must show flush or FUA requests on the
    data device. The R640 device reports `fua=1` [FACT `docs/R640_TOPOLOGY.md`], so its I/O path is not the VM's
    `fua=0` path. That gap, and whether the SSD has power-loss protection, are UNRESOLVED and the same for every
    system.
- **Only a verified configuration can gate.** Each benchmark has a gating order, fixed now. The gating baseline is the
  first system in the order whose best verified configuration passed. The others run as diagnostics, labelled as
  such. If SNC fails the test, that benchmark stops and the defect is recorded; no fix is made inside this plan.
- **One rule for PostgreSQL in both benchmarks.** If PostgreSQL (B1) or the PostgreSQL outbox (B2) ends up gating, a
  PASS is recorded as **NARROW at most**. PL/pgSQL removes the client-side logic, but PostgreSQL still runs general
  SQL execution, MVCC and one shared WAL, so beating it shows less than beating an engine built for the job. A FAIL
  stays FAIL.
- **Completion boundaries.** A transfer is **fully settled** when the credit to the target is durable. That is the
  only boundary used for decisions, and it is the same for every system:
  - atomic systems settle at their acknowledgement;
  - sagas and outboxes (SNC, Restate, the PostgreSQL outbox) settle later.

  The earlier **debit acknowledgement** is reported for every system, but it never decides anything.
- **Settlement probe (harness code, the same method for every system).**
  - **Share:** 1% of transfers (PROPOSED) go to probe accounts that receive nothing else.
  - **Pool:** at least the probe rate × p99.9 settlement time × 10 accounts. They are spread evenly across shards
    and partitions, and they follow the benchmark's locality rule, so B2's probes cross shards. Each probe account
    has at most one probed transfer outstanding.
  - **Poll:** every system polls on the same grid from the transfer's intended send time. The interval is SLO/20:
    100 µs in B1 and 500 µs in B2. There is one outstanding poll per target; the next one goes at the later of the
    next grid tick and the previous reply. A poll succeeds when the balance equals the earlier balance plus the
    amount.
  - **Durable reads:** SNC's default read, which waits for durability; PostgreSQL's committed read; Garnet's read,
    where the observation time is the later of the first successful poll and the transfer's commit-waited
    acknowledgement; for Restate, the cheapest read shown durable in bring-up (a shared handler or SQL state
    introspection, both UNRESOLVED). If neither qualifies, Restate's exclusive handler is used. Each poll is then a
    journaled invocation, which is labelled "favours SNC, used for decisions".
  - **Cost:** probe reads are counted and reported, and capped at 1% of all operations.
  - **Result:** full-settlement latency is the time from the intended send to the successful poll. It is an upper
    bound for every system.
- **Backlog at the end of a window, counted the same way for every system:**
  - SNC: outbox entries summed over shards;
  - Restate: pending invocations, from SQL introspection;
  - the outbox: rows in the outbox table;
  - Garnet and PostgreSQL: zero.

  The limit is one absolute number for every system, written into CRITERIA.md.
- **Settings that are never allowed:**
  - Garnet `--aof-commit-freq` above 0, or no `--aof-commit-wait`;
  - PostgreSQL `fsync=off`, `full_page_writes=off`, or `synchronous_commit=off`;
  - Restate `rocksdb-disable-wal-fsync`.
- **Tuning budget per baseline.** Up to 12 documented configurations, chosen before the runs and reviewed by Codex.
  For the PostgreSQL outbox this covers the worker's poll or NOTIFY interval, batch size, worker count and
  `SKIP LOCKED` use, starting from an example design fixed in CRITERIA.md. Each verified configuration is screened with
  one 30 s closed-loop run on the R640 inside the session. The best one counts.
- **Equivalence gaps are listed with the side each favours** (sections 4.2 and 4.3), and reported next to every
  result.

### 4.2 B1: durable object execution

**Claim tested.** Executing a registered handler on the owning core, with a WAL per shard, beats a competent engine
that runs the same logic next to the data, at the same durability.

- **Workload:** 1M accounts, 16-byte keys, 200-byte values. Two gated points:
  - **0% transfers:** 50% reads and 50% deposits;
  - **20% transfers:** exactly 40% reads, 40% deposits and 20% transfers of 1 cent between uniformly chosen accounts.
    In `net_bench` that is `--transfer 20 --reads 50`: transfers are taken first and the rest split by `--reads`
    [FACT `bench/net_bench.cpp` 755-758]. With `--xfer-local -1` about one transfer in eight is same-shard and
    atomic.

  One non-gating point: Zipf 0.99 at 0% transfers.
- **SNC:** `net_bench` owner-routed, with `--rate`, `--transfer`, `--reads`, `--xfer-local` and `--zipf` [FACT].
- **Garnet** (MIT):
  - `--aof --aof-commit-freq 0 --aof-commit-wait`;
  - reads and deposits through the cheapest documented single-key path (native commands or a custom command);
  - transfers as a C# `CustomTransactionProcedure` through `RUNTXP`, which locks both keys and recovers atomically
    [EXT];
  - `--aof-physical-sublog-count` is part of the tuning budget.
- **PostgreSQL 18:** a deposit is one `UPDATE ... RETURNING`; a transfer is one PL/pgSQL call; `commit_delay` is
  tuned.
- **`baseline_server`** (diagnostic only): our own shared-memory server. It calibrates the over-TCP prior from
  ROUTING.md on this hardware.
- **Gating order:**
  1. Garnet with its default `Native` device;
  2. Garnet with `--device-type FileStream`;
  3. PostgreSQL 18.

  Garnet's default Linux device opens files `O_DIRECT` with no `fsync` or `O_DSYNC` in its code [EXT-SRC], so it may
  fail the durability test. If neither Garnet configuration passes, Garnet's numbers are diagnostic and PostgreSQL
  gates, under the PostgreSQL rule of 4.1.
- **Garnet sizing:** `--memory` is set so that the 1M accounts never spill to disk, and checkpoints stay outside the
  measured window. If `FileStream`'s write-through also applies to the main log (UNVERIFIED), any remaining cost is
  labelled "favours SNC".
- **Equivalence gaps:**

  | Gap | Favours | Used for decisions? |
  |---|---|---|
  | Garnet and PostgreSQL transfers are atomic transactions; SNC's is a saga with no isolation across the two objects | the baseline, in semantics | no; stated |
  | SNC acknowledges a transfer at the durable debit; atomic baselines at commit | **SNC**, at debit acknowledgement | **no**: only full settlement decides |
  | Garnet readers may see writes not yet committed (UNVERIFIED); SNC reads wait for durability | the baseline | **yes**, through read p99. The probe counts a Garnet observation only after the commit-waited acknowledgement |
  | The cost of a durable read differs by system | depends on the system; see the probe rules | yes, through the probe |
  | SNC's 64-byte binary protocol over owner-routed connections, against RESP and libpq | **SNC** | yes, unavoidably. This is part of what "state-native" means here, and it is stated next to every result |
  | SNC's mature `net_bench`, against new clients for the baselines | **SNC** | limited by the shared scheduler and the C3 check |

- **SLOs (PROPOSED):**
  - p99 at most 1 ms for reads and deposits, the same edge as CP-1A;
  - p99 full-settlement latency at most 2 ms for probed transfers, for every system. It is wider because the probe
    adds a poll interval and one read.
- **Capacity** at the 20% point needs both SLOs, plus the bounded backlog, which includes unsettled transfers. So the
  sustained rate is also a settled rate.
- **Thresholds against the gating baseline (PROPOSED):**
  - **PASS:** at least **2.0x** capacity at SLO at 0% transfers, **and** at least **1.0x** at 20% transfers;
  - **FAIL:** below **1.3x** at 0% transfers, **or** below **0.8x** at 20% transfers;
  - in between: **NARROW** (an advantage only for single-object, owner-local work).
- **Reported, not gating:** Zipf, RAM per object, CPU per operation, WAL bytes per operation, restart time after
  `SIGKILL`.
- **Why these numbers.** A new substrate needs a clear margin over production software. 2.0x is above every in-house
  figure, so the test can fail. The 20% point checks falsification targets 2 and 8 of
  `docs/EXPERIMENTAL_VALIDATION.md`: whether cross-object work erases the gain.

### 4.3 B2: durable messaging between entities

**Claim tested.** Many durable entities, each woken by an event to read, mutate and durably message another entity,
run better on owner-local state than on infrastructure built for durable work between entities.

**What B2 does not test.** SNC has no timers and no dormant activation (scope freeze), and it keeps every entity in
RAM. So B2 tests event-driven durable messaging, not general scheduling. RAM per entity is reported because it is
the price of that design.

- **Workload:** 1M entities, uniform and Zipf 0.99. Open-loop events: entity A debits itself and durably sends a
  credit to a random entity B on another shard, and B applies it exactly once. This is SNC's existing transfer saga
  (`docs/TRANSFERS.md`).
- **SNC:** `net_bench --transfer 100 --xfer-local 0`, with `--rate`, the settlement probe and the conservation check
  from 4.1.
- **Restate** (single node; server BSL 1.1, Rust SDK MIT):
  - one Virtual Object per entity; `transfer` updates K/V state and makes a one-way `send` of `credit` to B [EXT];
  - clients use **request-response** ingress calls, so the acknowledgement follows the handler's durable journal (a
    `/send` would acknowledge before the debit);
  - fsync on (the default);
  - the tuning budget covers: partition count (8 or 16), local or replicated loglet with replication 1, SDK
    concurrency, and retry backoff;
  - the server and the SDK service share the same 8 cores. One sensitivity run puts the SDK on spare node-1 cores.
- **A PostgreSQL transactional outbox.** The debit and an outbox row commit in one transaction, and a worker applies
  the credits. This is the industry-standard pattern, and it reuses B1's PostgreSQL.
- **Gating order:**
  1. Restate;
  2. the PostgreSQL outbox.

  Restate gates only if it passes the durability test of 4.1 with request-response ingress. Its reply-after-fsync
  for a completed invocation is source-verified only for the log writer (UNVERIFIED end to end), so the test
  decides. If it fails, Restate is diagnostic and the outbox gates. The outbox is the standard competent pattern for
  this job. If it gates, the PostgreSQL rule of 4.1 applies.
- **Settled means:**
  - SNC: the `emits_now` gauge reads 0 on every shard, and no account has outbox entries;
  - Restate: SQL introspection shows no pending invocations [EXT];
  - outbox: the outbox table is empty.
- **All three are sagas**, so they share one completion boundary: full settlement, measured by the probe.
- **Metrics:**
  - **capacity at SLO on full settlement** (decides): the probe's p99, with the settlement backlog bounded;
  - debit-acknowledgement latency (reported only);
  - settlement throughput: the drain rate after the load stops, and the settle lag (reported);
  - **recovery:** `SIGKILL` of every process of the system (Restate: server and SDK) at 80% of its own capacity,
    after 120 s of load. Measured: the time to serve again, and the time until everything settles. SNC's checkpoint
    interval is fixed in CRITERIA.md, because it sets the replay length. This also measures SNC's full-scan `resume`
    at 1M objects, which is unmeasured (`docs/EVIDENCE.md` R14);
  - RAM and disk per million entities.
- **Equivalence gaps:**

  | Gap | Favours | Used for decisions? |
  |---|---|---|
  | Restate's handlers run in a separate SDK process over HTTP/2, which is how Restate is deployed | **SNC** | yes, stated. One sensitivity run moves the SDK to spare cores |
  | Restate journals every invocation and retries arbitrary code | Restate, in function; SNC, in cost | yes, stated |
  | Acknowledgement points differ: SNC and the outbox acknowledge at the debit, ahead of Restate at the handler's end | SNC and the outbox, at acknowledgement | **no**: reported only; only full settlement decides |
  | Restate's durable probe read may be a journaled invocation | **SNC** | yes, if no cheaper durable read qualifies (4.1) |
  | SNC keeps every entity in RAM; Restate and PostgreSQL keep them on disk with a cache | **SNC** in latency, the others in density | RAM and disk per million entities are reported beside every result |

- **SLO (PROPOSED):** p99 full settlement at most 10 ms, the same for every system. Event-driven work between
  entities (workflows, agents) is judged at millisecond scale. A journaling engine cannot meet B1's 1 ms edge, and a
  test it cannot enter tells us nothing.
- **Thresholds against the gating baseline (PROPOSED):**
  - **PASS:** at least **3.0x** capacity at SLO, **and** time to settle after recovery no slower than the baseline's;
  - **FAIL:** below **1.5x**, **or** time to settle after recovery more than 2x slower;
  - in between: **NARROW**.
- **Why 3.0x.** Restate gives much more than SNC: arbitrary durable code, timers, retries, journaling and signals.
  SNC must show a larger margin to justify building the missing function.

### 4.4 Why exactly these two

- They cover the two approaches that can be tested without new engine features: owner-local execution (B1) and
  saga messaging between entities (B2).
- Each can fail on known weaknesses: margins over sockets near 1.2x, 0.87-0.91x on remote work, three records per
  saga transfer, the full-scan `resume`, and every entity in RAM.
- Neither needs replication or a change to SNC's engine.
- Workloads B (MMO-like) and C (dormant workflows) of `docs/EXPERIMENTAL_VALIDATION.md` are not run. B2 covers the
  testable part of C, and B needs a new driver that would only be worth writing after B1 passes.

## 5. Report structure: the five decision criteria

One report, `docs/DIRECTION_COMPARISON_REPORT.md`, with one section per criterion. Each section uses the same table:
the rows are evidence items, and the columns are the four directions (state-native substrate research, agent runtime, SDK, hybrid).

1. **What existing systems already solve.**
   - Track A: for each competitor arm, which of O-1 to O-5 pass at step 1 and at step 2, and the lines added, by
     language and class.
   - Track B: the function each baseline has that SNC lacks, from its docs.
2. **What SNC demonstrably improves.** Only measured differences against a threshold set before the run: B1, B2, and
   Track A's line counts and reported latencies. Everything else is listed as not shown.
3. **What SNC provides that competitors cannot readily reproduce.** Operational definition:
   - a guarantee a competitor arm cannot meet within L (both limits of 3.5) without changing its engine; or
   - a Track B PASS that no documented configuration of the baseline closes.
4. **Engineering effort for each direction.** An estimate in agent-days and cody-hours, labelled HYP. It is built from
   the measured gaps: competitors' added lines, SNC's code size, and the frozen items each direction still needs. For
   the full OS those include replication (on HOLD), timers, activation and security. This estimates effort only; it
   is not a decision about the original OS.
5. **Whether the benefits justify the complexity.** The section 6 rule applied to the results, with the operations
   rubric as the measure of complexity.

## 6. What the results can establish (fixed before any result)

Three questions stay separate, and this plan answers only the first two:
- **research continuation:** whether the tested state-native substrate approach deserves more research (Track B);
- **product direction:** whether SNC's agent layer is a runtime, an SDK, or neither (Track A, then the product gate
  of `docs/AGENT_RUNTIME_DECISION_PACKAGE.md` 7.2);
- **implementation authorization:** never given by any result here. Each next step needs cody's own decision.

**Scope of Track B's answer.** B1 and B2 test two things: owner-local object execution, and saga messaging between
entities, as built in SNC on one node.
- They do not test, and cannot validate or rule out, the rest of a complete operating system: replication,
  scheduling with timers, activation, memory beyond RAM, isolation, networking.
- A FAIL is a FAIL of this approach against these criteria. It is not a result about every possible state-native
  architecture.
- No result is a decision to abandon the original OS.

**Track B.** Rows are ordered, and the first row that matches wins. NARROW includes a PASS capped at NARROW (4.1).

| B1 | B2 | What it supports |
|---|---|---|
| PASS | PASS | **Continued state-native substrate research is justified.** It does not validate a full OS. A bounded next research phase is proposed separately for cody |
| PASS or NARROW | PASS or NARROW | **Continued research, narrowed** to the workload that passed or narrowed |
| PASS or NARROW | FAIL | As above, for B1's workload only |
| FAIL | PASS or NARROW | As above, for B2's workload only |
| FAIL | FAIL | **The tested substrate approach failed its criteria.** That is recorded as the experimental-validation STOP for this approach. All research and designs are kept, no wider conclusion is drawn, and any restart is cody's decision |
| any other result: either benchmark UNRESOLVED, invalid or not run, or no verified gating baseline | **No Track B conclusion.** What ran is reported as it is |

**Track A.** Rows are ordered, and the first row that matches wins. "Within L" means within both limits of 3.5.

| Result | What it supports |
|---|---|
| SNC's arm, as frozen in PR #185, fails a guarantee | Neither product direction, on this evidence |
| Any competitor arm, LangGraph included, meets O-1 to O-5 within L | **SDK** at most, over that engine. The report lists what SNC's layer adds beyond that arm, but the list does not change the row |
| DBOS, Restate and Temporal were **all** built, and each needs more than L or an engine change | **Agent runtime** remains a candidate, subject to the product gate |
| anything else (an arm not built, a criterion-3 result UNRESOLVED, a cap hit) | **UNRESOLVED.** What ran is reported |

**Separation of the tracks.** SNC's agent arm runs on DBOS and PostgreSQL, not on the state-native core. So a Track B
result never counts for or against a product direction, and a Track A result never counts for or against substrate
research. A **hybrid** product (an agent runtime or SDK on SNC's core) would need both a Track A "runtime" or "SDK"
row and a Track B continuation row, and then its own design and decision.

Every campaign is reported together. A later PASS never replaces an earlier FAIL in these tables without cody's
explicit decision.

## 7. Cost, hardware and stop rule

| Item | Agent effort [HYP] | Machine | cody's hands |
|---|---|---|---|
| Track B shared harness: conservation check, rejection counts, settlement probe, backlog counts, `blktrace` check, CRITERIA.md | 1.5 days | cloud VM (bring-up and regression only) | none |
| Durability verification (`dm-log-writes` replay, fio controls, every candidate configuration) for SNC, Garnet (both devices), PostgreSQL, Restate and the outbox | 2 days | cloud VM | none |
| B1 baselines: Garnet commands and transfer procedure (C#), a RESP client and a libpq pipelined client on the shared scheduler, a PL/pgSQL transfer | 2 days | cloud VM | none |
| B1 runs: tuning screens, 4 systems × 2 gated points × bisection plus 3 confirmations, and the Zipf point | 0.5 day | R640, one session of about 6 h | start it from a prompt card, relay the output |
| B2 baselines: Restate Rust Virtual Object, an HTTP/2 client on the shared scheduler, the PostgreSQL outbox and worker, the recovery script | 2-3 days | cloud VM | none |
| B2 runs | 0.5 day | R640, one session of about 5 h | as B1 |
| Track A: checker port to O-1 to O-5, the services' generic APIs, up to four competitor arms with review, the matrix | 8-10 days | one dedicated cloud VM (8 vCPU, 32 GB, local SSD), up to 8 h of matrix runs | none |
| Report and review | 2 days | none | read and decide |
| **Total** | **about 19-22 agent-days** | | **two R640 sessions, plus decisions** |

**Stop rule:**
- **Caps.**
  - Track B: 9 agent-days of harness and verification work, and two R640 sessions, plus one rerun session per benchmark only if a run
    was invalid.
  - Track A: 12 agent-days after P-A.
  - A cap hit stops the item. What completed is reported, and the rest is UNRESOLVED. There is no extension without
    cody.
- **Early stop in Track A.** Arms are built in the order DBOS, Restate, Temporal, LangGraph. The first competitor arm
  that meets O-1 to O-5 within L decides the Track A row ("SDK at most"). The remaining arms are then
  described from their docs, labelled UNVERIFIED, and not built. cody is told that their performance and operations
  are unmeasured.
- **No tuning SNC to win.** No SNC feature, fix or parameter change happens inside this plan. A loss with a visible,
  fixable bottleneck is recorded as a REFINE candidate. Any fix needs its own authorization, and its rerun is a new
  campaign against the same CRITERIA, reported beside the first.
- **Thresholds do not move.** CRITERIA.md is committed before the first comparative run. A later change is a new
  commit, reported next to the results.
- **Nothing is deleted.** Every SNC design, model and result stays in the repository, whatever the outcome.

## 8. Attempts to break this plan

Two independent reviewers (Opus) were told to break the first draft, one per track. They raised 32 findings, 5 of
them blocking. All are applied in this revision.

**Track A (15 findings, 3 blocking):**
- Asynchronous events:
  - the approver was a request and reply, so "event before wait" was unreachable for three arms;
  - it now posts through an ingress the arm chooses;
  - the idiom column is now a suggestion.
- **O-1 and O-2** used runtime-internal words ("acted on", "activation"). They now rest on deterministic causality
  from the stub reply id.
- **Section 6 rows overlapped**, and "two arms" could be met by a weak arm. The rows are now ordered, and the agent
  runtime row needs every durable-execution arm to exceed L.
- Other changes:
  - the early stop now matches the rule;
  - step 1 is frozen before runs, and the whole diff is gated;
  - the zombie takeover is specified per arm, with a time limit and UNRESOLVED;
  - T-F and the owner have generic APIs;
  - competitors run without the standby, which only SNC's design needs;
  - the latency gate was dropped: it scaled with SNC's own commit count, and it would have been a G3 measurement;
  - the engine restart moved to in-flight points;
  - out-of-scope cases are listed;
  - seeds are trimmed where nothing varies;
  - O-4 takes input bytes and covers denials;
  - the SNC adapter is specified.

**Track B (17 findings, 2 blocking):**
- **The correctness gate.** `net_bench --verify` does not cover transfers. A conservation check after drain is now
  specified as harness code.
- **Application rejections.** Rejections (no funds, account busy) counted as served capacity, which favoured SNC under
  Zipf. Balances now start high, and rejections are bounded at 0.1%.
- **"Every gap favours the baseline" was false.** The gaps are now listed with the side each favours. The ones that
  favour SNC are: acknowledgement at the debit, the binary protocol, the mature client, and Restate's SDK hop.
- Other changes:
  - Garnet uses single-key paths for reads and deposits;
  - durability is checked by counting device flushes, not by a kill;
  - the ROUTING.md over-TCP prior (1.23x durable) is added, with the predicted outcome stated, and
    `baseline_server` runs as calibration;
  - B2 is relabelled as messaging, not scheduling, and a PostgreSQL outbox is added;
  - Restate uses request-response ingress, and "settled" is defined on each side;
  - the budget is honest: bisection to 5%, three runs only at gated points, and 5-6 h sessions;
  - section 6 now covers every NARROW combination;
  - the clients use the shared scheduler;
  - locality is pinned;
  - unsafe settings are forbidden by name;
  - the SLOs are explained;
  - REFINE reruns are reported together;
  - the extra scale points were dropped.

### Revision 2

Codex reviewed revision 1 and held it (`docs/handoff/PR186_Codex_Review.md`). Its six items are applied:
1. **Track A arithmetic.** Revision 1 said 18 points and 110 runs; the true count is 17 points and 114 runs per arm
   (94 S-M, 20 S-AP). The table now has a runs column, and `DIRECTION_COMPARISON_TRACK_A_MATRIX.py` generates the
   matrix file `DIRECTION_COMPARISON_TRACK_A_MATRIX.csv` independently of the table.
2. **Equal durability** is verified through the device and storage stack with `dm-log-writes` replay, with positive
   and negative controls. Only a verified configuration can gate, and each benchmark has a fixed gating order (4.1).
3. **Completion boundaries.** Only full settlement (the credit durable) decides. Debit acknowledgement is reported
   only, and every gap table says which side it favours and whether it decides.
4. **Section 6** now answers research continuation and product direction separately, and never authorizes
   implementation. A FAIL covers only the tested approach.
5. **Rust and Python L.** One preregistered N/not-N rule with worked examples, two classifiers, and per-language
   limits on N lines and on total lines (3.5).
6. **Boundaries** are listed in section 0 and repeated in section 9.

Then two independent reviewers (Opus) were told to break revision 2:
- **Decision rules: 21 findings, 1 blocking**: the gated quantity was ambiguous (step 2 only, so step-1 code escaped
  the gate). The arithmetic was confirmed by hand and by script. Applied: the gate now counts N lines in the whole
  arm, plus a total-lines cap; N versus not-N with worked examples replaces four classes; disagreement makes a line
  not N, and low agreement makes the result UNRESOLVED; brace-only lines and the shared services are not counted;
  both section 6 tables end with a catch-all row; LangGraph can decide the SDK row; the SDK row no longer depends on
  a judgement; "thesis" wording is removed; and the boundaries are restated in sections 1 and 9.
- **Equivalence: 14 findings, 2 blocking**:
  - the replay rule treated FUA writes wrongly in both directions; the test now runs over a loop device with
    `fua = 0`, so a flush is the only durability point;
  - a virtual disk can drop flushes; fio controls must pass and fail as expected first.

  Applied as well: one PostgreSQL rule for both benchmarks, the probe's poll grid, pool and cost cap, the durable
  read for each system, backlog counted per system with one limit, the outbox worker in the tuning budget, Garnet's
  memory sizing, and two quiet rounds before the B1 correctness check.

The review is filed in `docs/handoff/PR186_Focused_Review_rev2.md`.

**UNRESOLVED:**
- Whether `replay-log` builds on the bring-up VM (4.1).
- The R640's `fua = 1` path differs from the VM's `fua = 0` path, and the data SSD's power-loss protection is
  unverified. Both are the same for every system, and neither is settled by this plan.
- Whether Garnet's `FileStream` device flushes, and whether its readers can see writes not yet in the AOF.
- Whether Restate acknowledges a completed invocation only after its log fsync, and whether it has a durable read
  cheaper than an exclusive handler.
- The PostgreSQL outbox worker's best knobs, found only inside the tuning budget.
- The 1.5 factor between Python and Rust limits, and the limits themselves: cody's choice, with no measured basis.
- The time per Track A run with real takeover timeouts. It is measured in bring-up.

## 9. What cody decides

1. **P-G1:** approve this plan.
2. **Set the PROPOSED numbers:**
   - **Track A:** 17 points and 114 runs per arm (the matrix file), the 10 s quiesce, T = 60 s, the limits of 3.5
     (N lines: Python 300, Rust 450; total lines: Python 1,000, Rust 1,500), the 90% classifier agreement, and the
     cloud VM;
   - **B1:** SLOs of 1 ms (reads, deposits) and 2 ms (probed full settlement); PASS at 2.0x at 0% and 1.0x at 20%
     transfers; FAIL below 1.3x or below 0.8x; the gating order Garnet Native, Garnet FileStream, PostgreSQL, with a
     PASS against PostgreSQL capped at NARROW;
   - **B2:** SLO 10 ms on probed full settlement; PASS at 3.0x and recovery no slower; FAIL below 1.5x or recovery
     more than 2x slower; the gating order Restate, PostgreSQL outbox, with a PASS against the outbox capped at NARROW;
   - **Shared:** 20 durability marks per configuration at about 25% capacity, a 1% probe share polled every SLO/20
     with probe reads capped at 1% of operations, one absolute backlog limit, rejections at most 0.1%,
     starting balances of 10^12, 12 configurations per baseline, and the caps in section 7.
3. **P-B:** start Track B, B1 then B2, and agree to two R640 sessions.
4. **P-A, later:** start Track A once PR #185's G2 gaps are closed and reviewed.

PR #185's G2 gaps remain open, and G3 stays on HOLD. Track B needs its own authorization (P-B), and Track A needs P-A.
None of these authorizes production dependencies, AR-1's G3, replication, raft-rs, custom replication, any change to
SNC's engine, or any decision about the original OS.
