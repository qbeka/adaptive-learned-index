// Checks the dynamic index: reads across splits, invariants, oracle agreement.
#include <limits>
#include <vector>

#include "ali/dynamic_index.h"
#include "ali/map_oracle.h"
#include "ali/replay.h"
#include "check.h"

using namespace ali;

namespace {

constexpr Key kMax = std::numeric_limits<Key>::max();

using FixedIndex = DynamicIndex<FixedPolicy>;

FixedIndex make_index(std::uint64_t epsilon, std::size_t max_leaf,
                      std::size_t threshold) {
  DynamicConfig config;
  config.epsilon = epsilon;
  config.max_leaf_keys = max_leaf;
  config.delta_capacity = std::max<std::size_t>(threshold + 1, 8);
  return FixedIndex(config, FixedPolicy(threshold));
}

void check_starts_empty_and_accepts_writes() {
  FixedIndex index = make_index(4, 64, 4);
  CHECK(index.size() == 0);
  CHECK(!index.lookup(5));
  CHECK(!index.lower_bound(0));
  CHECK(index.scan({0, std::nullopt}, [](const Entry&) {}) == 0);
  CHECK(index.insert_or_assign(5, 50));
  CHECK(!index.insert_or_assign(5, 51));
  CHECK(index.lookup(5) == Value{51});
  CHECK(index.size() == 1);
  index.check_invariants();
}

void check_extreme_keys_survive_rebuilds() {
  FixedIndex index = make_index(4, 8, 2);
  CHECK(index.insert_or_assign(kMax, 1));
  CHECK(index.insert_or_assign(0, 2));
  CHECK(index.insert_or_assign(kMax - 1, 3));
  CHECK(index.insert_or_assign(1, 4));
  index.check_invariants();
  CHECK(index.lookup(kMax) == Value{1});
  CHECK(index.lookup(0) == Value{2});
  CHECK(index.lower_bound(2)->key == kMax - 1);
  CHECK(index.rank(kMax) == 3);
  CHECK(index.scan({0, kMax}, [](const Entry&) {}) == 3);
  CHECK(index.scan({0, std::nullopt}, [](const Entry&) {}) == 4);
}

// A value overwritten in the base must not reappear after a rebuild, and a
// value written to the delta must survive one. Both directions matter.
void check_values_survive_a_rebuild() {
  std::vector<Entry> entries;
  for (Key key = 0; key < 100; key += 2) entries.push_back({key, key});
  FixedIndex index = make_index(4, 1024, 4);
  index.bulk_load(entries);

  CHECK(!index.insert_or_assign(10, 1000));  // In the base: overwritten in place.
  CHECK(index.insert_or_assign(11, 1100));   // New: goes to the delta.
  CHECK(index.insert_or_assign(13, 1300));
  CHECK(!index.insert_or_assign(11, 1101));  // In the delta: replaced there.
  CHECK(index.insert_or_assign(15, 1500));
  CHECK(index.stats().rebuilds == 0);
  CHECK(index.insert_or_assign(17, 1700));  // Fourth new key: triggers a rebuild.
  CHECK(index.stats().rebuilds == 1);
  index.check_invariants();
  CHECK(index.lookup(10) == Value{1000});
  CHECK(index.lookup(11) == Value{1101});
  CHECK(index.lookup(17) == Value{1700});
}

// Reads immediately before and after a split must see the same contents.
void check_reads_across_a_split() {
  std::vector<Entry> entries;
  for (Key key = 0; key < 32; ++key) entries.push_back({key * 1000, key});
  FixedIndex index = make_index(0, 32, 8);
  index.bulk_load(entries);
  MapOracle oracle;
  oracle.bulk_load(entries);
  const std::size_t leaves_before = index.leaf_count();

  const auto all_equal = [&]() {
    for (Key key = 0; key < 32'000; key += 7) {
      CHECK(index.lookup(key) == oracle.lookup(key));
      const auto got = index.lower_bound(key);
      const auto want = oracle.lower_bound(key);
      CHECK(got == want);
    }
  };

  // Keys that bend the line, so epsilon 0 must split the leaf.
  std::size_t writes = 0;
  while (index.leaf_count() == leaves_before) {
    const Key key = 3 * writes + 1;
    index.insert_or_assign(key, key);
    oracle.insert_or_assign(key, key);
    ++writes;
    CHECK(writes < 64);
    all_equal();
    index.check_invariants();
  }
  CHECK(index.stats().splits > 0);
  all_equal();
}

void check_rebuild_stats_add_up() {
  FixedIndex index = make_index(8, 1 << 10, 16);
  for (Key key = 0; key < 1000; ++key) index.insert_or_assign(key * 3, key);
  const RebuildStats& stats = index.stats();
  CHECK(stats.rebuilds >= 1000 / 16);
  CHECK(stats.keys_merged >= 1000);
  CHECK(stats.bytes_moved >= stats.keys_merged * sizeof(Entry));
  CHECK(stats.total_ns >= stats.worst_ns);
  index.check_invariants();
}

void check_explain_matches_lookup() {
  std::vector<Entry> entries;
  for (Key key = 0; key < 1000; ++key) entries.push_back({key * key, key});
  FixedIndex index = make_index(4, 1 << 12, 64);
  index.bulk_load(entries);
  index.insert_or_assign(7, 777);
  for (Key key : {Key{0}, Key{7}, Key{8}, Key{9}, Key{998001}, Key{998002}}) {
    const LookupTrace trace = index.explain_lookup(key);
    const std::optional<Value> value = index.lookup(key);
    CHECK((trace.in_base || trace.in_delta) == value.has_value());
    if (value) CHECK(trace.value == *value);
    if (trace.in_base) {
      CHECK(trace.base_position >= trace.window_lo);
      CHECK(trace.base_position <= trace.window_hi);
    }
  }
}

template <class Index>
std::size_t replay_with_invariant_checks(Index& index, const Trace& trace) {
  MapOracle oracle;
  index.bulk_load(trace.bulk);
  oracle.bulk_load(trace.bulk);
  std::size_t step = 0;
  for (const Op& op : trace.ops) {
    CHECK(apply_op(index, op) == apply_op(oracle, op));
    if (++step % 64 == 0) index.check_invariants();
  }
  index.check_invariants();
  CHECK(index.size() == oracle.size());
  return trace.ops.size();
}

void check_traces_match_the_oracle() {
  struct Shape {
    std::uint64_t epsilon;
    std::size_t max_leaf;
    std::size_t threshold;
  };
  // Small caps force constant rebuilds and splits, which is the point here.
  const Shape shapes[] = {{0, 8, 2}, {1, 16, 3}, {4, 64, 8}, {16, 256, 32}, {64, 4096, 256}};
  std::size_t traces = 0;
  std::size_t ops = 0;
  std::uint64_t rebuilds = 0;
  std::uint64_t splits = 0;
  for (KeySpace space : kAllKeySpaces) {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
      for (std::size_t bulk_keys : {0UL, 1UL, 2UL, 100UL, 5000UL}) {
        for (const Shape& shape : shapes) {
          TraceConfig config;
          config.seed = seed * 104729 + bulk_keys;
          config.bulk_keys = bulk_keys;
          config.ops = 3000;
          config.space = space;
          const Trace trace = generate_trace(config);
          FixedIndex index = make_index(shape.epsilon, shape.max_leaf, shape.threshold);
          ops += replay_with_invariant_checks(index, trace);
          CHECK(index.widened_searches() == 0);
          rebuilds += index.stats().rebuilds;
          splits += index.stats().splits;
          ++traces;
        }
      }
    }
  }
  CHECK(rebuilds > 0);
  CHECK(splits > 0);
  std::printf("dynamic: %zu traces, %zu operations matched the oracle, "
              "%llu rebuilds, %llu splits\n",
              traces, ops, static_cast<unsigned long long>(rebuilds),
              static_cast<unsigned long long>(splits));
}

}  // namespace

int main() {
  check_starts_empty_and_accepts_writes();
  check_extreme_keys_survive_rebuilds();
  check_values_survive_a_rebuild();
  check_reads_across_a_split();
  check_rebuild_stats_add_up();
  check_explain_matches_lookup();
  check_traces_match_the_oracle();
  return 0;
}
