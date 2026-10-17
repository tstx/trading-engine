// Benchmarks des Matching-Kerns (MatchingEngine::process), single-threaded.
//
// Alle Orderflüsse werden vorab erzeugt (order_flow.hpp) und je Konfiguration
// gecacht. Eine Benchmark-Iteration verarbeitet einen kompletten Batch mit
// einer frischen Engine; Aufbau, Warmup (Vorbefüllen des Buchs) und Abbau der
// Engine laufen unter PauseTiming und zählen nicht mit. Methodik und
// Ergebnisse: docs/benchmarks.md.
//
// Stufe 3: Jeder Benchmark läuft als Template über die drei Buch-Varianten
// (BM_X<Map>, BM_X<Vector>, BM_X<Tick>). Die Flüsse der Baseline (Stufe 1) sind
// unverändert. Jede Engine bekommt eine zum Fluss passende EngineConfig (siehe
// size_config), damit weder Pool-Wachstum noch Rehashes in der Messung liegen.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <benchmark/benchmark.h>

#include "engine/matching_engine.hpp"
#include "order_flow.hpp"

namespace {

using engine::Command;

// Buch-Varianten (kurze Namen für die Benchmark-Namen BM_X<Map> usw.).
using Map = engine::MapMatchingEngine;
using Vector = engine::VectorMatchingEngine;
using Tick = engine::TickMatchingEngine;

// Verwirft alle Events. Die Engine ruft den Sink virtuell über eine Referenz aus
// einer anderen Übersetzungseinheit auf, die Aufrufe können also nicht
// wegoptimiert werden; DoNotOptimize auf die Engine sichert zusätzlich ab.
class NullSink final : public engine::EventSink {
public:
    void on_ack(engine::OrderId, engine::SeqNo) override {}
    void on_reject(engine::OrderId, engine::RejectReason) override {}
    void on_trade(const engine::Trade&) override {}
    void on_fill(engine::OrderId, engine::Quantity, engine::Quantity) override {}
    void on_cancelled(engine::OrderId, engine::Quantity, engine::CancelReason) override {}
};

// Vorab erzeugter Fluss: warmup füllt das Buch (ungemessen), measured wird gemessen.
// config wird von size_config() aus dem Fluss abgeleitet.
struct Flow {
    std::uint32_t symbols = 1;
    std::vector<Command> warmup;
    std::vector<Command> measured;
    engine::EngineConfig config;
};

// Zählt beim Abspielen eines Flusses, wie viele Orders gleichzeitig leben (inkl. des gerade
// matchenden Aggressors, der ebenfalls aus dem Pool kommt), und die Rejects.
class CapacitySink final : public engine::EventSink {
public:
    void on_ack(engine::OrderId id, engine::SeqNo) override {
        live_.insert(id);  // MODIFY-ACKs: ID ist schon enthalten
        peak_ = std::max(peak_, live_.size());
    }
    void on_reject(engine::OrderId, engine::RejectReason) override { ++rejects_; }
    void on_trade(const engine::Trade&) override {}
    void on_fill(engine::OrderId id, engine::Quantity, engine::Quantity remaining) override {
        if (remaining == 0) {
            live_.erase(id);
        }
    }
    void on_cancelled(engine::OrderId id, engine::Quantity, engine::CancelReason) override { live_.erase(id); }

