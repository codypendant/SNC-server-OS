# SNC-AFR-001 Phase 1 working notes

Agent-produced evidence behind `docs/AFR_001_INVENTORY.md`. A claim here is only as strong as the file:line or
experiment it cites. All notes were written against `main` at `24006ee`.

| File | Author | Scope |
|---|---|---|
| `A_core.md` | reviewer A | object model, execution, persistence, runtime, memory |
| `B_distributed.md` | reviewer B | replication, HA, witnesses/Paxos, placement, migration, membership |
| `C_interfaces.md` | reviewer C | executables, CLI flags and exit codes, listeners, protocols, error model |
| `D_verification_docs.md` | reviewer D | tests, CI, fault injection, observability, governing-document map, ID collisions |
| `qa/QA1_REPORT.md`, `qa/qa1/` | QA 1 (not shown the reviewer notes) | persistence, sessions, ordering claims P1-P8, with experiment sources and outputs |
| `qa/QA2_REPORT.md` | QA 2 (not shown the reviewer notes) | network and HA claims N1-N9 |
| `exp_reviewer_a/` | reviewer A | WAL corruption (d2, d2b, d2c) and session reuse (d3) probes |
| `exp_reviewer_b/` | reviewer B | O1 probe |

Notes:
- Paths such as `exp/`, `recon/`, `qa1/`, `build/` inside the notes refer to the reviewers' scratch directory; the
  sources kept are in the folders above. The experiment programs link against a RelWithDebInfo build of
  `24006ee` (`libsnc_core.a`) and are not part of the CMake build.
- Reviewer A's header says the commit was unknown (it did not see `.git`); the author confirmed the checkout was
  `24006ee` (`git log -1`).
- Every experiment ran on loopback in scratch directories; no repository file was changed and every process was
  stopped. QA 2 sent forged state-changing messages only to its own scratch witness and servers.
