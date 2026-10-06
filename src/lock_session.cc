// Implementation of the DSLR protocol (Yoon, Chowdhury, Mozafari: "Distributed Lock Management
// with RDMA: Decentralization without Starvation", SIGMOD 2018), Algorithms 1-3.
//
// Reading guide. A lock object is a 64-bit word {nX, nS, maxX, maxS}. Drawing a ticket is a
// single fetch-and-add on maxX or maxS; the returned previous value (`prev`) is the ticket. A
// ticket is served when the "now serving" counters have caught up with it:
//
//   shared    ticket:  nX == prev.maxX                      (all earlier writers are done)
//   exclusive ticket:  nX == prev.maxX && nS == prev.maxS   (all earlier writers and readers)
//
// Releasing is a fetch-and-add on nX or nS. Because fetch-and-add never fails there are no blind
// retries, and because tickets are handed out in arrival order the protocol is first-come
// first-served (Appendix A.2). CAS is only used to repair lock objects: skipping the tickets of
// failed transactions after a lease-based stall timeout, resetting counters before they
// overflow, and (in this implementation) withdrawing a ticket drawn from a frozen lock object.
//
// Every public entry point cites the pseudocode lines it implements; docs/algorithm.md explains
// the few places where this implementation is stricter than the pseudocode and why.

#include "dslr/lock_session.h"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <string>

