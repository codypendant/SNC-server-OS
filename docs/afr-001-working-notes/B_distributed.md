# Recon B: distributed architecture of the SNC C++ reference (replication, HA, Paxos, placement, migration)

Reviewer B, Phase 1, read-only. Source tree: `/home/user/snc-server-os` (as found 2026-11-15).
Labels: **VERIFIED** (code read, or run), **DOCUMENTED** (doc only), **INFERRED** (reasoned from verified code), **UNKNOWN**.
Paths below are relative to the repo root. One experiment was run (section 4e); its script is
`expB_o1.py`. Its work dir was deleted and every process it started was killed.

---

## 0. Headline facts

1. **The HA unit is a pair, not a group.** The witness watches exactly two nodes per group (`Pair{host[2],port[2]}`, `tools/snc_witness.cpp:531-534`, `State.primary` is 0 or 1, `:122`, `:131`). Placement allows N members per group (`include/snc/placement.h:44-48`). The replication path is one primary to one standby per shard (`src/core/replication.h:1-9`). VERIFIED.
2. **Two separate consensus-backed truths.** (a) The `ha/G` Paxos machine holds {uuid, term, degraded, primary}. (b) The `placement` machine holds an opaque text map with an epoch and at most one move. Both run on the same witness processes (`tools/snc_witness.cpp:35-47`, `tools/paxos.h:1-20`). There is also a `leases` machine for mint blocks (`snc_witness.cpp:244-273`). VERIFIED.
3. **Write safety across failover holds only in sync mode with witnesses.** It rests on term fencing at the standby's listener (`replication.cpp:680-686`) plus the sync ack rule (`shard.h:555-559`). There is no primary-side lease, so the old primary can serve stale reads (section 4a). Async mode can lose acknowledged writes **and** can run two acknowledging primaries (R08, section 5). VERIFIED (code) / INFERRED (runtime).
4. **Membership is static.** Witness peers come from `--peers`, HA pairs from `--group`/`--primary`/`--standby`, and placement nodes and groups from one `snc_ctl init`. Nodes never update group membership at runtime (`engine.cpp:1615` keeps `cur->groups`). No join or leave command exists (`tools/snc_ctl.cpp:1-32`). VERIFIED.
5. **The control-plane and replication protocols are unauthenticated and carry no cluster identity at the Paxos layer.** Any peer can send `COMMIT m <value>` and an acceptor adopts it as chosen (`paxos.h:142-153`) (new finding N1).

---

## 1. Subsystem map

### 1.1 Components

| Component | Files (lines) | Responsibility |
|---|---|---|
| WAL replication Sender (primary, per shard) | `src/core/replication.h:70-226`, `replication.cpp:78-623` | Ships each WAL flush (`on_flush`, `:301-336`). Keeps an in-memory backlog up to `retain_bytes` (default 1 GiB, `.h:80`) and catches up from disk on a helper thread (`:127-196`, read-ahead cap 16 MiB `:98`). Does the term handshake, acks, the ack timeout (`:561-575`), divergence and regression detection (`:397-404`, `:488-499`) and read-lease echo (`:519-522`, `:579-594`). |
| WAL replication Receiver (standby, per shard) | `replication.h:228-293`, `replication.cpp:625-792` | Listens on base+shard. Accepts one primary; **a new connection replaces the old** (`:697-704`). Refuses another space, shard count or slot crc, an older term, or anything once fenced (`:676-686`). Applies whole frames (`:736-764`), acks durable LSN, asks for leases (`:769-781`). |
| Shard replication glue | `src/core/shard.cpp:2097-2112` (start_sender), `:2114-2141` (stop_standby, promote), `:2146-2170` (withdraw), `:2184-2193` (apply_replicated), `:2226-2276` (poll_replication: degrade/blocked), `:2753-2768` (readiness), `shard.h:555-559` (`durable()` ack rule) | Sync ack gating, degrade decision, readiness proof, fencing and withdrawal. |
| Read gating | `shard.cpp:169-174`, `:540-558` (run_exec: E_STANDBY, E_STALE, lease), `:612-615` (read waits `last_lsn`), `:461-475` (complete_local) | Owner reads wait for the object's last LSN to be `durable()`. Standby reads need a lease. |
| Engine promotion and barriers | `src/core/engine.cpp:1320-1406` (promote), `:1257-1318` (re-seal, finish_barrier), `:980-1008` (start barrier at boot), `:840-874` (replica_config) | Fence listeners, term+1 in the superblock, lease block, placement linearizable read, re-seal. |
| snc_server HA loop | `tools/snc_server.cpp:1319-1529` | The witness client: NOTREADY, READY after=N, grant to degrade, withdraw on `NO term`. SIGUSR1 promotion (`:1412-1438`). Admin verbs: HEALTH (`:930-1061`), STATUS (`:1210-1227`), PROMOTE (`:1278-1282`, **no term guard**). `--rejoin` (`:620-677`), `--adopt-role` (`:679-768`), seed gate (`:786-797`). |
| ha line protocol | `tools/ha.h` (one-shot `ask` `:173-207`; kept conn K0 `:259-485`) | One request line, one reply line over TCP. Bind on INADDR_ANY (`:104`). |
| Paxos library | `tools/paxos.h` | Multi-machine single-decree Paxos per version. Acceptor (`:127-208`), read_latest (`:212-234`), read_linear (`:244-310`), propose (`:322-417`), wall-clock ballots (`:464-475`), synced state file (`:485-528`), strict loader (`:543-609`). |
| snc_witness | `tools/snc_witness.cpp` | HA state machine per group (`:112-138`). Readiness verbs (`:423-516`), placement GET/SYNC/INIT/CAS (`:196-242`), leases (`:252-273`), the watch/promote loop (`:715-883`), state-file start guards (`:649-674`), peer-list sanity (`:616-648`). |
| Placement and cluster map | `include/snc/placement.h`, `src/core/placement.cpp` (encode/decode `:105-260`), `include/snc/cluster.h`, `src/core/cluster.cpp` (crc `:40-58`) | Text-encoded placement v1/v2. ClusterMap with epoch, group_of[4096], arrivals. |
| Control client | `tools/control.h` | `get` (majority read, `:40-57`), `sync` (linearizable, `:62-86`), init, take_leases, node `resolve` with stored-map fallback (`:150-205`), `apply` installs fetch/sync hooks (`:211-265`). |
| Placement adoption | `engine.cpp:1609-1653` | `update_placement` (epoch strictly greater, `:1614`). `watch_placement` thread polls every `placement_poll_ms` (default 1000, `snc.h:475`) or on a node-link epoch hint. |
| Slot state and proof | `shard.cpp:3600-3830`, `:3913-4010` | Seal, reseal, unseal, export. `proven()` arrival proof (`:3651-3659`). `activate_staged` (`:3779-3805`). `serving()` (`shard.h:97-101`). |
| Migration node handler and driver | `tools/migrate.h:186-312` (NodeHandler), `:420-1009` (Driver) | MOVE verbs. Driver steps prepared→sealed→staged→committed with a commit gate (`:704-737`). |
| Node links | `src/core/node_link.{h,cpp}` | Cross-group follow-ups. Hello v2 (`node_link.cpp:30-43`, **no HA term**). Incarnation fencing (`:521-560`), member steering (`:396-417`), heartbeats, quarantine. |
| Links generation | `src/core/links_generation.{h,cpp}` | Durable, strictly increasing links incarnation per member. Clock-free (`.h:1-27`). |
| Seed | `src/core/seed.{h,cpp}` | Checkpoint transfer to a new standby, gated by `seed_gate` (`seed.cpp:138`). Carries the term (`seed.h:35`, `seed.cpp:271`). |
| ClusterClient | `src/net/cluster_client.cpp` | One `Client` per group, routed by `group_of`. Refreshes on `E_MOVED` with a newer epoch (`:161-165`). Never changes membership (`:149-151`). |

