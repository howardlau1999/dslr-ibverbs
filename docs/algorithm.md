# The DSLR protocol, as implemented here

This document walks through the algorithm of *Distributed Lock Management with RDMA:
Decentralization without Starvation* (Yoon, Chowdhury, Mozafari, SIGMOD 2018) and maps every
step of its pseudocode (Algorithms 1–3) to `src/lock_session.cc`. The last section lists the
places where this implementation deliberately differs from the pseudocode, with the reasoning.

## 1. The lock object

A lock object is one 64-bit word in the memory of the node that owns the data item
(`LockWord` in `include/dslr/lock_word.h`). Following Figure 3 of the paper it holds four 16-bit
counters, most significant first:

| segment | bits | bakery meaning |
|---|---|---|
| `nX` | 63..48 | exclusive ticket currently being served |
| `nS` | 47..32 | shared ticket currently being served (number of shared tickets released) |
| `maxX` | 31..16 | next exclusive ticket to hand out |
| `maxS` | 15..0 | next shared ticket to hand out |

`segment_add(segment, k)` builds the 64-bit addend that makes one RDMA fetch-and-add increment a
single segment by `k`: `FA(L, maxX, 1)` in the paper is `fetch_add(L, segment_add(Segment::MaxX,
1))` here. The returned previous value is decoded with `LockWord::decode`.

A transaction's **ticket** is the value `prev` returned by its fetch-and-add. Its ticket numbers
are `prev.maxX` (exclusive) or `prev.maxS` (shared); the other counters tell it which tickets
were drawn before it. The ticket is *served* when the "now serving" counters catch up:

```
shared    ticket:  nX == prev.maxX                     all earlier writers released
exclusive ticket:  nX == prev.maxX && nS == prev.maxS  all earlier writers and readers released
```

Because fetch-and-add always succeeds and hands out numbers in arrival order, tickets form a
FIFO queue without any queue data structure, which is what makes DSLR starvation-free
(Appendix A.2 of the paper).

## 2. Acquiring a lock — Algorithm 1 → `LockSession::try_acquire`

| paper | code |
|---|---|
| line 1 `ResetFrom[tid, L] ← 0` | `HeldLock::reset_from = 0` (kept in the lock handle instead of a node-wide table; only the owning transaction ever reads it) |
| lines 3 / 17 `prev ← FA(L, maxS/maxX, 1)` | `accessor_.fetch_add(ref, segment_add(segment, slots))` |
| lines 4–9 / 18–23: a counter already reached `COUNT_MAX` | `is_frozen(prev)` → `undo_ticket()` (withdraws our increment), `back_off_from_frozen_lock()` (random backoff, Appendix A.3; forced reset after a stall, lines 7–8) → `AcquireStatus::Retry` |
| lines 10–11 / 24–25: our ticket is the last one before `COUNT_MAX` | `lock.reset_from = {prev.maxX, COUNT_MAX, prev.maxX, COUNT_MAX}` (shared) or `{COUNT_MAX, prev.maxS, COUNT_MAX, prev.maxS}` (exclusive) |
| lines 12–13 / 26–27: no preceding tickets | `ticket_served(prev, lock)` → `AcquireStatus::Acquired` |
| lines 15 / 29 | `handle_conflict()` |

`try_acquire` is one run of Algorithm 1 and reports the paper's "Failure" as either
`AcquireStatus::Retry` (frozen object: nothing is held, try again) or `AcquireStatus::Aborted`
(our ticket was invalidated by a stall reset: release everything and restart the
transaction). `LockSession::acquire` loops over `Retry` for the caller.

## 3. Waiting — Algorithm 2 → `LockSession::handle_conflict`

| paper | code |
|---|---|
| line 2 `val ← READ(L)` | `accessor_.read(lock.ref)` |
| lines 3–4: counters passed our ticket → Failure | `ticket_skipped(val, lock)` → `Aborted` (see deviation 2 for the shared case) |
| lines 5–10: our turn | `ticket_served(val, lock)` → `Acquired` |
| lines 11–19: `nX`/`nS` unchanged for 2× lease → reset with CAS, Failure | `StallTimer` + `stall_threshold()`; CAS to `reset_val`; `Aborted` |
| lines 17–18: counters at `COUNT_MAX` → reset to zero | `drain_and_reset()` when we are the designated resetter |
| line 20: wait `wait_count × ω` | `sleep_for_precise(poll_interval(wait_count))` |

The stall rule is the paper's lease mechanism: a well-behaved transaction holds a lock for
less than one lease (10 ms by default), so if the "now serving" counters do not move for two
leases the transaction in front must have failed or be in a deadlock. The waiter then uses CAS
to advance the counters past the stalled tickets *and past its own ticket*, fails, and restarts
its transaction; everybody in front of it fails too (their tickets are now behind the counters),
everybody behind it proceeds. Together with the repairs of Section 7 this is the only use of
CAS, and all of them only run on failures.

