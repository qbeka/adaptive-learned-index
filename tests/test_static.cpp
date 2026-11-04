// Checks the static index: contract, bounds, numerical stability, bad models.
#include <limits>
#include <vector>

#include "ali/map_oracle.h"
#include "ali/replay.h"
#include "ali/static_index.h"
#include "check.h"

using namespace ali;

namespace {

constexpr Key kMax = std::numeric_limits<Key>::max();
constexpr std::uint64_t kEpsilons[] = {0, 1, 4, 64, 1024};

Trace read_only(Trace trace) {
  std::erase_if(trace.ops, [](const Op& op) {
    return op.kind == OpKind::kInsertOrAssign;
  });
  return trace;
}

std::vector<Entry> ascending(Key start, Key stride, std::size_t count) {
  std::vector<Entry> entries;
  for (std::size_t i = 0; i < count; ++i) {
    entries.push_back({start + stride * i, i});
  }
  return entries;
}

void check_empty_and_single() {
  StaticIndex index;
  index.bulk_load({});
  CHECK(!index.lookup(0));
  CHECK(!index.lower_bound(0));
  CHECK(index.rank(kMax) == 0);

  const std::vector<Entry> one = {{kMax, 9}};
  index.bulk_load(one);
  CHECK(index.lookup(kMax) == Value{9});
  CHECK(index.lower_bound(0)->key == kMax);
  CHECK(index.rank(kMax) == 0);
  CHECK(!index.lookup(kMax - 1));
}

// Adjacent keys at the top of the range. A double has 53 bits of mantissa, so
// a cast would collapse them and this test would fail.
void check_adjacent_keys_near_the_maximum() {
  for (std::uint64_t epsilon : kEpsilons) {
    StaticIndex index({epsilon});
    const std::size_t count = 4096;
    const std::vector<Entry> entries = ascending(kMax - (count - 1), 1, count);
    index.bulk_load(entries);
    for (std::size_t i = 0; i < count; ++i) {
      CHECK(index.lookup(entries[i].key) == Value{i});
      CHECK(index.rank(entries[i].key) == i);
    }
    CHECK(index.lower_bound(kMax)->key == kMax);
    CHECK(index.widened_searches() == 0);
  }
}

// Large offsets with tiny differences, plus every position between two keys.
void check_large_offsets_with_tiny_differences() {
  const Key base = (Key{1} << 63) + 12345;
  const std::vector<Entry> entries = ascending(base, 3, 5000);
  StaticIndex index({8});
  index.bulk_load(entries);
  CHECK(index.segment_count() == 1);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const Key key = entries[i].key;
    CHECK(index.lookup(key) == Value{i});
    CHECK(!index.lookup(key + 1));
    CHECK(!index.lookup(key + 2));
    CHECK(index.rank(key + 1) == i + 1);
  }
}

void check_slope_rounding_stays_within_one_position() {
  // A slope of exactly one cannot be stored in 0.64 fixed point. The largest
  // representable slope must still predict within one position.
  const std::vector<Entry> entries = ascending(100, 1, 100000);
  StaticIndex index({0});
  index.bulk_load(entries);
  CHECK(index.max_window() <= 2);
}

void check_observed_error_respects_epsilon() {
  for (KeySpace space : kAllKeySpaces) {
    for (std::uint64_t epsilon : kEpsilons) {
      TraceConfig config;
      config.seed = 99;
      config.bulk_keys = 20000;
      config.ops = 1;
      config.space = space;
      const Trace trace = generate_trace(config);
      StaticIndex index({epsilon});
      index.bulk_load(trace.bulk);
      // The cone allows epsilon on each side. Choosing the fixed-point slope
      // adds at most one position on each side.
      CHECK(index.max_window() <= 2 * (epsilon + 1));
    }
  }
}

void check_segment_cap_is_respected() {
  const std::vector<Entry> entries = ascending(0, 2, 10000);
  StaticIndex index({64, 256});
  index.bulk_load(entries);
  CHECK(index.segment_count() == 40);  // 10000 / 256, rounded up.
  CHECK(index.lookup(5000) == Value{2500});
}

