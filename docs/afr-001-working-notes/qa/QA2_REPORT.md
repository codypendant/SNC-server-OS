# QA2 independent verification: claims N1-N9 (main @ 24006ee)

Method: I read the source and ran loopback-only experiments with the release binaries in scratchpad/build.
Scratch data was kept under qa2/ and deleted afterwards, and every process I started was killed.
I did not read other reviewers' notes (recon/, expB_*), or docs/AFR_001_INVENTORY.md beyond one grep hit line.
I stopped running experiments that send forged state-changing messages after N3/N9. N4, N5 and N8 are verified by code reading only.

## N1 admin port: all interfaces, no auth, AUDIT PATH truncates. CONFIRMED (experiment)
- `tools/ha.h:97-110` `listen_on` binds `htonl(INADDR_ANY)` and takes no host. It is called at `tools/snc_server.cpp:1082` with only the port, so `--host` has no effect on it.
- `tools/snc_server.cpp:1277` handles `AUDIT <rest>` with no check. `audit()` at `snc_server.cpp:205-230` calls `std::fopen(path,"w")` and writes an "ID BALANCE" line for each served account.
- No authentication: `admin_loop.h` and the inline handler `snc_server.cpp:1100-1283` contain no credential check.
- Experiment: I started `snc_server --host 127.0.0.1 --port 24611 --admin-port 24612`. In `/proc/net/tcp`, the data port listens on `0100007F:6023` (127.0.0.1) and the admin port on `00000000:6024` (0.0.0.0).
  - `AUDIT qa2/n1/created.txt` returned `AUDIT 0 0 0 1 0 0` and created a 0-byte file.
  - A 29-byte existing file sent through AUDIT came back at 0 bytes. It was truncated.
- Details the claim misses:
  - (a) When accounts exist, the file receives every account ID and balance. That is a data-disclosure and placement primitive in addition to truncation.
  - (b) The file is created with the process umask (0644 here) and owned by the server user (root in this container).
  - (c) The `--control` mode also forces the data port to 0.0.0.0 when `--host` is not given (`snc_server.cpp:615-616`).
  - (d) HEALTH is also unauthenticated and returns `uuid=` and `term=` (seen in N2). That supplies the inputs the N4 Hello check needs.

## N2 PROMOTE has no term or identity guard. CONFIRMED (experiment)
- `snc_server.cpp:1278-1282`: the only check is `is_standby`, after which it sends SIGUSR1 to itself. The main loop (`snc_server.cpp:1416-1440`) calls `eng.promote()` with no caller check and no term argument.
- Experiment: a sync pair (no witness), with standby admin 24622 and primary admin 24624.
  - Sending `PROMOTE` to the standby returned `OK`, then `role=n term=1`, and the log said "promoted to primary: term 1".
  - The old primary reconnected to the still-listening receiver, got kRefusedTerm, and fenced itself (`role=f`).
- Narrowing: `eng.promote()` can still refuse ("no placement read or no lease block", `snc_server.cpp:1422-1428`) in clustered mode. That is an engine precondition, not an authority check.

## N3 witness acceptor takes unauthenticated COMMIT / PREPARE base. CONFIRMED (experiment)
- `tools/paxos.h:142-153`: COMMIT learns any value whose version is higher than chosen, then calls `save()`.
- `tools/paxos.h:157-166`: PREPARE learns `base` if its version is higher than chosen.
- `acc_[m]` creates arbitrary machine names. No message carries a cluster, space or witness-set identity or any authentication (`paxos.h:13-17`, `snc_witness.cpp:519-524`).
- Experiment: one witness on loopback with a fresh state.
  - `COMMIT placement 7 <body>` led to PLACEMENT GET and PLACEMENT SYNC returning that body at v7.
  - `COMMIT ha/0 3 X` led to `READ ha/0` returning `STATE 3 X`.
  - A PREPARE with a base at v11 recorded v11 as chosen.
  - A COMMIT to an invented machine name was persisted.
  - All of these appeared as `chosen` lines in the fsynced state file.
