# Research notes

What was read, what was taken from it, what was done differently, and what failed.

## Environment

| Item | Value |
|---|---|
| Machine | Apple M5 Pro, 18 logical CPUs (6 + 12 cores), 48 GiB RAM, no accelerator used |
| Caches | 128 KiB L1d, 16 MiB L2 per performance cluster, 128-byte cache lines |
| OS and compiler | macOS 26.6, Apple clang 21.0.0, arm64 |
| Build tools | GNU Make 3.81. CMake is not installed and is not needed |
| Python | 3.12 with NumPy 2.5 |
| Disk | 1.2 TiB free. Inputs use about 16 GiB |

Each run manifest records the exact hardware fields again.

## Sources

### Kraska et al., The Case for Learned Index Structures

arXiv 1712.01208, v3, April 2018.

- **Adopted:** an ordered index is a function from key to rank, so a model of that
  function can replace part of the search, and a bounded local search corrects
  the model.
- **Different:** no neural network. A piecewise linear model is cheap to fit and
  has a provable correction window. Prediction overhead is included in every
  timing, because the whole lookup is timed.

### PGM-index

Ferragina and Vinciguerra, PVLDB 13(8), 2020. Implementation pinned at commit
`c6fcf3d`, Apache 2.0.

- **Configuration:** `pgm::PGMIndex<uint64_t, epsilon, 4>`. Epsilon is swept.
  The recursive epsilon stays at PGM's default of 4.
- **Operations compared:** static predecessor search only. Keys and payloads sit
  in parallel arrays beside it, the same layout as `StaticIndex`.
- **Adopted:** epsilon as the single knob for memory against search window.
- **Different:** PGM finds the optimal segmentation with a convex hull and
  recursively indexes its segments. This repository uses the simpler shrinking
  cone anchored at each segment's first key. On real data at epsilon 64, the cone
  produced 22 to 27 percent more segments than PGM.
- **Finding:** PGM rejects `UINT64_MAX` as a key, and `fb_200M_uint64` contains
  that key. The benchmark drops it for every index, and records the count.
- **Build note:** without OpenMP, which Apple clang lacks, PGM builds on one
  thread. Construction times are single-threaded for all indexes.

### ALEX

Ding et al., SIGMOD 2020. Implementation pinned at commit `4370da6`, MIT.

- **How it adapts:** data nodes are gapped arrays, so an insert usually finds a
  free slot near its predicted position. When a node reaches its density limit,
  ALEX compares its empirical cost with the expected cost and then expands the
  node, retrains the model or splits.
- **Adopted:** decide structural changes from measured cost, not a fixed schedule.
- **Different:** ALEX decides when a node runs out of room. The policy here
  decides at any time, from read and write rates, and can merge early when reads
  justify it or late when they do not. Buffers replace gaps, so base arrays stay
  fully packed and a leaf's model never needs to change between rebuilds.
- **Compared through:** `alex::Alex<uint64_t, uint64_t, AlexCompare, allocator, false>`
  at default parameters. It stores keys and payloads in separate arrays. It has
  no insert-or-assign, so the adapter composes `get_payload` and `insert`.
- **Key domain:** ALEX reserves `UINT64_MAX` as a gap marker. Workloads never
  store it.
- **Patch:** ALEX assumes x86 and C++14. `scripts/fetch_baselines.sh` applies
  portability fixes to a copy: 22 changed lines covering `cpuid`, three bit
  intrinsics and two allocator calls removed in C++20. No index logic changes.

### SOSD

Marcus et al., PVLDB 14(1), 2020. Repository pinned at commit `f52f4cb`, GPLv3.

- **Adopted:** the datasets with their published MD5 sums, the file format, and
  the equality lookup workload.
- **Gap:** the upstream harness does not build here. `benchmark.h` includes
  `immintrin.h`, uses `_mm_mfence` and Linux `perf_event`, and its submodules need
  x86. `bench/sosd.h` reimplements the lookup procedure instead: same generator,
  seed 42, draw from unique keys and limit of 100 duplicates per key.
- **Difference from upstream:** this repository stores a map, so duplicate runs
  become one entry whose value is the sum of the run's positions. An exact lookup
  therefore still returns the expected checksum. The payload is the position,
  where SOSD uses a random value.
- **Licence:** SOSD is GPLv3, so no source file is copied. `bench/sosd.h` reimplements the procedure, and its three-line xorshift generator matches SOSD so that seeds give the same lookups.

### tlx B+ tree

