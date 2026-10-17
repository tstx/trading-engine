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
- [ ] Stage 1: core matching (LIMIT/MARKET), text parser, CLI — in progress.
- [ ] Stage 2: concurrency (lock-free MPSC input, SPSC event output).
- [ ] Stage 3: cancel/modify, memory pool, benchmarks.
- [ ] Stage 4: load generator, full documentation.

The CLI currently builds but does not process input yet (Stage 1).
