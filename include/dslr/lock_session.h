#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include "dslr/backoff.h"
#include "dslr/clock.h"
#include "dslr/config.h"
#include "dslr/lock_word.h"
#include "dslr/lock_word_accessor.h"
#include "dslr/types.h"

namespace dslr {

/// A lock held (or being waited for) by a transaction. It carries everything DSLR needs to
/// later release the lock, so no per-lock state has to be kept anywhere else.
struct HeldLock {
  LockRef ref;
  LockMode mode = LockMode::Shared;
  /// Number of consecutive tickets drawn (multi-slot leasing, Section 5.1). 1 unless the
  /// transaction asked for a longer lease.
  uint16_t slots = 1;
  /// `prev` in the paper: the value of the lock object right before our fetch-and-add. Our ticket
  /// numbers are prev.maxX (exclusive) / prev.maxS (shared); the other counters tell us which
  /// tickets precede ours.
  LockWord ticket;
  /// Lower bound on the moment our ticket became served: the last time we *knew* we did not hold
  /// the lock yet (the FA, or the last READ that still showed an earlier ticket). The lease runs
  /// from here, which keeps it safe even if we were descheduled before noticing the grant.
  Clock::time_point granted_at;
  /// ResetFrom[tid, L] in the paper: when our ticket pushed a counter to COUNT_MAX, this is the
  /// (encoded) value the lock object will have once every ticket up to ours has been released,
  /// and the value we must CAS to zero in release(). Zero when we are not the designated
  /// resetter. The paper keeps this in a node-wide table indexed by (tid, L); since only the
  /// owning transaction ever consults it, storing it with the lock is equivalent.
  uint64_t reset_from = 0;
};

/// Counters describing what a session has been doing. Useful for benchmarks and tests.
struct SessionStats {
  uint64_t acquire_attempts = 0;      ///< try_acquire calls (one FA each)
  uint64_t acquired_immediately = 0;  ///< granted by the FA alone (Algorithm 1 lines 12-13/26-27)
  uint64_t acquired_after_wait = 0;   ///< granted inside HandleConflict
  uint64_t polls = 0;                 ///< RDMA READs issued while waiting
  uint64_t frozen_retries = 0;        ///< FAs that hit a frozen lock (Algorithm 1 lines 4-9/18-23)
  uint64_t undo_cas_retries = 0;      ///< extra CAS rounds needed to withdraw an increment
  uint64_t forced_resets = 0;         ///< frozen locks reset by a requester (Algorithm 1 line 8)
  uint64_t stall_skips = 0;           ///< successful skip CASes (Algorithm 2 line 16)
  uint64_t aborted_skipped = 0;       ///< our own ticket was skipped by someone else
  uint64_t aborted_by_reset = 0;      ///< our ticket vanished in a reset to zero
  uint64_t releases = 0;              ///< release() calls
  uint64_t late_releases = 0;         ///< released after the lease expired (ticket still valid)
  uint64_t revoked_releases = 0;      ///< release() returned false: the ticket had been revoked
  uint64_t stray_releases = 0;        ///< subset of revoked_releases whose FA had to be undone
  uint64_t counter_resets = 0;        ///< lock objects we reset to zero (Section 4.9)
};

/// A DSLR lock manager instance acting on behalf of the transactions of one thread.
///
/// The session implements Algorithms 1-3 of the paper on top of a LockWordAccessor. It is
/// deliberately single-threaded: RDMA queue pairs are not thread-safe, and each transaction's
/// operations are sequential anyway (fetch-and-add, then read until served). Create one session
/// per worker thread; sessions may share a LocalLockTable but need their own LockTableClient.
///
/// Typical use:
///
///     dslr::LockSession session(accessor, config);
///     dslr::HeldLock lock;
///     if (session.acquire(ref, dslr::LockMode::Exclusive, lock) == dslr::AcquireStatus::Acquired)
///     {
///       ... critical section, shorter than config.lease ...
///       session.release(lock);
///     } else {
///       // Aborted: release everything this transaction holds and restart it.
///     }
///
/// dslr::Transaction wraps this pattern for transactions holding several locks.
class LockSession {
 public:
  /// `seed` fixes the random backoff sequence (tests); by default std::random_device is used.
  explicit LockSession(LockWordAccessor& accessor, Config config = {},
                       std::optional<uint64_t> seed = std::nullopt);

  LockSession(const LockSession&) = delete;
  LockSession& operator=(const LockSession&) = delete;

  /// Acquires `ref` in `mode`, drawing `slots` consecutive tickets (1 unless multi-slot leasing
  /// is enabled). Blocks while preceding tickets are served. Transparently retries while the
  /// lock object is frozen for a counter reset (AcquireStatus::Retry), so it only returns
  /// Acquired or Aborted. On Acquired, `out` describes the lock and must be passed to release().
  AcquireStatus acquire(LockRef ref, LockMode mode, HeldLock& out, uint16_t slots = 1);

