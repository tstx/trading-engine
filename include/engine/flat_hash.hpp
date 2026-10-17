#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>

namespace engine {

namespace detail {

// Heimat-Slot von key in einer Tabelle mit 2^(64 - shift) Slots. Fibonacci-Hashing nach einer
// Faltung der oberen Hälfte: fortlaufende IDs landen gut verstreut, die oberen Produktbits
// hängen von allen Schlüsselbits ab. (Frei stehend, damit Tests Kollisionen konstruieren können.)
template <typename K>
[[nodiscard]] constexpr std::size_t flat_hash_home(K key, unsigned shift) noexcept {
    auto x = static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<K>>(key));
    x ^= x >> 32;
    return static_cast<std::size_t>((x * 0x9E3779B97F4A7C15ULL) >> shift);
}

// Gemeinsamer Kern von FlatHashMap/FlatHashSet: offene Adressierung, lineares Sondieren,
// Kapazität Zweierpotenz, Lastfaktor <= 0.5, Löschen per Backward-Shift (keine Tombstones).
// Leere Slots tragen den Schlüssel kEmpty (= numeric_limits<K>::max()); dieser Schlüssel
// selbst wird außerhalb der Tabelle gehalten, sodass jeder Schlüssel zulässig ist.
// Slot muss ein Aggregat mit erstem Member `K key` sein.
template <typename K, typename Slot>
class FlatTable {
public:
    static constexpr K kEmpty = std::numeric_limits<K>::max();

    explicit FlatTable(std::size_t expected_size) {
        // expected_size Einträge passen ohne Rehash: 2 * expected_size <= Kapazität.
        std::size_t cap = kMinCapacity;
        if (expected_size > kMinCapacity / 2) {
            cap = std::bit_ceil(expected_size * 2);
        }
        allocate(cap);
    }

    FlatTable(const FlatTable&) = delete;
    FlatTable& operator=(const FlatTable&) = delete;

    [[nodiscard]] Slot* find(K key) noexcept {
        if (key == kEmpty) [[unlikely]] {
            return has_empty_key_ ? &empty_key_slot_ : nullptr;
        }
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key) {
                return &s;
            }
            if (s.key == kEmpty) {
                return nullptr;
            }
        }
    }
    [[nodiscard]] const Slot* find(K key) const noexcept { return const_cast<FlatTable*>(this)->find(key); }

    // Liefert den Slot für key; inserted == false, wenn key schon vorhanden war.
    // Bei einem neuen Slot ist nur key gesetzt; der Aufrufer füllt den Rest.
    Slot* insert(K key, bool& inserted) {
        if (key == kEmpty) [[unlikely]] {
            inserted = !has_empty_key_;
            if (inserted) {
                has_empty_key_ = true;
                empty_key_slot_ = Slot{};
                empty_key_slot_.key = key;
            }
            return &empty_key_slot_;
        }
        std::size_t i = home(key);
        for (;; i = (i + 1) & mask_) {
            const K k = slots_[i].key;
            if (k == key) {
                inserted = false;
                return &slots_[i];
            }
            if (k == kEmpty) {
                break;
            }
        }
        if ((table_size_ + 1) * 2 > capacity_) [[unlikely]] {
            grow();
            i = find_free(key);
        }
        inserted = true;
        ++table_size_;
        slots_[i].key = key;
        return &slots_[i];
    }

    bool erase(K key) noexcept {
        if (key == kEmpty) [[unlikely]] {
            const bool had = has_empty_key_;
            has_empty_key_ = false;
            return had;
        }
        std::size_t hole = home(key);
        for (;; hole = (hole + 1) & mask_) {
            const K k = slots_[hole].key;
            if (k == key) {
                break;
            }
            if (k == kEmpty) {
                return false;
            }
        }
        // Backward-Shift: Nachfolger der Kette rücken in das Loch, sofern ihr Heimat-Slot
        // nicht zyklisch zwischen Loch und aktueller Position liegt.
        for (std::size_t j = (hole + 1) & mask_;; j = (j + 1) & mask_) {
            const K k = slots_[j].key;
            if (k == kEmpty) {
                break;
            }
            const std::size_t h = home(k);
            if (((j - h) & mask_) >= ((j - hole) & mask_)) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].key = kEmpty;
        --table_size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return table_size_ + (has_empty_key_ ? 1 : 0); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t rehash_count() const noexcept { return rehash_count_; }

private:
    static constexpr std::size_t kMinCapacity = 16;

    [[nodiscard]] std::size_t home(K key) const noexcept { return flat_hash_home(key, shift_); }

    [[nodiscard]] std::size_t find_free(K key) const noexcept {
        std::size_t i = home(key);
        while (slots_[i].key != kEmpty) {
            i = (i + 1) & mask_;
        }
        return i;
    }

    void allocate(std::size_t cap) {
        slots_ = std::make_unique_for_overwrite<Slot[]>(cap);
        for (std::size_t i = 0; i < cap; ++i) {
            slots_[i].key = kEmpty;
        }
        capacity_ = cap;
        mask_ = cap - 1;
        shift_ = 64 - static_cast<unsigned>(std::countr_zero(cap));
    }

    void grow() {
        std::unique_ptr<Slot[]> old = std::move(slots_);
        const std::size_t old_cap = capacity_;
        allocate(old_cap * 2);
        for (std::size_t i = 0; i < old_cap; ++i) {
            if (old[i].key != kEmpty) {
                slots_[find_free(old[i].key)] = old[i];
            }
        }
        ++rehash_count_;
    }

    std::unique_ptr<Slot[]> slots_;
    std::size_t capacity_{0};
    std::size_t mask_{0};
    unsigned shift_{0};
    std::size_t table_size_{0};  // Einträge in slots_ (ohne kEmpty-Schlüssel)
    std::size_t rehash_count_{0};
    Slot empty_key_slot_{};
    bool has_empty_key_{false};
};

