# PR #137 review decision (Codex, relayed by cody, 2026-11-16 00:19 UTC)

Posted by cody in the project chat as a screenshot of Codex's review; the part about PR #137 transcribed
verbatim.

> | PR | Decision | Reason |
> |---|---|---|
> | #137 — Startup error handling | Conditional approval | The code changes and regression coverage look sound, but an intermittent CI failover timeout remains under investigation. |
>
> For PR #137, Claude reported that `test_xnode_proc` scenario x7 timed out during promotion in one CI run.
> The same commit passed another CI run and five local repetitions. That suggests an intermittent issue,
> but doesn't establish its root cause.
>
> **My decision:** Merge #137 once the CI investigation is resolved and required checks pass. If x7 fails
> again, investigate before merging.
>
> After these merges, I recommend prioritizing the architectural guarantees review before authorizing
> additional major implementation work.

cody also set a standing rule the same evening: subagents run on Opus or Sonnet, never Fable.

## The CI investigation (x7)

**What failed.** Run 43778062217 (manual dispatch, 3529f45), fast job: `test_xnode_proc` x7 raised
`timed out: the witnesses promoted node 4` after 60 s. The PR-triggered run of the same commit
(43778063022) passed the fast suite, x7 included. The kept directory was on the CI runner and is gone, so
the node and witness logs of that run are not available.

**What x7 does** (`tests/test_xnode_proc.py`): two HA groups with witnesses, each a primary and an async
standby started with `--replica-timeout-ms 300`. It waits until both groups are READY, starts a client load,
waits 3 s, kills group 1's primary (node 3), and waits up to 60 s for the witnesses to promote node 4.

**Mechanism, reproduced deterministically** (`tests/probe_x7_not_ready.py`, same cluster as x7):

| Probe | Witness state at the kill (version, term, NOT READY, primary) | Outcome |
|---|---|---|
| No stall, kill 2 s after the load starts | `(3, 0, False, 0)` | node 4 promoted after 0.6 s |
| Node 4 stopped for 1 s under load, kill 2 s after it resumes | `(5, 0, False, 0)` | promoted after 0.6 s; node 3 logged `granted, continuing without the standby`, then `standby back in sync (READY after v=4)` |
| Node 4 stopped for 1 s under load, kill right after it resumes (twice) | `(4, 0, True, 0)` | **not promoted in 20 s**; the state stays NOT READY |

When a standby's acks are late by more than the replica timeout (300 ms here), its primary asks the
witnesses to DEGRADE. Their grant puts NOT READY on record (`docs/SYNC_READINESS.md`). Until the primary
proves its standby back in sync (READY), the witnesses refuse to promote that standby, by design: a standby
that may lack acknowledged writes must not take over. If the primary dies inside that window, nothing
clears the NOT READY, and the group stays without a primary. That is "unavailable for failover, never
unsafe" (`docs/SYNC_READINESS.md`, liveness under load).

x7 checks READY only before the load starts. A shared CI runner that delays the standby's acks by 300 ms
during the 3 s of load would degrade the group, and a kill before the standby is back in sync gives exactly
the failure seen: no promotion within 60 s. Locally, x7 passed 5 of 5 alone and 10 of 10 next to four
CPU-bound processes, so it takes a stall longer than CPU contention alone produces here.

**Conclusion.**
- **Root cause, by mechanism:** a race in the test. x7 kills the primary at a fixed time without checking
  that the group is still READY, so on a slow host it can kill it while NOT READY is on record, and then the
  correct outcome is no promotion. Without the CI run's logs this is the mechanism shown to produce the
  symptom, not a trace of that run. No other path found keeps the witnesses from promoting for 60 s with
  every witness and node 4 alive.
- **Not PR #137's:** #137 changes only `snc_server`'s startup (flag parsing and building the engine, before
  `start()`). In x7, node 4 had started and its group was READY before the load. The DEGRADE, READY and
  promotion paths are untouched (`git diff 63cf6d7 3529f45 -- src tools` changes only the start of
  `tools/snc_server.cpp`'s `main`).
- **Proposed fix, its own slice (not started):** before the kill, x7 waits for the group to be READY and
  reads the witnesses' state just after the kill. If NOT READY was on record at the kill, it reports that
  case by name rather than as a generic timeout, and retries the kill once. The other HA kill tests
  (`x7b`, `test_fail_closed`, `test_health`) can be checked the same way. Also proposed: CI uploads the
  kept `/tmp/snc-*` directories of a failed test as an artifact, so the next intermittent failure has logs.

## How the decision is carried out

- PR #137 merges once the full CI suite (both jobs) passes on its final head, which includes this file
  and the probe. If x7 fails again, it is investigated before any merge.
- After #137: no D5, D4-b, D4-c, x7 test fix or other implementation work until cody says go. Codex
  recommends the architectural guarantees review first.
