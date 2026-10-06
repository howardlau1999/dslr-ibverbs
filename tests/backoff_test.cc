#include "dslr/backoff.h"

#include <gtest/gtest.h>

namespace dslr {
namespace {

using std::chrono::microseconds;

TEST(ExponentialBackoff, WindowDoublesAndIsTruncated) {
  ExponentialBackoff backoff(microseconds{10}, microseconds{100}, /*seed=*/1);
  for (int round = 0; round < 200; ++round) {
    EXPECT_LE(backoff.next(1).count(), 10);   // [0, 10]
    EXPECT_LE(backoff.next(2).count(), 20);   // [0, 20]
    EXPECT_LE(backoff.next(3).count(), 40);   // [0, 40]
    EXPECT_LE(backoff.next(4).count(), 80);   // [0, 80]
    EXPECT_LE(backoff.next(5).count(), 100);  // truncated at L = 100
    EXPECT_LE(backoff.next(40).count(), 100);
  }
}

TEST(ExponentialBackoff, ZeroFailuresBehavesLikeOne) {
  ExponentialBackoff backoff(microseconds{10}, microseconds{100}, 2);
  for (int round = 0; round < 100; ++round) {
    EXPECT_LE(backoff.next(0).count(), 10);
  }
}

TEST(ExponentialBackoff, IsActuallyRandom) {
  ExponentialBackoff backoff(microseconds{1000}, microseconds{1000}, 3);
  bool saw_different = false;
  const auto first = backoff.next(1);
  for (int round = 0; round < 50 && !saw_different; ++round) {
    saw_different = backoff.next(1) != first;
  }
  EXPECT_TRUE(saw_different);
}

TEST(ExponentialBackoff, SeedMakesSequenceReproducible) {
  ExponentialBackoff a(microseconds{100}, microseconds{10000}, 42);
  ExponentialBackoff b(microseconds{100}, microseconds{10000}, 42);
  for (unsigned c = 1; c < 10; ++c) {
    EXPECT_EQ(a.next(c), b.next(c));
  }
}

TEST(ExponentialBackoff, JitterIsBoundedByCap) {
  ExponentialBackoff backoff(microseconds{10}, microseconds{100}, 4);
  EXPECT_EQ(backoff.jitter(microseconds{0}).count(), 0);
  for (int round = 0; round < 100; ++round) {
    EXPECT_LE(backoff.jitter(microseconds{7}).count(), 7);
  }
}

}  // namespace
}  // namespace dslr
