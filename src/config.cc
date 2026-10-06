#include "dslr/config.h"

#include <stdexcept>
#include <string>

namespace dslr {

void Config::validate() const {
  if (lease <= Duration::zero()) {
    throw std::invalid_argument("Config::lease must be positive");
  }
  if (stall_multiplier < 2) {
    // A holder's lease runs from the last moment it knew it did not hold the lock yet, while a
    // waiter's stall timer runs from the last counter change it observed, which is later. One
    // extra lease of margin covers that difference and modest clock-rate differences between
    // nodes (Section 4.1).
    throw std::invalid_argument("Config::stall_multiplier must be at least 2");
  }
  if (poll_unit <= Duration::zero()) {
    throw std::invalid_argument("Config::poll_unit must be positive");
  }
  if (max_poll_interval <= Duration::zero() || max_poll_interval > lease) {
    throw std::invalid_argument("Config::max_poll_interval must be in (0, lease]");
  }
  if (backoff_base <= Duration::zero() || backoff_max < backoff_base) {
    throw std::invalid_argument("Config::backoff_base must be positive and <= backoff_max");
  }
  if (count_max < 2 || count_max > 32768) {
    throw std::invalid_argument("Config::count_max must be in [2, 32768]");
  }
  if (max_slots < 1 || max_slots > 4096 || max_slots >= count_max) {
    throw std::invalid_argument("Config::max_slots must be in [1, min(4096, count_max - 1)]");
  }
  if (max_frozen_retries == 0) {
    throw std::invalid_argument("Config::max_frozen_retries must be positive");
  }
}

}  // namespace dslr
