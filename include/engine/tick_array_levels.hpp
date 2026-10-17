#pragma once

#include "engine/level_store.hpp"

namespace engine {

// Stufe 3: Array über das Preisband 1..EngineConfig::max_tick_price, Index = Preis.
// Preise außerhalb des Bands: accepts() == false (Engine rejected mit INVALID_PRICE).
// STUB: delegiert vorerst an MapLevels (akzeptiert dadurch noch alle Preise).
template <typename Compare>
class TickArrayLevels : public MapLevels<Compare> {
public:
    using MapLevels<Compare>::MapLevels;
};

}  // namespace engine
