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

Since stage 3 every benchmark exists once per book variant, e.g. `BM_MixedFlow<Map>`,
`BM_MixedFlow<Vector>`, `BM_MixedFlow<Tick>`; select a subset with e.g.
`--benchmark_filter='<Tick>'`. One full run takes ~8 minutes.

### Measurement protocol (stage 3 and later)

This is the exact procedure used for the stage-3 numbers below. Run it on a quiet machine
(no parallel builds; the load average is recorded before and after every run) from the
repository root:

```sh
cmake --preset release && cmake --build --preset release
out=bench-results/$(git rev-parse --short HEAD); mkdir -p "$out"
for i in 1 2 3; do
    uptime > "$out/load_$i.txt"
    ./build/release/engine_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
        --benchmark_out="$out/run_$i.json" --benchmark_out_format=json > "$out/run_$i.txt"
    uptime >> "$out/load_$i.txt"
done
cat "$out"/load_*.txt
# median over the 3 runs of each run's median (range = min..max of the 3 run medians)
python3 - "$out"/run_*.json <<'EOF'
import json, statistics, sys, collections
keys = ["items_per_second", "p50_ns", "p90_ns", "p99_ns", "p99.9_ns", "max_ns"]
v = collections.defaultdict(lambda: collections.defaultdict(list))
for path in sys.argv[1:]:
    for b in json.load(open(path))["benchmarks"]:
        if b.get("aggregate_name") == "median":
            for k in keys:
                if k in b:
                    v[b["run_name"]][k].append(b[k])
for name, d in v.items():
    print(name, " ".join(f"{k}={statistics.median(x):.4g}[{min(x):.4g}..{max(x):.4g}]" for k, x in d.items()))
EOF
```

If the load average is above ~2 during a run, repeat that run.

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

## Stage 3 results (PRELIMINARY)

> **Preliminary.** Measured on 2026-10-17 on `2d0674a` (allocation-free hot path merged)
> plus the stage-3 benchmark changes. **3 full runs x 5 repetitions**, median of the per-run
> medians, [min–max of the 3 run medians]; load average 1.0–1.4 during all runs. The final
> measurement will be repeated with the protocol under "How to run". As a control, the
> unchanged **stage-1 binary** (`54d1654`) was re-run **once** (5 repetitions) in the same
> session (load ~1.0): it was within ±10 % of the documented baseline, so the comparison
> below is against both.

### What changed in the benchmarks

- Every baseline benchmark is a template over the three book variants
  (`Map` = `std::map` levels, `Vector` = sorted vector, `Tick` = tick-indexed array). Flows,
  batch sizes and warmups are **unchanged**, so `BM_X/...` (stage 1) and `BM_X<V>/...` are
  directly comparable.
- Each engine gets an `EngineConfig` derived from its flow (`size_config()` in
  `bench/matching_bench.cpp`, by replaying the flow once at generation time):
  `order_capacity` = peak of simultaneously live orders + 25 %, `id_capacity` = number of NEW
  orders over the engine's lifetime, `level_capacity` = max. distinct prices per symbol and
  side, `symbol_capacity` >= symbols. All flow prices lie inside the default tick band
  (max. price ~35 000 in the sweep ladder < 131 072), so `max_tick_price` stays at its default.
  A flow that produces any reject aborts the benchmark.
- New flow `generate_cancel_modify_flow()` (`bench/order_flow.hpp`): per command ~50 % NEW
  (same price/quantity model as the mixed flow, 10 % MARKET), ~35 % CANCEL, ~15 % MODIFY of a
  uniformly chosen resting order (half re-quotes to a new passive price with priority loss,
  half quantity reductions keeping priority). The cancels carry most of the outflow; a
  small proportional controller on the opposite side's order count (`cross_ratio` 0.06)
  keeps the book at ~`target_resting` orders per side. Measured mix: 50.0 / 35.0 / 14.9 %,
  ~0.15 trades per command, 0 rejects, 51/86/102 price levels for 100/1 000/10 000 resting.
  Benchmarks: `BM_CancelModifyFlow<V>/resting:{100,1000,10000}` (throughput, items =
  commands) and `BM_LatencyCancelModify<V>/resting:{1000,10000}`.

### Throughput (orders/s)

