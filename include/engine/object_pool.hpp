#pragma once

#include <algorithm>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace engine {

// Pool für Objekte fester Größe mit Freiliste (NFR-6).
// - Konstruktor reserviert initial_capacity Objekte am Stück.
// - create() nimmt ein Objekt aus der Freiliste (O(1), keine Allokation); ist sie leer,
//   wächst der Pool um einen weiteren Block (growth_count() zählt das).
// - destroy() gibt das Objekt in die Freiliste zurück. Adressen bleiben stabil.
// - Der Destruktor gibt alle Blöcke frei; noch lebende Objekte werden nicht einzeln zerstört.
// T muss trivial zerstörbar sein. Nicht thread-sicher.
//
// Umsetzung: Blöcke von Slots; jeder Slot ist entweder ein lebendes T oder ein Glied der
// intrusiven Freiliste (LIFO, damit zuletzt freigegebene, cache-warme Slots zuerst
// wiederverwendet werden). Nie benutzte Slots des jüngsten Blocks werden per Bump-Zeiger
// vergeben, damit der Konstruktor den Block nicht anfassen muss. Jeder weitere Block ist so
// groß wie die bisherige Gesamtkapazität (mindestens kMinGrowth) — geometrisches Wachstum.
template <typename T>
class ObjectPool {
    static_assert(std::is_trivially_destructible_v<T>);

public:
    explicit ObjectPool(std::size_t initial_capacity) {
        if (initial_capacity > 0) {
            add_block(initial_capacity);
        }
    }
    ~ObjectPool() {
        for (const Block& b : blocks_) {
            ::operator delete(b.slots, std::align_val_t{alignof(Slot)});
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    T* create(Args&&... args) {
        Slot* s = nullptr;
        if (free_ != nullptr) {
            s = free_;
            free_ = s->next;
        } else {
            if (bump_ == bump_end_) [[unlikely]] {
                add_block(std::max(capacity_, kMinGrowth));
                ++growth_count_;
            }
            s = bump_++;
        }
        ++in_use_;
        return ::new (static_cast<void*>(s->storage)) T{std::forward<Args>(args)...};
    }

    void destroy(T* p) noexcept {
        // T ist trivial zerstörbar; der Slot wird zum Freilistenglied.
        Slot* s = ::new (static_cast<void*>(p)) Slot;
        s->next = free_;
        free_ = s;
        --in_use_;
    }

    [[nodiscard]] std::size_t in_use() const noexcept { return in_use_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t growth_count() const noexcept { return growth_count_; }

private:
    union Slot {
        Slot* next;
        alignas(T) std::byte storage[sizeof(T)];
    };
    struct Block {
        Slot* slots;
        std::size_t count;
    };

    static constexpr std::size_t kMinGrowth = 64;

    void add_block(std::size_t count) {
        blocks_.reserve(blocks_.size() + 1);  // vor der Allokation: kein Leck bei bad_alloc
        auto* slots = static_cast<Slot*>(::operator new(count * sizeof(Slot), std::align_val_t{alignof(Slot)}));
        blocks_.push_back(Block{slots, count});
        // Reste des alten Blocks gibt es hier nicht: add_block wird nur bei bump_ == bump_end_
        // (oder im Konstruktor) aufgerufen.
        bump_ = slots;
        bump_end_ = slots + count;
        capacity_ += count;
    }

    std::vector<Block> blocks_;
    Slot* free_{nullptr};
    Slot* bump_{nullptr};
    Slot* bump_end_{nullptr};
    std::size_t capacity_{0};
    std::size_t in_use_{0};
    std::size_t growth_count_{0};
};

}  // namespace engine
