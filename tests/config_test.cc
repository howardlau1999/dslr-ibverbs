#include "dslr/config.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace dslr {
namespace {

TEST(Config, DefaultsMatchThePaper) {
  const Config config;
  EXPECT_NO_THROW(config.validate());
  EXPECT_EQ(config.lease, std::chrono::microseconds{10'000});  // 10 ms
  EXPECT_EQ(config.stall_multiplier, 2u);                      // twice the lease time
  EXPECT_EQ(config.poll_unit, std::chrono::microseconds{5});   // omega = 5 us
  EXPECT_EQ(config.backoff_base, std::chrono::microseconds{10});
  EXPECT_EQ(config.backoff_max, std::chrono::microseconds{10'000});
  EXPECT_EQ(config.count_max, 32768);  // 2^15
  EXPECT_EQ(config.max_slots, 1);
  EXPECT_FALSE(config.multi_slot_leasing());
}

TEST(Config, RejectsUnsafeValues) {
  auto with = [](auto&& mutate) {
    Config config;
    mutate(config);
    return config;
  };
  EXPECT_THROW(with([](Config& c) { c.lease = Config::Duration::zero(); }).validate(),
               std::invalid_argument);
  EXPECT_THROW(with([](Config& c) { c.stall_multiplier = 1; }).validate(), std::invalid_argument);
  EXPECT_THROW(with([](Config& c) { c.poll_unit = Config::Duration::zero(); }).validate(),
               std::invalid_argument);
  EXPECT_THROW(with([](Config& c) { c.max_poll_interval = c.lease * 2; }).validate(),
               std::invalid_argument);
  EXPECT_THROW(
      with([](Config& c) { c.backoff_max = c.backoff_base - Config::Duration{1}; }).validate(),
      std::invalid_argument);
  EXPECT_THROW(with([](Config& c) { c.count_max = 1; }).validate(), std::invalid_argument);
  EXPECT_THROW(with([](Config& c) { c.count_max = 40000; }).validate(), std::invalid_argument);
  EXPECT_THROW(with([](Config& c) { c.max_slots = 0; }).validate(), std::invalid_argument);
  EXPECT_THROW(with([](Config& c) {
                 c.count_max = 8;
                 c.max_slots = 8;
               }).validate(),
               std::invalid_argument);
  EXPECT_THROW(with([](Config& c) { c.max_frozen_retries = 0; }).validate(), std::invalid_argument);
}

TEST(Config, MultiSlotLeasingIsEnabledByMaxSlots) {
  Config config;
  config.max_slots = 16;
  EXPECT_NO_THROW(config.validate());
  EXPECT_TRUE(config.multi_slot_leasing());
}

}  // namespace
}  // namespace dslr