- Severity details the claim misses:
  - (a) `read_latest` (`paxos.h:211-232`) adopts the highest version reported by a majority. A value forged into a single witness is therefore adopted by the next proposer or reader and spread to the others. One witness is enough.
  - (b) Proposals build on base.version+1, so a forged high version moves the machine forward permanently.
  - (c) An `ha/<g>` body that parses as a State (`snc_witness.cpp:130-134`) sets uuid, term, degraded and primary. That is the input that drives PROMOTE decisions (`snc_witness.cpp:777-880`).
  - (d) Separately, the witness's own DEGRADE/NOTREADY/READY verbs (`snc_witness.cpp:423-516`) are also unauthenticated. Their only check is uuid and term, both of which HEALTH publishes.
- I did not test the multi-witness spread. It is based on code reading.

## N4 standby Receiver drops the current primary link on any new connection. CONFIRMED (code), and it is worse than the claim says
- `src/core/replication.cpp:697-707`: on `accept()`, `if (conn_ >= 0) drop();` runs before any byte of the new peer is read. A connection that never sends a Hello therefore drops the live primary link.
- `handshake()` (`replication.cpp:666-691`) checks magic, space uuid, shard, n_shards, slot_crc, `term >= cfg_.term` and `!fencing_`. There is no authentication. The listener defaults to 0.0.0.0 (`include/snc/snc.h:394`, `replication.h:232`).
- Missed details:
  - (a) A peer that is accepted can send a lease frame (`kFrameLease`, `replication.cpp:742-747`). That sets `lease_until_` from wire `first_lsn`. With `--standby-reads`, `lease_valid()` (`shard.cpp:555`) then lets the standby serve reads that may be stale for as long as that peer chooses.
  - (b) Repeated connects keep the primary link down. In sync mode that blocks or degrades the primary, and with no witness it degrades after replica_timeout_ms.
- Not exercised experimentally.

## N5 unbounded receive buffer; frame length trusted. CONFIRMED (code)
- `replication.cpp:711-721`: the read loop doubles `in_` whenever it is full (`in_.resize(in_.size()*2)`), with no cap. It also runs before the Hello is validated, so an unauthenticated peer can grow it. The initial size is 4 MiB (`replication.cpp:643`).
- `replication.cpp:738-741`: `h.len` (u32 from the wire) is used only to decide whether to wait. A frame claiming about 4 GiB keeps the receiver buffering.
- Narrowing: there is no out-of-bounds read. Records are bounded by `check_record(p+at, h.len-at)` (`replication.cpp:751-753`), and on 64-bit `sizeof(h)+h.len` cannot overflow. The impact is memory exhaustion or denial of service, not memory corruption. The `kind` field is not validated: anything other than lease is treated as records.

## N6 two primaries can both acknowledge. CONFIRMED for (b) and restart (experiment); (a) CONFIRMED by code; restart part NARROWED
- (a) async: `shard.cpp:2250-2271` sets `repl_blocked_` only when `repl_cfg_.sync`, so async never makes st.blocked > 0.
  - `repl_summary` (`shard.cpp:2761-2767`) never reports regressed for async.
  - After READY, `owe_synced=false` (`snc_server.cpp:1521-1524`). The loop (`snc_server.cpp:1443-1538`) then sends nothing to the witnesses unless blocked, regressed or a seed occurs.
  - The witness promotes on HEALTH silence once the state is not degraded (`snc_witness.cpp:858-880`).
  - The repo records this as a known characterization (R08, `tests/test_async_failover.py:21-23`). It is a documented limitation that conflicts with FAILURE_MODEL G1 ("never two primaries of one group serving", marked PROPOSED).
