#!/usr/bin/env bash
# Reruns every final measurement, then rebuilds the tables, charts and site data.
#
# Usage: scripts/run_full_benchmark.sh [MAX_LOAD]
#
# Needs the SOSD datasets (scripts/fetch_sosd.sh) and about 2 hours on an idle
# M5 Pro. It resumes if interrupted. Delete results/<name>/ to measure a run again.
#
# MAX_LOAD overrides each spec's quiet gate. Leave it unset on a quiet machine.
# Memory and work counters do not depend on timing for a fixed policy, so those two
# runs use parallel jobs. results/ARTIFACTS.md lists the gate of each recorded run.
set -euo pipefail
cd "$(dirname "$0")/.."

gate=()
if [[ $# -ge 1 ]]; then gate=(--max-load "$1"); fi

run() {
  python3 scripts/run_experiment.py "experiments/$1.json" "${gate[@]+"${gate[@]}"}" "${@:2}"
}

run update_final
run update_latency
run update_memory --jobs 4
run update_work --jobs 4
run static_final
run static_warm
run update_same_epsilon

python3 scripts/summarize.py
python3 scripts/plot.py tradeoff results/static_final results/plots/static_tradeoff.svg
python3 scripts/plot.py tradeoff results/static_warm results/plots/static_tradeoff_warm.svg
python3 scripts/plot.py shift results/update_final wiki_ts results/plots/shift_primary.svg \
  --labels adaptive fixed alex btree
python3 scripts/plot.py shift results/update_final osm results/plots/shift_hostile.svg \
  --labels adaptive fixed alex btree
python3 scripts/build_site_data.py
python3 scripts/artifact_manifest.py > /dev/null
