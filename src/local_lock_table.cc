#include "dslr/local_lock_table.h"

#include <random>
#include <stdexcept>
#include <string>

namespace dslr {

LocalLockTable::LocalLockTable(uint32_t lock_count, uint32_t node_id)
    : node_id_(node_id),
      lock_count_(lock_count),
      words_(std::make_unique<std::atomic<uint64_t>[]>(lock_count)) {
  if (lock_count == 0) {
    throw std::invalid_argument("LocalLockTable needs at least one lock object");
  }
  for (uint32_t i = 0; i < lock_count; ++i) {
    words_[i].store(0, std::memory_order_relaxed);
  }
}

LocalLockTable::~LocalLockTable() = default;

std::atomic<uint64_t>& LocalLockTable::word(LockRef ref) {
  if (ref.node != node_id_) {
    throw std::out_of_range("LocalLockTable " + std::to_string(node_id_) + " does not host node " +
                            std::to_string(ref.node));
  }
  if (ref.index >= lock_count_) {
    throw std::out_of_range("lock index " + std::to_string(ref.index) +
                            " out of range (table has " + std::to_string(lock_count_) + " locks)");
  }
  return words_[ref.index];
}

void LocalLockTable::maybe_delay() {
  const auto max_delay = injected_delay_us_.load(std::memory_order_relaxed);
  if (max_delay <= 0) {
    return;
  }
  thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<Duration::rep> dist(0, max_delay);
  sleep_for_precise(Duration{dist(rng)});
}

uint64_t LocalLockTable::fetch_add(LockRef ref, uint64_t addend) {
  maybe_delay();
  return word(ref).fetch_add(addend, std::memory_order_acq_rel);
}

uint64_t LocalLockTable::compare_swap(LockRef ref, uint64_t expected, uint64_t desired) {
  maybe_delay();
  // Like the RDMA verb, report the value seen; `expected` is overwritten with it on failure.
  word(ref).compare_exchange_strong(expected, desired, std::memory_order_acq_rel);
  return expected;
}

uint64_t LocalLockTable::read(LockRef ref) {
  maybe_delay();
  return word(ref).load(std::memory_order_acquire);
}

LockWord LocalLockTable::peek(uint32_t index) const {
  return LockWord::decode(words_[index].load(std::memory_order_acquire));
}

void LocalLockTable::poke(uint32_t index, LockWord value) {
  words_[index].store(value.encode(), std::memory_order_release);
}

void LocalLockTable::set_injected_delay(Duration max_delay) {
  injected_delay_us_.store(max_delay.count(), std::memory_order_relaxed);
}

}  // namespace dslr
