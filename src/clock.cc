#include "dslr/clock.h"

#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace dslr {
namespace {

// Below this threshold the overhead and imprecision of a kernel sleep dominate, so we spin.
constexpr Duration kSpinThreshold{100};

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

}  // namespace

void sleep_for_precise(Duration duration) {
  if (duration <= Duration::zero()) {
    return;
  }
  const auto deadline = Clock::now() + duration;
  if (duration > kSpinThreshold) {
    // Sleep for most of the interval and spin the remainder for accuracy.
    std::this_thread::sleep_for(duration - kSpinThreshold);
  }
  while (Clock::now() < deadline) {
    cpu_relax();
  }
}

}  // namespace dslr
