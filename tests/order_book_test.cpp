// OrderBook direkt testen (ohne MatchingEngine).

#include <gtest/gtest.h>

#include <deque>

#include "engine/order_book.hpp"
#include "engine/types.hpp"

namespace {

using engine::BookLevel;
using engine::Order;
using engine::OrderBook;
using engine::Price;
using engine::Quantity;
using engine::Side;

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

    OrderBook book{7};

private:
    std::deque<Order> storage_;
    engine::SeqNo seq_{0};
};

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

TEST_F(OrderBookTest, EmptyBook) {
    EXPECT_EQ(book.symbol(), 7U);
    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_ask(), nullptr);
    EXPECT_TRUE(book.bids().empty());
    EXPECT_TRUE(book.asks().empty());
    auto snap = book.snapshot(5);
    EXPECT_EQ(snap.symbol, 7U);
    EXPECT_TRUE(snap.bids.empty());
    EXPECT_TRUE(snap.asks.empty());
}

TEST_F(OrderBookTest, AddBidCreatesLevelOnBidSideOnly) {
    Order* o = bid(1, 10000, 50);
    EXPECT_EQ(book.best_bid(), o);
    EXPECT_EQ(book.best_ask(), nullptr);
    ASSERT_EQ(book.bids().size(), 1U);
    EXPECT_TRUE(book.asks().empty());
    const auto& level = book.bids().begin()->second;
    EXPECT_EQ(level.price, 10000);
    EXPECT_EQ(level.total_quantity, 50U);
    EXPECT_EQ(level.head, o);
    EXPECT_EQ(level.tail, o);
}

TEST_F(OrderBookTest, AddAskCreatesLevelOnAskSideOnly) {
    Order* o = ask(1, 10100, 30);
    EXPECT_EQ(book.best_ask(), o);
    EXPECT_EQ(book.best_bid(), nullptr);
    ASSERT_EQ(book.asks().size(), 1U);
    EXPECT_TRUE(book.bids().empty());
    const auto& level = book.asks().begin()->second;
    EXPECT_EQ(level.price, 10100);
    EXPECT_EQ(level.total_quantity, 30U);
}

TEST_F(OrderBookTest, BestBidIsHighestPrice) {
    bid(1, 9900, 10);
    Order* best = bid(2, 10000, 10);
    bid(3, 9800, 10);
    EXPECT_EQ(book.best_bid(), best);
    EXPECT_EQ(book.bids().size(), 3U);
}

TEST_F(OrderBookTest, BestAskIsLowestPrice) {
    ask(1, 10200, 10);
    Order* best = ask(2, 10100, 10);
    ask(3, 10300, 10);
    EXPECT_EQ(book.best_ask(), best);
    EXPECT_EQ(book.asks().size(), 3U);
}

TEST_F(OrderBookTest, FifoWithinLevel) {
    Order* a = ask(1, 10100, 50);
    Order* b = ask(2, 10100, 30);
    Order* c = ask(3, 10100, 20);
    ASSERT_EQ(book.asks().size(), 1U);
    const auto& level = book.asks().begin()->second;
    EXPECT_EQ(level.total_quantity, 100U);
    EXPECT_EQ(level.head, a);
    EXPECT_EQ(level.tail, c);
    EXPECT_EQ(a->next, b);
    EXPECT_EQ(b->next, c);
    EXPECT_EQ(book.best_ask(), a);

    book.remove(a);
    EXPECT_EQ(book.best_ask(), b);
    book.remove(b);
    EXPECT_EQ(book.best_ask(), c);
}

TEST_F(OrderBookTest, RemoveFromMiddleKeepsOrderAndTotal) {
    Order* a = bid(1, 10000, 10);
    Order* b = bid(2, 10000, 20);
    Order* c = bid(3, 10000, 30);
    book.remove(b);
    ASSERT_EQ(book.bids().size(), 1U);
    const auto& level = book.bids().begin()->second;
    EXPECT_EQ(level.total_quantity, 40U);
    EXPECT_EQ(level.head, a);
    EXPECT_EQ(level.tail, c);
    EXPECT_EQ(a->next, c);
    EXPECT_EQ(c->prev, a);
    EXPECT_EQ(book.best_bid(), a);
}

