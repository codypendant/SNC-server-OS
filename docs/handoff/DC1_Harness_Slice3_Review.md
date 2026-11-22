# DC1 Track B slice 3 (PR #190): breaker review and disposition

Independent adversarial review of 88e674e, told to break the change. No false pass found (clocks, ack-time direction, creates, zipf, probes, outstanding slots, amounts, E_REPLAYED_OLD checked).

The first CI run on a GitHub VM also failed 9 of 20 marks with off-by-one balances, which matched finding 2.

| # | Severity | Finding | Disposition |
|---|---|---|---|
| 1 | BLOCKING | The check read balances before cross-shard credits were re-delivered after recovery. Reproduced with SIGKILL: short by 1, and a second read was exact. | Fixed. The check re-reads until two reads agree and hold, or nothing moves for 5 s (cap 60 s). New test `kill` uses SIGKILL mid-run with the mark around the kill. |
| 2 | MAJOR | The mark time was taken before `dmsetup message`. Ops sent in the gap can be durable at the cut, so they were wrongly treated as "cannot be there". | Fixed. Times are taken on both sides (`DLW_MARK_NS`, `DLW_MARK_END_NS`). "Acked before" uses the earlier one and "sent before" the later one. |
| 3 | MINOR | Bounds were per account only, so a lost op could hide in another op's in-doubt allowance. Trivial marks also passed. | Added a whole-system sum check (start × existing accounts + deposits; transfers conserve). Each mark reports `must` (accounts that must exist). No minimum is enforced. |
| 4 | MINOR | No timeouts. The work dir was deleted on failure. `--check` used str.split. The snc job did not depend on controls. | Fixed. 600 s timeouts. The work dir is kept on failure, and CI uploads the ack log and server log. shlex.split. `needs: controls`. |
| 5 | MINOR | The `acks` test stopped the server cleanly. snc.py duplicates the wire header layout. | `kill` covers the unclean case. The duplicate header is kept on purpose: an independent reader is part of the check. |

Unresolved, as reported:
- How long re-delivery takes after real cache loss.
- Whether E_STALE or E_OVERLOADED can come back for an applied op. Both are logged as not applied.
- Session resends, which this configuration does not use.
