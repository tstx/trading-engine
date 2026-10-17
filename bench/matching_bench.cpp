#include <benchmark/benchmark.h>

#include "engine/matching_engine.hpp"

// Platzhalter, wird durch die Baseline-Benchmarks ersetzt.
namespace {

class NullSink final : public engine::EventSink {
public:
    void on_ack(engine::OrderId, engine::SeqNo) override {}
    void on_reject(engine::OrderId, engine::RejectReason) override {}
    void on_trade(const engine::Trade&) override {}
    void on_fill(engine::OrderId, engine::Quantity, engine::Quantity) override {}
    void on_cancelled(engine::OrderId, engine::Quantity, engine::CancelReason) override {}
};

void BM_Placeholder(benchmark::State& state) {
    NullSink sink;
    for (auto _ : state) {
        engine::MatchingEngine engine(sink);
        benchmark::DoNotOptimize(&engine);
    }
}
BENCHMARK(BM_Placeholder);

}  // namespace

BENCHMARK_MAIN();
