#!/usr/bin/env bash
# One-command check that the whole repository works on this machine. It needs
# no download beyond the pinned baselines and takes about a minute.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== correctness suite"
make --no-print-directory test

echo "== replay a saved trace against the oracle"
mkdir -p data/smoke
./build/release/gen_keys --shape bursts --keys 500000 --seed 1 --out data/smoke/bursts_500K
./build/release/gen_trace --seed 1 --bulk 20000 --ops 200000 --space near_max --out data/smoke/trace.bin
for index in sorted_vector fixed adaptive; do
  ./build/release/replay --trace data/smoke/trace.bin --index "$index"
done

echo "== baselines"
if [ ! -d third_party/ALEX-arm64 ]; then
  ./scripts/fetch_baselines.sh
fi
make --no-print-directory bench

echo "== static lookups (ns per lookup, lower is better)"
for spec in "binary 0" "ali 64" "pgm 64"; do
  set -- $spec
  ./build/release/static_bench --dataset data/smoke/bursts_500K --index "$1" \
    --epsilon "$2" --lookups 200000 --latency 10000 --passes 2 |
    python3 scripts/print_run.py
done

echo "== update workload (all indexes must print the same checksum)"
for index in btree64 alex fixed adaptive; do
  ./build/release/update_bench --dataset data/smoke/bursts_500K --workload shifting_hotspot \
    --index "$index" --ops 500000 --epsilon 16 |
    python3 scripts/print_run.py
done

echo "smoke run passed"
