#pragma once

#include <infiniband/verbs.h>

#include <cstddef>
#include <cstdint>

#include "dslr/rdma/device.h"

namespace dslr::rdma {

/// A registered memory region: pins `[addr, addr + length)` and makes it addressable by the
/// NIC with the given IBV_ACCESS_* flags. The caller keeps ownership of the memory, which must
/// outlive the region.
class MemoryRegion {
 public:
  MemoryRegion(const Device& device, void* addr, size_t length, unsigned access_flags);
  ~MemoryRegion();

  MemoryRegion(const MemoryRegion&) = delete;
  MemoryRegion& operator=(const MemoryRegion&) = delete;

  uint32_t lkey() const { return mr_->lkey; }
  uint32_t rkey() const { return mr_->rkey; }
  uint64_t address() const { return reinterpret_cast<uint64_t>(mr_->addr); }
  size_t length() const { return mr_->length; }

 private:
  ibv_mr* mr_;
};

}  // namespace dslr::rdma