namespace dslr {
namespace {

Segment ticket_segment(LockMode mode) {
  return mode == LockMode::Shared ? Segment::MaxS : Segment::MaxX;
}

Segment serving_segment(LockMode mode) {
  return mode == LockMode::Shared ? Segment::NS : Segment::NX;
}

uint64_t random_seed() {
  return std::random_device{}();
}

}  // namespace

namespace detail {

bool reset_observed(const LockWord& before, const LockWord& after, uint16_t count_max) {
  const bool frozen_before = before.maxX >= count_max || before.maxS >= count_max;
  const bool frozen_after = after.maxX >= count_max || after.maxS >= count_max;
  return after.nX < before.nX || after.nS < before.nS || (frozen_before && !frozen_after);
}

bool generation_changed(const LockWord& ticket, const LockWord& now) {
  // All four counters only grow between two resets. `ticket` was observed on an unfrozen word,
  // so it carries no transient increments that could later be withdrawn.
  return now.nX < ticket.nX || now.nS < ticket.nS || now.maxX < ticket.maxX ||
         now.maxS < ticket.maxS;
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

Duration LockSession::StallTimer::observe(const LockWord& word, Clock::time_point issued_at,
                                          Clock::time_point completed_at) {
  if (word.nX != nX || word.nS != nS) {
    nX = word.nX;
    nS = word.nS;
    since = completed_at;
    return Duration::zero();
  }
  return issued_at > since ? std::chrono::duration_cast<Duration>(issued_at - since)
                           : Duration::zero();
}

LockSession::LockSession(LockWordAccessor& accessor, Config config, std::optional<uint64_t> seed)
    : accessor_(accessor),
      config_(config),
      backoff_(config.backoff_base, config.backoff_max, seed.value_or(random_seed())) {
  config_.validate();
}

// A lock object is frozen once a "next ticket" counter reached COUNT_MAX (Section 4.9): no new
// tickets may be drawn until the designated resetter sets the whole word back to zero.
bool LockSession::is_frozen(const LockWord& word) const {
  return word.maxX >= config_.count_max || word.maxS >= config_.count_max;
}

// Algorithm 1 lines 12 and 26 / Algorithm 2 lines 5-10.
bool LockSession::ticket_served(const LockWord& now, const HeldLock& lock) {
  const LockWord& t = lock.ticket;
  if (lock.mode == LockMode::Shared) {
    return now.nX == t.maxX;
  }
  return now.nX == t.maxX && now.nS == t.maxS;
}

// Algorithm 2 lines 3-4: the counters moved past our ticket, which only happens when another
// transaction skipped us after a stall (we must have looked dead to it).
//
// nX passing our view is conclusive for both modes: no exclusive ticket drawn after ours can be
// released before we are done. nS is only conclusive for an exclusive ticket. A shared ticket is
// granted together with every other shared ticket that waited for the same writer, and a faster
// sibling may release before we have even noticed our grant, so nS > prev(maxS) is normal for a
// shared waiter (the paper's pseudocode tests it for both modes; see docs/algorithm.md).
bool LockSession::ticket_skipped(const LockWord& now, const HeldLock& lock) {
  if (now.nX > lock.ticket.maxX) {
    return true;
  }
  return lock.mode == LockMode::Exclusive && now.nS > lock.ticket.maxS;
}

// wait_count of Algorithm 2 line 20: tickets drawn before ours that are still unreleased.
unsigned LockSession::outstanding_before(const LockWord& now, const LockWord& ticket) {
  const unsigned x = ticket.maxX > now.nX ? ticket.maxX - now.nX : 0u;
  const unsigned s = ticket.maxS > now.nS ? ticket.maxS - now.nS : 0u;
  return x + s;
}

// All tickets of the object that are drawn but not yet released, as seen by someone without a
// ticket of their own. Used by the frozen-object path to scale the stall threshold.
unsigned LockSession::outstanding_total(const LockWord& now) {
  return outstanding_before(now, LockWord{0, 0, now.maxX, now.maxS});
}

Duration LockSession::stall_threshold(unsigned wait_count) const {
  // Section 5.1: with multi-slot leases a waiter does not know how many slots the transaction in
  // front holds, but it knows the number of outstanding tickets ahead of it, which bounds them.
  const unsigned scale = config_.multi_slot_leasing() ? std::max(1u, wait_count) : 1u;
  return config_.lease * config_.stall_multiplier * scale;
}

Duration LockSession::poll_interval(unsigned wait_count) const {
  // Dynamic interval polling (Section 4.6): the more tickets ahead of us, the longer we sleep
  // between two READs of the lock object.
  const Duration wanted = config_.poll_unit * std::max(1u, wait_count);
  return std::min(wanted, config_.max_poll_interval);
}

// ---------------------------------------------------------------------------------------------
// Acquisition (Algorithm 1)
// ---------------------------------------------------------------------------------------------

AcquireStatus LockSession::acquire(LockRef ref, LockMode mode, HeldLock& out, uint16_t slots) {
  for (unsigned retries = 0;; ++retries) {
    const AcquireStatus status = try_acquire(ref, mode, out, slots);
    if (status != AcquireStatus::Retry) {
      return status;
    }
    if (retries + 1 >= config_.max_frozen_retries) {
      return AcquireStatus::Aborted;
    }
  }
}

AcquireStatus LockSession::try_acquire(LockRef ref, LockMode mode, HeldLock& out, uint16_t slots) {
  if (slots < 1 || slots > config_.max_slots) {
    throw std::invalid_argument("slots must be in [1, Config::max_slots]");
  }
  ++stats_.acquire_attempts;

  // Lines 3 / 17: draw a ticket. This single FA either grants the lock outright or enqueues us.
  // The time *before* the FA is the last instant at which we certainly did not hold the lock; the
  // lease is measured from such a lower bound so that a delayed completion can never make us
  // believe the lease started later than a waiter's stall timer did.
  const Segment segment = ticket_segment(mode);
  const Clock::time_point issued_at = Clock::now();
  const LockWord prev = LockWord::decode(accessor_.fetch_add(ref, segment_add(segment, slots)));
  const Clock::time_point completed_at = Clock::now();

  // Lines 4-9 / 18-23: a counter already reached COUNT_MAX, so the object is frozen until it is
  // reset. Withdraw the ticket we just drew, back off, and let the caller try again.
  if (is_frozen(prev)) {
    undo_fetch_add(ref, prev, segment, slots);
    return back_off_from_frozen_lock(ref, prev, issued_at, completed_at);
  }
  frozen_locks_.erase(ref);

  const unsigned next_ticket = unsigned{prev.get(segment)} + slots;
  if (next_ticket > 0xFFFFu) {
    // Our FA carried into the neighbouring segment. The paper rules this out by assumption
    // (fewer than 32,767 concurrent requesters per lock object); there is no safe recovery.
    throw std::runtime_error("lock object " + to_string(ref) + " overflowed: " + to_string(prev));
  }

  HeldLock lock;
  lock.ref = ref;
  lock.mode = mode;
  lock.slots = slots;
  lock.ticket = prev;
  lock.granted_at = issued_at;

  // Lines 10-11 / 24-25: our ticket is the last one before COUNT_MAX, so we become responsible
  // for resetting the counters once everybody ahead of us (and we) released the lock. ResetFrom
  // is the value the object will have at that point: every counter equal to its "next ticket".
  if (next_ticket >= config_.count_max) {
    const auto last = static_cast<uint16_t>(next_ticket);
    lock.reset_from = mode == LockMode::Shared
                          ? LockWord{prev.maxX, last, prev.maxX, last}.encode()
                          : LockWord{last, prev.maxS, last, prev.maxS}.encode();
  }

  // Lines 12-13 / 26-27: nobody ahead of us, the FA alone acquired the lock.
  if (ticket_served(prev, lock)) {
    ++stats_.acquired_immediately;
    out = lock;
    return AcquireStatus::Acquired;
  }
  // Lines 15 / 29.
  return handle_conflict(lock, out, completed_at);
}

// ---------------------------------------------------------------------------------------------
// Waiting for preceding tickets (Algorithm 2)
// ---------------------------------------------------------------------------------------------

AcquireStatus LockSession::handle_conflict(HeldLock& lock, HeldLock& out,
                                           Clock::time_point fa_completed_at) {
  LockWord last_seen = lock.ticket;
  StallTimer stall(lock.ticket, fa_completed_at);
  // Issue time of the most recent operation that showed our ticket unserved (the FA so far).
  Clock::time_point last_unserved_at = lock.granted_at;

  while (true) {
    // Line 2.
    ++stats_.polls;
    const Clock::time_point issued_at = Clock::now();
    const LockWord val = LockWord::decode(accessor_.read(lock.ref));
    const Clock::time_point completed_at = Clock::now();

    // Lines 3-4: somebody skipped our ticket after a stall; we must restart the transaction.
    if (ticket_skipped(val, lock)) {
      ++stats_.aborted_skipped;
      return AcquireStatus::Aborted;
    }
    // The object was reset to zero behind our back (a stall reset of a frozen object, Section
    // 4.7). Our ticket now belongs to an old generation and must not be used.
    if (detail::reset_observed(last_seen, val, config_.count_max)) {
      ++stats_.aborted_by_reset;
      return AcquireStatus::Aborted;
    }
    last_seen = val;

    // Lines 5-10: our turn. The counters changed somewhere between the previous READ and this
    // one, so the lease conservatively starts at the previous READ.
    if (ticket_served(val, lock)) {
      ++stats_.acquired_after_wait;
      lock.granted_at = last_unserved_at;
      out = lock;
      return AcquireStatus::Acquired;
    }
    last_unserved_at = issued_at;

    // Lines 11-19: the counters have not moved for too long, so the transaction in front of us
    // must have failed or be deadlocked. Advance the counters past the stalled tickets with CAS,
    // which (a) lets everybody behind us proceed and (b) makes everybody in front of us,
    // including the culprit, fail when they next look at the object. We fail as well and retry
    // from scratch, exactly as the paper prescribes (Section 4.7).
    //
    // What the skip declares "done" depends on our mode:
    //  - exclusive (paper, line 15): every exclusive ticket up to and including ours (nX :=
    //    prev(maxX) + k) and every shared ticket before ours (nS := prev(maxS)). Shared waiters
    //    with the same view as ours are evicted by the new nX, so none of those tickets can be
    //    released a second time.
    //  - shared: the exclusive tickets we were waiting for (nX := prev(maxX)) and our own ticket
    //    (nS := nS + k). Shared waiters with the same view are legitimately granted together
    //    with the skip and will release their tickets themselves, so unlike the paper's line 13
    //    we must not count them here or nS would run ahead of the real releases.
    const unsigned wait_count = outstanding_before(val, lock.ticket);
    if (stall.observe(val, issued_at, completed_at) >= stall_threshold(wait_count)) {
      const LockWord& t = lock.ticket;
      const LockWord reset_val =
          lock.mode == LockMode::Shared
              ? LockWord{t.maxX, static_cast<uint16_t>(val.nS + lock.slots), val.maxX, val.maxS}
              : LockWord{static_cast<uint16_t>(t.maxX + lock.slots), t.maxS, val.maxX, val.maxS};
      const uint64_t seen = accessor_.compare_swap(lock.ref, val.encode(), reset_val.encode());
      if (seen == val.encode()) {
        ++stats_.stall_skips;
        // Lines 17-18: when our ticket was the last one before COUNT_MAX we are also the
        // designated resetter, and nobody else will reset the frozen object for us.
        if (lock.reset_from != 0) {
          drain_and_reset(lock, reset_val);
        }
        return AcquireStatus::Aborted;
      }
      // The CAS lost against a concurrent update, which means the object changed: re-read and
      // re-evaluate (the stall timer restarts if the counters moved).
      continue;
    }

    // Line 20: dynamic interval polling.
    sleep_for_precise(poll_interval(wait_count));
  }
}

// ---------------------------------------------------------------------------------------------
// Frozen lock objects (Algorithm 1 lines 4-9 / 18-23, Section 4.9)
// ---------------------------------------------------------------------------------------------

// Reverts a fetch-and-add of `amount` on `segment` that returned `prev`. Used to withdraw a
// ticket drawn from a frozen object and to take back a release increment that turned out to be
// stray (see release()).
//
// The paper withdraws tickets with FA(L, maxX/maxS, -1). We use a CAS loop instead: a decrement
// that lands *after* the object was reset to zero would borrow into the neighbouring segments
// and corrupt the whole word, whereas a CAS only ever modifies the exact value we observed and
// simply gives up once the reset is visible. See docs/algorithm.md.
void LockSession::undo_fetch_add(LockRef ref, const LockWord& prev, Segment segment,
                                 uint16_t amount) {
  const uint64_t addend = segment_add(segment, amount);
  uint64_t expected = prev.encode() + addend;  // the word right after our FA
  for (unsigned failures = 0;; ++failures) {
    const uint64_t seen = accessor_.compare_swap(ref, expected, expected - addend);
    if (seen == expected) {
      return;
    }
    const LockWord current = LockWord::decode(seen);
    // Our increment was wiped by a reset to zero together with everything else: nothing to undo.
    // (`get(segment) < amount` can only happen after a reset too and guards the subtraction.)
    if (detail::reset_observed(prev, current, config_.count_max) || current.get(segment) < amount) {
      return;
    }
    ++stats_.undo_cas_retries;
    expected = seen;
    sleep_for_precise(backoff_.next(failures + 1));
  }
}

AcquireStatus LockSession::back_off_from_frozen_lock(LockRef ref, const LockWord& prev,
                                                     Clock::time_point fa_issued_at,
                                                     Clock::time_point fa_completed_at) {
  ++stats_.frozen_retries;
  if (frozen_locks_.size() >= kMaxTrackedFrozenLocks && !frozen_locks_.contains(ref)) {
    // Entries are dropped once a lock is seen unfrozen; a session that merely brushed against
    // many frozen objects would otherwise accumulate them. Forgetting only delays forced resets.
    frozen_locks_.clear();
  }
  FrozenLockState& state = frozen_locks_[ref];
  if (state.consecutive_failures == 0) {
    state.stall = StallTimer(prev, fa_completed_at);
  }
  ++state.consecutive_failures;
  const Duration stalled_for = state.stall.observe(prev, fa_issued_at, fa_completed_at);
  const Duration threshold = stall_threshold(outstanding_total(prev));

  // Line 6 / 20: random backoff gives the resetter a quiet window for its CAS (Appendix A.3).
  sleep_for_precise(backoff_.next(state.consecutive_failures));

  // Lines 7-8 / 21-22: the frozen object made no progress for a whole stall period, so its
  // resetter (or a transaction it is waiting for) must have failed. Reset it ourselves.
  if (stalled_for >= threshold) {
    force_reset_if_stalled(ref);  // may erase `state`
  }
  // Line 9 / 23.
  return AcquireStatus::Retry;
}

void LockSession::force_reset_if_stalled(LockRef ref) {
  const Clock::time_point issued_at = Clock::now();
  const LockWord current = LockWord::decode(accessor_.read(ref));
  const Clock::time_point completed_at = Clock::now();
  if (!is_frozen(current)) {
    frozen_locks_.erase(ref);  // somebody else already reset it
    return;
  }
  FrozenLockState& state = frozen_locks_[ref];
  if (state.stall.observe(current, issued_at, completed_at) <
      stall_threshold(outstanding_total(current))) {
    return;  // progress after all; keep waiting
  }
  ++stats_.forced_resets;
  reset_to_zero(ref, current);
  frozen_locks_.erase(ref);
}

// "Repeat CAS(L, current, 0) until it succeeds" (Algorithm 3 line 7). Every failed CAS returns
// the value that beat us, which becomes the next expected value; we stop as soon as the object
// is no longer frozen because that means another node completed the reset.
void LockSession::reset_to_zero(LockRef ref, LockWord current) {
  for (unsigned failures = 0; is_frozen(current); ++failures) {
    const uint64_t seen = accessor_.compare_swap(ref, current.encode(), 0);
    if (seen == current.encode()) {
      ++stats_.counter_resets;
      return;
    }
    current = LockWord::decode(seen);
    sleep_for_precise(backoff_.next(failures + 1));
  }
}

// ---------------------------------------------------------------------------------------------
// Release (Algorithm 3)
// ---------------------------------------------------------------------------------------------

// A ticket is revoked once the lock object shows that the other transactions moved on without
// us: a waiter skipped our ticket after a stall, or the object was reset to zero (every counter
// of the new generation is smaller than our ticket's, and for the designated resetter the
// object is simply no longer frozen). Releasing a revoked ticket would hand the lock to the
// wrong transaction, so release() must not increment anything in that case.
bool LockSession::ticket_revoked(const LockWord& now, const HeldLock& lock) const {
  return ticket_skipped(now, lock) || detail::generation_changed(lock.ticket, now) ||
         (lock.reset_from != 0 && !is_frozen(now));
}

bool LockSession::release(const HeldLock& lock) {
  ++stats_.releases;
  const Duration lease = config_.lease * lock.slots;  // k slots buy a k times longer lease
  const bool within_lease = Clock::now() - lock.granted_at < lease;
  const Segment segment = serving_segment(lock.mode);
  const uint64_t addend = segment_add(segment, lock.slots);
  LockWord after;  // the object right after our increment; the resetter path needs it

  if (within_lease) {
    // Lines 1-5: hand the lock to the next ticket with a single FA. While the lease runs nobody
    // may skip our ticket (a skip needs stall_multiplier >= 2 leases of silence), so the FA
    // normally needs no protection.
    const LockWord prev = LockWord::decode(accessor_.fetch_add(lock.ref, addend));
    if (ticket_revoked(prev, lock)) {
      // Normally, but the lease check and the FA are not one atomic step. If this thread was
      // descheduled in between for longer than the slack between lease expiry and the stall
      // threshold, a waiter has skipped our ticket meanwhile and our increment is a stray one:
      // on nX it lets the ticket behind the new holder in early, on nS it makes every later
      // exclusive ticket look skipped until the next counter reset. The FA's return value
      // exposes this exactly, so take the increment back (docs/algorithm.md, deviation 7.6).
      ++stats_.stray_releases;
      ++stats_.revoked_releases;
      undo_fetch_add(lock.ref, prev, segment, lock.slots);
      return false;
    }
    after = LockWord::decode(prev.encode() + addend);
  } else {
    // Line 1 in the paper: a holder whose lease expired does not release at all, because a
    // waiter may already have skipped its ticket and a second increment would let a later
    // ticket in early. We do slightly better: look at the object and release with CAS unless
    // the ticket was indeed skipped or the object was reset meanwhile. A skip racing with our
    // CAS makes the CAS fail and the re-read notices the skip.
    LockWord current = LockWord::decode(accessor_.read(lock.ref));
    while (true) {
      if (ticket_revoked(current, lock)) {
        ++stats_.revoked_releases;
        return false;
      }
      const uint64_t seen =
          accessor_.compare_swap(lock.ref, current.encode(), current.encode() + addend);
      if (seen == current.encode()) {
        ++stats_.late_releases;
        after = LockWord::decode(seen + addend);
        break;
      }
      current = LockWord::decode(seen);
    }
  }

  // Lines 6-7: our ticket pushed a counter to COUNT_MAX, so the object is frozen and we must
  // reset it once everybody ahead of us is done.
  if (lock.reset_from != 0) {
    drain_and_reset(lock, after);
  }
  return true;
}

// Waits until every ticket before ours has been released (or skipped after a stall), then resets
// the lock object to zero. ResetFrom (`lock.reset_from`) is the value the object reaches when
// every ticket up to ours is released; its "now serving" counters are the drain target.
//
// An exclusive resetter never actually waits here: it was only granted the lock after all
// preceding tickets had been released. A shared resetter waits for the shared holders that were
// granted together with it.
void LockSession::drain_and_reset(const HeldLock& lock, LockWord current) {
  const LockWord target = LockWord::decode(lock.reset_from);
  Clock::time_point issued_at = Clock::now();
  Clock::time_point completed_at = issued_at;
  StallTimer stall(current, completed_at);

  while (true) {
    if (!is_frozen(current)) {
      return;  // another node reset the object in the meantime
    }
    if (current.nX >= target.nX && current.nS >= target.nS) {
      reset_to_zero(lock.ref, current);
      return;
    }

    const unsigned wait_count = outstanding_before(current, target);
    if (stall.observe(current, issued_at, completed_at) >= stall_threshold(wait_count)) {
      // The transactions ahead of us failed. Skip their tickets the same way Algorithm 2 line 16
      // does, keeping maxX/maxS untouched so that tickets being withdrawn concurrently stay
      // consistent. For a shared resetter, nX is pushed one past our own view as well: shared
      // holders of this generation that are still around then see their ticket as skipped and
      // cannot release into the freshly reset object.
      const uint16_t evict = lock.mode == LockMode::Shared ? 1 : 0;
      const LockWord skipped{
          std::max<uint16_t>(current.nX, static_cast<uint16_t>(target.nX + evict)),
          std::max(current.nS, target.nS), current.maxX, current.maxS};
      const uint64_t seen = accessor_.compare_swap(lock.ref, current.encode(), skipped.encode());
      if (seen == current.encode()) {
        ++stats_.stall_skips;
        current = skipped;
      } else {
        current = LockWord::decode(seen);
      }
      issued_at = completed_at = Clock::now();
      continue;
    }

    sleep_for_precise(poll_interval(wait_count));
    ++stats_.polls;
    issued_at = Clock::now();
    current = LockWord::decode(accessor_.read(lock.ref));
    completed_at = Clock::now();
  }
}

}  // namespace dslr
