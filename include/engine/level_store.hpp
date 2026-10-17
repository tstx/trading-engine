#pragma once

#include <cstddef>
#include <map>

#include "engine/engine_config.hpp"
#include "engine/price_level.hpp"

namespace engine {

// LevelStore: Container der Price Levels EINER Buchseite, sortiert nach Priorität.
// Compare = std::greater<> (Bids, höchster Preis zuerst) bzw. std::less<> (Asks).
// Jede Implementierung bietet:
//   explicit Store(const EngineConfig&)
//   bool accepts(Price) const noexcept          Preis darstellbar? (Tick-Array: Preisband)
//   PriceLevel* find_or_create(Price)           legt ein leeres Level an, falls nötig
//   void erase(PriceLevel*)                     entfernt ein (leeres) Level
//   PriceLevel* best() / const PriceLevel* best() const   nullptr wenn leer
//   template <class F> void for_each(F&& f) const          bester zuerst; f(const PriceLevel&) -> bool
//                                                           (false bricht ab)
//   std::size_t size() const noexcept / bool empty() const noexcept
// Garantie: Die Adresse eines PriceLevel bleibt stabil, solange das Level existiert
// (Order::level zeigt darauf).

// Referenz-Implementierung (Stufe 1): std::map, Knoten-basiert.
template <typename Compare>
class MapLevels {
public:
    explicit MapLevels(const EngineConfig& /*config*/) {}

    [[nodiscard]] bool accepts(Price /*price*/) const noexcept { return true; }

    PriceLevel* find_or_create(Price price) {
        auto [it, inserted] = levels_.try_emplace(price);
        if (inserted) {
            it->second.price = price;
        }
        return &it->second;
    }

    void erase(PriceLevel* level) { levels_.erase(level->price); }

    [[nodiscard]] PriceLevel* best() noexcept { return levels_.empty() ? nullptr : &levels_.begin()->second; }
    [[nodiscard]] const PriceLevel* best() const noexcept {
        return levels_.empty() ? nullptr : &levels_.begin()->second;
    }

    template <typename F>
    void for_each(F&& f) const {
        for (const auto& [price, level] : levels_) {
            if (!f(level)) {
                break;
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }
    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }

private:
    std::map<Price, PriceLevel, Compare> levels_;
};

}  // namespace engine
