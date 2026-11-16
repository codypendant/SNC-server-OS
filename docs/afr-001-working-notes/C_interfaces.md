# C. Interfaces inventory: CLIs, listeners, protocols, management surfaces

Reviewer C, Phase 1 (reconnaissance only). Repository: `/home/user/snc-server-os`.
Build used: `build` (binaries present, built 2026-11-15 23:49).

Labels:
- **VERIFIED**: I read the cited code, or ran the command and show its output.
- **DOCUMENTED**: a comment or doc says so, and I did not check it against behavior.
- **INFERRED**: my conclusion from code I read, not exercised.
- **UNKNOWN**: not determined.

Runtime checks: one scratch `snc_server` (2 shards, data ports on 127.0.0.1:47200-47201, admin port 47290, data dir in the scratchpad) and one scratch `snc_witness` (port 47300, new state file). I sent them read-only verbs only: HEALTH, STATUS, RESOURCES, GAUGES, AUDIT with no path, MOVE ROLE, an unknown verb, KEEP 1, PLACEMENT GET/SYNC, READ, and wire TOPOLOGY/CLUSTER. Both processes were stopped with SIGTERM and their scratch directories deleted (`pgrep` showed none left).

---

## 1. Executable inventory

All `add_executable` targets come from `CMakeLists.txt` (VERIFIED).

