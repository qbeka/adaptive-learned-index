// The dynamic learned index: leaves of the static design plus write buffers.
//
// Each leaf owns a sorted base array with one linear model, and a small sorted
// delta buffer holding keys that are not in the base.
//
// The one invariant everything rests on: a key is in the base or in the delta,
// never both. A write that finds its key in the base overwrites the value in
// place. So there is no shadowing and no precedence to arbitrate: a buffered
// value can never conflict with a base value. Two consequences:
//   - A read that hits the base is finished without touching the delta, so old
//     data pays nothing for recent inserts.
//   - Only reads of buffered keys, misses and ordered operations pay for the
//     delta. That is the extra work a rebuild can remove.
//
// A rebuild merges one leaf's base and delta, refits it, and replaces it with
// one or more leaves. Routing keys are updated in the same step.
#pragma once

#include <algorithm>
#include <bit>
#include <chrono>
#include <iterator>
#include <memory>
#include <vector>

#include "ali/policy.h"
#include "ali/segment.h"
#include "ali/static_index.h"
#include "ali/types.h"

#ifndef ALI_INSTRUMENT
#define ALI_INSTRUMENT 0
#endif

namespace ali {

inline constexpr bool kInstrumented = ALI_INSTRUMENT != 0;

struct DynamicConfig {
  std::uint64_t epsilon = 64;
  // Bounds the cost of one rebuild, so it bounds the worst pause.
  std::size_t max_leaf_keys = std::size_t{1} << 14;
  // A delta buffer never grows past this, whatever the policy says.
  std::size_t delta_capacity = 4096;
};

// Rebuild accounting is always on: rebuilds are rare and already expensive.
struct RebuildStats {
  std::uint64_t rebuilds = 0;
  std::uint64_t forced_rebuilds = 0;  // Reached delta_capacity.
  std::uint64_t splits = 0;           // Extra leaves created.
  std::uint64_t keys_merged = 0;
  std::uint64_t bytes_moved = 0;      // Merged entries plus directory shifts.
  std::uint64_t total_ns = 0;
  std::uint64_t worst_ns = 0;
};

// Only filled by a build with ALI_INSTRUMENT=1. Those builds are never timed.
struct WorkCounters {
  std::uint64_t reads = 0;
  std::uint64_t writes = 0;
  std::uint64_t directory_steps = 0;
  std::uint64_t window_steps = 0;
  std::uint64_t delta_steps = 0;  // The extra lookup work from buffering.
  std::uint64_t delta_shifted = 0;  // Entries moved by delta inserts.
};

// What one lookup did, for tests and the website recording.
struct LookupTrace {
  std::size_t leaf = 0;
  std::size_t base_keys = 0;
  std::size_t delta_keys = 0;
  std::uint64_t predicted = 0;
  std::uint64_t window_lo = 0;
  std::uint64_t window_hi = 0;
  std::size_t base_position = 0;
  bool in_base = false;
  bool in_delta = false;
  Value value = 0;
};

template <class Policy = FixedPolicy>
class DynamicIndex {
 public:
  explicit DynamicIndex(DynamicConfig config = {}, Policy policy = Policy{})
      : config_(config), policy_(policy) {
    reset_to_one_empty_leaf();
  }

  void bulk_load(std::span<const Entry> entries) {
    std::vector<Key> keys;
    std::vector<Value> values;
    split_sorted_entries(entries, keys, values);  // Throws before any change.
    stats_ = {};
    work_ = {};
    size_ = keys.size();
    if (keys.empty()) {
      reset_to_one_empty_leaf();
      return;
    }
    first_keys_.clear();
    leaves_.clear();
    append_fitted_leaves(keys, values, leaves_, first_keys_);
    first_keys_.shrink_to_fit();
  }

