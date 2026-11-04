// Reference implementation of the full API on one sorted array.
//
// This is the correctness baseline and the "optimized sorted-array search"
// competitor: lookups are O(log n) with a branchless search, inserts are O(n).
#pragma once

#include "ali/types.h"

namespace ali {

class SortedVectorIndex {
 public:
  void bulk_load(std::span<const Entry> entries) {
    std::vector<Key> keys;
    std::vector<Value> values;
    split_sorted_entries(entries, keys, values);
    keys_ = std::move(keys);
    values_ = std::move(values);
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

  bool insert_or_assign(Key key, Value value) {
    const std::size_t pos = position(key);
    if (pos < keys_.size() && keys_[pos] == key) {
      values_[pos] = value;
      return false;
    }
    keys_.insert(keys_.begin() + static_cast<std::ptrdiff_t>(pos), key);
    values_.insert(values_.begin() + static_cast<std::ptrdiff_t>(pos), value);
    return true;
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

 private:
  std::size_t position(Key key) const {
    return branchless_lower_bound(keys_.data(), keys_.size(), key);
  }

  std::vector<Key> keys_;
  std::vector<Value> values_;
};

}  // namespace ali
