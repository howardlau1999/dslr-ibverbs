// Single-threaded (or two-threaded, with the second thread acting as a scripted peer) scenario
// tests for LockSession. Each test puts a lock object into a specific state with
// LocalLockTable::poke and checks that the session reacts the way Algorithms 1-3 prescribe.

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

#include "dslr/local_lock_table.h"
#include "dslr/lock_session.h"

namespace dslr {
namespace {

using namespace std::chrono_literals;

constexpr LockRef kLock{0, 0};

/// Short lease so that stall-based tests finish quickly. Hand-overs are polled every few
/// microseconds, so a wait of a few hundred microseconds is "long" for these tests.
Config fast_config(uint16_t count_max = 32768) {
  Config config;
  config.lease = 2ms;
  config.max_poll_interval = 200us;
  config.poll_unit = 2us;
  config.backoff_base = 5us;
  config.backoff_max = 200us;
  config.count_max = count_max;
  return config;
}

class LockSessionScenario : public ::testing::Test {
 protected:
  /// Blocks until lock 0 shows `expected`, i.e. until a background session has drawn its ticket.
  /// still_waiting() alone cannot tell "blocked on the lock" from "not scheduled yet".
  void wait_for_word(LockWord expected) {
    const auto deadline = Clock::now() + 2s;
    while (table.peek(0) != expected && Clock::now() < deadline) {
      std::this_thread::yield();
    }
    ASSERT_EQ(table.peek(0), expected);
  }

  LocalLockTable table{4};
};

/// Runs `acquire` on another thread and lets the test observe whether it is still blocked.
struct BackgroundAcquire {
  BackgroundAcquire(LockWordAccessor& table, const Config& config, LockMode mode,
                    uint16_t slots = 1)
      : session(table, config, /*seed=*/7),
        result(std::async(std::launch::async, [this, mode, slots] {
          return session.acquire(kLock, mode, lock, slots);
        })) {}

  bool still_waiting(std::chrono::milliseconds grace = 2ms) {
    return result.wait_for(grace) == std::future_status::timeout;
  }

  LockSession session;
  HeldLock lock;
  std::future<AcquireStatus> result;
};

// ------------------------------------------------------------------------------------------
// Algorithm 1: a single FA grants an uncontended lock
// ------------------------------------------------------------------------------------------

TEST_F(LockSessionScenario, UncontendedSharedLockIsGrantedByTheFetchAndAdd) {
  LockSession session(table, fast_config());
  HeldLock lock;
  ASSERT_EQ(session.acquire(kLock, LockMode::Shared, lock), AcquireStatus::Acquired);
  EXPECT_EQ(lock.ticket, (LockWord{0, 0, 0, 0}));
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 0, 1}));
  EXPECT_EQ(lock.reset_from, 0u);
  EXPECT_EQ(session.stats().acquired_immediately, 1u);
  EXPECT_EQ(session.stats().polls, 0u);

  EXPECT_TRUE(session.release(lock));
  EXPECT_EQ(table.peek(0), (LockWord{0, 1, 0, 1}));
}

TEST_F(LockSessionScenario, UncontendedExclusiveLockIsGrantedByTheFetchAndAdd) {
  LockSession session(table, fast_config());
  HeldLock lock;
  ASSERT_EQ(session.acquire(kLock, LockMode::Exclusive, lock), AcquireStatus::Acquired);
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 1, 0}));
  EXPECT_TRUE(session.release(lock));
  EXPECT_EQ(table.peek(0), (LockWord{1, 0, 1, 0}));
}

TEST_F(LockSessionScenario, SharedLocksCoexist) {
  LockSession session(table, fast_config());
  HeldLock a, b, c;
  ASSERT_EQ(session.acquire(kLock, LockMode::Shared, a), AcquireStatus::Acquired);
  ASSERT_EQ(session.acquire(kLock, LockMode::Shared, b), AcquireStatus::Acquired);
  ASSERT_EQ(session.acquire(kLock, LockMode::Shared, c), AcquireStatus::Acquired);
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 0, 3}));
  // Shared holders may release in any order.
  EXPECT_TRUE(session.release(b));
  EXPECT_TRUE(session.release(c));
  EXPECT_TRUE(session.release(a));
  EXPECT_EQ(table.peek(0), (LockWord{0, 3, 0, 3}));
}

