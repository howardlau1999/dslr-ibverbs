#pragma once

#include <stdexcept>
#include <string>

namespace dslr::rdma {

/// Raised for every failure of the RDMA layer: verbs calls, connection setup, failed work
/// completions and bootstrap (TCP) errors. The message names the failing call and the errno text.
class RdmaError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Throws RdmaError("<what>: <strerror(err)>"). `err` defaults to the current errno.
[[noreturn]] void throw_errno(const std::string& what, int err = 0);

}  // namespace dslr::rdma
