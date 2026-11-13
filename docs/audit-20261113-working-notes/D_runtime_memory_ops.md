# Audit D: runtime, memory/NUMA, observability, operational lifecycle (PR #121, main @ 23f4a0c)
Read-only. No benchmarks run, no build-* touched, tests not executed. Paths relative to /home/solo/SNC-server-OS.

## 1. Runtime interface and app integration -- Partially verified
1. Intent: SRI (include/snc/sri.h:1-5) hides the runtime; native default (AGENTS.md invariant 7), Seastar a measured comparison (docs/SEASTAR.md).
2. Actual: NativeRuntime = one pthread per shard, sched_setaffinity, full mesh of SPSC rings, eventfd+poll idle, per-shard io_uring (src/native/runtime.cpp:440-500). Backend chosen at link time. Seastar backend (src/seastar/runtime.cc, 510 lines) is built only with -DSNC_SEASTAR=ON, and only snc_bench_seastar links it (CMakeLists.txt:38-48). snc_server, tests, and every lab/pilot tool use native only.
3. Refs above; include/snc/native_runtime.h:15-37 (RuntimeConfig); sri.h:46-93 (send/claim/write contract).
4. Evidence: tests/test_runtime.cpp (ring full/refuse; 4 CHECKs; prints pinned but does not assert it, :103). Seastar: bench/results/matrix_seastar.csv on 4-vCPU VM only (docs/SEASTAR.md:36-60). No Seastar test in ctest (CMake: no add_test for it). docs/SEASTAR.md:30-33 says unit tests cannot run on Seastar.
5. Demonstrated: SRI is genuinely link-swappable (the same core compiles on both; SEASTAR.md says so, CMake confirms the shared SNC_CORE_SOURCES :43-47). Native backend works under unit tests.
6. Unverified: Seastar correctness (only benchmarked, not tested; no restart; shards never sleep SEASTAR.md:30-34). sri_progress unsupported on Seastar (native_runtime.h:62-65), so HEALTH decoupling is native-only.
7. Limits: Seastar one runtime per process; the "interface held" claim is compile+benchmark, not conformance-tested.
8. Next: either add a backend-conformance test or mark Seastar as benchmark-only in STATUS.

