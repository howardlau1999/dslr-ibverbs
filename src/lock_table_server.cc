#include "dslr/lock_table_server.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "dslr/rdma/atomic_probe.h"
#include "dslr/rdma/error.h"

namespace dslr {
namespace {

constexpr size_t kTableAlignment = 4096;

}  // namespace

LockTableServer::LockTableServer(const ServerOptions& options)
    : options_(options), device_(options.device) {
  if (options.lock_count == 0) {
    throw std::invalid_argument("a lock table needs at least one lock object");
  }
  if (!device_.supports_atomics()) {
    throw rdma::RdmaError("device " + device_.name() + " does not support RDMA atomics");
  }
  byte_order_ = device_.atomic_byte_order();

  // Page-aligned, zero-filled table. RDMA atomics additionally need every lock object to be
  // 8-byte aligned, which the 8-byte element size guarantees.
  const size_t bytes = size_t{options.lock_count} * sizeof(uint64_t);
  void* memory = nullptr;
  if (::posix_memalign(&memory, kTableAlignment, bytes) != 0) {
    throw std::bad_alloc();
  }
  std::memset(memory, 0, bytes);
  table_ = static_cast<uint64_t*>(memory);

  try {
    table_region_ = std::make_unique<rdma::MemoryRegion>(
        device_, table_, bytes,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
    listener_ = rdma::tcp_listen(options.bind_address, options.port, &port_);
  } catch (...) {
    table_region_.reset();  // deregister before the memory goes away
    std::free(table_);
    throw;
  }
}

LockTableServer::~LockTableServer() {
  stop();
  table_region_.reset();
  std::free(table_);
}

void LockTableServer::start() {
  if (running_.exchange(true)) {
    return;
  }
  acceptor_ = std::thread([this] { accept_loop(); });
}

void LockTableServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  listener_.shutdown();
  if (acceptor_.joinable()) {
    acceptor_.join();
  }
  std::vector<std::unique_ptr<Client>> clients;
  {
    std::lock_guard<std::mutex> guard(clients_mutex_);
    clients.swap(clients_);
  }
  for (auto& client : clients) {
    client->control.shutdown();
    if (client->thread.joinable()) {
      client->thread.join();
    }
  }
}

size_t LockTableServer::client_count() const {
  std::lock_guard<std::mutex> guard(clients_mutex_);
  size_t count = 0;
  for (const auto& client : clients_) {
    if (!client->finished) {
      ++count;
    }
  }
  return count;
}

LockWord LockTableServer::peek(uint32_t index) const {
  if (index >= options_.lock_count) {
    throw std::out_of_range("lock index out of range");
  }
  return LockWord::decode(rdma::from_nic(__atomic_load_n(&table_[index], __ATOMIC_ACQUIRE),
                                         byte_order_.memory_big_endian));
}

void LockTableServer::accept_loop() {
  while (running_.load()) {
    rdma::UniqueFd fd;
    try {
      fd = rdma::tcp_accept(listener_);
    } catch (const rdma::RdmaError&) {
      break;  // listener shut down by stop()
    }
    reap_finished_clients();
    auto client = std::make_unique<Client>();
    client->control = std::move(fd);
    Client* raw = client.get();
    std::lock_guard<std::mutex> guard(clients_mutex_);
    clients_.push_back(std::move(client));
    raw->thread = std::thread([this, raw] { serve_client(*raw); });
  }
}

void LockTableServer::serve_client(Client& client) {
  try {
    // Handshake: the client introduces its queue pair, we answer with ours plus the lock table.
    const rdma::EndpointInfo remote = rdma::recv_endpoint(client.control.get());
    client.connection = std::make_unique<rdma::ReliableConnection>(device_, options_.connection);
    client.connection->connect(remote);
    rdma::send_endpoint(client.control.get(),
                        client.connection->local_endpoint(table_region_.get(), options_.lock_count,
                                                          byte_order_.memory_big_endian));
    // From here on the NIC does all the work; we only wait for the client to go away.
    rdma::wait_for_peer_close(client.control.get());
  } catch (const std::exception& e) {
    // A failed handshake or a vanished client: drop the connection below.
    if (options_.on_handshake_error) {
      options_.on_handshake_error(e.what());
    }
  }
  // Tell the peer right away: a client still waiting for our answer would otherwise block until
  // this descriptor is finally closed, which happens only when the entry is reaped.
  client.control.shutdown();
  client.connection.reset();
  std::lock_guard<std::mutex> guard(clients_mutex_);
  client.finished = true;
}

void LockTableServer::reap_finished_clients() {
  std::vector<std::unique_ptr<Client>> finished;
  {
    std::lock_guard<std::mutex> guard(clients_mutex_);
    for (auto it = clients_.begin(); it != clients_.end();) {
      if ((*it)->finished) {
        finished.push_back(std::move(*it));
        it = clients_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& client : finished) {
    if (client->thread.joinable()) {
      client->thread.join();
    }
  }
}

}  // namespace dslr