// Found by independent review. An epsilon this large once overflowed the 32-bit
// error bound, leaving a window too small for the keys it had to cover.
void check_huge_epsilon_cannot_truncate_the_error_bound() {
  const std::vector<Entry> entries = {{0, 1}, {Key{1} << 63, 2}, {(Key{1} << 63) + 1, 3}};
  StaticIndex index({(std::uint64_t{1} << 33) + 1});
  index.bulk_load(entries);
  for (const Entry& entry : entries) CHECK(index.lookup(entry.key) == entry.value);
  CHECK(index.lower_bound(1)->key == Key{1} << 63);
  CHECK(index.rank((Key{1} << 63) + 1) == 2);
  CHECK(index.widened_searches() == 0);
}

void check_memory_report_adds_up() {
  const std::vector<Entry> entries = ascending(0, 3, 1000);
  StaticIndex index({0, 100});
  index.bulk_load(entries);
  const MemoryReport report = index.memory();
  CHECK(report.keys == 1000 * sizeof(Key));
  CHECK(report.payloads == 1000 * sizeof(Value));
  CHECK(report.directory == index.segment_count() * sizeof(Key));
  CHECK(report.models > 0);
  CHECK(report.deltas == 0);
  CHECK(report.capacity >= report.total_used());
}

void check_rejected_load_keeps_contents() {
  StaticIndex index;
  const std::vector<Entry> good = {{1, 10}, {2, 20}};
  index.bulk_load(good);
  const std::vector<Entry> bad = {{9, 1}, {8, 1}};
  bool threw = false;
  try {
    index.bulk_load(bad);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(index.lookup(2) == Value{20});
  CHECK(index.size() == 2);
}

void check_traces_match_the_oracle() {
  std::size_t traces = 0;
  std::size_t ops = 0;
  for (KeySpace space : kAllKeySpaces) {
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
      for (std::size_t bulk_keys : {0UL, 1UL, 2UL, 100UL, 5000UL, 50000UL}) {
        for (std::uint64_t epsilon : kEpsilons) {
          TraceConfig config;
          config.seed = seed * 7919 + bulk_keys;
          config.bulk_keys = bulk_keys;
          config.ops = 1000;
          config.space = space;
          const Trace trace = read_only(generate_trace(config));
          StaticIndex index({epsilon});
          MapOracle oracle;
          CHECK(!first_divergence(index, oracle, trace).has_value());
          // The window proof in segment.h says the slow path never runs.
          CHECK(index.widened_searches() == 0);
          ++traces;
          ops += trace.ops.size();
        }
      }
    }
  }
  std::printf("static: %zu traces, %zu operations matched the oracle\n", traces,
              ops);
}

// A wrong model must cost time, never correctness.
void check_corrupted_bounds_never_change_an_answer() {
  std::uint64_t total_widened = 0;
  for (KeySpace space : kAllKeySpaces) {
    TraceConfig config;
    config.seed = 5;
    config.bulk_keys = 20000;
    config.ops = 20000;
    config.space = space;
    const Trace trace = read_only(generate_trace(config));
    StaticIndex index({256});
    MapOracle oracle;
    index.bulk_load(trace.bulk);
    oracle.bulk_load(trace.bulk);
    index.corrupt_error_bounds_for_test();
    for (const Op& op : trace.ops) {
      CHECK(apply_op(index, op) == apply_op(oracle, op));
    }
    total_widened += index.widened_searches();
  }
  CHECK(total_widened > 0);  // Otherwise this test proved nothing.
  std::printf("static: corrupted bounds widened %llu searches, 0 wrong answers\n",
              static_cast<unsigned long long>(total_widened));
}

void check_widen_finds_every_position() {
  const std::vector<Key> keys = {2, 4, 6, 8, 10, 12, 14};
  for (Key key = 0; key <= 16; ++key) {
    const std::size_t want =
        branchless_lower_bound(keys.data(), keys.size(), key);
    for (std::size_t start = 0; start <= keys.size(); ++start) {
      CHECK(widen_lower_bound(keys.data(), keys.size(), start, key) == want);
    }
  }
}

}  // namespace

int main() {
  check_empty_and_single();
  check_adjacent_keys_near_the_maximum();
  check_large_offsets_with_tiny_differences();
  check_slope_rounding_stays_within_one_position();
  check_observed_error_respects_epsilon();
  check_segment_cap_is_respected();
  check_widen_finds_every_position();
  check_huge_epsilon_cannot_truncate_the_error_bound();
  check_rejected_load_keeps_contents();
  check_memory_report_adds_up();
  check_traces_match_the_oracle();
  check_corrupted_bounds_never_change_an_answer();
  return 0;
}
