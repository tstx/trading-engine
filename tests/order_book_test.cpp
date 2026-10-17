// OrderBook direkt testen (ohne MatchingEngine).

#include "engine/order_book.hpp"

#include <deque>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "engine/engine_config.hpp"
#include "engine/price_level.hpp"
#include "engine/types.hpp"

namespace {

using engine::BookLevel;
using engine::Order;
using engine::Price;
using engine::Quantity;
using engine::Side;

template <typename Book>
class OrderBookTest : public ::testing::Test {
protected:
    // Der Test besitzt die Orders (wie sonst die MatchingEngine); deque hält Adressen stabil.
    Order* make(engine::OrderId id, Side side, Price price, Quantity qty) {
        Order& o = storage_.emplace_back();
        o.id = id;
        o.side = side;
        o.price = price;
        o.remaining = qty;
        o.filled = 0;
        o.seq = ++seq_;
        return &o;
    }
    Order* bid(engine::OrderId id, Price price, Quantity qty) {
        Order* o = make(id, Side::Buy, price, qty);
        book.add(o);
        return o;
    }
    Order* ask(engine::OrderId id, Price price, Quantity qty) {
        Order* o = make(id, Side::Sell, price, qty);
        book.add(o);
        return o;
    }

    Book book{7};

private:
    std::deque<Order> storage_;
    engine::SeqNo seq_{0};
};

using BookTypes = ::testing::Types<engine::MapOrderBook, engine::VectorOrderBook, engine::TickOrderBook>;
TYPED_TEST_SUITE(OrderBookTest, BookTypes);

void expect_levels(const std::vector<BookLevel>& actual, const std::vector<BookLevel>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].price, expected[i].price) << "level " << i;
        EXPECT_EQ(actual[i].quantity, expected[i].quantity) << "level " << i;
    }
}

// ---------------------------------------------------------------------------
// PriceLevel

TEST(PriceLevelTest, PushBackAndEraseKeepFifoLinks) {
    Order a{};
    a.id = 1;
    Order b{};
    b.id = 2;
    Order c{};
    c.id = 3;
    engine::PriceLevel level{};
    EXPECT_TRUE(level.empty());

    level.push_back(&a);
    level.push_back(&b);
    level.push_back(&c);
    EXPECT_FALSE(level.empty());
    EXPECT_EQ(level.head, &a);
    EXPECT_EQ(level.tail, &c);
    EXPECT_EQ(a.next, &b);
    EXPECT_EQ(b.prev, &a);
    EXPECT_EQ(b.next, &c);
    EXPECT_EQ(c.prev, &b);
    EXPECT_EQ(a.prev, nullptr);
    EXPECT_EQ(c.next, nullptr);

    level.erase(&b);  // Mitte
    EXPECT_EQ(level.head, &a);
    EXPECT_EQ(level.tail, &c);
    EXPECT_EQ(a.next, &c);
    EXPECT_EQ(c.prev, &a);

    level.erase(&a);  // Kopf
    EXPECT_EQ(level.head, &c);
    EXPECT_EQ(level.tail, &c);
    EXPECT_EQ(c.prev, nullptr);

    level.erase(&c);  // letzte
    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.head, nullptr);
    EXPECT_EQ(level.tail, nullptr);
}

// ---------------------------------------------------------------------------
// OrderBook

TYPED_TEST(OrderBookTest, EmptyBook) {
    EXPECT_EQ(this->book.symbol(), 7U);
    EXPECT_EQ(this->book.best_bid(), nullptr);
    EXPECT_EQ(this->book.best_ask(), nullptr);
    EXPECT_TRUE(this->book.bids().empty());
    EXPECT_TRUE(this->book.asks().empty());
    auto snap = this->book.snapshot(5);
    EXPECT_EQ(snap.symbol, 7U);
    EXPECT_TRUE(snap.bids.empty());
    EXPECT_TRUE(snap.asks.empty());
}

TYPED_TEST(OrderBookTest, AddBidCreatesLevelOnBidSideOnly) {
    Order* o = this->bid(1, 10000, 50);
    EXPECT_EQ(this->book.best_bid(), o);
    EXPECT_EQ(this->book.best_ask(), nullptr);
    ASSERT_EQ(this->book.bids().size(), 1U);
    EXPECT_TRUE(this->book.asks().empty());
    const auto& level = (*this->book.bids().best());
    EXPECT_EQ(level.price, 10000);
    EXPECT_EQ(level.total_quantity, 50U);
    EXPECT_EQ(level.head, o);
    EXPECT_EQ(level.tail, o);
}