TEST_F(LockSessionScenario, RejectsInvalidSlotCount) {
  LockSession session(table, fast_config());
  HeldLock lock;
  EXPECT_THROW(session.acquire(kLock, LockMode::Shared, lock, 0), std::invalid_argument);
  EXPECT_THROW(session.acquire(kLock, LockMode::Shared, lock, 2), std::invalid_argument);
}

// ------------------------------------------------------------------------------------------
// Algorithm 2: waiting for preceding tickets
// ------------------------------------------------------------------------------------------

TEST_F(LockSessionScenario, SharedWaitsForPrecedingExclusiveTicket) {
  table.poke(0, LockWord{0, 0, 1, 0});  // exclusive ticket 0 is held by someone else
  BackgroundAcquire waiter(table, fast_config(), LockMode::Shared);
  wait_for_word(LockWord{0, 0, 1, 1});  // the waiter drew shared ticket 0
  EXPECT_TRUE(waiter.still_waiting());

  table.fetch_add(kLock, segment_add(Segment::NX, 1));  // the writer releases
  ASSERT_EQ(waiter.result.get(), AcquireStatus::Acquired);
  EXPECT_EQ(waiter.lock.ticket, (LockWord{0, 0, 1, 0}));
  EXPECT_EQ(waiter.session.stats().acquired_after_wait, 1u);
  EXPECT_GE(waiter.session.stats().polls, 1u);
}

TEST_F(LockSessionScenario, ExclusiveWaitsForAllPrecedingSharedTickets) {
  table.poke(0, LockWord{0, 0, 0, 2});  // two shared holders
  BackgroundAcquire waiter(table, fast_config(), LockMode::Exclusive);
  wait_for_word(LockWord{0, 0, 1, 2});
  EXPECT_TRUE(waiter.still_waiting());

  table.fetch_add(kLock, segment_add(Segment::NS, 1));  // first reader releases
  EXPECT_TRUE(waiter.still_waiting());
  table.fetch_add(kLock, segment_add(Segment::NS, 1));  // second reader releases
  ASSERT_EQ(waiter.result.get(), AcquireStatus::Acquired);
  EXPECT_EQ(table.peek(0), (LockWord{0, 2, 1, 2}));
}

TEST_F(LockSessionScenario, TicketsAreServedFirstComeFirstServed) {
  table.poke(0, LockWord{0, 0, 1, 0});  // a writer holds exclusive ticket 0
  BackgroundAcquire reader(table, fast_config(), LockMode::Shared);
  wait_for_word(LockWord{0, 0, 1, 1});  // reader's ticket: {x=1, s=0}
  BackgroundAcquire writer(table, fast_config(), LockMode::Exclusive);
  wait_for_word(LockWord{0, 0, 2, 1});  // writer's ticket: {x=1, s=1}, behind the reader
  ASSERT_TRUE(writer.still_waiting());

  table.fetch_add(kLock, segment_add(Segment::NX, 1));  // the first writer releases
  ASSERT_EQ(reader.result.get(), AcquireStatus::Acquired);
  EXPECT_TRUE(writer.still_waiting());  // reader-writer order is preserved
  EXPECT_TRUE(reader.session.release(reader.lock));
  ASSERT_EQ(writer.result.get(), AcquireStatus::Acquired);
  EXPECT_EQ(writer.lock.ticket, (LockWord{0, 0, 1, 1}));
}

TEST_F(LockSessionScenario, SharedWaiterIsNotFooledBySiblingsReleasingFirst) {
  // Regression test: a shared waiter with ticket {x=1, s=0} is granted together with a second
  // reader (ticket s=1). If that reader releases before the waiter polls again, nS passes the
  // waiter's ticket. The paper's pseudocode would read this as "skipped"; it is a normal grant.
  table.poke(0, LockWord{0, 0, 1, 0});
  BackgroundAcquire waiter(table, fast_config(), LockMode::Shared);
  wait_for_word(LockWord{0, 0, 1, 1});
  table.poke(0, LockWord{1, 1, 1, 2});  // writer released, sibling reader came and went
  ASSERT_EQ(waiter.result.get(), AcquireStatus::Acquired);
  EXPECT_EQ(waiter.session.stats().aborted_skipped, 0u);
  EXPECT_TRUE(waiter.session.release(waiter.lock));
  EXPECT_EQ(table.peek(0), (LockWord{1, 2, 1, 2}));
}

