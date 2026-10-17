#pragma once

#include <cstddef>
#include <functional>

#include "engine/engine_config.hpp"
#include "engine/level_store.hpp"
#include "engine/price_level.hpp"
#include "engine/sorted_vector_levels.hpp"
#include "engine/tick_array_levels.hpp"
#include "engine/types.hpp"

namespace engine {

// Orderbuch eines Instruments, parametrisiert über die Level-Struktur (siehe level_store.hpp).
// Kennt kein Matching-Protokoll und keine Events; die MatchingEngine iteriert über die
// Gegenseite und ruft fill()/reduce()/remove().
template <template <typename> class LevelStore>
class BasicOrderBook {
public:
    using Bids = LevelStore<std::greater<>>;  // best() = höchster Bid
    using Asks = LevelStore<std::less<>>;     // best() = niedrigster Ask

    explicit BasicOrderBook(SymbolId symbol, const EngineConfig& config = {});

    // Kann ein LIMIT-Preis in diesem Buch liegen? (Tick-Array: Preisband)
    [[nodiscard]] bool accepts_price(Price price) const noexcept;

    // Hängt die Order ans Ende ihres Price Levels (legt das Level ggf. an), setzt order->level.
    void add(Order* order);

    // Entfernt die Order aus ihrem Level; leeres Level wird gelöscht (FR-9). order->level = nullptr.
    void remove(Order* order);

    // Reduziert remaining um qty (und total_quantity des Levels), erhöht filled.
    // Entfernt die Order NICHT, auch wenn remaining == 0 wird.
    void fill(Order* order, Quantity qty);

    // Setzt remaining auf new_remaining (< remaining) OHNE Positionsverlust (MODIFY-Reduktion, FR-12).
    // filled bleibt unverändert.
    void reduce(Order* order, Quantity new_remaining);

    // Älteste Order auf dem besten Level der Seite, nullptr wenn leer.
    [[nodiscard]] Order* best_bid() const noexcept;
    [[nodiscard]] Order* best_ask() const noexcept;

    // Bis zu depth Level je Seite, bester zuerst (FR-13).
    [[nodiscard]] BookSnapshot snapshot(std::size_t depth) const;

    [[nodiscard]] SymbolId symbol() const noexcept { return symbol_; }
    [[nodiscard]] const Bids& bids() const noexcept { return bids_; }
    [[nodiscard]] const Asks& asks() const noexcept { return asks_; }

private:
    SymbolId symbol_;
    Bids bids_;
    Asks asks_;
};

// Explizit instanziiert in src/order_book.cpp.
using MapOrderBook = BasicOrderBook<MapLevels>;
using VectorOrderBook = BasicOrderBook<SortedVectorLevels>;
using TickOrderBook = BasicOrderBook<TickArrayLevels>;

// Standard-Variante (Benchmark-Vergleich Stufe 3, docs/benchmarks.md): sortierter Vektor —
// allokationsfrei, ohne Preisband. TickOrderBook nur für Instrumente mit engem, bekanntem Band.
using OrderBook = VectorOrderBook;

}  // namespace engine