  std::optional<Value> lookup(Key key) {
    const std::size_t index = route(key);
    Leaf& leaf = *leaves_[index];
    if constexpr (kInstrumented) ++work_.reads;
    const std::size_t pos = base_position(leaf, key);
    if (pos < leaf.keys.size() && leaf.keys[pos] == key) return leaf.values[pos];
    if (leaf.delta.empty()) return std::nullopt;

    // Only here does a read pay for the buffer.
    const std::size_t at = sampled_delta_position(leaf, key);
    std::optional<Value> result;
    if (at < leaf.delta.size() && leaf.delta[at].key == key) {
      result = leaf.delta[at].value;
    }
    policy_.on_delta_read(leaf.state);
    maybe_rebuild_after_read(index);
    return result;
  }

  std::optional<Entry> lower_bound(Key key) {
    const std::size_t index = route(key);
    if constexpr (kInstrumented) ++work_.reads;
    std::optional<Entry> result = lower_bound_in_leaf(*leaves_[index], key);
    if (!result && index + 1 < leaves_.size()) {
      // Later leaves are never empty, and only the first leaf can hold delta
      // keys below its first base key, so the next base key is the answer.
      const Leaf& next = *leaves_[index + 1];
      result = Entry{next.keys.front(), next.values.front()};
    }
    if (!leaves_[index]->delta.empty()) {
      policy_.on_delta_read(leaves_[index]->state);
      maybe_rebuild_after_read(index);
    }
    return result;
  }

  bool insert_or_assign(Key key, Value value) {
    const std::size_t index = route(key);
    Leaf& leaf = *leaves_[index];
    if constexpr (kInstrumented) ++work_.writes;
    const std::size_t pos = base_position(leaf, key);
    if (pos < leaf.keys.size() && leaf.keys[pos] == key) {
      leaf.values[pos] = value;  // The invariant: never buffer a base key.
      return false;
    }
    const std::size_t at = delta_position(leaf, key);
    if (at < leaf.delta.size() && leaf.delta[at].key == key) {
      leaf.delta[at].value = value;
      return false;
    }
    if constexpr (kInstrumented) work_.delta_shifted += leaf.delta.size() - at;
    leaf.delta.insert(leaf.delta.begin() + static_cast<std::ptrdiff_t>(at),
                      Entry{key, value});
    ++size_;
    policy_.on_write(leaf.state);
    const bool forced = leaf.delta.size() >= config_.delta_capacity;
    if (forced || policy_.should_rebuild_after_write(leaf.state, view(leaf))) {
      stats_.forced_rebuilds += forced ? 1 : 0;
      rebuild(index);
    }
    return true;
  }

  // Merges the base and delta streams of each leaf in key order. A scan counts
  // as one buffer-paying read per leaf with a delta, and never rebuilds: the
  // visitor must not see the structure change underneath it.
  template <class Visit>
  std::size_t scan(const Range& range, Visit&& visit) {
    if (range.hi && range.lo >= *range.hi) return 0;
    std::size_t visited = 0;
    for (std::size_t index = route(range.lo); index < leaves_.size(); ++index) {
      Leaf& leaf = *leaves_[index];
      if (!leaf.delta.empty()) policy_.on_delta_read(leaf.state);
      std::size_t b = base_position(leaf, range.lo);
      std::size_t d = delta_position(leaf, range.lo);
      while (b < leaf.keys.size() || d < leaf.delta.size()) {
        const bool from_base =
            d == leaf.delta.size() ||
            (b < leaf.keys.size() && leaf.keys[b] < leaf.delta[d].key);
        const Entry entry = from_base ? Entry{leaf.keys[b], leaf.values[b]}
                                      : leaf.delta[d];
        if (!range.contains(entry.key)) return visited;
        visit(entry);
        ++visited;
        b += from_base ? 1 : 0;
        d += from_base ? 0 : 1;
      }
    }
    return visited;
  }

  // Linear in the number of leaves. For verification, see docs/API.md.
  std::size_t rank(Key key) {
    const std::size_t index = route(key);
    std::size_t total = 0;
    for (std::size_t i = 0; i < index; ++i) {
      total += leaves_[i]->keys.size() + leaves_[i]->delta.size();
    }
    const Leaf& leaf = *leaves_[index];
    return total + base_position(leaf, key) + delta_position(leaf, key);
  }

