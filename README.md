# dslr-ibverbs

A C++20 implementation of **DSLR — Decentralized and Starvation-free Lock management with
RDMA** (Yoon, Chowdhury, Mozafari; SIGMOD 2018) on top of `libibverbs`.

DSLR is a distributed lock manager in which lock tables live in the memory of their nodes and
every lock/unlock is a *single one-sided RDMA atomic* executed by the NIC: no server CPU, no
message exchange, no blind compare-and-swap retries. It adapts Lamport's bakery algorithm to a
64-bit lock word `{nX, nS, maxX, maxS}`: a transaction draws a ticket with fetch-and-add and
waits (with RDMA READs) until the "now serving" counters reach it, which makes lock scheduling
first-come-first-served and therefore starvation-free. Leases detect failed or deadlocked
transactions, and counters are reset before they can overflow.

```
      63      48 47      32 31      16 15       0
     +----------+----------+----------+----------+
  L  |    nX    |    nS    |   maxX   |   maxS   |     FA(L, maxX, 1) -> exclusive ticket
     +----------+----------+----------+----------+     FA(L, maxS, 1) -> shared ticket
       now serving (X / S)   next ticket (X / S)       FA(L, nX|nS, 1) -> release
```

## What is in the box

| Component | Where | Notes |
|---|---|---|
| The DSLR protocol (Algorithms 1–3, counter resets, multi-slot leasing) | `include/dslr/lock_session.h`, `src/lock_session.cc` | written against a 3-operation `LockWordAccessor` interface (FA / CAS / READ) |
| In-process lock table | `local_lock_table.h` | same semantics as RDMA, for tests, `dslr_bench --local`, single-node use |
| ibverbs transport | `include/dslr/rdma/*` | device/PD, memory regions, RC queue pairs with synchronous one-sided verbs, TCP bootstrap, atomic byte-order probe |
| Lock table server / client | `lock_table_server.h`, `lock_table_client.h` | server exports its table once per client; afterwards its CPU is idle |
| `Transaction` helper | `transaction.h` | releases everything on abort, as the paper requires |
| Tools | `tools/` | `dslr_server`, `dslr_bench` (RDMA or `--local`), `dslr_doctor` (device check + loopback self-test) |
| Tests | `tests/` | unit, scripted scenario, multi-threaded stress/fault-injection, RDMA loopback (auto-skipped without a device) |
| Docs | `docs/algorithm.md`, `docs/rdma-layer.md` | pseudocode-to-code mapping, deviations and why, RDMA details |

## Building

Requirements: Linux, a C++20 compiler (GCC 12 / Clang 15 or newer), CMake ≥ 3.16,
`libibverbs-dev` (rdma-core), and GoogleTest for the tests.

```sh
sudo apt install build-essential cmake ninja-build libibverbs-dev libgtest-dev   # Debian/Ubuntu
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake options: `DSLR_BUILD_RDMA` (default ON; OFF builds only the protocol and the local
table), `DSLR_BUILD_TOOLS`, `DSLR_BUILD_TESTS`, `DSLR_WARNINGS_AS_ERRORS`.

## Quick start

Check the RDMA setup of a machine (lists devices and GIDs, probes the atomic byte order, runs a
loopback acquire/release and prints latencies):

```sh
./build/dslr_doctor [--device mlx5_0] [--gid-index 3]
```

Run a lock table on each server node, then benchmark from any node:

```sh
# on node1 and node2
./build/dslr_server --device mlx5_0 --locks 1000000 --port 7777

# on a client machine: 8 worker threads, 2 locks per transaction, Zipf-skewed access
./build/dslr_bench --servers node1:7777,node2:7777 --device mlx5_0 \
                   --threads 8 --locks-per-txn 2 --zipf 0.99 --duration 10s