Pinned at commit `2dd63ab`, Boost licence. Used as `tlx::btree_map` with
explicit traits. The node size is chosen by the validation sweep from 16, 64 and
256 slots.

## Design decisions

### Fixed-point slopes

A model is a single unsigned 0.64 fixed-point slope applied to `key - first_key`.
Keys are distinct integers, so rank grows by at most one per key unit and the
true slope is at most 1. Rounding the slope moves it by less than 2^-64, offsets
are below 2^64, so rounding moves any prediction by less than one position, on
any data. A double offers no such statement.

Prediction is one 64 by 64 bit multiply. Fitting uses exact fractions compared by
128-bit cross multiplication, so it needs no division per key.

### The window proof

`segment.h` shows that the answer for any key lies in
`[P - over, P + under + 1]`, where `over` and `under` are measured on the stored
keys. The extra 1 covers absent keys and depends on the prediction being
monotone. The result is still verified against its real neighbours, and a failed
check widens the search. The widening path never ran on real models. With
deliberately corrupted bounds it ran 70,560 times with no wrong answer.

### Base writes in place

The brief asks for a precedence rule when a buffered value replaces a base
value. This design removes the case: a write that finds its key in the base
overwrites it in place, so a key is never in both. That has two benefits.
Repeated writes to an old key do not fill the buffer, and a read that hits the
base is finished without touching the delta. Only buffered keys, misses and
ordered operations pay for the buffer.

### The rebuild decision

For a leaf with `n` base keys, `d` buffered keys, and `R` reads and `W` writes in
the recent window, assume the next window looks like the last one.

Without a rebuild the buffer averages `d + W/2` keys over that window. With one it
starts empty and averages `W/2`. A read in a buffer of size `x` costs about
`log2(1 + x)` search steps, and a write shifts about `x/2` entries.

```
read saving  = R * ns_per_step * (log2(1 + d + W/2) - log2(1 + W/2))
write saving = W * ns_per_shifted_entry * d / 2
merge cost   = (n + d) * ns_per_merged_key

rebuild when read saving + write saving > (1 + margin) * merge cost
```

The `W/2` term matters. If writes will refill the buffer immediately, emptying it
buys little, and the formula says so.

Only reads that actually paid for the buffer count towards `R`, because a base
hit does not touch it. Costs come from the machine: merge cost from timing real
rebuilds, step cost from timing one delta read in 256, shift cost from timing
`memmove` once at startup.

**What stops an unusual burst from causing wasteful rebuilds:**

1. A leaf needs `min_samples` operations before any decision.
2. The margin requires savings to clearly exceed the cost.
3. A rebuild clears the leaf's counters, so the next one is earned from zero.
4. Counters halve every `2^half_life_log2` buffered operations, so old activity
   stops counting as recent.

This bounds work, not the number of rebuilds. Total merge cost stays below the
estimated savings, but the count can grow with the burst: the independent review
measured 8 rebuilds from one 4,092-key write burst into small leaves, and 519
from a 400,000-operation burst. A read-only burst gave 1. Rebuilds forced by the
delta cap are outside the bound. An earlier version of this note said one burst
cannot cause repeated rebuilds, which was false as worded.

## Failed experiments and corrections

1. **Reading timings from a loaded machine.** The first profile showed the same
   code at 294 to 571 ns per lookup. Load average was 61 from unrelated jobs.
   Everything timed before the load gate in `run_experiment.py` was discarded.

2. **Assuming routing was the bottleneck.** PGM looked faster and its recursive
   routing was the obvious suspect. The stage profile showed routing at only 6
   to 20 percent of lookup time. The planned routing work was dropped.

3. **Epsilon 64 as a default.** It is the common example value, and in the first
   comparison ALEX was 2 to 3 times faster. At epsilon 64 the window spans about
   1 KiB, several cache misses. Epsilon is now part of the validation sweep.

4. **Sampling real inserts to learn shift cost.** Dividing the time of a short
   insert by the entries moved measured 3.6 ns per entry, where about 0.5 ns is
   plausible, because fixed overhead dominates a short insert. The policy
   overvalued write savings and rebuilt more often than the fixed threshold it
   was meant to beat. Replaced by a direct `memmove` calibration.

5. **The calibration measured zero.** The compiler proved the loop's result
   unused and deleted it, which removed write costs from every decision. Fixed
   with a compiler barrier, and a test now fails if the measurement is implausible.

6. **Sealed leaves kept their buffers.** `leaf.delta = {}` resolves to the
   initializer-list assignment, which keeps capacity. Memory rose by 5 bytes per
   key at every threshold, equal to 16 bytes times every appended key. The index's
   own size report could not show this. The allocator-level counter did.

