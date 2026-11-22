# DC1 Track B slice 4 (PR #193, Garnet baseline): breaker review and disposition

An independent adversarial review of 284e771 found no false pass and no BLOCKING issue. It checked the following:
- With strace delays on pwrite, SET, ACCT.DEP, ACCT.XFER and the NOFUNDS reply all wait for the AOF commit.
- FileStream opens the AOF O_SYNC, while Native and RandomAccess open it O_DIRECT and never sync. This matches the CI result in `bench/results/dc1-trackb/GARNET_DURABILITY.md`.
- Four SIGKILL-mid-run recoveries were exact.
- Truncating the AOF makes the check fail.
- The port opens only after replay.
- Keys and ack log are consistent.
- The CopyUpdater path is exercised under spill.
- The client sustains 300k ops/s against Garnet without an AOF.
- The AOF cost per transfer is about 117 bytes.

| # | Severity | Finding | Disposition |
|---|---|---|---|
| 1 | MAJOR | Reads shared connections with writes, so `--aof-commit-wait` held each read until its batch's commit. Read p99 equalled deposit p99, which is unfair to Garnet against plan 4.2. | Fixed. Each thread has separate read connections. Locally, read p50 is 57 µs against 336 µs for deposits. |
| 2 | MAJOR | `continue-on-error` let a broken FileStream job stay green. | Fixed. Each device now has an expected outcome: FileStream must pass, and Native and RandomAccess must lose writes with every negative control still rejected, which makes them a second negative control. Any other outcome fails the job. |
| 3 | MINOR | The tight-mark control could turn void on slow O_SYNC. | Unchanged. The first CI run had 19 non-void controls of 20 on FileStream, and void counts as not passed. |
| 4 | MINOR | `ACCT.XFER k k` created a cent. The source value was read after the second GET. Each op allocated a string to parse its amount. DEP on a missing key replies without an integer. | Fixed: same-account transfers are refused, each value is copied before the next GET, and amounts are parsed from bytes. DEP on a missing key stays as is: Garnet's NotFound reply has no integer, so resp_bench counts it as an error and leaves the op in doubt. |
| 5 | MINOR | Blocking sockets; verify summed INT64_MIN for unreadable accounts; a vector allocation per poll; unanswered creates logged as deposits. | Fixed. Sockets are non-blocking, unreadable accounts count as mismatches, the pollfd vector is reused, and unanswered creates are no longer logged. |
| 6 | MINOR | The negative control counted a crash (exit 1) as a rejection. | Fixed. A system that cannot start or be read now exits 2, which is never counted as a rejection. |
