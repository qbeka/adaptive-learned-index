// The static learned index: piecewise linear segments over one sorted array.
//
// A lookup finds its segment in the directory, predicts a position, and corrects
// it with a bounded search. All keys and values stay in two contiguous arrays.
#pragma once

#include <vector>

#include "ali/segment.h"
#include "ali/types.h"

namespace ali {

struct StaticConfig {
  std::uint64_t epsilon = 64;  // Fitting tolerance, in positions.
  std::size_t max_segment_keys = std::size_t{1} << 32;
};

// Bytes, split the way docs/RESEARCH.md reports them.
struct MemoryReport {
  std::size_t directory = 0;  // Routing keys.
  std::size_t models = 0;     // Slopes, error bounds, offsets.
  std::size_t keys = 0;       // Stored keys.
  std::size_t payloads = 0;   // Stored values.
  std::size_t deltas = 0;     // Buffered writes, if any.
  std::size_t capacity = 0;   // Everything above as actually reserved.
  std::size_t total_used() const {
    return directory + models + keys + payloads + deltas;
  }
};

class StaticIndex {
 public:
  explicit StaticIndex(StaticConfig config = {}) : config_(config) {}

  void bulk_load(std::span<const Entry> entries) {
    std::vector<Key> keys;
    std::vector<Value> values;
    split_sorted_entries(entries, keys, values);
    keys_ = std::move(keys);
    values_ = std::move(values);
    build_segments();
  }

  std::optional<Value> lookup(Key key) const {
    const std::size_t pos = position(key);
    if (pos == keys_.size() || keys_[pos] != key) return std::nullopt;
    return values_[pos];
  }

  std::optional<Entry> lower_bound(Key key) const {
    const std::size_t pos = position(key);
    if (pos == keys_.size()) return std::nullopt;
    return Entry{keys_[pos], values_[pos]};
  }

  template <class Visit>
  std::size_t scan(const Range& range, Visit&& visit) const {
    std::size_t visited = 0;
    for (std::size_t pos = position(range.lo);
         pos < keys_.size() && range.contains(keys_[pos]); ++pos) {
      visit(Entry{keys_[pos], values_[pos]});
      ++visited;
    }
    return visited;
  }

  std::size_t rank(Key key) const { return position(key); }
  std::size_t size() const { return keys_.size(); }
  std::size_t segment_count() const { return first_keys_.size(); }
  std::uint64_t widened_searches() const { return widened_; }

  MemoryReport memory() const {
    MemoryReport report;
    report.directory = first_keys_.size() * sizeof(Key);
    report.models = segments_.size() * sizeof(SegmentRecord);
    report.keys = keys_.size() * sizeof(Key);
    report.payloads = values_.size() * sizeof(Value);
    report.capacity = first_keys_.capacity() * sizeof(Key) +
                      segments_.capacity() * sizeof(SegmentRecord) +
                      keys_.capacity() * sizeof(Key) +
                      values_.capacity() * sizeof(Value);
    return report;
  }

  // Largest over + under across segments: the widest correction window.
  std::uint64_t max_window() const {
    std::uint64_t widest = 0;
    for (const SegmentRecord& record : segments_) {
      widest = std::max<std::uint64_t>(widest, std::uint64_t{record.over} + record.under);
    }
    return widest;
  }

  // Test hook: makes every error bound wrong, so that tests can show a bad model
  // costs time and never changes an answer.
  void corrupt_error_bounds_for_test() {
    for (SegmentRecord& record : segments_) {
      record.over = 0;
      record.under = 0;
    }
  }

 private:
  struct SegmentRecord {
    std::uint64_t slope;
    std::uint64_t begin;  // Position of the segment's first key.
    std::uint32_t over;
    std::uint32_t under;
  };

  void build_segments() {
    first_keys_.clear();
    segments_.clear();
    std::size_t begin = 0;
    while (begin < keys_.size()) {
      SegmentModel model;
      const std::size_t length =
          fit_prefix(keys_.data() + begin, keys_.size() - begin,
                     config_.epsilon, config_.max_segment_keys, model);
      first_keys_.push_back(model.first_key);
      segments_.push_back({model.slope, begin, model.over, model.under});
      begin += length;
    }
    first_keys_.shrink_to_fit();
    segments_.shrink_to_fit();
  }

  // Global position of the first key >= `key`.
  std::size_t position(Key key) const {
    if (first_keys_.empty()) return 0;
    // The segment that could hold `key` is the last one starting at or below it.
    const std::size_t after =
        branchless_lower_bound(first_keys_.data(), first_keys_.size(), key);
    const std::size_t index =
        after < first_keys_.size() && first_keys_[after] == key ? after
        : after > 0                                             ? after - 1
                                                                : 0;
    const SegmentRecord& record = segments_[index];
    const std::size_t end = index + 1 < segments_.size()
                                ? segments_[index + 1].begin
                                : keys_.size();
    const SegmentModel model{first_keys_[index], record.slope, record.over, record.under};
    return record.begin + corrected_lower_bound(model, keys_.data() + record.begin,
                                                end - record.begin, key, widened_);
  }

  StaticConfig config_;
  std::vector<Key> keys_;
  std::vector<Value> values_;
  std::vector<Key> first_keys_;
  std::vector<SegmentRecord> segments_;
  mutable std::uint64_t widened_ = 0;  // Only touched on the slow path.
};

}  // namespace ali