`wait_count` (line 20) is the number of outstanding tickets ahead of us; sleeping
`wait_count × ω` between READs is the paper's dynamic interval polling (Section 4.6).

## 4. Releasing — Algorithm 3 → `LockSession::release`

| paper | code |
|---|---|
| line 1: release only if the lease has not expired (or we are the resetter) | `within_lease` → plain `fetch_add`, whose returned value is checked for a revoked ticket (deviation 7.6); otherwise the conditional release of deviation 7.4 |
| lines 3 / 5 `FA(L, nS/nX, 1)` | `accessor_.fetch_add(ref, segment_add(serving_segment(mode), slots))` |
| lines 6–7: `ResetFrom > 0` → repeat `CAS(L, ResetFrom, 0)` | `drain_and_reset()` → `reset_to_zero()` |

## 5. Counter resets — Section 4.9

Each 16-bit segment may only count up to `COUNT_MAX` (2^15). The transaction whose ticket
pushes `maxX` or `maxS` to `COUNT_MAX` becomes the **designated resetter**: it remembers
`ResetFrom`, the value the object will have once every ticket up to its own has been released.
From that moment the object is **frozen**: every later fetch-and-add sees a counter
`>= COUNT_MAX`, withdraws its increment, backs off randomly (truncated binary exponential
backoff, Appendix A.3) and retries. When the resetter releases, it waits until the object equals
`ResetFrom` (an exclusive resetter never waits — it was only granted the lock after everybody
ahead of it finished) and sets the whole word to zero with CAS. If the resetter itself dies, the
requesters bouncing off the frozen object notice that it makes no progress for two leases and
reset it themselves (Algorithm 1 lines 7–8, `force_reset_if_stalled`).

## 6. Multi-slot leasing — Section 5.1

A transaction may draw `k` consecutive tickets at once (`slots` argument, bounded by
`Config::max_slots`), which buys it a lease of `k × lease`. Waiters cannot know how many slots
the transaction in front of them holds, so when multi-slot leasing is enabled the stall
threshold grows with the number of outstanding tickets ahead (`stall_multiplier × lease ×
wait_count`), exactly as the paper describes. With `max_slots == 1` the protocol is the fixed
lease version of Section 4.

## 7. Deviations from the pseudocode and why

The implementation follows the pseudocode wherever it is unambiguous and safe. Six places
needed more care; each was found by reasoning about interleavings and confirmed by a test in
`tests/`.

### 7.1 Withdrawing a ticket from a frozen object uses CAS, not `FA(-1)`

*Paper:* Algorithm 1 lines 5 / 19 undo the ticket with `FA(L, maxS, -1)`.

*Problem:* a frozen object may be reset to zero at any moment by its resetter or by a stalled
requester. A decrement that lands *after* the reset borrows from the neighbouring segments
(`0x0000_0000_0000_0000 - 0x0001_0000 = 0xFFFF_FFFF_FFFF_0000`) and corrupts the whole word.

*Here:* `undo_fetch_add` performs the decrement with CAS on the exact value it last observed and
stops as soon as a reset becomes visible (`detail::reset_observed`: a "now serving" counter went
backwards, or the object is no longer frozen). The resetter's CAS to zero is therefore safe even
while other requesters are still withdrawing their tickets.

### 7.2 A shared waiter does not treat `nS > prev(maxS)` as "skipped"

*Paper:* Algorithm 2 line 3 returns Failure when `prev(maxX) < val(nX)` **or** `prev(maxS) <
val(nS)`, for both modes.

*Problem:* shared tickets are granted together. Reader A (ticket `s`) and reader B (ticket
`s+1`) both wait for the same writer; the writer releases; B polls first, finishes and releases
before A polls again. A now sees `nS = s + 1 > s` although it was granted perfectly normally.
With the paper's rule A aborts without releasing, its ticket is never released, and the next
writer behind stalls for two leases (`tests/lock_session_scenario_test.cc`,
`SharedWaiterIsNotFooledBySiblingsReleasingFirst`). Under contention this happened constantly
in the benchmark.

*Here:* `ticket_skipped` uses `nX > prev(maxX)` for both modes and `nS > prev(maxS)` only for
exclusive tickets, where it is conclusive (no shared ticket drawn after an exclusive one can be
released before that writer is done).

### 7.3 A shared skipper only accounts for its own ticket

*Paper:* Algorithm 2 line 13 sets `nS := prev(maxS) + 1` when a shared waiter skips a stalled
writer, i.e. declares every shared ticket up to its own released.

*Problem:* other readers waiting for the same writer are legitimately granted by the skip
(`nX` reaches their view) and will release their tickets themselves; counting them in the skip
makes `nS` run ahead of the real releases, and a later writer can then be served while readers
still hold the lock.