### 1.2 Processes and connections

| From → To | Listener | Dialer | Protocol | Cite |
|---|---|---|---|---|
| primary shard i → standby shard i | standby `standby_port+i` (Receiver) | primary Sender, reconnects every 100 ms | binary SNCREPL1: Hello/HelloAck, RecordsHeader frames, Ack{durable,lease_ts,lease_ns} | `replication.h:24-61`, `replication.cpp:36` |
| old primary → promoted node | promoted node's **fence listener** (old Receiver, kept in-process only) | old Sender | same; answers kRefusedTerm | `shard.cpp:2114-2120`, `:931` |
| witness → node admin port | node `--admin-port` (admin::Loop) | witness watch thread, every poll_ms | line: HEALTH, STATUS, PROMOTE | `snc_witness.cpp:721-764`, `:841` |
| node → witness | witness `--port` | snc_server main loop (50 ms tick) | line: NOTREADY/READY/DEGRADE/SYNCED uuid term [group] [after=N] | `snc_server.cpp:1341-1376`, `snc_witness.cpp:423-516` |
| witness ↔ witness | each witness `--port` | proposer broadcasts, one thread per peer, 300 ms | line: READ, PREPARE, ACCEPT, COMMIT | `paxos.h:13-20`, `:451-462` |
| node/snc_ctl/driver → witness | witness port | control.h | PLACEMENT GET/SYNC/INIT/CAS, LEASES TAKE | `control.h:40-131` |
| driver (snc_ctl move) → node admin | node admin | migrate::Driver | MOVE ROLE/INFO/HAVE/EXPORT/STAGE/SEAL/UNSEAL/ACTIVATE/REFRESH/STATES | `migrate.h:11-25` |
| group A shard → group B shard | B member `link_port+shard`, **primary only** | A NodeLinks Out, tries members in turn | binary 64 B frames, Hello v2 + HelloAck, heartbeats, END/SYNC | `node_link.h:1-20`, `node_link.cpp:30-55` |
| new standby → primary | primary `--seed-port` | standby `--seed-from` | binary SNCSEED1 | `seed.h:20-41` |
| client → node | `client_port+shard` | ClusterClient / Client | wire (CLUSTER request gives the map) | `cluster_client.cpp:35-69` |

There is no separate controller process. The migration "controller" is the stateless `snc_ctl move` driver (`migrate.h:1-9`). VERIFIED.

### 1.3 Dependency edges (VERIFIED)

`snc_server` → `Engine` → `Shard` → {`repl::Sender`/`Receiver`, `NodeLinks`, WAL}. `snc_server` → `ha.h`, `control.h`, `migrate.h`, `admin_loop.h`. `snc_witness` → `paxos.h` → `ha.h`. `snc_ctl` → `control.h`, `migrate.h`. The Engine's control-plane needs come in as std::function hooks (`placement_fetch`, `placement_sync`, `lease_blocks`, `seed_gate`, `adopt_primary_check`), so the core has no direct dependency on the tools (`control.h:211-265`).

