#pragma once

#include <cstdint>
#include <random>

#include "dslr/clock.h"

namespace dslr {

/// Truncated binary exponential backoff (Appendix A.3 of the paper). After the c-th consecutive
/// collision with a frozen lock object, the requester waits a uniformly random duration drawn
/// from [0, min(base * 2^(c-1), max)]. The randomization is what lets the resetting
/// compare-and-swap eventually find a quiet window between the fetch-and-adds of other
/// requesters.
class ExponentialBackoff {
 public:
  ExponentialBackoff(Duration base, Duration max, uint64_t seed);

  /// Delay for the `consecutive_failures`-th failure in a row (1-based; 0 is treated as 1).
  Duration next(unsigned consecutive_failures);

  /// Uniform random duration in [0, cap]. Used for short "settle" pauses between CAS retries.
  Duration jitter(Duration cap);

 private:
  Duration base_;
  Duration max_;
  std::mt19937_64 rng_;
};

}  // namespace dslr