```

Without RDMA hardware, the protocol can still be exercised in-process:

```sh
./build/dslr_bench --local --threads 4 --locks 1000 --zipf 0.99 --locks-per-txn 2
./build/dslr_bench --local --locks 8 --locks-per-txn 3 --no-sort --lease 2ms --max-poll 1ms  # deadlocks
./build/dslr_bench --local --locks 16 --fail-rate 0.01 --lease 2ms --max-poll 1ms            # crashes
./build/dslr_bench --local --locks 4 --count-max 16                                           # resets
```

A machine without an RDMA NIC can usually create a Soft-RoCE device for real end-to-end runs
(`sudo modprobe rdma_rxe && sudo rdma link add rxe0 type rxe netdev eth0`); see
`docs/rdma-layer.md`.

## Using the library

```cpp
#include "dslr/lock_table_client.h"
#include "dslr/lock_session.h"
#include "dslr/transaction.h"

dslr::rdma::Device device({.name = "mlx5_0"});
dslr::LockTableClient client(device, {{"node1", 7777}, {"node2", 7777}});  // one per thread
dslr::LockSession session(client, dslr::Config{});                          // one per thread

for (;;) {
  dslr::Transaction txn(session);
  if (!txn.lock({.node = 0, .index = 42}, dslr::LockMode::Shared) ||
      !txn.lock({.node = 1, .index = 7}, dslr::LockMode::Exclusive)) {
    continue;  // DSLR aborted us (suspected deadlock/failure); all locks are released, retry
  }
  // ... critical section, shorter than Config::lease (10 ms by default) ...
  if (txn.commit()) break;  // false: a lease expired and the ticket was revoked; redo the work
}
```

`LockSession::acquire()` and `release()` give access to the individual steps; `SessionStats`
reports what the protocol did (immediate grants, polls, stall skips, counter resets, ...).
`Config` holds the paper's tunables: lease (10 ms), stall threshold (2× lease), polling unit
ω (5 µs), backoff parameters (10 µs / 10 ms), `COUNT_MAX` (2^15) and the multi-slot leasing
limit.

## Project layout

```
include/dslr/            public headers (protocol, local table, transaction, rdma/)
src/                     implementation
tools/                   dslr_server, dslr_bench, dslr_doctor
tests/                   GoogleTest suites
docs/algorithm.md        the protocol, line by line, and where this code deviates from the paper
docs/rdma-layer.md       ibverbs details: queue pairs, bootstrap, byte order, Soft-RoCE testing
```

## Status and testing

* The protocol core is covered by scripted scenario tests for every branch of Algorithms 1–3
  (immediate grants, waiting, FCFS order, stall skips by readers and writers, lease expiry,
  stray releases, counter resets by the designated resetter, forced resets of frozen objects,
  multi-slot leasing) and by multi-threaded stress tests with a lease-aware mutual-exclusion
  checker, including crashed transactions, deadlocks and tiny `COUNT_MAX` values. They pass
  under ThreadSanitizer and AddressSanitizer/UBSan.
* The ibverbs layer is exercised by `tests/rdma_loopback_test.cc` and `dslr_doctor` on any
  machine with an RDMA device (a Soft-RoCE device is enough). It has been run on Mellanox
  ConnectX NICs (mlx5, RoCE v2): `dslr_doctor`, the loopback tests (also under
  AddressSanitizer/UBSan), `dslr_server` + `dslr_bench` on one and two lock table servers with
  up to 8 client threads under Zipf contention, deadlock-prone and crashing workloads, and the
  failure paths — a client killed while holding locks (the server drops it and later clients
  recover the locks), a server killed under load (clients report the dead peer after ~4 s), a
  handshake that cannot connect (reported on both sides at once), and a peer that accepts but
  never answers. Run `dslr_doctor` first on new hardware: it also probes the atomic byte order.
* Not implemented: the update-lock extension sketched in Section 5.2 of the paper (six
  10-bit counters). The core protocol, counter resets, lease-based failure/deadlock handling,
  random backoff and multi-slot leasing (Section 5.1) are implemented.