---

## 2. State machines

### 2.1 Replica role and term (per node)

Roles in HEALTH and STATUS: `s` standby, `m` promoting, `p` primary with a sender, `n` primary without one, `f` fenced or withdrawn (`snc_server.cpp:893-899`, `:1074`).

| Transition | Trigger | Code |
|---|---|---|
| start → standby | `--standby PORT`. Superblock term is kept, or taken from the seed's `term` | `engine.cpp:841-856`, `seed.cpp:271` |
| start → primary | no `--standby`. A directory last a standby, malformed, or legacy runs the **start barrier** (linearizable placement read) | `engine.cpp:980-1008`, `:1281-1318` |
| standby → m → primary (term+1) | SIGUSR1. The witness or an operator sends PROMOTE. Steps: placement_sync (throws if the control plane is down), lease block, `stop_standby(term+1)` (fence listener), wait for settle, `++sb_.term` committed to the superblock, then serve | `snc_server.cpp:1278-1282`, `:1412-1438`; `engine.cpp:1320-1406` |
| primary → withdrawn (f) | (a) Sender gets kRefusedTerm, so `fenced_` is set (`replication.cpp:483`), then `Shard::poll` withdraws (`shard.cpp:920-922`); or (b) the witness answers `NO term`, so `eng.withdraw()` (`snc_server.cpp:1460-1463`, `:1499-1502`, `:1517-1520`) | `shard.cpp:2146-2170` |
| withdrawn → ? | **not persisted**: a restart starts as primary at the old term (`engine.cpp:1948-1952` sets only an in-memory flag) | INFERRED from code |

Term checks happen only at (1) the replication Hello (`replication.cpp:680`) and (2) witness proposals (`snc_witness.cpp:485-487`). Terms are **not** in node-link hellos (`node_link.cpp:30-43`; DOCUMENTED as deliberate in `docs/CLUSTER.md:217-219`), not in WAL records, and not in client acks.

### 2.2 Group readiness (witness `ha/G`: degraded flag = NOT READY)

State = {version, uuid, term, degraded, primary} (`snc_witness.cpp:112-138`).

| Event | Proposal | Guard | Cite |
|---|---|---|---|
| learn | uuid, term, primary=n, degraded=true | `base.uuid` empty | `snc_witness.cpp:780-804` |
| NOTREADY / DEGRADE | degraded=true, **fresh version** | uuid and term match | `:458-460`, `:477-507` (fresh=`d`) |
| READY uuid term after=N | degraded=false | linearizable read first (`:474`). Base version ≥ N. If degraded then version == N | `:493-501` |
| SYNCED (old verb) | refused | always | `:488-492` |
| promote | term+1, primary=other, degraded=true | primary silent ≥ failover_ms, `!st.degraded`, standby STATUS role=s with the same uuid and term, base unchanged | `:848-874` |

Primary side (`snc_server.cpp:1438-1529`):
- **Blocked** (sync, standby down past the timeout, gate closed) → NOTREADY. On OK the grant is set, the gate opens, and shards may degrade.
- **Healthy** again → the gate closes before READY.
- READY is sent only when every shard is `ready` (`shard.cpp:2761-2764`), not degraded, not blocked, live, and not regressed.

The readiness proof for **sync** is `link_acked_lsn ≥ durable_lsn` on the current link and not catching up. For **async** it is only `live && !degraded && !blocked` (`shard.cpp:2761-2764`, the comment says "R08 unchanged"). VERIFIED.

### 2.3 Shard sync/degrade (per shard, primary)

Each row is an action at a `poll_replication` tick (`shard.cpp:2250-2272`):
- `repl_hold_` true (sync acks).
- overdue = hold && !live && timeout≠0 && down ≥ timeout.
- blocked = overdue && (gate closed || now < lease_until).
- overdue && !blocked → hold=false, degraded=true, waiters released (**writes then ack on local durability only**).
- degraded && live && !catching_up && link_acked ≥ durable → hold=true.
- Ack timeout: a live link with an owed ack and no progress for `timeout_ns` is closed (`replication.cpp:567-575`).

Without `--witness`, `degrade_gate` is null (`engine.cpp:872`, `snc_server.cpp:512`), so a sync primary degrades on its own after the default `replica_timeout_ms` of 2000 (`include/snc/snc.h:387`). VERIFIED.

### 2.4 Witness and Paxos

- Persisted, synced before any dependent reply. Per machine: `chosen`, `open_v`, `promised`, `accepted b value` (`paxos.h:481-528`). Any save failure → `_exit(3)` (`:529-536`). A load that cannot read a line → exit 2 (`:543-609`).
- **Not persisted:** `ballot_counter_` (`paxos.h:617`). A ballot is `max(counter+1, wall_ms) << 6 | self` (`:464-475`). VERIFIED.
- PREPARE needs ballot > promised; ACCEPT needs ballot ≥ promised (`:174`, `:197`). The acceptor **learns the proposer's claimed base as chosen** (`:164-167`) and any `COMMIT` with a higher version (`:142-153`). VERIFIED.
- One proposal mutex for every machine (`one_at_a_time_`, `:326`, `:246`). The deadline is computed **after** the lock is taken (`:328`), so queued requests never expire (F7). VERIFIED.
- Start guards: `--new-state 1` is required when the file is missing and refused when it exists. Creation is exclusive by link() (`snc_witness.cpp:649-674`, `paxos.h:90-93`, `:509-517`). Peer-list duplicates are refused (`snc_witness.cpp:616-648`). A file restored from backup is undetectable (F9; DOCUMENTED `docs/PAXOS_BALLOTS.md:21`, 9.4).

