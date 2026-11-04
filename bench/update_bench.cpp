// The update experiment: one index, one workload, one process, one JSON line.
// scripts/bench_update.py runs this repeatedly and pairs the results.
//
// Modes are separate runs on purpose. Per-operation clock reads and allocation
// tracking both disturb timing, so only throughput mode gives a throughput
// number, and only the other modes give latency and memory.
//
// Usage: update_bench --dataset PATH --workload NAME --index NAME
//                     --mode throughput|latency|memory [--stride N] [--ops N]
//                     [--seed N] [--threshold N] [--margin X] [--half-life N]
//                     [--epsilon N] [--max-leaf N] [--delta-cap N]
//                     [--seal-tail 0|1]
#include <malloc/malloc.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "alex.h"
#include "ali/dynamic_index.h"
#include "sosd.h"
#include "timing.h"
#include "tlx/container/btree_map.hpp"
#include "workload.h"

// Counts live bytes as the allocator reserved them, for every index alike. This
// is what catches slack that an index's own size report would not show.
namespace {
bool g_track_allocations = false;
std::int64_t g_live_bytes = 0;
std::int64_t g_peak_bytes = 0;
}  // namespace

void* operator new(std::size_t size) {
  void* pointer = std::malloc(size);
  if (pointer == nullptr) throw std::bad_alloc();
  if (g_track_allocations) {
    g_live_bytes += static_cast<std::int64_t>(malloc_size(pointer));
    g_peak_bytes = std::max(g_peak_bytes, g_live_bytes);
  }
  return pointer;
}

void operator delete(void* pointer) noexcept {
  if (g_track_allocations && pointer != nullptr) {
    g_live_bytes -= static_cast<std::int64_t>(malloc_size(pointer));
  }
  std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
  operator delete(pointer);
}

using namespace ali;
using namespace ali::bench;

namespace {

struct Options {
  std::string dataset;
  std::string workload = "read_heavy";
  std::string index = "adaptive";
  std::string mode = "throughput";
  std::size_t stride = 1;
  std::size_t ops = 10'000'000;
  std::uint64_t seed = 1;
  std::size_t threshold = 256;
  double margin = 1.0;
  unsigned half_life_log2 = 20;
  std::uint64_t epsilon = 64;
  std::size_t max_leaf = std::size_t{1} << 14;
  std::size_t delta_capacity = 4096;
  bool seal_tail = false;
};

template <int kLeafSlots>
struct BtreeTraits {
  static const bool self_verify = false;
  static const bool debug = false;
  static const int leaf_slots = kLeafSlots;
  static const int inner_slots = kLeafSlots;
  static const std::size_t binsearch_threshold = 256;
};

template <int kLeafSlots>
class BtreeAdapter {
 public:
  void bulk_load(const std::vector<Entry>& entries) {
    std::vector<std::pair<Key, Value>> pairs;
    pairs.reserve(entries.size());
    for (const Entry& entry : entries) pairs.emplace_back(entry.key, entry.value);
    map_.bulk_load(pairs.begin(), pairs.end());
  }

  std::optional<Value> lookup(Key key) const {
    const auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    return it->second;
  }

  bool insert_or_assign(Key key, Value value) {
    auto [it, inserted] = map_.insert2(key, value);
    if (!inserted) it->second = value;
    return inserted;
  }

  std::uint64_t scan_sum(Key lo, Key hi) const {
    std::uint64_t sum = 0;
    for (auto it = map_.lower_bound(lo); it != map_.end() && it->first < hi; ++it) {
      sum += it->second;
    }
    return sum;
  }

  std::size_t size() const { return map_.size(); }

 private:
  using Map = tlx::btree_map<Key, Value, std::less<Key>, BtreeTraits<kLeafSlots>>;
  Map map_;
};

class AlexAdapter {
 public:
  void bulk_load(const std::vector<Entry>& entries) {
    std::vector<std::pair<Key, Value>> pairs;
    pairs.reserve(entries.size());
    for (const Entry& entry : entries) pairs.emplace_back(entry.key, entry.value);
    alex_.bulk_load(pairs.data(), static_cast<int>(pairs.size()));
  }

  std::optional<Value> lookup(Key key) {
    Value* value = alex_.get_payload(key);
    if (value == nullptr) return std::nullopt;
    return *value;
  }

