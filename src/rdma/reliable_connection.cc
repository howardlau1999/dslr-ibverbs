#include "dslr/rdma/reliable_connection.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <random>

#include "dslr/clock.h"
#include "dslr/rdma/error.h"

namespace dslr::rdma {
namespace {

// We never have more than one work request outstanding, but give the queues some slack so a
// provider that reports completions late cannot overflow them.
constexpr int kQueueDepth = 16;

constexpr int kAccessFlags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                             IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

uint32_t random_psn() {
  std::random_device rd;
  return rd() & 0xFFFFFFu;  // PSNs are 24 bits wide
}

}  // namespace

ReliableConnection::ReliableConnection(const Device& device, ConnectionOptions options)
    : device_(device), options_(options), initial_psn_(random_psn()) {
  if (!device.supports_atomics()) {
    throw RdmaError("device " + device.name() + " does not support RDMA atomics");
  }
  try {
    cq_ = ibv_create_cq(device.context(), kQueueDepth, nullptr, nullptr, 0);
    if (cq_ == nullptr) {
      throw_errno("ibv_create_cq");
    }

    ibv_qp_init_attr init{};
    init.send_cq = cq_;
    init.recv_cq = cq_;
    init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = kQueueDepth;
    init.cap.max_recv_wr = 1;  // never used: all traffic is one-sided
    init.cap.max_send_sge = 1;
    init.cap.max_recv_sge = 1;
    init.sq_sig_all = 0;
    qp_ = ibv_create_qp(device.pd(), &init);
    if (qp_ == nullptr) {
      throw_errno("ibv_create_qp");
    }

    result_region_ = std::make_unique<MemoryRegion>(device, &result_slot_, sizeof(result_slot_),
                                                    IBV_ACCESS_LOCAL_WRITE);

    // RESET -> INIT. The access flags are what the *peer* may do to memory through this queue
    // pair; servers need REMOTE_READ and REMOTE_ATOMIC for the lock table, and using the same
    // flags everywhere keeps both ends symmetric.
    ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = device.port();
    attr.qp_access_flags = kAccessFlags;
    if (ibv_modify_qp(qp_, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) {
      throw_errno("ibv_modify_qp(INIT)");
    }
  } catch (...) {
    destroy();
    throw;
  }
}

ReliableConnection::~ReliableConnection() {
  destroy();
}

void ReliableConnection::destroy() noexcept {
  if (qp_ != nullptr) {
    ibv_destroy_qp(qp_);
    qp_ = nullptr;
  }
  result_region_.reset();
  if (cq_ != nullptr) {
    ibv_destroy_cq(cq_);
    cq_ = nullptr;
  }
}

EndpointInfo ReliableConnection::local_endpoint(const MemoryRegion* exported_table,
                                                uint32_t table_locks, bool table_big_endian) const {
  EndpointInfo info;
  info.qp_num = qp_->qp_num;
  info.psn = initial_psn_;
  info.lid = device_.lid();
  std::memcpy(info.gid.data(), device_.gid().raw, info.gid.size());
  if (exported_table != nullptr) {
    info.table_addr = exported_table->address();
    info.table_rkey = exported_table->rkey();
    info.table_locks = table_locks;
    info.table_big_endian = table_big_endian;
  }
  return info;
}

void ReliableConnection::connect(const EndpointInfo& remote) {
  if (connected_) {
    throw RdmaError("queue pair is already connected");
  }
  const uint32_t depth = std::min(
      {options_.atomic_depth, device_.max_initiator_depth(), device_.max_responder_depth()});

  // INIT -> RTR: where to send to, and how many incoming READ/atomic requests we serve.
  ibv_qp_attr rtr{};
  rtr.qp_state = IBV_QPS_RTR;
  // Only 8-byte payloads travel over these queue pairs; a conservative MTU works everywhere.
  rtr.path_mtu = std::min(device_.port_attributes().active_mtu, IBV_MTU_1024);
  rtr.dest_qp_num = remote.qp_num;
  rtr.rq_psn = remote.psn;
  rtr.max_dest_rd_atomic = static_cast<uint8_t>(depth);
  rtr.min_rnr_timer = 12;
  rtr.ah_attr.dlid = remote.lid;
  rtr.ah_attr.sl = 0;
  rtr.ah_attr.src_path_bits = 0;
  rtr.ah_attr.port_num = device_.port();
  if (device_.needs_global_routing()) {
    rtr.ah_attr.is_global = 1;
    std::memcpy(rtr.ah_attr.grh.dgid.raw, remote.gid.data(), remote.gid.size());
    rtr.ah_attr.grh.sgid_index = static_cast<uint8_t>(device_.gid_index());
    rtr.ah_attr.grh.hop_limit = 64;
    rtr.ah_attr.grh.traffic_class = 0;
    rtr.ah_attr.grh.flow_label = 0;
  }
  if (ibv_modify_qp(qp_, &rtr,
                    IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) != 0) {
    throw_errno("ibv_modify_qp(RTR)");
  }

  // RTR -> RTS: our own send-side parameters.
  ibv_qp_attr rts{};
  rts.qp_state = IBV_QPS_RTS;
  rts.timeout = options_.ack_timeout;
  rts.retry_cnt = options_.retry_count;
  rts.rnr_retry = options_.rnr_retry;
  rts.sq_psn = initial_psn_;
  rts.max_rd_atomic = static_cast<uint8_t>(depth);
  if (ibv_modify_qp(qp_, &rts,
                    IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                        IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC) != 0) {
    throw_errno("ibv_modify_qp(RTS)");
  }
  connected_ = true;
}

uint64_t ReliableConnection::fetch_add(uint64_t remote_addr, uint32_t rkey, uint64_t addend) {
  ibv_send_wr wr{};
  wr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
  wr.wr.atomic.remote_addr = remote_addr;
  wr.wr.atomic.rkey = rkey;
  wr.wr.atomic.compare_add = addend;
  return execute(wr);
}

uint64_t ReliableConnection::compare_swap(uint64_t remote_addr, uint32_t rkey, uint64_t expected,
                                          uint64_t desired) {
  ibv_send_wr wr{};
  wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
  wr.wr.atomic.remote_addr = remote_addr;
  wr.wr.atomic.rkey = rkey;
  wr.wr.atomic.compare_add = expected;
  wr.wr.atomic.swap = desired;
  return execute(wr);
}

uint64_t ReliableConnection::read_u64(uint64_t remote_addr, uint32_t rkey) {
  ibv_send_wr wr{};
  wr.opcode = IBV_WR_RDMA_READ;
  wr.wr.rdma.remote_addr = remote_addr;
  wr.wr.rdma.rkey = rkey;
  return execute(wr);
}

uint64_t ReliableConnection::execute(ibv_send_wr& wr) {
  if (!connected_) {
    throw RdmaError("queue pair is not connected");
  }
  const uint64_t remote_addr =
      wr.opcode == IBV_WR_RDMA_READ ? wr.wr.rdma.remote_addr : wr.wr.atomic.remote_addr;
  if (remote_addr % sizeof(uint64_t) != 0) {
    throw RdmaError("remote address is not 8-byte aligned");
  }

  ibv_sge sge{};
  sge.addr = reinterpret_cast<uint64_t>(&result_slot_);
  sge.length = sizeof(result_slot_);
  sge.lkey = result_region_->lkey();
  wr.wr_id = ++next_wr_id_;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;

  ibv_send_wr* bad = nullptr;
  if (const int rc = ibv_post_send(qp_, &wr, &bad); rc != 0) {
    connected_ = false;
    throw_errno("ibv_post_send", rc);
  }

  // Busy-poll for the completion: lock operations are latency-critical and exactly one request
  // is in flight, so the first completion is ours. The deadline turns a silently dead peer or a
  // broken fabric into an error instead of a hang (RC retries normally report it earlier).
  ibv_wc wc{};
  const auto deadline = Clock::now() + options_.completion_timeout;
  for (unsigned spins = 0;; ++spins) {
    const int polled = ibv_poll_cq(cq_, 1, &wc);
    if (polled < 0) {
      connected_ = false;
      throw_errno("ibv_poll_cq");
    }
    if (polled == 1) {
      break;
    }
    if ((spins & 0x3FF) == 0 && Clock::now() > deadline) {
      connected_ = false;
      throw RdmaError("timed out waiting for an RDMA completion (peer unreachable?)");
    }
  }
  if (wc.status != IBV_WC_SUCCESS) {
    connected_ = false;
    throw RdmaError(std::string("RDMA operation failed: ") + ibv_wc_status_str(wc.status));
  }
  if (wc.wr_id != next_wr_id_) {
    connected_ = false;
    throw RdmaError("unexpected work completion (wr_id mismatch)");
  }

  // The provider's poll routine contains the DMA-visibility barrier; the fence and volatile read
  // keep the compiler from reusing a stale copy of the result slot.
  std::atomic_thread_fence(std::memory_order_acquire);
  return *static_cast<volatile uint64_t*>(&result_slot_);
}

}  // namespace dslr::rdma