    std::unordered_set<engine::OrderId> live_;
    std::size_t peak_ = 0;
    std::size_t rejects_ = 0;
};

// Leitet die EngineConfig aus dem Fluss ab (NFR-6), durch einmaliges Abspielen von
// warmup + measured gegen eine Map-Engine (akzeptiert jeden Preis):
// - order_capacity >= Spitzenzahl gleichzeitig lebender Orders (+25 % Reserve),
// - id_capacity    >= Anzahl NEW-Orders über die gesamte Lebensdauer der Engine,
// - level_capacity >= größte Zahl verschiedener Preise je Symbol und Seite (obere Schranke
//                     für gleichzeitig belegte Level),
// - symbol_capacity >= Anzahl Symbole,
// - max_tick_price: Default, solange alle Preise im Band liegen, sonst angehoben.
// Ein Fluss, der Rejects erzeugt, ist ein Fehler im Benchmark (Abbruch).
void size_config(Flow& flow) {
    CapacitySink sink;
    engine::MapMatchingEngine ref(sink);
    for (engine::SymbolId s = 0; s < flow.symbols; ++s) {
        ref.add_symbol(s);
    }

    std::size_t new_orders = 0;
    engine::Price max_price = 0;
    std::unordered_map<engine::OrderId, std::pair<engine::SymbolId, engine::Side>> owner;
    std::vector<std::array<std::set<engine::Price>, 2>> prices(flow.symbols);
    const auto note_price = [&](engine::SymbolId symbol, engine::Side side, engine::Price price) {
        prices[symbol][static_cast<std::size_t>(side)].insert(price);
        max_price = std::max(max_price, price);
    };

    for (const std::vector<Command>* part : {&flow.warmup, &flow.measured}) {
        for (const Command& cmd : *part) {
            if (const auto* n = std::get_if<engine::NewOrder>(&cmd)) {
                ++new_orders;
                owner.emplace(n->id, std::pair{n->symbol, n->side});
                if (n->type == engine::OrderType::Limit) {
                    note_price(n->symbol, n->side, n->price);
                }
            } else if (const auto* m = std::get_if<engine::ModifyOrder>(&cmd)) {
                const auto& [symbol, side] = owner.at(m->id);
                note_price(symbol, side, m->new_price);
            }
            ref.process(cmd);
        }
    }
    if (sink.rejects_ != 0) {
        std::fprintf(stderr, "benchmark flow produces %zu rejects\n", sink.rejects_);
        std::abort();
    }

    std::size_t levels = 0;
    for (const auto& sides : prices) {
        levels = std::max({levels, sides[0].size(), sides[1].size()});
    }

    engine::EngineConfig& c = flow.config;
    c.order_capacity = sink.peak_ + sink.peak_ / 4 + 64;
    c.id_capacity = new_orders;
    c.level_capacity = std::max<std::size_t>(levels, 16);
    c.symbol_capacity = std::max<std::size_t>(c.symbol_capacity, flow.symbols);
    c.max_tick_price = std::max(c.max_tick_price, max_price);
}

constexpr std::size_t kBatch = 200'000;  // gemessene Orders je Iteration (Durchsatz)

// ---------------------------------------------------------------------------
// Flussfabriken (gecacht, damit Wiederholungen nicht neu generieren)

const Flow& mixed_flow(std::size_t target_resting, std::uint32_t symbols) {
    static std::map<std::tuple<std::size_t, std::uint32_t>, Flow> cache;
    auto [it, inserted] = cache.try_emplace({target_resting, symbols});
    if (inserted) {
        bench::OrderFlowConfig cfg;
        cfg.symbols = symbols;
        cfg.target_resting = target_resting;
        // Warmup: genug Orders, damit jedes Buch seine stationäre Tiefe erreicht.
        const std::size_t warmup = std::max<std::size_t>(100'000, 50 * target_resting * symbols);
        cfg.count = warmup + kBatch;
        std::vector<Command> all = bench::generate_order_flow(cfg);
        it->second.symbols = symbols;
        it->second.warmup.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(warmup));
        it->second.measured.assign(all.begin() + static_cast<std::ptrdiff_t>(warmup), all.end());
        size_config(it->second);
    }
    return it->second;
}

const Flow& passive_flow(engine::Price levels) {
    static std::map<engine::Price, Flow> cache;
    auto [it, inserted] = cache.try_emplace(levels);
    if (inserted) {
        it->second.measured = bench::generate_passive_flow(kBatch, levels);
        size_config(it->second);
    }
    return it->second;
}

// Ask-Leiter mit sweeps * levels_per_sweep Leveln à kOrdersPerLevel Orders, dazu
// sweeps MARKET-Buys, von denen jeder genau levels_per_sweep Level abräumt.
constexpr std::size_t kOrdersPerLevel = 4;
constexpr engine::Quantity kLadderQty = 100;
constexpr std::size_t kSweepRestingOrders = 100'000;

const Flow& sweep_flow(engine::Price levels_per_sweep) {
    static std::map<engine::Price, Flow> cache;
    auto [it, inserted] = cache.try_emplace(levels_per_sweep);
    if (inserted) {
        const std::size_t sweeps = kSweepRestingOrders / (static_cast<std::size_t>(levels_per_sweep) * kOrdersPerLevel);
        const auto levels = static_cast<engine::Price>(sweeps) * levels_per_sweep;
        it->second.warmup = bench::generate_ask_ladder(levels, kOrdersPerLevel, kLadderQty, 1);
        const engine::OrderId first = it->second.warmup.size() + 1;
        const engine::Quantity qty = static_cast<engine::Quantity>(levels_per_sweep) * kOrdersPerLevel * kLadderQty;
        it->second.measured.reserve(sweeps);
        for (std::size_t i = 0; i < sweeps; ++i) {
            it->second.measured.emplace_back(engine::NewOrder{
                .id = first + i,
                .symbol = 0,
                .side = engine::Side::Buy,
                .type = engine::OrderType::Market,
                .price = 0,
                .quantity = qty,
            });
        }
        size_config(it->second);
    }
    return it->second;
}

// Stufe 3: ~50 % NEW / ~35 % CANCEL / ~15 % MODIFY gegen ein Buch mit ~target_resting
// liegenden Orders je Seite. Warmup wie beim gemischten Fluss, gemessen werden kBatch Commands.
const Flow& cancel_modify_flow(std::size_t target_resting) {
    static std::map<std::size_t, Flow> cache;
    auto [it, inserted] = cache.try_emplace(target_resting);
    if (inserted) {
        bench::CancelModifyFlowConfig cfg;
        cfg.base.target_resting = target_resting;
        const std::size_t warmup = std::max<std::size_t>(100'000, 50 * target_resting);
        cfg.base.count = warmup + kBatch;
        std::vector<Command> all = bench::generate_cancel_modify_flow(cfg);
        it->second.warmup.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(warmup));
        it->second.measured.assign(all.begin() + static_cast<std::ptrdiff_t>(warmup), all.end());
        size_config(it->second);
    }
    return it->second;
}

// ---------------------------------------------------------------------------
// Hilfen

template <typename Engine>
std::unique_ptr<Engine> make_engine(engine::EventSink& sink, const Flow& flow) {
    auto eng = std::make_unique<Engine>(sink, flow.config);
    for (engine::SymbolId s = 0; s < flow.symbols; ++s) {
        eng->add_symbol(s);
    }
    for (const Command& cmd : flow.warmup) {
        eng->process(cmd);
    }
    return eng;
}

// Buchgröße am Ende des letzten Batches (über alle Symbole) als Kontext-Counter.
template <typename Engine>
void report_book(benchmark::State& state, const Engine& eng, const Flow& flow) {
    double levels = 0;
    double qty = 0;
    for (engine::SymbolId s = 0; s < flow.symbols; ++s) {
        const engine::BookSnapshot snap = eng.snapshot(s, 1'000'000);
        levels += static_cast<double>(snap.bids.size() + snap.asks.size());
        for (const auto& l : snap.bids) {
            qty += static_cast<double>(l.quantity);
        }
        for (const auto& l : snap.asks) {
            qty += static_cast<double>(l.quantity);
        }
    }
    state.counters["book_levels"] = levels;
    state.counters["book_qty"] = qty;
}

// Durchsatz: je Iteration eine frische, vorbefüllte Engine, gemessen wird nur
// die Schleife über flow.measured. Die Engine des letzten Batches wird erst in
// der nächsten Iteration (pausiert) bzw. nach der Schleife (ungemessen) abgebaut.
template <typename Engine>
void run_throughput(benchmark::State& state, const Flow& flow) {
    NullSink sink;
    std::unique_ptr<Engine> eng;
    for (auto _ : state) {
        state.PauseTiming();
        eng.reset();
        eng = make_engine<Engine>(sink, flow);
        state.ResumeTiming();

        Engine& e = *eng;
        for (const Command& cmd : flow.measured) {
            e.process(cmd);
        }
        benchmark::DoNotOptimize(e);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(flow.measured.size()));
    if (eng) {
        report_book(state, *eng, flow);
    }
}

// Latenz: jede Order einzeln mit steady_clock (vDSO, Clocksource TSC) gemessen.
// Die Samples landen in einem vorab reservierten Vektor; nach der Messung werden
// Perzentile über alle Iterationen gebildet. Die Overhead-Messung von
// steady_clock::now() wird als clock_ns mit ausgegeben (nicht abgezogen).
constexpr benchmark::IterationCount kLatencyIterations = 5;

double percentile(std::vector<std::int64_t>& v, double q) {
    const auto k = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return static_cast<double>(v[k]);
}

double clock_overhead_ns() {
    using clock = std::chrono::steady_clock;
    std::vector<std::int64_t> d;
    d.reserve(100'000);
    for (int i = 0; i < 100'000; ++i) {
        const auto t0 = clock::now();
        const auto t1 = clock::now();
        d.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    return percentile(d, 0.5);
}

template <typename Engine>
void run_latency(benchmark::State& state, const Flow& flow) {
    using clock = std::chrono::steady_clock;
    NullSink sink;
    std::unique_ptr<Engine> eng;
    std::vector<std::int64_t> samples;
    samples.reserve(static_cast<std::size_t>(state.max_iterations) * flow.measured.size());

    for (auto _ : state) {
        state.PauseTiming();
        eng.reset();
        eng = make_engine<Engine>(sink, flow);
        state.ResumeTiming();

        Engine& e = *eng;
        for (const Command& cmd : flow.measured) {
            const auto t0 = clock::now();
            std::atomic_signal_fence(std::memory_order_seq_cst);
            e.process(cmd);
            std::atomic_signal_fence(std::memory_order_seq_cst);
            const auto t1 = clock::now();
            samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
        benchmark::DoNotOptimize(e);
    }
    eng.reset();  // nach der Schleife läuft keine Zeitmessung mehr

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(flow.measured.size()));
    if (samples.empty()) {
        return;
    }
    state.counters["p50_ns"] = percentile(samples, 0.50);
    state.counters["p90_ns"] = percentile(samples, 0.90);
    state.counters["p99_ns"] = percentile(samples, 0.99);
    state.counters["p99.9_ns"] = percentile(samples, 0.999);
    state.counters["max_ns"] = static_cast<double>(*std::max_element(samples.begin(), samples.end()));
    state.counters["clock_ns"] = clock_overhead_ns();
}

// Registrierung: jeder Benchmark einmal je Buch-Variante, Argumente über eine Apply-Funktion.
#define ENGINE_VARIANTS(bm, args)                \
    BENCHMARK_TEMPLATE(bm, Map)->Apply(args);    \
    BENCHMARK_TEMPLATE(bm, Vector)->Apply(args); \
    BENCHMARK_TEMPLATE(bm, Tick)->Apply(args)

// ---------------------------------------------------------------------------
// Durchsatz (Flüsse unverändert gegenüber der Baseline)

// Nur passive LIMITs (kein Matching) in ein anfangs leeres Buch mit bis zu
// 2 * range(0) Price Levels; das Buch wächst im Batch auf kBatch Orders.
template <typename Engine>
void BM_PassiveInsert(benchmark::State& state) {
    run_throughput<Engine>(state, passive_flow(static_cast<engine::Price>(state.range(0))));
}
void passive_args(benchmark::internal::Benchmark* b) {
    b->ArgName("levels")->Arg(10)->Arg(100)->Arg(1000)->Unit(benchmark::kMillisecond);
}
ENGINE_VARIANTS(BM_PassiveInsert, passive_args);

// Realistischer gemischter Fluss (~90 % LIMIT / 10 % MARKET, ~1 Trade je Order)
// gegen ein stationäres Buch mit ~range(0) ruhenden Orders je Seite.
template <typename Engine>
void BM_MixedFlow(benchmark::State& state) {
    run_throughput<Engine>(state, mixed_flow(static_cast<std::size_t>(state.range(0)), 1));
}
void resting_args(benchmark::internal::Benchmark* b) {
    b->ArgName("resting")->Arg(100)->Arg(1000)->Arg(10000)->Unit(benchmark::kMillisecond);
}
ENGINE_VARIANTS(BM_MixedFlow, resting_args);

// Gemischter Fluss verteilt auf range(0) Symbole (je ~200 ruhende Orders je Seite).
template <typename Engine>
void BM_MixedFlowSymbols(benchmark::State& state) {
    run_throughput<Engine>(state, mixed_flow(200, static_cast<std::uint32_t>(state.range(0))));
}
void symbols_args(benchmark::internal::Benchmark* b) {
    b->ArgName("symbols")->Arg(1)->Arg(10)->Arg(100)->Unit(benchmark::kMillisecond);
}
ENGINE_VARIANTS(BM_MixedFlowSymbols, symbols_args);

// MARKET-Buys, die je range(0) Level à 4 Orders komplett abräumen
// (items = Sweeps; trades_per_sweep zur Umrechnung auf Trades/s).
template <typename Engine>
void BM_AggressiveSweep(benchmark::State& state) {
    const Flow& flow = sweep_flow(static_cast<engine::Price>(state.range(0)));
    run_throughput<Engine>(state, flow);
    state.counters["trades_per_sweep"] = static_cast<double>(state.range(0) * kOrdersPerLevel);
}
void sweep_args(benchmark::internal::Benchmark* b) {
    b->ArgName("levels")->Arg(1)->Arg(10)->Arg(50)->Unit(benchmark::kMillisecond);
}
ENGINE_VARIANTS(BM_AggressiveSweep, sweep_args);

// Stufe 3: ~50 % NEW / ~35 % CANCEL / ~15 % MODIFY gegen ein Buch mit ~range(0)
// liegenden Orders je Seite (items = Commands).
template <typename Engine>
void BM_CancelModifyFlow(benchmark::State& state) {
    run_throughput<Engine>(state, cancel_modify_flow(static_cast<std::size_t>(state.range(0))));
}
ENGINE_VARIANTS(BM_CancelModifyFlow, resting_args);

// ---------------------------------------------------------------------------
// Latenz pro Command

void latency_resting_args(benchmark::internal::Benchmark* b) {
    b->ArgName("resting")->Arg(1000)->Arg(10000)->Iterations(kLatencyIterations)->Unit(benchmark::kMillisecond);
}

template <typename Engine>
void BM_LatencyMixed(benchmark::State& state) {
    run_latency<Engine>(state, mixed_flow(static_cast<std::size_t>(state.range(0)), 1));
}
ENGINE_VARIANTS(BM_LatencyMixed, latency_resting_args);

template <typename Engine>
void BM_LatencyPassive(benchmark::State& state) {
    run_latency<Engine>(state, passive_flow(static_cast<engine::Price>(state.range(0))));
}
void latency_passive_args(benchmark::internal::Benchmark* b) {
    b->ArgName("levels")->Arg(100)->Iterations(kLatencyIterations)->Unit(benchmark::kMillisecond);
}
ENGINE_VARIANTS(BM_LatencyPassive, latency_passive_args);

template <typename Engine>
void BM_LatencyCancelModify(benchmark::State& state) {
    run_latency<Engine>(state, cancel_modify_flow(static_cast<std::size_t>(state.range(0))));
}
ENGINE_VARIANTS(BM_LatencyCancelModify, latency_resting_args);

}  // namespace

BENCHMARK_MAIN();
