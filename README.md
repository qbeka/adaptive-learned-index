# adaptive-learned-index

*An in-memory ordered index that predicts where a key is, then measures when a rebuild pays for itself.*

A C++20 library for exact ordered lookups over unsigned 64-bit keys. A small linear model predicts a key's position, and a bounded search corrects the prediction. Writes go to a small buffer beside each segment. A policy decides when merging that buffer is worth the cost, using the reads, writes and costs it observes on the machine it runs on.

The research question: **when does a local rebuild pay for itself?**

## Results

**The main claim failed.** On the primary data, the adaptive policy did not beat the best fixed threshold. The hypotheses and the rule were written down before any test data was read. Full results are in [docs/REPORT.md](docs/REPORT.md).

| Result | Measured | Compared with | Data |
|---|---|---|---|
| Lookup time | 3.1x faster, at 0.021 index bytes per key | Sorted array | 90M Wikipedia timestamps |
| Throughput on data it was not tuned on | 1.54x [1.42, 1.67] | Fixed threshold tuned on other data | OpenStreetMap keys, moving hotspot |
| Rebuild copying under heavy writes | 58 GB removed, throughput 1.08x | Fixed threshold | Wikipedia timestamps |
| Speed against ALEX | 1.2x to 3.4x slower, with 23 to 32 percent less memory | ALEX | Wikipedia timestamps |
| Wrong answers | 0 in 2.03 billion fuzzed operations | Ordered-map oracle | 500,000 random index shapes |

Brackets are 95 percent paired bootstrap intervals over 8 seeds. Machine: Apple M5 Pro, measured 5 November 2026.

| | |
|---|---|
| **What it does** | Bulk load, lookup, lower bound, insert-or-assign, half-open range scan, over `uint64_t` keys with an 8-byte payload. |
| **What it guarantees** | A wrong model costs time and can never change an answer. Every result is verified against its real neighbours. |
| **What it is not** | A production database. It is single-threaded and in-memory, and it has no delete. |
| **What you need** | A Mac with clang and C++20, Make, Git, and Python 3 with NumPy for the analysis. Tested on macOS arm64 only. |
| **Baselines compared** | Sorted array, [PGM-index](https://pgm.di.unipi.it/), [ALEX](https://github.com/microsoft/ALEX), [tlx B+ tree](https://github.com/tlx/tlx), on [SOSD](https://github.com/learnedsystems/SOSD) data. |

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
| Open the demo | Open `site/index.html` in a browser. It works offline. |

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
| `docs/` | Contract, research notes, report, interview walkthrough, checkpoint. |

## Documents

| Document | Read it for |
|---|---|
| [docs/REPORT.md](docs/REPORT.md) | The measured results and what they mean. |
| [docs/API.md](docs/API.md) | The exact behaviour of every operation. |
| [docs/RESEARCH.md](docs/RESEARCH.md) | Sources, derivations, and every failed experiment. |
| [docs/INTERVIEW.md](docs/INTERVIEW.md) | A short walkthrough of the hardest decisions. |
| [docs/CHECKPOINT.md](docs/CHECKPOINT.md) | What passed, what failed, and the next command. |
| [data/README.md](data/README.md) | Where the datasets come from. |

## Credits

The ideas this builds on come from Kraska et al. (learned indexes), Ferragina and Vinciguerra (PGM-index), Ding et al. (ALEX), and Marcus et al. (SOSD). Baselines are fetched at pinned commits and are not redistributed here. Licences: PGM-index Apache 2.0, ALEX MIT, tlx Boost, SOSD GPLv3. No SOSD code is copied. Its file format and lookup procedure are reimplemented.

## License

[MIT](LICENSE)