*Here:* a shared skipper sets `nX := prev(maxX)` and `nS := nS + k` (its own tickets only).
Exclusive skippers follow the paper (`nX := prev(maxX) + k`, `nS := prev(maxS)`), which is
sound because the new `nX` evicts every reader with a ticket older than the skipper's.

### 7.4 Releasing after the lease expired is conditional instead of forbidden

*Paper:* Algorithm 3 line 1 skips the release entirely when the lease has expired, because a
waiter may already have skipped the ticket and a second increment would let a later ticket in
early.

*Here:* `release` reads the object and, unless the ticket has indeed been skipped or the object
reset, releases it with CAS (a skip racing with the CAS makes the CAS fail and the re-read sees
the skip). A late but unrevoked release therefore does not leave a hole that costs everybody
behind a two-lease stall. `release` returns `false` when the ticket had been revoked; the
caller should then treat its transaction as failed because the critical section may have
overlapped with another holder's (`Transaction::commit()` propagates this).

### 7.5 Leases start at the last moment we knew we did *not* hold the lock

The lease of a holder and the stall timer of a waiter measure the same interval from two sides,
and the 2× margin only holds if neither side is optimistic. Both are therefore measured
conservatively:

* `HeldLock::granted_at` is the issue time of the fetch-and-add (immediate grant) or of the
  last READ that still showed an earlier ticket — a lower bound on the moment the counters
  reached our ticket. A thread descheduled between issuing a READ and looking at its result
  cannot believe its lease started later than it did.
* `StallTimer` proves stillness from the *completion* of the first observation to the *issue*
  of the latest one; it can under-estimate a stall but never over-estimate it, so a waiter that
  was descheduled cannot skip a transaction whose lease is still running.

The polling cap `Config::max_poll_interval` consequently only affects latency, not safety.

### 7.6 A release within the lease checks what its fetch-and-add returned

*Paper:* Algorithm 3 checks the lease (line 1) and then increments `nX`/`nS` with a blind FA
(lines 3 / 5).

*Problem:* the check and the FA are two steps. The protocol's margin for the second step is
`(stall_multiplier - 1) × lease` — the silence a waiter still needs after the lease expired before
it may skip the ticket. A thread descheduled for longer than that between the two steps (an OS
or hypervisor hiccup, a GC pause, a lost and retransmitted RDMA packet) performs its FA *after*
being skipped. On `nX` such a stray increment lets the ticket behind the new holder in while the
new holder still works, and every following holder inherits the off-by-one until a late release
is refused. On `nS` it is worse: `nS` is now one ahead of the shared tickets ever drawn, so every
later exclusive ticket sees `nS > prev(maxS)`, concludes it was skipped, and aborts — the object
is unusable for writers until its next counter reset. The paper's pseudocode has the same
window.

*Here:* the FA's return value is the lock object right before the increment, and it tells
exactly whether the ticket was still valid: `nX` must equal `prev(maxX)` (and, for an exclusive
ticket, `nS` must equal `prev(maxS)`), and the counters must belong to our generation
(`ticket_revoked`). If not, `release` takes the increment back with the same CAS loop that
withdraws tickets from frozen objects (7.1) and returns `false`. This costs nothing in the common
case (no extra round trip) and confines the damage of a very late release to the instant between
the stray FA and its undo (`stray_releases` in `SessionStats`). Tests:
`StrayExclusiveReleaseIsUndone`, `StraySharedReleaseIsUndone`.

Two smaller points: the designated resetter's "reset to zero" in Algorithm 2 lines 17–18 is only
performed by the transaction that actually is the designated resetter (`lock.reset_from != 0`),
via the same `drain_and_reset` path as a normal release — otherwise live waiters between the
skipper and the resetter would be wiped out; and a shared resetter that has to skip dead readers
pushes `nX` one past its own view so that lingering readers of the old generation see their
ticket as skipped rather than releasing into the freshly reset object.

## 8. Assumptions inherited from the paper

* Fewer than 32,767 transactions compete for one lock object at the same time (Section 4.9);
  otherwise a 16-bit segment could overflow into its neighbour. `try_acquire` throws if it
  detects that this happened.
* Clocks on each node are well-behaved (do not run too fast or too slow); they need not be
  synchronized, since all durations are measured locally (Section 4.1).
* Holders respect their lease: a transaction that overstays may be skipped, and its later
  accesses are no longer protected. `release()` reports this so the application can react.
* Delays are bounded: a release decided within the lease reaches the lock object within
  `(stall_multiplier - 1) × lease` (10 ms by default). Deviation 7.6 detects a release that
  arrived later and undoes it, but the critical section may still have overlapped with the next
  holder's for an instant. Raise `Config::stall_multiplier` (or the lease) on hosts with coarse
  scheduling; the in-process stress tests use `stall_multiplier = 3` for that reason.
* Locking is advisory: participants follow the protocol; nothing enforces it (Section 2.2.2).
