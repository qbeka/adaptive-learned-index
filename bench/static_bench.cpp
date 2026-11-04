// The static lookup experiment: one index, one dataset, one process, one JSON
// line. scripts/bench_static.py runs this repeatedly and pairs the results.
//
// Usage: static_bench --dataset PATH --index binary|ali|pgm --epsilon N
//                     [--stride N] [--lookups N] [--passes N] [--latency N]
//                     [--seed N]
#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

#include "ali/sorted_vector_index.h"
#include "ali/static_index.h"
#include "pgm/pgm_index.hpp"
#include "sosd.h"
#include "timing.h"

using namespace ali;
using namespace ali::bench;

namespace {

struct Options {
  std::string dataset;
  std::string index = "ali";
  std::uint64_t epsilon = 64;
  std::size_t stride = 1;
  std::size_t lookups = 10'000'000;
  std::size_t passes = 3;           // Lookup passes; the last one is reported.
  std::size_t latency_samples = 1'000'000;
  std::uint64_t seed = 42;          // 42 is the seed SOSD itself uses.
};

struct Footprint {
  std::size_t index_bytes = 0;  // Directory plus models.
  std::size_t data_bytes = 0;   // Keys plus payloads.
  std::size_t segments = 0;
  std::size_t max_window = 0;
};

// PGM's index stores only segments, so keys and payloads sit beside it in plain
// arrays, which matches how StaticIndex lays out its data.
template <std::size_t kEpsilon>
class PgmAdapter {
 public:
  void bulk_load(std::span<const Entry> entries) {
    split_sorted_entries(entries, keys_, values_);
    pgm_ = pgm::PGMIndex<Key, kEpsilon, 4>(keys_);
  }

  std::optional<Value> lookup(Key key) const {
    const auto approx = pgm_.search(key);
    const std::size_t pos =
        approx.lo + branchless_lower_bound(keys_.data() + approx.lo,
                                           approx.hi - approx.lo, key);
    if (pos == keys_.size() || keys_[pos] != key) return std::nullopt;
    return values_[pos];
  }

  Footprint footprint() const {
    return {pgm_.size_in_bytes(), (keys_.size() + values_.size()) * sizeof(Key),
            pgm_.segments_count(), 2 * kEpsilon + 2};
  }

 private:
  std::vector<Key> keys_;
  std::vector<Value> values_;
  pgm::PGMIndex<Key, kEpsilon, 4> pgm_;
};

class BinaryAdapter {
 public:
  void bulk_load(std::span<const Entry> entries) { index_.bulk_load(entries); }
  std::optional<Value> lookup(Key key) const { return index_.lookup(key); }
  Footprint footprint() const {
    return {0, index_.size() * 2 * sizeof(Key), 0, 0};
  }

 private:
  SortedVectorIndex index_;
};

class AliAdapter {
 public:
  explicit AliAdapter(std::uint64_t epsilon) : index_({epsilon}) {}
  void bulk_load(std::span<const Entry> entries) { index_.bulk_load(entries); }
  std::optional<Value> lookup(Key key) const { return index_.lookup(key); }
  Footprint footprint() const {
    const MemoryReport report = index_.memory();
    return {report.directory + report.models, report.keys + report.payloads,
            index_.segment_count(), index_.max_window()};
  }
  std::uint64_t widened() const { return index_.widened_searches(); }

