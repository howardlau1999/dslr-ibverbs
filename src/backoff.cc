#include "dslr/backoff.h"

#include <algorithm>

namespace dslr {

ExponentialBackoff::ExponentialBackoff(Duration base, Duration max, uint64_t seed)
    : base_(base), max_(max), rng_(seed) {}

Duration ExponentialBackoff::next(unsigned consecutive_failures) {
  const unsigned exponent = consecutive_failures == 0 ? 0 : consecutive_failures - 1;
  // Beyond 2^20 the window would exceed any sensible `max_` anyway; clamping also keeps the
  // shift well-defined.
  const auto window = base_.count() << std::min(exponent, 20u);
  const Duration cap{std::min<Duration::rep>(window, max_.count())};
  return jitter(cap);
}

Duration ExponentialBackoff::jitter(Duration cap) {
  if (cap <= Duration::zero()) {
    return Duration::zero();
  }
  std::uniform_int_distribution<Duration::rep> dist(0, cap.count());
  return Duration{dist(rng_)};
}

}  // namespace dslr