TEST_F(LockSessionScenario, WaiterAbortsWhenItsTicketWasSkipped) {
  table.poke(0, LockWord{0, 0, 1, 0});
  BackgroundAcquire waiter(table, fast_config(), LockMode::Exclusive);  // ticket {x=1, s=0}
  wait_for_word(LockWord{0, 0, 2, 0});
  table.poke(0, LockWord{2, 0, 2, 0});  // somebody skipped everything up to ticket 1
  EXPECT_EQ(waiter.result.get(), AcquireStatus::Aborted);
  EXPECT_EQ(waiter.session.stats().aborted_skipped, 1u);
}

TEST_F(LockSessionScenario, ExclusiveWaiterDetectsSkipThroughSharedCounter) {
  table.poke(0, LockWord{0, 0, 0, 1});
  BackgroundAcquire waiter(table, fast_config(), LockMode::Exclusive);  // ticket {x=0, s=1}
  wait_for_word(LockWord{0, 0, 1, 1});
  table.poke(0, LockWord{0, 2, 1, 2});  // nS jumped past our ticket: a later writer skipped us
  EXPECT_EQ(waiter.result.get(), AcquireStatus::Aborted);
  EXPECT_EQ(waiter.session.stats().aborted_skipped, 1u);
}

// ------------------------------------------------------------------------------------------
// Algorithm 2 lines 11-19: stall detection (failed transactions, deadlocks)
// ------------------------------------------------------------------------------------------

TEST_F(LockSessionScenario, ExclusiveWaiterSkipsDeadWriterAfterTwoLeases) {
  table.poke(0, LockWord{0, 0, 1, 0});  // exclusive ticket 0 will never be released
  LockSession session(table, fast_config());
  HeldLock lock;
  const auto start = Clock::now();
  EXPECT_EQ(session.acquire(kLock, LockMode::Exclusive, lock), AcquireStatus::Aborted);
  const auto elapsed = Clock::now() - start;
  EXPECT_GE(elapsed, 4ms);  // stall_multiplier (2) x lease (2 ms)
  EXPECT_LT(elapsed, 500ms);
  EXPECT_EQ(session.stats().stall_skips, 1u);
  // Paper line 15: nX := prev(maxX) + 1 (our own ticket is consumed too), nS := prev(maxS).
  EXPECT_EQ(table.peek(0), (LockWord{2, 0, 2, 0}));

  // The object is usable again right away.
  EXPECT_EQ(session.acquire(kLock, LockMode::Exclusive, lock), AcquireStatus::Acquired);
  EXPECT_EQ(lock.ticket, (LockWord{2, 0, 2, 0}));
}

TEST_F(LockSessionScenario, SharedWaiterSkipsDeadWriterAndCountsOnlyItself) {
  table.poke(0, LockWord{0, 0, 1, 0});
  LockSession session(table, fast_config());
  HeldLock lock;
  EXPECT_EQ(session.acquire(kLock, LockMode::Shared, lock), AcquireStatus::Aborted);
  EXPECT_EQ(session.stats().stall_skips, 1u);
  // nX := prev(maxX) (writer skipped), nS := nS + 1 (our own ticket), maxX/maxS untouched.
  EXPECT_EQ(table.peek(0), (LockWord{1, 1, 1, 1}));
  EXPECT_EQ(session.acquire(kLock, LockMode::Exclusive, lock), AcquireStatus::Acquired);
}

