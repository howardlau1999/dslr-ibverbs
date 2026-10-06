#include "dslr/rdma/atomic_probe.h"

#include <atomic>
#include <cstdio>
#include <string>

#include "dslr/rdma/error.h"
#include "dslr/rdma/memory_region.h"
#include "dslr/rdma/reliable_connection.h"

namespace dslr::rdma {
namespace {

// Every byte differs, so any byte-order mix-up changes the value in a recognisable way.
constexpr uint64_t kPattern = 0x0102030405060708ull;

std::string hex(uint64_t value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(value));
  return buf;
}

}  // namespace

AtomicByteOrder probe_atomic_byte_order(const Device& device) {
  alignas(8) uint64_t word = kPattern;
  MemoryRegion region(device, &word, sizeof(word),
                      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);

  // Two queue pairs on the same port, connected to each other: the NIC processes the request
  // exactly as it would one coming from another node.
  ReliableConnection requester(device);
  ReliableConnection responder(device);
  requester.connect(responder.local_endpoint());
  responder.connect(requester.local_endpoint());

  const uint64_t reply = requester.fetch_add(region.address(), region.rkey(), 1);
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint64_t memory = *static_cast<volatile uint64_t*>(&word);

  AtomicByteOrder order;
  // The responder either added 1 to the host-order integer in memory, or to the integer it
  // obtained by reading the bytes as big-endian (and wrote that one back big-endian).
  if (memory == kPattern + 1) {
    order.memory_big_endian = false;
  } else if (memory == htobe64(be64toh(kPattern) + 1)) {
    order.memory_big_endian = true;
  } else {
    throw RdmaError("atomic probe on " + device.name() + ": fetch-and-add of 1 turned " +
                    hex(kPattern) + " into " + hex(memory) + ", which matches no known convention");
  }
  // The reply carries the value before the add, either as a host-order integer or as the raw
  // big-endian wire encoding.
  const uint64_t logical_before = order.memory_big_endian ? be64toh(kPattern) : kPattern;
  if (reply == logical_before) {
    order.reply_big_endian = false;
  } else if (reply == htobe64(logical_before)) {
    order.reply_big_endian = true;
  } else {
    throw RdmaError("atomic probe on " + device.name() + ": fetch-and-add replied " + hex(reply) +
                    " for a previous value of " + hex(logical_before) +
                    ", which matches no known convention");
  }

  // Cross-check with a compare-and-swap of the logical value back to the original pattern.
  const uint64_t logical_after = logical_before + 1;
  const uint64_t cas_reply = from_nic(
      requester.compare_swap(region.address(), region.rkey(), logical_after, logical_before),
      order.reply_big_endian);
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint64_t restored = *static_cast<volatile uint64_t*>(&word);
  if (cas_reply != logical_after || restored != kPattern) {
    throw RdmaError("atomic probe on " + device.name() + ": compare-and-swap misbehaved (reply " +
                    hex(cas_reply) + ", memory " + hex(restored) + ")");
  }
  return order;
}

}  // namespace dslr::rdma