TYPED_TEST(OrderBookTest, AddAskCreatesLevelOnAskSideOnly) {
    Order* o = this->ask(1, 10100, 30);
    EXPECT_EQ(this->book.best_ask(), o);
    EXPECT_EQ(this->book.best_bid(), nullptr);
    ASSERT_EQ(this->book.asks().size(), 1U);
    EXPECT_TRUE(this->book.bids().empty());
    const auto& level = (*this->book.asks().best());
    EXPECT_EQ(level.price, 10100);
    EXPECT_EQ(level.total_quantity, 30U);
}

TYPED_TEST(OrderBookTest, BestBidIsHighestPrice) {
    this->bid(1, 9900, 10);
    Order* best = this->bid(2, 10000, 10);
    this->bid(3, 9800, 10);
    EXPECT_EQ(this->book.best_bid(), best);
    EXPECT_EQ(this->book.bids().size(), 3U);
}

TYPED_TEST(OrderBookTest, BestAskIsLowestPrice) {
    this->ask(1, 10200, 10);
    Order* best = this->ask(2, 10100, 10);
    this->ask(3, 10300, 10);
    EXPECT_EQ(this->book.best_ask(), best);
    EXPECT_EQ(this->book.asks().size(), 3U);
}

TYPED_TEST(OrderBookTest, FifoWithinLevel) {
    Order* a = this->ask(1, 10100, 50);
    Order* b = this->ask(2, 10100, 30);
    Order* c = this->ask(3, 10100, 20);
    ASSERT_EQ(this->book.asks().size(), 1U);
    const auto& level = (*this->book.asks().best());
    EXPECT_EQ(level.total_quantity, 100U);
    EXPECT_EQ(level.head, a);
    EXPECT_EQ(level.tail, c);
    EXPECT_EQ(a->next, b);
    EXPECT_EQ(b->next, c);
    EXPECT_EQ(this->book.best_ask(), a);

    this->book.remove(a);
    EXPECT_EQ(this->book.best_ask(), b);
    this->book.remove(b);
    EXPECT_EQ(this->book.best_ask(), c);
}

TYPED_TEST(OrderBookTest, RemoveFromMiddleKeepsOrderAndTotal) {
    Order* a = this->bid(1, 10000, 10);
    Order* b = this->bid(2, 10000, 20);
    Order* c = this->bid(3, 10000, 30);
    this->book.remove(b);
    ASSERT_EQ(this->book.bids().size(), 1U);
    const auto& level = (*this->book.bids().best());
    EXPECT_EQ(level.total_quantity, 40U);
    EXPECT_EQ(level.head, a);
    EXPECT_EQ(level.tail, c);
    EXPECT_EQ(a->next, c);
    EXPECT_EQ(c->prev, a);
    EXPECT_EQ(this->book.best_bid(), a);
}

TYPED_TEST(OrderBookTest, RemoveLastOrderDeletesLevel) {
    Order* a = this->ask(1, 10100, 10);
    Order* b = this->ask(2, 10200, 10);
    this->book.remove(a);
    ASSERT_EQ(this->book.asks().size(), 1U);
    EXPECT_EQ(this->book.asks().best()->price, 10200);
    EXPECT_EQ(this->book.best_ask(), b);
    this->book.remove(b);
    EXPECT_TRUE(this->book.asks().empty());
    EXPECT_EQ(this->book.best_ask(), nullptr);
}

TYPED_TEST(OrderBookTest, FillReducesRemainingAndLevelTotal) {
    Order* a = this->ask(1, 10100, 50);
    this->ask(2, 10100, 30);
    ASSERT_EQ(this->book.asks().size(), 1U);
    this->book.fill(a, 20);
    EXPECT_EQ(a->remaining, 30U);
    EXPECT_EQ(a->filled, 20U);
    EXPECT_EQ((*this->book.asks().best()).total_quantity, 60U);
    EXPECT_EQ(this->book.best_ask(), a);  // Priorität bleibt erhalten

    this->book.fill(a, 5);
    EXPECT_EQ(a->remaining, 25U);
    EXPECT_EQ(a->filled, 25U);
    EXPECT_EQ((*this->book.asks().best()).total_quantity, 55U);
}

