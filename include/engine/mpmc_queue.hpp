#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine {

// Lock-free, bounded FIFO für mehrere Producer und mehrere Consumer.
// Nach Dmitry Vyukovs bounded MPMC queue; übernommen aus ~/dev/tst/ringbuffer_mpmc.hpp.
// In der Pipeline als MPSC genutzt (ein Consumer: der Engine-Thread).
// Pro Producer bleibt die Reihenfolge erhalten (Positionen werden monoton vergeben).
//
// Speicher ist ein festes std::array (keine Allokation) — große Instanzen auf dem Heap anlegen.
template <typename T, std::size_t N>
class MpmcQueue {
    // Zweierpotenz, damit pos % N zu pos & kMask wird.
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of 2");
    static constexpr std::size_t kMask = N - 1;
    static constexpr std::size_t kCacheLine = 64;

    struct Cell {
        std::atomic<std::size_t> seq;  // == pos: frei zum Schreiben, == pos + 1: bereit zum Lesen
        T data;
    };

public:
    MpmcQueue() {
        // Slot i ist anfangs "frei für Position i".
        for (std::size_t i = 0; i < N; ++i) {
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    // Beliebiger Thread. false, wenn die Queue voll ist.
    bool push(const T& value) {
        Cell* cell = nullptr;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &cells_[pos & kMask];

            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

            if (diff == 0) {
                // Slot ist frei — Position beanspruchen.
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // voll
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        cell->data = value;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Beliebiger Thread. nullopt, wenn die Queue leer ist.
    std::optional<T> pop() {
        Cell* cell = nullptr;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &cells_[pos & kMask];

            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);

            if (diff == 0) {
                // Daten sind bereit — Position beanspruchen.
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return std::nullopt;  // leer
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        T value = std::move(cell->data);

        // Slot für die nächste Runde freigeben.
        cell->seq.store(pos + N, std::memory_order_release);
        return value;
    }

    static constexpr std::size_t capacity() { return N; }

private:
    std::array<Cell, N> cells_;  // fester Speicher, keine Allokation

    // Getrennte Cache-Lines gegen False Sharing.
    alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};  // nächste Schreibposition
    alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_{0};  // nächste Leseposition
};

}  // namespace engine
