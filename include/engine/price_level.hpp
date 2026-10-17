#pragma once

#include "engine/types.hpp"

namespace engine {

struct PriceLevel;

// Resting Order, Knoten einer intrusiven doppelt verketteten FIFO-Liste.
struct Order {
    OrderId id{};
    SymbolId symbol{};
    Side side{};
    Price price{};
    Quantity remaining{};  // noch offene Menge
    Quantity filled{};     // bereits gehandelte Menge
    SeqNo seq{};
    PriceLevel* level{nullptr};  // Level, in dem die Order liegt (nullptr = nicht im Buch)
    Order* prev{nullptr};
    Order* next{nullptr};
};

// Alle Orders einer Seite zum selben Preis, in Ankunftsreihenfolge.
// Die Liste besitzt die Orders nicht (Ownership liegt bei der MatchingEngine).
struct PriceLevel {
    Price price{};
    Quantity total_quantity{};  // Summe von remaining aller Orders
    Order* head{nullptr};       // älteste Order
    Order* tail{nullptr};       // jüngste Order

    void push_back(Order* order) noexcept;  // O(1)
    void erase(Order* order) noexcept;      // O(1), auch aus der Mitte
    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
};

}  // namespace engine
