cmake_minimum_required(VERSION 3.20)
project(snc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Threads REQUIRED)
find_library(MIMALLOC_LIB mimalloc REQUIRED)
find_library(XXHASH_LIB xxhash REQUIRED)
find_library(URING_LIB uring REQUIRED)

add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)

set(SNC_CORE_SOURCES
  src/core/object_id.cpp
  src/core/allocator.cpp
  src/core/pages.cpp
  src/core/registry.cpp
  src/core/shard.cpp
  src/core/engine.cpp
  src/core/storage.cpp
  src/core/replication.cpp
  src/core/seed.cpp
  src/core/session.cpp
  src/core/slot_state.cpp
  src/core/cluster.cpp
  src/core/placement.cpp
  src/core/node_link.cpp
  src/core/wal_format.cpp
  src/net/conn_set.cpp
  src/net/server.cpp
  src/net/client.cpp
  src/net/cluster_client.cpp
  src/types/account.cpp)

add_library(snc_core ${SNC_CORE_SOURCES} src/native/runtime.cpp)
target_include_directories(snc_core PUBLIC include)
target_link_libraries(snc_core PUBLIC Threads::Threads ${MIMALLOC_LIB} ${XXHASH_LIB} ${URING_LIB})

# The same core on the Seastar backend (docs/SEASTAR.md). Off by default:
# needs a Seastar build, e.g. -DSNC_SEASTAR=ON -DCMAKE_PREFIX_PATH=<seastar prefix>.
option(SNC_SEASTAR "Build snc_bench_seastar on the Seastar runtime backend" OFF)
if(SNC_SEASTAR)
  find_package(Seastar REQUIRED)
  add_library(snc_core_seastar ${SNC_CORE_SOURCES} src/seastar/runtime.cc)
  target_include_directories(snc_core_seastar PUBLIC include)
  target_link_libraries(snc_core_seastar PUBLIC Seastar::seastar Threads::Threads ${MIMALLOC_LIB}
                        ${XXHASH_LIB} ${URING_LIB})
  set_target_properties(snc_core_seastar PROPERTIES CXX_STANDARD 23)
  add_executable(snc_bench_seastar bench/snc_bench.cpp)
  target_link_libraries(snc_bench_seastar PRIVATE snc_core_seastar)
  set_target_properties(snc_bench_seastar PROPERTIES CXX_STANDARD 23)
endif()

enable_testing()
foreach(t test_rate_schedule test_object_id test_local_map test_pages test_runtime test_execute test_persistence test_crash test_net test_transfer test_replication test_cluster test_migrate test_resend_refusals test_proof test_placement test_stage_chain test_gauges test_s1_admission test_s2_pacing test_s3_writeback test_lifecycle test_s4_sync_tokens test_admin_loop test_wal_capacity test_intake)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE snc_core)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
# A hung S3 test fails the suite instead of stalling it (a CHECK failure
# exits with the engine running, which can hang under a sanitizer).
set_tests_properties(test_s3_writeback test_lifecycle test_s4_sync_tokens PROPERTIES TIMEOUT 2400)

add_executable(snc_bench bench/snc_bench.cpp)
target_link_libraries(snc_bench PRIVATE snc_core)

add_executable(baseline_bench bench/baseline_bench.cpp)
target_link_libraries(baseline_bench PRIVATE snc_core)

