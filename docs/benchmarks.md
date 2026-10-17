# Matching core benchmarks

This document records the **baseline** ("before") performance of the matching core as of
stage 1 (`std::map` price levels, one `new`/`unique_ptr` per order, `std::unordered_map` /
`std::unordered_set` indices). Stage 3 optimizations (memory pool, array price levels, ...)
are measured against these numbers with the same benchmarks (`plan.txt` NFR-7, stage 3).

## How to run

```sh
cmake --preset release && cmake --build --preset release
./build/release/engine_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

The benchmark binary also builds in the `debug` and `asan-ubsan` presets (useful as a smoke
test, e.g. `--benchmark_min_time=1x`), but numbers are only meaningful from `release`.

## Methodology

**Order flow** (`bench/order_flow.hpp`). All commands are generated up front into a
`std::vector<engine::Command>` and cached per configuration; the measured loop only iterates
over that vector. Generation uses `std::mt19937_64` with a fixed seed and hand-written
distributions (no `std::*_distribution`, whose output differs between standard libraries), so
the flow is bit-for-bit reproducible. Generators:

| Generator | Content |
|---|---|
| `generate_order_flow(OrderFlowConfig)` | Realistic mix: both sides 50/50, quantities uniform 1–1000, ~10 % MARKET / ~90 % LIMIT. LIMIT prices are geometrically distributed around the current mid (passive: mean 8 ticks behind the mid, marketable: mean 4 ticks through it, capped at ±50 ticks). |
| `generate_passive_flow(n, levels)` | Only non-crossing LIMITs, bids in `[mid-levels, mid-1]`, asks in `[mid+1, mid+levels]`. No matching at all. |
| `generate_ask_ladder(...)` | A ladder of resting asks used to set up sweep benchmarks. |

Stage 1 has no CANCEL, so a flow with a fixed passive share either grows without bound or
drains the book. The mixed generator therefore replays its own output against a shadow
`MatchingEngine` while generating and scales the probability of a marketable LIMIT with the
resting volume on the opposite side (proportional controller, set point `target_resting`
orders per side). Result: a stationary book (~`target_resting` orders per side, ~110–150
price levels) and ~1 trade per order. This is still deterministic and independent of the
measurement. Parameters (count, seed, symbols, market ratio, offsets, target depth, quantity
range, first id) live in `OrderFlowConfig`.

**Harness** (`bench/matching_bench.cpp`). The sink is a `NullSink` whose virtual methods do
nothing; the engine calls it through a reference from another translation unit, so the calls
cannot be elided. One benchmark iteration = one batch: a fresh engine is constructed,
symbols are added and the book is pre-filled under `PauseTiming` (mixed flow: a warmup of
≥ 100 000 orders from the same generator, enough to reach the stationary depth; sweeps: the
ask ladder; passive: empty book); then only the measured batch (200 000 orders for the
passive and mixed benchmarks) is timed while it runs through `engine.process()`. Engine
destruction (freeing all orders) also happens while paused. A fresh engine per batch is
required because order IDs stay "known" forever (DUPLICATE_ID).

**Throughput** is reported as `items_per_second` (orders per CPU second; for sweeps one item
is one sweeping MARKET order). `book_levels` / `book_qty` show the book size at the end of
the last batch.

**Latency** benchmarks time every single `process()` call with `std::chrono::steady_clock`
(vDSO, clocksource `tsc`), with compiler fences around the call, and store the samples in a
pre-reserved vector (5 iterations x 200 000 = 1 000 000 samples). Percentiles (p50, p90,
p99, p99.9, max) are computed over all samples after the loop and exported as counters.
`clock_ns` is the median cost of two back-to-back `now()` calls (20 ns); it is **not**
subtracted. The clock's effective granularity in this VM is ~10 ns, so the percentiles are
quantized to 10 ns. Latency includes the measurement overhead (two `now()` calls plus a
`push_back`), which is why the latency benchmarks report a lower throughput.

**Aggregation.** Each full run uses `--benchmark_repetitions=5`; the table below reports the
**median of the per-run medians over 3 full runs**, with the min–max range of the three
run medians in brackets where it is informative.

## Environment

| | |
|---|---|
| CPU | AMD Ryzen 7 5700X (Zen 3), 8 cores / 16 threads, 3.4 GHz nominal, L2 512 KiB/core, L3 32 MiB |
| VM | WSL2 (Hyper-V, full virtualization), 16 vCPUs, 15 GiB RAM |
| Kernel | Linux 6.18.33.2-microsoft-standard-WSL2 |
| OS | Ubuntu 26.04 LTS |
| Compiler | GCC 15.2.0 (`/usr/bin/c++`), libstdc++, glibc malloc |
| Flags | `-O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -Werror` (CMake `Release`, no `-march`, no LTO) |
| Build | CMake 4.2.3, Ninja 1.13.2, Google Benchmark 1.9.1 |
| Clock | clocksource `tsc`, `steady_clock::now()` pair ≈ 20 ns |

**Noise.** This is a VM on a desktop host with CPU frequency scaling and no core isolation;
the benchmark was not pinned. A first exploratory run while other builds/tests were running
(load average ~6) measured `BM_PassiveInsert` at 5.3–6.0 M orders/s, i.e. **~40 % slower**
than the quiet runs below (load average 1–2). Within the three quiet runs the coefficient of
variation of the repetitions was mostly 1–9 % (up to 15 % for the 50-level sweep). The `max`
latency values (hundreds of µs to several ms) are dominated by single outliers — hypervisor
preemption, page faults while the book grows, and hash-table rehashes (see below) — and
should not be read as a property of the matching algorithm.

## Baseline results (stage 1)

Date: 2026-10-17, commit "Stufe 2: Gerüst für Pipeline, Queues und Benchmarks" (`e0b775a`)
plus the benchmark files. Median of 3 runs x 5 repetitions.

### Throughput

| Benchmark | Scenario | Orders/s | Notes |
|---|---|---:|---|
| `BM_PassiveInsert/levels:10` | 200k passive limits, 20 levels | **10.8 M** [8.9–12.0 M] | book grows 0 → 200k orders |
| `BM_PassiveInsert/levels:100` | 200k passive limits, 200 levels | **9.5 M** [8.3–10.4 M] | |
| `BM_PassiveInsert/levels:1000` | 200k passive limits, 2000 levels | **6.8 M** [6.2–7.6 M] | deeper `std::map` |
| `BM_MixedFlow/resting:100` | mixed flow, ~100 resting/side | **9.6 M** [9.4–10.0 M] | ~1 trade/order |
| `BM_MixedFlow/resting:1000` | mixed flow, ~1 000 resting/side | **9.0 M** [8.6–9.5 M] | |
| `BM_MixedFlow/resting:10000` | mixed flow, ~10 000 resting/side | **7.5 M** [5.3–7.9 M] | |
| `BM_MixedFlowSymbols/symbols:1` | mixed, 1 symbol, ~200 resting/side | **9.5 M** [8.8–9.6 M] | |
| `BM_MixedFlowSymbols/symbols:10` | mixed, 10 symbols | **7.3 M** [6.5–7.6 M] | |
| `BM_MixedFlowSymbols/symbols:100` | mixed, 100 symbols | **3.6 M** [2.8–3.9 M] | ~7.7k levels, 40k resting orders in total |
| `BM_AggressiveSweep/levels:1` | MARKET buy clears 1 level (4 orders) | **4.1 M** sweeps/s | ≈ 16.6 M trades/s |
| `BM_AggressiveSweep/levels:10` | MARKET buy clears 10 levels (40 orders) | **385 k** sweeps/s | ≈ 15.4 M trades/s |
| `BM_AggressiveSweep/levels:50` | MARKET buy clears 50 levels (200 orders) | **72 k** sweeps/s | ≈ 14.4 M trades/s |

### Latency per order (engine-internal, includes ~20 ns clock overhead)

| Benchmark | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|
| `BM_LatencyMixed/resting:1000` | **140 ns** [100–140] | 280 ns | **540 ns** [340–550] | 1.0 µs [0.6–1.3] | 3.5 ms |
| `BM_LatencyMixed/resting:10000` | **150 ns** [130–190] | 440 ns | **970 ns** [850–1390] | 2.2 µs [1.7–3.0] | 0.3 ms |
| `BM_LatencyPassive/levels:100` | **90 ns** [90–130] | 110 ns | **160 ns** [160–230] | 330 ns | 6.0 ms |

## Assessment against NFR-7

| Target | Requirement | Baseline (mixed flow, 1 symbol) | Status |
|---|---|---|---|
| Throughput, single-threaded | > 1 M orders/s | 7.5–9.6 M orders/s (3.6 M with 100 symbols) | met, ~4–10x headroom |
| Latency p50 | < 1 µs | 140–150 ns | met |
| Latency p99 | < 5 µs | 0.5–1.0 µs | met |

The stage-1 implementation already meets the NFR-7 orientation targets on this machine by a
wide margin. Stage 3 is therefore primarily about **NFR-6** (no dynamic allocation in the hot
path) and about the **tail**: p99.9 is already 1–3 µs and the max is in the millisecond range,
which is where allocation, rehashing and cache misses show up. Caveats: the numbers are for a
fast desktop CPU with a warm cache, a `NullSink` (no event formatting/I/O) and a single
thread; the pipeline (stage 2) and a real sink add their own costs on top.

## Where the time goes (hypotheses for stage 3)

`perf` is not available in this WSL2 image, so a coarse profile was taken with `gprof` on a
separate, statically linked `-O3 -pg -fno-inline-functions-called-once` build (not part of the
repository). Caveat: the gprof histogram also covers the untimed warmup and engine
destruction, so it over-weights insertion and `free`. Flat profile, `BM_MixedFlow/resting:1000`:

| Share | Symbol |
|---:|---|
| 30 % | `MatchingEngine::handle(NewOrder)` (inlined: `live_orders_` lookups/erase, matching loop, cache misses on resting orders) |
| ~23 % | glibc `malloc`/`free`/`operator new` incl. `_int_free_merge_chunk`, `systrim` |
| 9.5 % | `unordered_set<OrderId>::insert` (`known_ids_`) |
| 6.4 % | `OrderBook::fill` (`std::map::find`) |
| 6.3 % | `OrderBook::add` (`std::map::try_emplace`) |
| 3.6 % | `OrderBook::remove` (`std::map::find` + erase) |
| ~4.5 % | `_Rb_tree_insert_and_rebalance` / `_Rb_tree_rebalance_for_erase` |

In `BM_PassiveInsert` the two hash tables (`known_ids_` and `live_orders_`) together account
for ~26 % and `OrderBook::add` for ~18 %.

Concrete hypotheses, in order of expected impact:

1. **Heap allocation per order (NFR-6).** Every accepted order costs 2–3 allocations:
   `make_unique<Order>`, a node in `live_orders_` (if it rests) and a node in `known_ids_`;
   new price levels add a `std::map` node. Every fill frees the order and the
   `live_orders_` node. malloc/free is ~20–25 % of samples. A free-list/pool of `Order`
   objects (plan stage 3) and node-free index structures should remove most of this and
   also the `systrim` calls (glibc returning memory to the OS when the book shrinks).
2. **`known_ids_` grows without bound.** Every accepted ID is inserted into an
   `unordered_set` forever (~10 % of the mixed flow). Besides the node allocation, the set
   periodically rehashes all entries in one `process()` call — with hundreds of thousands
   of IDs this is a plausible source of the millisecond `max` latencies. Options: a
   pre-sized open-addressing table, or (if IDs are monotonic per client) a high-water mark
   plus a bitmap of recent IDs.
3. **Redundant price-level lookups in the match loop.** `OrderBook::fill()` and
   `OrderBook::remove()` each do a `std::map::find(order->price)` although the matching loop
   already works on the best level (`begin()`). That is two O(log L) pointer-chasing searches
   per trade. Storing a `PriceLevel*` in `Order` (or passing the iterator) removes them;
   `BM_AggressiveSweep` (~15 M trades/s, i.e. ~65 ns per trade) should profit most.
4. **`std::map` as level container.** Node-based, one allocation per new level, poor
   locality; cost visibly grows with the number of levels (`PassiveInsert` 10.8 M → 6.8 M
   orders/s from 20 to 2000 levels). A tick-indexed array (or a small sorted vector for the
   few levels near the touch) over the active price band would make add/best/remove O(1)
   with contiguous memory.
5. **Per-symbol and per-order indirections / cache footprint.** `books_` is an
   `unordered_map<SymbolId, unique_ptr<OrderBook>>` and `live_orders_` maps to
   `unique_ptr<Order>` — two dependent loads before any real work. The drop from 9.5 M
   (1 symbol) to 3.6 M orders/s (100 symbols) is consistent with the working set of
   scattered nodes outgrowing L1/L2. Dense `SymbolId` → vector index and intrusive/pooled
   orders should reduce misses. (The three virtual `EventSink` calls per trade are cheap
   with a `NullSink` and are not a hotspot here.)