| Benchmark | Stage 1 (doc) | Stage 1 (re-run) | Map | Vector | Tick |
|---|---:|---:|---:|---:|---:|
| `PassiveInsert/levels:10` | 10.8 M | 11.1 M | 6.7 M | 6.5 M | 6.5 M |
| `PassiveInsert/levels:100` | 9.5 M | 9.9 M | 6.4 M | 5.8 M | 6.7 M |
| `PassiveInsert/levels:1000` | 6.8 M | 7.3 M | 5.7 M | 5.1 M | 6.6 M |
| `MixedFlow/resting:100` | 9.6 M | 9.7 M | 6.2 M | 7.2 M | **7.5 M** |
| `MixedFlow/resting:1000` | 9.0 M | 8.2 M | 6.1 M | **7.2 M** | 6.7 M |
| `MixedFlow/resting:10000` | 7.5 M | 7.8 M | 5.4 M | **6.4 M** | 6.3 M |
| `MixedFlowSymbols/symbols:1` | 9.5 M | 9.7 M | 6.2 M | **13.7 M** [7.7–16.4] | 8.7 M [7.6–16.0] |
| `MixedFlowSymbols/symbols:10` | 7.3 M | 7.8 M | 6.3 M | **11.5 M** [7.6–13.1] | 8.5 M [6.7–9.8] |
| `MixedFlowSymbols/symbols:100` | 3.6 M | 4.6 M | 4.5 M | **6.2 M** | 5.6 M |
| `AggressiveSweep/levels:1` (sweeps/s) | 4.1 M | 4.5 M | 3.0 M [2.4–4.0] | 3.1 M | 3.2 M |
| `AggressiveSweep/levels:10` | 385 k | 410 k | 320 k | 400 k | 410 k |
| `AggressiveSweep/levels:50` | 72 k | 80 k | 60 k | 80 k | 90 k |
| `CancelModifyFlow/resting:100` (new) | – | – | 13.0 M | 13.6 M | **15.8 M** |
| `CancelModifyFlow/resting:1000` (new) | – | – | 12.4 M | 12.8 M | **14.1 M** |
| `CancelModifyFlow/resting:10000` (new) | – | – | 10.3 M | 10.5 M | **11.7 M** |

### Latency per command (includes ~20 ns clock overhead)

| Benchmark | Variant | p50 | p90 | p99 | p99.9 | max |
|---|---|---:|---:|---:|---:|---:|
| `LatencyMixed/resting:1000` | Stage 1 (doc / re-run) | 140 / 100 ns | 280 / 180 ns | 540 / 300 ns | 1.0 µs / 490 ns | 3.5 / 2.0 ms |
| | Map | 150 ns | 250 ns | 500 ns | 670 ns | 177 µs |
| | Vector | 150 ns | 230 ns | 490 ns | 650 ns | 100 µs |
| | Tick | 140 ns | 220 ns | 480 ns | 630 ns | 139 µs |
| `LatencyMixed/resting:10000` | Stage 1 (doc / re-run) | 150 / 110 ns | 440 / 220 ns | 970 / 450 ns | 2.2 µs / 760 ns | 0.3 ms / 46 µs |
| | Map | 190 ns | 320 ns | 590 ns | 820 ns | 171 µs |
| | Vector | 170 ns | 280 ns | 560 ns | 770 ns | 163 µs |
| | Tick | 170 ns | 280 ns | 550 ns | 760 ns | 127 µs |
| `LatencyPassive/levels:100` | Stage 1 (doc / re-run) | 90 / 90 ns | 110 / 100 ns | 160 / 140 ns | 330 / 210 ns | 6.0 / 3.6 ms |
| | Map | 190 ns | 280 ns | 570 ns | 730 ns | 146 µs |
| | Vector | 200 ns | 290 ns | 580 ns | 740 ns | 174 µs |
| | Tick | 200 ns | 300 ns | 590 ns | 750 ns | 210 µs |
| `LatencyCancelModify/resting:1000` (new) | Map | 90 ns | 160 ns | 400 ns | 570 ns | 160 µs |
| | Vector | 80 ns | 160 ns | 400 ns | 550 ns | 156 µs |
| | Tick | **60 ns** | 140 ns | 380 ns | 530 ns | 123 µs |
| `LatencyCancelModify/resting:10000` (new) | Map | 120 ns | 210 ns | 470 ns | 630 ns | 130 µs |
| | Vector | 120 ns | 200 ns | 470 ns | 630 ns | 157 µs |
| | Tick | 110 ns | 180 ns | 440 ns | 600 ns | 99 µs |

Noise: the coefficient of variation of the repetitions was mostly 2–8 %, but up to 20–28 %
for `MixedFlowSymbols<Tick>`, `CancelModifyFlow<Tick>/resting:100` and the sweeps, and some
benchmarks are bimodal between runs (`MixedFlowSymbols<Vector>/symbols:1`: 7.7 vs 16.4 M).
Differences below ~15 % between variants should not be over-interpreted.

### Findings

1. **The tail is fixed.** `max` dropped from 0.3–6 ms (stage 1) to 100–210 µs for all
   variants, and p99.9 of the 10 000-resting mixed flow from 2.2 µs to ~0.8 µs. A quick A/B
   with the *default* `EngineConfig` (not sized to the flow) on `LatencyPassive<Tick>` gave
   p99 1.85 µs, p99.9 3.4 µs and max 8.4 ms vs. 0.6 µs / 0.8 µs / 0.3 ms with the sized
   config — this confirms the stage-1 hypothesis that the ms outliers come from rehashing
   (`known_ids_` beyond 2^17) and pool growth.