add_executable(snc_server tools/snc_server.cpp)
target_link_libraries(snc_server PRIVATE snc_core)
add_executable(snc_ctl tools/snc_ctl.cpp)
target_link_libraries(snc_ctl PRIVATE snc_core)
add_executable(snc_witness tools/snc_witness.cpp)
target_link_libraries(snc_witness PRIVATE Threads::Threads)
target_include_directories(snc_witness PRIVATE ${CMAKE_SOURCE_DIR}/include)
find_program(PYTHON3 python3)
if(PYTHON3)
  add_test(NAME test_witness COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_witness.py ${CMAKE_BINARY_DIR})
  add_test(NAME test_async_failover COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_async_failover.py ${CMAKE_BINARY_DIR})
  add_test(NAME test_control COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_control.py ${CMAKE_BINARY_DIR})
  add_test(NAME test_health COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_health.py ${CMAKE_BINARY_DIR})
  add_test(NAME test_gauges_admin COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_gauges_admin.py ${CMAKE_BINARY_DIR})
  add_test(NAME test_fail_closed COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_fail_closed.py ${CMAKE_BINARY_DIR})
  add_test(NAME test_s1 COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_s1.py ${CMAKE_BINARY_DIR})
  # Pre-baseline readiness (docs/EXPERIMENTAL_BASELINE.md section 5): the
  # scenarios of the fixed code (B1: q3_runtime; B2: q5_catchup_race and
  # test_sync_readiness; B3: b3_stalled_catchup). The long ones (q1_blocked,
  # q4_delayed*, q3_runtime20) run by name.
  add_test(NAME test_profile_check COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_profile_check.py)
  add_test(NAME test_baseline_retry COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_baseline_retry.py
           ${CMAKE_BINARY_DIR} q1_kill q1_stop q1_stall q5_kill_kill q5_catchup_race b3_stalled_catchup r11_guard)
  add_test(NAME test_sync_readiness COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_sync_readiness.py
           ${CMAKE_BINARY_DIR})
  add_test(NAME test_baseline_move COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_baseline_move.py
           ${CMAKE_BINARY_DIR} q2_lost_move q10_sealed q10_committed)
  add_test(NAME test_baseline_ha COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_baseline_ha.py
           ${CMAKE_BINARY_DIR} h1_measurement q3_runtime q3_restart q4_normal)
  # The causal order of NOT READY and READY (docs/SYNC_READINESS.md 7).
  add_test(NAME test_ready_order COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_ready_order.py
           ${CMAKE_BINARY_DIR})
  # F8: Paxos ballots after a witness restart (docs/PAXOS_BALLOTS.md).
  add_test(NAME test_paxos_ballots COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_paxos_ballots.py
           ${CMAKE_BINARY_DIR})
  # Witness hardening before the baseline freeze (docs/PAXOS_BALLOTS.md 9).
  add_test(NAME test_witness_hardening COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_witness_hardening.py
           ${CMAKE_BINARY_DIR})
  # G2 at the lab's campaign boundaries (docs/PAXOS_BALLOTS.md 9.1).
  add_test(NAME test_lab_witness_guard COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_lab_witness_guard.py
           ${CMAKE_BINARY_DIR})
  add_test(NAME test_index_wire COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_index_wire.py ${CMAKE_BINARY_DIR})
  add_test(NAME test_pilot COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_pilot.py)
  # net_bench --rate (open loop) and --mix (PR #61 review).
  add_test(NAME test_net_bench_rate COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_net_bench_rate.py
           ${CMAKE_BINARY_DIR})
endif()

add_executable(baseline_server bench/baseline_server.cpp)
target_link_libraries(baseline_server PRIVATE snc_core)

add_executable(net_bench bench/net_bench.cpp)
target_link_libraries(net_bench PRIVATE snc_core)

# The lab's load and checker (docs/REAL_MACHINES.md).
add_executable(stage_stall bench/stage_stall.cpp)
target_link_libraries(stage_stall PRIVATE snc_core)

add_executable(snc_load bench/snc_load.cpp)
target_link_libraries(snc_load PRIVATE snc_core)

# Checkpoint walk cost, for the S2 pacing defaults (docs/S2_PACING.md 5).
add_executable(ckpt_walk_cost bench/ckpt_walk_cost.cpp)
target_link_libraries(ckpt_walk_cost PRIVATE snc_core)
# S3-D writeback micro-characterization (docs/S3D_WRITEBACK.md).
add_executable(ckpt_writeback_char bench/ckpt_writeback_char.cpp)
target_link_libraries(ckpt_writeback_char PRIVATE Threads::Threads)
