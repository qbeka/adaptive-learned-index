// The test oracle: the API contract written with std::map in the most obvious
// way possible. It is deliberately slow and shares no code with the indexes.
#pragma once

#include <iterator>
#include <map>
#include <span>
#include <stdexcept>

#include "ali/types.h"

namespace ali {

class MapOracle {
 public:
  void bulk_load(std::span<const Entry> entries) {
    for (std::size_t i = 1; i < entries.size(); ++i) {
      if (entries[i].key < entries[i - 1].key) {
        throw std::invalid_argument("bulk_load: keys are not sorted");
      }
    }
    map_.clear();
    for (const Entry& entry : entries) map_[entry.key] = entry.value;
  }

  std::optional<Value> lookup(Key key) const {
    const auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    return it->second;
  }

  std::optional<Entry> lower_bound(Key key) const {
    const auto it = map_.lower_bound(key);
    if (it == map_.end()) return std::nullopt;
    return Entry{it->first, it->second};
  }

  bool insert_or_assign(Key key, Value value) {
    return map_.insert_or_assign(key, value).second;
  }

  template <class Visit>
  std::size_t scan(const Range& range, Visit&& visit) const {
    std::size_t visited = 0;
    for (auto it = map_.lower_bound(range.lo);
         it != map_.end() && range.contains(it->first); ++it) {
      visit(Entry{it->first, it->second});
      ++visited;
    }
    return visited;
  }

  std::size_t rank(Key key) const {
    return static_cast<std::size_t>(
        std::distance(map_.begin(), map_.lower_bound(key)));
  }

  std::size_t size() const { return map_.size(); }

 private:
  std::map<Key, Value> map_;
};

}  // namespace ali
