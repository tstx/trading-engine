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

template <typename Store>
void add_to(Store& levels, Order* order) {
    PriceLevel* level = levels.find_or_create(order->price);
    level->push_back(order);
    order->level = level;
}

template <typename Store>
void remove_from(Store& levels, Order* order) {
    PriceLevel* level = order->level;
    if (level == nullptr) {
        return;
    }
    level->erase(order);
    order->level = nullptr;
    if (level->empty()) {
        levels.erase(level);
    }
}

template <typename Store>
void collect(const Store& levels, std::size_t depth, std::vector<BookLevel>& out) {
    out.reserve(std::min(depth, levels.size()));
    levels.for_each([&](const PriceLevel& level) {
        if (out.size() >= depth) {
            return false;
        }
        out.push_back(BookLevel{level.price, level.total_quantity});
        return true;
    });
}

}  // namespace

template <template <typename> class LevelStore>
BasicOrderBook<LevelStore>::BasicOrderBook(SymbolId symbol, const EngineConfig& config)
    : symbol_(symbol), bids_(config), asks_(config) {}

template <template <typename> class LevelStore>
bool BasicOrderBook<LevelStore>::accepts_price(Price price) const noexcept {
    return bids_.accepts(price) && asks_.accepts(price);
}

template <template <typename> class LevelStore>
void BasicOrderBook<LevelStore>::add(Order* order) {
    if (order->side == Side::Buy) {
        add_to(bids_, order);
    } else {
        add_to(asks_, order);
    }
}

template <template <typename> class LevelStore>
void BasicOrderBook<LevelStore>::remove(Order* order) {
    if (order->side == Side::Buy) {
        remove_from(bids_, order);
    } else {
        remove_from(asks_, order);
    }
}

template <template <typename> class LevelStore>
void BasicOrderBook<LevelStore>::fill(Order* order, Quantity qty) {
    order->remaining -= qty;
    order->filled += qty;
    if (order->level != nullptr) {
        order->level->total_quantity -= qty;
    }
}

template <template <typename> class LevelStore>
void BasicOrderBook<LevelStore>::reduce(Order* order, Quantity new_remaining) {
    if (order->level != nullptr) {
        order->level->total_quantity -= order->remaining - new_remaining;
    }
    order->remaining = new_remaining;
}

template <template <typename> class LevelStore>
Order* BasicOrderBook<LevelStore>::best_bid() const noexcept {
    const PriceLevel* level = bids_.best();
    return level == nullptr ? nullptr : level->head;
}

template <template <typename> class LevelStore>
Order* BasicOrderBook<LevelStore>::best_ask() const noexcept {
    const PriceLevel* level = asks_.best();
    return level == nullptr ? nullptr : level->head;
}

template <template <typename> class LevelStore>
BookSnapshot BasicOrderBook<LevelStore>::snapshot(std::size_t depth) const {
    BookSnapshot snap{symbol_, {}, {}};
    collect(bids_, depth, snap.bids);
    collect(asks_, depth, snap.asks);
    return snap;
}

template class BasicOrderBook<MapLevels>;
template class BasicOrderBook<SortedVectorLevels>;
template class BasicOrderBook<TickArrayLevels>;

}  // namespace engine