  std::size_t size() const { return size_; }
  std::size_t leaf_count() const { return leaves_.size(); }
  const RebuildStats& stats() const { return stats_; }
  const WorkCounters& work() const { return work_; }
  std::uint64_t widened_searches() const { return widened_; }
  const Policy& policy() const { return policy_; }

  // Runs one lookup and reports what it did, without touching policy state.
  LookupTrace explain_lookup(Key key) {
    LookupTrace trace;
    trace.leaf = route(key);
    const Leaf& leaf = *leaves_[trace.leaf];
    trace.base_keys = leaf.keys.size();
    trace.delta_keys = leaf.delta.size();
    if (!leaf.keys.empty() && key >= leaf.model.first_key) {
      const std::uint64_t n = leaf.keys.size();
      trace.predicted = std::min<std::uint64_t>(predict(leaf.model, key), n);
      trace.window_lo = trace.predicted > leaf.model.over
                            ? trace.predicted - leaf.model.over
                            : 0;
      trace.window_hi = std::min<std::uint64_t>(
          trace.predicted + leaf.model.under + 1, n);
    }
    trace.base_position = base_position(leaf, key);
    if (trace.base_position < leaf.keys.size() &&
        leaf.keys[trace.base_position] == key) {
      trace.in_base = true;
      trace.value = leaf.values[trace.base_position];
      return trace;
    }
    const std::size_t at = delta_position(leaf, key);
    if (at < leaf.delta.size() && leaf.delta[at].key == key) {
      trace.in_delta = true;
      trace.value = leaf.delta[at].value;
    }
    return trace;
  }

  // Read-only views of a leaf, for the website recording.
  std::span<const Key> leaf_keys(std::size_t index) const { return leaves_[index]->keys; }
  std::span<const Entry> leaf_delta(std::size_t index) const { return leaves_[index]->delta; }
  const SegmentModel& leaf_model(std::size_t index) const { return leaves_[index]->model; }

  MemoryReport memory() const {
    MemoryReport report;
    report.directory = first_keys_.size() * sizeof(Key) +
                       leaves_.size() * sizeof(std::unique_ptr<Leaf>);
    report.models = leaves_.size() * sizeof(Leaf);
    report.capacity = first_keys_.capacity() * sizeof(Key) +
                      leaves_.capacity() * sizeof(std::unique_ptr<Leaf>) +
                      report.models;
    for (const auto& leaf : leaves_) {
      report.keys += leaf->keys.size() * sizeof(Key);
      report.payloads += leaf->values.size() * sizeof(Value);
      report.deltas += leaf->delta.size() * sizeof(Entry);
      report.capacity += leaf->keys.capacity() * sizeof(Key) +
                         leaf->values.capacity() * sizeof(Value) +
                         leaf->delta.capacity() * sizeof(Entry);
    }
    return report;
  }

  // Throws if any structural invariant is broken. Tests call this between ops.
  void check_invariants() const {
    std::size_t total = 0;
    Key previous = 0;
    bool any = false;
    if (first_keys_.size() != leaves_.size()) fail("directory size mismatch");
    for (std::size_t i = 0; i < leaves_.size(); ++i) {
      const Leaf& leaf = *leaves_[i];
      total += leaf.keys.size() + leaf.delta.size();
      if (leaf.keys.size() != leaf.values.size()) fail("base arrays differ");
      if (leaf.keys.size() > config_.max_leaf_keys) fail("leaf over its cap");
      if (leaf.delta.size() >= config_.delta_capacity) fail("delta over its cap");
      if (first_keys_[i] != leaf.model.first_key) fail("stale routing key");
      if (i > 0 && leaf.keys.empty()) fail("empty leaf after the first");
      if (i > 0 && first_keys_[i - 1] >= first_keys_[i]) fail("routing out of order");
      // Walk the merged stream: strictly increasing means sorted and disjoint.
      std::size_t b = 0;
      std::size_t d = 0;
      while (b < leaf.keys.size() || d < leaf.delta.size()) {
        const bool from_base = d == leaf.delta.size() ||
                               (b < leaf.keys.size() && leaf.keys[b] < leaf.delta[d].key);
        const Key key = from_base ? leaf.keys[b] : leaf.delta[d].key;
        if (any && key <= previous) fail("keys not strictly increasing");
        if (i > 0 && key < first_keys_[i]) fail("key below its leaf");
        previous = key;
        any = true;
        b += from_base ? 1 : 0;
        d += from_base ? 0 : 1;
      }
    }
    if (total != size_) fail("size mismatch");
  }

