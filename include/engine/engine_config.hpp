#pragma once

#include <cstddef>

#include "engine/types.hpp"

namespace engine {

// Vorab reservierte Kapazitäten (NFR-6). Innerhalb dieser Grenzen allokiert der heiße Pfad
// nicht; darüber hinaus wachsen Pool/Tabellen blockweise (kein Reject).
struct EngineConfig {
    std::size_t order_capacity = std::size_t{1} << 16;  // gleichzeitig liegende Orders (ObjectPool)
    std::size_t id_capacity = std::size_t{1} << 17;     // je akzeptierte OrderIds (DUPLICATE_ID-Prüfung)
    std::size_t level_capacity = std::size_t{1} << 10;  // Price Levels je Buchseite
    std::size_t symbol_capacity = 64;                   // Instrumente
    Price max_tick_price = Price{1} << 17;              // TickArrayLevels: gültige Preise 1..max_tick_price
};

}  // namespace engine