TEST_F(OrderBookTest, RemoveLastOrderDeletesLevel) {
    Order* a = ask(1, 10100, 10);
    Order* b = ask(2, 10200, 10);
    book.remove(a);
    ASSERT_EQ(book.asks().size(), 1U);
    EXPECT_EQ(book.asks().begin()->first, 10200);
    EXPECT_EQ(book.best_ask(), b);
    book.remove(b);
    EXPECT_TRUE(book.asks().empty());
    EXPECT_EQ(book.best_ask(), nullptr);
}

TEST_F(OrderBookTest, FillReducesRemainingAndLevelTotal) {
    Order* a = ask(1, 10100, 50);
    ask(2, 10100, 30);
    ASSERT_EQ(book.asks().size(), 1U);
    book.fill(a, 20);
    EXPECT_EQ(a->remaining, 30U);
    EXPECT_EQ(a->filled, 20U);
    EXPECT_EQ(book.asks().begin()->second.total_quantity, 60U);
    EXPECT_EQ(book.best_ask(), a);  // Priorität bleibt erhalten

    book.fill(a, 5);
    EXPECT_EQ(a->remaining, 25U);
    EXPECT_EQ(a->filled, 25U);
    EXPECT_EQ(book.asks().begin()->second.total_quantity, 55U);
}

TEST_F(OrderBookTest, FillToZeroDoesNotRemove) {
    Order* a = bid(1, 10000, 40);
    book.fill(a, 40);
    EXPECT_EQ(a->remaining, 0U);
    EXPECT_EQ(a->filled, 40U);
    ASSERT_EQ(book.bids().size(), 1U);
    EXPECT_EQ(book.bids().begin()->second.total_quantity, 0U);
    EXPECT_EQ(book.best_bid(), a);

    book.remove(a);
    EXPECT_TRUE(book.bids().empty());
    EXPECT_EQ(book.best_bid(), nullptr);
}

TEST_F(OrderBookTest, SnapshotSortedAndAggregated) {
    bid(1, 9900, 10);
    bid(2, 10000, 20);
    bid(3, 9900, 5);
    bid(4, 9800, 7);
    ask(5, 10200, 100);
    ask(6, 10100, 50);
    ask(7, 10100, 30);
    ask(8, 10300, 1);

    auto snap = book.snapshot(10);
    EXPECT_EQ(snap.symbol, 7U);
    expect_levels(snap.bids, {{10000, 20}, {9900, 15}, {9800, 7}});
    expect_levels(snap.asks, {{10100, 80}, {10200, 100}, {10300, 1}});
}

TEST_F(OrderBookTest, SnapshotRespectsDepth) {
    for (int i = 0; i < 7; ++i) {
        bid(static_cast<engine::OrderId>(1 + i), 10000 - i, 1);
        ask(static_cast<engine::OrderId>(100 + i), 10100 + i, 2);
    }
    auto snap = book.snapshot(5);
    expect_levels(snap.bids, {{10000, 1}, {9999, 1}, {9998, 1}, {9997, 1}, {9996, 1}});
    expect_levels(snap.asks, {{10100, 2}, {10101, 2}, {10102, 2}, {10103, 2}, {10104, 2}});

    auto top = book.snapshot(1);
    expect_levels(top.bids, {{10000, 1}});
    expect_levels(top.asks, {{10100, 2}});

    auto none = book.snapshot(0);
    EXPECT_TRUE(none.bids.empty());
    EXPECT_TRUE(none.asks.empty());
}

TEST_F(OrderBookTest, SnapshotReflectsPartialFill) {
    Order* a = ask(1, 10100, 50);
    ask(2, 10100, 30);
    book.fill(a, 50);
    book.remove(a);
    expect_levels(book.snapshot(5).asks, {{10100, 30}});
}

}  // namespace
