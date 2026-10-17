// Pipeline (Stufe 2): Ergebnis identisch zur direkten MatchingEngine, Lebenszyklus, Backpressure.
// Der Mehr-Producer-Stresstest liegt in einer eigenen Datei.

#include "engine/pipeline.hpp"

#include <cstddef>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "engine/event.hpp"
#include "engine/matching_engine.hpp"
#include "engine/symbol_table.hpp"
#include "test_sink.hpp"

namespace engine::test {
namespace {

// Zeichnet alle Events (inkl. BOOK) als Text auf und merkt sich, aus welchen Threads gerufen wurde.
// Die SymbolTable wird während der Pipeline-Laufzeit nicht verändert.
class CollectingSink final : public EventSink {
public:
    explicit CollectingSink(const SymbolTable& symbols) : recorder_(symbols), symbols_(symbols) {}

    void on_ack(OrderId id, SeqNo seq) override {
        record([&] { recorder_.on_ack(id, seq); });
    }
    void on_reject(OrderId id, RejectReason reason) override {
        record([&] { recorder_.on_reject(id, reason); });
    }
    void on_trade(const Trade& trade) override {
        record([&] { recorder_.on_trade(trade); });
    }
    void on_fill(OrderId id, Quantity filled, Quantity remaining) override {
        record([&] { recorder_.on_fill(id, filled, remaining); });
    }
    void on_cancelled(OrderId id, Quantity remaining, CancelReason reason) override {
        record([&] { recorder_.on_cancelled(id, remaining, reason); });
    }
    void on_book(const BookSnapshot& snapshot) override {
        record([&] { recorder_.events.push_back(book_line(symbols_, snapshot)); });
    }

    [[nodiscard]] const Events& events() const { return recorder_.events; }
    std::set<std::thread::id> threads;

private:
    template <typename F>
    void record(F&& f) {
        const std::thread::id self = std::this_thread::get_id();
        if (threads.empty() || self != last_thread_) {
            threads.insert(self);
            last_thread_ = self;
        }
        f();
    }

