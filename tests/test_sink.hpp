#pragma once

// Test-Hilfen: RecordingSink zeichnet jedes Engine-Event als String im
// Text-Format von plan.txt §4.1 auf; EngineFixture<Engine> setzt SymbolTable, Engine
// und Sink mit dem Symbol "ACME" auf; EngineTypes/EngineTypeNames für TYPED_TESTs
// über alle drei Engine-Varianten.

#include <cstddef>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "engine/engine_config.hpp"
#include "engine/event_sink.hpp"
#include "engine/matching_engine.hpp"
#include "engine/symbol_table.hpp"
#include "engine/types.hpp"

namespace engine::test {

using Events = std::vector<std::string>;

inline std::string levels_text(const std::vector<BookLevel>& levels) {
    std::string out;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i != 0) {
            out += ';';
        }
        out += std::to_string(levels[i].price);
        out += 'x';
        out += std::to_string(levels[i].quantity);
    }
    return out;
}

// "BOOK,ACME,BIDS:<p>x<q>;...,ASKS:<p>x<q>;..." (kein abschließendes ';').
inline std::string book_line(const SymbolTable& symbols, const BookSnapshot& snap) {
    std::string out = "BOOK,";
    out += symbols.name(snap.symbol);
    out += ",BIDS:";
    out += levels_text(snap.bids);
    out += ",ASKS:";
    out += levels_text(snap.asks);
    return out;
}

class RecordingSink final : public EventSink {
public:
    explicit RecordingSink(const SymbolTable& symbols) : symbols_(symbols) {}

    void on_ack(OrderId id, SeqNo seq) override {
        events.push_back("ACK," + std::to_string(id) + "," + std::to_string(seq));
    }
    void on_reject(OrderId id, RejectReason reason) override {
        events.push_back("REJECT," + std::to_string(id) + "," + std::string(to_string(reason)));
    }
    void on_trade(const Trade& t) override {
        events.push_back("TRADE," + std::string(symbols_.name(t.symbol)) + "," + std::to_string(t.price) + "," +
                         std::to_string(t.quantity) + "," + std::to_string(t.buy_order_id) + "," +
                         std::to_string(t.sell_order_id) + "," + std::string(to_string(t.aggressor)));
    }
    void on_fill(OrderId id, Quantity filled, Quantity remaining) override {
        events.push_back("FILL," + std::to_string(id) + "," + std::to_string(filled) + "," + std::to_string(remaining));
    }
    void on_cancelled(OrderId id, Quantity remaining, CancelReason reason) override {
        events.push_back("CANCELLED," + std::to_string(id) + "," + std::to_string(remaining) + "," +
                         std::string(to_string(reason)));
    }

    // Liefert die bisher aufgezeichneten Events und leert den Puffer.
    Events take() {
        Events out;
        out.swap(events);
        return out;
    }

    Events events;

private:
    const SymbolTable& symbols_;
};

// Die drei Engine-Varianten (Level-Struktur std::map / sortierter Vektor / Tick-Array).
using EngineTypes = ::testing::Types<MapMatchingEngine, VectorMatchingEngine, TickMatchingEngine>;

// Lesbare Namen für TYPED_TEST_SUITE: "Map", "Vector", "Tick".
struct EngineTypeNames {
    template <typename T>
    static std::string GetName(int /*index*/) {
        if constexpr (std::is_same_v<T, MapMatchingEngine>) {
            return "Map";
        } else if constexpr (std::is_same_v<T, VectorMatchingEngine>) {
            return "Vector";
        } else if constexpr (std::is_same_v<T, TickMatchingEngine>) {
            return "Tick";
        } else {
            return "Unknown";
        }
    }
};

// Fixture-Basis: Engine (Variante Engine) mit registriertem Symbol "ACME".
// In TYPED_TESTs sind die Member über this-> anzusprechen.
template <typename Engine = MatchingEngine>
class EngineFixture : public ::testing::Test {
protected:
    static constexpr std::size_t kDepth = 100;

    explicit EngineFixture(const EngineConfig& config = {}) : acme(symbols.add("ACME")), engine{sink, config} {
        engine.add_symbol(acme);
    }

    void new_order(OrderId id, SymbolId symbol, Side side, OrderType type, Price price, Quantity qty) {
        engine.process(Command{NewOrder{id, symbol, side, type, price, qty}});
    }
    void new_limit(OrderId id, Side side, Price price, Quantity qty) {
        new_order(id, acme, side, OrderType::Limit, price, qty);
    }
    void new_market(OrderId id, Side side, Quantity qty, Price price = 0) {
        new_order(id, acme, side, OrderType::Market, price, qty);
    }
    void cancel(OrderId id) { engine.process(Command{CancelOrder{id}}); }
    void modify(OrderId id, Price new_price, Quantity new_quantity) {
        engine.process(Command{ModifyOrder{id, new_price, new_quantity}});
    }
    [[nodiscard]] std::string book(std::size_t depth = kDepth) const {
        return book_line(symbols, engine.snapshot(acme, depth));
    }
    Events take() { return sink.take(); }

    SymbolTable symbols;
    SymbolId acme;
    RecordingSink sink{symbols};
    Engine engine;
};

}  // namespace engine::test
