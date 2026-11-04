// Operation traces: the input to replay, differential tests and the demo.
//
// generate_trace builds a correctness trace. It deliberately visits the cases
// from docs/API.md that a uniform random workload rarely reaches.
#pragma once

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "ali/rng.h"
#include "ali/types.h"

namespace ali {

enum class OpKind : std::uint64_t {
  kLookup = 0,
  kLowerBound = 1,
  kInsertOrAssign = 2,  // arg is the value.
  kScan = 3,            // arg is the exclusive upper bound.
  kScanToEnd = 4,       // arg is unused.
  kRank = 5,
};

struct Op {
  OpKind kind;
  Key key;
  std::uint64_t arg;
};

struct Trace {
  std::uint64_t seed = 0;
  std::vector<Entry> bulk;
  std::vector<Op> ops;
};

// Where keys come from. Each space stresses a different failure mode.
enum class KeySpace {
  kUniform,    // Random 64-bit keys, huge gaps.
  kDense,      // Consecutive integers, slope of exactly one.
  kNearMax,    // Just below UINT64_MAX, where any double rounding is fatal.
  kNearZero,   // Just above 0, where subtraction can underflow.
  kHugeOffset, // Tiny gaps above 2^63: large values, small differences.
  kClustered,  // Tight clusters separated by huge gaps, hostile to one line.
};

inline const char* to_string(KeySpace space) {
  switch (space) {
    case KeySpace::kUniform: return "uniform";
    case KeySpace::kDense: return "dense";
    case KeySpace::kNearMax: return "near_max";
    case KeySpace::kNearZero: return "near_zero";
    case KeySpace::kHugeOffset: return "huge_offset";
    case KeySpace::kClustered: return "clustered";
  }
  return "unknown";
}

inline constexpr KeySpace kAllKeySpaces[] = {
    KeySpace::kUniform,  KeySpace::kDense,      KeySpace::kNearMax,
    KeySpace::kNearZero, KeySpace::kHugeOffset, KeySpace::kClustered,
};

inline KeySpace key_space_from_string(const std::string& name) {
  for (KeySpace space : kAllKeySpaces) {
    if (name == to_string(space)) return space;
  }
  throw std::invalid_argument("unknown key space: " + name);
}

struct TraceConfig {
  std::uint64_t seed = 1;
  std::size_t bulk_keys = 1000;
  std::size_t ops = 10000;
  KeySpace space = KeySpace::kUniform;
};

namespace detail {

constexpr Key kMaxKey = ~Key{0};

inline Key draw_key(Rng& rng, KeySpace space, std::size_t bulk_keys) {
  const std::uint64_t span = 4 * static_cast<std::uint64_t>(bulk_keys) + 16;
  switch (space) {
    case KeySpace::kUniform:
      return rng.next();
    case KeySpace::kDense:
      return 1'000'000 + rng.below(span);
    case KeySpace::kNearMax:
      return kMaxKey - rng.below(span);
    case KeySpace::kNearZero:
      return rng.below(span);
    case KeySpace::kHugeOffset:
      return (Key{1} << 63) + rng.below(span);
    case KeySpace::kClustered:
      return (rng.below(8) << 60) + rng.below(span);
  }
  return 0;
}

// Keeps a sorted copy of the keys the trace has stored, so that bursts can aim
// at real neighbours instead of hoping a random key lands near one.
class KeySet {
 public:
  explicit KeySet(std::vector<Key> keys) : keys_(std::move(keys)) {
    std::sort(keys_.begin(), keys_.end());
    keys_.erase(std::unique(keys_.begin(), keys_.end()), keys_.end());
  }

  bool empty() const { return keys_.empty(); }
  Key min() const { return keys_.front(); }
  Key max() const { return keys_.back(); }
  Key pick(Rng& rng) const { return keys_[rng.below(keys_.size())]; }