  /// One run of Algorithm 1 (which calls Algorithm 2 on conflicts). Unlike acquire(), it reports
  /// AcquireStatus::Retry when the lock object is frozen for a counter reset.
  AcquireStatus try_acquire(LockRef ref, LockMode mode, HeldLock& out, uint16_t slots = 1);

  /// Algorithm 3: releases a lock obtained from acquire()/try_acquire(). When this transaction
  /// is the designated resetter of the lock object it also waits for the preceding tickets to
  /// drain and resets the counters to zero.
  ///
  /// Returns true when the release was counted. Returns false when the ticket had already been
  /// revoked by other transactions (skipped after a stall, or wiped by a reset), which can only
  /// happen once the lease has expired; the critical section may then have overlapped with
  /// another holder's, and the caller should treat its transaction as failed.
  bool release(const HeldLock& lock);

  const Config& config() const { return config_; }
  const SessionStats& stats() const { return stats_; }
  void reset_stats() { stats_ = SessionStats{}; }

 private:
  /// Remembers the last observed "now serving" counters of a lock object and since when they
  /// have provably been unchanged. Both the lease and the stall logic depend only on local time.
  ///
  /// An operation issued at time i and completed at time c observed the counters at some
  /// unknown instant inside [i, c]. Because the counters only move forward, two observations of
  /// the same value prove stillness from the completion of the first to the issue of the second;
  /// measuring that way can only under-estimate a stall and therefore never skips a transaction
  /// that is still within its lease, however long this thread was descheduled.
  struct StallTimer {
    uint16_t nX = 0;
    uint16_t nS = 0;
    Clock::time_point since{};

    StallTimer() = default;
    /// `observed_at` is the completion time of the operation that returned `word`.
    StallTimer(const LockWord& word, Clock::time_point observed_at)
        : nX(word.nX), nS(word.nS), since(observed_at) {}

    /// Records an observation made by an operation issued at `issued_at` and completed at
    /// `completed_at`; returns for how long the counters have provably been unchanged.
    Duration observe(const LockWord& word, Clock::time_point issued_at,
                     Clock::time_point completed_at);
  };

  /// State kept between consecutive attempts on a lock object that was found frozen; this is
  /// the "since last failure" memory of Algorithm 1 line 7 and the per-object failure count of
  /// the exponential backoff (Appendix A.3).
  struct FrozenLockState {
    unsigned consecutive_failures = 0;
    StallTimer stall;
  };

  AcquireStatus handle_conflict(HeldLock& lock, HeldLock& out, Clock::time_point fa_completed_at);
  AcquireStatus back_off_from_frozen_lock(LockRef ref, const LockWord& prev,
                                          Clock::time_point fa_issued_at,
                                          Clock::time_point fa_completed_at);
  void undo_fetch_add(LockRef ref, const LockWord& prev, Segment segment, uint16_t amount);
  bool ticket_revoked(const LockWord& now, const HeldLock& lock) const;
  void force_reset_if_stalled(LockRef ref);
  void drain_and_reset(const HeldLock& lock, LockWord current);
  void reset_to_zero(LockRef ref, LockWord current);

  bool is_frozen(const LockWord& word) const;
  static bool ticket_served(const LockWord& now, const HeldLock& lock);
  static bool ticket_skipped(const LockWord& now, const HeldLock& lock);
  static unsigned outstanding_before(const LockWord& now, const LockWord& ticket);
  static unsigned outstanding_total(const LockWord& now);
  Duration stall_threshold(unsigned wait_count) const;
  Duration poll_interval(unsigned wait_count) const;

  static constexpr size_t kMaxTrackedFrozenLocks = 1024;

  LockWordAccessor& accessor_;
  Config config_;
  ExponentialBackoff backoff_;
  SessionStats stats_;
  std::unordered_map<LockRef, FrozenLockState, LockRefHash> frozen_locks_;
};

namespace detail {

/// True when `after` can only have been produced from `before` by a reset of the lock object to
/// zero (Section 4.9): the "now serving" counters never decrease otherwise, and a counter that
/// reached COUNT_MAX stays there until the reset. `before` may be a frozen word carrying
/// transient (soon withdrawn) ticket increments, so the "next ticket" counters are not compared.
bool reset_observed(const LockWord& before, const LockWord& after, uint16_t count_max);

/// Stricter variant for a ticket drawn from an unfrozen word: all four counters only grow until
/// the next reset, so any decrease proves that `now` belongs to a later generation.
bool generation_changed(const LockWord& ticket, const LockWord& now);

}  // namespace detail

}  // namespace dslr