TEST_F(LockSessionScenario, SharedSkipLeavesConcurrentReadersConsistent) {
  // Two readers wait behind a dead writer. Whoever times out first skips the writer; the other
  // reader must be granted (not aborted), and nS must end up exactly 2 after both are done.
  table.poke(0, LockWord{0, 0, 1, 0});
  LockSession a(table, fast_config(), 1), b(table, fast_config(), 2);
  HeldLock la, lb;
  auto fa = std::async(std::launch::async, [&] { return a.acquire(kLock, LockMode::Shared, la); });
  auto fb = std::async(std::launch::async, [&] { return b.acquire(kLock, LockMode::Shared, lb); });
  const AcquireStatus sa = fa.get();
  const AcquireStatus sb = fb.get();
  const unsigned granted = static_cast<unsigned>(sa == AcquireStatus::Acquired) +
                           static_cast<unsigned>(sb == AcquireStatus::Acquired);
  const unsigned skips = static_cast<unsigned>(a.stats().stall_skips + b.stats().stall_skips);
  EXPECT_EQ(skips, 1u);
  EXPECT_EQ(granted, 1u);
  if (sa == AcquireStatus::Acquired) {
    EXPECT_TRUE(a.release(la));
  }
  if (sb == AcquireStatus::Acquired) {
    EXPECT_TRUE(b.release(lb));
  }
  EXPECT_EQ(table.peek(0), (LockWord{1, 2, 1, 2}));
}

TEST_F(LockSessionScenario, StallTimerRestartsWheneverCountersMove) {
  // Three shared holders release one by one, each just inside the stall threshold. The waiting
  // writer must not give up as long as there is progress.
  table.poke(0, LockWord{0, 0, 0, 3});
  BackgroundAcquire writer(table, fast_config(), LockMode::Exclusive);
  wait_for_word(LockWord{0, 0, 1, 3});
  for (int i = 0; i < 3; ++i) {
    std::this_thread::sleep_for(3ms);  // < 2 x 2 ms lease
    EXPECT_TRUE(writer.still_waiting(0ms));
    table.fetch_add(kLock, segment_add(Segment::NS, 1));
  }
  EXPECT_EQ(writer.result.get(), AcquireStatus::Acquired);
  EXPECT_EQ(writer.session.stats().stall_skips, 0u);
}

// ------------------------------------------------------------------------------------------
// Algorithm 3: release and leases
// ------------------------------------------------------------------------------------------

TEST_F(LockSessionScenario, ReleaseAfterLeaseExpiryStillCountsIfNobodySkippedUs) {
  LockSession session(table, fast_config());
  HeldLock lock;
  ASSERT_EQ(session.acquire(kLock, LockMode::Exclusive, lock), AcquireStatus::Acquired);
  std::this_thread::sleep_for(5ms);  // > 2 ms lease
  EXPECT_TRUE(session.release(lock));
  EXPECT_EQ(table.peek(0), (LockWord{1, 0, 1, 0}));
  EXPECT_EQ(session.stats().late_releases, 1u);
  EXPECT_EQ(session.stats().revoked_releases, 0u);
}

TEST_F(LockSessionScenario, ReleaseAfterLeaseExpiryIsRefusedIfWeWereSkipped) {
  LockSession session(table, fast_config());
  HeldLock lock;
  ASSERT_EQ(session.acquire(kLock, LockMode::Exclusive, lock), AcquireStatus::Acquired);
  std::this_thread::sleep_for(5ms);
  table.poke(0, LockWord{2, 0, 2, 0});  // a waiter gave up on us and skipped our ticket
  EXPECT_FALSE(session.release(lock));
  EXPECT_EQ(table.peek(0), (LockWord{2, 0, 2, 0}));  // not incremented a second time
  EXPECT_EQ(session.stats().revoked_releases, 1u);
}

TEST_F(LockSessionScenario, ReleaseAfterLeaseExpiryIsRefusedIfTheObjectWasReset) {
  Config config = fast_config(/*count_max=*/8);
  LockSession session(table, config);
  table.poke(0, LockWord{3, 3, 3, 3});
  HeldLock lock;
  ASSERT_EQ(session.acquire(kLock, LockMode::Shared, lock), AcquireStatus::Acquired);
  std::this_thread::sleep_for(5ms);
  table.poke(0, LockWord{0, 0, 1, 0});  // reset to zero and already reused by a new generation
  EXPECT_FALSE(session.release(lock));
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 1, 0}));
}

// The lease check and the release FA are two steps. A thread descheduled between them for
// longer than the slack between lease expiry and the stall threshold releases *after* a waiter
// skipped it; the FA's previous value reveals this and the increment must be taken back. These
// tests play the holder that still believes its lease is valid while the object already says
// otherwise.

