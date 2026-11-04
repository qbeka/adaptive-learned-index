// Times each stage of a static lookup separately, to show where the time goes.
// Each stage repeats the work of the one before, so differences are stage costs.
// Usage: profile_static --dataset PATH --epsilon N [--lookups N]
#include <cstdio>
#include <string>

#include "ali/static_index.h"
#include "sosd.h"
#include "timing.h"

using namespace ali;
using namespace ali::bench;

namespace {

template <class Work>
double ns_per_op(const std::vector<Entry>& lookups, Work&& work) {
  std::uint64_t sink = 0;
  for (const Entry& lookup : lookups) sink += work(lookup.key);  // Warm-up.
  const std::uint64_t start = now_ns();
  for (const Entry& lookup : lookups) sink += work(lookup.key);
  const std::uint64_t spent = now_ns() - start;
  std::printf("  (sink %llu)\n", static_cast<unsigned long long>(sink));
  return static_cast<double>(spent) / static_cast<double>(lookups.size());
}

}  // namespace

int main(int argc, char** argv) {
  std::string dataset_path;
  std::uint64_t epsilon = 64;
  std::size_t count = 2'000'000;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--dataset") dataset_path = argv[i + 1];
    else if (flag == "--epsilon") epsilon = std::stoull(argv[i + 1]);
    else if (flag == "--lookups") count = std::stoull(argv[i + 1]);
  }
  prefer_performance_cores();
  const Dataset dataset = make_dataset(load_sosd_keys(dataset_path));
  const std::vector<Entry> lookups = sosd_equality_lookups(dataset, count);
  StaticIndex index({epsilon});
  index.bulk_load(dataset.entries);

  const double scan_only = ns_per_op(lookups, [](Key key) { return key; });
  const double route = ns_per_op(lookups, [&](Key key) { return index.route(key); });
  const double full = ns_per_op(lookups, [&](Key key) { return *index.lookup(key); });
  std::printf("segments=%zu loop=%.1f route=%.1f full=%.1f route_share=%.0f%%\n",
              index.segment_count(), scan_only, route, full,
              100 * route / full);
  return 0;
}