2. **The median and throughput got worse, independent of the book variant.** Throughput is
   20–40 % below stage 1 in `PassiveInsert` and `MixedFlow`, and `LatencyPassive` p50 went from
   90 ns to ~200 ns for *all three* variants. Since all variants are affected equally, the
   cause is not the level structure. Root cause (verified by experiment): **cache/TLB
   locality of `FlatHashMap`/`FlatHashSet`**. `std::hash<uint64_t>` is the identity, so the
   stage-1 `unordered_set`/`unordered_map` touched their buckets *sequentially* for the
   sequential order IDs of all flows. `flat_hash_home()` uses Fibonacci hashing, which
   deliberately scatters consecutive IDs over the whole table — with flow-sized tables
   (`known_ids_` 4 MiB, `live_orders_` 8 MiB for 200k orders) every NEW costs 2–3
   random cache + TLB misses (expensive in a VM). The latency harness serializes around every
   call, so these misses show up fully in p50.
   - Experiment (scratch build, not committed): with an identity hash (`home = key & mask`) the
     same benchmarks reach `PassiveInsert<Tick>` 38–54 M/s, `MixedFlow` 15–25 M/s,
     `CancelModifyFlow` 25–39 M/s and p50 30–70 ns, i.e. **2–5x faster than stage 1**.
   - But identity hashing with linear probing is **not** a fix: in `AggressiveSweep` the
     100 000 sequential IDs form one contiguous cluster and every backward-shift erase scans
     it, so the sweeps collapsed to < 10 k/s (O(n²)).
   - Suggestion for the engine (not part of this change): a home function that keeps
     consecutive IDs close (e.g. scatter only blocks of 8–64 IDs) or a separate structure for
     `known_ids_` (high-water mark + bitmap of recent IDs, as proposed in stage 1).
     Page faults in the freshly `mmap`ed pool are a minor factor (glibc tunables
     `mmap_threshold`/`trim_threshold` changed little).
3. **Variants:** Vector and Tick both beat Map in all matching flows (+10–25 %), Map is
   only competitive in pure passive insertion with few levels. Tick is best where levels are
   created/removed a lot (`CancelModifyFlow` +10–15 % over Vector, `PassiveInsert/1000`
   +30 %, `LatencyCancelModify` p50 60 vs 80 ns) and in the experiment build (where the hash
   noise is gone) it leads clearly in passive insertion. Vector is best with many symbols
   (`symbols:100` 6.2 vs 5.6 M; in the experiment build 14.4 vs 11.6 M): each Tick book holds
   two 1 MiB slot arrays plus bitmaps, which pollutes the cache with 100 books. Vector's
   weakness is deep books (insertion into the middle shifts pointers: `PassiveInsert/1000`
   5.1 M vs Tick 6.6 M).

### Assessment against NFR-7 (preliminary)

| Target | Requirement | Stage 3 (best variant, mixed / cancel-modify flow) | Status |
|---|---|---|---|
| Throughput | > 1 M orders/s | 6.3–7.5 M (mixed), 11.7–15.8 M commands/s (cancel/modify), 5.6–6.2 M with 100 symbols | met, 5–15x headroom |
| Latency p50 | < 1 µs | 60–200 ns | met |
| Latency p99 | < 5 µs | 0.38–0.59 µs | met |

All variants meet NFR-7 by a wide margin, and NFR-6 (no allocation in the hot path, Vector
and Tick) now also holds with a correctly sized `EngineConfig`. Relative to stage 1, stage 3
so far trades median/throughput for a much better tail; the hash locality issue (finding 2)
should be addressed before the final measurement.

### Recommendation for the default (`using OrderBook = ...` in `order_book.hpp`)

**`VectorOrderBook` as default; `TickOrderBook` as opt-in** for instruments with a known,
narrow price band.

- Vector is within ~0–15 % of Tick in every single-symbol flow, best with many symbols,
  allocation-free within `level_capacity`, and has **no semantic restriction**: any positive
  price is accepted, as in stage 1 and the protocol.
- Tick is the fastest variant for level churn and passive insertion, but has real costs as
  a default: (a) **price band** — only prices `1..max_tick_price` (default 131 072 ticks) are
  accepted, anything above is rejected with `INVALID_PRICE`, which is a visible behaviour
  change for clients (e.g. a price of 2 000.00 at tick 0.001 is already out of band); a
  moving band (offset) or per-instrument band configuration would be needed; (b) **memory
  per symbol** — ~2 MiB of slot arrays per book at the default band (zeroed in
  `add_symbol`), 128 MiB for 64 symbols, and the resulting cache pressure shows in
  `MixedFlowSymbols/symbols:100`.
- Map should remain only as a reference: it is the slowest variant and is not NFR-6
  compliant (one node allocation per new price level).

The final decision should be confirmed by the final measurement (after the hash locality
fix, if any), since finding 2 currently masks part of the differences between the variants.
