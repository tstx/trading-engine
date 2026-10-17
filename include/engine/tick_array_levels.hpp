#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/engine_config.hpp"
#include "engine/object_pool.hpp"
#include "engine/price_level.hpp"

namespace engine {

// Stufe 3: Array über das Preisband 1..EngineConfig::max_tick_price, Index = Preis
// (LevelStore-Vertrag siehe level_store.hpp).
// Preise außerhalb des Bands: accepts() == false (Engine rejected mit INVALID_PRICE);
// find_or_create() setzt accepts(price) voraus.
//
// Speicher: Das Array hält PriceLevel* (8 Byte/Tick, beim Default-Band 1 MiB je Seite), nicht
// PriceLevel selbst (32 Byte/Tick, 4 MiB je Seite bzw. 8 MiB je Buch). Bücher werden pro
// Instrument und in Tests sehr oft erzeugt; das Nullsetzen von 4x mehr Speicher pro Buch wäre
// spürbar. Die Level-Objekte kommen aus einem ObjectPool, ihre Adressen sind dadurch stabil.
// Der Preis des Levels liefert beim Löschen direkt den Array-Index.
//
// Bestes Level: best_ ist der Index des besten belegten Ticks (O(1) für best()). Ein neues Level
// verbessert best_ ggf. direkt. Wird das beste Level gelöscht, sucht eine zweistufige Bitmap das
// nächste belegte Level:
//   occupied_[w]  Bit b gesetzt <=> Tick 64*w+b belegt          (64 Ticks pro Wort)
//   summary_[s]   Bit b gesetzt <=> occupied_[64*s+b] != 0      (4096 Ticks pro Bit-Wort)
// Eine Suche prüft das aktuelle Wort, springt über summary_ zum nächsten nichtleeren Wort und
// nimmt dort countr_zero/countl_zero. Beim Default-Band (131072 Ticks = 2049 Wörter, 33
// Summary-Wörter) kostet selbst eine Lücke über das ganze Band höchstens ~35 Wortzugriffe statt
// 131072 Zeiger-Tests. Gleiches gilt für for_each (Snapshots) über dünn besetzte Bücher.
template <typename Compare>
class TickArrayLevels {
public:
    explicit TickArrayLevels(const EngineConfig& config)
        : max_price_(config.max_tick_price > 0 ? config.max_tick_price : 0),
          slots_(static_cast<std::size_t>(max_price_) + 1, nullptr),
          occupied_(words_for(slots_.size()), 0),
          summary_(words_for(occupied_.size()), 0),
          pool_(config.level_capacity) {}

    TickArrayLevels(const TickArrayLevels&) = delete;
    TickArrayLevels& operator=(const TickArrayLevels&) = delete;

    [[nodiscard]] bool accepts(Price price) const noexcept { return price >= 1 && price <= max_price_; }

    PriceLevel* find_or_create(Price price) {
        assert(accepts(price));
        const auto idx = static_cast<std::size_t>(price);
        PriceLevel*& slot = slots_[idx];
        if (slot != nullptr) {
            return slot;
        }
        slot = pool_.create();
        slot->price = price;
        set_bit(idx);
        if (count_ == 0 || Compare{}(price, static_cast<Price>(best_))) {
            best_ = idx;
        }
        ++count_;
        return slot;
    }

    void erase(PriceLevel* level) {
        const auto idx = static_cast<std::size_t>(level->price);
        assert(slots_[idx] == level);
        slots_[idx] = nullptr;
        clear_bit(idx);
        --count_;
        if (idx == best_) {
            best_ = count_ == 0 ? kNone : next_worse(idx);
        }
        pool_.destroy(level);
    }

    [[nodiscard]] PriceLevel* best() noexcept { return count_ == 0 ? nullptr : slots_[best_]; }
    [[nodiscard]] const PriceLevel* best() const noexcept { return count_ == 0 ? nullptr : slots_[best_]; }

