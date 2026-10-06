// dslr_bench: micro-benchmark for the DSLR lock manager (Appendix A.4 of the paper).
//
// Every worker thread runs transactions back to back. A transaction picks `--locks-per-txn`
// distinct lock objects (uniformly or Zipf-skewed), requests each as shared with probability
// `--shared-ratio` and exclusive otherwise, holds them for `--think`, and releases them. When
// DSLR aborts the transaction (suspected deadlock or failure) it is retried from scratch.
//
//   dslr_bench --local --threads 8 --locks 100000 --zipf 0.99 --duration 10s
//   dslr_bench --servers node1:7777,node2:7777 --device mlx5_0 --threads 4
//
// `--fail-rate` makes transactions "crash" while holding their locks (Appendix A.6); the leases
// of the abandoned tickets expire and other transactions skip them. `--no-sort` acquires locks
// in random order so that real deadlocks occur and are resolved by the stall mechanism.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dslr/config.h"
#include "dslr/local_lock_table.h"
#include "dslr/lock_session.h"
#include "dslr/transaction.h"

#include "args.h"

#if DSLR_HAVE_RDMA
#include "dslr/lock_table_client.h"
#include "dslr/rdma/device.h"
#endif

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------
// Workload generation
// ---------------------------------------------------------------------------------------------

/// Zipf-distributed integers in [0, n): P(k) ~ 1 / (k + 1)^theta. Uses the rejection-inversion
/// method of Hörmann & Derflinger (1996), which needs O(1) time and memory regardless of n.
class ZipfSampler {
 public:
  ZipfSampler(uint64_t n, double theta) : n_(n), theta_(theta) {
    if (theta_ > 0) {
      h_x1_ = h_integral(1.5) - 1.0;
      h_n_ = h_integral(static_cast<double>(n_) + 0.5);
      s_ = 2.0 - h_integral_inverse(h_integral(2.5) - h(2.0));
    }
  }

  template <typename Rng>
  uint64_t operator()(Rng& rng) {
    if (theta_ <= 0) {
      return std::uniform_int_distribution<uint64_t>(0, n_ - 1)(rng);
    }
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    while (true) {
      const double u = h_n_ + uniform(rng) * (h_x1_ - h_n_);
      const double x = h_integral_inverse(u);
      auto k = static_cast<uint64_t>(std::clamp(x + 0.5, 1.0, static_cast<double>(n_)));
      if (static_cast<double>(k) - x <= s_ ||
          u >= h_integral(static_cast<double>(k) + 0.5) - h(static_cast<double>(k))) {
        return k - 1;
      }
    }
  }

 private:
  double h(double x) const { return std::exp(-theta_ * std::log(x)); }
  double h_integral(double x) const {
    const double log_x = std::log(x);
    return helper2((1.0 - theta_) * log_x) * log_x;
  }
  double h_integral_inverse(double x) const {
    double t = x * (1.0 - theta_);
    if (t < -1.0) {
      t = -1.0;  // numerical guard
    }
    return std::exp(helper1(t) * x);
  }
  // log(1 + x) / x and (exp(x) - 1) / x, stable near zero.
  static double helper1(double x) {
    return std::abs(x) > 1e-8 ? std::log1p(x) / x : 1.0 - x * (0.5 - x * (1.0 / 3.0 - 0.25 * x));
  }
  static double helper2(double x) {
    return std::abs(x) > 1e-8 ? std::expm1(x) / x
                              : 1.0 + x * 0.5 * (1.0 + x * (1.0 / 3.0) * (1.0 + 0.25 * x));
  }

  uint64_t n_;
  double theta_;
  double h_x1_ = 0, h_n_ = 0, s_ = 0;
};

/// Log-linear histogram with ~1.5% relative precision, for latency percentiles without storing
/// every sample.
class LatencyHistogram {
 public:
  void record(std::chrono::nanoseconds latency) {
    const auto ns = static_cast<uint64_t>(std::max<int64_t>(1, latency.count()));
    const unsigned exponent = 63u - static_cast<unsigned>(__builtin_clzll(ns));
    const unsigned mantissa =
        exponent >= kMantissaBits
            ? static_cast<unsigned>((ns >> (exponent - kMantissaBits)) & kMantissaMask)
            : static_cast<unsigned>((ns << (kMantissaBits - exponent)) & kMantissaMask);
    const size_t bucket =
        std::min<size_t>(exponent * (1u << kMantissaBits) + mantissa, counts_.size() - 1);
    ++counts_[bucket];
    ++total_;
    sum_ns_ += ns;
    max_ns_ = std::max(max_ns_, ns);
  }

