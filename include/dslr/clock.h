#pragma once

#include <chrono>

namespace dslr {

/// Monotonic clock used for leases, stall detection and polling. DSLR only ever compares
/// durations measured on the *same* node, so clocks across the cluster need not be synchronized
/// (Section 4.1 of the paper).
using Clock = std::chrono::steady_clock;
using Duration = std::chrono::microseconds;

/// Sleeps for roughly `duration`. Short waits (a few RDMA round trips, as produced by the
/// dynamic interval polling) are spun on the CPU because the kernel's timer slack would turn a
/// 5 us sleep into a ~60 us one; longer waits are handed to the scheduler.
void sleep_for_precise(Duration duration);

}  // namespace dslr