 private:
  StaticIndex index_;
};

template <class Index>
int run(Index& index, const Options& options) {
  prefer_performance_cores();
  const Dataset dataset = make_dataset(load_sosd_keys(options.dataset, options.stride));
  const std::vector<Entry> lookups = sosd_equality_lookups(dataset, options.lookups, options.seed);

  const std::uint64_t build_start = now_ns();
  index.bulk_load(dataset.entries);
  const std::uint64_t build_ns = now_ns() - build_start;

  // Throughput: the clock is read twice per pass. Earlier passes warm the
  // caches, the last pass is reported.
  std::uint64_t checksum = 0;
  std::uint64_t lookup_ns = 0;
  for (std::size_t pass = 0; pass < options.passes; ++pass) {
    checksum = 0;
    const std::uint64_t start = now_ns();
    for (const Entry& lookup : lookups) checksum += *index.lookup(lookup.key);
    lookup_ns = now_ns() - start;
  }
  std::uint64_t expected = 0;
  for (const Entry& lookup : lookups) expected += lookup.value;

  // Latency: a separate pass, since a clock read around every lookup would
  // distort the throughput figure.
  std::vector<std::uint32_t> samples;
  samples.reserve(options.latency_samples);
  std::uint64_t sink = 0;
  const std::size_t latency_ops = std::min(options.latency_samples, lookups.size());
  for (std::size_t i = 0; i < latency_ops; ++i) {
    const std::uint64_t t0 = now_ns();
    sink += *index.lookup(lookups[i].key);
    const std::uint64_t t1 = now_ns();
    samples.push_back(static_cast<std::uint32_t>(t1 - t0));
  }
  const double overhead = clock_overhead_ns();
  const std::uint64_t p50 = percentile(samples, 50);
  const std::uint64_t p99 = percentile(samples, 99);
  const std::uint64_t worst = samples.empty() ? 0 : *std::max_element(samples.begin(), samples.end());

  const Footprint footprint = index.footprint();
  std::uint64_t widened = 0;
  if constexpr (requires { index.widened(); }) widened = index.widened();

  const double ops = static_cast<double>(lookups.size());
  const double keys = static_cast<double>(dataset.entries.size());
  std::printf(
      "{\"dataset\":\"%s\",\"index\":\"%s\",\"epsilon\":%" PRIu64
      ",\"stride\":%zu,\"raw_keys\":%zu,\"dropped_max_keys\":%zu,\"keys\":%zu,\"lookups\":%zu"
      ",\"build_ns\":%" PRIu64 ",\"ns_per_lookup\":%.3f,\"lookups_per_s\":%.0f"
      ",\"p50_ns\":%" PRIu64 ",\"p99_ns\":%" PRIu64 ",\"max_ns\":%" PRIu64
      ",\"clock_overhead_ns\":%.1f,\"index_bytes\":%zu,\"data_bytes\":%zu"
      ",\"index_bytes_per_key\":%.3f,\"segments\":%zu,\"max_window\":%zu"
      ",\"widened\":%" PRIu64 ",\"checksum\":%" PRIu64 ",\"checksum_ok\":%s,\"sink\":%" PRIu64 "}\n",
      options.dataset.substr(options.dataset.find_last_of('/') + 1).c_str(),
      options.index.c_str(), options.epsilon, options.stride, dataset.raw_keys,
      dataset.dropped_max_keys, dataset.entries.size(), lookups.size(), build_ns,
      static_cast<double>(lookup_ns) / ops, ops * 1e9 / static_cast<double>(lookup_ns),
      p50, p99, worst, overhead, footprint.index_bytes, footprint.data_bytes,
      static_cast<double>(footprint.index_bytes) / keys, footprint.segments,
      footprint.max_window, widened, checksum, checksum == expected ? "true" : "false", sink);
  return checksum == expected ? 0 : 1;
}

template <std::size_t kEpsilon>
int run_pgm(const Options& options) {
  PgmAdapter<kEpsilon> index;
  return run(index, options);
}

// PGM takes epsilon as a template argument, so each value is its own type.
int dispatch_pgm(const Options& options) {
  switch (options.epsilon) {
    case 4: return run_pgm<4>(options);
    case 8: return run_pgm<8>(options);
    case 16: return run_pgm<16>(options);
    case 32: return run_pgm<32>(options);
    case 64: return run_pgm<64>(options);
    case 128: return run_pgm<128>(options);
    case 256: return run_pgm<256>(options);
    case 512: return run_pgm<512>(options);
    case 1024: return run_pgm<1024>(options);
    case 4096: return run_pgm<4096>(options);
    default:
      std::fprintf(stderr, "no PGM build for epsilon %llu\n",
                   static_cast<unsigned long long>(options.epsilon));
      return 2;
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const std::string value = argv[i + 1];
    if (flag == "--dataset") options.dataset = value;
    else if (flag == "--index") options.index = value;
    else if (flag == "--epsilon") options.epsilon = std::stoull(value);
    else if (flag == "--stride") options.stride = std::stoull(value);
    else if (flag == "--lookups") options.lookups = std::stoull(value);
    else if (flag == "--passes") options.passes = std::stoull(value);
    else if (flag == "--latency") options.latency_samples = std::stoull(value);
    else if (flag == "--seed") options.seed = std::stoull(value);
    else {
      std::fprintf(stderr, "unknown flag %s\n", flag.c_str());
      return 2;
    }
  }
  if (options.dataset.empty()) {
    std::fprintf(stderr, "--dataset is required\n");
    return 2;
  }
  if (options.index == "pgm") return dispatch_pgm(options);
  if (options.index == "binary") {
    BinaryAdapter index;
    return run(index, options);
  }
  if (options.index == "ali") {
    AliAdapter index(options.epsilon);
    return run(index, options);
  }
  std::fprintf(stderr, "unknown index %s\n", options.index.c_str());
  return 2;
}
