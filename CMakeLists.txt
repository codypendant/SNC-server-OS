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
  src/core/wal_format.cpp
  src/net/conn_set.cpp
  src/net/server.cpp
  src/net/client.cpp
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
foreach(t test_object_id test_local_map test_pages test_runtime test_execute test_persistence test_crash test_net test_transfer)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE snc_core)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

add_executable(snc_bench bench/snc_bench.cpp)
target_link_libraries(snc_bench PRIVATE snc_core)

add_executable(baseline_bench bench/baseline_bench.cpp)
target_link_libraries(baseline_bench PRIVATE snc_core)

add_executable(snc_server tools/snc_server.cpp)
target_link_libraries(snc_server PRIVATE snc_core)

add_executable(baseline_server bench/baseline_server.cpp)
target_link_libraries(baseline_server PRIVATE snc_core)

add_executable(net_bench bench/net_bench.cpp)
target_link_libraries(net_bench PRIVATE snc_core)
