# The RDMA layer

Everything under `include/dslr/rdma/` is a thin, purpose-built wrapper around `libibverbs`. It
knows exactly one kind of traffic — 8-byte one-sided operations on lock words — and keeps the
protocol code free of verbs details.

## Pieces

| class | file | role |
|---|---|---|
| `Device` | `device.h` | opens an ibverbs device, allocates the protection domain, selects port and GID, probes the atomic byte order |
| `MemoryRegion` | `memory_region.h` | registers a buffer for RDMA (lock table on servers, result slot on every connection) |
| `ReliableConnection` | `reliable_connection.h` | one RC queue pair with its own completion queue; synchronous `fetch_add`, `compare_swap`, `read_u64` |
| `EndpointInfo` + bootstrap | `endpoint.h`, `bootstrap.h` | out-of-band exchange of QP number, PSN, LID/GID and lock table (address, rkey, size, byte order) over TCP |
| `probe_atomic_byte_order` | `atomic_probe.h` | loopback experiment determining how the NIC represents atomic data |
| `LockTableServer` | `../lock_table_server.h` | hosts a lock table and bootstraps queue pairs for clients |
| `LockTableClient` | `../lock_table_client.h` | the `LockWordAccessor` used by `LockSession`: one queue pair per server |

## Why RC, and why synchronous

RDMA atomics exist only on the Reliable Connection transport (paper, Section 2.1.1), so every
client holds one RC queue pair per lock table server. DSLR issues the operations of a
transaction strictly one after another (draw a ticket, then read until served, then release),
so each operation is posted as a single signaled work request and its completion is
busy-polled. Latency is what matters for a lock manager; busy polling avoids the wake-up
latency of completion channels. A per-operation deadline (`ConnectionOptions::completion_timeout`)
turns a dead fabric into an exception instead of a hang; RC retry exhaustion
(`IBV_WC_RETRY_EXC_ERR`) normally reports a dead peer earlier, which is also how the paper
detects node failures. How much earlier depends on the NIC: the IB spec defines the local ACK
timeout as `4.096 us * 2^timeout`, but mlx5 (ConnectX) in RoCE mode was measured to wait about
0.5 s per attempt for any `timeout <= 16`, so the defaults (`timeout = 14`, `retry_cnt = 7`)
report a crashed lock table server after roughly 4 s. Every later operation on that queue pair
fails immediately; the owner has to reconnect.

A `Device` must outlive every connection, memory region, client and server created on it (their
verbs objects live in its protection domain). Destroying it first leaves `ibv_dealloc_pd` with
`EBUSY`; the destructor reports that on stderr, and the dangling connections crash later.

Queue pairs are not thread-safe, so a `ReliableConnection`, a `LockTableClient` and a
`LockSession` all belong to one thread. Create one set per worker thread; they may share a
`Device`.

## Queue pair setup

`ReliableConnection` creates the QP in the constructor and moves it to `INIT` with access flags
`LOCAL_WRITE | REMOTE_READ | REMOTE_WRITE | REMOTE_ATOMIC` (servers need the remote flags for
the lock table; using the same flags on both ends keeps things symmetric). `connect()` then
performs `INIT → RTR → RTS` with the peer's `EndpointInfo`:

* RTR: destination QP number and PSN, path MTU `min(active_mtu, 1024)` (payloads are 8 bytes),
  `max_dest_rd_atomic`, `min_rnr_timer = 12`, and the address vector — LID-based on InfiniBand,
  GID-based with a GRH (`is_global = 1`, `hop_limit = 64`) on RoCE.
* RTS: `timeout = 14`, `retry_cnt = 7`, `rnr_retry = 7`, our PSN, `max_rd_atomic`.

The outstanding-atomics depth is `min(ConnectionOptions::atomic_depth, device limits)`; the
protocol only ever has one operation in flight per queue pair anyway.

### GID selection on RoCE

RoCE ports have no LIDs; packets are addressed by GID. Unless `DeviceOptions::gid_index` is set,
`Device` picks the first RoCE v2 entry of the port, preferring an IPv4-mapped one
(`::ffff:a.b.c.d`), via `ibv_query_gid_table` (falling back to probing indices with
`ibv_query_gid` on old kernels). `dslr_doctor` lists the GID table so you can override the
choice when several networks are configured.

