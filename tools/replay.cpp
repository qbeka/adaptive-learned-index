// Replays a trace file against an index and the oracle, and prints the digest.
// Usage: replay --trace t.bin [--index sorted_vector|fixed|adaptive]
#include <cinttypes>
#include <cstdio>
#include <string>

#include "ali/dynamic_index.h"
#include "ali/map_oracle.h"
#include "ali/replay.h"
#include "ali/sorted_vector_index.h"

namespace {

template <class Index>
int run(const std::string& name, const ali::Trace& trace) {
  Index index;
  ali::MapOracle oracle;
  const auto divergence = ali::first_divergence(index, oracle, trace);
  if (divergence) {
    std::printf(
        "%s: DIVERGED at op %zu kind=%" PRIu64 " key=%" PRIu64
        " got=(%" PRIu64 ",%" PRIu64 ",%" PRIu64 ") want=(%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ")\n",
        name.c_str(), divergence->op_index,
        static_cast<std::uint64_t>(divergence->op.kind), divergence->op.key,
        divergence->subject.present, divergence->subject.first,
        divergence->subject.second, divergence->oracle.present,
        divergence->oracle.first, divergence->oracle.second);
    return 1;
  }
  Index fresh;
  const std::uint64_t digest = ali::replay_digest(fresh, trace);
  std::printf("%s: %zu operations matched the oracle, digest %016" PRIx64 "\n",
              name.c_str(), trace.ops.size(), digest);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string trace_path;
  std::string index = "sorted_vector";
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--trace") {
      trace_path = argv[i + 1];
    } else if (flag == "--index") {
      index = argv[i + 1];
    } else {
      std::fprintf(stderr, "unknown flag %s\n", flag.c_str());
      return 2;
    }
  }
  if (trace_path.empty()) {
    std::fprintf(stderr, "--trace is required\n");
    return 2;
  }
  const ali::Trace trace = ali::read_trace(trace_path);
  if (index == "sorted_vector") return run<ali::SortedVectorIndex>(index, trace);
  if (index == "fixed") return run<ali::DynamicIndex<ali::FixedPolicy>>(index, trace);
  if (index == "adaptive") return run<ali::DynamicIndex<ali::AdaptivePolicy>>(index, trace);
  std::fprintf(stderr, "unknown index %s\n", index.c_str());
  return 2;
}