### 1b. Network server/client/ConnSet -- Partially verified
1. Intent: owner-aware routed TCP front end, port base+shard (include/snc/net.h:1-15, wire.h:1-8).
2. Actual: epoll ConnSet per shard polled from the shard loop (src/net/conn_set.cpp), NetServer, Client with sessions/failover/resend (net.h:98-216), ClusterClient. Wire v0: 48-byte headers, little-endian only (types.h:17 static_assert), kinds EXEC/CREATE/DELETE/TOPOLOGY/CLUSTER/INDEX (wire.h:14-21).
3. Refs: src/net/{conn_set,server,client,cluster_client}.cpp; include/snc/wire.h.
4. Evidence: tests/test_net.cpp (53 CHECKs), test_resend_refusals.cpp, many python process-level tests (test_async_failover.py, test_baseline_*), real-machine lab (docs/REAL_MACHINES.md, 3 Vultr VPS).
5. Demonstrated: owner forwarding, session replay/double-apply fix (STATUS.md:460 cites PR #28), failover ~1.5 s kill, ~7.7-8.5 s partition (lab, small n).
6. Unverified: behaviour under hostile/malformed peers, connection-count limits, slow-reader backpressure on the data port, IPv6, TLS.
7. Limits/risks (verified in code): no authentication, TLS, or ACL anywhere on data port, admin port or control (grep auth|token|TLS|ssl in src/net, tools/*: no hits). IPv4 only (inet_pton AF_INET, conn_set.cpp:49). Listener failures abort the process: die(...) -> std::abort() at conn_set.cpp:23-25, 43-57, and epoll_ctl failure on accept aborts (:86). No cap on accepted connections (accept_all loop :75-90, no limit, no EMFILE handling: accept4 error just returns). Default bind 127.0.0.1 (net.h NetServer::Config), with --control it listens on all interfaces unless --host (snc_server.cpp header comment).
8. Next: fuzz/malformed-frame test; connection cap; auth decision before any non-lab exposure.

### 1c. Admin/control interface -- Partially verified
1. Intent: HEALTH/GAUGES never wait on a shard (tools/admin_loop.h:1-33).
2. Actual: line protocol on --admin-port: HEALTH, GAUGES [n], STATUS, RESOURCES, AUDIT, PROMOTE, MOVE ..., DEBUG * (only with --debug-admin) (snc_server.cpp:1050-1200). Bounded queue/half-open/write deadlines (admin_loop.h:41-48). Control plane = witness processes (tools/snc_witness.cpp, paxos.h, control.h) + snc_ctl (get/init/move/placement backfill; tools/snc_ctl.cpp).
3. Refs: tools/admin_loop.h, tools/snc_server.cpp:1050-1200, tools/ha.h:38 (admin/witness listeners bind INADDR_ANY).
4. Evidence: tests/test_admin_loop.cpp (31 CHECKs), tests/test_gauges_admin.py, test_control.py, test_health.py, test_witness*.py, test_paxos_ballots.py.
5. Demonstrated: HEALTH/GAUGES answer while a shard is stalled (T8c, test_gauges_admin.py header).
6. Unverified: security; operation under admin-port abuse beyond bounded counters.
7. Risk (verified): admin listener binds INADDR_ANY (ha.h:38) and PROMOTE/MOVE are unauthenticated plain text (snc_server.cpp:1197-1200) -> anyone reachable can promote or move slots. Lab-only posture.
8. Next: bind admin to a configured address; document trust boundary.

### 1d. App-developer integration -- Implemented, unverified (as a developer surface)
1. Intent: "persistent application objects as executable state"; spec sec 2/6 type+op registration.
2. Actual: the only app-facing API is the in-process C++ API in include/snc/snc.h (execute/create/create_near/remove/index_lookup/prefetch; OpCtx) plus include/snc/registry.h TypeDescriptor/OpDescriptor with function pointers. Types are compiled in and registered before start (registry.h:1-3). The single concrete type is Account (src/types/account.cpp:346 register_type; include/snc/types/account.h). snc_server hardwires account::register_type() (tools/snc_server.cpp:545); there is no dlopen/plugin loader (grep dlopen: none), no IDL/schema file, no client SDK beyond net::Client/ClusterClient that speak raw op ids + byte args. A new application type requires editing and rebuilding snc_server (or writing a new main). User type ids start at 256 (registry.h:79; registry.cpp:17 dies if reserved).
3. Refs as above.
4. Evidence: account type tests (test_execute, test_transfer, test_proof...). All against Account. test of a second user-defined type: none found (tests use Account; Unknown whether any test registers a synthetic type; I did not find one).
5. Demonstrated: one type with create/delete ops, index, follow-up emit/resume, transfers.
6. Unverified: that the descriptor API generalizes to a second, differently shaped type; handler time budget (20 us) not measured (STATUS.md:490).
7. Schema evolution: none. A manifest type catalog mismatch of schema_ver or name refuses to start (src/core/engine.cpp:1011-1015); STATUS.md:484-486 confirms only additive new types are accepted. No upcast/migration hook.
8. Next: add a second toy type test through the public descriptor API; define schema-version story; decide whether apps are linked in or loaded.

## 2. Memory, NUMA, affinity, hugepages, allocator -- Partially verified
1. Intent: pinned worker, shard-local allocator, first-touch NUMA-local memory, selected structures on huge pages (include/snc/allocator.h:1-8, AGENTS.md invariant 4).
2. Actual:
 - Affinity: worker pins itself with sched_setaffinity to cfg.cpus[i % n] or the i-th allowed CPU (runtime.cpp:445-465). Failure is silent: topo.pinned = (rc==0); no log, no abort (:464). snc_server never reads/reports topology().pinned (grep pinned in tools: none); only snc_bench prints it (bench/snc_bench.cpp:494). Pinning to a cpu list entry of -1 when allowed set empty is not handled specially (CPU_SET(-1) UB if allowed mask empty; edge).
 - NUMA: no mbind/set_mempolicy/libnuma calls anywhere (grep numa in src include tools: only getcpu node recorded, runtime.cpp:466-470, and mi_manage_os_memory_ex numa_node=-1 at allocator.cpp:25). Locality = pin-then-first-touch (allocator created after pinning, runtime.cpp:472) plus external `numactl --physcpubind --membind` used by bench/pilot.py:512 and the R640 procedure (R640_TOPOLOGY.md:271). numa_node is recorded but never consulted by any decision (grep: no readers outside seastar/native assignment).
 - Rings: inbound rings allocated by the consumer (runtime.cpp:495-498, `new Ring`), plain operator new, so consumer-thread first-touch only; not NUMA-bound.
 - Hugepages: map_pages (src/core/pages.cpp:25-65): HUGETLB mmap, silent fallback to THP then madvise; abort if mmap fails. Per-shard mimalloc exclusive arena from heap_reserve_bytes sized 512 B/object, min 256 MiB (engine.cpp:535-541); overflow spills to a non-arena heap (allocator.cpp:55-60, counted arena_misses). Default PageMode::SMALL (native_runtime.h:25); server flag --pages (snc_server.cpp:521).
 - Memory limit: ShardAllocator enforces limit_bytes -> nullptr -> E_NO_MEMORY (allocator.cpp:44-48), but heap_limit_bytes is never set by snc_server, bench or any test (grep: only native_runtime.h:23, runtime.cpp:472, seastar:247). So the cap is dead in all shipped paths.
 - Worker-thread helper: replication disk-reader thread resets affinity to all CPUs and nice 10 (replication.cpp:121-128), i.e. deliberately off-shard threads exist.
3. Refs above. 4. Evidence: tests/test_pages.cpp (17 CHECKs: map_pages, local map, arena spill THP; HUGETLB accepted as THP fallback :20, so hugetlb path is not proven to have been exercised); R640 measurements in docs/R640_TOPOLOGY.md (PILOT-L vs PILOT-X are named configs; PILOT-X results not located in this review => Unknown). docs/HUGEPAGES.md measured only on a 1-node 4-vCPU VM and says NUMA "Not measured here" (:84-90).
5. Demonstrated: huge pages give 3-10% in-memory on a VM (HUGEPAGES.md:51); THP/hugetlb equal within noise.
6. Unverified: any NUMA-local placement guarantee. Nothing in code verifies where pages land (no move_pages/numa_maps check, no gauge). Correctness of locality on R640 depends on operator wrapping with numactl and on kernel numa_balancing=0 (R640_TOPOLOGY.md:126,179).
7. Risks: silent pinning failure; silent HUGETLB->THP->4k downgrade is only visible in ShardStats (docs claim; runtime does not warn); arena never unmapped, "one Engine per process" (allocator.cpp:33-34); heap_reserve is a heuristic of 512 B/object.
8. Next: log/refuse when pinned=false or page mode downgraded (server startup); expose numa_node and pinned in HEALTH/GAUGES; if NUMA locality is to be a product claim, add mbind/numa_maps verification.
Classification split: pinning/first-touch/hugepage arena = Partially verified (measured on VM + R640 pilot config, not asserted by tests); NUMA enforcement in code = Planned (only recorded); shard memory limit = Implemented, unverified.

## 3. Observability -- Partially verified
1. Intent: inexpensive observability with no reader waiting on shards (gauges.h:1-15; admin_loop.h:30-33 "no metrics subsystem").
2. Actual: per-shard published atomic gauge blocks with seqlock (include/snc/gauges.h, 455 lines), GAUGES/HEALTH/STATUS/RESOURCES admin verbs; SNC_TRACE=1 env stopgap writing lines to stderr (include/snc/trace.h:21-26). Proposed ring-buffer tracing design is explicitly NOT started (docs/TRACING.md:1-9, :162 "Next steps (not started)"). No Prometheus/metrics endpoint, no structured logs, no log levels; server output is printf to stdout.
3. Refs: include/snc/gauges.h; include/snc/trace.h; tools/snc_server.cpp:1050-1200.
4. Evidence: tests/test_gauges.cpp (110 CHECKs), test_gauges_admin.py (T8a-d incl. responsiveness under a stalled shard), test_admin_loop.cpp (31).
5. Demonstrated: gauge consistency contract (seq), keys stable/ordered, answers while a shard is stalled.
6. Unverified: trace-disabled cost "INFERRED (not measured)" (TRACING.md:20-21). Publish overhead of gauges (s0_overhead.py exists in bench/; result not reviewed here => Unknown). Ring/NUMA/pinned state not in gauges.
7. Limits: gauge names are mirrored by hand between gauges.h kGaugeNames and tests (test_gauges_admin.py:"Must match") -- drift is caught only by that test.
8. Next: add pinned/numa/page-mode to HEALTH; decide on metrics export.

## 4. Failure handling and operational lifecycle -- Partially verified
1. Intent: correctness before benchmark wins; crash recovery, failover, fencing (AGENTS.md inv. 8).
2. Actual: snc_server (1517 lines) parses ~50 flags (snc_server.cpp:251-530); no config file, only flags plus a few env vars (SNC_TRACE, SNC_FOLLOWUP_WINDOW, SNC_EXCHANGE_BUDGET per test header). Signals: SIGINT/SIGTERM blocked in all threads, main thread sigtimedwait; SIGUSR1 promotes a standby (:739-745, :1295-1330). Shutdown order: stop admin, join sampler, wait moves, eng.stop(), print stats (:1480-1490). SIGHUP not handled (default action terminates, no reload). Unknown flag exits 2 (:529-531). Many fatal paths std::abort()/storage_fatal (shard.cpp aborts at 1224,1421,1452,1651,1752; conn_set die()). Control-plane/witness failover documented and tested in python process tests.
3. Refs as cited.
4. Evidence: test_crash.cpp, test_persistence.cpp, test_lifecycle.cpp, test_wal_capacity.cpp, test_replication.cpp, 20+ python process tests wired in CMakeLists.txt:76-122; lab runs in docs/REAL_MACHINES.md.
5. Demonstrated: crash recovery, replication/failover, fail-closed migration at test scale and in a 3-VPS lab (small n).
6. Unverified: disk-full / EIO / ENOMEM in the long run; fd exhaustion; clock jumps; operator restarts under load. docs/STATUS.md:393 "FULL EXPERIMENTAL BASELINE: NOT YET FROZEN".
7. Deployment artifacts found: none. No systemd unit, Dockerfile, packaging, logrotate, install target (grep systemd|Dockerfile|.service: only bench manifests/docs mention). Only bench/lab/vultr_setup.sh (lab host provisioning) and bench/manifest.sh. No `install()` in CMakeLists.txt.
8. CI: none. No .github, no .gitlab-ci, no scripts/ dir (ls). Builds present locally: build-asan (Debug, -fsanitize=address,undefined), build-diag/pilot/s6 (RelWithDebInfo, no sanitizer), build-preS1 (no CMakeCache => not a usable build dir). README documents a TSAN build (README sanitizer section) but no build-tsan exists in the tree -- whether TSAN has ever been run is Unknown.
 Tests registered: 28 C++ tests via foreach (CMakeLists.txt:99) + ~21 python tests inside `if(PYTHON3)`. Several python tests (test_pilot, test_s6, test_profile_check) take no BUILD arg and are not about the server.
Next: add CI that at least builds + runs ctest and an asan run; a unit file and config doc; startup warning policy.

### 4b. test_pilot.py:366 red on main -- verified cause
- tests/test_pilot.py:365-366 reads bench/cp1a_frozen.json and asserts `frozen["go_record"] is None`. bench/cp1a_frozen.json now has go_record = "docs/handoff/CP1A_R640_GO_20261111.md" (set by commit 2027874 "File CP-1A GO record", merged in b2948a3 PR #110). So the assertion fails by design of the stale test, not due to a product bug. Line 370 (`any("go_record is not set" in x ...)`) would also fail afterwards, since the committed freeze file now has a GO record (inferred from file content; test not run).
- Trap: bench/pilot.py:265-269 POST_FREEZE_ALLOWED = docs/, AGENTS.md, bench/cp1a_frozen.json, bench/results/ only. tests/test_pilot.py:344-345 itself asserts that a change to tests/test_pilot.py after runner_commit is refused ("changed after runner_commit"). Repairing the red test therefore changes a frozen path; the freeze interlock would refuse a real pilot from a HEAD that includes the fix unless runner_commit is re-chosen. (Verified from reading both files; the interaction with freeze_problems was read at pilot.py:276-296, not executed.)
- Also test_pilot.py:361-362 earlier lines (quick gate) are unaffected.
Also note: _status text in cp1a_frozen.json still says "NOT FROZEN, NO GO" while runner_commit and go_record are set -> stale status string.

## Deployability gap list (each checked by looking)
- Packaging/install target, systemd unit, container image: absent.
- CI pipeline: absent (no .github, no scripts). Regression protection is manual; test_pilot red on main shows it.
- Config file / env-based config: absent; ~50 CLI flags only.
- AuthN/AuthZ/TLS on data, admin, witness/control ports: absent; admin and witness listeners on INADDR_ANY (ha.h:38).
- IPv6: absent. Connection limits: absent.
- Application loading: types compiled into snc_server (snc_server.cpp:545); no plugin/IDL/schema registry; schema change refuses start (engine.cpp:1011).
- Client SDK/language bindings: only C++ net::Client taking raw op ids and byte args; no codegen.
- Metrics export/log pipeline: absent (stdout printf, SNC_TRACE stderr stopgap; ring tracing not started).
- Startup self-checks (pinning, hugepage, NUMA, THP, numa_balancing, governor): only pilot.py checks (pilot_plan.md:94); the server does not.
- Per-shard memory limit wired to server: absent (heap_limit_bytes never set).
- Config reload (SIGHUP), rolling upgrade/mixed-version: x9 mixed-version test exists by name (CMakeLists.txt:~113) but is not in default ctest; Unknown beyond that.
- Backup/restore tooling, WAL/segment disk-space alarms: not found (test_wal_capacity.cpp covers engine behaviour; no operator tool).
- Unit conventions for multi-node bring-up: snc_ctl init + witnesses documented in tool headers only; no runbook beyond docs/CLUSTER.md, docs/CONTROL_PLANE.md (not read in depth).

## Cross-cutting hazards found (verified in code)
1. Silent pin failure: runtime.cpp:464 records pinned=false without any log; server never surfaces it (tools/snc_server.cpp has no `pinned`).
2. NUMA locality not enforced in code: no mbind/policy anywhere; mi_manage_os_memory_ex numa_node=-1 (allocator.cpp:25); depends on external numactl and first-touch.
3. Silent hugepage downgrade HUGETLB->THP->4k: pages.cpp:47-61; test accepts the fallback (tests/test_pages.cpp:20).
4. Per-shard heap limit is dead code in all shipped binaries (heap_limit_bytes never assigned).
5. Unauthenticated PROMOTE/MOVE on an INADDR_ANY admin port (ha.h:38; snc_server.cpp:1197).
6. Network front end can abort the process on epoll_ctl failure and has no connection cap (conn_set.cpp:75-90).
7. Stale test + freeze interlock interaction: test_pilot.py:366 vs bench/cp1a_frozen.json; fixing the test touches a path pilot.py forbids post-freeze (test_pilot.py:344, pilot.py:265).
8. Schema change = refuse to start, no migration (engine.cpp:1011-1015).
9. One Engine per process (mimalloc arenas never freed; allocator.cpp:33-34); also one Seastar runtime per process.
10. Unknown: whether TSAN has been run (no build-tsan); whether PILOT-X (cross-NUMA) data exists; second user type test; s0_overhead result.
