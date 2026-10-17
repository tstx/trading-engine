#pragma once

#include "engine/level_store.hpp"

namespace engine {

// Stufe 3: sortierter Vektor von PriceLevel* (bestes Level am Ende), Level-Objekte aus einem Pool.
// STUB: delegiert vorerst an MapLevels.
template <typename Compare>
class SortedVectorLevels : public MapLevels<Compare> {
public:
    using MapLevels<Compare>::MapLevels;
};

}  // namespace engine
