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

using AdaptiveIndex = DynamicIndex<AdaptivePolicy>;

AdaptiveConfig frozen_costs() {
  AdaptiveConfig config;
  config.measure_costs = false;  // Decisions must not depend on the clock here.
  config.min_samples = 16;
  return config;
}

std::vector<Entry> even_keys(std::size_t count) {
  std::vector<Entry> entries;
  for (Key key = 0; key < count; ++key) entries.push_back({key * 2, key});
  return entries;
}

// Writes alone cannot justify a merge while the buffer is small: nobody reads
// it, and shifting a few entries is cheap.
void check_adaptive_leaves_write_only_leaves_alone() {
  DynamicConfig config;
  config.max_leaf_keys = 1 << 14;
  AdaptiveIndex index(config, AdaptivePolicy(frozen_costs()));
  index.bulk_load(even_keys(1 << 14));
  for (Key key = 0; key < 200; ++key) index.insert_or_assign(key * 2 + 1, key);
  CHECK(index.stats().rebuilds == 0);
  index.check_invariants();
}

// Reads that hit the base never touch the delta, so they must earn no rebuild.
void check_adaptive_ignores_reads_that_skip_the_delta() {
  DynamicConfig config;
  AdaptiveIndex index(config, AdaptivePolicy(frozen_costs()));
  index.bulk_load(even_keys(1 << 12));
  for (Key key = 0; key < 64; ++key) index.insert_or_assign(key * 2 + 1, key);
  for (int round = 0; round < 100; ++round) {
    for (Key key = 0; key < 1 << 12; ++key) CHECK(index.lookup(key * 2));
  }
  CHECK(index.stats().rebuilds == 0);
}

// Reads of buffered keys do pay, so enough of them must trigger a merge, and
// afterwards those keys are found in the base.
void check_adaptive_rebuilds_a_leaf_with_a_hot_delta() {
  DynamicConfig config;
  AdaptiveIndex index(config, AdaptivePolicy(frozen_costs()));
  index.bulk_load(even_keys(1 << 12));
  for (Key key = 0; key < 64; ++key) index.insert_or_assign(key * 2 + 1, key);
  CHECK(index.stats().rebuilds == 0);
  for (int round = 0; round < 200000 && index.stats().rebuilds == 0; ++round) {
    CHECK(index.lookup(static_cast<Key>(round % 64) * 2 + 1));
  }
  CHECK(index.stats().rebuilds == 1);
  CHECK(index.explain_lookup(1).in_base);
  index.check_invariants();
}

// The decision must rise with reads and fall with leaf size.
void check_adaptive_decision_moves_the_right_way() {
  const AdaptivePolicy policy(frozen_costs());
  AdaptivePolicy::LeafState state;
  state.reads = 1000;
  const LeafView small{1 << 10, 64};
  const LeafView large{1 << 14, 64};
  CHECK(policy.expected_saving_ns(state, small) > 0);
  CHECK(policy.merge_cost_ns(large) > policy.merge_cost_ns(small));
  AdaptivePolicy::LeafState busier = state;
  busier.reads = 2000;
  CHECK(policy.expected_saving_ns(busier, small) > policy.expected_saving_ns(state, small));
  // If writes will refill the buffer at once, merging saves less per read.
  AdaptivePolicy::LeafState writing = state;
  writing.writes = 1 << 12;
  const double read_part_with_refill =
      policy.expected_saving_ns(writing, small) -
      writing.writes * frozen_costs().ns_per_shifted_entry * 64 / 2;
  CHECK(read_part_with_refill < policy.expected_saving_ns(state, small));
}

// Sealing must leave the base alone, and every key must stay reachable.
void check_seal_keeps_appended_keys_and_skips_the_merge() {
  DynamicConfig config;
  config.max_leaf_keys = 1 << 12;
  config.seal_tail_appends = true;
  FixedIndex index(config, FixedPolicy(32));
  index.bulk_load(even_keys(1 << 10));
  const Key start = 2 << 10;
  for (Key i = 0; i < 1024; ++i) index.insert_or_assign(start + i * 2, i);
  index.check_invariants();
  CHECK(index.stats().seals > 0);
  CHECK(index.stats().keys_merged <= 1024);  // Only the appended keys moved.
  CHECK(index.lookup(start + 2 * 1023) == Value{1023});
  CHECK(index.lower_bound(start + 1)->key == start + 2);
  CHECK(index.rank(start) == 1 << 10);
  // A sealed leaf gives its buffer back. Keeping the capacity once cost 5 bytes
  // per key on the append workload.
  const MemoryReport memory = index.memory();
  CHECK(memory.capacity < memory.total_used() + 64 * sizeof(Entry) + 8192);
}

