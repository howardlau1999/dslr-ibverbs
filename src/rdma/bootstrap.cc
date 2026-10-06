#include "dslr/rdma/bootstrap.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "dslr/rdma/error.h"

namespace dslr::rdma {
namespace {

constexpr uint8_t kMagic[4] = {'D', 'S', 'L', 'R'};
constexpr uint16_t kProtocolVersion = 1;

void send_exactly(int fd, const void* data, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  while (length > 0) {
    const ssize_t n = ::send(fd, bytes, length, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("send");
    }
    bytes += n;
    length -= static_cast<size_t>(n);
  }
}

void recv_exactly(int fd, void* data, size_t length) {
  auto* bytes = static_cast<uint8_t*>(data);
  while (length > 0) {
    const ssize_t n = ::recv(fd, bytes, length, 0);
    if (n == 0) {
      throw RdmaError("bootstrap connection closed by peer");
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("recv");
    }
    bytes += n;
    length -= static_cast<size_t>(n);
  }
}

void enable_keepalive(int fd) {
  // Probe an idle peer every 5 s after 10 s of silence; give up after 3 missed probes. This is
  // our stand-in for the 10 s heartbeat the paper uses to detect node failures.
  int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
  int idle = 10, interval = 5, count = 3;
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

}  // namespace

void UniqueFd::reset(int fd) {
  if (fd_ >= 0) {
    ::close(fd_);
  }
  fd_ = fd;
}

void UniqueFd::shutdown() noexcept {
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_RDWR);
  }
}

UniqueFd tcp_listen(const std::string& bind_address, uint16_t port, uint16_t* bound_port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (const int rc = ::getaddrinfo(bind_address.c_str(), service.c_str(), &hints, &results);
      rc != 0) {
    throw RdmaError("getaddrinfo(" + bind_address + "): " + gai_strerror(rc));
  }

  UniqueFd listener;
  std::string last_error = "no addresses";
  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    UniqueFd fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (!fd.valid()) {
      last_error = std::strerror(errno);
      continue;
    }
    int on = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (::bind(fd.get(), ai->ai_addr, ai->ai_addrlen) != 0 || ::listen(fd.get(), 64) != 0) {
      last_error = std::strerror(errno);
      continue;
    }
    listener = std::move(fd);
    break;
  }
  ::freeaddrinfo(results);
  if (!listener.valid()) {
    throw RdmaError("cannot listen on " + bind_address + ":" + service + ": " + last_error);
  }

  if (bound_port != nullptr) {
    sockaddr_storage local{};
    socklen_t len = sizeof(local);
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&local), &len) != 0) {
      throw_errno("getsockname");
    }
    *bound_port = local.ss_family == AF_INET6
                      ? ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port)
                      : ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
  }
  return listener;
}

UniqueFd tcp_accept(const UniqueFd& listener) {
  while (true) {
    UniqueFd fd(::accept(listener.get(), nullptr, nullptr));
    if (fd.valid()) {
      enable_keepalive(fd.get());
      return fd;
    }
    if (errno == EINTR) {
      continue;
    }
    throw_errno("accept");
  }
}

UniqueFd tcp_connect(const std::string& host, uint16_t port, std::chrono::milliseconds timeout) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results); rc != 0) {
    throw RdmaError("getaddrinfo(" + host + "): " + gai_strerror(rc));
  }

  std::string last_error = "no addresses";
  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    UniqueFd fd(::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol));
    if (!fd.valid()) {
      last_error = std::strerror(errno);
      continue;
    }
    int rc = ::connect(fd.get(), ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
      pollfd pfd{fd.get(), POLLOUT, 0};
      rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
      if (rc == 0) {
        last_error = "connection timed out";
        continue;
      }
      int err = 0;
      socklen_t len = sizeof(err);
      ::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &err, &len);
      rc = err == 0 ? 0 : -1;
      errno = err;
    }
    if (rc != 0) {
      last_error = std::strerror(errno);
      continue;
    }
    // Back to blocking mode for the simple exchange that follows.
    ::fcntl(fd.get(), F_SETFL, ::fcntl(fd.get(), F_GETFL) & ~O_NONBLOCK);
    enable_keepalive(fd.get());
    ::freeaddrinfo(results);
    return fd;
  }
  ::freeaddrinfo(results);
  throw RdmaError("cannot connect to " + host + ":" + service + ": " + last_error);
}

void send_endpoint(int fd, const EndpointInfo& info) {
  uint8_t header[6] = {kMagic[0],
                       kMagic[1],
                       kMagic[2],
                       kMagic[3],
                       static_cast<uint8_t>(kProtocolVersion >> 8),
                       static_cast<uint8_t>(kProtocolVersion)};
  send_exactly(fd, header, sizeof(header));
  const auto body = info.to_wire();
  send_exactly(fd, body.data(), body.size());
}

EndpointInfo recv_endpoint(int fd) {
  uint8_t header[6];
  recv_exactly(fd, header, sizeof(header));
  if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
    throw RdmaError("bootstrap peer is not a DSLR endpoint (bad magic)");
  }
  const uint16_t version = static_cast<uint16_t>((header[4] << 8) | header[5]);
  if (version != kProtocolVersion) {
    throw RdmaError("bootstrap protocol version mismatch: peer speaks " + std::to_string(version));
  }
  std::array<uint8_t, EndpointInfo::kWireSize> body{};
  recv_exactly(fd, body.data(), body.size());
  return EndpointInfo::from_wire(body);
}

void wait_for_peer_close(int fd) {
  uint8_t byte = 0;
  while (true) {
    const ssize_t n = ::recv(fd, &byte, 1, 0);
    if (n == 0) {
      return;  // orderly close
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      return;  // reset, keepalive failure, or our own shutdown()
    }
    // Unexpected payload: the protocol sends nothing after the handshake; ignore it.
  }
}

}  // namespace dslr::rdma