TYPED_TEST(OrderBookTest, FillToZeroDoesNotRemove) {
    Order* a = this->bid(1, 10000, 40);
    this->book.fill(a, 40);
    EXPECT_EQ(a->remaining, 0U);
    EXPECT_EQ(a->filled, 40U);
    ASSERT_EQ(this->book.bids().size(), 1U);
    EXPECT_EQ((*this->book.bids().best()).total_quantity, 0U);
    EXPECT_EQ(this->book.best_bid(), a);

    this->book.remove(a);
    EXPECT_TRUE(this->book.bids().empty());
    EXPECT_EQ(this->book.best_bid(), nullptr);
}

TYPED_TEST(OrderBookTest, SnapshotSortedAndAggregated) {
    this->bid(1, 9900, 10);
    this->bid(2, 10000, 20);
    this->bid(3, 9900, 5);
    this->bid(4, 9800, 7);
    this->ask(5, 10200, 100);
    this->ask(6, 10100, 50);
    this->ask(7, 10100, 30);
    this->ask(8, 10300, 1);

    auto snap = this->book.snapshot(10);
    EXPECT_EQ(snap.symbol, 7U);
    expect_levels(snap.bids, {{10000, 20}, {9900, 15}, {9800, 7}});
    expect_levels(snap.asks, {{10100, 80}, {10200, 100}, {10300, 1}});
}

TYPED_TEST(OrderBookTest, SnapshotRespectsDepth) {
    for (int i = 0; i < 7; ++i) {
        this->bid(static_cast<engine::OrderId>(1 + i), 10000 - i, 1);
        this->ask(static_cast<engine::OrderId>(100 + i), 10100 + i, 2);
    }
    auto snap = this->book.snapshot(5);
    expect_levels(snap.bids, {{10000, 1}, {9999, 1}, {9998, 1}, {9997, 1}, {9996, 1}});
    expect_levels(snap.asks, {{10100, 2}, {10101, 2}, {10102, 2}, {10103, 2}, {10104, 2}});

    auto top = this->book.snapshot(1);
    expect_levels(top.bids, {{10000, 1}});
    expect_levels(top.asks, {{10100, 2}});

    auto none = this->book.snapshot(0);
    EXPECT_TRUE(none.bids.empty());
    EXPECT_TRUE(none.asks.empty());
}

TYPED_TEST(OrderBookTest, SnapshotReflectsPartialFill) {
    Order* a = this->ask(1, 10100, 50);
    this->ask(2, 10100, 30);
    this->book.fill(a, 50);
    this->book.remove(a);
    expect_levels(this->book.snapshot(5).asks, {{10100, 30}});
}

TYPED_TEST(OrderBookTest, AcceptsPricesInsideBand) {
    const engine::EngineConfig config{};
    EXPECT_TRUE(this->book.accepts_price(1));
    EXPECT_TRUE(this->book.accepts_price(10000));
    EXPECT_TRUE(this->book.accepts_price(config.max_tick_price));
    if constexpr (std::is_same_v<TypeParam, engine::TickOrderBook>) {
        EXPECT_FALSE(this->book.accepts_price(0));
        EXPECT_FALSE(this->book.accepts_price(-5));
        EXPECT_FALSE(this->book.accepts_price(config.max_tick_price + 1));
    } else {
        EXPECT_TRUE(this->book.accepts_price(config.max_tick_price + 1));
    }
}

TYPED_TEST(OrderBookTest, LevelAddressStableWhileOtherLevelsChange) {
    engine::Order* anchor = this->bid(1, 5000, 10);
    const engine::PriceLevel* level = anchor->level;
    for (int i = 0; i < 300; ++i) {
        this->bid(static_cast<engine::OrderId>(100 + i), 5001 + i, 1);
        this->bid(static_cast<engine::OrderId>(1000 + i), 4999 - i, 1);
    }
    EXPECT_EQ(anchor->level, level);
    EXPECT_EQ(level->price, 5000);
    EXPECT_EQ(level->head, anchor);
    EXPECT_EQ(this->book.bids().size(), 601U);
}

}  // namespace