TEST_F(LockSessionScenario, StrayExclusiveReleaseIsUndone) {
  LockSession session(table, fast_config());
  HeldLock lock;
  ASSERT_EQ(session.acquire(kLock, LockMode::Exclusive, lock), AcquireStatus::Acquired);
  // Ticket 1 waited, skipped us (nX := 2) and aborted; ticket 2 was drawn and is now served.
  table.poke(0, LockWord{2, 0, 3, 0});

  EXPECT_FALSE(session.release(lock));  // our lease is still "valid" in our own eyes
  EXPECT_EQ(table.peek(0), (LockWord{2, 0, 3, 0}))
      << "a stray increment would have let ticket 3 in while ticket 2 holds the lock";
  EXPECT_EQ(session.stats().stray_releases, 1u);
  EXPECT_EQ(session.stats().revoked_releases, 1u);
  EXPECT_EQ(session.stats().late_releases, 0u);
}

TEST_F(LockSessionScenario, StraySharedReleaseIsUndone) {
  LockSession session(table, fast_config());
  HeldLock reader;
  ASSERT_EQ(session.acquire(kLock, LockMode::Shared, reader), AcquireStatus::Acquired);
  // A writer (ticket {0,0,0,1}) waited two leases and skipped us: nX := 1, nS := 1.
  table.poke(0, LockWord{1, 1, 1, 1});

  EXPECT_FALSE(session.release(reader));
  EXPECT_EQ(table.peek(0), (LockWord{1, 1, 1, 1}));
  EXPECT_EQ(session.stats().stray_releases, 1u);

  // Without the undo, nS would be one ahead of the shared tickets ever drawn and every later
  // writer would see its ticket as skipped until the next counter reset.
  HeldLock writer;
  EXPECT_EQ(session.acquire(kLock, LockMode::Exclusive, writer), AcquireStatus::Acquired);
  EXPECT_TRUE(session.release(writer));
  EXPECT_EQ(table.peek(0), (LockWord{2, 1, 2, 1}));
}

// ------------------------------------------------------------------------------------------
// Section 4.9: counter resets
// ------------------------------------------------------------------------------------------

TEST_F(LockSessionScenario, LastTicketBeforeCountMaxBecomesTheResetter) {
  LockSession session(table, fast_config(/*count_max=*/4));
  HeldLock locks[4];
  for (HeldLock& lock : locks) {
    ASSERT_EQ(session.acquire(kLock, LockMode::Shared, lock), AcquireStatus::Acquired);
  }
  EXPECT_EQ(locks[0].reset_from, 0u);
  EXPECT_EQ(locks[2].reset_from, 0u);
  // Paper line 11: ResetFrom = {prev(maxX), COUNT_MAX, prev(maxX), COUNT_MAX}.
  EXPECT_EQ(LockWord::decode(locks[3].reset_from), (LockWord{0, 4, 0, 4}));
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 0, 4}));

  // Nobody can draw a ticket while the object is frozen.
  HeldLock late;
  EXPECT_EQ(session.try_acquire(kLock, LockMode::Shared, late), AcquireStatus::Retry);
  EXPECT_EQ(session.try_acquire(kLock, LockMode::Exclusive, late), AcquireStatus::Retry);
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 0, 4}));  // both tickets were withdrawn
  EXPECT_EQ(session.stats().frozen_retries, 2u);

  // Releasing the three earlier readers leaves the object frozen; the resetter's release
  // drains it and resets it to zero.
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(session.release(locks[i]));
  }
  EXPECT_EQ(table.peek(0), (LockWord{0, 3, 0, 4}));
  EXPECT_TRUE(session.release(locks[3]));
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 0, 0}));
  EXPECT_EQ(session.stats().counter_resets, 1u);

  // Business as usual afterwards (Figure 6).
  ASSERT_EQ(session.acquire(kLock, LockMode::Exclusive, late), AcquireStatus::Acquired);
  EXPECT_EQ(late.ticket, (LockWord{0, 0, 0, 0}));
}