  void merge(const LatencyHistogram& other) {
    for (size_t i = 0; i < counts_.size(); ++i) {
      counts_[i] += other.counts_[i];
    }
    total_ += other.total_;
    sum_ns_ += other.sum_ns_;
    max_ns_ = std::max(max_ns_, other.max_ns_);
  }

  uint64_t count() const { return total_; }
  double mean_us() const {
    return total_ == 0 ? 0.0 : static_cast<double>(sum_ns_) / static_cast<double>(total_) / 1e3;
  }
  double max_us() const { return static_cast<double>(max_ns_) / 1e3; }

  double percentile_us(double p) const {
    if (total_ == 0) {
      return 0.0;
    }
    const auto target = static_cast<uint64_t>(std::ceil(p / 100.0 * static_cast<double>(total_)));
    uint64_t seen = 0;
    for (size_t bucket = 0; bucket < counts_.size(); ++bucket) {
      seen += counts_[bucket];
      if (seen >= target && counts_[bucket] != 0) {
        const unsigned exponent = static_cast<unsigned>(bucket >> kMantissaBits);
        const unsigned mantissa = static_cast<unsigned>(bucket & kMantissaMask);
        const double lower = std::ldexp(1.0 + static_cast<double>(mantissa) / (1u << kMantissaBits),
                                        static_cast<int>(exponent));
        return lower / 1e3;
      }
    }
    return max_us();
  }

 private:
  static constexpr unsigned kMantissaBits = 6;
  static constexpr unsigned kMantissaMask = (1u << kMantissaBits) - 1;
  std::vector<uint64_t> counts_ = std::vector<uint64_t>(64u << kMantissaBits, 0);
  uint64_t total_ = 0;
  uint64_t sum_ns_ = 0;
  uint64_t max_ns_ = 0;
};

struct BenchOptions {
  bool local = true;
  std::vector<std::pair<std::string, uint16_t>> servers;
  std::string device;
  uint8_t ib_port = 1;
  std::optional<uint32_t> gid_index;

  unsigned threads = 4;
  std::chrono::microseconds duration{10'000'000};
  std::chrono::microseconds warmup{1'000'000};
  uint32_t locks = 1u << 20;  // per node
  unsigned locks_per_txn = 1;
  double shared_ratio = 0.5;
  double zipf = 0.0;
  std::chrono::microseconds think{0};
  double fail_rate = 0.0;
  bool sort_locks = true;
  uint16_t slots = 1;
  uint64_t seed = 42;
  dslr::Config config;
};

struct WorkerResult {
  uint64_t commits = 0;
  uint64_t aborts = 0;
  uint64_t revoked = 0;
  uint64_t crashes = 0;
  uint64_t locks_granted = 0;
  LatencyHistogram latency;
  dslr::SessionStats stats;
};

void add(dslr::SessionStats& into, const dslr::SessionStats& from) {
  into.acquire_attempts += from.acquire_attempts;
  into.acquired_immediately += from.acquired_immediately;
  into.acquired_after_wait += from.acquired_after_wait;
  into.polls += from.polls;
  into.frozen_retries += from.frozen_retries;
  into.undo_cas_retries += from.undo_cas_retries;
  into.forced_resets += from.forced_resets;
  into.stall_skips += from.stall_skips;
  into.aborted_skipped += from.aborted_skipped;
  into.aborted_by_reset += from.aborted_by_reset;
  into.releases += from.releases;
  into.late_releases += from.late_releases;
  into.revoked_releases += from.revoked_releases;
  into.stray_releases += from.stray_releases;
  into.counter_resets += from.counter_resets;
}

// ---------------------------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------------------------

struct LockRequest {
  dslr::LockRef ref;
  dslr::LockMode mode;
};

