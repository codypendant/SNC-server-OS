# D3: a cluster member's shard count comes from its group (2026-11-15)

Status: **for review.** Assigned by cody's review of PR #134 and #135
(`docs/handoff/PR135_D2a_Default_Shards_Review_Approval.md`), Priority 1. It is the first of D3, D4 and D5 from
`docs/CPU_PLACEMENT_R1.md` section 7.

## Summary

- **Defect.** In `--cluster` and `--control` mode, each group's shard count is recorded in the `--cluster` list
  or in the placement map that `snc_ctl init` wrote. `Engine` requires the node to run exactly that many
  shards. But `snc_server` chose its shard count without looking at it:
  - With no `--shards` it used the D2-a default: one shard per CPU it may run on (before D2-a, per online CPU).
  - Whenever that differed from the group's count, the node **aborted** (`std::terminate`, SIGABRT) with
    `cluster: group_id or member out of range, or the group's shard count is not runtime.n_shards`.
  - A `--shards` or `--cpus` that disagreed aborted the same way.
  - **Observed** on the code before this change: a member of a 2-shard group started under 1 CPU, or
    unrestricted on this 4-CPU host.
- **Second gap, same contract.** A data directory holding another shard count than the group's was
  **remapped silently** to the group's count when started as a cluster member. **Observed:** a 3-shard
  standalone directory started as a member of a 2-shard group logged `recovery: checkpoint before serving
  (shard count changed)`.
- **Fix.**
  - A member with neither `--shards` nor `--cpus` takes its group's count.
  - A `--shards` or `--cpus` that gives another count is refused before anything starts, with exit 2 and a
    message naming both counts.
  - A `--group` or `--member` the list or map does not have is refused the same way.
  - In cluster mode `Engine::recover` refuses a data directory whose shard count is not the group's, before it
    writes anything to it (the role file included). It checks again after a seed, for a directory seeded from
    a peer. `snc_server` reports either as `not started`, exit 1.
  - In `--control` mode, `control::resolve` has already written the directory's `node` and `placement` files
    (creating the directory if needed) before any of these refusals. That order is unchanged by D3; see section 8.
- **Not changed:**
  - Standalone nodes, including D2-a's default.
  - Any member that already passes a matching `--shards`, which every launcher in the repository does.
  - The placement map and the `--cluster` format.
  - The on-disk format.

## 1. Dependency order of D3, D4 and D5

| | What | Depends on |
|---|---|---|
| D3 | A member's shard count must agree with its group's | Nothing. Its refusals are checked in `snc_server` before `Engine` is built. They exit 2 like the other configuration errors (the checkpoint-walk check does the same), so D3 does not need D4. |
| D4 | A refusal thrown by `Engine`'s constructor (or by `parse_cluster` while flags are read) aborts `snc_server` instead of exiting with the reason | Nothing. After D3, the shard-count case no longer reaches the constructor's check. That check stays as a backstop for embedders, and the constructor's other refusals still abort. |
| D5 | A primary/standby shard-count mismatch is refused quietly | Nothing in D3 or D4: it is in the replication handshake. D3 narrows it: two `--control` members of one group now run the map's count. D5 stays reachable in a cluster, though. Two `--cluster` lists can give one group different counts, since each node parses its own list and node links compare map shapes only between groups. A standalone standby can serve a cluster primary. And a map whose count was changed (section 8) can meet a fresh or seeded directory. D3 also constrains D5's options: the "seeded standby takes the seed's count" idea must not apply in cluster mode, where the group's count is authoritative. |

So D3 comes first, as cody proposed. D4 and D5 do not depend on it or on each other. D4 is the smaller of
the two and makes every other start-time refusal readable, so the proposed order is **D3, then D4, then D5**.

## 2. Root cause and the contract

The contract is that the placement map (or `--cluster`) is authoritative for a group's shard count
(`docs/CLUSTER.md`; `Placement` groups carry `n_shards`, `src/core/placement.cpp`). Every member must run that
count: node links check each shard against the map (`src/core/node_link.cpp`), and `Engine::Engine` throws
otherwise (`src/core/engine.cpp`).