| Target | Source | Purpose (from the file header) | Class |
|---|---|---|---|
| `snc_server` | tools/snc_server.cpp | The engine behind a TCP front end. Also runs the admin port, replication, seeding, node links, and the cluster/control-plane client. | **production** |
| `snc_ctl` | tools/snc_ctl.cpp | Control-plane CLI: placement `get` / `init`, slot `move` / `move --abort`, `placement backfill`. | **production** (operator) |
| `snc_witness` | tools/snc_witness.cpp | Failover witness plus Paxos control plane (HA machines, placement, mint leases). | **production** |
| `snc_bench` | bench/snc_bench.cpp | In-process locality/durability micro-benchmark. | bench |
| `snc_bench_seastar` | bench/snc_bench.cpp | The same, on the Seastar backend. Built only with `-DSNC_SEASTAR=ON`, so it is not in this build. | bench (optional) |
| `baseline_bench` | bench/baseline_bench.cpp | Conventional striped-lock baseline, in process. | bench |
| `baseline_server` | bench/baseline_server.cpp | Thread-pool baseline behind the same `ConnSet` wire front end. | bench (also listens) |
| `net_bench` | bench/net_bench.cpp | TCP load generator for snc_server and baseline_server. | bench |
| `stage_stall` | bench/stage_stall.cpp | Single-process reproducer for the stage_slot stall. | bench |
| `snc_load` | bench/snc_load.cpp | The lab's transfer load (`create` / `run`) and offline checker (`check`). | bench / lab |
| `ckpt_walk_cost` | bench/ckpt_walk_cost.cpp | Measures checkpoint walk cost. | bench |
| `ckpt_writeback_char` | bench/ckpt_writeback_char.cpp | Characterizes writeback with plain syscalls. | bench |
| `test_*` (29 in the foreach list, plus `test_ha_caller`) | tests/*.cpp | ctest unit and integration tests. | test-only |

The Python files in `tests/` (test_witness.py, test_control.py and others) run as ctest entries; they are not executables.

Python and shell drivers in `bench/` (all bench-only; each line is the first line of the file's docstring, VERIFIED):
- `bench/lab/lab.py`: "The real-machine lab". argparse: `lab.py INVENTORY {deploy,p0,p1,p2,p3,p4,p5,soak}` with `--seconds --accounts --threads --depth --depths --fault --failover-ms --delay-ms --cut-s --slot-size --chunk --crash --at --minutes --seed` (lab.py:899-916). It prints result JSON and exits 1 when `ok` is false (lab.py:950-951).
- Others in `bench/lab/`: `cpu_sample.py` (per-thread CPU sampler), `trace_merge.py` (merges SNC_TRACE lines), `vultr_setup.sh`, and the inventories `loopback.toml`, `vultr.toml`, `inventory.example.toml`.
- `bench/`: `pilot.py`, `s6.py`, `s1_move.py`, `s0_overhead.py`, `p1_intake.py`, `profile_check.py`, `run_failover.py`, `run_matrix.py`, `run_net_matrix.py`, `run_promotion.py`, `run_retries.py`, `run_seeding.py`, `run_standby_reads.py`, `summarize.py`, `manifest.sh`, `run_replication.sh`.
- lab.py has its own admin-port client, `ask(addr, port, line, timeout)` (lab.py:95), and parses STATUS and AUDIT replies itself (lab.py:106, 489-513).

---

## 2. Command-line interfaces

### 2.1 snc_server (tools/snc_server.cpp)

**Grammar.** A hand-written loop over argv (snc_server.cpp:286-549). Each `--flag` is followed by its value as a separate word; there is no `--flag=value` form. A missing value prints `missing value for <flag>` and calls `std::exit(2)` (snc_server.cpp:288-293). Any unrecognised word prints `unknown flag X` and returns 2 (snc_server.cpp:545-548). Positional arguments are not accepted.

**Help.** None. The usage text exists only as the comment at the top of the file (snc_server.cpp:5-24). VERIFIED by running:
```
$ snc_server --help      -> "unknown flag --help"  exit=2
$ snc_server -h          -> "unknown flag -h"      exit=2
```

**Flags as parsed.** Line numbers are in snc_server.cpp. "atoi" means the value is not validated: non-numeric text becomes 0 and out-of-range values are truncated by `static_cast<u16/u32>`.

| Flag | Argument | Default | Validation | Line |
|---|---|---|---|---|
| `--shards` | N | 0, which means one shard per CPU in the affinity mask (575-594) | atoi | 295 |
| `--cpus` | `0,1,...` | empty | atoi per item | 296, 237-245 |
| `--host` | IPv4 literal | `127.0.0.1` (net.h:85); `0.0.0.0` when `--control` is given and `--host` is not (616) | not checked at parse time. A hostname aborts later in `inet_pton` (conn_set.cpp:49). | 297-300 |
| `--port` | P (base data port) | 7400 (net.h:86) | atoi, cast to u16 | 301 |
| `--objects` | N | 1,000,000 | strtoull | 302 |
| `--data-dir` | DIR | empty (in memory) | none | 303 |
| `--group-commit-us` | N | Engine default | atoi | 304 |
| `--no-mint-local` | none | mint_local=true | none | 305 |
| `--spin` | none | off | none | 306 |
| `--pages` | `4k`, `thp` or `hugetlb` | Engine default | checked: `--pages takes 4k, thp or hugetlb` | 535-544 |
| `--replica-of` | HOST:PORT | none | needs a colon, else `--replica-of takes HOST:PORT`. The port is atoi. | 307-316 |
| `--async-replica` | none | sync | none | 317 |
| `--replica-timeout-ms` | N | Engine default | atoi | 318 |
| `--standby` | PORT | off | atoi | 319-322 |
| `--standby-host` | IPv4 | `0.0.0.0` (snc.h:394) | none | 323 |
| `--seed-port` | P | 0 (off) | atoi | 324 |
| `--seed-from` | HOST:PORT | empty | none | 325 |
| `--cluster` | `G,G,...` where each G is `host:link:shards[:client]` joined by `\|` | empty | `parse_cluster` throws, which is uncaught: exit 134 (VERIFIED below) | 326 |
| `--group` | G | 0 | atoi | 327 |
| `--member` | M | 0 | atoi | 328 |
| `--links-generation-file` | PATH | none | none. **Missing from the usage comment.** | 329 |
| `--xnode-hb-ms`, `--xnode-dead-ms`, `--xnode-release-ms`, `--xnode-block-ms` | N | Engine defaults | atoi. **Missing from the usage comment.** | 330-333 |
| `--links-generation-min` | N > 0 | none | checked; also prints a notice on stderr. **Missing from the usage comment.** | 334-346 |
| `--control` | HOST:PORT[,...] | none | `control::parse_members`, else an error and exit 2 | 347-352 |
| `--node-id` | N | -1 | atoll. A cross-check at 586-590 needs `--control`, `--data-dir`, and no `--cluster`. | 353 |
| `--report-ms` | N | 0 | atoi | 354 |
| `--standby-reads` | LEASE_MS | 0 | atoi | 355 |
| `--admin-port` | P | 0 (no admin port) | atoi | 356 |
| `--debug-admin` | none | off | enables the DEBUG verbs | 357 |
| `--no-keep` | none | keep on | turns off the K0 kept connections | 358 |
| `--move-units` | `on` or `off` | on (DOCUMENTED, 25-29) | checked | 359-366 |
| `--move-unit-us`, `--move-unit-bytes`, `--move-window-bytes` | positive N; us at most 1e7 | 1000 us, 1 MiB, 8 MiB (DOCUMENTED) | strtoull, rejects 0 | 367-378 |
| `--admission` | `s1` or `legacy` | s1 | checked | 379-387 |
| `--waiter-budget-bytes`, `--wal-buffer-budget-bytes`, `--pending-budget` | positive N | Engine defaults | digits only, checks ERANGE | 388-401 |
| `--ckpt-pacing` | `on` or `off` | on | checked | 402-410 |
| `--ckpt-target-ms`, `--ckpt-max-age-ms` | N up to 2^40 | 30000 / 300000 (seen in the startup line) | checked; also cross-checked target <= max age (798-802) | 411-423 |
| `--ckpt-walk-batch`, `--ckpt-walk-{min,max,ceiling,step}-us` | 1..2^32-1 for batch; 1..6e7 us for the others | 16 / 20 / 100 / 250 / 10 (startup line) | checked; also cross-checked min <= max <= ceiling (805-812) | 424-446 |
| `--ckpt-io` | `legacy` or `dsync` | dsync | checked | 447-456 |
| `--ckpt-window` | 1..64 | 4 (gauge `ckpt_window=4`) | checked | 457-467 |
| `--ckpt-sync-tokens` | 0..256 | 0 (unlimited) | checked | 468-479 |
| `--adopt-role` | `primary` or `standby` | none | checked | 480-489 |
| `--debug-barrier` | `hold:MS` or `crash` | none | checked; needs `--debug-admin` (550-553) | 490-500 |
| `--witness` | HOST:PORT[,...] | none | `ha::parse_hostport` (port must be non-zero) | 501-513 |
| `--rejoin` | `STANDBY_PORT,PEER_HOST:ADMIN,PEER_HOST:SEED` | none | checked | 514-525 |
| `--checkpoint-s` | seconds (float) | 0 (never) | atof | 526 |
| `--catchup-mib-s` | MiB/s (float) | 0 | atof | 527 |
| `--replica-retain-mib` | N | Engine default (1 GiB, replication.h:79) | strtoull | 528 |
| `--space-uuid` | 32 hex digits; dashes allowed | none | length checked. Non-hex text throws from `std::stoul` (uncaught, INFERRED from snc_server.cpp:248-255). | 529-534 |

**Environment variables that change server behavior** (VERIFIED by grep of `getenv`): `SNC_CKPT_IO`, `SNC_CKPT_SYNC_TOKENS`, `SNC_FOLLOWUP_INTAKE`, `SNC_FOLLOWUP_WINDOW`, `SNC_EXCHANGE_BUDGET`, `SNC_LINK_HOLD_MAX`, `SNC_XNODE_ACCOUNTING` (engine.cpp:474-523), `SNC_TRACE` (trace.h:26) and `SNC_HA_KEEP` (ha.h:214). A bad value throws `std::invalid_argument` out of the `Engine` constructor. That constructor sits outside the `try` at snc_server.cpp:813, so the process aborts:
```
$ SNC_CKPT_IO=bad snc_server --shards 1 --host 127.0.0.1 --port 47100
terminate called after throwing an instance of 'std::invalid_argument'
  what():  SNC_CKPT_IO must be legacy or dsync
exit=134
```

**Signals** (snc_server.cpp:769-775, 1424-1456): SIGINT and SIGTERM stop the server (it prints per-shard stats and exits 0). SIGUSR1 promotes a standby in place. `PROMOTE` on the admin port sends SIGUSR1 to the process itself (1278-1282).

**Exit codes** (VERIFIED by reading; observed where marked "obs"):

| Code | When |
|---|---|
| 0 | Normal stop after SIGINT or SIGTERM (1636). obs: SIGTERM gave a clean shutdown report. |
| 1 | Runtime refusal: `control::resolve` failed (596); rejoin could not rename the data dir (660); `eng.start()` threw, printing `snc_server: not started: <why>` (815-821); the data dir holds another space than the control map (826-831); the admin port could not be bound (1084-1089). |
| 2 | Any flag or config validation error. obs: every bad-flag case below. |
| 134 (SIGABRT) | A data port that cannot be bound, an unparseable `--host` (`ConnSet` calls `die()`, which calls `std::abort()`, conn_set.cpp:22-26, 49-51); a standby receiver that cannot listen (replication.cpp:635-640); an exception thrown by the `--cluster` parser or by the Engine constructor (env vars). obs below. |

Real output captured:
```
--bogus                    -> "unknown flag --bogus"                                 exit=2
--port                     -> "missing value for --port"                             exit=2
--pages foo                -> "--pages takes 4k, thp or hugetlb"                     exit=2
--replica-of nohost        -> "--replica-of takes HOST:PORT"                         exit=2
--control x                -> "--control takes HOST:PORT[,HOST:PORT...]"             exit=2
--node-id 1                -> "--control needs --node-id and --data-dir, and replaces --cluster" exit=2
--ckpt-window 0            -> "--ckpt-window takes 1 to 64"                          exit=2
--debug-barrier crash      -> "--debug-barrier is for tests: it needs --debug-admin" exit=2
--move-units maybe         -> "--move-units takes on or off"                         exit=2
--host localhost           -> "inet_pton: No such file or directory"                 exit=134
port already bound         -> "bind: Address already in use"                         SIGABRT (-6)
--cluster garbage          -> "terminate called ... cluster: expected host:link_port:shards[:client_port], got 'garbage'" exit=134
--shards abc               -> starts with shards=4 (atoi gives 0, which means "auto")  [observed]
```

**Output format** (VERIFIED, obs):
- Startup lines on stdout are free text with some embedded `key=value` pairs:
  ```
  recovery: checkpoint before serving in 0.00s
  snc_server shards=2 ports 47200-47201 durability=DURABLE (WAL, RWF_DSYNC) mint_local=1
  ckpt_pacing: pacing=on batch=16 min_us=20 max_us=100 ceiling_us=250 step_us=10 update_us=1000 target_ms=30000 max_age_ms=300000
  space uuid 1bb4f64138fff78759eb9931832bac2b
  ```
  Other startup lines, depending on configuration: `control: node N, group G member M, placement epoch E [(stored map ...)]` (617-619), `cluster: group ...` (848-851), `rejoin: ...` (661-671), `adopt: ...`.
- Runtime lines: `report t=... live=... lag=...` when `--report-ms` is set (1593-1597), `witness: ...` lines, `checkpoint: N objects in Xs, ...`, `promoted to primary: term T, in Xs`, and `debug: ...`.
- On shutdown (1608-1635), free text: `shard i: requests ... forwarded ...%`, then `total: ...`.
- Errors go to stderr as free text, sometimes prefixed `snc_server:`, `control:`, `snc:` or `adopt:`. There is no JSON anywhere.

### 2.2 snc_ctl (tools/snc_ctl.cpp)

**Grammar** (snc_ctl.cpp:67-104). Global flags may come before or between the subcommand words. `--placement-v2` is a bare flag. `--control`, `--nodes` and `--groups` each take a value. The words `get`, `init` and `placement backfill` set the command. `move` consumes every remaining argument (89-93). If two command words are given, the last one wins (INFERRED from 85-88). Any unknown word, a missing value, or a missing `--control` or command calls `usage()`, which prints to stderr and returns **2** (49-62, 94-104).

Subcommands:

| Command | Args | Behavior | Output | Exit |
|---|---|---|---|---|
| `get` | none | `control::get` sends `PLACEMENT GET` to each member in turn (control.h:47-66) | stdout: the encoded map on one line, or `none`. On failure stderr says `snc_ctl: <why>`. | 0 for a map or none, 1 when unreachable (106-114) |
| `init` | `--nodes ID@HOST:ADMIN,...` and `--groups SHARDS:NODE/LINK/CLIENT+...,...`, optional `--placement-v2` | The CLI builds the whole first placement (random cluster ID, random space UUID per group, slot split) with `Placement::initial`, then sends `PLACEMENT INIT <map>` (control.h:101-110) | stdout: the encoded map on success, or the witness's `NO ...` reply | 0 on OK, 1 when refused or unreachable, 2 on parse errors (155-199) |
| `move SLOT GROUP` | optional `--stop-after STEP` (crash tests) and `--chunk N` (default 8) | The CLI itself runs the multi-step move driver, `migrate::Driver::move` (migrate.h:451) | stderr: the driver log. stdout: `DONE`, `ABORTED...`, `BUSY move <id> of slot <s>`, `STUCK <why> ...` or `STOPPED <step>` | 0 for DONE, ABORTED*, NONE or STOPPED*; 1 otherwise (147-150) |
| `move --abort SLOT` | none | `Driver::abort` (migrate.h:511) | `ABORTED`, `NONE`, `NO committed` or `STUCK ...` | as above |
| `placement backfill` | needs `--placement-v2` | `Driver::backfill` (migrate.h:551) | `BACKFILLED N at epoch E ...`, `NOTHING ...` or `NO ...` | 0 for BACKFILLED or NOTHING, 1 otherwise; 2 without `--placement-v2` (116-127) |

Both `--stop-after` and `--chunk` are parsed only while `move_args.size() >= 4` (snc_ctl.cpp:133). Each can be given only before SLOT GROUP. The deadline is fixed at 60 s and there is no flag to change it (131, 121).

Real output captured:
```
$ snc_ctl                -> usage (6 lines, stderr)  exit=2
$ snc_ctl --help         -> usage                    exit=2   (help exists but exits 2)
usage: snc_ctl --control HOST:PORT,... get
       snc_ctl --control HOST:PORT,... init --nodes ID@HOST:ADMIN,... --groups SHARDS:NODE/LINK/CLIENT+...,...
       snc_ctl --control HOST:PORT,... [--placement-v2] move [--stop-after STEP] [--chunk N] SLOT GROUP | move --abort SLOT
       snc_ctl --control HOST:PORT,... --placement-v2 placement backfill
STEP (for crash tests): prepared copying copied sealed staged committed activated
N: objects per request when copying ahead of the seal (default 8)
$ snc_ctl --control 127.0.0.1:47300 get            -> "none"                         exit=0
$ snc_ctl --control 127.0.0.1:47399 get            -> "snc_ctl: no control member answered" exit=1
$ snc_ctl --control localhost:47300 get            -> "snc_ctl: no control member answered" exit=1  (the witness WAS up)
$ snc_ctl --control 127.0.0.1:47300 frob           -> usage                          exit=2
$ snc_ctl ... move abc 1                           -> "terminate called ... std::invalid_argument  what(): stoul"  exit=134
$ snc_ctl ... move 99999 1                         -> usage                          exit=2
$ snc_ctl ... placement backfill                   -> "snc_ctl: backfill writes placement v2: pass --placement-v2 once every node reads v2" exit=2
$ snc_ctl ... init --nodes bad --groups x          -> "snc_ctl: bad node 'bad' (ID@HOST:ADMIN)"  exit=2
```
In `init`, `std::stoul` is also called on the node IDs, shard counts and member fields without a try (snc_ctl.cpp:165, 178, 186-187). Non-numeric text there would abort with exit 134 (INFERRED, same mechanism as the observed `move abc`).

### 2.3 snc_witness (tools/snc_witness.cpp)

**Grammar** (snc_witness.cpp:541-590). Every flag takes exactly one value; there are no bare flags. Even the switches are written `--debug 1` and `--new-state 1`. The loop takes the value before it checks the flag. So `--help` alone prints `missing value for --help` (exit 2), and an unknown flag, or a flag whose value fails its inline check, prints `bad flag K V` (exit 2). After the loop a usage line is printed (exit 2) if `--port` or `--state` is missing, if only one of `--primary` / `--standby` is given, or if `--primary` conflicts with `--group 0`.

| Flag | Arg | Default | Line (approx.) |
|---|---|---|---|
| `--port` | P | required | 555 |
| `--primary`, `--standby` | HOST:ADMIN | none | 556-557. A bad value prints `bad flag`. |
| `--group` | `G=PHOST:ADMIN,SHOST:ADMIN` (repeatable) | none | 558-569 |
| `--state` | FILE | required | 570 |
| `--failover-ms` | N | 1000 | 571 |
| `--poll-ms` | N | 100 | 572 |
| `--id` | I | 0 | 573 |
| `--peers` | `H:P,...` (at most 63) | this process alone, as 127.0.0.1:port | 574, 597-640 |
| `--health-verb` | `health` or `status` | health | 575 |
| `--debug` | `1` (anything else means off) | off | 576 |
| `--debug-clock-offset-ms` | N | 0 | 577; needs `--debug 1` (590-593) |
| `--new-state` | `0` or `1` | 0 | 578 |

Startup checks that exit 2: a `--peers` entry that is bad, unresolvable or duplicated; an `--id` that does not index this witness, or whose address is not on this machine; a state file that is missing without `--new-state 1`, present with `--new-state 1`, or not a regular file (641-672). Exit 1: `cannot listen on P` (678-682). After that the process runs until killed (no signal handling except SIGPIPE ignored).

Real output captured:
```
$ snc_witness                -> usage line, exit=2:
usage: snc_witness --port P [--primary HOST:ADMIN --standby HOST:ADMIN] [--group G=PHOST:ADMIN,SHOST:ADMIN ...] --state FILE [--failover-ms N] [--health-verb health|status] [--poll-ms N] [--id I --peers H:P,...] [--new-state 1]
$ snc_witness --help         -> "missing value for --help"                    exit=2
$ snc_witness --port 47300 --bogus 1        -> "bad flag --bogus 1"          exit=2
$ snc_witness --port 47300 --health-verb foo -> "bad flag --health-verb foo" exit=2
$ ... --state F (absent)     -> "snc_witness: state file F does not exist. A witness's first start says so with --new-state 1; ..." exit=2
$ ... --new-state 1 --debug-clock-offset-ms 5 -> "--debug-clock-offset-ms needs --debug 1 (tests only)" exit=2
$ ... --id 0 --peers 127.0.0.1:47300,127.0.0.1:47300 -> "--peers names this machine:47300 twice (entries 0 and 1): ..." exit=2
$ snc_witness --port 47300 --state F --new-state 1   (stdout):
snc_witness: port 47300, 1 witness
snc_witness: no pair to watch: placement and leases only
```
The usage line leaves out `--debug` and `--debug-clock-offset-ms` (DOCUMENTED only in the header comment, lines 63-74). Runtime output is free-text event lines such as `t=%.2f group G ...: ...` (510-515, 700-850).

### 2.4 snc_load (bench/snc_load.cpp)

**Grammar** (snc_load.cpp:554-586). `snc_load CMD --k v --k v ...`. Every flag is a `--key value` pair put into a map. **Unknown flags are silently ignored.** Values are atoi/strtoull with no validation. Every error goes through `die()`, which prints `snc_load: <why>` and exits **1** (61-64). There is no help, only `usage: snc_load create|run|check ... (see the header of bench/snc_load.cpp)`.

| Cmd | Required | Optional (default) |
|---|---|---|
| `create` | `--connect HOST:PORT`, `--accounts N`, `--ids FILE` | `--in-slot M` (0), `--near ID`, `--start CENTS` (1000000), `--email shared\|unique` (shared) |
| `run` | `--connect`, `--ids`, `--seconds T`, `--out PREFIX` | `--threads` (1), `--depth` (12), `--hot-pct` (0), `--report-ms` (1000) |
| `check` | `--ids`, `--balances F1,...`, `--runs P1,...` | `--start` (1000000) |

Output: `create` prints `created N accounts over G groups...`. `run` prints a `#`-commented header and then whitespace-separated columns, `# t_s ops_s p50_us p99_us p999_us max_us moved migrating overloaded unknown rejected` (377, 402), and writes `PREFIX.delta`, `PREFIX.unknown`, and `PREFIX.summary` (a `key value` per line, 442-448). `check` prints `OK: ...` or `FAIL: ...` (474-547) and returns 0 or 1.
```
$ snc_load                         -> "snc_load: usage: snc_load create|run|check ..." exit=1
$ snc_load frob --connect 127.0.0.1:1 -> "snc_load: unknown command frob"            exit=1
$ snc_load create --connect 127.0.0.1:47999 --accounts 1 --ids /nonexist --bogus 1
                                   -> "snc_load: connect: connect: Connection refused" exit=1   (--bogus ignored)
$ snc_load create --connect 127.0.0.1:47999 -> "snc_load: missing --ids"              exit=1
$ snc_load run --connect           -> "snc_load: every flag takes a value"            exit=1
```

### 2.5 net_bench (bench/net_bench.cpp)

**Grammar** (net_bench.cpp:496-577). This loop is like snc_server's: `missing value for X` exits 2, and `unknown flag X` exits 2 (553-555). Cross-checks exit 2 with a message: `--mix` replaces `--index`, `--transfer` and `--reads`; `--warmup` and `--drain-s` need `--rate`; percentages must be 0..100; `--threads` and `--depth` must be at least 1. `--mix` is validated strictly (459-493). The other numbers go through atoi.

Flags and defaults (Args struct, around net_bench.cpp:89-115): `--host` (127.0.0.1), `--port` (7400), `--threads` (1), `--cpus`, `--objects` (1e6), `--seconds` (4), `--depth` (64), `--reads` (50), `--route owner|any` (owner), `--misroute` (0), `--zipf` (0), `--transfer` (0), `--xfer-local` (-1), `--index` (0), `--mix read=..,deposit=..,transfer=..,index=..`, `--rate` (0, closed loop), `--warmup` (0), `--drain-s` (10), `--ids FILE`, `--endpoint H:P` (repeatable), `--session ID`, `--request-timeout-ms` (5000), `--verify`, `--retry-claim-ttl-s` (0).

Exit codes: 0 ok; 1 load, thread or verify failure (642, 690, 874, 894); 2 usage; **3** when the retry claim is invalid and `retry claim: INVALID` is printed (896). Output is free text with embedded numbers (`total ops/s ...`, `latency ns p50 ...`).
```
$ net_bench --help              -> "unknown flag --help"                         exit=2
$ net_bench --mix read=50       -> "--mix read=50: the shares must add up to 100" exit=2
$ net_bench --port 47999 ...    -> "thread 0: connect shard 0: Connection refused" / "load failed (0 create errors)" exit=1
```

### 2.6 Other bench tools (brief, VERIFIED by running `--bogus`)
- `snc_bench`, `baseline_bench`, `baseline_server`: same loop style; `unknown flag --bogus` exits 2. Usage text appears only in the header comments.
- `stage_stall`: reads `--k v` pairs and **ignores unknown keys**. `stage_stall --bogus` ran the full scenario and exited 0 (stage_stall.cpp:69-78).
- `ckpt_walk_cost`: positional `DIR N [off|BUDGET_US]`; prints `usage: ckpt_walk_cost DIR N [off|BUDGET_US]` and exits 2.
- `ckpt_writeback_char`: `run|probe ...`; prints `usage: ckpt_writeback_char run|probe ...` and exits 2.

---

## 3. Network listener inventory

All listeners are IPv4 TCP. No listener has TLS or authentication (§7).

| Listener | Opened by | Port | Bind address | Configurable? | Backlog / limits | Protocol |
|---|---|---|---|---|---|---|
| Data port, one per shard | `net::NetServer` creates one `ConnSet` per shard (server.cpp:206-210; conn_set.cpp:40-51) | `--port` + shard i (default 7400+i) | `--host`. Default `127.0.0.1` (net.h:85); **`0.0.0.0` under `--control` unless `--host` is given** (snc_server.cpp:616). obs: `0100007F 47200/47201` in /proc/net/tcp. | yes (`--host`; IPv4 literal only) | backlog 1024. No connection cap. Frames larger than `kMaxFrame` (48 B + 64 KiB) close the connection. Reading stops while more than 8 MiB of output is queued (conn_set.cpp:19, 101, 133). | binary wire v0 (§4.1) |
| Admin port | `ha::listen_on(admin_port, 256)` (snc_server.cpp:1082; ha.h:97-110) | `--admin-port` (0 = off) | **`INADDR_ANY`, hard-coded** (ha.h:104). obs: `00000000 47290`. | port only | backlog 256. One poll loop. At most 256 half-read connections (oldest evicted), 16 queued heavy jobs (`NO busy`), 500 ms to deliver the line, 64 KiB per line (except MOVE: 256 MiB), 5 s write deadline, 64 waiting replies, kept-connection cap min(64, RLIMIT_NOFILE/16) idle with 2x that live (admin_loop.h:77-84, 105-115, 825-830). | text line protocol (§4.2) |
| Witness port | `ha::listen_on(port)` (snc_witness.cpp:678) | `--port` | **`INADDR_ANY`, hard-coded**. obs: `00000000 47300`. | port only | backlog 16 (ha.h default). **One detached thread per accepted connection, no cap** (snc_witness.cpp:695-701). 500 ms line read (serve, 416-418). | text: HA, placement, leases and Paxos verbs (§4.3) |
| Replication receiver (standby), one per shard | `repl::Receiver` (replication.cpp:627-641) | `--standby` PORT + i (or `--replica-of` port + i, engine.cpp:850) | `--standby-host`, default **`0.0.0.0`** (snc.h:394, replication.h:232) | yes | backlog 4. One connection at a time: **a new connection replaces the current one** (replication.cpp:695-700). A listen failure calls abort(). | binary `SNCREPL1` (§4.4) |
| Seed server (primary) | `Engine::start` (engine.cpp:1240-1253); served by one thread, one connection at a time (seed.cpp:109-160) | `--seed-port` (0 = off) | **`INADDR_ANY`, hard-coded** (engine.cpp:1247) | port only | backlog 4; 60 s socket timeouts (seed.cpp:116) | binary `SNCSEED1` (§4.5) |
| Node-link listener (clustered primary), one per shard | `NodeLinks::try_listen` (node_link.cpp:197-215); retried every 100 ms on failure | member `link_port` + shard (from `--cluster` or the placement) | **`INADDR_ANY`, hard-coded** (node_link.cpp:203) | port only | backlog 64 | binary node-link wire v2 (§4.6) |
| baseline_server (bench) | `ConnSet` (baseline_server.cpp:271) | `--port` + i | `--host`, default 127.0.0.1 (baseline_server.cpp:221) | yes | as for data ports | wire v0 subset |

Outbound clients (`ha::ask`, ha.h:180-209; `net::Client`, client.cpp:43; `ClusterClient`, cluster_client.cpp:40; replication Sender) all use `inet_pton`. **Hostnames are not accepted anywhere on the client side.** VERIFIED: `snc_ctl --control localhost:47300 get` returned "no control member answered" while the witness was up. snc_witness's own `--peers` check resolves hostnames with getaddrinfo (snc_witness.cpp:302-311), but the Paxos traffic then goes through `ha::ask(peers_[i].host, ...)` (paxos.h:457). So a hostname peer passes validation and can never be reached (INFERRED).

---

## 4. Protocol inventory

### 4.1 Client data wire "v0" (include/snc/wire.h; DOCUMENTED in docs/ROUTING.md:34-40)
- Little-endian, length-prefixed frames on TCP. Every frame starts with a u32 `len` covering header plus body (wire.h:42-43).
- **No version field, magic or handshake.** `reserved0` and `reserved1` are not checked by the server for EXEC/CREATE/DELETE (server.cpp:64-171). The only checked reserved field is `IndexQueryBody.reserved` (server.cpp:110-115). The name "v0" exists only in comments and docs. VERIFIED.
- `ReqHeader`, 48 B (wire.h:44-55): `len u32 | kind u8 | flags u8 | reserved0 u16 | op u32 | reserved1 u32 | req_id u64 | target ObjectID(16) | expected_version u64`.
- `RespHeader`, 48 B (wire.h:57-69): `len | kind=RESP(16) | flags | status u16 | app_code u16 | owner u16 | map_epoch u32 | req_id | id(16) | version u64`.
- Kinds: EXEC=1, CREATE=2 (op = type id; a non-null target means create_near), DELETE=3, TOPOLOGY=4 (TopologyBody: map_epoch, n_shards, 4096 x u16 owners), CLUSTER=5 (ClusterBody plus n x 72-byte ClusterEntry with a 64-byte host), INDEX=6, RESP=16.
- Request flags: `kReqAllowUndurable=1`, `kReqApplied=2`, `kReqSequenced=4`. Sequenced requests put a 16-byte `SeqPrefix{client_id, seq}` before the args. Response flags: `kRespDurable=1`, `kRespForwarded=2`, `kRespReplayed=4`.
- Errors: an unknown kind is answered with status `E_BAD_ARGS` and the connection stays open. A length below 48 or above `kMaxFrame` closes the connection silently.
- VERIFIED against the scratch server:
  ```
  TOPOLOGY resp len 8248 kind 16 status 0 req_id 42 epoch 1 ; body map_epoch 1 n_shards 2
  CLUSTER  resp len 4168 status 0 ; n_groups 0 (not clustered)
  kind 99  -> resp status 4 (E_BAD_ARGS), connection kept
  len=8    -> connection closed, no reply
  ```
- `E_MOVED` and `E_MIGRATING` carry a 16-byte `MovedResult{epoch u64, group u16, reserved}` in the result (cluster.h:74-79).

### 4.2 snc_server admin text protocol (tools/admin_loop.h, tools/snc_server.cpp:1090-1310, tools/migrate.h:189-268)

**Framing.** One request line ending in `\n`, one reply line, then the server closes the connection. The opt-in K0 mode is the exception: the first line `KEEP 1` is answered `OK keep`, later requests are `K <seq> <line>`, and replies are `<seq> <reply>`. A protocol error closes the connection; `--no-keep` turns the mode off (admin_loop.h:28-48, 690-837). Replies never contain a newline, and a reply cut short has no final newline (admin_loop.h:16-23). The verb is matched exactly and is case-sensitive. There are three dispatch classes: inline on the loop thread, heavy on one worker thread, and MOVE on a thread per request.

| Verb | Class | Needs | Reply | Mutates? |
|---|---|---|---|---|
| `HEALTH` | inline | none | One line of `key=value`: `role=p\|n\|s\|f term uuid epoch leases=lo-hi shards stall_ms stall_shard main_ms adm_* ckpt* ckpt_sync_* adm_out* adm_accept_fail adm_kept* ...` (about 40 keys, obs below; built at 998-1066) | no |
| `GAUGES [shard]` | inline | none | `shards=.. ckpt=.. ckpt_seq=.. ... sync_tokens_* ... s<i>.<gauge>=<v> ...` (141-201). More than 8 shards without an argument returns `NO GAUGES <shard> needed above 8 shards, shard < N`; a bad shard returns `NO GAUGES [shard], shard < N` (obs). | no |
| `STATUS` | heavy (asks every shard) | none | `role term uuid shards live degraded blocked epoch leases unproven` (1216-1233) | no |
| `RESOURCES` | heavy | none | `rss_kb hwm_kb heap_live waiters overflow emits_in_flight emits_held repl_retained wal_unacked wal_undurable blocked degraded disk_* ckpt_running ckpts` (1235-1276) | no |
| `AUDIT` | heavy | none | `AUDIT accounts total pending epoch held misrouted`, positional (205-232) | no |
| `AUDIT PATH` | heavy | none | the same, and it **writes a balance file to PATH on the server host** (223-226); `NO cannot write PATH` on failure | **yes (file system)** |
| `PROMOTE` | heavy | none | `OK` (raises SIGUSR1 on itself) or `NO not a standby` (1278-1282) | **yes** |
| `MOVE ROLE` | move thread | `--control` | `ROLE p\|s\|f\|m EPOCH` | no |
| `MOVE INFO SLOT`, `MOVE HAVE SLOT`, `MOVE STATES` | move thread | `--control` | `INFO <report>`, `HAVE N OBJ...` (**full object payloads in hex**), `STATES N ...` | no |
| `MOVE REFRESH` | move thread | `--control` | `OK EPOCH` (fetches the placement) | yes (local map) |
| `MOVE SEAL/UNSEAL/EXPORT/STAGE/ACTIVATE ...` | move thread | `--control` | `OK <report>`, `NO <status> <report>`, `OBJS ...` (migrate.h:11-25, 220-266). SEAL and STAGE check the control plane first (`current()`). | **yes** |
| `DEBUG STALL <shard> <ms>`, `DEBUG ACCEPT-HOLD <ms>`, `DEBUG REPL-PAUSE 1\|0`, `DEBUG DROP-ANSWERS 1\|0`, `DEBUG CKPT-HOLD commit\|sync 1\|0\|?`, `DEBUG MOVE-STOP <phase> <unit> hold:MS\|crash` | inline | `--debug-admin` | `OK...` or `NO <usage>` (1103-1212) | **yes (test faults, including process exit)** |
| `KEEP 1` | loop | keep on | `OK keep` or `NO keep full` | connection mode |
| anything else | heavy | none | `NO unknown request` (1283) | no |

Loop-level replies: `NO busy` when the queue is full, `NO line too long`, `NO stopping`. Without `--control`, `MOVE ...` lines go to the heavy handler and get `NO unknown request` (VERIFIED: `MOVE ROLE` returned `NO unknown request` on the scratch server). The admin port opens only after `Engine::start()` returns (1338-1344), so no verb is answered during recovery.

Real replies (scratch server, obs):
```
HEALTH    -> role=n term=0 uuid=1bb4f641... epoch=1 leases=0-0 shards=2 stall_ms=0 stall_shard=0 main_ms=30 adm_q=0 adm_q_hw=0 adm_busy=0 adm_half=0 adm_half_hw=0 adm_evicted=0 adm_timeouts=0 ckpt=idle ckpt_age_ms=0 ckpt_last_ms=4 ckpt_done=1 ckpt_state=ok ckpt_past_target=0 ckpt_catchup=0 ckpt_io=dsync ckpt_sync_writes=0 ckpt_sync_k=0 ckpt_sync_active=0 ckpt_sync_waiting=0 ckpt_slow_sync=none adm_out=0 adm_out_hw=0 adm_write_timeouts=0 adm_write_failed=0 adm_write_evicted=0 adm_accept_fail=0 adm_kept=0 adm_keep_ok=0 adm_keep_full=0 adm_kept_idle_closed=0 adm_kept_evicted=0 adm_kept_proto=0
STATUS    -> role=n term=0 uuid=1bb4f641... shards=2 live=0 degraded=0 blocked=0 epoch=1 leases=0-0 unproven=0
RESOURCES -> rss_kb=6260 hwm_kb=6260 heap_live=0 waiters=0 overflow=0 emits_in_flight=0 emits_held=0 repl_retained=0 wal_unacked=0 wal_undurable=0 blocked=0 degraded=0 disk_readers=0 disk_queued=0 disk_queued_hwm=0 disk_started=0 disk_stopped_on_cancel=0 ckpt_running=0 ckpts=0
AUDIT     -> AUDIT 0 0 0 1 0 0
GAUGES 1  -> shards=2 ckpt=idle ckpt_seq=1 ... s1.refused_followup=0 ... s1.consistent=1   (about 230 keys per shard, about 6.7 KB)
GAUGES 9  -> NO GAUGES [shard], shard < 2
FOO       -> NO unknown request
70000 x 'X' (no newline) -> NO line too long
KEEP 1    -> OK keep ; "K 7 HEALTH" -> "7 role=n term=0 ..."
```
Note: the GAUGES comment says about 2.5 KB per shard (snc_server.cpp:144); the observed reply for one shard was about 6.7 KB.

### 4.3 snc_witness text protocol (snc_witness.cpp:416-530; paxos.h:13-20, 127-210)
One line each way, one request per connection, no KEEP. Every verb is accepted from any peer on the same port.

| Verb | Reply | Mutates? |
|---|---|---|
| `DEGRADE uuid term [group]` | `OK` or `NO <why>` (`not learned yet`, `another space`, `term T was replaced by T2`, `bad request`, `group G is not watched here`, `no quorum`) | **yes** (HA state: NOT READY / grant) |
| `NOTREADY uuid term [group]` | `OK v=N` or `NO ...` | **yes** |
| `READY uuid term [group] after=N` | `OK v=N`, or `NO behind v=..` / `NO not ready since v=..` / `NO ...` | **yes** |
| `SYNCED uuid term [group]` | always `NO SYNCED has no causal base: ...` (kept for older primaries) | no |
| `PLACEMENT GET` | `MAP <body>`, `NONE`, or `NO no quorum` (obs: `NONE`) | no |
| `PLACEMENT SYNC` | `STATE <version> <body>` (obs: `STATE 0 -`) | runs a linearizable read. After only GET and SYNC the state file held `promised placement <ballot>`, so the acceptor state is persisted (INFERRED: read_linear runs a Paxos round). |
| `PLACEMENT INIT <body>` | `OK`, `NO already initialised`, `NO bad request` | **yes** |
| `PLACEMENT CAS V <body>` | `OK` or `NO version <cur>` | **yes**. The body is opaque to the witness: any well-formed CAS is accepted. |
| `LEASES TAKE COUNT` | `BLOCK LO HI` or `NO ...` | **yes** |
| Paxos `READ m` | `STATE <ver> <body>` (obs: `READ ha/0` returned `STATE 0 -`) | no |
| Paxos `PREPARE m v ballot <base>`, `ACCEPT m v ballot <value>` | `PROMISE ...`, `NACK n`, `LATE <chosen>`, `ACCEPTED`, `NO behind` | **yes** (acceptor state) |
| Paxos `COMMIT m <version body>` | `OK`. **Any higher version is learned as chosen and saved** (paxos.h:142-153). | **yes** |
| `DEBUG HOLD/RELEASE/HELD/SKIP-COMMIT/CLOCK-OFFSET-MS/SET-COUNTER/FAIL-PERSIST/NEXT-BALLOT/PROPOSE` | only with `--debug 1`; otherwise `NO unknown request` (obs) | yes (tests) |
| anything else | `NO unknown request` (obs: `HEALTH` returned `NO unknown request`) | no |

The witness also acts as a client of node admin ports: `HEALTH` (with `STATUS` as fallback), `STATUS` for promotion eligibility, and `PROMOTE` (snc_witness.cpp:720-841). It parses those replies as `key=value` with `fields()` (271-280) and requires a reply that starts with `role=`.

Versioning: none on the line protocol. Compatibility works by verb: an older peer answers `NO unknown request`, which callers treat as "older binary" and skip (snc_server.cpp:1386-1396; snc_witness.cpp:596-597, 735-760). State files have a legacy path (`px.init(..., legacy=ha/0)`, snc_witness.cpp:675).

### 4.4 Replication stream (src/core/replication.h:24-58; replication.cpp)
- Little-endian, one TCP connection per shard. Primary to standby: `Hello{magic "SNCREPL1"(8), space_uuid(16), shard u16, n_shards u16, slot_crc u32, term u64}`, then `RecordsHeader{len u32, kind u32 (0 records, 1 lease), first_lsn, last_lsn}` plus records. Standby to primary: `HelloAck{magic, status u32, last_crc u32, next_lsn u64}`, then `Ack{durable_lsn, lease_ts, lease_ns}`.
- Version: carried only by the magic string, `SNCREPL1`. On a magic, uuid, shard, n_shards or slot-map mismatch the standby replies `kRefusedSpace=1`; on an older term, `kRefusedTerm=3`. A fenced (promoted) receiver answers `kRefusedSpace` (replication.cpp:672-689). The primary marks itself fenced on `kRefusedTerm` (replication.cpp:479-483). Code 2 is unused (UNKNOWN whether it was retired).

### 4.5 Seed protocol (src/core/seed.h:18-41; seed.cpp:109-160)
- `SeedRequest{magic "SNCSEED1", space_uuid}`. **An all-zero uuid means "any space".** The reply is `SeedHeader{magic, status u32 (0 ok, 1 another space, 2 no checkpoint, 3 not yet: retry), n_files, space_uuid, seq, next_lease, term}`, then per file `SeedFile{name_len, pad, size}` plus name plus bytes. Version: magic only.

### 4.6 Node-link wire v2 (src/core/node_link.h:9-14, 33-66; node_link.cpp:29-66, 950-985)
- `HelloV2` (48 B): `magic "SNC2" (0x324e4353), group, shard, crc (cluster shape), version=2, epoch, conn nonce, incarnation, member, flags (bit0 accounting), zero, conn_seq`. The accept side writes one `HelloAck` (32 B, magic "SNCA") back on the same socket. Then 64-byte `Frame`s with the kind in pad[0] (DATA=0, END=1, SYNC=2, HEARTBEAT=3) and flags in pad[1].
- **This is the only internal protocol with an explicit version field.** A mismatch is refused and logged on stderr: `refused group G shard S: another wire version`. The legacy v1 magic `SNCL` is recognised and refused, and counted in `version_refusals` (gauge `link_version_refusals`). A mismatch in accounting flags is refused too (`SNC_XNODE_ACCOUNTING must match on every node`).

### 4.7 Internal (not network): `msg.h`
`Msg` is a 64-byte cross-shard ring slot (`MsgKind` EXEC_REQ..MULTI_RESP). It is in-process only and has no listener (VERIFIED, msg.h:1-68).

---

## 5. Error model

| Surface | Codes | Where | Structured? | Documented/stable? |
|---|---|---|---|---|
| Wire `RespHeader.status` | `snc::Status` u16: 0 OK, 1 E_NOT_FOUND, 2 E_TYPE_MISMATCH, 3 E_UNKNOWN_OP, 4 E_BAD_ARGS, 5 E_VERSION_CONFLICT, 6 E_APP (plus `app_code`), 7 E_TOO_LARGE, 8 E_NO_MEMORY, 9 E_OVERLOADED, 10 E_SHUTTING_DOWN, 11 E_STANDBY, 12 E_STALE, 13 E_REPLAYED_OLD, 14 E_NOT_OWNER, 15 E_WRONG_NODE, 16 E_MOVED, 17 E_FENCED, 18 E_MIGRATING | include/snc/types.h:26-48 (implicit enum numbering); names in `status_name()`, src/core/object_id.cpp:48-71 | yes (numeric) | The meanings are commented in types.h. The numbers are implicit (no `= n`), so inserting a value would renumber the rest (INFERRED risk). |
| Admin and witness text | `OK ...` / `NO <free text>`, or a verb-specific tag (`MAP`, `NONE`, `STATE`, `BLOCK`, `ROLE`, `INFO`, `HAVE`, `OBJS`, `STATES`, `AUDIT`, `PROMISE`, `NACK`, `LATE`, `ACCEPTED`, `HELD`, `BALLOT`) | spread across snc_server.cpp, migrate.h, snc_witness.cpp, paxos.h, admin_loop.h | Only the first token is reliable. After `NO` the reason is free English (`NO not a standby`, `NO term 3 was replaced by 4`), and callers match prefixes of that English (for example `reply.rfind("NO not ready since", 0)` at snc_server.cpp:1531, `"NO term"` at 1475/1500/1535, `"NO unknown request"` at 1386). MOVE `NO` replies embed `status_name()` (migrate.h:228, 237). | Partly listed in the migrate.h and snc_witness.cpp header comments. No central list. |
| Replication / seed handshake | `kRefusedSpace=1`, `kRefusedTerm=3`; seed status 0/1/2/3 | replication.h:56-57; seed.h:27-29 | numeric | header comments only |
| Process exit codes | 0 / 1 / 2 (snc_server, snc_ctl, snc_witness, net_bench), plus 3 (net_bench retry claim) and 134 (aborts); snc_load uses 1 for everything | §2 | numeric, no names | not documented in any doc I read |
| Driver results (snc_ctl move) | `DONE`, `ABORTED`, `NONE`, `BUSY ...`, `STUCK <why>`, `STOPPED <step>`, `NO committed`, `BACKFILLED`, `NOTHING`, `UNRESOLVED` | migrate.h:474-649 | first token | header of snc_ctl.cpp |

---

## 6. Inconsistencies (all VERIFIED unless marked)

1. **Flag grammar differs by tool.**
   - snc_server and net_bench: `--flag value`, bare switches, `unknown flag` gives exit 2.
   - snc_witness: every flag needs a value, so switches are `--debug 1` and `--new-state 1`. An unknown flag prints `bad flag K V`, and `--help` prints `missing value`.
   - snc_load: `CMD --k v`, unknown keys ignored, every error exits 1.
   - stage_stall: unknown keys ignored, exit 0.
   - snc_ctl: global flags mixed with subcommand words; `move` swallows the rest of argv, and its options are honoured only in front of SLOT GROUP.
2. **Help.** Only snc_ctl prints a real usage text for `--help`, and it exits 2. snc_witness prints its usage line only when a required flag is missing. snc_server, net_bench and the benches have no usage output at all; their usage lives in source comments. snc_server's usage comment leaves out `--links-generation-file`, `--links-generation-min` and the four `--xnode-*` flags. snc_witness's usage line leaves out `--debug` and `--debug-clock-offset-ms`.
3. **Bad input can abort instead of returning a usage error.**
   - snc_ctl `std::stoul` on non-numeric text: exit 134.
   - snc_server `--cluster` parse errors, bad env values and an unbindable `--host` or port: exit 134 (abort), not 1 or 2.
   - Numeric flags are mostly atoi: `--shards abc` silently means auto, and `--port 70000` is truncated to u16 (INFERRED from the cast).
4. **The same value flag has different names across tools.** Control members are `--control` (snc_server, snc_ctl) but `--peers` (snc_witness). Witnesses are `--witness` (snc_server). The data endpoint is `--connect HOST:PORT` (snc_load), `--host` + `--port` (net_bench, snc_server), or `--endpoint H:P` (net_bench failover). `--primary` and `--standby` mean admin addresses in snc_witness, but `--standby PORT` in snc_server is a replication listen port.
5. **Management logic runs in the client, not behind a server API.**
   - The whole slot-move state machine (`migrate::Driver`, about 600 lines) runs inside `snc_ctl move`. It writes placement with `PLACEMENT CAS` to the witnesses and drives each node with `MOVE SEAL/EXPORT/STAGE/ACTIVATE/UNSEAL`. No server owns the move, and a crash is recovered by rerunning the CLI (DOCUMENTED, migrate.h:5-9). The same driver class is used by tests and lab.py (INFERRED from lab.py calling snc_ctl).
   - `snc_ctl init` builds the full placement (random UUIDs, slot split) on the client and sends it as an opaque body. The witness does not interpret it (`PLACEMENT CAS` accepts any body, snc_witness.cpp:206-219).
   - snc_server implements peer-role checks client-side three times, each parsing `HEALTH`/`STATUS` `key=value` with its own loop: rejoin (630-672), adopt-role (692-765), and the witness's own `fields()` (snc_witness.cpp:271). lab.py has a fourth parser.
6. **Two health verbs overlap.** HEALTH (published state, about 40 keys) and STATUS (asks every shard, 11 keys) share `role term uuid epoch leases shards` but in a different key order. RESOURCES and GAUGES cover more of the same ground. Callers fall back from HEALTH to STATUS by testing whether the reply starts with `role=`.
7. **Outputs are hard to parse by machine.** AUDIT is positional (`AUDIT a b c d e f`). Startup and shutdown lines mix free text with `k=v`. snc_load and net_bench print free-form tables. Nothing uses JSON except lab.py's final print. The admin `NO` reasons are English, and callers depend on their exact wording (§5).
8. **Bind policy is inconsistent.** Only the data ports and the standby receiver take a bind address. The admin port, witness port, seed port and node-link ports are hard-wired to `INADDR_ANY`. The data ports switch from loopback to all interfaces implicitly when `--control` is used.
9. **"Read" verbs persist state.** Sending only PLACEMENT GET, PLACEMENT SYNC and READ to a fresh witness left `promised placement <ballot>` and `open placement 1` in its state file. PLACEMENT SYNC is a Paxos round, not a pure read (INFERRED from the state-file contents).

---

## 7. Security-relevant facts

- **No authentication, authorization, TLS or shared secret on any listener.** VERIFIED by reading every accept and handler path above; there is no credential or secret handling in tools/ or src/net. docs/ROUTING.md:84-86 lists "auth and TLS" under "Not covered yet" (DOCUMENTED).
- **Listening on all interfaces by default:**
  - admin port (`INADDR_ANY`, obs `00000000:47290`)
  - witness port (obs `00000000:47300`)
  - seed port
  - node-link ports
  - standby replication receiver (default `0.0.0.0`)
  - data ports, under `--control` only (snc_server.cpp:616)

  Only the data ports default to 127.0.0.1, and that only in non-control mode.
- **State-changing actions any network peer can trigger** (by reading the handlers):
  - On the admin port: `PROMOTE` (a standby becomes primary, which raises the term and fences the old primary).
  - On the admin port: `AUDIT PATH`, which creates or truncates any file the server's user can write, at a path the client chooses (snc_server.cpp:223-226).
  - On the admin port with `--control`: `MOVE SEAL/UNSEAL/STAGE/ACTIVATE/REFRESH`. SEAL and STAGE check the move against the control plane (`current()`), so they need a matching move id (INFERRED). `MOVE HAVE` and `MOVE EXPORT` return object payloads in hex: a data read that bypasses the wire protocol.
  - On the admin port with `--debug-admin`: every DEBUG verb, including `MOVE-STOP ... crash`, which makes the process exit.
  - On the witness port: `DEGRADE`, `NOTREADY` and `READY` (they need the space uuid and term, which HEALTH/STATUS on any admin port reveal, obs), `PLACEMENT INIT`, `PLACEMENT CAS` (any body at the right version), `LEASES TAKE`.
  - On the witness port, raw Paxos: `PREPARE`, `ACCEPT`, and `COMMIT m <version body>`. `COMMIT` makes the acceptor learn and persist any value with a higher version as chosen (paxos.h:142-153), so one unauthenticated line can overwrite a witness's HA or placement state (INFERRED from code; not exercised).
  - On the standby replication port: any peer that presents the right space uuid, shard count, slot crc and a term at least the current one is accepted as the primary, and it replaces the current connection (replication.cpp:672-700). The uuid is in the HEALTH output.
  - On the seed port: a zero uuid gets the full latest checkpoint, that is, all data (seed.cpp:132-135). The witness gate (`seed_gate`) is the only other check.
- Admin denial-of-service limits exist (half-open cap, queue cap, line cap, eviction), but there is no per-source limit. The witness spawns an unbounded thread per connection.
- Secrets or credentials: none in the code paths read. lab.py uses SSH with `BatchMode=yes` and the operator's keys (lab.py:66, 92). Inventories hold host addresses only (not inspected in depth).
- Debug surfaces are gated by flags (`--debug-admin`, `--debug 1`), not by build type. They exist in the release binaries (VERIFIED: the flags parse in the release build).

---

## Observations for the later CLI assessment (no redesign proposed)

1. There is no common argument parser. Help, exit codes and unknown-flag handling differ per tool, and only snc_ctl has a usage function.
2. Most numeric flags are unvalidated atoi; some parse errors abort the process (exit 134) instead of exiting 2.
3. snc_ctl is a thick client: the move driver and placement construction run in the CLI process, with no server-side owner.
4. The admin and witness replies are `key=value` or positional text whose `NO` reasons are English, and callers match on that English. Nothing is versioned or machine-schema'd, and none of it is JSON.
5. Read-only verbs (HEALTH, STATUS, GAUGES, RESOURCES, AUDIT, PLACEMENT GET, MOVE INFO/ROLE/STATES) and mutating verbs (PROMOTE, AUDIT PATH, MOVE SEAL..., PLACEMENT INIT/CAS, LEASES, Paxos COMMIT) share the same unauthenticated ports.
6. Bind addresses are configurable for only two of six listener types. Client-side host parsing accepts IPv4 literals only, and the resulting error ("no control member answered") hides the cause.
7. The client data wire has no version or handshake field; only the node links (v2) and the magic strings (`SNCREPL1`, `SNCSEED1`) give any version signal.
8. The documentation of the CLIs is in source header comments, which have drifted from the parsers (missing flags; the GAUGES size estimate).
