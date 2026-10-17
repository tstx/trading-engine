#include "engine/order_book.hpp"

#include <algorithm>
#include <vector>

namespace engine {

void PriceLevel::push_back(Order* order) noexcept {
    order->prev = tail;
    order->next = nullptr;
    if (tail != nullptr) {
        tail->next = order;
    } else {
        head = order;
    }
    tail = order;
    total_quantity += order->remaining;
}

void PriceLevel::erase(Order* order) noexcept {
    if (order->prev != nullptr) {
        order->prev->next = order->next;
    } else {
        head = order->next;
    }
    if (order->next != nullptr) {
        order->next->prev = order->prev;
    } else {
        tail = order->prev;
    }
    order->prev = nullptr;
    order->next = nullptr;
    total_quantity -= order->remaining;
}

namespace {

template <typename SideMap>
void add_to(SideMap& levels, Order* order) {
    auto [it, inserted] = levels.try_emplace(order->price);
    if (inserted) {
        it->second.price = order->price;
    }
    it->second.push_back(order);
}

template <typename SideMap>
void remove_from(SideMap& levels, Order* order) {
    auto it = levels.find(order->price);
    if (it == levels.end()) {
        return;
    }
    it->second.erase(order);
    if (it->second.empty()) {
        levels.erase(it);
    }
}

template <typename SideMap>
Order* best_of(const SideMap& levels) noexcept {
    return levels.empty() ? nullptr : levels.begin()->second.head;
}

template <typename SideMap>
void collect(const SideMap& levels, std::size_t depth, std::vector<BookLevel>& out) {
    out.reserve(std::min(depth, levels.size()));
    for (const auto& [price, level] : levels) {
        if (out.size() >= depth) {
            break;
        }
        out.push_back(BookLevel{price, level.total_quantity});
    }
}

}  // namespace

void OrderBook::add(Order* order) {
    if (order->side == Side::Buy) {
        add_to(bids_, order);
    } else {
        add_to(asks_, order);
    }
}

void OrderBook::remove(Order* order) {
    if (order->side == Side::Buy) {
        remove_from(bids_, order);
    } else {
        remove_from(asks_, order);
    }
}

void OrderBook::fill(Order* order, Quantity qty) {
    PriceLevel* level = nullptr;
    if (order->side == Side::Buy) {
        auto it = bids_.find(order->price);
        if (it != bids_.end()) {
            level = &it->second;
        }
    } else {
        auto it = asks_.find(order->price);
        if (it != asks_.end()) {
            level = &it->second;
        }
    }
    order->remaining -= qty;
    order->filled += qty;
    if (level != nullptr) {
        level->total_quantity -= qty;
    }
}

Order* OrderBook::best_bid() const noexcept { return best_of(bids_); }
Order* OrderBook::best_ask() const noexcept { return best_of(asks_); }

BookSnapshot OrderBook::snapshot(std::size_t depth) const {
    BookSnapshot snap{symbol_, {}, {}};
    collect(bids_, depth, snap.bids);
    collect(asks_, depth, snap.asks);
    return snap;
}

}  // namespace engine
