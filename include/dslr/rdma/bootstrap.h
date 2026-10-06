#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "dslr/rdma/endpoint.h"

namespace dslr::rdma {

/// Owning wrapper for a file descriptor.
class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}
  ~UniqueFd() { reset(); }

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  int release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }
  void reset(int fd = -1);

  /// Wakes up any thread blocked in accept()/recv() on this descriptor.
  void shutdown() noexcept;

 private:
  int fd_ = -1;
};

/// The bootstrap protocol: a plain TCP connection over which the two sides exchange their
/// EndpointInfo once (client first, then server). The connection is kept open afterwards as a
/// liveness signal: when a client disappears its server drops the queue pair, and TCP keepalive
/// probes play the role of the paper's periodic heartbeats (Section 4.7).

/// Listens on `bind_address:port` (port 0 picks a free one; the chosen port is returned via
/// `bound_port`).
UniqueFd tcp_listen(const std::string& bind_address, uint16_t port, uint16_t* bound_port);

/// Accepts one connection; throws RdmaError if the listening socket was shut down.
UniqueFd tcp_accept(const UniqueFd& listener);

/// Connects to `host:port`. `timeout` bounds the TCP connect and, as a send/receive timeout on
/// the returned socket, the handshake that follows.
UniqueFd tcp_connect(const std::string& host, uint16_t port, std::chrono::milliseconds timeout);

void send_endpoint(int fd, const EndpointInfo& info);
EndpointInfo recv_endpoint(int fd);

/// Blocks until the peer closes the connection (or it breaks). Used by servers to notice
/// departed clients.
void wait_for_peer_close(int fd);

}  // namespace dslr::rdma
