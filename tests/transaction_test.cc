#include "dslr/transaction.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "dslr/local_lock_table.h"

namespace dslr {
namespace {

using namespace std::chrono_literals;

Config quick_config() {
  Config config;
  config.lease = 2ms;
  config.max_poll_interval = 200us;
  config.poll_unit = 2us;
  config.backoff_base = 5us;
  config.backoff_max = 200us;
  return config;
}

TEST(Transaction, CommitReleasesEverythingInReverseOrder) {
  LocalLockTable table(3);
  LockSession session(table, quick_config());
  {
    Transaction txn(session);
    ASSERT_TRUE(txn.lock(LockRef{0, 0}, LockMode::Shared));
    ASSERT_TRUE(txn.lock(LockRef{0, 1}, LockMode::Exclusive));
    ASSERT_TRUE(txn.lock(LockRef{0, 2}, LockMode::Shared));
    EXPECT_EQ(txn.held().size(), 3u);
    EXPECT_EQ(table.peek(1), (LockWord{0, 0, 1, 0}));
    EXPECT_TRUE(txn.commit());
    EXPECT_TRUE(txn.held().empty());
  }
  EXPECT_EQ(table.peek(0), (LockWord{0, 1, 0, 1}));
  EXPECT_EQ(table.peek(1), (LockWord{1, 0, 1, 0}));
  EXPECT_EQ(table.peek(2), (LockWord{0, 1, 0, 1}));
}

TEST(Transaction, DestructorReleasesHeldLocks) {
  LocalLockTable table(1);
  LockSession session(table, quick_config());
  {
    Transaction txn(session);
    ASSERT_TRUE(txn.lock(LockRef{0, 0}, LockMode::Exclusive));
    EXPECT_EQ(table.peek(0), (LockWord{0, 0, 1, 0}));
  }
  EXPECT_EQ(table.peek(0), (LockWord{1, 0, 1, 0}));
}

TEST(Transaction, AbortedAcquisitionReleasesEarlierLocks) {
  LocalLockTable table(2);
  table.poke(1, LockWord{0, 0, 1, 0});  // lock 1 is held by a transaction that will never release
  LockSession session(table, quick_config());
  Transaction txn(session);
  ASSERT_TRUE(txn.lock(LockRef{0, 0}, LockMode::Exclusive));
  EXPECT_FALSE(txn.lock(LockRef{0, 1}, LockMode::Exclusive));  // stall -> Aborted
  EXPECT_TRUE(txn.aborted());
  EXPECT_TRUE(txn.held().empty());
  EXPECT_EQ(table.peek(0), (LockWord{1, 0, 1, 0}));  // lock 0 was released by the abort
  EXPECT_EQ(table.peek(1), (LockWord{2, 0, 2, 0}));  // lock 1 was skipped past the dead holder
}

TEST(Transaction, AbandonLeavesTicketsUnreleased) {
  LocalLockTable table(1);
  LockSession session(table, quick_config());
  {
    Transaction txn(session);
    ASSERT_TRUE(txn.lock(LockRef{0, 0}, LockMode::Shared));
    txn.abandon();
  }
  EXPECT_EQ(table.peek(0), (LockWord{0, 0, 0, 1}));  // nobody released ticket 0
  EXPECT_EQ(session.stats().releases, 0u);
}

TEST(Transaction, CommitReportsRevokedLeases) {
  LocalLockTable table(1);
  LockSession session(table, quick_config());
  Transaction txn(session);
  ASSERT_TRUE(txn.lock(LockRef{0, 0}, LockMode::Exclusive));
  std::this_thread::sleep_for(5ms);     // lease expired
  table.poke(0, LockWord{2, 0, 2, 0});  // and somebody skipped our ticket meanwhile
  EXPECT_FALSE(txn.commit());
  EXPECT_EQ(table.peek(0), (LockWord{2, 0, 2, 0}));
}

}  // namespace
}  // namespace dslr