void run_worker(const BenchOptions& options, unsigned worker_id, dslr::LockWordAccessor& accessor,
                uint32_t node_count, const std::atomic<int>& phase, WorkerResult& result) {
  dslr::LockSession session(accessor, options.config, options.seed + worker_id);
  std::mt19937_64 rng(options.seed * 7919 + worker_id);
  ZipfSampler sampler(uint64_t{node_count} * options.locks, options.zipf);
  std::uniform_real_distribution<double> coin(0.0, 1.0);
  std::vector<LockRequest> requests;

  // phase: 0 = warm-up (run but do not record), 1 = measuring, 2 = stop.
  while (phase.load(std::memory_order_relaxed) != 2) {
    requests.clear();
    while (requests.size() < options.locks_per_txn) {
      const uint64_t rank = sampler(rng);
      const dslr::LockRef ref{static_cast<uint32_t>(rank % node_count),
                              static_cast<uint32_t>(rank / node_count)};
      const bool duplicate = std::any_of(requests.begin(), requests.end(),
                                         [&](const LockRequest& r) { return r.ref == ref; });
      if (!duplicate) {
        requests.push_back({ref, coin(rng) < options.shared_ratio ? dslr::LockMode::Shared
                                                                  : dslr::LockMode::Exclusive});
      }
    }
    if (options.sort_locks) {
      // A global order on lock objects is the classic way to avoid deadlocks.
      std::sort(requests.begin(), requests.end(), [](const LockRequest& a, const LockRequest& b) {
        return std::tie(a.ref.node, a.ref.index) < std::tie(b.ref.node, b.ref.index);
      });
    }

    const auto start = Clock::now();
    unsigned aborts = 0;
    unsigned revoked = 0;
    bool crashed = false;
    while (true) {
      dslr::Transaction txn(session);
      bool ok = true;
      for (const LockRequest& request : requests) {
        if (!txn.lock(request.ref, request.mode, options.slots)) {
          ok = false;
          break;
        }
      }
      if (!ok) {
        ++aborts;
        if (phase.load(std::memory_order_relaxed) == 2) {
          break;
        }
        continue;
      }
      if (options.think.count() > 0) {
        dslr::sleep_for_precise(options.think);
      }
      if (options.fail_rate > 0 && coin(rng) < options.fail_rate) {
        txn.abandon();  // crash while holding the locks: tickets are never released
        crashed = true;
      } else if (!txn.commit()) {
        // A lease expired and the ticket was revoked meanwhile: the work must be redone.
        ++revoked;
        if (phase.load(std::memory_order_relaxed) == 2) {
          break;
        }
        continue;
      }
      break;
    }
    const auto end = Clock::now();

    if (phase.load(std::memory_order_relaxed) == 1) {
      result.aborts += aborts;
      result.revoked += revoked;
      if (crashed) {
        ++result.crashes;
      } else {
        ++result.commits;
        result.locks_granted += requests.size();
        result.latency.record(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start));
      }
    }
  }
  result.stats = session.stats();
}

// ---------------------------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------------------------

void usage() {
  std::cout
      << "Usage: dslr_bench [--local | --servers host:port,...] [options]\n"
         "Transport:\n"
         "  --local               in-process lock table, no RDMA (default when --servers is "
         "absent)\n"
         "  --servers LIST        comma-separated lock table servers; LockRef::node follows this "
         "order\n"
         "  --device NAME         ibverbs device for the client side (default: first device)\n"
         "  --ib-port N           physical port (default: 1)\n"
         "  --gid-index N         GID index for RoCE (default: auto)\n"
         "Workload:\n"
         "  --threads N           worker threads, one LockSession each (default: 4)\n"
         "  --duration DUR        measured duration (default: 10s)\n"
         "  --warmup DUR          warm-up before measuring (default: 1s)\n"
         "  --locks N             lock objects per node to pick from (default: 1048576)\n"
         "  --locks-per-txn N     locks acquired by each transaction (default: 1)\n"
         "  --shared-ratio F      probability of requesting a shared lock, rho (default: 0.5)\n"
         "  --zipf THETA          skew of the access distribution, 0 = uniform (default: 0)\n"
         "  --think DUR           time spent holding the locks (default: 0)\n"
         "  --fail-rate F         probability that a transaction never releases its locks "
         "(default: 0)\n"
         "  --no-sort             acquire locks in random order (allows deadlocks)\n"
         "  --slots N             tickets per lock request, needs --max-slots >= N (default: 1)\n"
         "  --seed N              random seed (default: 42)\n"
         "DSLR parameters:\n"
         "  --lease DUR           lease time (default: 10ms)\n"
         "  --stall-multiplier N  stall threshold as a multiple of the lease (default: 2)\n"
         "  --poll-unit DUR       omega, polling interval per preceding ticket (default: 5us)\n"
         "  --max-poll DUR        cap on one polling sleep (default: 5ms)\n"
         "  --backoff-base DUR    R of the exponential backoff (default: 10us)\n"
         "  --backoff-max DUR     L of the exponential backoff (default: 10ms)\n"
         "  --count-max N         COUNT_MAX (default: 32768)\n"
         "  --max-slots N         enable multi-slot leasing with up to N slots (default: 1)\n";
}

