#pragma once

// Deterministischer Orderfluss-Generator für die Benchmarks.
//
// Alle Commands werden vorab in einen std::vector erzeugt; die Messschleife
// iteriert nur noch darüber. Gleiche Konfiguration (inkl. Seed) -> bitgenau
// gleicher Fluss, unabhängig von Plattform-Implementierungen der
// std::*_distribution (es wird nur std::mt19937_64 direkt verwendet).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "engine/event_sink.hpp"
#include "engine/matching_engine.hpp"
#include "engine/types.hpp"

namespace bench {

struct OrderFlowConfig {
    std::size_t count = 100'000;  // Anzahl erzeugter NEW-Orders
    std::uint64_t seed = 0x5EED'0001;
    std::uint32_t symbols = 1;           // SymbolIds 0 .. symbols-1 (der Benchmark muss sie anlegen)
    double market_ratio = 0.10;          // Anteil MARKET-Orders
    double cross_ratio = 0.40;           // Grundanteil kreuzender LIMIT-Orders bei Soll-Buchtiefe (siehe unten)
    engine::Price mid_price = 10'000;    // Start-Mittelpreis, solange das Buch leer ist
    engine::Price max_offset = 50;       // LIMIT-Preise liegen in [mid - max_offset, mid + max_offset]
    double mean_offset = 8.0;            // passive LIMITs: mittlere (geometrische) Distanz zum Mittelpreis in Ticks
    double cross_mean_offset = 4.0;      // kreuzende LIMITs: mittlere Distanz jenseits des Mittelpreises
    std::size_t target_resting = 2'000;  // Soll-Anzahl ruhender Orders je Seite und Symbol (Regelgröße)
    engine::Quantity min_qty = 1;
    engine::Quantity max_qty = 1000;
    engine::OrderId first_id = 1;  // IDs sind fortlaufend ab first_id
};

namespace detail {

// Eigene, triviale Verteilungen: std::uniform_*_distribution ist zwischen
// Standardbibliotheken nicht reproduzierbar.
inline double unit(std::mt19937_64& rng) { return static_cast<double>(rng() >> 11) * 0x1.0p-53; }

inline std::uint64_t uniform(std::mt19937_64& rng, std::uint64_t lo, std::uint64_t hi) {
    return lo + rng() % (hi - lo + 1);  // Modulo-Bias ist für Benchmarkdaten irrelevant
}

// Geometrisch verteilte Distanz in [0, max] (Ablehnungs-Sampling statt Abschneiden,
// damit am Rand keine Häufung entsteht).
inline engine::Price offset(std::mt19937_64& rng, double mean, engine::Price max) {
    const double p = 1.0 / (mean + 1.0);
    for (;;) {
        engine::Price d = 0;
        while (unit(rng) >= p && d <= max) {
            ++d;
        }
        if (d <= max) {
            return d;
        }
    }
}

}  // namespace detail

// Realistischer gemischter Fluss: beide Seiten gleich wahrscheinlich, Mengen
// gleichverteilt in [min_qty, max_qty], ~market_ratio MARKET-Orders.
// LIMIT-Preise liegen geometrisch verteilt um den aktuellen Mittelpreis des
// Buchs: passive auf ihrer Seite, kreuzende (marketable) darüber hinaus.
//
// Ohne CANCEL (Stufe 1) wächst ein Buch mit festem Passiv-Anteil entweder
// unbegrenzt oder läuft leer. Der Generator spielt den Fluss deshalb gegen
// eine Schatten-Engine und regelt den Anteil kreuzender Orders je Seite
// proportional zur ruhenden Gegenseite (Soll: target_resting Orders). Das
// Ergebnis bleibt vollständig deterministisch (Seed + Engine-Semantik) und
// hängt nicht von der Messung ab.
inline std::vector<engine::Command> generate_order_flow(const OrderFlowConfig& cfg) {
    // Zählt ruhende Menge je Symbol und Seite mit (nur Trades verändern sie außer dem Einstellen).
    class ShadowSink final : public engine::EventSink {
    public:
        explicit ShadowSink(std::uint32_t symbols) : resting_(symbols) {}
        void on_ack(engine::OrderId, engine::SeqNo) override {}
        void on_reject(engine::OrderId, engine::RejectReason) override {}
        void on_trade(const engine::Trade& t) override {
            const auto passive = t.aggressor == engine::Side::Buy ? engine::Side::Sell : engine::Side::Buy;
            resting_[t.symbol][static_cast<std::size_t>(passive)] -= t.quantity;
            aggressor_traded_ += t.quantity;
        }
        void on_fill(engine::OrderId, engine::Quantity, engine::Quantity) override {}
        void on_cancelled(engine::OrderId, engine::Quantity, engine::CancelReason) override {}

        std::vector<std::array<engine::Quantity, 2>> resting_;
        engine::Quantity aggressor_traded_ = 0;
    };

    std::mt19937_64 rng(cfg.seed);
    ShadowSink shadow(cfg.symbols);
    engine::MatchingEngine book(shadow);
    for (engine::SymbolId s = 0; s < cfg.symbols; ++s) {
        book.add_symbol(s);
    }
    std::vector<engine::Price> mids(cfg.symbols, cfg.mid_price);
    // Im Gleichgewicht liegt p_cross bei ~2 * cross_ratio; der Faktor 1/2 sorgt dafür, dass sich die
    // ruhende Tiefe dann bei ~target_resting Orders je Seite einpendelt (empirisch geprüft).
    const double mean_qty = static_cast<double>(cfg.min_qty + cfg.max_qty) / 2.0;
    const double target_qty = static_cast<double>(cfg.target_resting) * mean_qty / 2.0;

    std::vector<engine::Command> out;
    out.reserve(cfg.count);
    for (std::size_t i = 0; i < cfg.count; ++i) {
        const auto symbol =
            static_cast<engine::SymbolId>(cfg.symbols > 1 ? detail::uniform(rng, 0, cfg.symbols - 1) : 0);

        // Mittelpreis aus dem Top of Book; fehlt eine Seite, bleibt der letzte Wert.
        const engine::BookSnapshot top = book.snapshot(symbol, 1);
        engine::Price& mid = mids[symbol];
        if (!top.bids.empty() && !top.asks.empty()) {
            mid = (top.bids.front().price + top.asks.front().price) / 2;
        }

        engine::NewOrder order{};
        order.id = cfg.first_id + i;
        order.symbol = symbol;
        order.side = (rng() & 1U) != 0 ? engine::Side::Buy : engine::Side::Sell;
        order.quantity = detail::uniform(rng, cfg.min_qty, cfg.max_qty);
        const bool buy = order.side == engine::Side::Buy;

        if (detail::unit(rng) < cfg.market_ratio) {
            order.type = engine::OrderType::Market;
            order.price = 0;
        } else {
            order.type = engine::OrderType::Limit;
            const auto opposite = static_cast<std::size_t>(buy ? engine::Side::Sell : engine::Side::Buy);
            const double fill_level = static_cast<double>(shadow.resting_[symbol][opposite]) / target_qty;
            const double p_cross = std::min(0.95, cfg.cross_ratio * fill_level);
            if (detail::unit(rng) < p_cross) {
                const engine::Price d = detail::offset(rng, cfg.cross_mean_offset, cfg.max_offset);
                order.price = buy ? mid + d : mid - d;
            } else {
                const engine::Price d = 1 + detail::offset(rng, cfg.mean_offset, cfg.max_offset - 1);
                order.price = buy ? mid - d : mid + d;
            }
            order.price = std::max<engine::Price>(order.price, 1);
        }

        shadow.aggressor_traded_ = 0;
        book.process(order);
        if (order.type == engine::OrderType::Limit) {
            shadow.resting_[symbol][static_cast<std::size_t>(order.side)] += order.quantity - shadow.aggressor_traded_;
        }
        out.emplace_back(order);
    }
    return out;
}

// Nur passive LIMIT-Orders: Bids in [mid - levels, mid - 1], Asks in
// [mid + 1, mid + levels], gleichverteilt. Kreuzt nie -> misst reines Einfügen
// in ein Buch mit bis zu 2 * levels Price Levels.
inline std::vector<engine::Command> generate_passive_flow(std::size_t count, engine::Price levels,
                                                          std::uint64_t seed = 0x5EED'0002,
                                                          engine::OrderId first_id = 1,
                                                          engine::Price mid_price = 10'000) {
    std::mt19937_64 rng(seed);
    std::vector<engine::Command> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const bool buy = (rng() & 1U) != 0;
        const auto d = static_cast<engine::Price>(detail::uniform(rng, 1, static_cast<std::uint64_t>(levels)));
        out.emplace_back(engine::NewOrder{
            .id = first_id + i,
            .symbol = 0,
            .side = buy ? engine::Side::Buy : engine::Side::Sell,
            .type = engine::OrderType::Limit,
            .price = buy ? mid_price - d : mid_price + d,
            .quantity = detail::uniform(rng, 1, 1000),
        });
    }
    return out;
}

// Ask-Leiter für Sweep-Benchmarks: levels Level ab mid + 1 mit je orders_per_level
// Orders der Menge qty. IDs fortlaufend ab first_id.
inline std::vector<engine::Command> generate_ask_ladder(engine::Price levels, std::size_t orders_per_level,
                                                        engine::Quantity qty, engine::OrderId first_id,
                                                        engine::Price mid_price = 10'000) {
    std::vector<engine::Command> out;
    out.reserve(static_cast<std::size_t>(levels) * orders_per_level);
    engine::OrderId id = first_id;
    for (engine::Price l = 1; l <= levels; ++l) {
        for (std::size_t k = 0; k < orders_per_level; ++k) {
            out.emplace_back(engine::NewOrder{
                .id = id++,
                .symbol = 0,
                .side = engine::Side::Sell,
                .type = engine::OrderType::Limit,
                .price = mid_price + l,
                .quantity = qty,
            });
        }
    }
    return out;
}

}  // namespace bench