### 2.5 Placement epoch and adoption

- The epoch changes only at a move's commit or the backfill (`migrate.h:842-845`, `:609-611`). Failover never touches placement (`snc_witness.cpp:38`). VERIFIED.
- Nodes adopt a map **only if its epoch is strictly greater** (`engine.cpp:1614`). Adoption runs in two passes over all shards: `set_map` (proof refresh, activate staged, release held) and then `map_changed` (resume, re-send) (`engine.cpp:1620-1621`, `shard.cpp:3753-3774`). This implements invariant 17 node-wide. VERIFIED.
- Sources: a 1 s poll and the node-link epoch hint (`engine.cpp:1636-1647`, `shard.cpp:1938-1943`). Node-link frames carry the low 32 bits of the epoch. A frame with a newer epoch is held until adoption. A frame with an older or equal epoch for a foreign target is dropped as misrouted (`shard.cpp:1944-1957`).
- `GET` is a majority read and may be behind (`snc_witness.cpp:199-203`). `SYNC` is linearizable (`:204-208`, `paxos.h:244-310`). Barrier, unseal and driver steps use SYNC (`control.h:62-86`, `migrate.h:163-166`).
- Every adopted map is kept forever (`engine.cpp:1618` `maps_.push_back`), about 70 KB per epoch. Growth is bounded by the number of moves. VERIFIED, minor.

### 2.6 Slot move states

Placement: NONE → PREPARED (CAS, `move.id` = the chosen version) → SEALED → STAGED → COMMITTED (the same CAS sets the slot owner, epoch and arrival) → NONE. Abort is allowed in any state before COMMITTED (`migrate.h:451-496`, `:499-526`, `:740-748`). Local slot phases are NONE, ACTIVE, SEALED, STAGED, plus the `kSlotUnsealed` flag (`shard.cpp:3651-3659`, `:3913-3955`, `:3987-4006`).

| Step | Who | Guards | Cite |
|---|---|---|---|
| copy ahead | driver: dst HAVE, src EXPORT, dst STAGE non-final | node `current()` does a linearizable check of move id, slot and state | `migrate.h:821-844`, `:268-278` |
| seal | src primary `seal_slot` | move id monotonic, not unproven, not foreign. Counts and digests objects (`moves_with_slot`: type ≥ 256) | `shard.cpp:3913-3955`, `:3578` |
| SEALED→STAGED | final export to stage. Aborts if the source primary has no seal (a lossy promotion) or on a persistent version conflict | `migrate.h:854-899` |
| commit gate | every source member: no `ROLE m`, at least one `p`, no `p` serving the slot | `migrate.h:704-737` |
| activate | dst `activate_staged` when map epoch ≥ st.epoch and arrival == move | `shard.cpp:3779-3805` |
| start barrier | a promoted standby or an ex-standby directory reseals if the move is PREPARED, SEALED or STAGED from its group | `engine.cpp:1263-1318` |
| unseal | linearizable read: the slot is ours and no move has this id | `migrate.h:283-296` |

### 2.7 Node link generations

- The incarnation is a durable counter per data directory (`links_generation.h:1-27`).
- A peer keeps the highest incarnation seen per (link, member). It refuses lower ones, and refuses higher ones while an older one is still live within T_dead (`node_link.cpp:521-560`). This is **per member**, not per group.
- A blocked sync primary (no standby acking for T_dead) announces it in heartbeats and leaves links after T_block (`shard.cpp:1995-2040`).
- Member steering redirects an Out link to the member that sends In connections (`node_link.cpp:396-417`).

VERIFIED.

---

## 3. Consistency actually provided

| Operation class | As built | Documented intent | Cite |
|---|---|---|---|
| Ack of DURABLE write, sync, healthy | after the local WAL is durable **and** standby acked LSN ≥ lsn | the same (REPLICATION.md:87-112) | `shard.h:555-559` VERIFIED |
| Ack, sync, degraded (witness grant, or no witness after 2 s) | local durability only. A failover now would lose it, but the witness won't promote while degraded=true | "continues in async mode… says so" | `shard.cpp:2258-2264` VERIFIED |
| Ack, async | local durability only. **Witness will promote anyway** (async readiness = link up) | CORE_CONTRACTS §9 F-1 proposes never auto-promote (not built) | `shard.cpp:2761-2764`; DOCUMENTED test_async_failover lost 500 writes (CORE_CONTRACTS.md:396-400) |
| Read on owner (default) | waits for `durable(last_lsn)`, then answers. Already-durable objects answer **at once** without any freshness check | R-1/R-2 (PROPOSED) | `shard.cpp:612-615`, `:461-466` VERIFIED |
| Read `allow_undurable` | immediate. Can show state that is later lost | weaker by design | `shard.cpp:615` VERIFIED |
| Read on standby | READ ops only, under a lease; `E_STALE` otherwise. The lease is granted only by a sync, non-degraded primary, which won't degrade before lease expiry. Safety depends on clock **rate** drift < 10% (0.9 factor) | REPLICATION.md:263-286 says the same | `shard.cpp:546-558`, `replication.cpp:519-522`, `:742-748`, `shard.cpp:2256-2257` VERIFIED |
| After failover (sync, READY) | promoted node holds every acked write. Its follow-ups resume. Sessions are in its WAL | the same | `snc_witness.cpp:852`, `engine.cpp:1355-1366` |
| After failover (async, or manual PROMOTE) | acked writes may be missing. Sessions are as old as the standby | F-4 (PROPOSED) | INFERRED |
| During partition (sync + witness) | old primary acks no writes (standby fenced, gate closed, witness says `NO term`). **Serves stale reads** of durable objects until it hears of the new term | KNOWN GAP (CORE_CONTRACTS.md:182) | section 4a |
| During partition (async) | **both** sides can ack writes (section 5) | not addressed | INFERRED |
| During migration | slot refused `E_MIGRATING` while sealed or staged at the source and staged at the destination. `E_MOVED` with epoch once foreign. Sequenced resends are answered from the session before refusal (inv 18) | MIGRATION.md | `shard.cpp:169-182`, `:356-372`, `:567-571` VERIFIED |
| Cross-group transfer | ack at the debit; the credit is applied later via a node-link follow-up, with resume passes re-emitting until settled | stated property | CORE_CONTRACTS.md:157, :188 DOCUMENTED |