BenchOptions parse(const dslr::tools::Args& args) {
  BenchOptions o;
  if (args.has("servers")) {
    o.local = false;
    o.servers = dslr::tools::parse_host_ports(args.get("servers", ""), 7777);
    if (o.servers.empty()) {
      throw std::invalid_argument("--servers needs at least one host:port");
    }
  }
  if (args.flag("local")) {
    o.local = true;
  }
  o.device = args.get("device", "");
  o.ib_port = args.number<uint8_t>("ib-port", 1);
  if (args.has("gid-index")) {
    o.gid_index = args.number<uint32_t>("gid-index", 0);
  }
  o.threads = args.number<unsigned>("threads", 4);
  o.duration = args.duration("duration", o.duration);
  o.warmup = args.duration("warmup", o.warmup);
  o.locks = args.number<uint32_t>("locks", o.locks);
  o.locks_per_txn = args.number<unsigned>("locks-per-txn", 1);
  o.shared_ratio = args.number<double>("shared-ratio", 0.5);
  o.zipf = args.number<double>("zipf", 0.0);
  o.think = args.duration("think", o.think);
  o.fail_rate = args.number<double>("fail-rate", 0.0);
  o.sort_locks = !args.flag("no-sort");
  o.slots = args.number<uint16_t>("slots", 1);
  o.seed = args.number<uint64_t>("seed", 42);

  dslr::Config& c = o.config;
  c.lease = args.duration("lease", c.lease);
  c.stall_multiplier = args.number<unsigned>("stall-multiplier", c.stall_multiplier);
  c.poll_unit = args.duration("poll-unit", c.poll_unit);
  c.max_poll_interval = args.has("max-poll") ? args.duration("max-poll", c.max_poll_interval)
                                             : std::min(c.max_poll_interval, c.lease / 2);
  c.backoff_base = args.duration("backoff-base", c.backoff_base);
  c.backoff_max = args.duration("backoff-max", c.backoff_max);
  c.count_max = args.number<uint16_t>("count-max", c.count_max);
  c.max_slots = args.number<uint16_t>("max-slots", c.max_slots);
  c.validate();

  if (o.threads == 0 || o.locks == 0 || o.locks_per_txn == 0) {
    throw std::invalid_argument("--threads, --locks and --locks-per-txn must be positive");
  }
  if (o.locks_per_txn > o.locks) {
    throw std::invalid_argument("--locks-per-txn cannot exceed --locks");
  }
  if (o.slots < 1 || o.slots > c.max_slots) {
    throw std::invalid_argument("--slots must be in [1, --max-slots]");
  }
  return o;
}

std::string fmt(double value, int precision = 1) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", precision, value);
  return buf;
}