## Bootstrap

Servers listen on TCP (`ServerOptions::port`, default 7777). A client connects, sends its
`EndpointInfo`, and the server answers with its own plus the lock table (virtual address, rkey,
number of 8-byte lock objects, byte-order flag). Both sides then move their queue pair to RTS;
the server does so before answering, so the client's first operation can never arrive at a
queue pair that is not ready. The frame is `"DSLR"`, a 16-bit protocol version, and a fixed
43-byte big-endian body.

The TCP connection stays open as a liveness signal: when a client disappears, `recv()` on the
server returns and the client's queue pair is destroyed; TCP keepalive probes (10 s idle, 5 s
interval, 3 probes) stand in for the periodic heartbeats the paper uses to detect node
failures.

A handshake can fail after the TCP connection is up: the frame is malformed, or the server
cannot connect its queue pair to the client's address (`ibv_modify_qp(RTR)` fails with
`ETIMEDOUT`/`EHOSTUNREACH` when the kernel cannot resolve the client's GID — typically the two
sides use GIDs on different networks). The server then shuts the TCP connection down at once
and reports the reason through `ServerOptions::on_handshake_error` (`dslr_server` logs it); the
client sees "connection closed by peer" naming the server. The client's connect timeout also
applies to the handshake itself as a socket send/receive timeout, so a foreign process that
accepts the port but never answers is reported instead of waited for.

## Byte order of atomics — read this before running on real hardware

RDMA READ copies raw bytes. For atomics, however, the NIC interprets the 8-byte target as an
integer, adds/compares, and returns the previous value — and *which* integer it sees depends
on the device generation:

* modern NICs (mlx5 / ConnectX-4 and later, Soft-RoCE `rxe`) treat the word in memory and the
  reply as **host-order** integers;
* older NICs (mlx4 / ConnectX-3, the hardware used in the paper) treat the word in memory as a
  **big-endian** integer and deliver the reply in big-endian as well.

A host-order counter incremented by a big-endian NIC turns into garbage, silently. Rather than
assuming, `Device::atomic_byte_order()` *measures* the convention once per process with a
loopback experiment (`probe_atomic_byte_order`): two queue pairs on the device are connected to
each other, a fetch-and-add of 1 is applied to a word holding `0x0102030405060708`, and the
resulting memory contents and reply identify the convention (or an unknown one, which is
reported as an error). The result is applied in exactly two places:

* `LockTableClient` byte-swaps atomic replies when *its* NIC delivers them big-endian, and READ
  results when the *server's* NIC stores lock words big-endian (the server advertises this flag
  in its `EndpointInfo`);
* `LockTableServer::peek` byte-swaps when reading its own table for monitoring.

Operands (`compare_add`, `swap`) are always logical host-order integers; the provider encodes
them. The protocol code never sees anything but logical values.

`dslr_doctor` prints the probed convention and runs the same checks against values read straight
from the table memory.

## Testing without an RDMA NIC

Linux ships a software RoCE implementation that is fully compatible with this code and
supports atomics:

```sh
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev eth0      # any Ethernet interface
ibv_devinfo                                        # should list rxe0 (link_layer: Ethernet)
./build/dslr_doctor                                # probe + loopback self-test
ctest --test-dir build -R Rdma --output-on-failure # RDMA loopback tests
```

Soft-RoCE latencies are tens of microseconds, so use a longer `--lease` than the 10 ms default
only if your critical sections are long; the RDMA tests use 50 ms. Note that `rdma_rxe` cannot
be loaded on hosts whose InfiniBand core stack comes from an out-of-tree OFED/DOCA package (the
in-tree module is rejected with "Invalid argument" / symbol version mismatch); use a VM with a
stock kernel or a machine with a real NIC in that case.

## Known limitations

* One queue pair per (thread, server). A deployment with hundreds of threads and nodes may want
  shared queue pairs with a lock, or dynamically connected transport; neither is needed for the
  protocol itself.
* The bootstrap listener is plain TCP without authentication; run it on a trusted network, as
  the paper assumes for the RDMA fabric as well (advisory locking, Section 2.2.2).
* The lock table size is fixed at server start; a directory service mapping data items to
  `(node, index)` is outside the scope, as in the paper (Section 4.1).
