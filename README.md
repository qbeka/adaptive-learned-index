# adaptive-learned-index

*An in-memory ordered index that predicts where a key is, then measures when a rebuild pays for itself.*

[Live replay](https://www.qendrimbeka.com/projects/adaptive-learned-index/index.html) | [Report](docs/REPORT.md)

A C++20 library for exact ordered lookups over unsigned 64-bit keys. A small linear model predicts a key's position, and a bounded search corrects the prediction. Writes go to a small buffer beside each segment. A policy decides when merging that buffer is worth the cost, using the reads, writes and costs it observes on the machine it runs on.

The research question: **when does a local rebuild pay for itself?**

## Results

**The main claim failed.** On the primary data, the adaptive policy did not beat the best fixed threshold. I wrote the update hypotheses and the rule down before reading any test data. Full results are in [docs/REPORT.md](docs/REPORT.md).

| Result | Measured | Compared with | Data |
|---|---|---|---|
| Adaptive throughput, the preregistered test | 0.948 [0.907, 0.994], so the claim fails | Best fixed threshold | 43M Wikipedia timestamps, four workloads |
| Adaptive on small data, exploratory | 1.30x [1.29, 1.31] | Threshold that almost never merges | 46,000 keys, four workloads |
| Adaptive on full-size data, exploratory | 0.96x to 0.98x | Threshold that almost never merges | Wikipedia and OpenStreetMap keys |
| Lookups, best case | 3.1x faster, at 0.021 index bytes per key | Sorted array | 90M Wikipedia timestamps |
| Speed against ALEX | 1.2x to 3.4x slower, with 23 to 32 percent less memory | ALEX | Wikipedia timestamps |
| Speed against PGM | 1.1x to 2.8x slower, about 2.5x larger | PGM-index | Full SOSD datasets |
| Wrong answers | 0 in 2.03 billion fuzzed operations | Ordered-map oracle | 500,000 random index shapes |

So the short answer: on full-size data over 20M operations, rebuilds barely pay and the policy correctly does almost nothing. On small data they pay, and it finds them. It does this without retuning, but it does not beat the best fixed setting for each case by a margin that survives noise on the primary data.

Brackets are 95 percent paired bootstrap intervals over 8 seeds. Machine: Apple M5 Pro.

## Quickstart

Run everything end to end. This takes about a minute and downloads only the pinned baselines.

```bash
make smoke
```

It runs the correctness suite, replays a trace against the oracle, and compares every index on a small synthetic dataset. It ends with `smoke run passed`.

## Commands

| Goal | Command |
|---|---|
| Correctness suite | `make test` |
| Same suite under ASan, then UBSan | `make sanitize` |
| Fetch and pin the baselines | `scripts/fetch_baselines.sh` |
| Download a SOSD dataset | `scripts/fetch_sosd.sh wiki_ts_200M_uint64` |
| Run a benchmark experiment | `scripts/run_experiment.py experiments/NAME.json` |
| Summarise a run | `scripts/analyze.py results/NAME --reference LABEL` |
| Rerun all final benchmarks | `scripts/run_full_benchmark.sh` |
| Open the demo | Open `site/index.html` in a browser, or see the [live replay](https://www.qendrimbeka.com/projects/adaptive-learned-index/index.html). |
| Build the demo as one file | `scripts/bundle_site.py` |

Experiments resume if interrupted, and they wait for the machine to go quiet before each measurement. [results/ARTIFACTS.md](results/ARTIFACTS.md) lists every recorded run.

## Use it

The library is header-only. Add `include/` to your include path.

```cpp
#include "ali/dynamic_index.h"

ali::DynamicIndex<ali::AdaptivePolicy> index;

std::vector<ali::Entry> sorted = {{10, 100}, {20, 200}, {30, 300}};
index.bulk_load(sorted);

index.insert_or_assign(25, 250);

std::optional<ali::Value> value = index.lookup(25);        // 250
std::optional<ali::Entry> next = index.lower_bound(21);    // {25, 250}

index.scan({10, 30}, [](const ali::Entry& entry) {
  // Visits keys 10, 20, 25 in order. 30 is excluded.
});
```

[docs/API.md](docs/API.md) defines every operation, including duplicates, missing keys, empty ranges, and 0 and `UINT64_MAX`.

## How a lookup works

```
key
 |
 v
Directory         binary search over each leaf's first key
 |
 v
Linear model      position = (key - first_key) * slope
 |                one integer multiply, no floating point
 v
Bounded search    within the error measured when the leaf was built
 |
 v
Verify            compare with the real neighbours, widen if the check fails
 |
 +--- found ------> value
 |
 v
Delta buffer      only reached if the key is not in the base
```

## How a write works

```
insert_or_assign(key, value)
 |
 +--- key in base ----> overwrite in place, done
 |
 v
Sorted delta buffer of that leaf
 |
 v
Policy: expected saving > (1 + margin) * merge cost ?
 |
 +--- no -------------> done
 |
 v
Rebuild: merge base and delta, refit, split if needed, update routing
```

A key is in the base or the buffer, never both. So a read that hits the base never pays for recent inserts, and only reads that actually touch the buffer count towards a rebuild. [docs/RESEARCH.md](docs/RESEARCH.md) derives the decision.

## Repository layout

| Path | Contents |
|---|---|
| `include/ali/` | The library. `segment.h` holds the model and the correctness proof, `policy.h` holds the rebuild decision. |
| `tests/` | Correctness suite: contract checks, seeded differential replay, invariants. |
| `tools/` | Trace and key generators, and a trace replayer. |
| `bench/` | Benchmarks, baseline adapters, workloads, demo recorder. |
| `experiments/` | Frozen experiment specs and the plan. |
| `results/` | Raw measurements, manifests, tables and charts for each run. |
| `site/` | Offline demo generated from a recorded run. |
| `docs/` | Contract, research notes, report, checkpoint. |

## Documents

| Document | Read it for |
|---|---|
| [docs/REPORT.md](docs/REPORT.md) | The measured results and what they mean. |
| [docs/API.md](docs/API.md) | The exact behaviour of every operation. |
| [docs/RESEARCH.md](docs/RESEARCH.md) | Sources, derivations, and every failed experiment. |
| [data/README.md](data/README.md) | Where the datasets come from. |

## Credits

The ideas this builds on come from Kraska et al. (learned indexes), Ferragina and Vinciguerra (PGM-index), Ding et al. (ALEX), and Marcus et al. (SOSD). Baselines are fetched at pinned commits and are not redistributed here. Licences: PGM-index Apache 2.0, ALEX MIT, tlx Boost, SOSD GPLv3. No SOSD source file is copied. Its file format and lookup procedure are reimplemented, including the three-line xorshift generator needed to reproduce its seed.

## License

[MIT](LICENSE), except `site/fonts/`. Geist and Geist Serif are under the SIL Open Font License, included as `site/fonts/LICENSE-GEIST.txt` and `site/fonts/GeistSerif-OFL.txt`.
