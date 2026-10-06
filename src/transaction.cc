#include "dslr/transaction.h"

namespace dslr {

Transaction::~Transaction() {
  try {
    release_all();
  } catch (...) {
    // A broken connection while unwinding: the leases of the remaining tickets will expire and
    // other transactions skip them (Section 4.7). Destructors must not throw.
  }
}

bool Transaction::lock(LockRef ref, LockMode mode, uint16_t slots) {
  HeldLock held;
  const AcquireStatus status = session_.acquire(ref, mode, held, slots);
  if (status == AcquireStatus::Acquired) {
    held_.push_back(held);
    return true;
  }
  aborted_ = true;
  release_all();
  return false;
}

bool Transaction::release_all() {
  bool all_valid = true;
  while (!held_.empty()) {
    const HeldLock lock = held_.back();
    held_.pop_back();
    all_valid = session_.release(lock) && all_valid;
  }
  return all_valid;
}

}  // namespace dslr