---

## 4. Status re-check of recorded gaps

| Gap | Status | Evidence |
|---|---|---|
| **(a)** cut-off old primary serves stale reads | **STILL PRESENT** (cited lines moved). CORE_CONTRACTS cites `shard.cpp:362-364`. The logic is now in `run` READ at `shard.cpp:612-615` (`wait_lsn = last_lsn`) and `complete_local` `:461-466` (answers at once when durable). The only gate is `withdrawn_` (`:542-545`, `:174`). Withdrawal needs the new primary's fence listener (`:920-921`) or a reachable witness (`snc_server.cpp:1460-1463`). No primary-side lease exists anywhere (`grep lease` in shard.cpp shows only the standby lease) | VERIFIED code |
| **(b)** async standby promoted without acked writes (R08) | **STILL PRESENT.** The witness State has no mode field (`snc_witness.cpp:112-138`). Async readiness = link live (`shard.cpp:2761-2764`, comment "R08 unchanged"). Admin PROMOTE has no term guard (`snc_server.cpp:1278-1282`). | VERIFIED code |
| **(c)** standby receive unbounded, stops at 1 GiB WAL max | **STILL PRESENT, and worse than stated.** (1) `Receiver::poll` reads until EAGAIN and doubles `in_` with no cap (`replication.cpp:712-722`). (2) Each record goes to `append_raw` → `ensure_capacity` "fail-stop if it cannot fit" (`storage.cpp:623-628`), with a 1 GiB hard max (`storage.h:120-133`, the comment acknowledges it). (3) Frame `len` is a u32 from the wire, trusted for buffering (`replication.cpp:741`). (4) No flow control back to the Sender other than TCP. | VERIFIED code |
| **(d) F8** ballots after clock step | **UNCHANGED, liveness only.** The counter is not persisted (`paxos.h:464-475`, `:617`). Strict `>` on PREPARE (`:174`) makes reuse safe. DOCUMENTED proof in PAXOS_BALLOTS.md §4; my reading agrees (INFERRED). | VERIFIED code |
| **(d) F9** loss or rollback of acceptor state | **MITIGATED FOR DELETION ONLY.** A missing file without `--new-state 1` → refuse. An existing file with it → refuse (`snc_witness.cpp:663-673`). Restore from an older backup: file exists, so nothing detects it, and a safety break is possible (DOCUMENTED `PAXOS_BALLOTS.md:21`). Operator misuse of `--new-state 1` after deleting the file is still possible. | VERIFIED code |
| **(e) O1** resumed old primary reports `role=p` at old term | **NARROWED (experiment).** Unclustered single-shard sync pair, one witness: SIGSTOP the primary, the standby is promoted in 1.26 s, then SIGCONT. HEALTH said `role=p term=0` at t=0 and `role=f term=0` at t=0.102 s, through the Sender refusal (`replication.cpp:479-486`). From code, role=p persists **indefinitely** when the new primary's fence listener is unreachable (partition, or the promoted process restarted, because the fence listener is in-memory only, `shard.cpp:2119`). In sync mode it then lasts until blocked plus a witness `NO term` (about `replica_timeout_ms`). In async mode it never ends (it never asks the witness: the async primary is never `blocked`). The clustered variant behind O1 was not re-run. HEALTH's `term` is the pre-promotion term (`snc_server.cpp:883`, `:943`). | VERIFIED (run, see method) + INFERRED |
| **(f) F6** degraded mode no back-pressure | **STILL PRESENT.** No path throttles client writes on standby lag. `catchup_bytes_per_s` throttles only the Sender (`replication.cpp:221-258`). Memory backlog is capped at `retain_bytes` and then goes to disk (`:313-335`). | VERIFIED code |
| **(f) F7** readiness latency / shared proposal queue | **STILL PRESENT.** One mutex for every machine (`paxos.h:326`). The deadline starts after the lock (`:328`). Broadcast joins every peer thread (300 ms each) (`:451-462`). The witness serves each connection on a detached thread with no bound (`snc_witness.cpp:695-701`). New aggravation (N5): a withdrawn primary sends NOTREADY every 50 ms forever. | VERIFIED code + run |
| **(g)** no-witness degrade in sync mode | **STILL PRESENT.** `degrade_gate` is only set with `--witness` (`snc_server.cpp:512`, `engine.cpp:872`). The default timeout is 2000 ms (`snc.h:387`). Manual promotion plus a degraded old primary gives two acking primaries (REPLICATION.md:104-111 DOCUMENTED as operator responsibility). | VERIFIED code |
| **(h)** sessions do not move with slots; cross-group resend | **STILL PRESENT.** `moves_with_slot(type) = type >= 256` (`shard.cpp:3578`). Session type is 1 (`session.h:13`). ClusterClient keeps one `Client` per group, each with its own sequence space under the **same** client_id (`cluster_client.cpp:85-89`). An app-level resend after `E_MOVED` is a new request in the destination's sequence (test pattern `tests/test_migrate.cpp:510-515`). It is safe only because the source answers a duplicate from its session before refusing (`shard.cpp:176-182`, `:356-372`). It is unsafe when the outcome is unknown and the source answer is unavailable (source session expired, R11, or the source group down and the client gives up on it) and the app resends through the new map. DOCUMENTED "safe today… no doc says so" (`docs/EVIDENCE.md:410-412`). | VERIFIED code / INFERRED risk |

