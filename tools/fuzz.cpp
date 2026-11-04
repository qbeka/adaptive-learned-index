// Randomized differential fuzzing: every seed draws its own index shape, key
// space and trace, deliberately favouring tiny caps that force constant rebuilds,
// and replays against the oracle with structural checks.
//
// Usage: fuzz --seeds 1000 [--first 1] [--ops 4000]
// Prints one JSON line. Exits non-zero, naming the seed, on any disagreement.
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <string>

#include "ali/dynamic_index.h"
#include "ali/map_oracle.h"
#include "ali/replay.h"

using namespace ali;

namespace {

struct Totals {
  std::uint64_t traces = 0;
  std::uint64_t ops = 0;
  std::uint64_t rebuilds = 0;
  std::uint64_t splits = 0;
  std::uint64_t seals = 0;
  std::uint64_t widened = 0;
};

template <class Index>
bool replay(Index& index, const Trace& trace, Totals& totals) {
  MapOracle oracle;
  index.bulk_load(trace.bulk);
  oracle.bulk_load(trace.bulk);
  std::size_t step = 0;
  for (const Op& op : trace.ops) {
    if (!(apply_op(index, op) == apply_op(oracle, op))) return false;
    if (++step % 128 == 0) index.check_invariants();
  }
  index.check_invariants();
  if (index.size() != oracle.size()) return false;
  totals.traces += 1;
  totals.ops += trace.ops.size();
  totals.rebuilds += index.stats().rebuilds;
  totals.splits += index.stats().splits;
  totals.seals += index.stats().seals;
  totals.widened += index.widened_searches();
  return true;
}

constexpr std::size_t kLeafCaps[] = {4, 8, 16, 64, 256, 4096};
constexpr std::size_t kDeltaCaps[] = {2, 4, 8, 32, 128, 1024};
constexpr std::uint64_t kEpsilons[] = {0, 1, 2, 8, 64, 1024};
constexpr std::size_t kBulkSizes[] = {0, 1, 2, 16, 500, 5000};

bool run_seed(std::uint64_t seed, std::size_t ops, Totals& totals) {
  Rng rng(seed);
  TraceConfig trace_config;
  trace_config.seed = rng.next();
  trace_config.bulk_keys = kBulkSizes[rng.below(6)];
  trace_config.ops = ops;
  trace_config.space = kAllKeySpaces[rng.below(6)];
  const Trace trace = generate_trace(trace_config);

  DynamicConfig config;
  config.epsilon = kEpsilons[rng.below(6)];
  config.max_leaf_keys = kLeafCaps[rng.below(6)];
  config.delta_capacity = kDeltaCaps[rng.below(6)];
  config.seal_tail_appends = rng.chance(50);

  if (rng.chance(50)) {
    DynamicIndex<FixedPolicy> index(config, FixedPolicy(1 + rng.below(config.delta_capacity)));
    return replay(index, trace, totals);
  }
  AdaptiveConfig adaptive;
  adaptive.measure_costs = rng.chance(50);
  adaptive.margin = static_cast<double>(rng.below(5));
  adaptive.min_samples = 1 + static_cast<std::uint32_t>(rng.below(64));
  adaptive.min_delta_keys = 1 + rng.below(16);
  adaptive.half_life_log2 = 4 + static_cast<unsigned>(rng.below(20));
  DynamicIndex<AdaptivePolicy> index(config, AdaptivePolicy(adaptive));
  return replay(index, trace, totals);
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t first = 1;
  std::uint64_t seeds = 1000;
  std::size_t ops = 4000;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--first") first = std::stoull(argv[i + 1]);
    else if (flag == "--seeds") seeds = std::stoull(argv[i + 1]);
    else if (flag == "--ops") ops = std::stoull(argv[i + 1]);
  }
  Totals totals;
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t seed = first; seed < first + seeds; ++seed) {
    if (!run_seed(seed, ops, totals)) {
      std::printf("{\"failed_seed\":%" PRIu64 "}\n", seed);
      return 1;
    }
  }
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("{\"seeds\":%" PRIu64 ",\"first_seed\":%" PRIu64 ",\"traces\":%" PRIu64
              ",\"ops\":%" PRIu64 ",\"rebuilds\":%" PRIu64 ",\"splits\":%" PRIu64
              ",\"seals\":%" PRIu64 ",\"widened\":%" PRIu64 ",\"mismatches\":0,\"seconds\":%.1f}\n",
              seeds, first, totals.traces, totals.ops, totals.rebuilds, totals.splits,
              totals.seals, totals.widened, seconds);
  return 0;
}
