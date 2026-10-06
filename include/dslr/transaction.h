#pragma once

#include <vector>

#include "dslr/lock_session.h"

namespace dslr {

/// Convenience wrapper that tracks the locks of one transaction and implements the paper's
/// failure rule: when DSLR aborts an acquisition, *all* locks held so far are released and the
/// caller restarts the transaction from the beginning (Section 4.7).
///
///     for (;;) {
///       dslr::Transaction txn(session);
///       if (!txn.lock(a, dslr::LockMode::Shared) || !txn.lock(b, dslr::LockMode::Exclusive)) {
///         continue;  // aborted: nothing is held any more, try again
///       }
///       ... do the work ...
///       if (txn.commit()) break;  // false: a lease expired and the work must be redone
///     }
///
/// Locks still held when the Transaction is destroyed are released (abort semantics), so an
/// exception inside the critical section cannot leak tickets.
class Transaction {
 public:
  explicit Transaction(LockSession& session) : session_(session) {}
  ~Transaction();

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  /// Acquires `ref`. Returns false when DSLR aborted the transaction; every lock acquired so
  /// far has then been released already.
  bool lock(LockRef ref, LockMode mode, uint16_t slots = 1);

  /// Releases all locks, most recently acquired first. Returns false if any lock could no longer
  /// be released because its lease had expired and other transactions had revoked the ticket
  /// (see LockSession::release); the transaction's work may then have overlapped with a
  /// conflicting transaction and should be considered failed.
  bool commit() { return release_all(); }

  /// Releases all locks. Same operation as commit() for a lock manager; the name documents the
  /// caller's intent.
  void abort() { release_all(); }

  /// Forgets every held lock *without* releasing it. Used by tests and benchmarks to emulate
  /// a transaction that crashed while holding locks (Appendix A.6); the leases of the abandoned
  /// tickets expire and other transactions skip them.
  void abandon() { held_.clear(); }

  const std::vector<HeldLock>& held() const { return held_; }
  bool aborted() const { return aborted_; }

 private:
  bool release_all();

  LockSession& session_;
  std::vector<HeldLock> held_;
  bool aborted_ = false;
};

}  // namespace dslr