 private:
  struct Leaf {
    SegmentModel model;
    std::vector<Key> keys;
    std::vector<Value> values;
    std::vector<Entry> delta;  // Sorted. Disjoint from keys.
    typename Policy::LeafState state{};
  };

  [[noreturn]] static void fail(const char* message) {
    throw std::logic_error(message);
  }

  void reset_to_one_empty_leaf() {
    // One leaf always exists, so a write always has somewhere to go.
    leaves_.clear();
    first_keys_.assign(1, 0);
    leaves_.push_back(std::make_unique<Leaf>());
    size_ = 0;
  }

  static LeafView view(const Leaf& leaf) {
    return {leaf.keys.size(), leaf.delta.size()};
  }

  static std::uint64_t delta_steps(const Leaf& leaf) {
    return static_cast<std::uint64_t>(std::bit_width(leaf.delta.size()));
  }

  // The last leaf starting at or below `key`. Keys below every routing key go
  // to the first leaf.
  std::size_t route(Key key) {
    if constexpr (kInstrumented) {
      work_.directory_steps += static_cast<std::uint64_t>(std::bit_width(first_keys_.size()));
    }
    const std::size_t after =
        branchless_lower_bound(first_keys_.data(), first_keys_.size(), key);
    if (after < first_keys_.size() && first_keys_[after] == key) return after;
    return after > 0 ? after - 1 : 0;
  }

  std::size_t base_position(const Leaf& leaf, Key key) {
    if constexpr (kInstrumented) {
      work_.window_steps += static_cast<std::uint64_t>(
          std::bit_width(std::uint64_t{leaf.model.over} + leaf.model.under + 2));
    }
    return corrected_lower_bound(leaf.model, leaf.keys.data(), leaf.keys.size(),
                                 key, widened_);
  }

  std::size_t delta_position(const Leaf& leaf, Key key) {
    if constexpr (kInstrumented) work_.delta_steps += delta_steps(leaf);
    const auto it = std::lower_bound(
        leaf.delta.begin(), leaf.delta.end(), key,
        [](const Entry& entry, Key probe) { return entry.key < probe; });
    return static_cast<std::size_t>(it - leaf.delta.begin());
  }

  std::optional<Entry> lower_bound_in_leaf(const Leaf& leaf, Key key) {
    const std::size_t b = base_position(leaf, key);
    const std::size_t d = leaf.delta.empty() ? 0 : delta_position(leaf, key);
    const bool has_base = b < leaf.keys.size();
    const bool has_delta = d < leaf.delta.size();
    if (has_base && (!has_delta || leaf.keys[b] < leaf.delta[d].key)) {
      return Entry{leaf.keys[b], leaf.values[b]};
    }
    if (has_delta) return leaf.delta[d];
    return std::nullopt;
  }

  // Times one read in a few, so the policy learns what a delta probe really
  // costs on this machine, including the cache miss on the buffer.
  std::size_t sampled_delta_position(const Leaf& leaf, Key key) {
    if constexpr (Policy::kSamplesCosts) {
      if (policy_.should_sample()) [[unlikely]] {
        const auto start = std::chrono::steady_clock::now();
        const std::size_t at = delta_position(leaf, key);
        policy_.on_read_sample(elapsed_ns(start), delta_steps(leaf));
        return at;
      }
    }
    return delta_position(leaf, key);
  }

  static std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
  }