    RecordingSink recorder_;
    const SymbolTable& symbols_;
    std::thread::id last_thread_;
};

// Referenz: dieselben Requests direkt (single-threaded) gegen eine MatchingEngine.
Events run_direct(const SymbolTable& symbols, const std::vector<Request>& requests) {
    CollectingSink sink(symbols);
    MatchingEngine engine(sink);
    for (const Request& request : requests) {
        if (const auto* cmd = std::get_if<Command>(&request)) {
            engine.process(*cmd);
        } else if (const auto* add = std::get_if<AddSymbolRequest>(&request)) {
            engine.add_symbol(add->symbol);
        } else {
            const auto& book = std::get<BookRequest>(request);
            const std::size_t depth = book.depth < kMaxBookDepth ? book.depth : kMaxBookDepth;
            sink.on_book(engine.snapshot(book.symbol, depth));
        }
    }
    return sink.events();
}

Events run_pipeline(const SymbolTable& symbols, const std::vector<Request>& requests) {
    CollectingSink sink(symbols);
    {
        Pipeline pipeline(sink);
        pipeline.start();
        for (const Request& request : requests) {
            pipeline.submit(request);
        }
        pipeline.stop();
    }
    // output wird nur vom Logger-Thread aufgerufen, nie vom Producer.
    EXPECT_LE(sink.threads.size(), 1U);
    EXPECT_EQ(sink.threads.count(std::this_thread::get_id()), 0U);
    return sink.events();
}

Request limit(OrderId id, SymbolId symbol, Side side, Price price, Quantity qty) {
    return Command{NewOrder{id, symbol, side, OrderType::Limit, price, qty}};
}

class PipelineTest : public ::testing::Test {
protected:
    SymbolTable symbols;
    SymbolId acme = symbols.add("ACME");
};

TEST_F(PipelineTest, ReproducesExample42IncludingBook) {
    const std::vector<Request> requests = {
        AddSymbolRequest{acme},
        limit(1, acme, Side::Sell, 10100, 50),
        limit(2, acme, Side::Sell, 10100, 30),
        limit(3, acme, Side::Sell, 10200, 100),
        limit(4, acme, Side::Buy, 10150, 70),
        BookRequest{acme, 5},
    };
    const Events expected = {
        "ACK,1,1",
        "ACK,2,2",
        "ACK,3,3",
        "ACK,4,4",
        "TRADE,ACME,10100,50,4,1,BUY",
        "FILL,1,50,0",
        "FILL,4,50,20",
        "TRADE,ACME,10100,20,4,2,BUY",
        "FILL,2,20,10",
        "FILL,4,20,0",
        "BOOK,ACME,BIDS:,ASKS:10100x10;10200x100",
    };
    EXPECT_EQ(run_direct(symbols, requests), expected);
    EXPECT_EQ(run_pipeline(symbols, requests), expected);
}

TEST_F(PipelineTest, MatchesDirectEngineWithRejectsMarketAndUnknownSymbol) {
    const SymbolId other = symbols.add("OTHER");  // nie per AddSymbolRequest handelbar gemacht
    const std::vector<Request> requests = {
        BookRequest{acme, 5},  // vor AddSymbol: leeres Buch
        AddSymbolRequest{acme},
        limit(1, acme, Side::Buy, 9900, 10),
        limit(1, acme, Side::Buy, 9900, 10),  // DUPLICATE_ID
        limit(2, acme, Side::Buy, 0, 10),     // INVALID_PRICE
        limit(3, acme, Side::Buy, 9900, 0),   // INVALID_QUANTITY
        limit(4, other, Side::Buy, 9900, 5),  // UNKNOWN_SYMBOL
        Command{NewOrder{5, acme, Side::Sell, OrderType::Market, 0, 25}},
        Command{CancelOrder{1}},
        BookRequest{acme, 5},
    };
    const Events direct = run_direct(symbols, requests);
    ASSERT_FALSE(direct.empty());
    EXPECT_EQ(run_pipeline(symbols, requests), direct);
}

TEST_F(PipelineTest, BookDepthIsClampedToMaxDepth) {
    std::vector<Request> requests = {AddSymbolRequest{acme}};
    for (OrderId id = 1; id <= 8; ++id) {
        requests.push_back(limit(id, acme, Side::Sell, 10000 + static_cast<Price>(id), 1));
        requests.push_back(limit(100 + id, acme, Side::Buy, 9000 - static_cast<Price>(id), 1));
    }
    requests.push_back(BookRequest{acme, 1000});
    requests.push_back(BookRequest{acme, 2});

    const Events events = run_pipeline(symbols, requests);
    ASSERT_GE(events.size(), 2U);
    EXPECT_EQ(events[events.size() - 2],
              "BOOK,ACME,BIDS:8999x1;8998x1;8997x1;8996x1;8995x1,ASKS:10001x1;10002x1;10003x1;10004x1;10005x1");
    EXPECT_EQ(events.back(), "BOOK,ACME,BIDS:8999x1;8998x1,ASKS:10001x1;10002x1");
    EXPECT_EQ(events, run_direct(symbols, requests));
}

TEST_F(PipelineTest, StopWithoutSubmit) {
    CollectingSink sink(symbols);
    Pipeline pipeline(sink);
    pipeline.start();
    pipeline.stop();
    EXPECT_TRUE(sink.events().empty());
}

TEST_F(PipelineTest, StopWithoutStartAndDestructorWithoutStart) {
    CollectingSink sink(symbols);
    {
        Pipeline pipeline(sink);
        pipeline.stop();
    }
    { Pipeline pipeline(sink); }
    EXPECT_TRUE(sink.events().empty());
}

TEST_F(PipelineTest, DestructorWithoutStopDeliversEverything) {
    CollectingSink sink(symbols);
    {
        Pipeline pipeline(sink);
        pipeline.start();
        pipeline.submit(AddSymbolRequest{acme});
        pipeline.submit(limit(1, acme, Side::Sell, 10100, 5));
        pipeline.submit(limit(2, acme, Side::Buy, 10100, 5));
    }  // ~Pipeline ruft stop()
    const Events expected = {"ACK,1,1", "ACK,2,2", "TRADE,ACME,10100,5,2,1,BUY", "FILL,1,5,0", "FILL,2,5,0"};
    EXPECT_EQ(sink.events(), expected);
}

TEST_F(PipelineTest, DoubleStopIsIdempotent) {
    CollectingSink sink(symbols);
    Pipeline pipeline(sink);
    pipeline.start();
    pipeline.submit(AddSymbolRequest{acme});
    pipeline.submit(limit(1, acme, Side::Buy, 9900, 5));
    pipeline.stop();
    pipeline.stop();
    const Events expected = {"ACK,1,1"};
    EXPECT_EQ(sink.events(), expected);
}

TEST_F(PipelineTest, RestartAfterStopStartsWithFreshEngine) {
    CollectingSink sink(symbols);
    Pipeline pipeline(sink);
    pipeline.start();
    pipeline.submit(AddSymbolRequest{acme});
    pipeline.submit(limit(1, acme, Side::Buy, 9900, 5));
    pipeline.stop();
    pipeline.start();
    pipeline.submit(AddSymbolRequest{acme});
    pipeline.submit(limit(1, acme, Side::Buy, 9900, 5));  // neue Engine: ID 1 wieder frei, Seq wieder 1
    pipeline.stop();
    const Events expected = {"ACK,1,1", "ACK,1,1"};
    EXPECT_EQ(sink.events(), expected);
}

TEST_F(PipelineTest, BackpressureMoreRequestsThanQueueCapacity) {
    // Mehr Requests als die RequestQueue fasst; jedes Paar erzeugt 5 Events, also läuft
    // auch die EventQueue voll. Nichts darf verloren gehen, Reihenfolge bleibt erhalten.
    constexpr std::size_t kPairs = kRequestQueueCapacity / 2 + 1000;
    static_assert(2 * kPairs > kRequestQueueCapacity);
    static_assert(5 * kPairs > kEventQueueCapacity);

    std::vector<Request> requests = {AddSymbolRequest{acme}};
    requests.reserve(2 * kPairs + 2);
    for (std::size_t i = 0; i < kPairs; ++i) {
        const auto id = static_cast<OrderId>(2 * i + 1);
        requests.push_back(limit(id, acme, Side::Sell, 10000 + static_cast<Price>(i % 7), 3));
        requests.push_back(limit(id + 1, acme, Side::Buy, 10010, 3));
    }
    requests.push_back(BookRequest{acme, 5});

    const Events events = run_pipeline(symbols, requests);
    EXPECT_EQ(events.size(), 5 * kPairs + 1);
    EXPECT_EQ(events.back(), "BOOK,ACME,BIDS:,ASKS:");
    EXPECT_EQ(events, run_direct(symbols, requests));
}

}  // namespace
}  // namespace engine::test
