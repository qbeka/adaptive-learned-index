# API contract

Every index in this repository, and every baseline adapter, follows this
contract. The differential tests check it against an ordered map.

## Types

| Name | Definition | Notes |
|---|---|---|
| `Key` | `uint64_t` | All 2^64 values are legal for our indexes, including 0 and `UINT64_MAX`. |
| `Value` | `uint64_t` | The fixed 8-byte payload. Every compared index stores exactly this. |
| `Entry` | `{Key key; Value value;}` | 16 bytes. |
| `Range` | `{Key lo; std::optional<Key> hi;}` | `hi` is exclusive. No `hi` means through `UINT64_MAX`. |

The index is a map: each key appears at most once.

Scope is single-threaded and in-memory. Concurrency is a separate research
question. There is no delete operation.

## Operations

| Operation | Result |
|---|---|
| `bulk_load(entries)` | Replaces all contents. See the rules below. |
| `lookup(key)` | The value for `key`, or nothing. |
| `lower_bound(key)` | The first entry with `entry.key >= key`, or nothing if all keys are smaller. |
| `insert_or_assign(key, value)` | Stores `value`. Returns `true` if `key` was new, `false` if it replaced a value. |
| `scan(range, visit)` | Calls `visit(entry)` once per entry with `lo <= key < hi`, in ascending key order. Returns the number of entries visited. |
| `rank(key)` | The number of stored keys strictly less than `key`. |
| `size()` | The number of stored keys. |

## Rules

**Bulk load.** Keys must be sorted in non-decreasing order, otherwise
`bulk_load` throws `std::invalid_argument` and the index is left unchanged. Equal
adjacent keys are duplicates: the last one wins, the same result as replaying
the entries through `insert_or_assign`.

**Duplicates.** After bulk load a key has exactly one value. Inserting an
existing key replaces its value and leaves `size()` unchanged.

**Missing keys.** `lookup` returns nothing. `lower_bound` returns the next
larger key. `rank` is defined for every key, stored or not.

**Empty index.** `lookup` and `lower_bound` return nothing. `scan` visits
nothing. `rank` returns 0.

**Empty ranges.** `lo >= hi` is valid and visits nothing. It is not an error.

**Minimum and maximum keys.** 0 and `UINT64_MAX` are ordinary keys. A half-open
range with a bounded `hi` cannot include `UINT64_MAX`, so a `Range` with no `hi`
is the way to reach it. `lower_bound(UINT64_MAX)` returns that key if stored and
nothing otherwise.

**Numeric safety.** No code path may convert a raw key to floating point.
Doubles have 53 bits of mantissa, so adjacent keys near `UINT64_MAX` would become
equal. Models work on integer offsets from a segment's first key.

**Cost of `rank`.** `rank` exists for verification. Our dynamic index answers it
by walking the directory, so it is linear in the number of segments and is not
part of any timed workload.

## Baseline differences

Baselines do not all meet this contract. Adapters must not hide that, so each
difference is listed here and in `docs/RESEARCH.md`.

| Baseline | Difference | How the benchmark handles it |
|---|---|---|
| ALEX | Reserves `UINT64_MAX` as an internal gap marker. | Workloads never store `UINT64_MAX` in compared runs. |
| PGM-index | The static index stores sorted keys only, with no writes. Also reserves `UINT64_MAX` as its final sentinel key. | Compared on static lookups only. Payloads live in a parallel array. |
