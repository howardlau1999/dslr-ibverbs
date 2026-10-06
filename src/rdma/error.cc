#include "dslr/rdma/error.h"

#include <cerrno>
#include <cstring>

namespace dslr::rdma {

void throw_errno(const std::string& what, int err) {
  if (err == 0) {
    err = errno;
  }
  throw RdmaError(what + ": " + std::strerror(err) + " (errno " + std::to_string(err) + ")");
}

}  // namespace dslr::rdma
