// End-to-end tests over real ibverbs: a LockTableServer and LockTableClients in this process,
// connected through the (first) RDMA device in loopback. They are skipped when the machine has
// no RDMA device; a Soft-RoCE (rdma_rxe) device is sufficient. Set DSLR_TEST_DEVICE to pick a
// device and DSLR_TEST_GID_INDEX to override the GID selection.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dslr/lock_session.h"
#include "dslr/lock_table_client.h"
#include "dslr/lock_table_server.h"
#include "dslr/rdma/bootstrap.h"
#include "dslr/rdma/device.h"
#include "dslr/rdma/error.h"
#include "dslr/transaction.h"

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

// A peer that cannot connect its queue pair to us closes the bootstrap connection without an
// answer. The client must report that promptly instead of waiting for the kernel to time out
// the TCP connection (which never happens while the server process is alive).
TEST_F(RdmaLoopback, HandshakeFailureIsReportedPromptlyAndLogged) {
  std::mutex mutex;
  std::vector<std::string> errors;
  ServerOptions options;
  options.device = test_device_options();
  options.lock_count = 4;
  options.bind_address = "127.0.0.1";
  options.port = 0;
  options.on_handshake_error = [&](const std::string& error) {
    std::lock_guard<std::mutex> guard(mutex);
    errors.push_back(error);
  };
  LockTableServer picky(options);
  picky.start();

  // Introduce ourselves with an endpoint that cannot be connected to: QP number 0 with a GID
  // that routes nowhere. The server's ibv_modify_qp(RTR) fails and it must hang up on us.
  //
  // The socket timeout is deliberately far longer than the bound asserted below: the only way
  // the client can fail in time is the server closing the connection, not the timeout.
  rdma::UniqueFd control = rdma::tcp_connect("127.0.0.1", picky.port(), 20s);
  rdma::EndpointInfo bogus;
  bogus.qp_num = 0;
  bogus.psn = 1;
  bogus.gid = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 198, 18, 0, 1};  // 198.18.0.1 (RFC 2544)
  const auto t0 = std::chrono::steady_clock::now();
  rdma::send_endpoint(control.get(), bogus);
  std::string what;
  try {
    rdma::recv_endpoint(control.get());
    ADD_FAILURE() << "the server answered a handshake that cannot succeed";
  } catch (const rdma::RdmaError& e) {
    what = e.what();
  }
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  // The kernel gives up resolving the unreachable GID after about a second; the server must
  // then hang up right away rather than leave us waiting on the socket.
  EXPECT_LT(elapsed, 10s);
  EXPECT_NE(what.find("closed by peer"), std::string::npos) << what;

  for (int i = 0; i < 100 && picky.client_count() != 0; ++i) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(picky.client_count(), 0u);
  std::lock_guard<std::mutex> guard(mutex);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].find("ibv_modify_qp"), std::string::npos) << errors[0];
}

// The client must not wait forever for a server that accepts the TCP connection but never
// completes the handshake (a stuck or foreign process on the port).
TEST_F(RdmaLoopback, ClientGivesUpOnASilentServer) {
  uint16_t port = 0;
  rdma::UniqueFd listener = rdma::tcp_listen("127.0.0.1", 0, &port);
  std::thread accepter([&] {
    try {
      rdma::UniqueFd fd = rdma::tcp_accept(listener);
      rdma::wait_for_peer_close(fd.get());  // never answer, just hold the connection
    } catch (const rdma::RdmaError&) {
    }
  });
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_THROW(LockTableClient(*device, std::vector<ServerAddress>{{"127.0.0.1", port}},
                               rdma::ConnectionOptions{}, 300ms),
               rdma::RdmaError);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  EXPECT_GE(elapsed, 250ms);
  EXPECT_LT(elapsed, 5s);
  listener.shutdown();
  accepter.join();
}

// When a lock table server dies, the RC transport reports the dead peer as a failed completion
// after its retries are exhausted. That must surface as an RdmaError from the accessor, so the
// protocol layer and the application can react, and the connection must stay unusable.
TEST_F(RdmaLoopback, OperationsFailOnceTheServerIsGone) {
  rdma::ConnectionOptions options;
  options.retry_count = 0;  // fail fast: one attempt only (~0.5 s on mlx5)
  options.completion_timeout = 30s;
  auto client = std::make_unique<LockTableClient>(
      *device, std::vector<ServerAddress>{{"127.0.0.1", server->port()}}, options);
  EXPECT_EQ(client->read(LockRef{0, 0}), 0u);

  server->stop();  // destroys the server-side queue pair, like a crashed process would
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_THROW(client->fetch_add(LockRef{0, 0}, 1), rdma::RdmaError);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 20s);
  // Every later operation fails immediately: the queue pair is in the error state.
  EXPECT_THROW(client->read(LockRef{0, 0}), rdma::RdmaError);

  // A Transaction being destroyed on a dead connection must not propagate the exception.
  LockSession session(*client);
  EXPECT_NO_THROW({
    Transaction txn(session);
    EXPECT_THROW(txn.lock(LockRef{0, 1}, LockMode::Exclusive), rdma::RdmaError);
  });
}

}  // namespace
}  // namespace dslr