- (b) sync without `--witness`: `degrade_gate` is null, so the primary degrades after `replica_timeout_ms` (default 2000, `include/snc/snc.h:387`; `shard.cpp:2256-2262`).
- Fencing is in memory only: `Engine::withdraw` sets the atomic `state_->fenced` (`engine.cpp:1948-1952`) and nothing persists it.
- Experiment (continuing N2):
  - I killed the fenced old primary and restarted it with its replication target unreachable (a simulated partition). It came up as `role=p term=0` while the promoted node was `role=n term=1`. The log said "standby unreachable, continuing without it".
  - net_bench against each node completed about 3,100 and 2,900 acknowledged deposits plus about 330 reads. AUDIT showed 50 accounts on each with different totals (53161 vs 52928). Both nodes acknowledged writes, so there was a real split brain.
- Narrowing for the restart part:
  - If the new primary's replication port is reachable, the restarted node re-fences on its first Hello (kRefusedTerm, `replication.cpp:479-484`; I observed this).
  - With `--rejoin` it asks the peer and becomes a standby (`snc_server.cpp:630-682`).
  - With witnesses, its first NOTREADY gets `NO term` and it withdraws (`snc_server.cpp:1499-1502`).
  - So "starts as primary at its old term" holds in every case, but it keeps serving only while it cannot reach any of those.

## N7 cut-off primary keeps serving reads; no primary read lease. CONFIRMED (code plus experiment)
- `shard.cpp:540-575`: on a primary, the only client gate is `withdrawn_`. The lease check applies only to `repl_cfg_.standby`. No primary-side lease or liveness check exists before a READ.
- In the N6 experiment, the partitioned term-0 primary answered about 331 reads immediately while a term-1 primary existed.
- I did not test reads during the window before the first witness contact in witness mode.

## N8 withdrawn primary repeats NOTREADY about every 50 ms. NARROWED (code only)
- The loop runs while `!witnesses.empty() && !cfg.standby` (`snc_server.cpp:1443`). It does not check `eng.fenced()`, and the wake is 50 ms when witnesses exist (`snc_server.cpp:1411`).
- On the no_base path (`owe_synced && ready_base==0`), a `NO term` reply leaves `ready_base==0`, and `not_ready()` sets `owe_synced=true` again (`snc_server.cpp:1364-1366`). NOTREADY is therefore re-sent on every tick indefinitely. The same applies on the `st.blocked>0` path (`snc_server.cpp:1451-1467`).
- If the fence comes via a READY answered `NO term`, `owe_synced=false` (`snc_server.cpp:1532-1535`) and the repetition stops unless blocked, regressed or a seed occurs.
- It is true for the start-up, promotion, blocked and seed paths, not for every withdrawal. Each ask is serial with a 1500 ms timeout per witness, so the real period is max(50 ms, RTT).

## N9 PLACEMENT SYNC (a read) persists a Paxos promise. CONFIRMED (experiment)
- `snc_witness.cpp:204-207` routes SYNC to `read_linear` (`paxos.h:244-310`). That runs phase 1 (PREPARE) on all peers, and each acceptor does `a.promised = ballot; save();` (`paxos.h:176-180`), an fsync of the file and its directory.
- Experiment: after `PLACEMENT GET` the state file was unchanged. After the first SYNC it gained `promised placement 114866402773952`, and the second SYNC raised it to `...775744`.
- Missed details:
  - (a) Every witness persists the promise, not only the receiver.
  - (b) A read's higher ballot can NACK a concurrent proposer's ACCEPT, so reads can starve writes.
  - (c) If phase 1 finds an accepted value, a read also runs ACCEPT and COMMIT (`paxos.h:291-303`).

## Cross-cutting notes
- Stale reads (N7), resumed-primary role (N6 restart) and fencing share one cause. Authority is in-memory state plus contact-driven refusal: the receiver's kRefusedTerm, the witness's `NO term`, or `--rejoin`. Nothing is persisted, and there is no time-bounded lease on the primary.
- The identity checks that do exist (uuid and term in the Hello and the witness verbs) rely on values that unauthenticated HEALTH publishes.