  // ALEX has no insert-or-assign, so this composes the two calls it has. The
  // lookup happens either way, which is what the contract's semantics require.
  bool insert_or_assign(Key key, Value value) {
    if (Value* existing = alex_.get_payload(key)) {
      *existing = value;
      return false;
    }
    alex_.insert(key, value);
    return true;
  }

  std::uint64_t scan_sum(Key lo, Key hi) {
    std::uint64_t sum = 0;
    for (auto it = alex_.lower_bound(lo); !it.is_end() && it.key() < hi; ++it) {
      sum += it.payload();
    }
    return sum;
  }

  std::size_t size() const { return static_cast<std::size_t>(alex_.size()); }

 private:
  alex::Alex<Key, Value, alex::AlexCompare, std::allocator<std::pair<Key, Value>>, false> alex_;
};

template <class Policy>
class LearnedAdapter {
 public:
  LearnedAdapter(DynamicConfig config, Policy policy) : index_(config, policy) {}

  void bulk_load(const std::vector<Entry>& entries) { index_.bulk_load(entries); }
  std::optional<Value> lookup(Key key) { return index_.lookup(key); }
  bool insert_or_assign(Key key, Value value) {
    return index_.insert_or_assign(key, value);
  }
  std::uint64_t scan_sum(Key lo, Key hi) {
    std::uint64_t sum = 0;
    index_.scan({lo, hi}, [&sum](const Entry& entry) { sum += entry.value; });
    return sum;
  }
  std::size_t size() const { return index_.size(); }
  const DynamicIndex<Policy>& index() const { return index_; }

