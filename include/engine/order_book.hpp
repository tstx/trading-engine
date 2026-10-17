#pragma once

#include <cstddef>
#include <functional>
#include <map>

#include "engine/types.hpp"

namespace engine {

// Resting Order, Knoten einer intrusiven doppelt verketteten FIFO-Liste.
struct Order {
    OrderId id{};
    Side side{};
    Price price{};
    Quantity remaining{};  // noch offene Menge
    Quantity filled{};     // bereits gehandelte Menge
    SeqNo seq{};
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

// Orderbuch eines Instruments. Kennt kein Matching-Protokoll und keine Events;
// die MatchingEngine iteriert über die Gegenseite und ruft fill()/remove().
class OrderBook {
public:
    using Bids = std::map<Price, PriceLevel, std::greater<>>;  // begin() = höchster Bid
    using Asks = std::map<Price, PriceLevel, std::less<>>;     // begin() = niedrigster Ask

    explicit OrderBook(SymbolId symbol) : symbol_(symbol) {}

    // Hängt die Order ans Ende ihres Price Levels (legt das Level ggf. an).
    void add(Order* order);

    // Entfernt die Order aus ihrem Level; leeres Level wird gelöscht (FR-9).
    void remove(Order* order);

    // Reduziert remaining um qty (und total_quantity des Levels), erhöht filled.
    // Entfernt die Order NICHT, auch wenn remaining == 0 wird.
    void fill(Order* order, Quantity qty);

    // Älteste Order auf dem besten Level der Seite, nullptr wenn leer.
    [[nodiscard]] Order* best_bid() const noexcept;
    [[nodiscard]] Order* best_ask() const noexcept;

    // Bis zu depth Level je Seite, bester zuerst (FR-13).
    [[nodiscard]] BookSnapshot snapshot(std::size_t depth) const;

    [[nodiscard]] SymbolId symbol() const noexcept { return symbol_; }
    [[nodiscard]] const Bids& bids() const noexcept { return bids_; }
    [[nodiscard]] const Asks& asks() const noexcept { return asks_; }

private:
    SymbolId symbol_;
    Bids bids_;
    Asks asks_;
};

}  // namespace engine
