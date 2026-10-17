#pragma once

#include <cstddef>
#include <memory>
#include <vector>

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
//
// Allokationsfreiheit (NFR-6)
// - Heißer Pfad = process() mit NEW (LIMIT/MARKET), CANCEL, MODIFY inkl. aller Rejects,
//   Matching über beliebig viele Level und aller Aufrufe der EventSink.
// - Garantie für VectorMatchingEngine (SortedVectorLevels) und TickMatchingEngine
//   (TickArrayLevels): process() ruft weder operator new noch malloc auf, solange die
//   Kapazitäten der EngineConfig nicht überschritten sind, d. h.
//     gleichzeitig liegende Orders + 1 (Aggressor)  <= order_capacity     (ObjectPool)
//     liegende Orders                                <= order_capacity     (FlatHashMap, Lastfaktor 1/2)
//     je akzeptierte OrderIds                        <= id_capacity        (FlatHashSet)
//     Price Levels je Buchseite                      <= level_capacity     (ObjectPool + Vektor)
//   Darüber hinaus wachsen Pool und Tabellen blockweise bzw. per Rehash (amortisiert, kein
//   Reject); jede Wachstumsstufe ist eine Allokation, danach ist der Pfad wieder allokationsfrei.
//   Der Pfad selbst ist frei von Allokationen: std::visit, Trade/Events als Werte, Lookup des
//   Buchs per Vektorindex, Orders/Level aus Pools, Freigabe in Freilisten.
// - MapMatchingEngine (std::map, Referenzvariante aus Stufe 1) allokiert pro NEU angelegtem
//   Price Level einen Map-Knoten (und gibt ihn beim Leeren des Levels frei); alles andere ist
//   wie oben allokationsfrei. Sie erfüllt NFR-6 deshalb NICHT und dient nur als Referenz.
// - Die Garantie umfasst nicht die EventSink-Implementierung (die übergebene Sink muss selbst
//   allokationsfrei sein, z. B. die Event-Queue der Pipeline) und nicht add_symbol()/snapshot()
//   (Verwaltung bzw. Abfrage, außerhalb des heißen Pfads).
// - Nachweis: tests/allocation_test.cpp (eigenes Executable mit zählendem globalen operator new).
template <typename Book>
class BasicMatchingEngine {
public:
    explicit BasicMatchingEngine(EventSink& sink, const EngineConfig& config = {});
    ~BasicMatchingEngine();

    BasicMatchingEngine(const BasicMatchingEngine&) = delete;
    BasicMatchingEngine& operator=(const BasicMatchingEngine&) = delete;

    // Macht ein Instrument handelbar (idempotent). IDs müssen nicht dicht sein; dichte IDs
    // (SymbolTable vergibt sie ab 0) sind aber der schnelle Fall (siehe find_book()).
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

    // Buch zu symbol oder nullptr. Allokationsfrei, O(1).
    [[nodiscard]] Book* find_book(SymbolId symbol) const noexcept;

    // IDs unterhalb dieser Grenze werden direkt über einen Vektor indiziert (8 Byte je ID bis
    // zur größten angelegten ID, also höchstens 512 KiB). Größere IDs (sparse Nutzung, z. B.
    // fremd vergebene IDs) landen in einer FlatHashMap; so führt weder add_symbol(0xFFFFFFFE)
    // zu einer riesigen Allokation noch ein Lookup von kUnknownSymbol (0xFFFFFFFF) zu einem
    // Zugriff außerhalb des Vektors — beides ergibt sauber nullptr bzw. UNKNOWN_SYMBOL.
    static constexpr SymbolId kDenseSymbolLimit = SymbolId{1} << 16;

    EngineConfig config_;
    EventSink& sink_;
    SeqNo next_seq_{1};
    std::vector<std::unique_ptr<Book>> books_;   // besitzt alle Bücher (Anlagereihenfolge)
    std::vector<Book*> dense_books_;             // Index = SymbolId < kDenseSymbolLimit, sonst nullptr
    FlatHashMap<SymbolId, Book*> sparse_books_;  // SymbolId >= kDenseSymbolLimit
    ObjectPool<Order> orders_;                   // besitzt alle liegenden Orders
    FlatHashMap<OrderId, Order*> live_orders_;   // liegende Orders (Cancel/Modify in O(1))
    FlatHashSet<OrderId> known_ids_;             // alle je akzeptierten IDs
};

// Explizit instanziiert in src/matching_engine.cpp.
using MapMatchingEngine = BasicMatchingEngine<MapOrderBook>;
using VectorMatchingEngine = BasicMatchingEngine<VectorOrderBook>;
using TickMatchingEngine = BasicMatchingEngine<TickOrderBook>;

using MatchingEngine = BasicMatchingEngine<OrderBook>;

}  // namespace engine
