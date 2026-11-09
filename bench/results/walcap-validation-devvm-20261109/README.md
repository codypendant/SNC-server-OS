# WAL active-buffer capacity fix: validation (dev VM, 2026-11-09)

Order: cody's review of PR #83 (filed as docs/handoff/PR83_Design_Review.txt):
fix the `WalStream::ensure_capacity` u32 overflow first, as a small
standalone change.

Last code change: 59da52b. Later commits add evidence only. All runs are
serial (ctest -j1), 44 tests (43 plus test_wal_capacity), every run listed:

| Run | SHA | Result |
|---|---|---|
| release run1 | 2075ab9 | 44/44 |
| asan run1 (ASan+UBSan) | 316710e | 44/44, zero sanitizer reports |
| tsan run1 | b93c219 | 44/44, zero ThreadSanitizer reports |

## Negative mutations (negative/)

| Attempt | Base | Result |
|---|---|---|
| attempt1 | 0e6712a | 5 of 5 caught; but M1 (`if (false)`) failed C3 through heap corruption (capacity 0), not unbounded growth (independent review) |
| attempt2 | 59da52b | 5 of 5 caught. M1 now removes the limit: growth succeeds and C3 fails on the missing fail-stop. M2: C3 (u32 record size). M3: C4 (append_raw unchecked). M4: C1 (sum before check, near 2^64). M5: C5 (no open check) |

Logs have the scratch build path replaced by `<src>`. Death tests that a
mutation makes fail leave their temp dirs; they were removed by name.

## Independent review

Read-only review of 0e6712a, which also ran test_wal_capacity in Release,
ASan+UBSan and TSan builds:
- no HIGH findings;
- `grown_capacity` is overflow-free for every u64 input;
- the fail-stop comes before any state change;
- every other u32 size computation stays far below 2^31 at the 1 GiB hard
  maximum;
- WalConfig is built in one place, so the default applies everywhere.

Fixed in 59da52b:
- a capacity above max is now clamped;
- one comment's bound is corrected;
- `<cstdlib>` is included;
- negative M1 is fixed.

Known limit, documented in include/snc/storage.h: a standby has no
receive-side bound (replication.cpp `Receiver::poll`). A standby whose
fsync falls far behind a fast link now fails stop at 1 GiB, where before
it hung or corrupted memory at 2 GiB. A standby receive budget is
separate work.

Separate finding, not changed: `check_record` compares a u32
`record_size(payload_len)`, which wraps for a payload near 4 GiB. A
crafted record with a valid CRC passes the check and then aborts at
install (payload > max_size). The primary can no longer produce one.
