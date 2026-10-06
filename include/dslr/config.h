#pragma once

#include <chrono>
#include <cstdint>

namespace dslr {

/// Tunables of the DSLR protocol. Defaults follow the values used in the paper (Sections 4.1,
/// 4.6, 4.9 and Appendix A.3). Call validate() after changing anything; LockSession does so too.
struct Config {
  using Duration = std::chrono::microseconds;

  /// Lease: the longest time a well-behaved transaction may hold a lock (per slot, see
  /// max_slots). A holder whose lease expired must not release the lock any more because a
  /// waiter may already have skipped its ticket (Algorithm 3 line 1). Paper default: 10 ms.
  Duration lease{10'000};

  /// Stall detection threshold as a multiple of the lease: when the "now serving" counters of a
  /// lock object do not change for `stall_multiplier * lease`, the transaction in front is
  /// considered failed or deadlocked and its tickets are skipped (Algorithm 2 lines 11-19).
  /// Paper default: twice the lease time.
  unsigned stall_multiplier = 2;

  /// omega: base interval of the dynamic interval polling. A waiter with `wait_count`
  /// preceding tickets sleeps `wait_count * poll_unit` between two RDMA READs of the lock object
  /// (Algorithm 2 line 20). Paper default: 5 us, roughly one RDMA round trip.
  Duration poll_unit{5};

  /// Upper bound for a single polling sleep. Safety does not depend on it (the lease is measured
  /// from the last moment a holder knew it did not hold the lock yet), but a long sleep delays
  /// the moment a waiter notices its grant and therefore the hand-over latency. Must not exceed
  /// the lease.
  Duration max_poll_interval{5'000};

  /// Truncated binary exponential backoff (Appendix A.3) used after bumping into a frozen lock
  /// object: the c-th consecutive failure sleeps uniformly in [0, min(backoff_base * 2^(c-1),
  /// backoff_max)]. Paper defaults: R = 10 us, L = 10 ms.
  Duration backoff_base{10};
  Duration backoff_max{10'000};

  /// COUNT_MAX: tickets are reset once a counter reaches this value, long before the 16-bit
  /// segment can overflow (Section 4.9). The paper uses 2^15 = 32768, i.e. the 16th bit acts as a
  /// canary. Lower values are useful in tests to exercise the reset paths quickly.
  uint16_t count_max = 32768;

  /// Multi-slot leasing (Section 5.1): a transaction may draw `k <= max_slots` consecutive
  /// tickets at once to obtain a lease of `k * lease`. With max_slots == 1 the protocol behaves
  /// exactly like the fixed-lease version of Section 4. With max_slots > 1 waiters cannot know
  /// how many slots the transaction in front of them holds, so the stall threshold becomes
  /// proportional to the number of outstanding tickets ahead of them (`stall_multiplier * lease *
  /// wait_count`), as described in the paper.
  uint16_t max_slots = 1;

  /// How many consecutive AcquireStatus::Retry outcomes LockSession::acquire tolerates before
  /// giving up with AcquireStatus::Aborted. Every retry is preceded by a random backoff, so this
  /// bounds the time spent on a lock object that stays frozen abnormally long.
  unsigned max_frozen_retries = 1000;

  bool multi_slot_leasing() const { return max_slots > 1; }

  /// Throws std::invalid_argument when the combination of parameters is unsafe or meaningless.
  void validate() const;
};

}  // namespace dslr
