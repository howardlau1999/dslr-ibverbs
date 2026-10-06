#include "dslr/rdma/memory_region.h"

#include "dslr/rdma/error.h"

namespace dslr::rdma {

MemoryRegion::MemoryRegion(const Device& device, void* addr, size_t length, unsigned access_flags)
    : mr_(ibv_reg_mr(device.pd(), addr, length, access_flags)) {
  if (mr_ == nullptr) {
    throw_errno("ibv_reg_mr(" + std::to_string(length) + " bytes)");
  }
}

MemoryRegion::~MemoryRegion() {
  ibv_dereg_mr(mr_);
}

}  // namespace dslr::rdma
