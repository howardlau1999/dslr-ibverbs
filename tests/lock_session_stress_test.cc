// Multi-threaded stress tests: many sessions hammer a few lock objects while a checker verifies
// the safety property of a lease-based lock manager — no two conflicting holders are ever
// inside their critical sections while both leases are valid — and the tests verify liveness
// (every transaction eventually commits) and the bookkeeping of the lock objects.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dslr/local_lock_table.h"
#include "dslr/lock_session.h"

namespace dslr {
namespace {

using namespace std::chrono_literals;

/// Tracks who is inside the critical section of each lock object. An overlap of conflicting
/// holders is only a violation while *both* leases are valid: under leases a holder that
/// overstays is, by definition, no longer protected (its release reports this), and a holder
/// that was descheduled between noticing its grant and entering its critical section may find
/// its lease already expired on arrival.
class ExclusionChecker {
 public:
  ExclusionChecker(uint32_t lock_count, Duration lease) : holders_(lock_count), lease_(lease) {}

  void enter(const HeldLock& lock) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto now = Clock::now();
    const auto expires_at = lock.granted_at + lease_ * lock.slots;
    if (expires_at > now) {
      for (const Entry& other : holders_[lock.ref.index]) {
        const bool conflict =
            other.lock.mode == LockMode::Exclusive || lock.mode == LockMode::Exclusive;
        if (conflict && other.expires_at > now) {
          record_violation(lock, other, now);
        }
      }
    }
    holders_[lock.ref.index].push_back({lock, expires_at});
  }

  void leave(const HeldLock& lock) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto& entries = holders_[lock.ref.index];
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) {
      return e.lock.ticket == lock.ticket && e.lock.mode == lock.mode;
    });
    ASSERT_NE(it, entries.end()) << "leave() without matching enter()";
    entries.erase(it);
  }

  int violations() const { return violations_.load(); }

  /// Describes the first violation (for the failure message); empty if there was none.
  std::string first_violation() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return first_violation_;
  }

 private:
  struct Entry {
    HeldLock lock;
    Clock::time_point expires_at;
  };

  void record_violation(const HeldLock& newcomer, const Entry& holder, Clock::time_point now) {
    if (violations_++ == 0) {
      const auto left = std::chrono::duration_cast<Duration>(holder.expires_at - now).count();
      first_violation_ = "lock " + std::to_string(newcomer.ref.index) + ": " +
                         to_string(holder.lock.mode) + " ticket " + to_string(holder.lock.ticket) +
                         " (lease left " + std::to_string(left) + " us) overlaps with " +
                         to_string(newcomer.mode) + " ticket " + to_string(newcomer.ticket);
    }
  }

  mutable std::mutex mutex_;
  std::vector<std::vector<Entry>> holders_;
  Duration lease_;
  std::atomic<int> violations_{0};
  std::string first_violation_;
};

struct StressOptions {
  unsigned threads = 4;
  unsigned transactions_per_thread = 3000;
  uint32_t lock_count = 8;
  unsigned max_locks_per_txn = 2;
  double shared_ratio = 0.5;
  double crash_ratio = 0.0;  ///< fraction of transactions that never release their locks
  bool sorted = true;        ///< acquire in index order (no deadlocks)
  Duration hold{0};          ///< time spent inside the critical section
  Config config;
};

struct StressOutcome {
  uint64_t commits = 0;
  uint64_t aborts = 0;
  uint64_t revoked = 0;
  uint64_t crashes = 0;
  SessionStats stats;
};

Config stress_config(uint16_t count_max = 32768) {
  Config config;
  config.lease = 20ms;  // generous: OS scheduling hiccups must not look like failures
  // A holder preempted between its lease check and its release FA has (stall_multiplier - 1)
  // leases before a waiter may skip it. The tests below shrink the lease to a few ms to keep
  // stall detection fast, so give them two leases of slack instead of the default one.
  config.stall_multiplier = 3;
  config.max_poll_interval = 1ms;
  config.poll_unit = 1us;
  config.backoff_base = 2us;
  config.backoff_max = 200us;
  config.count_max = count_max;
  return config;
}

void accumulate(SessionStats& into, const SessionStats& from) {
  into.acquire_attempts += from.acquire_attempts;
  into.acquired_immediately += from.acquired_immediately;
  into.acquired_after_wait += from.acquired_after_wait;
  into.polls += from.polls;
  into.frozen_retries += from.frozen_retries;
  into.undo_cas_retries += from.undo_cas_retries;
  into.forced_resets += from.forced_resets;
  into.stall_skips += from.stall_skips;
  into.aborted_skipped += from.aborted_skipped;
  into.aborted_by_reset += from.aborted_by_reset;
  into.releases += from.releases;
  into.late_releases += from.late_releases;
  into.revoked_releases += from.revoked_releases;
  into.stray_releases += from.stray_releases;
  into.counter_resets += from.counter_resets;
}

