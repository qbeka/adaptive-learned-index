# Experiment plan

Written before any validation result was read. Changes after the freeze go in a
new spec and are stated in the report.

## Question

When does a local rebuild pay for itself? Specifically: does a policy that
estimates the work a buffer will cause, from observed reads, writes and costs,
beat the best single fixed rebuild threshold?

## Data split

| Split | Data | Seeds | Use |
|---|---|---|---|
| Validation | `wiki_ts`, stride 8 | 1, 2, 3 | Tuning only |
| Test, primary | `wiki_ts`, stride 1 | 101 to 108 | Final claims |
| Test, hostile | `osm_cellids_200M`, stride 4 | 101 to 108 | The dataset where a linear model fits worst |
| Test, warm | `wiki_ts`, stride 2048 | 101 to 108 | Data fits in the 16 MiB L2 cache |

Test data shares its source with validation, but test seeds choose different
bulk, pool and absent keys and different operations. Test results are not read
until the configuration below is frozen.

## Tuning, with equal effort

| Policy | Grid | Configurations |
|---|---|---|
| Fixed | threshold in 16, 64, 256, 1024, 2048, 4095, epsilon in 8, 16, 32 | 18 |
| Adaptive | margin in 0, 1, 4, half-life in 2^18, 2^22, epsilon in 8, 16, 32 | 18 |
| B+ tree | 16, 64, 256 leaf slots | 3 |
| ALEX | Default parameters | 1 |

Both learned policies use a leaf cap of 2^14 keys, a delta cap of 4096 keys and
tail sealing, none of which are tuned.

**Selection rule.** For each policy, pick the one configuration with the highest
geometric mean throughput across the four workloads and three seeds. One
configuration per policy, for all workloads. Tuning a fixed threshold per
workload would need the knowledge an online policy is meant to replace.

## Workloads

Defined in `bench/workload.h`: `read_heavy`, `write_heavy`, `append_only`,
`shifting_hotspot`. All include misses, and all run as bursts.

## Hypotheses

| Name | Statement |
|---|---|
| H1, primary | On `shifting_hotspot` on the primary test data, adaptive throughput exceeds fixed. |
| H2 | Across all four workloads, the geometric mean throughput of adaptive exceeds fixed, and adaptive is at least 95 percent of fixed on every workload. |
| H3 | Adaptive moves fewer bytes in rebuilds than fixed, on every workload. |

## Decision rule

The unit of replication is one seed: one trace, replayed by one fresh process per
index. Comparisons are paired by seed.

- H1 holds if the 95 percent bootstrap interval of the paired throughput ratio,
  adaptive over fixed, lies above 1.0.
- H2 holds if the same interval for the per-seed geometric mean lies above 1.0, and
  every workload's interval reaches 0.95 or above.
- H3 is descriptive and uses the median bytes moved.
- Memory must not be worse by more than 5 percent, measured in final bytes per key.
  If it is, a win does not count.

The claim holds only if H1 and H2 both hold. Otherwise the report states a negative
or mixed result.

## Metrics

Construction time, operations per second, read and write p50 and p99, bytes per
key at allocator capacity, rebuild count, keys merged, bytes moved, rebuild time,
worst observed pause, throughput through the distribution shift.

Throughput, latency and memory are separate runs, because per-operation clocks and
allocation tracking disturb timing. Work counters come from a separate
instrumented binary and are never timed.

## Timing limits

`steady_clock` ticks at 24 MHz on this hardware, so single-operation latencies are
quantized to 41.7 ns and include clock overhead. Percentiles are reported as
measured with that stated. Throughput is not affected.

macOS has no public thread affinity API, so runs request the highest
quality-of-service class instead.

## Quiet machine rule

A process starts only when the 1-minute load average is below the spec's
`max_load`. Load before and after each process is stored.

## Amendment, 4 November 2026: gate relaxed

Other jobs on this shared machine held the load average near 170, so the quiet gate
had stopped the experiments at 12 of 468 validation runs. The owner chose to relax
it rather than wait. The limit is now 1000, which in practice removes it. This was
decided before any validation or test result was analysed.

What changes:

- Timings are taken on a heavily loaded machine. The same code has swung by about
  2x under load, so throughput and latency are noisier and may understate every
  index. They are reported as shared-machine measurements, never as peak figures.
- Pairing still helps, because every index in a replicate replays the same trace in
  shuffled order. Noise that drifts during a replicate is not removed, and the
  bootstrap intervals are expected to widen.
- Load before and after every process stays in `raw.jsonl`, and the report states
  its median and range.

What does not change: the hypotheses, the selection rule, the decision rule and
the seeds. Rebuild counts, keys merged, bytes moved, memory and correctness results
do not depend on load for a fixed policy, so those remain as strong as planned. The
adaptive policy's decisions do use measured costs, so its counts can shift with load.

To replace these numbers on a quiet machine, delete the run folders and rerun
`scripts/run_full_benchmark.sh`, which uses each spec's own gate.

## Amendment, 4 November 2026: parallel tuning sweep

Four other projects kept the load average between 60 and 170 when the owner asked
to resume, so the quiet gate stays relaxed. The validation sweep continues from its
saved runs with three processes at a time, because it only picks configurations and
every replicate runs in shuffled order. Final measurements run one process at a
time. No validation result had been analysed when this was decided.

## Freeze, 4 November 2026

The validation sweep finished with 468 of 468 runs and no checksum mismatch. Load
before each run had a median of 153 and a range of 7 to 165, so neighbouring
configurations differ by less than the noise. The selection rule was applied as
written and chose:

| Policy | Configuration | Validation geometric mean |
|---|---|---|
| Fixed | epsilon 16, threshold 64 | 3.00 Mops/s |
| Adaptive | epsilon 8, margin 1, half-life 2^22 | 2.86 Mops/s |
| B+ tree | 64 slots | 2.21 Mops/s |

Adaptive did not win validation, and fixed enters the test as the favourite.

Test runs are frozen in `experiments/update_final.json` (throughput, 8 seeds),
`update_latency.json` and `update_memory.json` (3 seeds, because percentiles and
memory vary little between seeds), and `update_work.json` (instrumented, 3 seeds,
learned policies only). The hypotheses, decision rule and memory limit do not
change. All four use 20M operations and the three data variants above.

The memory and work runs use four parallel jobs, because neither reports timing.