template <typename K, typename V>
struct MapSlot {
    K key;
    V value;
};

template <typename K>
struct SetSlot {
    K key;
};

}  // namespace detail

// Hash-Tabellen mit offener Adressierung für ganzzahlige Schlüssel (NFR-6).
// - Konstruktor reserviert Platz für expected_size Einträge (Kapazität Zweierpotenz,
//   Lastfaktor <= 0.5 o. Ä.); bis dahin keine Allokation.
// - Darüber hinaus Rehash auf doppelte Größe (rehash_count() zählt das).
// - Schlüssel: ganzzahlig. Werte: trivial kopierbar. Nicht thread-sicher.
// - Zeiger aus find() sind nur bis zur nächsten Änderung der Tabelle gültig.
template <typename K, typename V>
class FlatHashMap {
    static_assert(std::is_integral_v<K>);
    static_assert(std::is_trivially_copyable_v<V>);

public:
    explicit FlatHashMap(std::size_t expected_size = 0) : table_(expected_size) {}

    [[nodiscard]] V* find(K key) noexcept {
        auto* s = table_.find(key);
        return s == nullptr ? nullptr : &s->value;
    }
    [[nodiscard]] const V* find(K key) const noexcept {
        const auto* s = table_.find(key);
        return s == nullptr ? nullptr : &s->value;
    }
    [[nodiscard]] bool contains(K key) const noexcept { return table_.find(key) != nullptr; }

    // false, wenn key schon vorhanden ist (Wert bleibt dann unverändert).
    bool insert(K key, V value) {
        bool inserted = false;
        auto* s = table_.insert(key, inserted);
        if (inserted) {
            s->value = value;
        }
        return inserted;
    }

    // false, wenn key nicht vorhanden war.
    bool erase(K key) noexcept { return table_.erase(key); }

    [[nodiscard]] std::size_t size() const noexcept { return table_.size(); }
    [[nodiscard]] bool empty() const noexcept { return table_.size() == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return table_.capacity(); }
    [[nodiscard]] std::size_t rehash_count() const noexcept { return table_.rehash_count(); }

private:
    detail::FlatTable<K, detail::MapSlot<K, V>> table_;
};

template <typename K>
class FlatHashSet {
    static_assert(std::is_integral_v<K>);

public:
    explicit FlatHashSet(std::size_t expected_size = 0) : table_(expected_size) {}

    [[nodiscard]] bool contains(K key) const noexcept { return table_.find(key) != nullptr; }
    bool insert(K key) {  // false, wenn schon vorhanden
        bool inserted = false;
        table_.insert(key, inserted);
        return inserted;
    }
    bool erase(K key) noexcept { return table_.erase(key); }

    [[nodiscard]] std::size_t size() const noexcept { return table_.size(); }
    [[nodiscard]] bool empty() const noexcept { return table_.size() == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return table_.capacity(); }
    [[nodiscard]] std::size_t rehash_count() const noexcept { return table_.rehash_count(); }

private:
    detail::FlatTable<K, detail::SetSlot<K>> table_;
};

}  // namespace engine
