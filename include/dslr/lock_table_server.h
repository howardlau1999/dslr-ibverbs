#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dslr/lock_word.h"
#include "dslr/rdma/bootstrap.h"
#include "dslr/rdma/device.h"
#include "dslr/rdma/memory_region.h"
#include "dslr/rdma/reliable_connection.h"

namespace dslr {

struct ServerOptions {
  rdma::DeviceOptions device;
  /// Number of lock objects (8 bytes each) hosted by this node.
  uint32_t lock_count = 1u << 20;
  /// Where the bootstrap listener binds. Port 0 lets the kernel choose (see port()).
  std::string bind_address = "0.0.0.0";
  uint16_t port = 7777;
  rdma::ConnectionOptions connection;
  /// Called from a server thread when a client's handshake fails (malformed bootstrap frame,
  /// queue pair cannot be connected to the client's address, ...). The client only sees its
  /// connection being closed, so this is where the reason can be logged. Optional.
  std::function<void(const std::string& error)> on_handshake_error;
};

/// Hosts one node's lock table and hands out RDMA access to it.
///
/// This is the whole "server": after a client has connected its queue pair, every lock
/// operation is a one-sided RDMA verb executed by the NIC, and the server's CPU is never
/// involved again (Section 2.2.2 of the paper). The only work done here is bootstrapping queue
/// pairs over TCP and tearing them down when clients leave.
class LockTableServer {
 public:
  explicit LockTableServer(const ServerOptions& options);
  ~LockTableServer();

  LockTableServer(const LockTableServer&) = delete;
  LockTableServer& operator=(const LockTableServer&) = delete;

  /// Starts accepting clients in a background thread.
  void start();
  /// Stops accepting, disconnects all clients and joins the background threads.
  void stop();

  uint16_t port() const { return port_; }
  uint32_t lock_count() const { return options_.lock_count; }
  const rdma::Device& device() const { return device_; }
  size_t client_count() const;

  /// Reads a lock object straight from memory (the NIC updates it coherently). For monitoring
  /// and tests; the protocol itself never needs it.
  LockWord peek(uint32_t index) const;

  /// Byte-order convention of the hosting NIC's atomics (probed at construction).
  const rdma::AtomicByteOrder& atomic_byte_order() const { return byte_order_; }

 private:
  struct Client {
    rdma::UniqueFd control;
    std::unique_ptr<rdma::ReliableConnection> connection;
    std::thread thread;
    bool finished = false;
  };

  void accept_loop();
  void serve_client(Client& client);
  void reap_finished_clients();

  ServerOptions options_;
  rdma::Device device_;
  rdma::AtomicByteOrder byte_order_;
  uint64_t* table_ = nullptr;
  std::unique_ptr<rdma::MemoryRegion> table_region_;
  rdma::UniqueFd listener_;
  uint16_t port_ = 0;
  std::thread acceptor_;
  std::atomic<bool> running_{false};
  mutable std::mutex clients_mutex_;
  std::vector<std::unique_ptr<Client>> clients_;
};

}  // namespace dslr
