#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "dslr/clock.h"
#include "dslr/lock_word.h"
#include "dslr/lock_word_accessor.h"

namespace dslr {

/// A lock table living in this process, accessed through CPU atomics instead of RDMA verbs.
/// It behaves exactly like a remote table seen through dslr::LockTableClient and is used for
/// unit tests, the `--local` mode of the benchmark, and single-node deployments. All methods are
/// thread-safe; any number of LockSessions may share one table.
class LocalLockTable final : public LockWordAccessor {
 public:
  /// Creates `lock_count` zeroed lock objects that answer to LockRef::node == `node_id`.
  explicit LocalLockTable(uint32_t lock_count, uint32_t node_id = 0);
  ~LocalLockTable() override;

  LocalLockTable(const LocalLockTable&) = delete;
  LocalLockTable& operator=(const LocalLockTable&) = delete;

  uint64_t fetch_add(LockRef ref, uint64_t addend) override;
  uint64_t compare_swap(LockRef ref, uint64_t expected, uint64_t desired) override;
  uint64_t read(LockRef ref) override;

  uint32_t node_id() const { return node_id_; }
  uint32_t lock_count() const { return lock_count_; }

  /// Test helpers: inspect or overwrite a lock object directly, bypassing the protocol.
  LockWord peek(uint32_t index) const;
  void poke(uint32_t index, LockWord word);

  /// Fault injection for tests: every operation first sleeps a random duration in
  /// [0, max_delay] to widen race windows the way network latency would. Zero disables it.
  void set_injected_delay(Duration max_delay);

 private:
  std::atomic<uint64_t>& word(LockRef ref);
  void maybe_delay();

  uint32_t node_id_;
  uint32_t lock_count_;
  std::unique_ptr<std::atomic<uint64_t>[]> words_;
  std::atomic<Duration::rep> injected_delay_us_{0};
};

}  // namespace dslr