O1 method: one `snc_witness` with no peers and `--failover-ms 1000`, a 1-shard primary with `--replica-of` and `--witness`, and a 1-shard standby with `--standby`. HEALTH was polled every 20 ms. The script is at the path given in the header.

---

## 5. Split-brain and stale-leader analysis

### 5.1 Can two primaries of one group both acknowledge writes?

| Configuration | Answer | Mechanism / cite |
|---|---|---|
| sync + witnesses (gated), failover by witness | **No** (INFERRED, strong). The old primary acks only when the standby acks (`shard.h:555-559`). The standby fences before the new term exists (`engine.cpp:1355-1358`) and refuses older terms (`replication.cpp:680`). Degrading needs a witness grant that is refused for a replaced term (`snc_witness.cpp:485-487`). A grant and a promotion are mutually exclusive by Paxos on one machine (`:852`, `:865`). |
| sync, no witness, timeout > 0 (default) | **Yes.** The old primary degrades after 2 s on its own (`shard.cpp:2250-2264`, gate null). An operator promotes the standby. Both ack. |
| sync, no witness, timeout 0 | No: it never degrades (`shard.cpp:2251`). |
| async (with or without witness) | **Yes.** The async primary never blocks, so it never asks the witnesses after READY (`snc_server.cpp:1452`, `shard.cpp:2250` sync-only). The witness promotes the reachable standby after `failover_ms` of HEALTH silence (`snc_witness.cpp:848-874`). The old primary keeps acking until its Sender reaches the new primary's fence listener (a partition prevents that). Divergent acked writes; the old side's are discarded on rejoin (`snc_server.cpp:654-660` moves the directory aside). |
| manual PROMOTE or SIGUSR1 (any mode) | No term guard (`snc_server.cpp:1278-1282`). Safety falls back to the rows above. |
| `--rejoin` peer unreachable | starts as primary at the old term (`snc_server.cpp:668-675`). With witnesses: refused, then withdraws (start NOTREADY, `:1486-1502`). Without: as "sync, no witness". |
| old primary restarted after being fenced | withdrawal is not persisted (`engine.cpp:1948-1950`). Same as the row above. |

### 5.2 Can two groups both serve one slot?

**Designed: no** (INFERRED from code; DOCUMENTED test `MIGRATION.md:315-321` "Never two serving owners").
- The source stops serving at a durable seal (`shard.cpp:3913-3955`; sync durability includes the standby).
- The destination serves only after the commit, at map epoch ≥ move epoch, with an exact arrival match (`shard.cpp:3787-3792`, `proven` `:3651-3659`).
- The commit gate refuses while any reachable source member serves or is promoting (`migrate.h:704-737`).
- A promoted source standby re-seals (`engine.cpp:1263-1276`, `:1395-1404`).

Residual windows:
- The **gate only sees members that answer**. The code says so: "a second primary the driver cannot reach is the HA problem (R08)" (`migrate.h:700-703`). If the source is async or no-witness split-brained (5.1), an unreached old source primary that never saw the seal keeps serving the slot after the commit. Its acks are then lost. INFERRED.
- A cut-off source primary serving stale **reads** (4a) does so for any slot it still considers its own, including a slot just committed away, until it adopts the new map. Its seal (if it had one) blocks this; if the seal was written on the other side of a failover, the old process has no seal. INFERRED.

### 5.3 Timing assumptions safety depends on

| Assumption | Where | Effect if violated |
|---|---|---|
| Clock **rate** drift < 10% between primary and standby over `lease_ns` | `replication.cpp:742-748` (0.9), `:519-520` | a standby lease outlives the primary's commitment, so standby reads can miss acked writes (sync degrade path). |
| none for sync write safety | term fencing + sync acks | — |
| failover_ms / poll_ms | `snc_witness.cpp:848` | liveness and false failovers only (sync). **Safety** in async (5.1). |
| wall clock for ballots | `paxos.h:466-471` | liveness only (F8). |
| session TTL wall clock (R11) | not in my scope; CORE_CONTRACTS §10 | double apply after expiry (DOCUMENTED). |
| node-link T_dead / T_block / T_release | `node_link.h:82-84` | follow-up loss until resume; liveness. |

