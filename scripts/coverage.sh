#!/usr/bin/env bash
# Measures line, function and region coverage of include/ali from the test suite.
# Writes results/quality/coverage.json.
set -euo pipefail
cd "$(dirname "$0")/.."
build=build/coverage
mkdir -p "$build" results/quality
flags="-std=c++20 -O0 -g -Iinclude -Itests -w -fprofile-instr-generate -fcoverage-mapping"
objects=()
for test in test_reference test_static test_dynamic; do
  clang++ $flags "tests/$test.cpp" -o "$build/$test"
  LLVM_PROFILE_FILE="$build/$test.profraw" "./$build/$test" > /dev/null
  objects+=("-object" "$build/$test")
done
xcrun llvm-profdata merge -sparse "$build"/*.profraw -o "$build/all.profdata"
xcrun llvm-cov export -summary-only -instr-profile="$build/all.profdata" "${objects[@]}" \
  --ignore-filename-regex='(tests/|/usr/|Xcode)' > "$build/coverage_raw.json"
python3 - "$build/coverage_raw.json" <<'PY'
import json
import sys
from pathlib import Path

data = json.load(open(sys.argv[1]))["data"][0]
files = {}
for entry in data["files"]:
    summary = entry["summary"]
    files[Path(entry["filename"]).name] = {
        kind: {"covered": summary[kind]["covered"], "count": summary[kind]["count"], "percent": round(summary[kind]["percent"], 2)}
        for kind in ("lines", "functions", "regions", "branches")
    }
totals = data["totals"]
result = {
    "lines_percent": round(totals["lines"]["percent"], 2),
    "functions_percent": round(totals["functions"]["percent"], 2),
    "regions_percent": round(totals["regions"]["percent"], 2),
    "branches_percent": round(totals["branches"]["percent"], 2),
    "lines": totals["lines"]["count"],
    "files": files,
}
Path("results/quality/coverage.json").write_text(json.dumps(result, indent=2) + "\n")
print({k: v for k, v in result.items() if k != "files"})
for name, values in files.items():
    print(name, values["lines"]["percent"])
PY
