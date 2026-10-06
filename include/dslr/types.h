#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dslr {

/// Identifies one lock object in the cluster: the lock table (node) hosting it and the index
/// of its 64-bit word inside that table. Lock tables are arrays of 8-byte words, so a LockRef
/// maps directly to an RDMA remote address (`table_base + index * 8`).
struct LockRef {
  uint32_t node = 0;
  uint32_t index = 0;

  friend constexpr bool operator==(const LockRef&, const LockRef&) = default;
};

struct LockRefHash {
  size_t operator()(const LockRef& ref) const noexcept {
    const uint64_t key = (uint64_t{ref.node} << 32) | ref.index;
    // Fibonacci hashing: spreads consecutive indices across buckets.
    return static_cast<size_t>(key * 0x9E3779B97F4A7C15ull);
  }
};

/// Lock modes supported by the core DSLR protocol (Section 4 of the paper).
enum class LockMode : uint8_t {
  Shared,
  Exclusive,
};

/// Outcome of a lock acquisition attempt (the paper's "Success"/"Failure" split into the two
/// different kinds of failure, because the caller must react to them differently).
enum class AcquireStatus : uint8_t {
  /// The lock is held by the caller.
  Acquired,
  /// Algorithm 1 lines 4-9 / 18-23: a counter of the lock object reached COUNT_MAX, so the lock
  /// is frozen until its holder resets it. Our ticket was withdrawn and a random backoff has
  /// already been performed; the acquisition can simply be attempted again. No ticket is held.
  Retry,
  /// Algorithm 2 lines 3-4 and 11-19: a stall (suspected deadlock or failed transaction) was
  /// detected and the lock object was reset, which invalidated our ticket. The paper requires the
  /// transaction to release every lock it holds and restart from the beginning. No ticket is held.
  Aborted,
};

std::string to_string(LockMode mode);
std::string to_string(AcquireStatus status);
std::string to_string(const LockRef& ref);

}  // namespace dslr
