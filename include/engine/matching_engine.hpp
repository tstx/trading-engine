#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>

#include "engine/engine_config.hpp"
#include "engine/event_sink.hpp"
#include "engine/flat_hash.hpp"
#include "engine/object_pool.hpp"
#include "engine/order_book.hpp"
#include "engine/types.hpp"

namespace engine {

// Single-threaded Matching nach Price-Time-Priority. Kein I/O.
//
// Semantik (verbindlich, abgeleitet aus plan.txt §2/§4):
// NEW
// - Validierung in dieser Reihenfolge: DUPLICATE_ID -> UNKNOWN_SYMBOL
//   -> INVALID_QUANTITY -> INVALID_PRICE (nur LIMIT: price <= 0 oder !book.accepts_price()).
//   Eine abgelehnte Order belegt ihre ID nicht und bekommt keine Seq.
// - Seq ist ein globaler Zähler pro Engine, startet bei 1, nur für akzeptierte Orders
//   (und für MODIFY mit Prioritätsverlust, s. u.).
// - MARKET wird ebenfalls geACKt; der Preis wird ignoriert; nicht ausführbarer
//   Rest -> on_cancelled(id, rest, NoLiquidity).
// - Event-Reihenfolge: ACK -> pro Match TRADE, FILL(resting), FILL(aggressor) -> ggf. CANCELLED.
// - Jede je akzeptierte ID bleibt gesperrt (DUPLICATE_ID bei Wiederverwendung), auch nach
//   Fill, Cancel oder NO_LIQUIDITY.
// CANCEL (FR-11)
// - Liegende Order -> aus dem Buch, on_cancelled(id, remaining, User).
// - Unbekannte, gefüllte oder schon gecancelte ID -> on_reject(id, UnknownOrder).
// MODIFY (FR-12), new_quantity = neue GESAMTmenge inkl. bereits gefüllter Menge
// - Prüfreihenfolge: nicht liegend -> UNKNOWN_ORDER; new_quantity <= filled -> INVALID_MODIFY;
//   new_price <= 0 oder !accepts_price -> INVALID_PRICE. Bei Reject bleibt die Order unverändert.
// - Gleicher Preis und new_quantity <= bisherige Gesamtmenge (filled + remaining):
//   remaining = new_quantity - filled, Position bleibt; on_ack(id, alte Seq).
// - Sonst (Preisänderung oder Erhöhung): Order verlässt das Buch, bekommt eine neue Seq,
//   on_ack(id, neue Seq), matcht als Aggressor wie eine NEW-LIMIT-Order mit
//   remaining = new_quantity - filled (TRADE/FILL wie bei NEW; FILL meldet den Rest);
//   ein Rest geht hinten ins Level des neuen Preises.
template <typename Book>
class BasicMatchingEngine {
public:
    explicit BasicMatchingEngine(EventSink& sink, const EngineConfig& config = {});
    ~BasicMatchingEngine();

    BasicMatchingEngine(const BasicMatchingEngine&) = delete;
    BasicMatchingEngine& operator=(const BasicMatchingEngine&) = delete;

    // Macht ein Instrument handelbar (idempotent). IDs müssen nicht dicht sein.
    void add_symbol(SymbolId symbol);
    [[nodiscard]] bool has_symbol(SymbolId symbol) const;

    void process(const Command& cmd);

    // Leeres Snapshot (ohne Level) für unbekannte Symbole.
    [[nodiscard]] BookSnapshot snapshot(SymbolId symbol, std::size_t depth) const;

    [[nodiscard]] const EngineConfig& config() const noexcept { return config_; }

private:
    void handle(const NewOrder& order);
    void handle(const CancelOrder& cancel);
    void handle(const ModifyOrder& modify);

    EngineConfig config_;
    EventSink& sink_;
    SeqNo next_seq_{1};
    std::unordered_map<SymbolId, std::unique_ptr<Book>> books_;
    ObjectPool<Order> orders_;                  // besitzt alle liegenden Orders
    FlatHashMap<OrderId, Order*> live_orders_;  // liegende Orders (Cancel/Modify in O(1))
    FlatHashSet<OrderId> known_ids_;            // alle je akzeptierten IDs
};

// Explizit instanziiert in src/matching_engine.cpp.
using MapMatchingEngine = BasicMatchingEngine<MapOrderBook>;
using VectorMatchingEngine = BasicMatchingEngine<VectorOrderBook>;
using TickMatchingEngine = BasicMatchingEngine<TickOrderBook>;

using MatchingEngine = BasicMatchingEngine<OrderBook>;

}  // namespace engine
