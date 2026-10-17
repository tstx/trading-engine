#include "engine/order_book.hpp"

namespace engine {

void PriceLevel::push_back(Order* /*order*/) noexcept {}
void PriceLevel::erase(Order* /*order*/) noexcept {}

void OrderBook::add(Order* /*order*/) {}
void OrderBook::remove(Order* /*order*/) {}
void OrderBook::fill(Order* /*order*/, Quantity /*qty*/) {}
Order* OrderBook::best_bid() const noexcept { return nullptr; }
Order* OrderBook::best_ask() const noexcept { return nullptr; }
BookSnapshot OrderBook::snapshot(std::size_t /*depth*/) const { return BookSnapshot{symbol_, {}, {}}; }

}  // namespace engine
