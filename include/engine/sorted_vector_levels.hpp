#pragma once

#include <cstddef>
#include <vector>

#include "engine/engine_config.hpp"
#include "engine/object_pool.hpp"
#include "engine/price_level.hpp"

namespace engine {

// Stufe 3: sortierter Vektor von PriceLevel* (LevelStore-Vertrag siehe level_store.hpp).
//
// Layout: levels_ ist vom SCHLECHTESTEN zum BESTEN Level sortiert, das beste Level liegt am Ende.
// Damit ist best() ein back(), und das häufigste Strukturereignis im Matching – das beste Level
// wird leergehandelt – ein pop_back() ohne Verschieben. Neue Levels entstehen meist nahe am Top
// des Buchs, d. h. nahe am Vektorende; vector::insert verschiebt dann nur wenige Zeiger.
//
// Die PriceLevel-Objekte selbst liegen in einem ObjectPool, der Vektor hält nur Zeiger. So bleiben
// die Adressen stabil (Order::level), auch wenn der Vektor Elemente verschiebt oder reallokiert.
//
// Suche: Zuerst linear vom Ende aus über höchstens kLinearProbe Einträge, danach binäre Suche über
// den Rest. Begründung: Orders treffen in der Praxis fast immer die obersten paar Levels; dort ist
// ein linearer Scan über wenige, zusammenhängend liegende Zeiger (eine Cache-Zeile Zeiger, aber je
// Eintrag eine Dereferenzierung) billiger und besser vorhersagbar als binäre Suche mit schwer
// vorhersagbaren Sprüngen. Für tiefe Preise bleibt es dank binärer Suche bei O(log n).
template <typename Compare>
class SortedVectorLevels {
public:
    explicit SortedVectorLevels(const EngineConfig& config) : pool_(config.level_capacity) {
        levels_.reserve(config.level_capacity);
    }

    SortedVectorLevels(const SortedVectorLevels&) = delete;
    SortedVectorLevels& operator=(const SortedVectorLevels&) = delete;

    [[nodiscard]] bool accepts(Price /*price*/) const noexcept { return true; }

    PriceLevel* find_or_create(Price price) {
        const std::size_t pos = lower_position(price);
        if (pos < levels_.size() && levels_[pos]->price == price) {
            return levels_[pos];
        }
        PriceLevel* level = pool_.create();
        level->price = price;
        levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(pos), level);
        return level;
    }

    void erase(PriceLevel* level) {
        if (!levels_.empty() && levels_.back() == level) {
            levels_.pop_back();  // häufigster Fall: bestes Level leergehandelt
        } else {
            const std::size_t pos = lower_position(level->price);
            levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(pos));
        }
        pool_.destroy(level);
    }

    [[nodiscard]] PriceLevel* best() noexcept { return levels_.empty() ? nullptr : levels_.back(); }
    [[nodiscard]] const PriceLevel* best() const noexcept { return levels_.empty() ? nullptr : levels_.back(); }

    template <typename F>
    void for_each(F&& f) const {
        for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) {
            if (!f(static_cast<const PriceLevel&>(**it))) {
                break;
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }
    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }

private:
    static constexpr std::size_t kLinearProbe = 8;

    // Kleinster Index pos, so dass levels_[pos] nicht schlechter als price ist (d. h. alle
    // Einträge davor sind echt schlechter). Existiert price, ist levels_[pos]->price == price;
    // sonst ist pos die Einfügeposition.
    [[nodiscard]] std::size_t lower_position(Price price) const noexcept {
        const Compare better{};
        std::size_t hi = levels_.size();  // alle Einträge ab hi sind nicht schlechter als price
        const std::size_t stop = hi > kLinearProbe ? hi - kLinearProbe : 0;
        while (hi > stop) {
            if (better(price, levels_[hi - 1]->price)) {
                return hi;  // levels_[hi-1] ist echt schlechter
            }
            --hi;
        }
        // Binäre Suche über [0, hi): erster Eintrag, der nicht echt schlechter als price ist.
        std::size_t lo = 0;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (better(price, levels_[mid]->price)) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    ObjectPool<PriceLevel> pool_;
    std::vector<PriceLevel*> levels_;  // schlechtestes zuerst, bestes am Ende
};

}  // namespace engine
