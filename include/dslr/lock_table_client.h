#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dslr/lock_word_accessor.h"
#include "dslr/rdma/bootstrap.h"
#include "dslr/rdma/device.h"
#include "dslr/rdma/reliable_connection.h"

namespace dslr {

struct ServerAddress {
  std::string host;
  uint16_t port = 7777;
};

/// Client-side view of the cluster's lock tables: one RC queue pair per lock table server, and
/// the LockWordAccessor that LockSession drives. LockRef::node indexes the `servers` vector
/// passed to the constructor, so every node must use the same server list.
///
/// Like the queue pairs it owns, a LockTableClient belongs to a single thread. Create one per
/// worker thread (plus a LockSession on top of it).
class LockTableClient final : public LockWordAccessor {
 public:
  LockTableClient(const rdma::Device& device, const std::vector<ServerAddress>& servers,
                  rdma::ConnectionOptions options = {},
                  std::chrono::milliseconds connect_timeout = std::chrono::milliseconds{5000});
  ~LockTableClient() override;

  LockTableClient(const LockTableClient&) = delete;
  LockTableClient& operator=(const LockTableClient&) = delete;

  uint64_t fetch_add(LockRef ref, uint64_t addend) override;
  uint64_t compare_swap(LockRef ref, uint64_t expected, uint64_t desired) override;
  uint64_t read(LockRef ref) override;

  uint32_t node_count() const { return static_cast<uint32_t>(tables_.size()); }
  uint32_t lock_count(uint32_t node) const;

 private:
  struct RemoteTable {
    rdma::UniqueFd control;  ///< bootstrap connection, kept open as a liveness signal
    std::unique_ptr<rdma::ReliableConnection> connection;
    uint64_t base_addr = 0;
    uint32_t rkey = 0;
    uint32_t lock_count = 0;
    bool memory_big_endian = false;  ///< how the server's NIC stores lock words (READ results)
  };

  const RemoteTable& table(LockRef ref) const;
  static uint64_t address(const RemoteTable& table, LockRef ref);

  std::vector<RemoteTable> tables_;
  bool reply_big_endian_ = false;  ///< how our own NIC delivers atomic replies
};

}  // namespace dslr
