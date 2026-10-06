#include "dslr/lock_table_client.h"

#include <stdexcept>

#include "dslr/rdma/atomic_probe.h"
#include "dslr/rdma/error.h"

namespace dslr {

LockTableClient::LockTableClient(const rdma::Device& device,
                                 const std::vector<ServerAddress>& servers,
                                 rdma::ConnectionOptions options,
                                 std::chrono::milliseconds connect_timeout) {
  if (servers.empty()) {
    throw std::invalid_argument("LockTableClient needs at least one server");
  }
  reply_big_endian_ = device.atomic_byte_order().reply_big_endian;
  tables_.reserve(servers.size());
  for (const ServerAddress& server : servers) {
    const std::string where = server.host + ":" + std::to_string(server.port);
    RemoteTable table;
    table.control = rdma::tcp_connect(server.host, server.port, connect_timeout);
    table.connection = std::make_unique<rdma::ReliableConnection>(device, options);
    // Handshake: we go first, the server answers with its queue pair and lock table.
    rdma::EndpointInfo remote;
    try {
      rdma::send_endpoint(table.control.get(), table.connection->local_endpoint());
      remote = rdma::recv_endpoint(table.control.get());
    } catch (const rdma::RdmaError& e) {
      // The server closes the connection without an answer when it cannot connect its queue
      // pair to us (its log has the reason); say which server so the operator knows where to look.
      throw rdma::RdmaError("handshake with lock table server " + where + " failed: " + e.what());
    }
    if (remote.table_locks == 0) {
      throw rdma::RdmaError(where + " did not export a lock table");
    }
    table.connection->connect(remote);
    table.base_addr = remote.table_addr;
    table.rkey = remote.table_rkey;
    table.lock_count = remote.table_locks;
    table.memory_big_endian = remote.table_big_endian;
    tables_.push_back(std::move(table));
  }
}

LockTableClient::~LockTableClient() = default;

const LockTableClient::RemoteTable& LockTableClient::table(LockRef ref) const {
  if (ref.node >= tables_.size()) {
    throw std::out_of_range("unknown lock table node " + std::to_string(ref.node));
  }
  const RemoteTable& t = tables_[ref.node];
  if (ref.index >= t.lock_count) {
    throw std::out_of_range("lock index " + std::to_string(ref.index) + " out of range for node " +
                            std::to_string(ref.node) + " (" + std::to_string(t.lock_count) +
                            " locks)");
  }
  return t;
}

uint64_t LockTableClient::address(const RemoteTable& table, LockRef ref) {
  return table.base_addr + uint64_t{ref.index} * sizeof(uint64_t);
}

// Operands are always logical (host-order) integers; the provider and NIC encode them. Replies
// of atomics follow our NIC's convention, READ results the server NIC's memory convention.

uint64_t LockTableClient::fetch_add(LockRef ref, uint64_t addend) {
  const RemoteTable& t = table(ref);
  return rdma::from_nic(t.connection->fetch_add(address(t, ref), t.rkey, addend),
                        reply_big_endian_);
}

uint64_t LockTableClient::compare_swap(LockRef ref, uint64_t expected, uint64_t desired) {
  const RemoteTable& t = table(ref);
  return rdma::from_nic(t.connection->compare_swap(address(t, ref), t.rkey, expected, desired),
                        reply_big_endian_);
}

uint64_t LockTableClient::read(LockRef ref) {
  const RemoteTable& t = table(ref);
  return rdma::from_nic(t.connection->read_u64(address(t, ref), t.rkey), t.memory_big_endian);
}

uint32_t LockTableClient::lock_count(uint32_t node) const {
  if (node >= tables_.size()) {
    throw std::out_of_range("unknown lock table node " + std::to_string(node));
  }
  return tables_[node].lock_count;
}

}  // namespace dslr
