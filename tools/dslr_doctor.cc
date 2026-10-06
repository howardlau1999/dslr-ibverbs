// dslr_doctor: checks that the local RDMA setup can run DSLR.
//
//   dslr_doctor [--device NAME] [--ib-port N] [--gid-index N]
//
// It lists the devices, ports and GIDs, verifies that the device supports atomics and reports
// the byte-order convention of its atomics (see atomic_probe.h), and then runs a loopback
// self-test: a LockTableServer and a LockTableClient in this process, connected through the
// NIC, exercising READ, fetch-and-add and compare-and-swap against values read straight from
// the table memory, followed by a DSLR acquire/release round trip with latency figures.

#include <chrono>
#include <iostream>
#include <thread>

#include "dslr/lock_session.h"
#include "dslr/lock_table_client.h"
#include "dslr/lock_table_server.h"
#include "dslr/rdma/device.h"
#include "dslr/rdma/error.h"

#include "args.h"

namespace {

using Clock = std::chrono::steady_clock;

const char* link_layer_name(uint8_t layer) {
  switch (layer) {
    case IBV_LINK_LAYER_INFINIBAND:
      return "InfiniBand";
    case IBV_LINK_LAYER_ETHERNET:
      return "Ethernet (RoCE)";
    default:
      return "unspecified";
  }
}

const char* atomic_cap_name(ibv_atomic_cap cap) {
  switch (cap) {
    case IBV_ATOMIC_NONE:
      return "none";
    case IBV_ATOMIC_HCA:
      return "HCA (atomic among RDMA operations)";
    case IBV_ATOMIC_GLOB:
      return "global (atomic also with CPU operations)";
  }
  return "unknown";
}

bool check(bool ok, const std::string& what) {
  std::cout << (ok ? "  [ OK ] " : "  [FAIL] ") << what << "\n";
  return ok;
}

int list_devices() {
  const auto names = dslr::rdma::Device::available_devices();
  if (names.empty()) {
    std::cout << "No RDMA devices found.\n"
                 "  - On a machine without an RDMA NIC you can create a Soft-RoCE device:\n"
                 "      sudo modprobe rdma_rxe && sudo rdma link add rxe0 type rxe netdev <eth>\n"
                 "  - The protocol itself can still be exercised with `dslr_bench --local`.\n";
    return 1;
  }
  std::cout << "RDMA devices:\n";
  for (const std::string& name : names) {
    std::cout << "  " << name << "\n";
    try {
      dslr::rdma::DeviceOptions opts;
      opts.name = name;
      dslr::rdma::Device dev(opts);
      const auto& attr = dev.attributes();
      std::cout << "    ports: " << int(attr.phys_port_cnt)
                << ", atomics: " << atomic_cap_name(attr.atomic_cap)
                << ", max outstanding atomics/QP: " << attr.max_qp_rd_atom << "\n";
      if (dev.supports_atomics()) {
        try {
          const dslr::rdma::AtomicByteOrder order = dev.atomic_byte_order();
          std::cout << "    atomic byte order: memory "
                    << (order.memory_big_endian ? "big-endian" : "host") << ", replies "
                    << (order.reply_big_endian ? "big-endian" : "host") << "\n";
        } catch (const std::exception& e) {
          std::cout << "    atomic byte order: probe failed (" << e.what() << ")\n";
        }
      }
      for (uint8_t port = 1; port <= attr.phys_port_cnt; ++port) {
        ibv_port_attr pattr{};
        if (ibv_query_port(dev.context(), port, &pattr) != 0) {
          continue;
        }
        std::cout << "    port " << int(port) << ": " << ibv_port_state_str(pattr.state) << ", "
                  << link_layer_name(pattr.link_layer) << ", lid " << pattr.lid << ", mtu "
                  << (128 << pattr.active_mtu) << "\n";
        for (int i = 0; i < pattr.gid_tbl_len; ++i) {
          ibv_gid_entry entry{};
          if (ibv_query_gid_ex(dev.context(), port, static_cast<uint32_t>(i), &entry, 0) != 0) {
            continue;
          }
          std::cout << "      gid[" << i << "] " << dslr::rdma::to_string(entry.gid)
                    << (entry.gid_type == IBV_GID_TYPE_ROCE_V2   ? " (RoCE v2)"
                        : entry.gid_type == IBV_GID_TYPE_ROCE_V1 ? " (RoCE v1)"
                                                                 : " (IB)")
                    << (i == static_cast<int>(dev.gid_index()) && dev.needs_global_routing()
                            ? "  <- default"
                            : "")
                    << "\n";
        }
      }
    } catch (const std::exception& e) {
      std::cout << "    (cannot open: " << e.what() << ")\n";
    }
  }
  return 0;
}

int self_test(const dslr::rdma::DeviceOptions& device_options) {
  std::cout << "\nLoopback self-test on "
            << (device_options.name.empty() ? "the first device" : device_options.name) << ":\n";
  dslr::ServerOptions server_options;
  server_options.device = device_options;
  server_options.lock_count = 16;
  server_options.bind_address = "127.0.0.1";
  server_options.port = 0;
  dslr::LockTableServer server(server_options);
  server.start();

  dslr::rdma::Device client_device(device_options);
  dslr::LockTableClient client(client_device, {{"127.0.0.1", server.port()}});
  bool ok = check(true, "queue pairs connected (" + client_device.name() + ")");
  const dslr::rdma::AtomicByteOrder order = server.atomic_byte_order();
  ok &= check(true, std::string("atomic byte order probed: memory ") +
                        (order.memory_big_endian ? "big-endian" : "host-order") + ", replies " +
                        (order.reply_big_endian ? "big-endian" : "host-order"));

  const dslr::LockRef ref{0, 3};
  ok &= check(client.read(ref) == 0 && server.peek(3) == dslr::LockWord{},
              "READ of a fresh lock object returns 0");

  // Fetch-and-add on each segment: the previous value comes back and the table shows the sum.
  uint64_t expected = 0;
  for (dslr::Segment seg :
       {dslr::Segment::MaxS, dslr::Segment::MaxX, dslr::Segment::NS, dslr::Segment::NX}) {
    const uint64_t addend = dslr::segment_add(seg, 0x1234);
    const uint64_t prev = client.fetch_add(ref, addend);
    ok &= check(prev == expected,
                "fetch-and-add returns the previous value (" + std::to_string(prev) + ")");
    expected += addend;
  }
  const dslr::LockWord in_memory = server.peek(3);
  ok &= check(in_memory == dslr::LockWord::decode(expected) && client.read(ref) == expected,
              "table memory and READ agree with the expected sum: " + dslr::to_string(in_memory));
  ok &= check(in_memory.nX == 0x1234 && in_memory.maxS == 0x1234,
              "segments land where the lock word layout expects them");

  // Compare-and-swap: a mismatching compare value leaves the word alone and reports it.
  const uint64_t miss = client.compare_swap(ref, expected + 1, 42);
  ok &= check(miss == expected && client.read(ref) == expected,
              "compare-and-swap with wrong compare value fails and returns the current value");
  const uint64_t hit = client.compare_swap(ref, expected, 0);
  ok &= check(hit == expected && client.read(ref) == 0,
              "compare-and-swap with matching compare value resets the word");

  // A quick protocol round trip with latencies.
  dslr::Config config;
  dslr::LockSession session(client, config);
  dslr::HeldLock lock;
  const auto t0 = Clock::now();
  const auto status = session.acquire(ref, dslr::LockMode::Exclusive, lock);
  const auto t1 = Clock::now();
  session.release(lock);
  const auto t2 = Clock::now();
  ok &=
      check(status == dslr::AcquireStatus::Acquired && server.peek(3) == dslr::LockWord{1, 0, 1, 0},
            "DSLR exclusive acquire/release leaves {nX=1, nS=0, maxX=1, maxS=0}");

  constexpr int kRounds = 2000;
  const auto r0 = Clock::now();
  for (int i = 0; i < kRounds; ++i) {
    client.read(ref);
  }
  const auto r1 = Clock::now();
  for (int i = 0; i < kRounds; ++i) {
    client.fetch_add(ref, 0);
  }
  const auto r2 = Clock::now();
  auto us = [](Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); };
  std::cout << "  latency: first acquire " << us(t1 - t0) << " us, release " << us(t2 - t1)
            << " us, READ " << us(r1 - r0) / kRounds << " us, FA " << us(r2 - r1) / kRounds
            << " us (loopback, averaged over " << kRounds << " ops)\n";

  server.stop();
  std::cout
      << (ok ? "\nAll checks passed: this device can run DSLR.\n"
             : "\nSome checks FAILED: do not run DSLR on this device without investigating.\n");
  return ok ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  dslr::tools::Args args(argc, argv);
  if (args.flag("help")) {
    std::cout
        << "Usage: dslr_doctor [--device NAME] [--ib-port N] [--gid-index N] [--no-selftest]\n";
    return 0;
  }
  try {
    if (list_devices() != 0) {
      return 1;
    }
    if (args.flag("no-selftest")) {
      return 0;
    }
    dslr::rdma::DeviceOptions options;
    options.name = args.get("device", "");
    options.port = args.number<uint8_t>("ib-port", 1);
    if (args.has("gid-index")) {
      options.gid_index = args.number<uint32_t>("gid-index", 0);
    }
    return self_test(options);
  } catch (const std::exception& e) {
    std::cerr << "dslr_doctor: " << e.what() << std::endl;
    return 1;
  }
}
