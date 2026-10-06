#pragma once

#include <cstdint>

#include "dslr/types.h"

namespace dslr {

/// The only three operations DSLR needs from the network, mirroring the one-sided RDMA verbs
/// used by the paper (Table 1). The lock manager is written against this interface so that the
/// same algorithm runs over real RDMA (dslr::LockTableClient) or on in-process atomics
/// (dslr::LocalLockTable) for tests and single-node deployments.
///
/// Semantics match the InfiniBand verbs: all three operate on a single 8-byte word, are atomic
/// with respect to each other, and return the value the word had *before* the operation.
class LockWordAccessor {
 public:
  virtual ~LockWordAccessor() = default;

  /// RDMA fetch-and-add: `word += addend`, returns the previous value. Always succeeds.
  virtual uint64_t fetch_add(LockRef ref, uint64_t addend) = 0;

  /// RDMA compare-and-swap: `if (word == expected) word = desired`, returns the previous value.
  /// The swap took place iff the returned value equals `expected`.
  virtual uint64_t compare_swap(LockRef ref, uint64_t expected, uint64_t desired) = 0;

  /// RDMA READ of the word.
  virtual uint64_t read(LockRef ref) = 0;
};

}  // namespace dslr
