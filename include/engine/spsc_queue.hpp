#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

namespace engine {

// Lock-free, bounded FIFO für genau einen Producer- und einen Consumer-Thread.
// Übernommen aus ~/dev/tst/ringbuffer_spsc.hpp.
//
// Speicher ist ein festes std::array (keine Allokation) — große Instanzen auf dem Heap anlegen.
template <typename T, std::size_t N>
class SpscQueue {
    static_assert(N > 0, "capacity must be > 0");
    static constexpr std::size_t kSlots = N + 1;  // ein Slot bleibt immer leer
    static constexpr std::size_t kCacheLine = 64;

public:
    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Nur Producer-Thread. false, wenn die Queue voll ist.
    bool push(const T& value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = increment(head);

        if (next == tail_.load(std::memory_order_acquire))
            return false;  // voll

        buffer_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Nur Consumer-Thread. nullopt, wenn die Queue leer ist.
    std::optional<T> pop() {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt;  // leer

        T value = std::move(buffer_[tail]);
        tail_.store(increment(tail), std::memory_order_release);
        return value;
    }

    // Momentaufnahmen — können veraltet sein, solange der andere Thread aktiv ist.
    [[nodiscard]] bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool full() const {
        return increment(head_.load(std::memory_order_acquire)) == tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() { return N; }

    // Nur Consumer-Thread. Verwirft alle aktuell enthaltenen Elemente.
    void clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    static constexpr std::size_t increment(std::size_t i) { return (i + 1) % kSlots; }  // Wrap-around

    std::array<T, kSlots> buffer_{};  // fester Speicher, keine Allokation

    // Getrennte Cache-Lines gegen False Sharing.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // nächste Schreibposition
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // nächste Leseposition
};

}  // namespace engine