int run(const BenchOptions& options) {
  std::cout << "dslr_bench: " << (options.local ? "local in-process table" : "RDMA") << ", "
            << options.threads << " threads, " << options.locks_per_txn
            << " lock(s)/txn, shared ratio " << options.shared_ratio << ", zipf " << options.zipf
            << ", lease " << options.config.lease.count() << "us, COUNT_MAX "
            << options.config.count_max << std::endl;

  // Transport set-up. Workers share the local table; with RDMA each worker owns a client.
  std::unique_ptr<dslr::LocalLockTable> local_table;
  std::vector<std::unique_ptr<dslr::LockWordAccessor>> accessors(options.threads);
  uint32_t node_count = 1;
  uint32_t locks_per_node = options.locks;
#if DSLR_HAVE_RDMA
  std::unique_ptr<dslr::rdma::Device> device;
#endif
  if (options.local) {
    local_table = std::make_unique<dslr::LocalLockTable>(options.locks);
  } else {
#if DSLR_HAVE_RDMA
    dslr::rdma::DeviceOptions device_options;
    device_options.name = options.device;
    device_options.port = options.ib_port;
    device_options.gid_index = options.gid_index;
    device = std::make_unique<dslr::rdma::Device>(device_options);
    std::vector<dslr::ServerAddress> servers;
    for (const auto& [host, port] : options.servers) {
      servers.push_back({host, port});
    }
    node_count = static_cast<uint32_t>(servers.size());
    for (unsigned i = 0; i < options.threads; ++i) {
      auto client = std::make_unique<dslr::LockTableClient>(*device, servers);
      for (uint32_t node = 0; node < node_count; ++node) {
        locks_per_node = std::min(locks_per_node, client->lock_count(node));
      }
      accessors[i] = std::move(client);
    }
    std::cout << "dslr_bench: connected to " << node_count << " lock table(s) via "
              << device->name() << ", using " << locks_per_node << " lock objects per node"
              << std::endl;
#else
    std::cerr << "dslr_bench was built without RDMA support; use --local" << std::endl;
    return 1;
#endif
  }
  BenchOptions effective = options;
  effective.locks = locks_per_node;

  std::atomic<int> phase{0};
  std::vector<WorkerResult> results(options.threads);
  std::vector<std::thread> workers;
  for (unsigned i = 0; i < options.threads; ++i) {
    dslr::LockWordAccessor& accessor =
        options.local ? static_cast<dslr::LockWordAccessor&>(*local_table) : *accessors[i];
    workers.emplace_back(run_worker, std::cref(effective), i, std::ref(accessor), node_count,
                         std::cref(phase), std::ref(results[i]));
  }

  std::this_thread::sleep_for(options.warmup);
  phase.store(1);
  const auto measure_start = Clock::now();
  std::this_thread::sleep_for(options.duration);
  phase.store(2);
  const auto measure_end = Clock::now();
  for (std::thread& worker : workers) {
    worker.join();
  }

  // Aggregate.
  WorkerResult total;
  for (const WorkerResult& r : results) {
    total.commits += r.commits;
    total.aborts += r.aborts;
    total.revoked += r.revoked;
    total.crashes += r.crashes;
    total.locks_granted += r.locks_granted;
    total.latency.merge(r.latency);
    add(total.stats, r.stats);
  }
  const double seconds = std::chrono::duration<double>(measure_end - measure_start).count();
  const auto& s = total.stats;

  std::cout << "\nThroughput\n"
            << "  committed transactions : " << total.commits << " ("
            << fmt(static_cast<double>(total.commits) / seconds, 0) << " txn/s)\n"
            << "  locks granted          : " << total.locks_granted << " ("
            << fmt(static_cast<double>(total.locks_granted) / seconds, 0) << " locks/s)\n"
            << "  aborted attempts       : " << total.aborts << " ("
            << fmt(100.0 * static_cast<double>(total.aborts) /
                       static_cast<double>(std::max<uint64_t>(1, total.commits + total.aborts)),
                   2)
            << "% of attempts)\n"
            << "  revoked at commit      : " << total.revoked << " (lease expired, work redone)\n";
  if (options.fail_rate > 0) {
    std::cout << "  crashed transactions   : " << total.crashes << "\n";
  }
  std::cout << "Transaction latency (us)\n"
            << "  mean " << fmt(total.latency.mean_us()) << "  p50 "
            << fmt(total.latency.percentile_us(50)) << "  p99 "
            << fmt(total.latency.percentile_us(99)) << "  p99.9 "
            << fmt(total.latency.percentile_us(99.9)) << "  max " << fmt(total.latency.max_us())
            << "\n"
            << "DSLR protocol counters (all threads, including warm-up)\n"
            << "  acquire attempts       : " << s.acquire_attempts << "\n"
            << "  granted by the FA alone: " << s.acquired_immediately << "\n"
            << "  granted after waiting  : " << s.acquired_after_wait << " (" << s.polls
            << " READs)\n"
            << "  frozen-lock retries    : " << s.frozen_retries << " (undo CAS retries "
            << s.undo_cas_retries << ")\n"
            << "  counter resets         : " << s.counter_resets << " (forced " << s.forced_resets
            << ")\n"
            << "  stall skips            : " << s.stall_skips << "\n"
            << "  aborted (skipped/reset): " << s.aborted_skipped << " / " << s.aborted_by_reset
            << "\n"
            << "  releases               : " << s.releases << " (late " << s.late_releases
            << ", revoked " << s.revoked_releases << ", of which stray and undone "
            << s.stray_releases << ")\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  dslr::tools::Args args(argc, argv);
  if (args.flag("help")) {
    usage();
    return 0;
  }
  try {
    return run(parse(args));
  } catch (const std::exception& e) {
    std::cerr << "dslr_bench: " << e.what() << std::endl;
    return 1;
  }
}