TEST_F(LockSessionScenario, ExclusiveResetterResetsImmediatelyOnRelease) {
  LockSession session(table, fast_config(/*count_max=*/8));
  table.poke(0, LockWord{7, 5, 7, 5});  // seven writers and five readers came and went
  HeldLock lock;
  ASSERT_EQ(session.acquire(kLock, LockMode::Exclusive, lock), AcquireStatus::Acquired);
  // Paper line 25: ResetFrom = {COUNT_MAX, prev(maxS), COUNT_MAX, prev(maxS)}.
  EXPECT_EQ(LockWord::decode(lock.reset_from), (LockWord{8, 5, 8, 5}));
  EXPECT_TRUE(session.release(lock));
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 0, 0}));
  EXPECT_EQ(session.stats().counter_resets, 1u);
}

TEST_F(LockSessionScenario, ResetterWaitsForConcurrentReadersBeforeResetting) {
  LockSession session(table, fast_config(/*count_max=*/4));
  table.poke(0, LockWord{0, 1, 0, 3});  // readers 1 and 2 still hold the lock
  HeldLock lock;
  ASSERT_EQ(session.acquire(kLock, LockMode::Shared, lock), AcquireStatus::Acquired);
  ASSERT_NE(lock.reset_from, 0u);
  auto released = std::async(std::launch::async, [&] { return session.release(lock); });
  wait_for_word(LockWord{0, 2, 0, 4});  // our release landed, drain in progress
  EXPECT_EQ(released.wait_for(1ms), std::future_status::timeout);
  table.fetch_add(kLock, segment_add(Segment::NS, 2));  // the two readers release
  EXPECT_TRUE(released.get());
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 0, 0}));
}

TEST_F(LockSessionScenario, ResetterSkipsDeadReadersAfterAStall) {
  LockSession session(table, fast_config(/*count_max=*/4));
  table.poke(0, LockWord{0, 1, 0, 3});  // readers 1 and 2 will never release
  HeldLock lock;
  ASSERT_EQ(session.acquire(kLock, LockMode::Shared, lock), AcquireStatus::Acquired);
  EXPECT_TRUE(session.release(lock));  // blocks ~2 leases, then skips them and resets
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 0, 0}));
  EXPECT_EQ(session.stats().stall_skips, 1u);
  EXPECT_EQ(session.stats().counter_resets, 1u);
}

TEST_F(LockSessionScenario, FrozenObjectWithDeadResetterIsForceResetByRequesters) {
  table.poke(0, LockWord{0, 3, 0, 4});  // frozen; the resetter (ticket 3) died
  LockSession session(table, fast_config(/*count_max=*/4));
  HeldLock lock;
  const auto start = Clock::now();
  ASSERT_EQ(session.acquire(kLock, LockMode::Shared, lock), AcquireStatus::Acquired);
  EXPECT_GE(Clock::now() - start, 4ms);
  EXPECT_EQ(session.stats().forced_resets, 1u);
  EXPECT_EQ(session.stats().counter_resets, 1u);
  EXPECT_GE(session.stats().frozen_retries, 1u);
  EXPECT_EQ(lock.ticket, (LockWord{0, 0, 0, 0}));
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 0, 1}));
}

TEST_F(LockSessionScenario, AcquireGivesUpAfterMaxFrozenRetries) {
  Config config = fast_config(/*count_max=*/4);
  config.lease = 1s;  // no forced reset within the test
  config.max_poll_interval = 1ms;
  config.max_frozen_retries = 5;
  table.poke(0, LockWord{0, 3, 0, 4});
  LockSession session(table, config);
  HeldLock lock;
  EXPECT_EQ(session.acquire(kLock, LockMode::Exclusive, lock), AcquireStatus::Aborted);
  EXPECT_EQ(session.stats().frozen_retries, 5u);
  EXPECT_EQ(table.peek(0), (LockWord{0, 3, 0, 4}));  // every withdrawn ticket left no trace
}

TEST_F(LockSessionScenario, ResetterThatStallsWhileWaitingStillResetsTheObject) {
  // The designated resetter times out behind a dead writer: its skip must be followed by the
  // reset, otherwise the object would stay frozen until some requester forces it.
  LockSession session(table, fast_config(/*count_max=*/4));
  table.poke(0, LockWord{2, 0, 3, 0});  // writer 2 is dead, writer 3 would be the resetter
  HeldLock lock;
  EXPECT_EQ(session.acquire(kLock, LockMode::Exclusive, lock), AcquireStatus::Aborted);
  EXPECT_EQ(session.stats().stall_skips, 1u);
  EXPECT_EQ(session.stats().counter_resets, 1u);
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 0, 0}));
}

