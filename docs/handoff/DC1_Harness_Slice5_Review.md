# DC1 Track B harness slice 5: Garnet settlement probe, breaker review

PR #198, review of fba72a0 (independent breaker, Opus). Harness work only, not a comparative result.

## Findings and disposition

| # | Finding | Label | Disposition |
|---|---|---|---|
| 1 | No test covered the reply half of the Garnet rule: a mutant that drops it (`!(acked && seen)` → `!seen`) passed every scenario. | CONFIRMED | Fixed: `unacked` scenario, a RESP stand-in that applies transfers at once and replies 300 ms late. Real client: 143/143 miss. Mutant: 113/143, test fails. |
| 2-5 | Ack log, verify, drain, unanswered transfers, account reuse, per-connection reply order. | VERIFIED correct | none |
| 6 | `--no-load` on probe accounts already above the start balance: the read check passes early; only the reply rule and `--verify` guard it. | PLAUSIBLE | Recorded. Counted runs load fresh. |
| 7 | Transfers never issued before the drain limit are neither probed nor skipped. | PLAUSIBLE | Recorded. Such a run is undrained, so invalid. Same in net_bench. |
| 8 | Probe rate rounds to 1e-4 %; a bad `--probe-pct` string turns the probe off; the pool-size floor is not enforced by the client. | PLAUSIBLE, minor | Recorded. The runner sets these and checks the `probe:` line (reads vs B). |
| 9 | net_bench decides at the read reply, so an E_APP rejection arriving after the read counts as a miss instead of dropped. Conservative against SNC. | CONFIRMED by reading | Recorded as a separate net_bench slice, not fixed here. |

## UNRESOLVED

- With `--aof-commit-freq 40`, GETs on read-only connections waited about 22 ms behind commits (0.25 ms without writes). The counted config is freq 0; whether Garnet blocks reads behind commits there is not measured.
- Garnet's commit-wait covering `CustomTransactionProcedure` replies is inferred from the FileStream durability control (bench/results/dc1-trackb/GARNET_DURABILITY.md), not shown in Garnet's source.

## CI

The first run's `conserve` missed 19/410 probes at the default 100 ms grace (probe read max 276 ms on the GH VM). Those misses are correct under the rule; `conserve` now uses a 5 s grace so it tests accounting only.
