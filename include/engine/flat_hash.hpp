#pragma once

#include <cstddef>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace engine {

// Hash-Tabellen mit offener Adressierung für ganzzahlige Schlüssel (NFR-6).
// - Konstruktor reserviert Platz für expected_size Einträge (Kapazität Zweierpotenz,
//   Lastfaktor <= 0.5 o. Ä.); bis dahin keine Allokation.
// - Darüber hinaus Rehash auf doppelte Größe (rehash_count() zählt das).
// - Schlüssel: ganzzahlig. Werte: trivial kopierbar. Nicht thread-sicher.
// - Zeiger aus find() sind nur bis zur nächsten Änderung der Tabelle gültig.
//
// STUB: delegiert an std::unordered_map/set; wird in Stufe 3 ersetzt.
template <typename K, typename V>
class FlatHashMap {
    static_assert(std::is_integral_v<K>);
    static_assert(std::is_trivially_copyable_v<V>);

public:
    explicit FlatHashMap(std::size_t expected_size = 0) { map_.reserve(expected_size); }

    [[nodiscard]] V* find(K key) noexcept {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const V* find(K key) const noexcept {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] bool contains(K key) const noexcept { return map_.contains(key); }

    // false, wenn key schon vorhanden ist (Wert bleibt dann unverändert).
    bool insert(K key, V value) { return map_.emplace(key, value).second; }

    // false, wenn key nicht vorhanden war.
    bool erase(K key) noexcept { return map_.erase(key) > 0; }

    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    [[nodiscard]] bool empty() const noexcept { return map_.empty(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return map_.bucket_count(); }
    [[nodiscard]] std::size_t rehash_count() const noexcept { return 0; }

private:
    std::unordered_map<K, V> map_;
};

template <typename K>
class FlatHashSet {
    static_assert(std::is_integral_v<K>);

public:
    explicit FlatHashSet(std::size_t expected_size = 0) { set_.reserve(expected_size); }

    [[nodiscard]] bool contains(K key) const noexcept { return set_.contains(key); }
    bool insert(K key) { return set_.insert(key).second; }  // false, wenn schon vorhanden
    bool erase(K key) noexcept { return set_.erase(key) > 0; }

    [[nodiscard]] std::size_t size() const noexcept { return set_.size(); }
    [[nodiscard]] bool empty() const noexcept { return set_.empty(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return set_.bucket_count(); }
    [[nodiscard]] std::size_t rehash_count() const noexcept { return 0; }

private:
    std::unordered_set<K> set_;
};

}  // namespace engine