    template <typename F>
    void for_each(F&& f) const {
        if (count_ == 0) {
            return;
        }
        for (std::size_t idx = best_; idx != kNone; idx = next_worse(idx)) {
            if (!f(static_cast<const PriceLevel&>(*slots_[idx]))) {
                break;
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

private:
    using Word = std::uint64_t;
    static constexpr std::size_t kBits = 64;
    static constexpr std::size_t kNone = 0;  // Tick 0 ist nie gültig
    // std::less<> -> niedrigster Preis ist der beste -> "schlechter" heißt aufwärts suchen.
    static constexpr bool kLowIsBest = Compare{}(Price{1}, Price{2});

    static constexpr std::size_t words_for(std::size_t bits) noexcept { return (bits + kBits - 1) / kBits; }

    void set_bit(std::size_t idx) noexcept {
        const std::size_t w = idx / kBits;
        occupied_[w] |= Word{1} << (idx % kBits);
        summary_[w / kBits] |= Word{1} << (w % kBits);
    }

    void clear_bit(std::size_t idx) noexcept {
        const std::size_t w = idx / kBits;
        occupied_[w] &= ~(Word{1} << (idx % kBits));
        if (occupied_[w] == 0) {
            summary_[w / kBits] &= ~(Word{1} << (w % kBits));
        }
    }

    // Nächstes belegtes Level, das schlechter als idx ist; kNone, wenn keines existiert.
    [[nodiscard]] std::size_t next_worse(std::size_t idx) const noexcept {
        if constexpr (kLowIsBest) {
            return find_at_or_above(idx + 1);
        } else {
            return idx == 0 ? kNone : find_at_or_below(idx - 1);
        }
    }

    // Kleinster belegter Index >= i, sonst kNone.
    [[nodiscard]] std::size_t find_at_or_above(std::size_t i) const noexcept {
        std::size_t w = i / kBits;
        if (w >= occupied_.size()) {
            return kNone;
        }
        const Word m = occupied_[w] & (~Word{0} << (i % kBits));
        if (m != 0) {
            return w * kBits + static_cast<std::size_t>(std::countr_zero(m));
        }
        // Nächstes nichtleeres Wort > w über die Summary-Bitmap.
        ++w;
        std::size_t s = w / kBits;
        if (s >= summary_.size()) {
            return kNone;
        }
        Word sm = summary_[s] & (~Word{0} << (w % kBits));
        while (sm == 0) {
            if (++s >= summary_.size()) {
                return kNone;
            }
            sm = summary_[s];
        }
        w = s * kBits + static_cast<std::size_t>(std::countr_zero(sm));
        return w * kBits + static_cast<std::size_t>(std::countr_zero(occupied_[w]));
    }

    // Größter belegter Index <= i, sonst kNone (Index 0 ist nie belegt).
    [[nodiscard]] std::size_t find_at_or_below(std::size_t i) const noexcept {
        std::size_t w = i / kBits;
        const Word m = occupied_[w] & (~Word{0} >> (kBits - 1 - i % kBits));
        if (m != 0) {
            return w * kBits + kBits - 1 - static_cast<std::size_t>(std::countl_zero(m));
        }
        if (w == 0) {
            return kNone;
        }
        // Nächstes nichtleeres Wort < w über die Summary-Bitmap.
        --w;
        std::size_t s = w / kBits;
        Word sm = summary_[s] & (~Word{0} >> (kBits - 1 - w % kBits));
        while (sm == 0) {
            if (s == 0) {
                return kNone;
            }
            sm = summary_[--s];
        }
        w = s * kBits + kBits - 1 - static_cast<std::size_t>(std::countl_zero(sm));
        return w * kBits + kBits - 1 - static_cast<std::size_t>(std::countl_zero(occupied_[w]));
    }

    Price max_price_;
    std::vector<PriceLevel*> slots_;  // Index = Preis, slots_[0] unbenutzt
    std::vector<Word> occupied_;
    std::vector<Word> summary_;
    ObjectPool<PriceLevel> pool_;
    std::size_t best_{kNone};
    std::size_t count_{0};
};

}  // namespace engine
