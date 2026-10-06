#pragma once

#include <endian.h>

#include <cstdint>

#include "dslr/rdma/device.h"

namespace dslr::rdma {

/// Determines empirically how a device handles 64-bit RDMA atomics, using a loopback probe:
/// two queue pairs on `device` are connected to each other, a fetch-and-add is performed on a
/// local word with a known bit pattern, and the resulting memory contents and reply are
/// compared against the two possible conventions.
///
/// Modern NICs (mlx5 ConnectX-4 and later, Soft-RoCE) treat the target word and the reply as
/// host-order integers. Older NICs (ConnectX-3) interpret the target word as a big-endian
/// integer and deliver the reply in big-endian as well; without correction, a host-order
/// counter incremented by such a NIC turns into garbage. RDMA READ always copies raw bytes, so
/// the memory convention of the *server's* device decides how READ results must be
/// interpreted, while the reply convention of the *client's* device applies to atomic results.
///
/// Throws RdmaError if the device behaves in neither way.
AtomicByteOrder probe_atomic_byte_order(const Device& device);

/// Converts a raw 64-bit value obtained from the NIC into the logical value, given whether it
/// was produced under the big-endian convention.
inline uint64_t from_nic(uint64_t raw, bool big_endian) {
  return big_endian ? be64toh(raw) : raw;
}

}  // namespace dslr::rdma
