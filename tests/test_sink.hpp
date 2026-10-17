#pragma once

// Test-Hilfen: RecordingSink zeichnet jedes Engine-Event als String im
// Text-Format von plan.txt §4.1 auf; EngineFixture setzt SymbolTable, Engine
// und Sink mit dem Symbol "ACME" auf.

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

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
        events.push_back("FILL," + std::to_string(id) + "," + std::to_string(filled) + "," +
                         std::to_string(remaining));
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

// Fixture-Basis: Engine mit registriertem Symbol "ACME".
class EngineFixture : public ::testing::Test {
protected:
    static constexpr std::size_t kDepth = 100;

    EngineFixture() : acme(symbols.add("ACME")) { engine.add_symbol(acme); }

    void new_order(OrderId id, SymbolId symbol, Side side, OrderType type, Price price, Quantity qty) {
        engine.process(Command{NewOrder{id, symbol, side, type, price, qty}});
    }
    void new_limit(OrderId id, Side side, Price price, Quantity qty) {
        new_order(id, acme, side, OrderType::Limit, price, qty);
    }
    void new_market(OrderId id, Side side, Quantity qty, Price price = 0) {
        new_order(id, acme, side, OrderType::Market, price, qty);
    }
    [[nodiscard]] std::string book(std::size_t depth = kDepth) const {
        return book_line(symbols, engine.snapshot(acme, depth));
    }
    Events take() { return sink.take(); }

    SymbolTable symbols;
    SymbolId acme;
    RecordingSink sink{symbols};
    MatchingEngine engine{sink};
};

}  // namespace engine::test
