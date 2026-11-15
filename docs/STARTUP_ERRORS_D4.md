# D4: startup refusals exit with their reason, never abort (2026-11-15)

Status: **for review.** Assigned by cody's PR #136 review
(`docs/handoff/PR136_D3_Cluster_Shards_Review_Approval.md`): "D4: controlled startup errors". D4 was first
recorded in `docs/CPU_PLACEMENT_R1.md` section 7; the dependency check is in `docs/SHARD_COUNT_D3.md`
section 1 (D4 needs neither D3 nor D5).

## Summary

- **Defect.** When `snc_server` was given a configuration that a helper or the `Engine` constructor refuses
  by throwing, the exception escaped `main`. The process died by `std::terminate` (SIGABRT, exit 134, a core
  dump where enabled) with `terminate called after throwing an instance of 'std::invalid_argument'`, instead
  of exiting with its reason like every other refused configuration.
- **Observed** on the code before this change (88c2f15, PR #136's head), each one SIGABRT:
  - `--cluster garbage`, and a `--cluster` group whose members give different shard counts
    (`parse_cluster`, while flags are read);
  - `SNC_CKPT_IO=bad`, `SNC_CKPT_SYNC_TOKENS=999`, `SNC_FOLLOWUP_INTAKE=maybe`, `SNC_XNODE_ACCOUNTING=x`
    (validated in `Engine::Engine`);
  - node-link timers the engine refuses (`--xnode-hb-ms 100000` in cluster mode);
  - a 32-character `--space-uuid` that is not hex (`std::stoul` in `parse_uuid`);
  - a shard count above 256: `--shards 300`, or no `--shards` with a `--cpus` list (or a host) of more than
    256 CPUs. This one is not a throw: `NativeRuntime`'s constructor calls `std::abort()` (QA 1).
- **Found on the way, same flags:**
  - `parse_uuid` also **accepted** some non-hex input. `std::stoul(.., 16)` reads `"0x"` as 0 and accepts
    whitespace or `+`. With `--space-uuid 0x0x...0x` the UUID came out all zero, which the engine treats as
    "not given", so the flag was silently ignored and the node minted a random space UUID (QA 2 observed
    `b317aa1b...`); with `+1+1...` or ` 1 1...` it got `0101...01`.
  - `--shards 65536` wrapped through `u16` to 0, read as "not given", and the node started with the
    default count.
- **Fix.**
  - `--cluster`: a parse error exits 2 with `snc_server: --cluster refused: <reason>`.
  - The `Engine` is built inside a `try`. `std::invalid_argument` (a refused configuration) exits 2 with
    `snc_server: refused: <reason>`. Any other exception from the constructor exits 1 with
    `snc_server: not started: <reason>`, as a refused `start()` already does.
  - `parse_uuid` requires 32 hex digits (dashes still ignored), so a bad value gets the existing
    `--space-uuid takes 32 hex digits` and exit 2.
  - `--shards` above 256 exits 2 (`--shards takes 1 to 256`), and so does a default count above 256
    (`N CPUs would give N shards; at most 256, so pass --shards`). `--shards 0` and a non-number still
    read as "not given", as before.
- **Not changed:** every accepted configuration, the engine, the on-disk format, the wire, and what happens
  after the node starts serving. Exit codes keep their meaning: 2 for a refused configuration, 1 for a
  refused start.

## 1. Root cause and contract

The contract `snc_server` keeps elsewhere: a configuration it refuses exits 2 with a message (flag checks,
the checkpoint-walk check, D3's shard-count checks), and a start the engine refuses exits 1 with
`snc_server: not started:` (the `try` around `eng.start()`). Launchers and tests rely on that. They tell a
refusal from a crash, and the reason is on stderr.

Three throw sites were outside every handler in `main`:

| Site | Throws | Before |
|---|---|---|
| `parse_cluster` (`src/core/cluster.cpp`), called in the flag loop | `std::invalid_argument` for a malformed list | abort |
| `Engine::Engine` (`src/core/engine.cpp`), called as `Engine eng(cfg);` just before the `try` around `start()` | `std::invalid_argument` for environment overrides, timers, follow-up and checkpoint limits, the cluster shape (`ClusterMap::make`/`assign` included); `std::bad_alloc` | abort |
| `parse_uuid` (`tools/snc_server.cpp`) | `std::invalid_argument` from `std::stoul` | abort |

Everything else that `main` calls before `start()` either returns an error code or runs inside `start()`
(the `--adopt-role primary` checks, for example, are callbacks run from `start()`). QA 1 and QA 2 each
enumerated the calls independently (`control::resolve`/`apply`, `Placement::decode`, `ha::ask`, the numeric
flags) and found no other throw.

One abort that is not a throw sits on the same path and is reachable by a flag: `NativeRuntime`'s
constructor (`src/native/runtime.cpp`) calls `std::abort()` when `n_shards` is above `kMaxShards` (256).
`snc_server` now refuses that count itself before building the engine.

## 2. The change

| Where | What |
|---|---|
| `tools/snc_server.cpp`, `--cluster` | `parse_cluster` in a `try`; `std::invalid_argument` exits 2 with `snc_server: --cluster refused: <reason>`. |
| `tools/snc_server.cpp`, before `start()` | `std::optional<Engine>` emplaced in a `try`; `Engine& eng = *eng_holder` afterwards, so every later use, the lifetime and the destruction order are the same as the old local. `std::invalid_argument` exits 2 (`refused:`), any other `std::exception` exits 1 (`not started:`). stdout is flushed first, as on the existing path. |
| `tools/snc_server.cpp`, `parse_uuid` | Every character must be a hex digit (`std::isxdigit`) before `std::stoul`. |
| `tools/snc_server.cpp`, `--shards` and the default count | `--shards` is read with `strtoull` and refused above 256 (no `u16` wrap); a default count (from `--cpus` or the CPU set) above 256 is refused, exit 2. |
| `CMakeLists.txt`, `tests/test_startup_errors.py` | The regression test. |

No thread is running when the constructor can throw (the `NetServer` is constructed, its threads are not),
so returning from `main` there runs ordinary destructors. The engine's constructor writes nothing to disk;
`recover` runs in `start()`.

C++ functionality is not expanded (SNC-ADR-RUST-001): no new flag, mode or behavior for an accepted
configuration; three abort paths become the exits the rest of `snc_server` already uses.

## 3. Regression test

`tests/test_startup_errors.py`, deterministic, about 1 s, real `snc_server` processes.

| Case | Before (88c2f15) | After |
|---|---|---|
| E1 `--cluster garbage`; a group `A:2\|B:3` | abort | exit 2, `snc_server: --cluster refused: <reason>` |
| E2 `SNC_CKPT_IO=bad`, `SNC_CKPT_SYNC_TOKENS=999`, `SNC_FOLLOWUP_INTAKE=maybe`, `SNC_XNODE_ACCOUNTING=x` | abort (exit 134), each | exit 2, `snc_server: refused: <the engine's reason>` |
| E3 `--xnode-hb-ms 100000` in cluster mode | abort | exit 2, `refused: node-link timers ...` with the four values |
| E4 `--space-uuid` of `z`x32, `0x`x16, ` 1`x16, `+1`x16 | abort (`z`); **started**, with a random UUID (`0x`) or `0101...` (` 1`, `+1`) | exit 2, `--space-uuid takes 32 hex digits` |
| E5 an existing 2-shard data directory started with `SNC_CKPT_IO=bad` | abort | exit 2; every file's bytes, mode and mtime unchanged; the next good start recovers with no remap |
| E6 `--shards 300`; `--shards 65536`; `--cpus` of 257 CPUs | abort (`300`, `--cpus`); **started** with the default count (`65536`) | exit 2 with the limit |

Not tested: the exit 1 path for an exception other than `std::invalid_argument` from the constructor
(only `std::bad_alloc` can reach it; no flag triggers it).

Every case asserts that no process aborted. On the old binary the test stops at E1 (`aborted`); each E2 to
E6 case was also run by hand on it, by me and by QA 2, with the results in the table.

E5 does not tell the old binary from the new by the directory: the old one aborted before writing anything
too, so it fails E5 only through the abort check (QA 3). E5 is there to keep a refused start from touching
the directory in future changes. E5 covers a plain node. In `--control` mode `control::resolve` writes the directory's `node` and
`placement` files, and `--rejoin` renames a directory aside, before the engine is built. That is unchanged
by D4 (see `docs/SHARD_COUNT_D3.md` section 8). QA 3 measured both on the new binary: with `--control` a
refused start rewrites `placement` with the same bytes and a new mtime; with `--rejoin`, when the peer is
primary of the same space, the directory is first moved aside to `<dir>.stale-<time>`, then the start is
refused (exit 2 now, SIGABRT before). No data is lost, since the next start seeds from the peer, but the
order is wrong: see section 4.

## 4. Remaining abort paths at startup (not changed; proposed)

- **The client listener (D4-b, proposed).** A port already in use, a `--host` that is not an IPv4 literal
  (`--host localhost`: `inet_pton: No such file or directory`, QA 2), or any `socket`/`bind`/`listen`/
  `epoll` failure while the listener is set up aborts: `net::ConnSet`'s constructor calls `die()`, which is
  `perror` + `std::abort()` (`src/net/conn_set.cpp`). Observed: exit -6 with `bind: Address already in
  use`. The listener is built before the engine, so a refused configuration on a busy port also aborts
  there first (QA 2 observed it with `SNC_CKPT_IO=bad`); D4's exits hold when the ports are free. These
  are startup errors an operator meets, but the abort is deliberate in the network library, which the
  client tools also link. Changing it to an exception changes that library's contract, so it is proposed
  as its own slice, not folded in here.
- **Type registration errors** (`src/core/registry.cpp` `die`) abort by design: they are programming errors
  in a build, not configuration.
- **Numeric flags are read with `atoi`/`atof`**, so garbage reads as 0 (`--cpus 0,abc` pins to CPU 0 twice,
  `--cpus 9999` starts unpinned, `--shards abc` reads as "not given"). Those start rather than abort; they
  are input validation, outside D4.
- **Configuration checked after side effects (D4-c, proposed).** The engine validates its configuration
  only when it is built, after `--control` has written `placement` and `node` and after `--rejoin` has moved
  the directory aside. A static check of the configuration (the constructor's validation, callable on its
  own) run before those steps would refuse first. That moves code out of the engine's constructor, so it is
  proposed separately rather than folded into D4.
- **`NativeRuntime`'s other aborts** (a bad ring-slot count, an `eventfd` failure) are not reachable from a
  flag: `snc_server` fixes the ring slots at 1024.

## 5. Risks and downstream effects

- A launcher that treated exit 134 as "configuration refused" now sees 2 or 1. None in the repository
  does (QA 2 searched tests, bench, tools, docs runbooks and R640 cards). `grep -rn "134\|SIGABRT"` does
  hit other tests: `test_intake`, `test_s3_writeback`, `test_wal_capacity`, `test_lifecycle`,
  `test_s4_sync_tokens` and `test_s1_admission` check fail-stops of in-process engines in forked children,
  and `test_s6.py` feeds synthetic `server_rc` values to the S6 verdict. None of them starts `snc_server`
  with a refused configuration. The harnesses that launch `snc_server` (`bench/s6.py`, `bench/pilot.py`,
  `bench/lab/lab.py`, `tests/test_ckpt_cli.py`) treat any nonzero exit as a failure, so exit 2 still fails
  them.
- A `--space-uuid` with `0x`, whitespace or `+` in it, which used to start, is now refused. No launcher in
  the repository passes one. A data directory created that way holds the UUID the node printed at start
  (`space uuid ...`): pass that, or start without `--space-uuid`.
- `--shards` above 256, which aborted, and `--shards 65536` and up, which silently started with the default
  count, now exit 2.
- After the change the refused constructor's partially built members are destroyed normally (before, the
  process died). The constructor holds no files or threads at the throw sites, so there is nothing to
  release that could fail.

## 6. Three-way QA

Three independent reviews: QA 1 the code paths, QA 2 the claims and deployment impact, QA 3 behavior end
to end. None found a blocker.

| Finding | Reviewer | Resolution |
|---|---|---|
| `--shards 300`, or a `--cpus` list of 257, still aborts: `NativeRuntime` calls `std::abort()` above 256 shards | QA 1, QA 2 | Fixed in this slice: refused with exit 2 at the flag and for the default count; `--shards 65536` no longer wraps to "not given" (E6) |
| `0x`x16 gave a random UUID, not an all-zero one; `+1` and whitespace also started | QA 2 | Summary, E4 row and section 5 corrected |
| The `grep 134\|SIGABRT` claim was wrong as written | QA 2 | Section 5 lists the hits and why each is unaffected |
| `NativeRuntime` was listed as a throw site | QA 2 | Corrected: it aborts; handled by the shard-count check |
| `--host localhost` aborts in the listener; a busy port aborts before a refused configuration is reported | QA 2 | Pre-existing; added to D4-b in section 4 |
| `--control` and `--rejoin` change the directory before the engine refuses | QA 2, QA 3 | Pre-existing; scoped the E5 claim and proposed D4-c (validate before side effects) |
| E5 passes on the old binary except for the abort check | QA 3 | Stated in section 3 |
| The exit 1 branch is untested | QA 3 | Stated in section 3: no flag reaches it |
| Doubled `--cluster: cluster:` prefix | QA 3 | Now `snc_server: --cluster refused: cluster: ...` |
| `parse_uuid` comment named `-1`, which dashes-stripping never lets through | QA 2, QA 3 | Comment corrected |

QA 1 checked the `std::optional<Engine>` change (lifetime, destruction order, no joinable thread at the
early return, `[&]` captures unchanged) and the `parse_uuid` change (dashes, upper case, lengths). QA 3 ran
six refused starts against existing directories (standalone, a primary and its standby, after `SIGKILL`
with 400 accounts written): every file unchanged, and every restart recovered all 400 with no remap. It also
compared five accepted configurations on both binaries: identical output and UUIDs. Fast suite 53/53 for
QA 3 before E6 was added.
