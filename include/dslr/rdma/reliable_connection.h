#pragma once

#include <infiniband/verbs.h>

#include <chrono>
#include <cstdint>
#include <memory>

#include "dslr/rdma/device.h"
#include "dslr/rdma/endpoint.h"
#include "dslr/rdma/memory_region.h"

namespace dslr::rdma {

struct ConnectionOptions {
  /// Outstanding RDMA READ/atomic operations allowed per queue pair (clamped to the device
  /// limits). DSLR issues one operation at a time, so a small value is plenty.
  uint32_t atomic_depth = 16;
  /// IB "local ACK timeout" exponent: nominally 4.096 us * 2^value per attempt (14 ~ 67 ms).
  /// Real NICs round this up: mlx5 (ConnectX) in RoCE mode was measured to wait ~0.5 s per
  /// attempt for any value <= 16 and twice the nominal time above that.
  uint8_t ack_timeout = 14;
  /// Transport retries before the queue pair goes into error, i.e. before a dead peer is
  /// reported as a failed completion (IBV_WC_RETRY_EXC_ERR). The paper detects node failures
  /// the same way (Section 4.7). With the mlx5 behaviour above, 7 retries report a dead lock
  /// table server after ~4 s.
  uint8_t retry_count = 7;
  uint8_t rnr_retry = 7;
  /// How long to spin on the completion queue before declaring the connection dead. This is a
  /// last resort for a NIC that never completes the request at all; keep it well above
  /// (retry_count + 1) * effective ACK timeout so the RC error is the one that gets reported.
  std::chrono::milliseconds completion_timeout{10000};
};

/// One reliable-connected (RC) queue pair with its own completion queue, driven synchronously:
/// each one-sided operation posts a single signaled work request and busy-polls for its
/// completion. RC is the only transport that supports RDMA atomics (Section 2.1.1).
///
/// A ReliableConnection is bound to the thread using it; ibverbs queue pairs are not thread-safe
/// and the lock protocol never has more than one operation in flight per transaction.
class ReliableConnection {
 public:
  ReliableConnection(const Device& device, ConnectionOptions options = {});
  ~ReliableConnection();

  ReliableConnection(const ReliableConnection&) = delete;
  ReliableConnection& operator=(const ReliableConnection&) = delete;

  /// What the peer needs to know to connect to this queue pair. Servers pass their lock table so
  /// clients learn its address, rkey, size and byte-order convention.
  EndpointInfo local_endpoint(const MemoryRegion* exported_table = nullptr,
                              uint32_t table_locks = 0, bool table_big_endian = false) const;

  /// Moves the queue pair INIT -> RTR -> RTS towards `remote`. Must be called exactly once.
  void connect(const EndpointInfo& remote);
  bool connected() const { return connected_; }
  uint32_t qp_num() const { return qp_->qp_num; }

  /// One-sided operations on an 8-byte word in the peer's memory. All return the value the word
  /// had before the operation (for READ: its current value). `remote_addr` must be 8-byte
  /// aligned, as required for RDMA atomics. Throw RdmaError on failure, after which the
  /// connection is unusable.
  uint64_t fetch_add(uint64_t remote_addr, uint32_t rkey, uint64_t addend);
  uint64_t compare_swap(uint64_t remote_addr, uint32_t rkey, uint64_t expected, uint64_t desired);
  uint64_t read_u64(uint64_t remote_addr, uint32_t rkey);

 private:
  uint64_t execute(ibv_send_wr& wr);
  void destroy() noexcept;

  const Device& device_;
  ConnectionOptions options_;
  ibv_cq* cq_ = nullptr;
  ibv_qp* qp_ = nullptr;
  uint32_t initial_psn_ = 0;
  uint64_t next_wr_id_ = 0;
  bool connected_ = false;

  /// Landing zone for the 8-byte result of every operation. The NIC writes it via DMA, hence the
  /// dedicated memory region. It lives inside the object, which is therefore not movable.
  alignas(8) uint64_t result_slot_ = 0;
  std::unique_ptr<MemoryRegion> result_region_;
};

}  // namespace dslr::rdma