  void maybe_rebuild_after_read(std::size_t index) {
    if constexpr (Policy::kChecksOnRead) {
      Leaf& leaf = *leaves_[index];
      if (policy_.should_rebuild_after_read(leaf.state, view(leaf))) rebuild(index);
    }
  }

  // Cuts sorted keys and values into fitted leaves and appends them.
  void append_fitted_leaves(const std::vector<Key>& keys,
                            const std::vector<Value>& values,
                            std::vector<std::unique_ptr<Leaf>>& leaves,
                            std::vector<Key>& first_keys) {
    std::size_t begin = 0;
    while (begin < keys.size()) {
      auto leaf = std::make_unique<Leaf>();
      const std::size_t length =
          fit_prefix(keys.data() + begin, keys.size() - begin, config_.epsilon,
                     config_.max_leaf_keys, leaf->model);
      const auto first = static_cast<std::ptrdiff_t>(begin);
      const auto last = static_cast<std::ptrdiff_t>(begin + length);
      leaf->keys.assign(keys.begin() + first, keys.begin() + last);
      leaf->values.assign(values.begin() + first, values.begin() + last);
      first_keys.push_back(leaf->model.first_key);
      leaves.push_back(std::move(leaf));
      begin += length;
    }
  }

  // Merges one leaf's delta into its base and refits it. Replaces the leaf with
  // however many leaves the refit produced, and updates routing to match.
  void rebuild(std::size_t index) {
    const auto start = std::chrono::steady_clock::now();
    Leaf& old_leaf = *leaves_[index];
    const std::size_t merged_size = old_leaf.keys.size() + old_leaf.delta.size();

    std::vector<Key> keys;
    std::vector<Value> values;
    keys.reserve(merged_size);
    values.reserve(merged_size);
    std::size_t b = 0;
    std::size_t d = 0;
    while (b < old_leaf.keys.size() || d < old_leaf.delta.size()) {
      const bool from_base =
          d == old_leaf.delta.size() ||
          (b < old_leaf.keys.size() && old_leaf.keys[b] < old_leaf.delta[d].key);
      keys.push_back(from_base ? old_leaf.keys[b] : old_leaf.delta[d].key);
      values.push_back(from_base ? old_leaf.values[b] : old_leaf.delta[d].value);
      b += from_base ? 1 : 0;
      d += from_base ? 0 : 1;
    }

    std::vector<std::unique_ptr<Leaf>> fresh;
    std::vector<Key> fresh_first_keys;
    append_fitted_leaves(keys, values, fresh, fresh_first_keys);

    const std::size_t tail = leaves_.size() - index - 1;
    const std::size_t extra = fresh.size() - 1;
    leaves_[index] = std::move(fresh.front());
    first_keys_[index] = fresh_first_keys.front();
    if (extra > 0) {
      const auto after = static_cast<std::ptrdiff_t>(index) + 1;
      leaves_.insert(leaves_.begin() + after,
                     std::make_move_iterator(fresh.begin() + 1),
                     std::make_move_iterator(fresh.end()));
      first_keys_.insert(first_keys_.begin() + after,
                         fresh_first_keys.begin() + 1, fresh_first_keys.end());
      stats_.bytes_moved += tail * (sizeof(Key) + sizeof(void*));
      stats_.splits += extra;
    }

    policy_.reset(leaves_[index]->state);
    const std::uint64_t ns = elapsed_ns(start);
    policy_.on_rebuild(merged_size, ns);
    ++stats_.rebuilds;
    stats_.keys_merged += merged_size;
    stats_.bytes_moved += merged_size * sizeof(Entry);
    stats_.total_ns += ns;
    stats_.worst_ns = std::max(stats_.worst_ns, ns);
  }

  DynamicConfig config_;
  Policy policy_;
  std::vector<Key> first_keys_;  // Routing keys, parallel to leaves_.
  std::vector<std::unique_ptr<Leaf>> leaves_;
  std::size_t size_ = 0;
  RebuildStats stats_;
  WorkCounters work_;
  std::uint64_t widened_ = 0;
};

}  // namespace ali
