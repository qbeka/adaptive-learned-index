// Writes a seeded correctness trace to a file.
// Usage: gen_trace --seed 1 --bulk 1000 --ops 10000 --space uniform --out t.bin
#include <cstdio>
#include <string>

#include "ali/trace.h"

int main(int argc, char** argv) {
  ali::TraceConfig config;
  std::string out;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const std::string value = argv[i + 1];
    if (flag == "--seed") {
      config.seed = std::stoull(value);
    } else if (flag == "--bulk") {
      config.bulk_keys = std::stoull(value);
    } else if (flag == "--ops") {
      config.ops = std::stoull(value);
    } else if (flag == "--space") {
      config.space = ali::key_space_from_string(value);
    } else if (flag == "--out") {
      out = value;
    } else {
      std::fprintf(stderr, "unknown flag %s\n", flag.c_str());
      return 2;
    }
  }
  if (out.empty()) {
    std::fprintf(stderr, "--out is required\n");
    return 2;
  }
  const ali::Trace trace = ali::generate_trace(config);
  ali::write_trace(trace, out);
  std::printf("wrote %s: %zu bulk entries, %zu operations\n", out.c_str(),
              trace.bulk.size(), trace.ops.size());
  return 0;
}