// ------------------------------------------------------------------------------------------
// Section 5.1: multi-slot leasing
// ------------------------------------------------------------------------------------------

TEST_F(LockSessionScenario, MultiSlotTicketsReserveAndReleaseSeveralNumbers) {
  Config config = fast_config();
  config.max_slots = 8;
  LockSession session(table, config);
  HeldLock lock;
  ASSERT_EQ(session.acquire(kLock, LockMode::Exclusive, lock, 3), AcquireStatus::Acquired);
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 3, 0}));
  // A follower waits for all three numbers.
  BackgroundAcquire follower(table, config, LockMode::Shared);
  wait_for_word(LockWord{0, 0, 3, 1});
  EXPECT_TRUE(follower.still_waiting());
  EXPECT_TRUE(session.release(lock));
  EXPECT_EQ(follower.result.get(), AcquireStatus::Acquired);
  EXPECT_EQ(table.peek(0), (LockWord{3, 0, 3, 1}));
}

TEST_F(LockSessionScenario, MultiSlotLeaseIsProportionalToSlots) {
  Config config = fast_config();
  config.max_slots = 8;
  LockSession session(table, config);
  HeldLock lock;
  ASSERT_EQ(session.acquire(kLock, LockMode::Exclusive, lock, 4), AcquireStatus::Acquired);
  std::this_thread::sleep_for(5ms);  // > 1 lease, < 4 leases
  EXPECT_TRUE(session.release(lock));
  EXPECT_EQ(session.stats().late_releases, 0u);
}

TEST_F(LockSessionScenario, MultiSlotStallThresholdScalesWithOutstandingTickets) {
  Config config = fast_config();
  config.max_slots = 8;
  table.poke(0, LockWord{0, 0, 4, 0});  // a 4-slot writer is in front of us
  BackgroundAcquire waiter(table, config, LockMode::Exclusive);
  wait_for_word(LockWord{0, 0, 5, 0});
  std::this_thread::sleep_for(6ms);  // > 2 leases: a single-slot waiter would have given up
  EXPECT_TRUE(waiter.still_waiting(0ms));
  table.fetch_add(kLock, segment_add(Segment::NX, 4));
  EXPECT_EQ(waiter.result.get(), AcquireStatus::Acquired);
}

// ------------------------------------------------------------------------------------------
// Helpers in dslr::detail
// ------------------------------------------------------------------------------------------

TEST(ResetDetection, ResetObserved) {
  constexpr uint16_t kCountMax = 8;
  // Counters moving forward is normal.
  EXPECT_FALSE(detail::reset_observed(LockWord{1, 1, 2, 2}, LockWord{2, 1, 2, 3}, kCountMax));
  // "Now serving" counters going backwards prove a reset.
  EXPECT_TRUE(detail::reset_observed(LockWord{5, 7, 8, 8}, LockWord{0, 0, 1, 0}, kCountMax));
  // A frozen object becoming unfrozen proves a reset even when the small counters match.
  EXPECT_TRUE(detail::reset_observed(LockWord{0, 0, 8, 0}, LockWord{0, 0, 1, 0}, kCountMax));
  // Transient increments on a frozen object are withdrawn without a reset.
  EXPECT_FALSE(detail::reset_observed(LockWord{0, 0, 9, 0}, LockWord{0, 0, 8, 0}, kCountMax));
}

TEST(ResetDetection, GenerationChanged) {
  EXPECT_FALSE(detail::generation_changed(LockWord{1, 1, 2, 2}, LockWord{1, 1, 2, 3}));
  EXPECT_TRUE(detail::generation_changed(LockWord{1, 1, 2, 2}, LockWord{0, 0, 1, 0}));
  EXPECT_TRUE(detail::generation_changed(LockWord{0, 0, 2, 2}, LockWord{0, 0, 1, 2}));
}

}  // namespace
}  // namespace dslr
