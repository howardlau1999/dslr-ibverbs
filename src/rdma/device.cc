#include "dslr/rdma/device.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "dslr/rdma/atomic_probe.h"
#include "dslr/rdma/error.h"

namespace dslr::rdma {
namespace {

bool is_ipv4_mapped(const ibv_gid& gid) {
  static constexpr uint8_t kPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  return std::memcmp(gid.raw, kPrefix, sizeof(kPrefix)) == 0;
}

bool is_zero(const ibv_gid& gid) {
  return std::all_of(std::begin(gid.raw), std::end(gid.raw), [](uint8_t b) { return b == 0; });
}

}  // namespace

std::vector<std::string> Device::available_devices() {
  int count = 0;
  ibv_device** list = ibv_get_device_list(&count);
  if (list == nullptr) {
    throw_errno("ibv_get_device_list");
  }
  std::vector<std::string> names;
  for (int i = 0; i < count; ++i) {
    names.emplace_back(ibv_get_device_name(list[i]));
  }
  ibv_free_device_list(list);
  return names;
}

Device::Device(const DeviceOptions& options) : port_(options.port) {
  int count = 0;
  ibv_device** list = ibv_get_device_list(&count);
  if (list == nullptr) {
    throw_errno("ibv_get_device_list");
  }
  ibv_device* chosen = nullptr;
  for (int i = 0; i < count && chosen == nullptr; ++i) {
    if (options.name.empty() || options.name == ibv_get_device_name(list[i])) {
      chosen = list[i];
    }
  }
  if (chosen == nullptr) {
    ibv_free_device_list(list);
    throw RdmaError(count == 0 ? "no RDMA devices found (is an RDMA NIC or Soft-RoCE configured?)"
                               : "RDMA device not found: " + options.name);
  }
  context_ = ibv_open_device(chosen);
  ibv_free_device_list(list);
  if (context_ == nullptr) {
    throw_errno("ibv_open_device");
  }

  try {
    if (ibv_query_device(context_, &device_attr_) != 0) {
      throw_errno("ibv_query_device");
    }
    if (port_ == 0 || port_ > device_attr_.phys_port_cnt) {
      throw RdmaError("device " + name() + " has no port " + std::to_string(port_));
    }
    if (ibv_query_port(context_, port_, &port_attr_) != 0) {
      throw_errno("ibv_query_port");
    }
    if (port_attr_.state != IBV_PORT_ACTIVE) {
      throw RdmaError("port " + std::to_string(port_) + " of " + name() + " is not active (" +
                      ibv_port_state_str(port_attr_.state) + ")");
    }
    choose_gid(options.gid_index);
    pd_ = ibv_alloc_pd(context_);
    if (pd_ == nullptr) {
      throw_errno("ibv_alloc_pd");
    }
  } catch (...) {
    ibv_close_device(context_);
    throw;
  }
}

Device::~Device() {
  if (pd_ != nullptr) {
    // EBUSY means queue pairs or memory regions created on this device are still alive. They
    // hold a dangling reference to us from now on and will crash when destroyed; say so here,
    // where the cause is, rather than in ibv_destroy_qp() later.
    if (const int rc = ibv_dealloc_pd(pd_); rc != 0) {
      std::fprintf(stderr,
                   "dslr::rdma::Device(%s) destroyed while queue pairs or memory regions still "
                   "use it (ibv_dealloc_pd: %s); destroy connections and regions before the "
                   "device\n",
                   name().c_str(), std::strerror(rc));
    }
  }
  if (context_ != nullptr) {
    ibv_close_device(context_);
  }
}

std::string Device::name() const {
  return ibv_get_device_name(context_->device);
}

uint32_t Device::max_initiator_depth() const {
  return static_cast<uint32_t>(std::max(1, device_attr_.max_qp_init_rd_atom));
}

uint32_t Device::max_responder_depth() const {
  return static_cast<uint32_t>(std::max(1, device_attr_.max_qp_rd_atom));
}

const AtomicByteOrder& Device::atomic_byte_order() const {
  std::lock_guard<std::mutex> guard(probe_mutex_);
  if (!atomic_byte_order_.has_value()) {
    atomic_byte_order_ = probe_atomic_byte_order(*this);
  }
  return *atomic_byte_order_;
}

void Device::choose_gid(std::optional<uint32_t> requested) {
  if (requested.has_value()) {
    gid_index_ = *requested;
    gid_index_requested_ = true;
    if (ibv_query_gid(context_, port_, static_cast<int>(gid_index_), &gid_) != 0) {
      throw_errno("ibv_query_gid(" + std::to_string(gid_index_) + ")");
    }
    if (is_zero(gid_)) {
      throw RdmaError("GID index " + std::to_string(gid_index_) + " is empty");
    }
    return;
  }

  if (port_attr_.link_layer != IBV_LINK_LAYER_ETHERNET) {
    // InfiniBand: LID routing. Record the port GID anyway so the peer can display it.
    gid_index_ = 0;
    ibv_query_gid(context_, port_, 0, &gid_);
    return;
  }

  // RoCE: a RoCE v2 GID is routable and the usual choice; prefer an IPv4-mapped one because
  // that is how the peers will have configured their addresses in practice.
  std::vector<ibv_gid_entry> entries(static_cast<size_t>(std::max(1, port_attr_.gid_tbl_len)) *
                                     device_attr_.phys_port_cnt);
  const ssize_t found = ibv_query_gid_table(context_, entries.data(), entries.size(), 0);
  int best_score = -1;
  if (found > 0) {
    for (ssize_t i = 0; i < found; ++i) {
      const ibv_gid_entry& e = entries[static_cast<size_t>(i)];
      if (e.port_num != port_ || is_zero(e.gid)) {
        continue;
      }
      const int score =
          (e.gid_type == IBV_GID_TYPE_ROCE_V2 ? 2 : 0) + (is_ipv4_mapped(e.gid) ? 1 : 0);
      if (score > best_score) {
        best_score = score;
        gid_index_ = e.gid_index;
        gid_ = e.gid;
      }
    }
  } else {
    // Older kernels without the GID table query: take the first populated entry.
    for (int i = 0; i < port_attr_.gid_tbl_len; ++i) {
      ibv_gid candidate{};
      if (ibv_query_gid(context_, port_, i, &candidate) == 0 && !is_zero(candidate)) {
        gid_index_ = static_cast<uint32_t>(i);
        gid_ = candidate;
        best_score = 0;
        break;
      }
    }
  }
  if (best_score < 0) {
    throw RdmaError("no usable GID on port " + std::to_string(port_) + " of " + name());
  }
}

std::string to_string(const ibv_gid& gid) {
  char buf[48];
  std::snprintf(buf, sizeof(buf),
                "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                gid.raw[0], gid.raw[1], gid.raw[2], gid.raw[3], gid.raw[4], gid.raw[5], gid.raw[6],
                gid.raw[7], gid.raw[8], gid.raw[9], gid.raw[10], gid.raw[11], gid.raw[12],
                gid.raw[13], gid.raw[14], gid.raw[15]);
  return buf;
}

}  // namespace dslr::rdma