No primary-side lease exists, so reads have **no** timing-based protection on the primary side (4a).

---

## 6. Membership

- **Witness quorum:** static `--peers` list, up to 63, majority = n/2+1 (`snc_witness.cpp:597-648`, `paxos.h:116`). No reconfiguration protocol. Adding a witness means restarting all of them with a new list, and nothing checks that the lists agree. INFERRED risk: two witnesses with different `--peers` compute different majorities.
- **HA pairs:** static per witness: `--primary/--standby` (group 0) or `--group G=P:ADMIN,S:ADMIN` (`snc_witness.cpp:556-569`). They are **not derived from placement**, so the group number and members can disagree with the placement map. INFERRED.
- **Placement:** nodes (id@host:admin) and groups (shards/uuid/members) are written once by `snc_ctl init` (`snc_ctl.cpp:5-12`). INIT refuses another body (`snc_witness.cpp:226-240`). CAS accepts any body. The witness treats it as opaque and does not validate it (`:209-225`), so a CAS with a bad body would be chosen and nodes would then fail to decode it (INFERRED).
- **Nodes:** `--control` with `--node-id` finds its group and member in the map (`control.h:150-205`). `--cluster` is a static list where every node must get the same string (`CLUSTER.md:297-299` DOCUMENTED). On running nodes, `update_placement` changes only `group_of`, epoch and arrivals. It never changes groups or addresses (`engine.cpp:1615`). ClusterClient ignores maps with a different group count (`cluster_client.cpp:149-151`).
- **Join and leave:** only the standby re-entry path: seed (`seed.cpp`), `--rejoin` (`snc_server.cpp:620-677`), `--adopt-role` (`:679-768`). No node add or remove, group add, or shard-count change. VERIFIED (absence by grep of snc_ctl verbs).

---

## 7. New findings

### 7.1 Verified at file:line

