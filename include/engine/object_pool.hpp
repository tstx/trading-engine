#pragma once

#include <cstddef>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace engine {

// Pool für Objekte fester Größe mit Freiliste (NFR-6).
// - Konstruktor reserviert initial_capacity Objekte am Stück.
// - create() nimmt ein Objekt aus der Freiliste (O(1), keine Allokation); ist sie leer,
//   wächst der Pool um einen weiteren Block (growth_count() zählt das).
// - destroy() gibt das Objekt in die Freiliste zurück. Adressen bleiben stabil.
// - Der Destruktor gibt alle Blöcke frei; noch lebende Objekte werden nicht einzeln zerstört.
// T muss trivial zerstörbar sein. Nicht thread-sicher.
//
// STUB: new/delete pro Objekt; wird in Stufe 3 durch einen echten Pool ersetzt.
template <typename T>
class ObjectPool {
    static_assert(std::is_trivially_destructible_v<T>);

public:
    explicit ObjectPool(std::size_t initial_capacity) : capacity_(initial_capacity) {}
    ~ObjectPool() {
        for (T* p : live_) {
            delete p;
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    T* create(Args&&... args) {
        T* p = new T{std::forward<Args>(args)...};
        live_.insert(p);
        if (live_.size() > capacity_) {
            capacity_ = live_.size();
            ++growth_count_;
        }
        return p;
    }

    void destroy(T* p) noexcept {
        live_.erase(p);
        delete p;
    }

    [[nodiscard]] std::size_t in_use() const noexcept { return live_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t growth_count() const noexcept { return growth_count_; }

private:
    std::unordered_set<T*> live_;
    std::size_t capacity_;
    std::size_t growth_count_{0};
};

}  // namespace engine