`tools/snc_server.cpp` set `runtime.n_shards` right after reading its flags. That was before the `--control`
block fetched the map, and it never consulted `cfg.cluster`. Its only check was the engine's throw. That
throw happens in `Engine`'s constructor, which `snc_server` calls outside the `try` that turns refused starts
into exit 1. That is D4, not fixed here.

`Engine::recover` remaps any directory whose manifest has another shard count (`slots_ =
SlotMap::make_default(n, ...)`, `recovery_.remapped`). That is right for a standalone node, and silent for a
cluster member.

## 3. The change

| Where | What |
|---|---|
| `tools/snc_server.cpp` | The shard count is resolved after the `--control` block. In cluster mode the group's count is used when neither `--shards` nor `--cpus` is given. Any other count is refused with `snc_server: group G has N shards in --cluster` (or `in the placement map`) `, but --shards gives M; pass --shards N or leave it out`, exit 2. An out-of-range `--group` is refused with exit 2. D2-a's default (the process's CPU set) then applies to standalone nodes only, unchanged. |
| `tools/snc_server.cpp` (QA) | `--member` out of range is refused with exit 2 too. A `--cpus` mismatch says `list N CPUs or add --shards N`. |
| `src/core/engine.cpp` | `recover`: in cluster mode, before the role file is written, an existing directory whose manifest has another shard count throws `cluster: data directory D holds M shards, but its group has N; refusing to remap a cluster member's state`. Step 3 repeats the check for a directory just seeded from a peer: a seed lands on an empty directory only, so nothing that was there is lost, and the seeded directory stays refused on every start. |
| `docs/CLUSTER.md` | Says the member takes its group's count. |
| `tests/test_default_shards.py` | Reads the pipe without Python's line buffer (see section 5). No behavior checked changes. |

C++ functionality is not expanded (SNC-ADR-RUST-001). The change removes an abort and a silent remap, and adds
no flag and no mode.

## 4. Regression test

`tests/test_cluster_shards.py` is deterministic and takes about 1 s. It uses real `snc_server` processes, and
for K5 and K6, three `snc_witness` processes and a placement map written by `snc_ctl init`.

| Case | Before (d605cdf) | After |
|---|---|---|
| K1 `--cluster` 2-shard group, no `--shards`, under 1 CPU and unrestricted | **aborts** (`terminate called ... shard count is not runtime.n_shards`) | `shards=2` both times |
| K2 `--shards 3` against the 2-shard group | aborts | exit 2, `group 0 has 2 shards in --cluster, but --shards gives 3` |
| K3 `--cpus` of 1 CPU against the 2-shard group | aborts | exit 2, `--cpus gives 1` |
| K4 `--group 5`, or `--member 9`, with one group of one member | aborts (constructor) | exit 2 |
| K5 `--control`, no `--shards`, under 1 CPU, then restarted from its directory under 3 CPUs | aborts | `shards=2`, and the restart keeps 2 with no remap |
| K6 `--control` with `--shards 3` | aborts | exit 2, `in the placement map` |
| K7 a 3-shard directory started as a 2-shard group's member, as a primary and as a standby, then restarted standalone at 3 | silently remapped to 2 (observed by hand) | exit 1, `refusing to remap`, both times; every file's bytes, mode and mtime are unchanged (a listing compared before and after); the standalone restart shows no remap |

Every case also asserts that no process aborted. The run on the old binary stopped at K1, as shown. QA 1
disabled the engine guard alone and K7 failed (remapped). The first version of the guard sat after the role-file
write: K7's standby variant fails on it (`the refused standby start changed the directory`). K5's restart runs
under 3 CPUs, because under 2 the D2-a default would equal the group's count by chance; with fewer than 3 CPUs it
is skipped, saying so. Only K5's first start then discriminates.

## 5. A test-harness defect found on the way

While I wrote K5, its startup line went missing. The cause was in the harness, not the server:
`readline()` on the pipe's text wrapper reads a whole chunk. The lines after the first stayed in Python's
buffer, so `select()` reported nothing more, and `communicate()` then read from the file descriptor, past
them. `tests/test_default_shards.py` (D2-a) has the same pattern. It passed only because its startup line is
the first line of its chunk. Both tests now read the descriptor directly and split lines themselves.

## 6. Tests run

On this host (4 CPUs), after rebasing on PR #135 merged with main (K0, PR #134): the fast suite (`ctest -LE
slow`), and `test_control`, `test_xnode_proc`, `test_fail_closed`, `test_baseline_move`, `test_persistence`
(standalone remap, unchanged), `test_migrate`, `test_witness` and `test_health`. Counts are in the PR, with
the full suite run by CI on the PR's head.

## 7. Three-way QA

Three independent reviews, each with the diff and this write-up, none anchored on the others.

| Finding | Resolution |
|---|---|
| The engine refusal came after `write_role`: a refused `--standby` start changed the directory | The check now runs before anything is written; K7 asserts every file is unchanged, as a primary and as a standby. Shown failing on the earlier placement. |
| `--member` out of range still reached the constructor and aborted | Refused with exit 2 (K4). |
| The `--cpus` mismatch message did not say how to fix a `--cpus` list | The message says to list N CPUs or add `--shards N` (K3). |
| The write-up said D5 could no longer arise in a cluster | Corrected: section 1 lists the cluster paths that remain. |
| D3 rules out one of D5's options (seeded standby takes the seed's count) in cluster mode | Recorded in section 1. |
| `--control` writes `node` and `placement` before the refusal; unvalidated `PLACEMENT CAS`; no-checkpoint directory; `--shards 0` | Pre-existing; recorded in section 8, not changed here. |
| K5's restart under 2 CPUs passed on the old code by chance | It runs under 3 CPUs, or is skipped saying so. |
| Disabling the engine guard alone: does any test notice? | K7 fails (directory remapped). |

QA 3 also ran an end-to-end check: a primary and a standby of one `--control` group under different
`taskset` sets, both without `--shards`. Both ran the map's count, replication and a failover worked,
`snc_load` verified the data, restarts did not remap, and an empty or seeded standby directory was not
refused. No reviewer found a blocker.

## 8. Remaining risks and downstream implications

- **`--control` writes before the shard-count check.** `control::resolve` creates the directory if needed and
  writes `node` (if absent) and `placement` (when fetched) before `snc_server` checks `--shards` and before
  `Engine` starts. A refused start therefore leaves those files, and a new directory is bound to that NodeID.
  This order predates D3. Moving the writes after the check would change `resolve`, which K0 and K1's
  control-connection work also touch, so it is left for review.
- **A group's shard count is not protected in the map itself.** The witnesses' `PLACEMENT CAS` stores any
  body without decoding it (`tools/snc_witness.cpp`). No tool changes a group's `n_shards`, but nothing
  forbids it. A running node adopts only slots from a newer map and keeps its groups' shapes
  (`Engine::update_placement`, the start barrier). A restarting node takes the new count, and D3 then refuses
  its directory: fail-closed, not consistent.
- **A directory with a superblock but no checkpoint yet** (a crash before the first start's recovery
  checkpoint) has no manifest to compare. Such a directory has served nothing.
- **`--shards 0` or a non-number** reads as "not given" (`atoi`), so a member takes its group's count. This
  is as before for standalone nodes.
- **Operators** must now get a clean refusal where they used to get an abort. A deployment that relied on the
  default and happened to match (the host's CPU count equal to the group's) keeps working. One that passes
  `--shards` keeps working whenever its count matches, as before.
- **A cluster member's directory** made with another count is now refused rather than converted. If a
  conversion is ever wanted, it should be an explicit operator step, not a side effect of a start. None
  exists today.
- **D4 remains:** other refusals from the `Engine` constructor, and a malformed `--cluster`, still abort.
- **D5 remains:** a primary/standby shard-count mismatch is still refused quietly, standalone, and in the
  cluster cases section 1 lists.
- **The constructor's own shard-count check** in `Engine` is kept as a backstop for embedders (tests build
  engines directly).