- **N1. Paxos acceptors accept unauthenticated `COMMIT` (and the PREPARE base) as chosen, with no cluster or machine identity.**
  - `paxos.h:142-153`: `COMMIT m <value>` with a higher version calls `learn` and then `save`.
  - `:157-167`: PREPARE carries `base`, which is learned if newer.
  - The witness listens on INADDR_ANY (`ha.h:97-110`, `snc_witness.cpp:678`).
  - Messages carry only machine names (`ha/0`, `placement`, `leases`), and those are identical in every cluster.
  - Consequence: a misdirected `--peers` entry (another cluster's witness), a test harness, or any host on the network can make a witness "learn" an arbitrary HA state (term or primary) or placement map. A forged commit to a majority rewrites the decision, and `read_latest` returns it (`:212-234`).
  - Severity: critical under an untrusted or shared network. Otherwise a misconfiguration hazard. Rust: authenticate, and bind a cluster id into every Paxos message.
- **N2. The replication Receiver accepts and replaces its primary connection on any Hello whose term is ≥ its own, with no authentication** (`replication.cpp:697-704`, `:676-691`).
  - The Hello fields (uuid, shard count, slot crc, term) are all observable.
  - Any peer can drop the real primary's link: in sync mode that blocks the primary, and in no-witness mode it forces a degrade.
  - With matching LSNs, a peer can also inject records that the standby appends to its WAL (`apply_` → `append_raw`, `shard.cpp:2188`).
  - The Receiver never raises its own term to the accepted primary's (`handshake` leaves `cfg_.term` unchanged, `replication.cpp:680-691`).
  - Severity: data corruption only by a hostile or buggy peer. A stale lower-term primary is refused correctly.
- **N3. The admin port is unauthenticated on all interfaces** (`ha.h:104`, `snc_server.cpp:1082`).
  - PROMOTE has no term guard (`:1278-1282`).
  - MOVE STAGE writes objects into a group if the placement step matches (`migrate.h:233-252`).
  - MOVE SEAL is reachable.
  - DEBUG verbs are guarded only by `--debug-admin`.
  - Severity: a split-brain trigger (a PROMOTE to the standby of a live async primary).
- **N4. Standby receive path is unbounded** (expands gap c): see (c). The wire-supplied `h.len` up to 4 GiB is buffered (`replication.cpp:741`, `:713`) before any check, and growth fails stop at 1 GiB in the WAL (`storage.cpp:627`). This is unbounded resource growth on a network path.
- **N5. A withdrawn (fenced) primary polls the witnesses with NOTREADY every 50 ms forever.**
  - The HA loop keeps running while `!cfg.standby` (`snc_server.cpp:1445`).
  - `no_base` stays true (`owe_synced && ready_base==0`, `:1486`).
  - `withdraw()` is skipped once fenced (`:1499`).
  - Each NOTREADY makes the witness take the global proposal lock and run a majority READ broadcast (`snc_witness.cpp:477`, `paxos.h:326-333`).
  - VERIFIED (run): about 20 per second in the witness log until the process was killed.
  - Effect: steady control-plane load. It competes for the single proposal queue that other groups need (it amplifies F7), and with a slow or stopped peer each request holds the lock for ≥ 300 ms.
- **N6. Withdrawal and fencing are not durable.** `Engine::withdraw` sets an in-memory flag (`engine.cpp:1948-1950`), and the fence listener exists only inside the promoted process (`shard.cpp:2119`, `:931`). After either process restarts, the old primary is fenced only through a witness `NO term` reply, which an async primary never asks for. Combined with 5.1 this is a recurring split-brain path in async or no-witness deployments.
- **N7. Seed serving has no fenced or withdrawn check.** `serve_seed` checks only uuid, checkpoint presence and `seed_gate` (`seed.cpp:131-145`), so a withdrawn old primary still hands out checkpoints and its term. A misconfigured `--seed-from` would seed a new standby from a stale lineage. The Sender's crc divergence check would later demand a reseed (`replication.cpp:418-438`), so this is availability, not loss. INFERRED for consequence.
- **N8. Two membership sources for HA.** Witness pairs (`--group`) and placement groups are configured separately. The witness resolves a node's group by uuid when no group is given (`snc_witness.cpp:451-456`). Nothing cross-checks the witness pair against the placement members. VERIFIED absence; INFERRED risk.
- Minor: `maps_` keeps every adopted ClusterMap (`engine.cpp:1618`). Node-link frame epochs are truncated to 32 bits (`node_link.h:39`, `shard.cpp:1938`).

### 7.2 Speculative (with the smallest confirming experiment)

| # | Hypothesis | Smallest experiment |
|---|---|---|
| S1 | Async split brain: two primaries ack concurrently under a partition (5.1) | Async pair plus one witness on netns or `iptables`. Drop old primary ↔ {standby, witness} but keep client ↔ old primary. Write to both, heal, and count lost acked writes. |
| S2 | Stale-read window on a cut-off sync primary is unbounded if the witnesses are reachable only by the new side | Same netns setup in sync mode: write X=2 on the new primary, read X on the old primary, and expect the stale 1 until `replica_timeout_ms` plus witness reachability. |
| S3 | Node links bind group B to a cut-off old primary of A (steering, no term in the hello), so credits are applied there and lost | Two groups, async A. Partition A's old primary from A's standby and witnesses only. Run a cross-group transfer from B to A, fail over A, and check the balances. |
| S4 | F7 plus N5: with k withdrawn primaries and one stopped witness, the witness queue and thread count grow | 3 witnesses, k=4 groups. SIGSTOP one witness, fence all old primaries, and sample `/proc/<w>/status` Threads and READY latency over 60 s. |
| S5 | A placement CAS with a malformed body bricks node starts and refreshes | `PLACEMENT CAS <v> garbage` to a test witness, then start a node with `--control`. |
| S6 | F9 by an operator restoring an older witness state file | Already characterized (PAXOS_BALLOTS C4a); rerun only if the guard changes. |

---

## 8. Implications for a Rust-native design (evidence-based, brief)

**Worth keeping (contracts that carry the safety):**
- **Term fencing at the replica listener, and the term as the HA version domain, separate from the placement epoch** (`replication.cpp:680`; AGENTS inv 12). In addition, make fencing durable and carry the term in every data-plane hello, including node links.
- **The sync ack rule** "durable locally AND standby-acked LSN ≥ lsn". The **readiness proof on the current link**, `link_acked ≥ durable` with a regression latch (`shard.cpp:2761-2764`; `replication.cpp:488-499`). The witness promotes only from READY, with a **causal `after=N`** guard and fresh-version NOT READY (`snc_witness.cpp:458-501`). These are the pieces that made E2/E5/E6 hold.
- **Mutual exclusion of "degrade grant" and "promotion" on one consensus value** (`snc_witness.cpp:852`, `:865`).
- **Move protocol:** CAS states on a linearizable placement record with monotonic move ids, a durable seal before SEALED, a commit gate, exact-arrival activation and proof of arrival, start-barrier re-seal, and node-wide epoch adoption before replay (inv 17) (`migrate.h`, `shard.cpp:3651-3805`, `engine.cpp:1257-1318`).
- **Durable, clock-free links incarnation** (`links_generation.h:1-27`).
- **Witness state-file discipline:** sync before reply, fail-stop, strict loader, exclusive creation (`paxos.h:485-609`).
- **Inv 18** (session answer before any refusal) (`shard.cpp:176-182`).

**Accidental to C++ (do not port as is):**
- A text line protocol with one TCP connection per request, a thread per connection, and a broadcast thread per peer (`ha.h`, `paxos.h:451-462`, `snc_witness.cpp:695-701`). Use framed, authenticated, versioned messages with bounded concurrency and request deadlines (F7).
- A single global proposal mutex across machines, with deadlines that start after the lock (`paxos.h:326-328`).
- Unauthenticated acceptance of `COMMIT` and PREPARE bases, and no cluster id in Paxos (N1).
- Wall-clock ballots without persistence (F8 is safe but needless). Persist (or derive from the promised value) a monotonic counter.
- A pair-only HA model (`Pair host[2]`) beside an N-member placement model. Unify them: derive HA membership from placement, add a mode field (sync/async) to HA state (R08), and add a term guard on PROMOTE.
- In-memory-only fencing (fence listener, withdraw flag), and an async primary that never consults the HA state (N6, 5.1).
- No primary-side read lease. Either add one (the mirror of the standby lease) or define owner reads as linearizable only under a leader lease (gap a).
- Unbounded receive buffering and a wire length trusted before validation (c/N4). Use credit-based flow control between primary and standby.
- The opaque placement body at the consensus layer: validate on CAS.
- Sessions pinned to a slot's original group (gap h). Decide explicitly whether sessions migrate (G-7 UNDECIDED, CORE_CONTRACTS.md:491-497).