StressOutcome run_stress(LocalLockTable& table, ExclusionChecker& checker,
                         const StressOptions& options) {
  std::vector<StressOutcome> outcomes(options.threads);
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < options.threads; ++t) {
    threads.emplace_back([&, t] {
      LockSession session(table, options.config, /*seed=*/t + 1);
      std::mt19937_64 rng(1000 + t);
      std::uniform_real_distribution<double> coin(0.0, 1.0);
      StressOutcome& out = outcomes[t];

      for (unsigned i = 0; i < options.transactions_per_thread; ++i) {
        // Pick distinct lock objects and modes for this transaction.
        std::vector<std::pair<uint32_t, LockMode>> plan;
        const unsigned count = 1 + static_cast<unsigned>(rng() % options.max_locks_per_txn);
        while (plan.size() < count) {
          const auto index = static_cast<uint32_t>(rng() % options.lock_count);
          if (std::none_of(plan.begin(), plan.end(),
                           [&](const auto& p) { return p.first == index; })) {
            plan.emplace_back(
                index, coin(rng) < options.shared_ratio ? LockMode::Shared : LockMode::Exclusive);
          }
        }
        if (options.sorted) {
          std::sort(plan.begin(), plan.end());
        }

        while (true) {
          // Acquire in plan order; a holder registers with the checker right after its grant and
          // deregisters right before its release, so the checker never sees a stale holder.
          std::vector<HeldLock> held;
          bool ok = true;
          for (const auto& [index, mode] : plan) {
            HeldLock lock;
            if (session.acquire(LockRef{0, index}, mode, lock) != AcquireStatus::Acquired) {
              ok = false;
              break;
            }
            checker.enter(lock);
            held.push_back(lock);
          }
          if (ok && options.hold.count() > 0) {
            sleep_for_precise(options.hold);
          }
          for (const HeldLock& lock : held) {
            checker.leave(lock);
          }
          if (ok && options.crash_ratio > 0 && coin(rng) < options.crash_ratio) {
            ++out.crashes;  // crash while holding: the tickets are never released
            break;
          }
          bool all_valid = true;
          for (auto it = held.rbegin(); it != held.rend(); ++it) {
            all_valid = session.release(*it) && all_valid;
          }
          if (!ok) {
            ++out.aborts;  // Section 4.7: release everything and restart the transaction
            continue;
          }
          if (all_valid) {
            ++out.commits;
            break;
          }
          ++out.revoked;  // a lease expired and the ticket was revoked: redo the transaction
        }
      }
      out.stats = session.stats();
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  StressOutcome total;
  for (const StressOutcome& o : outcomes) {
    total.commits += o.commits;
    total.aborts += o.aborts;
    total.revoked += o.revoked;
    total.crashes += o.crashes;
    accumulate(total.stats, o.stats);
  }
  return total;
}

/// After every transaction has finished, each lock object must be idle: every drawn ticket
/// served and released (or the object reset to zero).
void expect_quiescent(const LocalLockTable& table) {
  for (uint32_t i = 0; i < table.lock_count(); ++i) {
    const LockWord word = table.peek(i);
    EXPECT_EQ(word.nX, word.maxX) << "lock " << i << ": " << to_string(word);
    EXPECT_EQ(word.nS, word.maxS) << "lock " << i << ": " << to_string(word);
  }
}

TEST(LockSessionStress, MixedSharedAndExclusiveTransactions) {
  StressOptions options;
  options.config = stress_config();
  LocalLockTable table(options.lock_count);
  ExclusionChecker checker(options.lock_count, options.config.lease);

  const StressOutcome outcome = run_stress(table, checker, options);
  EXPECT_EQ(checker.violations(), 0) << checker.first_violation();
  EXPECT_EQ(outcome.commits, uint64_t{options.threads} * options.transactions_per_thread);
  EXPECT_GT(outcome.stats.acquired_after_wait, 0u) << "the test should exercise waiting";
  expect_quiescent(table);
}

TEST(LockSessionStress, InjectedDelaysWidenRaceWindows) {
  StressOptions options;
  options.transactions_per_thread = 600;
  options.config = stress_config();
  LocalLockTable table(options.lock_count);
  table.set_injected_delay(20us);
  ExclusionChecker checker(options.lock_count, options.config.lease);

  const StressOutcome outcome = run_stress(table, checker, options);
  EXPECT_EQ(checker.violations(), 0) << checker.first_violation();
  EXPECT_EQ(outcome.commits, uint64_t{options.threads} * options.transactions_per_thread);
  expect_quiescent(table);
}

TEST(LockSessionStress, CountersAreResetUnderContention) {
  StressOptions options;
  options.lock_count = 2;
  options.config = stress_config(/*count_max=*/8);
  LocalLockTable table(options.lock_count);
  ExclusionChecker checker(options.lock_count, options.config.lease);

  const StressOutcome outcome = run_stress(table, checker, options);
  EXPECT_EQ(checker.violations(), 0) << checker.first_violation();
  EXPECT_EQ(outcome.commits, uint64_t{options.threads} * options.transactions_per_thread);
  EXPECT_GT(outcome.stats.counter_resets, 100u);
  EXPECT_GT(outcome.stats.frozen_retries, 0u);
  expect_quiescent(table);
  for (uint32_t i = 0; i < table.lock_count(); ++i) {
    EXPECT_LT(table.peek(i).maxS, options.config.count_max);
    EXPECT_LT(table.peek(i).maxX, options.config.count_max);
  }
}

TEST(LockSessionStress, CrashedTransactionsAreSkippedByOthers) {
  StressOptions options;
  options.transactions_per_thread = 300;
  options.crash_ratio = 0.05;
  options.config = stress_config();
  options.config.lease = 2ms;  // crashes are detected after two leases
  options.config.max_poll_interval = 500us;
  LocalLockTable table(options.lock_count);
  ExclusionChecker checker(options.lock_count, options.config.lease);

  const StressOutcome outcome = run_stress(table, checker, options);
  EXPECT_EQ(checker.violations(), 0) << checker.first_violation();
  EXPECT_EQ(outcome.commits + outcome.crashes,
            uint64_t{options.threads} * options.transactions_per_thread);
  EXPECT_GT(outcome.crashes, 0u);
  EXPECT_GT(outcome.stats.stall_skips, 0u) << "dead holders must have been skipped";
}

TEST(LockSessionStress, CrashesOnFrozenObjectsTriggerForcedResets) {
  StressOptions options;
  options.threads = 3;
  options.transactions_per_thread = 400;
  options.lock_count = 1;
  options.max_locks_per_txn = 1;
  options.crash_ratio = 0.05;
  options.config = stress_config(/*count_max=*/6);
  options.config.lease = 2ms;
  options.config.max_poll_interval = 500us;
  LocalLockTable table(options.lock_count);
  ExclusionChecker checker(options.lock_count, options.config.lease);

  const StressOutcome outcome = run_stress(table, checker, options);
  EXPECT_EQ(checker.violations(), 0) << checker.first_violation();
  EXPECT_EQ(outcome.commits + outcome.crashes,
            uint64_t{options.threads} * options.transactions_per_thread);
  EXPECT_GT(outcome.stats.counter_resets, 0u);
}

TEST(LockSessionStress, DeadlocksAreResolvedByStallDetection) {
  StressOptions options;
  options.threads = 4;
  // Four writers taking up to three of three locks in random order deadlock on almost every
  // attempt and are only untangled by stall detection, so each commit costs several stall
  // periods; keep the count small.
  options.transactions_per_thread = 25;
  options.lock_count = 3;
  options.max_locks_per_txn = 3;
  options.shared_ratio = 0.0;  // writers only: every lock order inversion is a deadlock
  options.sorted = false;
  options.hold = 50us;
  options.config = stress_config();
  options.config.lease = 2ms;
  options.config.max_poll_interval = 500us;
  LocalLockTable table(options.lock_count);
  ExclusionChecker checker(options.lock_count, options.config.lease);

  const StressOutcome outcome = run_stress(table, checker, options);
  EXPECT_EQ(checker.violations(), 0) << checker.first_violation();
  EXPECT_EQ(outcome.commits, uint64_t{options.threads} * options.transactions_per_thread);
  EXPECT_GT(outcome.aborts + outcome.revoked, 0u) << "the workload should have deadlocked";
  EXPECT_GT(outcome.stats.stall_skips, 0u);
}

TEST(LockSessionStress, ExclusiveTicketsAreGrantedInTicketOrder) {
  // FCFS (Appendix A.2): the order in which exclusive locks are granted must equal the order
  // in which the tickets were drawn.
  constexpr unsigned kThreads = 4;
  constexpr unsigned kPerThread = 2000;
  LocalLockTable table(1);
  const Config config = stress_config();
  std::mutex log_mutex;
  std::vector<uint16_t> grant_order;

  std::vector<std::thread> threads;
  for (unsigned t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      LockSession session(table, config, t + 1);
      for (unsigned i = 0; i < kPerThread; ++i) {
        HeldLock lock;
        ASSERT_EQ(session.acquire(LockRef{0, 0}, LockMode::Exclusive, lock),
                  AcquireStatus::Acquired);
        {
          std::lock_guard<std::mutex> guard(log_mutex);
          grant_order.push_back(lock.ticket.maxX);
        }
        ASSERT_TRUE(session.release(lock));
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  ASSERT_EQ(grant_order.size(), kThreads * kPerThread);
  EXPECT_TRUE(std::is_sorted(grant_order.begin(), grant_order.end()));
  EXPECT_EQ(table.peek(0), (LockWord{kThreads * kPerThread, 0, kThreads * kPerThread, 0}));
}

}  // namespace
}  // namespace dslr