  void add(Key key) {
    const auto it = std::lower_bound(keys_.begin(), keys_.end(), key);
    if (it == keys_.end() || *it != key) keys_.insert(it, key);
  }

 private:
  std::vector<Key> keys_;
};

inline void add_repeated_key_burst(Rng& rng, const KeySet& set, KeySpace space,
                                   const TraceConfig& config,
                                   std::vector<Op>& ops, KeySet& grow) {
  const Key key = !set.empty() && rng.chance(70)
                      ? set.pick(rng)
                      : draw_key(rng, space, config.bulk_keys);
  const std::uint64_t writes = 1 + rng.below(32);
  for (std::uint64_t i = 0; i < writes; ++i) {
    ops.push_back({OpKind::kInsertOrAssign, key, rng.next()});
    if (rng.chance(30)) ops.push_back({OpKind::kLookup, key, 0});
  }
  ops.push_back({OpKind::kLookup, key, 0});
  ops.push_back({OpKind::kRank, key, 0});
  grow.add(key);
}

inline void add_ascending_burst(Rng& rng, const KeySet& set, KeySpace space,
                                const TraceConfig& config, std::vector<Op>& ops,
                                KeySet& grow) {
  const Key start = !set.empty() && rng.chance(50)
                        ? set.max()
                        : draw_key(rng, space, config.bulk_keys);
  const std::uint64_t writes = 1 + rng.below(128);
  Key key = start;
  for (std::uint64_t i = 0; i < writes; ++i) {
    const std::uint64_t step = 1 + rng.below(4);
    if (key > detail::kMaxKey - step) break;  // Never wrap past UINT64_MAX.
    key += step;
    ops.push_back({OpKind::kInsertOrAssign, key, rng.next()});
    grow.add(key);
    if (rng.chance(20)) ops.push_back({OpKind::kLookup, key - step, 0});
  }
}

inline void add_neighbour_burst(Rng& rng, const KeySet& set,
                                std::vector<Op>& ops) {
  if (set.empty()) return;
  const std::uint64_t count = 1 + rng.below(32);
  for (std::uint64_t i = 0; i < count; ++i) {
    const Key key = set.pick(rng);
    const Key below = key == 0 ? 0 : key - 1;
    const Key above = key == detail::kMaxKey ? key : key + 1;
    for (Key probe : {below, key, above}) {
      ops.push_back({OpKind::kLookup, probe, 0});
      ops.push_back({OpKind::kLowerBound, probe, 0});
      ops.push_back({OpKind::kRank, probe, 0});
    }
  }
}

inline void add_ends_burst(const KeySet& set, std::vector<Op>& ops) {
  std::vector<Key> probes = {0, 1, detail::kMaxKey - 1, detail::kMaxKey};
  if (!set.empty()) {
    if (set.min() > 0) probes.push_back(set.min() - 1);
    probes.push_back(set.min());
    probes.push_back(set.max());
    if (set.max() < detail::kMaxKey) probes.push_back(set.max() + 1);
  }
  for (Key probe : probes) {
    ops.push_back({OpKind::kLookup, probe, 0});
    ops.push_back({OpKind::kLowerBound, probe, 0});
    ops.push_back({OpKind::kRank, probe, 0});
  }
}

inline void add_scan_burst(Rng& rng, const KeySet& set, KeySpace space,
                           const TraceConfig& config, std::vector<Op>& ops) {
  const Key lo = !set.empty() && rng.chance(70)
                     ? set.pick(rng)
                     : draw_key(rng, space, config.bulk_keys);
  // Ranges stay short so a million-op trace does not spend its time scanning.
  const Key width = rng.below(64);
  const Key hi = lo > detail::kMaxKey - width ? detail::kMaxKey : lo + width;
  ops.push_back({OpKind::kScan, lo, hi});
  ops.push_back({OpKind::kScan, lo, lo});  // Empty: lo == hi.
  if (hi > lo) ops.push_back({OpKind::kScan, hi, lo});  // Empty: lo > hi.
  if (!set.empty()) {
    const Key near_top = set.max() < 8 ? 0 : set.max() - 7;
    ops.push_back({OpKind::kScanToEnd, near_top, 0});
  }
}

inline void add_random_burst(Rng& rng, KeySpace space, const TraceConfig& config,
                             std::vector<Op>& ops, KeySet& grow) {
  const std::uint64_t count = 1 + rng.below(32);
  for (std::uint64_t i = 0; i < count; ++i) {
    const Key key = draw_key(rng, space, config.bulk_keys);
    if (rng.chance(50)) {
      ops.push_back({OpKind::kInsertOrAssign, key, rng.next()});
      grow.add(key);
    } else {
      ops.push_back({OpKind::kLookup, key, 0});
    }
  }
}

}  // namespace detail

inline Trace generate_trace(const TraceConfig& config) {
  Rng rng(config.seed);
  Trace trace;
  trace.seed = config.seed;

  std::vector<Key> keys;
  keys.reserve(config.bulk_keys);
  for (std::size_t i = 0; i < config.bulk_keys; ++i) {
    keys.push_back(detail::draw_key(rng, config.space, config.bulk_keys));
  }
  std::sort(keys.begin(), keys.end());
  // Keys already contain repeats in the narrow spaces, and repeats reach
  // bulk_load on purpose: the last value must win.
  trace.bulk.reserve(keys.size());
  for (Key key : keys) trace.bulk.push_back({key, rng.next()});

  detail::KeySet set(keys);
  trace.ops.reserve(config.ops + 256);
  while (trace.ops.size() < config.ops) {
    switch (rng.below(6)) {
      case 0:
        detail::add_repeated_key_burst(rng, set, config.space, config,
                                       trace.ops, set);
        break;
      case 1:
        detail::add_ascending_burst(rng, set, config.space, config, trace.ops,
                                    set);
        break;
      case 2:
        detail::add_neighbour_burst(rng, set, trace.ops);
        break;
      case 3:
        detail::add_ends_burst(set, trace.ops);
        break;
      case 4:
        detail::add_scan_burst(rng, set, config.space, config, trace.ops);
        break;
      default:
        detail::add_random_burst(rng, config.space, config, trace.ops, set);
        break;
    }
  }
  return trace;
}

// File layout, little-endian: 4-word header, then the bulk entries, then ops.
inline constexpr std::uint64_t kTraceMagic = 0x3130454341525441ULL;  // "ATRACE01"

inline void write_trace(const Trace& trace, const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) throw std::runtime_error("cannot write " + path);
  const std::uint64_t header[4] = {kTraceMagic, trace.seed, trace.bulk.size(),
                                   trace.ops.size()};
  bool ok = std::fwrite(header, sizeof(header), 1, file) == 1;
  ok = ok && std::fwrite(trace.bulk.data(), sizeof(Entry), trace.bulk.size(),
                         file) == trace.bulk.size();
  ok = ok && std::fwrite(trace.ops.data(), sizeof(Op), trace.ops.size(),
                         file) == trace.ops.size();
  std::fclose(file);
  if (!ok) throw std::runtime_error("short write to " + path);
}

inline Trace read_trace(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) throw std::runtime_error("cannot read " + path);
  std::uint64_t header[4];
  bool ok = std::fread(header, sizeof(header), 1, file) == 1 &&
            header[0] == kTraceMagic;
  Trace trace;
  if (ok) {
    trace.seed = header[1];
    trace.bulk.resize(header[2]);
    trace.ops.resize(header[3]);
    ok = std::fread(trace.bulk.data(), sizeof(Entry), trace.bulk.size(),
                    file) == trace.bulk.size() &&
         std::fread(trace.ops.data(), sizeof(Op), trace.ops.size(), file) ==
             trace.ops.size();
  }
  std::fclose(file);
  if (!ok) throw std::runtime_error("bad trace file " + path);
  return trace;
}

}  // namespace ali
