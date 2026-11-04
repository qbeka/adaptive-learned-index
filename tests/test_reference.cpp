// Checks the sorted-vector reference against the contract and the oracle.
#include <limits>
#include <stdexcept>
#include <vector>

#include "ali/map_oracle.h"
#include "ali/replay.h"
#include "ali/sorted_vector_index.h"
#include "check.h"

using namespace ali;

namespace {

constexpr Key kMax = std::numeric_limits<Key>::max();

template <class Index>
void check_empty_index() {
  Index index;
  index.bulk_load({});
  CHECK(index.size() == 0);
  CHECK(!index.lookup(0));
  CHECK(!index.lookup(kMax));
  CHECK(!index.lower_bound(0));
  CHECK(index.rank(kMax) == 0);
  CHECK(index.scan({0, std::nullopt}, [](const Entry&) {}) == 0);
}

template <class Index>
void check_duplicates_last_wins() {
  Index index;
  const std::vector<Entry> entries = {{5, 1}, {5, 2}, {5, 3}, {9, 4}};
  index.bulk_load(entries);
  CHECK(index.size() == 2);
  CHECK(index.lookup(5) == Value{3});
  CHECK(!index.insert_or_assign(5, 7));
  CHECK(index.lookup(5) == Value{7});
  CHECK(index.size() == 2);
}

template <class Index>
void check_unsorted_input_is_rejected() {
  Index index;
  const std::vector<Entry> good = {{1, 1}};
  index.bulk_load(good);
  const std::vector<Entry> bad = {{2, 1}, {1, 1}};
  bool threw = false;
  try {
    index.bulk_load(bad);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(index.lookup(1) == Value{1});  // Contents survive a rejected load.
}

template <class Index>
void check_extreme_keys() {
  Index index;
  const std::vector<Entry> entries = {{0, 10}, {kMax - 1, 11}, {kMax, 12}};
  index.bulk_load(entries);
  CHECK(index.lookup(0) == Value{10});
  CHECK(index.lookup(kMax) == Value{12});
  CHECK(index.lookup(kMax - 1) == Value{11});
  CHECK(!index.lookup(kMax - 2));
  CHECK(index.lower_bound(1)->key == kMax - 1);
  CHECK(index.lower_bound(kMax)->key == kMax);
  CHECK(index.rank(kMax) == 2);

  std::size_t visited = index.scan({1, kMax}, [](const Entry&) {});
  CHECK(visited == 1);  // A bounded range cannot reach UINT64_MAX.
  visited = index.scan({1, std::nullopt}, [](const Entry&) {});
  CHECK(visited == 2);  // An open range can.
}

template <class Index>
void check_empty_ranges() {
  Index index;
  const std::vector<Entry> entries = {{1, 1}, {2, 2}, {3, 3}};
  index.bulk_load(entries);
  CHECK(index.scan({2, Key{2}}, [](const Entry&) {}) == 0);
  CHECK(index.scan({3, Key{1}}, [](const Entry&) {}) == 0);
}

template <class Index>
void check_contract() {
  check_empty_index<Index>();
  check_duplicates_last_wins<Index>();
  check_unsorted_input_is_rejected<Index>();
  check_extreme_keys<Index>();
  check_empty_ranges<Index>();
}

// An index with a one-off bug: lower_bound skips an exact match.
class BrokenIndex : public SortedVectorIndex {
 public:
  std::optional<Entry> lower_bound(Key key) const {
    return key == kMax ? SortedVectorIndex::lower_bound(key)
                       : SortedVectorIndex::lower_bound(key + 1);
  }
};

// The differential test is only worth trusting if it can fail.
void check_differential_catches_a_bug() {
  TraceConfig config;
  config.seed = 7;
  const Trace trace = generate_trace(config);
  BrokenIndex broken;
  MapOracle oracle;
  const auto divergence = first_divergence(broken, oracle, trace);
  CHECK(divergence.has_value());
  CHECK(divergence->op.kind == OpKind::kLowerBound);
}

void check_trace_generation_is_deterministic() {
  TraceConfig config;
  config.seed = 11;
  const Trace a = generate_trace(config);
  const Trace b = generate_trace(config);
  SortedVectorIndex first;
  SortedVectorIndex second;
  CHECK(replay_digest(first, a) == replay_digest(second, b));
}

void check_seeded_traces_match_the_oracle() {
  std::size_t traces = 0;
  std::size_t ops = 0;
  for (KeySpace space : kAllKeySpaces) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
      for (std::size_t bulk_keys : {0UL, 1UL, 2UL, 100UL, 5000UL}) {
        TraceConfig config;
        config.seed = seed * 1000 + bulk_keys;
        config.bulk_keys = bulk_keys;
        config.ops = 2000;
        config.space = space;
        const Trace trace = generate_trace(config);
        SortedVectorIndex index;
        MapOracle oracle;
        CHECK(!first_divergence(index, oracle, trace).has_value());
        ++traces;
        ops += trace.ops.size();
      }
    }
  }
  std::printf("reference: %zu traces, %zu operations matched the oracle\n",
              traces, ops);
}

}  // namespace

int main() {
  check_contract<MapOracle>();
  check_contract<SortedVectorIndex>();
  check_differential_catches_a_bug();
  check_trace_generation_is_deterministic();
  check_seeded_traces_match_the_oracle();
  return 0;
}
