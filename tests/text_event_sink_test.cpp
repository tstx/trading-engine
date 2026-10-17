#include <gtest/gtest.h>

#include <sstream>

#include "engine/symbol_table.hpp"
#include "engine/text_event_sink.hpp"

using namespace engine;

class TextEventSinkTest : public ::testing::Test {
protected:
    TextEventSinkTest() : acme_(symbols_.add("ACME")) {}

    SymbolTable symbols_;
    SymbolId acme_;
    std::ostringstream out_;
    TextEventSink sink_{out_, symbols_};
};

TEST_F(TextEventSinkTest, Ack) {
    sink_.on_ack(1, 1);
    EXPECT_EQ(out_.str(), "ACK,1,1\n");
}

TEST_F(TextEventSinkTest, Reject) {
    sink_.on_reject(5, RejectReason::DuplicateId);
    sink_.on_reject(6, RejectReason::UnknownSymbol);
    EXPECT_EQ(out_.str(), "REJECT,5,DUPLICATE_ID\nREJECT,6,UNKNOWN_SYMBOL\n");
}

TEST_F(TextEventSinkTest, Trade) {
    sink_.on_trade(Trade{acme_, 10100, 50, 4, 1, Side::Buy});
    sink_.on_trade(Trade{acme_, -3, 1, 10, 11, Side::Sell});
    EXPECT_EQ(out_.str(), "TRADE,ACME,10100,50,4,1,BUY\nTRADE,ACME,-3,1,10,11,SELL\n");
}

TEST_F(TextEventSinkTest, Fill) {
    sink_.on_fill(4, 50, 20);
    EXPECT_EQ(out_.str(), "FILL,4,50,20\n");
}

TEST_F(TextEventSinkTest, Cancelled) {
    sink_.on_cancelled(11, 60, CancelReason::NoLiquidity);
    sink_.on_cancelled(12, 5, CancelReason::User);
    EXPECT_EQ(out_.str(), "CANCELLED,11,60,NO_LIQUIDITY\nCANCELLED,12,5,USER\n");
}

TEST_F(TextEventSinkTest, BookEmptyBidsSpecExample) {
    sink_.write_book(BookSnapshot{acme_, {}, {{10100, 10}, {10200, 100}}});
    EXPECT_EQ(out_.str(), "BOOK,ACME,BIDS:,ASKS:10100x10;10200x100\n");
}

TEST_F(TextEventSinkTest, BookBothSidesEmpty) {
    sink_.write_book(BookSnapshot{acme_, {}, {}});
    EXPECT_EQ(out_.str(), "BOOK,ACME,BIDS:,ASKS:\n");
}

TEST_F(TextEventSinkTest, BookBothSides) {
    const SymbolId foo = symbols_.add("FOO");
    sink_.write_book(BookSnapshot{foo, {{9900, 40}, {9800, 1}}, {{10000, 7}}});
    EXPECT_EQ(out_.str(), "BOOK,FOO,BIDS:9900x40;9800x1,ASKS:10000x7\n");
}

TEST_F(TextEventSinkTest, SpecExampleSequence) {
    sink_.on_ack(1, 1);
    sink_.on_ack(2, 2);
    sink_.on_trade(Trade{acme_, 10100, 20, 4, 2, Side::Buy});
    sink_.on_fill(2, 20, 10);
    sink_.on_fill(4, 20, 0);
    EXPECT_EQ(out_.str(), "ACK,1,1\nACK,2,2\nTRADE,ACME,10100,20,4,2,BUY\nFILL,2,20,10\nFILL,4,20,0\n");
}
