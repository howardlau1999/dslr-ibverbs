#include "dslr/types.h"

namespace dslr {

std::string to_string(LockMode mode) {
  switch (mode) {
    case LockMode::Shared:
      return "shared";
    case LockMode::Exclusive:
      return "exclusive";
  }
  return "unknown";
}

std::string to_string(AcquireStatus status) {
  switch (status) {
    case AcquireStatus::Acquired:
      return "acquired";
    case AcquireStatus::Retry:
      return "retry";
    case AcquireStatus::Aborted:
      return "aborted";
  }
  return "unknown";
}

std::string to_string(const LockRef& ref) {
  return "node " + std::to_string(ref.node) + " lock " + std::to_string(ref.index);
}

}  // namespace dslr
