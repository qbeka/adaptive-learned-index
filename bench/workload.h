// The four update workloads. Each one replays identically through every index.
//
// A universe of sorted keys is split three ways:
//   bulk:    loaded before the trace starts.
//   pool:    inserted during the trace.
//   absent:  never stored, so a read of one is a guaranteed miss.
//
// Operations arrive in bursts, to match the use case: reads come in runs, and
// late inserts land between existing keys.
#pragma once

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include "ali/replay.h"
#include "ali/rng.h"
#include "ali/trace.h"

namespace ali::bench {

struct WorkloadShape {
  const char* name;
  std::uint64_t read_percent;    // Share of bursts that read.
  std::uint64_t miss_percent;    // Share of reads aimed at absent keys.
  std::uint64_t recent_percent;  // Share of reads aimed at recent inserts.
  std::uint64_t scan_percent;    // Share of reads that are short scans.
  bool append;                   // Inserts are ascending, past all bulk keys.
  bool hotspot;                  // All traffic follows a moving key window.
};

inline constexpr std::array<WorkloadShape, 4> kWorkloads = {{
    {"read_heavy", 95, 10, 20, 1, false, false},
    {"write_heavy", 10, 10, 20, 0, false, false},
    {"append_only", 50, 5, 50, 0, true, false},
    {"shifting_hotspot", 70, 5, 40, 0, false, true},
}};

inline const WorkloadShape& workload_shape(const std::string& name) {
  for (const WorkloadShape& shape : kWorkloads) {
    if (name == shape.name) return shape;
  }
  throw std::invalid_argument("unknown workload: " + name);
}

struct WorkloadTrace {
  Trace trace;
  std::size_t absent_keys = 0;
  std::size_t pool_keys = 0;
  std::uint64_t digest = 0;  // Identifies the trace in the run manifest.
};

namespace detail {

constexpr std::uint64_t kHotspotWindows = 64;
constexpr std::uint64_t kHotspotPhases = 8;  // The window moves 7 times.
constexpr std::size_t kRecentCapacity = std::size_t{1} << 16;
constexpr std::uint64_t kMeanBurst = 256;
constexpr std::size_t kScanKeys = 32;

// Picks from `keys`, restricted to one of kHotspotWindows equal slices when a
// window is active. Slices are by position, so they hold equal key counts.
inline std::size_t pick_index(Rng& rng, std::size_t size, bool windowed,
                              std::uint64_t window) {
  if (!windowed) return rng.below(size);
  const std::size_t begin = size * window / kHotspotWindows;
  const std::size_t end = size * (window + 1) / kHotspotWindows;
  return begin + rng.below(std::max<std::size_t>(end - begin, 1));
}

}  // namespace detail

inline WorkloadTrace generate_workload(const WorkloadShape& shape,
                                       const std::vector<Key>& universe,
                                       std::size_t op_count,
                                       std::uint64_t seed) {
  using namespace detail;
  Rng rng(seed);
  std::vector<Key> bulk;
  std::vector<Key> pool;
  std::vector<Key> absent;
  for (std::size_t i = 0; i < universe.size(); ++i) {
    if (rng.chance(5)) {
      absent.push_back(universe[i]);
    } else if (shape.append ? i < universe.size() / 2 : rng.chance(50)) {
      bulk.push_back(universe[i]);
    } else {
      pool.push_back(universe[i]);
    }
  }
  if (bulk.empty() || pool.empty() || absent.empty()) {
    throw std::invalid_argument("universe too small for a workload");
  }

  WorkloadTrace result;
  result.absent_keys = absent.size();
  result.pool_keys = pool.size();
  Trace& trace = result.trace;
  trace.seed = seed;
  trace.bulk.reserve(bulk.size());
  for (Key key : bulk) trace.bulk.push_back({key, key ^ 0x5bd1e995ULL});
  trace.ops.reserve(op_count + 2 * kMeanBurst);

  std::vector<Key> recent(kRecentCapacity, bulk.back());
  std::size_t recent_count = 0;
  std::size_t next_append = 0;

  while (trace.ops.size() < op_count) {
    std::uint64_t window = 0;
    if (shape.hotspot) {
      // 37 is coprime with 64, so the window jumps around and repeats no slice.
      const std::uint64_t phase = trace.ops.size() * kHotspotPhases / op_count;
      window = phase * 37 % kHotspotWindows;
    }
    const bool read_burst = rng.chance(shape.read_percent);
    const std::uint64_t burst = 1 + rng.below(2 * kMeanBurst);
    for (std::uint64_t i = 0; i < burst && trace.ops.size() < op_count; ++i) {
      if (!read_burst) {
        Key key;
        if (shape.append) {
          key = pool[next_append % pool.size()];
          ++next_append;
        } else {
          key = pool[pick_index(rng, pool.size(), shape.hotspot, window)];
        }
        trace.ops.push_back({OpKind::kInsertOrAssign, key, rng.next()});
        recent[recent_count++ % kRecentCapacity] = key;
        continue;
      }
      if (rng.chance(shape.miss_percent)) {
        const Key key = absent[pick_index(rng, absent.size(), shape.hotspot, window)];
        trace.ops.push_back({OpKind::kLookup, key, 0});
      } else if (recent_count > 0 && rng.chance(shape.recent_percent)) {
        const std::size_t limit = std::min(recent_count, kRecentCapacity);
        trace.ops.push_back({OpKind::kLookup, recent[rng.below(limit)], 0});
      } else if (rng.chance(shape.scan_percent)) {
        const std::size_t at = pick_index(rng, bulk.size(), shape.hotspot, window);
        const std::size_t end = std::min(at + kScanKeys, bulk.size() - 1);
        if (end > at) {
          trace.ops.push_back({OpKind::kScan, bulk[at], bulk[end]});
        }
      } else {
        const Key key = bulk[pick_index(rng, bulk.size(), shape.hotspot, window)];
        trace.ops.push_back({OpKind::kLookup, key, 0});
      }
    }
  }

  std::uint64_t digest = kFnvOffset;
  for (const Entry& entry : trace.bulk) digest = fnv_mix(digest, entry.key);
  for (const Op& op : trace.ops) {
    digest = fnv_mix(digest, static_cast<std::uint64_t>(op.kind));
    digest = fnv_mix(fnv_mix(digest, op.key), op.arg);
  }
  result.digest = digest;
  return result;
}

}  // namespace ali::bench
