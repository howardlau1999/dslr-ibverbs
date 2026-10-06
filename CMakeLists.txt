cmake_minimum_required(VERSION 3.16)
project(dslr_ibverbs
        VERSION 0.1.0
        DESCRIPTION "DSLR: Decentralized and Starvation-free Lock management with RDMA (SIGMOD'18) on libibverbs"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(DSLR_BUILD_RDMA  "Build the libibverbs transport, server and RDMA tools" ON)
option(DSLR_BUILD_TOOLS "Build command-line tools (server, benchmark, doctor)" ON)
option(DSLR_BUILD_TESTS "Build unit and integration tests (requires GoogleTest)" ON)
option(DSLR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(dslr_warnings INTERFACE)
target_compile_options(dslr_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor
  $<$<BOOL:${DSLR_WARNINGS_AS_ERRORS}>:-Werror>)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# dslr_core: the DSLR algorithm itself. It only depends on the C++ standard
# library; the RDMA verbs are abstracted behind dslr::LockWordAccessor so the
# algorithm can be exercised in-process (tests, --local benchmark) as well.
# ---------------------------------------------------------------------------
add_library(dslr_core STATIC
  src/backoff.cc
  src/clock.cc
  src/config.cc
  src/local_lock_table.cc
  src/lock_session.cc
  src/lock_word.cc
  src/transaction.cc
  src/types.cc)
target_include_directories(dslr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dslr_core PUBLIC Threads::Threads PRIVATE dslr_warnings)
add_library(dslr::core ALIAS dslr_core)

# ---------------------------------------------------------------------------
# dslr_rdma: libibverbs transport (RC queue pairs, one-sided READ/FA/CAS),
# the lock-table server and the client-side LockWordAccessor.
# ---------------------------------------------------------------------------
if(DSLR_BUILD_RDMA)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(IBVERBS IMPORTED_TARGET libibverbs)
  endif()
  if(NOT IBVERBS_FOUND)
    find_library(IBVERBS_LIBRARY NAMES ibverbs REQUIRED)
    find_path(IBVERBS_INCLUDE_DIR NAMES infiniband/verbs.h REQUIRED)
    add_library(PkgConfig::IBVERBS INTERFACE IMPORTED)
    target_link_libraries(PkgConfig::IBVERBS INTERFACE ${IBVERBS_LIBRARY})
    target_include_directories(PkgConfig::IBVERBS INTERFACE ${IBVERBS_INCLUDE_DIR})
  endif()

  add_library(dslr_rdma STATIC
    src/rdma/atomic_probe.cc
    src/rdma/bootstrap.cc
    src/rdma/device.cc
    src/rdma/endpoint.cc
    src/rdma/error.cc
    src/rdma/memory_region.cc
    src/rdma/reliable_connection.cc
    src/lock_table_client.cc
    src/lock_table_server.cc)
  target_link_libraries(dslr_rdma PUBLIC dslr_core PkgConfig::IBVERBS PRIVATE dslr_warnings)
  add_library(dslr::rdma ALIAS dslr_rdma)
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(DSLR_BUILD_TOOLS)
  add_executable(dslr_bench tools/dslr_bench.cc)
  target_link_libraries(dslr_bench PRIVATE dslr_core dslr_warnings)
  if(DSLR_BUILD_RDMA)
    target_compile_definitions(dslr_bench PRIVATE DSLR_HAVE_RDMA=1)
    target_link_libraries(dslr_bench PRIVATE dslr_rdma)

    add_executable(dslr_server tools/dslr_server.cc)
    target_link_libraries(dslr_server PRIVATE dslr_rdma dslr_warnings)

    add_executable(dslr_doctor tools/dslr_doctor.cc)
    target_link_libraries(dslr_doctor PRIVATE dslr_rdma dslr_warnings)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(DSLR_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)

  add_executable(dslr_core_tests
    tests/backoff_test.cc
    tests/config_test.cc
    tests/lock_session_scenario_test.cc
    tests/lock_session_stress_test.cc
    tests/lock_word_test.cc
    tests/transaction_test.cc)
  target_link_libraries(dslr_core_tests PRIVATE dslr_core GTest::gtest GTest::gtest_main dslr_warnings)
  gtest_discover_tests(dslr_core_tests)

  if(DSLR_BUILD_RDMA)
    add_executable(dslr_rdma_tests
      tests/endpoint_test.cc
      tests/rdma_loopback_test.cc)
    target_link_libraries(dslr_rdma_tests PRIVATE dslr_rdma GTest::gtest GTest::gtest_main dslr_warnings)
    gtest_discover_tests(dslr_rdma_tests)
  endif()
endif()
