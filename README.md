# trading-engine

A concurrent order-matching engine in C++20: one order book per instrument, price-time-priority
matching, results reported as a deterministic event stream.

## Build, test, run

Requires Linux, CMake >= 3.20, Ninja and GCC or Clang with C++20 support.
GoogleTest is fetched automatically via `FetchContent`.

```sh
cmake --preset debug             # also: release, asan-ubsan, tsan
cmake --build --preset debug
ctest --preset debug

./build/debug/trading_engine orders.txt     # or read from stdin
```

Sanitizer builds use the same three commands with `--preset asan-ubsan` or `--preset tsan`.

## Example

Input (one message per line, prices as integer ticks):

```
NEW,1,ACME,SELL,LIMIT,10100,50
NEW,2,ACME,SELL,LIMIT,10100,30
NEW,3,ACME,SELL,LIMIT,10200,100
NEW,4,ACME,BUY,LIMIT,10150,70
BOOK,ACME
```

Output:

```
ACK,1,1
ACK,2,2
ACK,3,3
ACK,4,4
TRADE,ACME,10100,50,4,1,BUY
FILL,1,50,0
FILL,4,50,20
TRADE,ACME,10100,20,4,2,BUY
FILL,2,20,10
FILL,4,20,0
BOOK,ACME,BIDS:,ASKS:10100x10;10200x100
```

Order 4 crosses the 101.00 ask level, fills against order 1 first (older), then partially against
order 2. Trades execute at the resting price; the 102.00 level is never touched.

## Status

- [x] Stage 0: scaffolding — CMake presets, GoogleTest, CI (GCC/Clang x debug/ASan+UBSan/TSan),
  clang-format, clang-tidy, interfaces and stubs.
- [x] Stage 1: core matching (LIMIT/MARKET), text parser, CLI, unit and replay tests.
- [x] Stage 2: concurrency — lock-free MPMC input queue, engine and logger threads, SPSC event output,
  `--threaded` CLI mode, multi-producer stress test under TSan.
- [x] Stage 3: CANCEL/MODIFY, allocation-free hot path (object pool, open-addressing hash tables,
  allocation-counting test), three price-level structures (std::map, sorted vector, tick array)
  compared by benchmark; sorted vector is the default.
- [ ] Stage 4: load generator, full documentation.

## Performance

Engine-internal, single-threaded, release build (GCC 15, Ryzen 7 5700X, WSL2). Preliminary numbers;
method, raw results and caveats in [docs/benchmarks.md](docs/benchmarks.md).

| Mixed flow, ~1,000 resting orders/side | Stage 1 (baseline) | Stage 3 (sorted vector) |
|---|---:|---:|
| Throughput | 9.0M orders/s | 7.2M orders/s |
| Latency p50 | 140 ns | 140–150 ns |
| Latency p99 | 0.54 µs | ~0.5 µs |
| Latency max | 2–3.5 ms | 0.1–0.2 ms |
| Heap allocations per order (steady state) | several | 0 |

NFR-7 (>1M orders/s, p50 < 1 µs, p99 < 5 µs) is met by a wide margin. Stage 3 removed the
millisecond outliers and all hot-path allocations; median throughput dropped because the new hash
tables scatter sequential order IDs across memory (cache/TLB misses). A locality-preserving hash is
the next optimisation (see docs/benchmarks.md).

## Design notes

- Order IDs stay reserved forever (duplicate detection, FR-2). Size `EngineConfig::id_capacity` for
  the expected number of orders; beyond it the ID table doubles once, causing a single latency spike.

