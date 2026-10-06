#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dslr::rdma {

/// Everything one side must learn about the other, out of band, before an RC queue pair can be
/// connected and a lock table addressed: queue pair number, initial packet sequence number,
/// LID/GID for addressing, and (servers only) the exported lock table.
struct EndpointInfo {
  uint32_t qp_num = 0;
  uint32_t psn = 0;
  uint16_t lid = 0;
  std::array<uint8_t, 16> gid{};
  uint64_t table_addr = 0;   ///< virtual address of the lock table on the server (0 for clients)
  uint32_t table_rkey = 0;   ///< remote key of the lock table's memory region
  uint32_t table_locks = 0;  ///< number of 8-byte lock objects in the table
  /// Set when the server's NIC treats lock words in memory as big-endian integers (see
  /// atomic_probe.h); clients must then byte-swap RDMA READ results.
  bool table_big_endian = false;

  /// Fixed-size big-endian encoding used on the bootstrap TCP connection.
  static constexpr size_t kWireSize = 4 + 4 + 2 + 16 + 8 + 4 + 4 + 1;
  std::array<uint8_t, kWireSize> to_wire() const;
  static EndpointInfo from_wire(const std::array<uint8_t, kWireSize>& bytes);

  std::string describe() const;
};

}  // namespace dslr::rdma