 private:
  DynamicIndex<Policy> index_;
};

// Applies one operation and folds its result into a checksum that every index
// must reproduce exactly.
template <class Index>
inline void apply(Index& index, const Op& op, std::uint64_t& checksum) {
  switch (op.kind) {
    case OpKind::kLookup: {
      const std::optional<Value> value = index.lookup(op.key);
      checksum += value ? *value : 1;
      break;
    }
    case OpKind::kInsertOrAssign:
      checksum += index.insert_or_assign(op.key, op.arg) ? 3 : 5;
      break;
    case OpKind::kScan:
      checksum += index.scan_sum(op.key, op.arg);
      break;
    default:
      break;
  }
}

struct RunResult {
  std::uint64_t build_ns = 0;
  std::uint64_t replay_ns = 0;
  std::uint64_t checksum = 0;
  std::size_t final_size = 0;
  // Latency mode.
  std::uint64_t read_p50 = 0, read_p99 = 0, write_p50 = 0, write_p99 = 0;
  std::uint64_t worst_ns = 0;
  // Memory mode.
  std::int64_t loaded_bytes = 0, final_bytes = 0, peak_bytes = 0;
  // Throughput mode: elapsed nanoseconds per chunk of operations.
  std::vector<std::uint64_t> chunk_ns;
};

constexpr std::size_t kChunkOps = std::size_t{1} << 18;

template <class Index>
RunResult replay(Index& index, const Trace& trace, const std::string& mode) {
  RunResult result;
  const std::int64_t before = g_live_bytes;
  g_track_allocations = mode == "memory";

  const std::uint64_t build_start = now_ns();
  index.bulk_load(trace.bulk);
  result.build_ns = now_ns() - build_start;
  result.loaded_bytes = g_live_bytes - before;

  std::uint64_t checksum = 0;
  if (mode == "latency") {
    std::vector<std::uint32_t> reads;
    std::vector<std::uint32_t> writes;
    reads.reserve(trace.ops.size());
    writes.reserve(trace.ops.size());
    const std::uint64_t start = now_ns();
    for (const Op& op : trace.ops) {
      const std::uint64_t t0 = now_ns();
      apply(index, op, checksum);
      const std::uint64_t spent = now_ns() - t0;
      (op.kind == OpKind::kInsertOrAssign ? writes : reads)
          .push_back(static_cast<std::uint32_t>(spent));
      result.worst_ns = std::max(result.worst_ns, spent);
    }
    result.replay_ns = now_ns() - start;
    result.read_p50 = percentile(reads, 50);
    result.read_p99 = percentile(reads, 99);
    result.write_p50 = percentile(writes, 50);
    result.write_p99 = percentile(writes, 99);
  } else {
    const std::size_t count = trace.ops.size();
    result.chunk_ns.reserve(count / kChunkOps + 1);
    const std::uint64_t start = now_ns();
    std::uint64_t chunk_start = start;
    for (std::size_t i = 0; i < count; ++i) {
      apply(index, trace.ops[i], checksum);
      if (((i + 1) & (kChunkOps - 1)) == 0) {
        const std::uint64_t now = now_ns();
        result.chunk_ns.push_back(now - chunk_start);
        chunk_start = now;
      }
    }
    result.replay_ns = now_ns() - start;
  }
  result.final_bytes = g_live_bytes - before;
  result.peak_bytes = g_peak_bytes - before;
  g_track_allocations = false;
  result.checksum = checksum;
  result.final_size = index.size();
  return result;
}

template <class Index>
void print_policy_fields(const Index& adapter) {
  if constexpr (requires { adapter.index(); }) {
    const auto& index = adapter.index();
    const RebuildStats& stats = index.stats();
    const MemoryReport memory = index.memory();
    std::printf(",\"rebuilds\":%" PRIu64 ",\"forced_rebuilds\":%" PRIu64
                ",\"seals\":%" PRIu64 ",\"splits\":%" PRIu64 ",\"keys_merged\":%" PRIu64
                ",\"bytes_moved\":%" PRIu64 ",\"rebuild_ns\":%" PRIu64
                ",\"worst_rebuild_ns\":%" PRIu64 ",\"leaves\":%zu"
                ",\"widened\":%" PRIu64 ",\"delta_bytes\":%zu,\"directory_bytes\":%zu"
                ",\"model_bytes\":%zu,\"capacity_bytes\":%zu",
                stats.rebuilds, stats.forced_rebuilds, stats.seals, stats.splits,
                stats.keys_merged, stats.bytes_moved, stats.total_ns,
                stats.worst_ns, index.leaf_count(), index.widened_searches(),
                memory.deltas, memory.directory, memory.models, memory.capacity);
    if constexpr (kInstrumented) {
      const WorkCounters& work = index.work();
      std::printf(",\"reads\":%" PRIu64 ",\"writes\":%" PRIu64
                  ",\"directory_steps\":%" PRIu64 ",\"window_steps\":%" PRIu64
                  ",\"delta_steps\":%" PRIu64 ",\"delta_shifted\":%" PRIu64,
                  work.reads, work.writes, work.directory_steps,
                  work.window_steps, work.delta_steps, work.delta_shifted);
    }
    if constexpr (requires { index.policy().ns_per_step(); }) {
      std::printf(",\"ns_per_step\":%.2f,\"ns_per_merged_key\":%.2f"
                  ",\"ns_per_shifted_entry\":%.3f",
                  index.policy().ns_per_step(), index.policy().ns_per_merged_key(),
                  index.policy().ns_per_shifted_entry());
    }
  }
}

template <class Index>
int run(Index& index, const Options& options, const WorkloadTrace& workload,
        std::size_t universe_keys) {
  const RunResult result = replay(index, workload.trace, options.mode);
  const double ops = static_cast<double>(workload.trace.ops.size());
  const double keys = static_cast<double>(result.final_size);
  std::printf(
      "{\"dataset\":\"%s\",\"workload\":\"%s\",\"index\":\"%s\",\"mode\":\"%s\""
      ",\"seed\":%" PRIu64 ",\"stride\":%zu,\"threshold\":%zu,\"margin\":%.3f"
      ",\"half_life_log2\":%u,\"epsilon\":%" PRIu64 ",\"max_leaf\":%zu"
      ",\"delta_capacity\":%zu,\"universe_keys\":%zu,\"bulk_keys\":%zu,\"ops\":%zu"
      ",\"trace_digest\":\"%016" PRIx64 "\",\"checksum\":\"%016" PRIx64 "\""
      ",\"final_keys\":%zu,\"build_ns\":%" PRIu64 ",\"replay_ns\":%" PRIu64
      ",\"ops_per_s\":%.0f,\"read_p50_ns\":%" PRIu64 ",\"read_p99_ns\":%" PRIu64
      ",\"write_p50_ns\":%" PRIu64 ",\"write_p99_ns\":%" PRIu64
      ",\"worst_op_ns\":%" PRIu64 ",\"loaded_bytes_per_key\":%.3f"
      ",\"final_bytes_per_key\":%.3f,\"peak_bytes_per_key\":%.3f",
      options.dataset.substr(options.dataset.find_last_of('/') + 1).c_str(),
      options.workload.c_str(), options.index.c_str(), options.mode.c_str(),
      options.seed, options.stride, options.threshold, options.margin,
      options.half_life_log2, options.epsilon, options.max_leaf,
      options.delta_capacity, universe_keys, workload.trace.bulk.size(),
      workload.trace.ops.size(), workload.digest, result.checksum,
      result.final_size, result.build_ns, result.replay_ns,
      ops * 1e9 / static_cast<double>(result.replay_ns), result.read_p50,
      result.read_p99, result.write_p50, result.write_p99, result.worst_ns,
      static_cast<double>(result.loaded_bytes) / static_cast<double>(workload.trace.bulk.size()),
      static_cast<double>(result.final_bytes) / keys,
      static_cast<double>(result.peak_bytes) / keys);
  print_policy_fields(index);
  std::printf(",\"chunk_ops\":%zu,\"chunk_ns\":[", kChunkOps);
  for (std::size_t i = 0; i < result.chunk_ns.size(); ++i) {
    std::printf("%s%" PRIu64, i == 0 ? "" : ",", result.chunk_ns[i]);
  }
  std::printf("]}\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const std::string value = argv[i + 1];
    if (flag == "--dataset") options.dataset = value;
    else if (flag == "--workload") options.workload = value;
    else if (flag == "--index") options.index = value;
    else if (flag == "--mode") options.mode = value;
    else if (flag == "--stride") options.stride = std::stoull(value);
    else if (flag == "--ops") options.ops = std::stoull(value);
    else if (flag == "--seed") options.seed = std::stoull(value);
    else if (flag == "--threshold") options.threshold = std::stoull(value);
    else if (flag == "--margin") options.margin = std::stod(value);
    else if (flag == "--half-life") options.half_life_log2 = static_cast<unsigned>(std::stoul(value));
    else if (flag == "--epsilon") options.epsilon = std::stoull(value);
    else if (flag == "--max-leaf") options.max_leaf = std::stoull(value);
    else if (flag == "--delta-cap") options.delta_capacity = std::stoull(value);
    else if (flag == "--seal-tail") options.seal_tail = value == "1";
    else {
      std::fprintf(stderr, "unknown flag %s\n", flag.c_str());
      return 2;
    }
  }
  if (options.dataset.empty()) {
    std::fprintf(stderr, "--dataset is required\n");
    return 2;
  }

  prefer_performance_cores();
  const Dataset dataset = make_dataset(load_sosd_keys(options.dataset, options.stride));
  std::vector<Key> universe;
  universe.reserve(dataset.entries.size());
  for (const Entry& entry : dataset.entries) universe.push_back(entry.key);

  const WorkloadTrace workload = generate_workload(
      workload_shape(options.workload), universe, options.ops, options.seed);

  DynamicConfig config;
  config.epsilon = options.epsilon;
  config.max_leaf_keys = options.max_leaf;
  config.delta_capacity = options.delta_capacity;
  config.seal_tail_appends = options.seal_tail;

  if (options.index == "btree16") {
    BtreeAdapter<16> index;
    return run(index, options, workload, universe.size());
  }
  if (options.index == "btree64") {
    BtreeAdapter<64> index;
    return run(index, options, workload, universe.size());
  }
  if (options.index == "btree256") {
    BtreeAdapter<256> index;
    return run(index, options, workload, universe.size());
  }
  if (options.index == "alex") {
    AlexAdapter index;
    return run(index, options, workload, universe.size());
  }
  if (options.index == "fixed") {
    LearnedAdapter<FixedPolicy> index(config, FixedPolicy(options.threshold));
    return run(index, options, workload, universe.size());
  }
  if (options.index == "adaptive") {
    AdaptiveConfig adaptive;
    adaptive.margin = options.margin;
    adaptive.half_life_log2 = options.half_life_log2;
    LearnedAdapter<AdaptivePolicy> index(config, AdaptivePolicy(adaptive));
    return run(index, options, workload, universe.size());
  }
  std::fprintf(stderr, "unknown index %s\n", options.index.c_str());
  return 2;
}
