#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/event_sink.hpp"
#include "engine/order_book.hpp"
#include "engine/types.hpp"

namespace engine {

// Single-threaded Matching nach Price-Time-Priority. Kein I/O.
//
// Semantik (verbindlich, abgeleitet aus plan.txt §2/§4):
// - Validierung in dieser Reihenfolge: DUPLICATE_ID -> UNKNOWN_SYMBOL
//   -> INVALID_QUANTITY -> INVALID_PRICE (nur LIMIT, price <= 0).
//   Eine abgelehnte Order belegt ihre ID nicht und bekommt keine Seq.
// - Seq ist ein globaler Zähler pro Engine, startet bei 1, nur für akzeptierte Orders.
// - MARKET wird ebenfalls geACKt; der Preis wird ignoriert; nicht ausführbarer
//   Rest -> on_cancelled(id, rest, NoLiquidity).
// - Gefüllte Orders (remaining == 0) verlassen Buch und Order-Index, ihre ID
//   bleibt aber als "bekannt" gesperrt (DUPLICATE_ID bei Wiederverwendung).
// - CANCEL/MODIFY: Stufe 3. Bis dahin -> on_reject(id, UnknownOrder).
class MatchingEngine {
public:
    explicit MatchingEngine(EventSink& sink);
    ~MatchingEngine();

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // Macht ein Instrument handelbar. IDs müssen nicht dicht sein.
    void add_symbol(SymbolId symbol);
    [[nodiscard]] bool has_symbol(SymbolId symbol) const;

    void process(const Command& cmd);

    // Leeres Snapshot (ohne Level) für unbekannte Symbole.
    [[nodiscard]] BookSnapshot snapshot(SymbolId symbol, std::size_t depth) const;

private:
    void handle(const NewOrder& order);
    void handle(const CancelOrder& cancel);
    void handle(const ModifyOrder& modify);

    EventSink& sink_;
    SeqNo next_seq_{1};
    std::unordered_map<SymbolId, std::unique_ptr<OrderBook>> books_;
    std::unordered_map<OrderId, std::unique_ptr<Order>> live_orders_;  // resting, besitzt die Orders
    std::unordered_set<OrderId> known_ids_;                            // alle je akzeptierten IDs
};

}  // namespace engine
