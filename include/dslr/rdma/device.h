#pragma once

#include <infiniband/verbs.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dslr::rdma {

/// How a NIC represents 64-bit atomic data; see probe_atomic_byte_order() in atomic_probe.h.
struct AtomicByteOrder {
  /// The responder interprets the target word in memory as a big-endian integer.
  bool memory_big_endian = false;
  /// The requester delivers atomic replies as big-endian integers.
  bool reply_big_endian = false;
};

struct DeviceOptions {
  /// ibverbs device name, e.g. "mlx5_0" or "rxe0". Empty selects the first device found.
  std::string name;
  /// Physical port, 1-based.
  uint8_t port = 1;
  /// GID table index to use for RoCE addressing. By default the first RoCE v2 entry is chosen,
  /// preferring an IPv4-mapped one; InfiniBand ports are addressed by LID and ignore this
  /// unless set.
  std::optional<uint32_t> gid_index;
};

/// An opened RDMA device: the ibv_context, a protection domain shared by all queue pairs and
/// memory regions of this process, and the attributes of the chosen port.
///
/// A Device must outlive every ReliableConnection, MemoryRegion, LockTableClient and
/// LockTableServer created on it: they keep a reference to it and their verbs objects live in
/// its protection domain. Declare the device first (or hold it in a longer-lived scope).
class Device {
 public:
  explicit Device(const DeviceOptions& options = {});
  ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  ibv_context* context() const { return context_; }
  ibv_pd* pd() const { return pd_; }
  uint8_t port() const { return port_; }
  const ibv_device_attr& attributes() const { return device_attr_; }
  const ibv_port_attr& port_attributes() const { return port_attr_; }
  uint16_t lid() const { return port_attr_.lid; }
  const ibv_gid& gid() const { return gid_; }
  uint32_t gid_index() const { return gid_index_; }
  std::string name() const;

  /// RoCE ports (Ethernet link layer) have no LIDs: packets must carry a global route header
  /// addressed by GID. InfiniBand ports within one subnet are addressed by LID.
  bool needs_global_routing() const {
    return port_attr_.link_layer == IBV_LINK_LAYER_ETHERNET || gid_index_requested_;
  }

  /// DSLR is built on fetch-and-add and compare-and-swap; a device without them is useless.
  bool supports_atomics() const { return device_attr_.atomic_cap != IBV_ATOMIC_NONE; }

  /// Largest number of outstanding RDMA READ/atomic operations a queue pair may have as the
  /// initiator and as the responder.
  uint32_t max_initiator_depth() const;
  uint32_t max_responder_depth() const;

  /// Byte-order conventions of this device's atomics, determined with a loopback probe the
  /// first time it is asked for (see atomic_probe.h). Throws RdmaError if the probe fails.
  const AtomicByteOrder& atomic_byte_order() const;

  static std::vector<std::string> available_devices();

 private:
  void choose_gid(std::optional<uint32_t> requested);

  ibv_context* context_ = nullptr;
  ibv_pd* pd_ = nullptr;
  ibv_device_attr device_attr_{};
  ibv_port_attr port_attr_{};
  uint8_t port_;
  ibv_gid gid_{};
  uint32_t gid_index_ = 0;
  bool gid_index_requested_ = false;
  mutable std::mutex probe_mutex_;
  mutable std::optional<AtomicByteOrder> atomic_byte_order_;
};

/// "fe80::..." style rendering of a GID for logs.
std::string to_string(const ibv_gid& gid);

}  // namespace dslr::rdma
