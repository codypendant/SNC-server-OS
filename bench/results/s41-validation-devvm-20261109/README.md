# S4.1 validation (dev VM, 2026-11-09)

Scope: docs/handoff/PR81_S4_Review.txt section 11. S4.1 changes the admin
reply path (tools/admin_loop.h), HEALTH's buffer and counters
(tools/snc_server.cpp), adds tests/test_admin_loop.cpp, and rewords the
token fairness contract (docs, comments, T3 output). No engine, token,
format, recovery or lifecycle code changed.

Last code change: 617a080. Later commits add evidence only, so every run
below tests the same code. All runs are serial (ctest -j1), 43 tests
(42 from S4 plus test_admin_loop). Every run is listed, in order
(runs.txt has the start/end lines).

| Run | SHA | Result |
|---|---|---|
| release run1 | ef39687 | 43/43 |
| asan run1 (ASan+UBSan) | 9577aa9 | INVALID: stopped by the session's 30 min background-task limit during test 37; the 36 finished tests passed; not evidence |
| asan run2 (ASan+UBSan) | d2a2592 | 43/43, zero sanitizer reports |
| tsan run1 | 9236af6 | 43/43, zero ThreadSanitizer reports |

S4 T1-T9 (test_s4_sync_tokens) passed in each complete run.

## Negative mutations (negative/)

| Attempt | Base | Result |
|---|---|---|
| attempt1 | 92f2b5f | 4 of 4 caught, but N7 (one-call send) and N8 (blocking write) only via a wait on the loop's `out` counter, not the received content |
| attempt2 | 617a080 | 4 of 4 caught; N7 fails on the received bytes, N8 on HEALTH's 3 s receive timeout (negative/attempt2/NOTES.txt) |

## Independent review

A read-only reviewer built the patch (RelWithDebInfo, ASan+UBSan, TSan)
and checked: HEALTH cannot be held by a slow GAUGES reader (send_some never
blocks; HEALTH 0.2-0.3 ms beside a stalled 16 MiB reply); a cut reply has
no newline, so GAUGES cannot be cut silently; the fairness docs match the
allocator. It found A1 failing under TSan 2 of 3 times (A1 shared A2's
400 ms deadline), plus four small items (HEALTH buffer clamp, out_max 0,
a stale comment, deadline wording). All were fixed in 617a080 before the
runs above; docs/S4_SYNC_TOKENS.md section 9 records them.