7. **Truncated error bounds.** The independent review found that an epsilon above
   2^33 could overflow the 32-bit `over` field, so the stored window was too small.
   Answers stayed correct, because the neighbour check widened the search, but the
   claim that widening never happens was false there. Epsilon and segment length
   are now clamped so that both bounds fit.

8. **The routing conclusion was too broad.** Item 2 dropped routing work on a
   profile at larger epsilon. A later diagnostic on `fb` measured routing at 29
   percent at epsilon 8 and 11 percent at epsilon 64, on a machine at load 5 or
   below. The gap to PGM stayed unexplained in part, so routing work was cut too
   early. The diagnostic is `bench/profile_static.cpp`, and it is not a benchmark
   result.

9. **A false mutation score.** The first complete run reported 44 of 44. Four of those
   mutants had passed `test_dynamic` and `test_static`, then "failed" `test_reference`
   only because the sandbox had no `build/` folder to write a file into. The real
   score was 40 of 44. The script now runs the unmodified suite in the sandbox first
   and refuses to score if it fails. Each of the four survivors then got a test.

10. **Selection compared two things at once.** The rule picked epsilon 8 for adaptive
    and epsilon 16 for fixed, so the verdict mixes policy and epsilon. An exploratory
    run at equal epsilon moved `read_heavy` from 0.821 to 0.970, and moved
    `write_heavy` from 1.076 to 0.879. Tuning should have held epsilon fixed across
    the policies.

11. **Tuning on a loaded machine.** The validation sweep ran at median load 153, so
    neighbouring configurations differ by less than the noise, and the chosen
    settings may not be the best. The final runs repeated on a quiet machine, but
    the sweep did not.

12. **No near-never-merge control in the plan.** On full-size data the policy made
    almost no rebuilds, so comparing it with threshold 64 only showed that merging
    did not pay. I first wrote this up as a policy win. Review caught it, and a control
    at threshold 4095 matched or beat adaptive there. The same control on small data
    is where the policy earned its keep, at 1.300 [1.286, 1.313]. The plan should have
    bracketed the policy with both extremes from the start.

13. **"58 GB of copying" was mostly directory shifting.** `bytes_moved` adds merged
    entries and shifted directory slots, and I quoted the total as rebuild copying.
    Merged keys were 3.43 GB of it. The flat directory is the larger cost, and I did not
    see that until review separated the two.

14. **A mutation list has blind spots.** After 44 of 44, a reviewer wrote 27 new mutants
    and 17 survived, including `lower_bound` earning no read and merge cost never
    being learned. Those paths passed every correctness check, because a policy bug
    costs time and never an answer. That is the design working, and it is also why
    answer-level tests could not see them.

## Independent reviews

Both reviews were done by separate AI agents, not by a person.


A separate agent reviewed commit `9d6ba6d` against written acceptance criteria,
working from its own clone.

- **Fuzzing:** 4,000 seeds at `-O2` and 400 under sanitizers, 957k rebuilds,
  749k splits, no result mismatch.
- **Mutations:** 22 of 26 were caught. Two survivors were equivalent. Two exposed
  real test gaps: counter decay was untested, and `bulk_load` rollback was not
  checked on `StaticIndex` or `DynamicIndex`. Both now have tests.
- **Fixed:** the truncation above, a use-after-free when a `scan` visitor calls back
  into the index, now a documented contract rule, undefined behaviour for a
  half-life of 2^64 or more, and the false hysteresis claim above.

A second agent reviewed commit `154823c` from its own clone before publishing. It
recomputed the report's numbers from raw data and found 2 wrong figures, no wrong
answer and no broken invariant. Its main findings were about interpretation: items
12 and 13 above, an overclaimed write win, a memory explanation aimed at the wrong
workload, and selective latency rows. All are corrected in the report.

## Retained optimizations

**Tail sealing.** When every buffered key lies past the base, the buffer becomes
new leaves and the base is not merged. On `append_only` at 6M operations and
epsilon 16:

| Threshold | Keys merged, no seal | Keys merged, seal | Reduction | Bytes per key, no seal and seal |
|---|---:|---:|---:|---|
| 64 | 33,380,383 | 2,988,288 | 11.2x | 17.70, 17.81 |
| 256 | 10,578,410 | 2,988,288 | 3.5x | 17.70, 17.32 |
| 1024 | 4,911,973 | 2,988,032 | 1.6x | 17.70, 17.43 |

These counts are deterministic for a fixed policy, so they do not depend on load.
