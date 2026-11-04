// Rebuild policies. A policy sees what happens to one leaf and decides when
// merging that leaf's delta buffer is worth it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ali {

// What a policy may inspect about a leaf when it decides.
struct LeafView {
  std::size_t base_keys = 0;
  std::size_t delta_keys = 0;
};

// Rebuilds a leaf as soon as its delta buffer holds `threshold` keys. It does
// not look at reads at all, which makes it the reproducible baseline.
class FixedPolicy {
 public:
  struct LeafState {};

  explicit FixedPolicy(std::size_t threshold = 256) : threshold_(threshold) {}

  void on_delta_read(LeafState&, std::uint64_t) {}
  void on_write(LeafState&) {}
  void on_rebuild(std::uint64_t, std::uint64_t) {}
  void reset(LeafState&) {}

  bool should_rebuild_after_write(LeafState&, const LeafView& leaf) {
    return leaf.delta_keys >= threshold_;
  }
  static constexpr bool kChecksOnRead = false;
  bool should_rebuild_after_read(LeafState&, const LeafView&) { return false; }

  std::size_t threshold() const { return threshold_; }

 private:
  std::size_t threshold_;
};

}  // namespace ali
