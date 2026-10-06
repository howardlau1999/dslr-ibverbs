// End-to-end tests over real ibverbs: a LockTableServer and LockTableClients in this process,
// connected through the (first) RDMA device in loopback. They are skipped when the machine has
// no RDMA device; a Soft-RoCE (rdma_rxe) device is sufficient. Set DSLR_TEST_DEVICE to pick a
// device and DSLR_TEST_GID_INDEX to override the GID selection.

#include <gtest/gtest.h>

#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "dslr/lock_session.h"
#include "dslr/lock_table_client.h"
#include "dslr/lock_table_server.h"
#include "dslr/rdma/device.h"

namespace dslr {
namespace {

using namespace std::chrono_literals;

rdma::DeviceOptions test_device_options() {
  rdma::DeviceOptions options;
  if (const char* name = std::getenv("DSLR_TEST_DEVICE")) {
    options.name = name;
  }
  if (const char* gid = std::getenv("DSLR_TEST_GID_INDEX")) {
    options.gid_index = static_cast<uint32_t>(std::atoi(gid));
  }
  return options;
}

class RdmaLoopback : public ::testing::Test {
 protected:
  void SetUp() override {
    if (rdma::Device::available_devices().empty()) {
      GTEST_SKIP() << "no RDMA device available (configure a NIC or Soft-RoCE to run this test)";
    }
    ServerOptions options;
    options.device = test_device_options();
    options.lock_count = kLocks;
    options.bind_address = "127.0.0.1";
    options.port = 0;
    server = std::make_unique<LockTableServer>(options);
    server->start();
    device = std::make_unique<rdma::Device>(options.device);
  }

  void TearDown() override {
    if (server) {
      server->stop();
    }
  }

  std::unique_ptr<LockTableClient> connect() {
    return std::make_unique<LockTableClient>(
        *device, std::vector<ServerAddress>{{"127.0.0.1", server->port()}});
  }

  static constexpr uint32_t kLocks = 64;
  std::unique_ptr<LockTableServer> server;
  std::unique_ptr<rdma::Device> device;
};

TEST_F(RdmaLoopback, VerbsHaveTheExpectedSemantics) {
  auto client = connect();
  const LockRef ref{0, 5};
  EXPECT_EQ(client->lock_count(0), kLocks);
  EXPECT_EQ(client->read(ref), 0u);

  // Fetch-and-add returns the previous value and adds in host byte order.
  EXPECT_EQ(client->fetch_add(ref, segment_add(Segment::MaxS, 3)), 0u);
  EXPECT_EQ(client->fetch_add(ref, segment_add(Segment::NX, 1)), (LockWord{0, 0, 0, 3}).encode());
  EXPECT_EQ(server->peek(5), (LockWord{1, 0, 0, 3}));
  EXPECT_EQ(client->read(ref), (LockWord{1, 0, 0, 3}).encode());

  // Compare-and-swap only swaps on an exact match and always reports the value it saw.
  const uint64_t current = (LockWord{1, 0, 0, 3}).encode();
  EXPECT_EQ(client->compare_swap(ref, current + 1, 0), current);
  EXPECT_EQ(server->peek(5), (LockWord{1, 0, 0, 3}));
  EXPECT_EQ(client->compare_swap(ref, current, 0), current);
  EXPECT_EQ(server->peek(5), (LockWord{}));

  // Out-of-range references are rejected locally.
  EXPECT_THROW(client->read(LockRef{0, kLocks}), std::out_of_range);
  EXPECT_THROW(client->read(LockRef{1, 0}), std::out_of_range);
  EXPECT_EQ(server->client_count(), 1u);
}

TEST_F(RdmaLoopback, SingleSessionAcquireRelease) {
  auto client = connect();
  LockSession session(*client);
  HeldLock shared, exclusive;
  ASSERT_EQ(session.acquire(LockRef{0, 1}, LockMode::Shared, shared), AcquireStatus::Acquired);
  ASSERT_EQ(session.acquire(LockRef{0, 2}, LockMode::Exclusive, exclusive),
            AcquireStatus::Acquired);
  EXPECT_EQ(server->peek(1), (LockWord{0, 0, 0, 1}));
  EXPECT_EQ(server->peek(2), (LockWord{0, 0, 1, 0}));
  EXPECT_TRUE(session.release(shared));
  EXPECT_TRUE(session.release(exclusive));
  EXPECT_EQ(server->peek(1), (LockWord{0, 1, 0, 1}));
  EXPECT_EQ(server->peek(2), (LockWord{1, 0, 1, 0}));
}

TEST_F(RdmaLoopback, ConcurrentSessionsRespectMutualExclusion) {
  constexpr unsigned kThreads = 4;
  constexpr unsigned kIterations = 500;
  std::mutex mutex;
  int writers = 0, readers = 0, violations = 0;

  std::vector<std::thread> threads;
  for (unsigned t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      auto client = connect();
      Config config;
      config.lease = 50ms;  // Soft-RoCE latencies are in the tens of microseconds
      config.max_poll_interval = 1ms;
      LockSession session(*client, config, t);
      for (unsigned i = 0; i < kIterations; ++i) {
        const LockMode mode = (i + t) % 3 == 0 ? LockMode::Exclusive : LockMode::Shared;
        HeldLock lock;
        ASSERT_EQ(session.acquire(LockRef{0, 7}, mode, lock), AcquireStatus::Acquired);
        {
          std::lock_guard<std::mutex> guard(mutex);
          if (mode == LockMode::Exclusive) {
            if (writers != 0 || readers != 0)
              ++violations;
            ++writers;
          } else {
            if (writers != 0)
              ++violations;
            ++readers;
          }
        }
        {
          std::lock_guard<std::mutex> guard(mutex);
          mode == LockMode::Exclusive ? --writers : --readers;
        }
        ASSERT_TRUE(session.release(lock));
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(violations, 0);
  const LockWord final_word = server->peek(7);
  EXPECT_EQ(final_word.nX, final_word.maxX);
  EXPECT_EQ(final_word.nS, final_word.maxS);
  EXPECT_EQ(unsigned{final_word.maxX} + final_word.maxS, kThreads * kIterations);
}

TEST_F(RdmaLoopback, ServerDropsDisconnectedClients) {
  {
    auto client = connect();
    EXPECT_EQ(server->client_count(), 1u);
  }
  // The server notices the closed bootstrap connection asynchronously.
  for (int i = 0; i < 100 && server->client_count() != 0; ++i) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(server->client_count(), 0u);
}

}  // namespace
}  // namespace dslr