void check_rejected_load_keeps_contents() {
  FixedIndex index = make_index(4, 64, 8);
  const std::vector<Entry> good = {{1, 10}, {2, 20}};
  index.bulk_load(good);
  index.insert_or_assign(3, 30);
  const std::vector<Entry> bad = {{9, 1}, {8, 1}};
  bool threw = false;
  try {
    index.bulk_load(bad);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(index.lookup(3) == Value{30});
  CHECK(index.size() == 3);
  index.check_invariants();
}

// Old activity must stop counting. A leaf with plenty of recent reads goes idle
// while other leaves stay busy, and must lose that evidence.
void check_counters_decay_while_a_leaf_is_idle() {
  AdaptiveConfig adaptive = frozen_costs();
  adaptive.half_life_log2 = 8;
  AdaptivePolicy policy(adaptive);
  AdaptivePolicy::LeafState idle;
  AdaptivePolicy::LeafState busy;
  for (int i = 0; i < 200; ++i) policy.on_delta_read(idle);
  CHECK(idle.reads == 200);  // Inside one half-life, nothing has decayed.
  for (int i = 0; i < 1 << 12; ++i) policy.on_delta_read(busy);
  policy.on_delta_read(idle);  // Touching a leaf applies the decay it missed.
  CHECK(idle.reads <= 2);
}

void check_extreme_half_life_is_safe() {
  AdaptiveConfig adaptive = frozen_costs();
  adaptive.half_life_log2 = 200;
  AdaptivePolicy policy(adaptive);
  AdaptivePolicy::LeafState state;
  policy.on_delta_read(state);
  CHECK(state.reads == 1);
  // A half-life this long means never decaying. An unclamped shift wraps on
  // arm64, which would halve these counters many times over.
  for (int i = 0; i < 1 << 12; ++i) policy.on_delta_read(state);
  CHECK(state.reads == (1 << 12) + 1);
}

// The two gates before the cost comparison. Each case below has savings far above
// the merge cost, so only the gate can refuse it.
void check_minimums_gate_every_decision() {
  AdaptiveConfig adaptive = frozen_costs();
  adaptive.min_samples = 1000;
  adaptive.min_delta_keys = 8;
  AdaptivePolicy policy(adaptive);
  const LeafView small_buffer{10, 7};
  const LeafView enough_buffer{10, 8};

  AdaptivePolicy::LeafState few_samples;
  few_samples.reads = 999;
  CHECK(policy.expected_saving_ns(few_samples, enough_buffer) > 10 * policy.merge_cost_ns(enough_buffer));
  CHECK(!policy.should_rebuild_after_write(few_samples, enough_buffer));

  AdaptivePolicy::LeafState enough_samples;
  enough_samples.reads = 1000;
  CHECK(policy.expected_saving_ns(enough_samples, small_buffer) > 10 * policy.merge_cost_ns(small_buffer));
  CHECK(!policy.should_rebuild_after_write(enough_samples, small_buffer));
  CHECK(policy.should_rebuild_after_write(enough_samples, enough_buffer));
}

// A compiler once deleted the calibration loop and this measured zero, which
// silently removed write costs from every decision.
void check_shift_cost_calibration_measures_something() {
  const AdaptivePolicy policy{AdaptiveConfig{}};
  CHECK(policy.ns_per_shifted_entry() > 0.001);
  CHECK(policy.ns_per_shifted_entry() < 100.0);
}

// The hysteresis promise: after a rebuild the counters are clear, so a single
// burst cannot pay for a second rebuild. The loop alternates a small write burst
// and an equally small read burst, then bounds what the policy spent.
void check_adaptive_does_not_thrash_on_repeated_bursts() {
  DynamicConfig config;
  AdaptiveIndex index(config, AdaptivePolicy(frozen_costs()));
  index.bulk_load(even_keys(1 << 12));
  Key next = 1;
  std::uint64_t reads_of_delta = 0;
  for (int burst = 0; burst < 400; ++burst) {
    std::vector<Key> fresh;
    for (int i = 0; i < 8; ++i, next += 2) {
      if (next >= (1 << 13)) next = 1;
      if (index.insert_or_assign(next, next)) fresh.push_back(next);
    }
    for (Key key : fresh) {
      index.lookup(key);
      ++reads_of_delta;
    }
  }
  // Ski rental: a merge of about 4096 keys costs about 41us at the frozen 10ns
  // per key, and each buffered read saves at most about 20ns * 6 steps. A merge
  // therefore needs on the order of hundreds of reads or more to be earned.
  CHECK(index.stats().rebuilds * 300 <= reads_of_delta);
  index.check_invariants();
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
          for (bool seal : {false, true}) {
            DynamicConfig dynamic;
            dynamic.epsilon = shape.epsilon;
            dynamic.max_leaf_keys = shape.max_leaf;
            dynamic.delta_capacity = std::max<std::size_t>(shape.threshold + 1, 8);
            dynamic.seal_tail_appends = seal;
            FixedIndex index(dynamic, FixedPolicy(shape.threshold));
            ops += replay_with_invariant_checks(index, trace);
            CHECK(index.widened_searches() == 0);
            rebuilds += index.stats().rebuilds;
            splits += index.stats().splits;
            ++traces;
          }
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

// Rebuild timing can never change an answer, so the adaptive policy is replayed
// with measured costs too, where decisions depend on the clock.
void check_adaptive_traces_match_the_oracle() {
  std::size_t traces = 0;
  std::size_t ops = 0;
  std::uint64_t rebuilds = 0;
  for (KeySpace space : kAllKeySpaces) {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
      for (std::size_t bulk_keys : {0UL, 100UL, 5000UL}) {
        for (bool measured : {false, true}) {
          for (std::size_t max_leaf : {std::size_t{16}, std::size_t{256}, std::size_t{4096}}) {
            TraceConfig trace_config;
            trace_config.seed = seed * 15485863 + bulk_keys;
            trace_config.bulk_keys = bulk_keys;
            trace_config.ops = 3000;
            trace_config.space = space;
            const Trace trace = generate_trace(trace_config);
            DynamicConfig config;
            config.epsilon = 4;
            config.max_leaf_keys = max_leaf;
            config.delta_capacity = 32;
            AdaptiveConfig adaptive = frozen_costs();
            adaptive.measure_costs = measured;
            adaptive.min_delta_keys = 2;
            adaptive.min_samples = 4;
            AdaptiveIndex index(config, AdaptivePolicy(adaptive));
            ops += replay_with_invariant_checks(index, trace);
            CHECK(index.widened_searches() == 0);
            rebuilds += index.stats().rebuilds;
            ++traces;
          }
        }
      }
    }
  }
  CHECK(rebuilds > 0);
  std::printf("adaptive: %zu traces, %zu operations matched the oracle, "
              "%llu rebuilds\n",
              traces, ops, static_cast<unsigned long long>(rebuilds));
}

}  // namespace

int main() {
  check_starts_empty_and_accepts_writes();
  check_extreme_keys_survive_rebuilds();
  check_values_survive_a_rebuild();
  check_reads_across_a_split();
  check_rebuild_stats_add_up();
  check_explain_matches_lookup();
  check_adaptive_leaves_write_only_leaves_alone();
  check_adaptive_ignores_reads_that_skip_the_delta();
  check_adaptive_rebuilds_a_leaf_with_a_hot_delta();
  check_adaptive_decision_moves_the_right_way();
  check_rejected_load_keeps_contents();
  check_counters_decay_while_a_leaf_is_idle();
  check_extreme_half_life_is_safe();
  check_minimums_gate_every_decision();
  check_shift_cost_calibration_measures_something();
  check_seal_keeps_appended_keys_and_skips_the_merge();
  check_adaptive_does_not_thrash_on_repeated_bursts();
  check_traces_match_the_oracle();
  check_adaptive_traces_match_the_oracle();
  return 0;
}
