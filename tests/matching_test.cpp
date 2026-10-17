// MatchingEngine: Testfälle aus plan.txt §9 (ohne Cancel/Modify/Stress) und
// die Beispiele aus §4.2/§4.3 als exakte Event-Sequenzen.

#include <gtest/gtest.h>

#include "engine/matching_engine.hpp"
#include "engine/types.hpp"
#include "test_sink.hpp"

namespace {

using engine::OrderType;
using engine::Side;
using engine::SymbolId;
using engine::test::EngineFixture;
using engine::test::Events;

constexpr SymbolId kUnknown = 999;

class MatchingTest : public EngineFixture {};

// ---------------------------------------------------------------------------
// Beispiele aus plan.txt §4

TEST_F(MatchingTest, Example_4_2) {
    new_limit(1, Side::Sell, 10100, 50);
    new_limit(2, Side::Sell, 10100, 30);
    new_limit(3, Side::Sell, 10200, 100);
    new_limit(4, Side::Buy, 10150, 70);
    EXPECT_EQ(take(), (Events{
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
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:10100x10;10200x100");
}

TEST_F(MatchingTest, Example_4_3_MarketWithoutEnoughLiquidity) {
    new_limit(10, Side::Buy, 9900, 40);
    new_market(11, Side::Sell, 100);
    EXPECT_EQ(take(), (Events{
                          "ACK,10,1",
                          "ACK,11,2",
                          "TRADE,ACME,9900,40,10,11,SELL",
                          "FILL,10,40,0",
                          "FILL,11,40,60",
                          "CANCELLED,11,60,NO_LIQUIDITY",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:");
}

// ---------------------------------------------------------------------------
// Ruhen ohne Gegenseite

TEST_F(MatchingTest, LimitBuyWithoutCounterpartyRests) {
    new_limit(1, Side::Buy, 10000, 50);
    EXPECT_EQ(take(), (Events{"ACK,1,1"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:10000x50,ASKS:");
}

TEST_F(MatchingTest, LimitSellWithoutCounterpartyRests) {
    new_limit(1, Side::Sell, 10100, 25);
    EXPECT_EQ(take(), (Events{"ACK,1,1"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:10100x25");
}

TEST_F(MatchingTest, SameSideOrdersNeverMatch) {
    new_limit(1, Side::Buy, 10000, 10);
    new_limit(2, Side::Buy, 10100, 10);
    new_limit(3, Side::Buy, 10000, 5);
    EXPECT_EQ(take(), (Events{"ACK,1,1", "ACK,2,2", "ACK,3,3"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:10100x10;10000x15,ASKS:");
}

// ---------------------------------------------------------------------------
// Exakter und partieller Match

TEST_F(MatchingTest, ExactMatchFillsBothAndEmptiesBook) {
    new_limit(1, Side::Sell, 10100, 50);
    new_limit(2, Side::Buy, 10100, 50);
    EXPECT_EQ(take(), (Events{
                          "ACK,1,1",
                          "ACK,2,2",
                          "TRADE,ACME,10100,50,2,1,BUY",
                          "FILL,1,50,0",
                          "FILL,2,50,0",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:");
}

TEST_F(MatchingTest, PartialFillAggressorLargerRestsRemainder) {
    new_limit(1, Side::Sell, 10100, 30);
    new_limit(2, Side::Buy, 10100, 50);
    EXPECT_EQ(take(), (Events{
                          "ACK,1,1",
                          "ACK,2,2",
                          "TRADE,ACME,10100,30,2,1,BUY",
                          "FILL,1,30,0",
                          "FILL,2,30,20",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:10100x20,ASKS:");
}

TEST_F(MatchingTest, PartialFillRestingLargerKeepsRemainder) {
    new_limit(1, Side::Buy, 10000, 100);
    new_limit(2, Side::Sell, 10000, 40);
    EXPECT_EQ(take(), (Events{
                          "ACK,1,1",
                          "ACK,2,2",
                          "TRADE,ACME,10000,40,1,2,SELL",
                          "FILL,1,40,60",
                          "FILL,2,40,0",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:10000x60,ASKS:");
}

TEST_F(MatchingTest, FillReportsQuantityOfThisTradeOnly) {
    new_limit(1, Side::Buy, 10000, 100);
    new_limit(2, Side::Sell, 10000, 30);
    new_limit(3, Side::Sell, 10000, 30);
    EXPECT_EQ(take(), (Events{
                          "ACK,1,1",
                          "ACK,2,2",
                          "TRADE,ACME,10000,30,1,2,SELL",
                          "FILL,1,30,70",
                          "FILL,2,30,0",
                          "ACK,3,3",
                          "TRADE,ACME,10000,30,1,3,SELL",
                          "FILL,1,30,40",
                          "FILL,3,30,0",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:10000x40,ASKS:");
}

TEST_F(MatchingTest, RestedRemainderOfAggressorCanBeMatchedLater) {
    new_limit(1, Side::Sell, 10100, 30);
    new_limit(2, Side::Buy, 10100, 50);  // 20 bleiben liegen
    take();
    new_limit(3, Side::Sell, 10050, 25);
    EXPECT_EQ(take(), (Events{
                          "ACK,3,3",
                          "TRADE,ACME,10100,20,2,3,SELL",
                          "FILL,2,20,0",
                          "FILL,3,20,5",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:10050x5");
}

// ---------------------------------------------------------------------------
// Priorität

TEST_F(MatchingTest, TimePriorityOlderOrderFilledFirst) {
    new_limit(1, Side::Sell, 10100, 10);
    new_limit(2, Side::Sell, 10100, 10);
    take();
    new_limit(3, Side::Buy, 10100, 10);
    EXPECT_EQ(take(), (Events{
                          "ACK,3,3",
                          "TRADE,ACME,10100,10,3,1,BUY",
                          "FILL,1,10,0",
                          "FILL,3,10,0",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:10100x10");
    new_limit(4, Side::Buy, 10100, 10);
    EXPECT_EQ(take(), (Events{
                          "ACK,4,4",
                          "TRADE,ACME,10100,10,4,2,BUY",
                          "FILL,2,10,0",
                          "FILL,4,10,0",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:");
}

TEST_F(MatchingTest, TimePriorityOnBidSide) {
    new_limit(7, Side::Buy, 10000, 10);
    new_limit(5, Side::Buy, 10000, 10);  // kleinere ID, aber jünger
    take();
    new_limit(9, Side::Sell, 10000, 15);
    EXPECT_EQ(take(), (Events{
                          "ACK,9,3",
                          "TRADE,ACME,10000,10,7,9,SELL",
                          "FILL,7,10,0",
                          "FILL,9,10,5",
                          "TRADE,ACME,10000,5,5,9,SELL",
                          "FILL,5,5,5",
                          "FILL,9,5,0",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:10000x5,ASKS:");
}

TEST_F(MatchingTest, PricePriorityBeatsTimePriorityForAsks) {
    new_limit(1, Side::Sell, 10200, 10);  // älter, schlechterer Preis
    new_limit(2, Side::Sell, 10100, 10);  // jünger, besserer Preis
    take();
    new_limit(3, Side::Buy, 10200, 10);
    EXPECT_EQ(take(), (Events{
                          "ACK,3,3",
                          "TRADE,ACME,10100,10,3,2,BUY",
                          "FILL,2,10,0",
                          "FILL,3,10,0",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:10200x10");
}

TEST_F(MatchingTest, PricePriorityBeatsTimePriorityForBids) {
    new_limit(1, Side::Buy, 9900, 10);   // älter, schlechterer Preis
    new_limit(2, Side::Buy, 10000, 10);  // jünger, besserer Preis
    take();
    new_limit(3, Side::Sell, 9900, 10);
    EXPECT_EQ(take(), (Events{
                          "ACK,3,3",
                          "TRADE,ACME,10000,10,2,3,SELL",
                          "FILL,2,10,0",
                          "FILL,3,10,0",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:9900x10,ASKS:");
}

// ---------------------------------------------------------------------------
// Sweep und Ausführungspreis

TEST_F(MatchingTest, SweepMultipleLevelsStopsAtLimit) {
    new_limit(1, Side::Sell, 10100, 10);
    new_limit(2, Side::Sell, 10200, 20);
    new_limit(3, Side::Sell, 10300, 30);
    new_limit(4, Side::Sell, 10400, 40);
    take();
    new_limit(5, Side::Buy, 10300, 50);
    EXPECT_EQ(take(), (Events{
                          "ACK,5,5",
                          "TRADE,ACME,10100,10,5,1,BUY",
                          "FILL,1,10,0",
                          "FILL,5,10,40",
                          "TRADE,ACME,10200,20,5,2,BUY",
                          "FILL,2,20,0",
                          "FILL,5,20,20",
                          "TRADE,ACME,10300,20,5,3,BUY",
                          "FILL,3,20,10",
                          "FILL,5,20,0",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:10300x10;10400x40");
}

TEST_F(MatchingTest, SweepWithRemainderRestingAtLimit) {
    new_limit(1, Side::Sell, 10100, 10);
    new_limit(2, Side::Sell, 10200, 20);
    new_limit(3, Side::Sell, 10300, 30);
    take();
    new_limit(4, Side::Buy, 10250, 50);
    EXPECT_EQ(take(), (Events{
                          "ACK,4,4",
                          "TRADE,ACME,10100,10,4,1,BUY",
                          "FILL,1,10,0",
                          "FILL,4,10,40",
                          "TRADE,ACME,10200,20,4,2,BUY",
                          "FILL,2,20,0",
                          "FILL,4,20,20",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:10250x20,ASKS:10300x30");
}

TEST_F(MatchingTest, SellSweepAcrossBidLevels) {
    new_limit(1, Side::Buy, 10000, 5);
    new_limit(2, Side::Buy, 9900, 5);
    new_limit(3, Side::Buy, 9800, 5);
    take();
    new_limit(4, Side::Sell, 9900, 20);
    EXPECT_EQ(take(), (Events{
                          "ACK,4,4",
                          "TRADE,ACME,10000,5,1,4,SELL",
                          "FILL,1,5,0",
                          "FILL,4,5,15",
                          "TRADE,ACME,9900,5,2,4,SELL",
                          "FILL,2,5,0",
                          "FILL,4,5,10",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:9800x5,ASKS:9900x10");
}

TEST_F(MatchingTest, ExecutionPriceIsRestingPriceForBuyAggressor) {
    new_limit(1, Side::Sell, 10000, 10);
    new_limit(2, Side::Buy, 10500, 10);
    EXPECT_EQ(take(), (Events{
                          "ACK,1,1",
                          "ACK,2,2",
                          "TRADE,ACME,10000,10,2,1,BUY",
                          "FILL,1,10,0",
                          "FILL,2,10,0",
                      }));
}

TEST_F(MatchingTest, ExecutionPriceIsRestingPriceForSellAggressor) {
    new_limit(1, Side::Buy, 10050, 10);
    new_limit(2, Side::Sell, 9900, 10);
    EXPECT_EQ(take(), (Events{
                          "ACK,1,1",
                          "ACK,2,2",
                          "TRADE,ACME,10050,10,1,2,SELL",
                          "FILL,1,10,0",
                          "FILL,2,10,0",
                      }));
}

TEST_F(MatchingTest, LimitDoesNotCrossNoTrade) {
    new_limit(1, Side::Buy, 10000, 10);
    new_limit(2, Side::Sell, 10001, 10);
    new_limit(3, Side::Buy, 10000, 5);
    EXPECT_EQ(take(), (Events{"ACK,1,1", "ACK,2,2", "ACK,3,3"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:10000x15,ASKS:10001x10");
}

// ---------------------------------------------------------------------------
// Market-Orders

TEST_F(MatchingTest, MarketBuyWithEnoughLiquidity) {
    new_limit(1, Side::Sell, 10100, 30);
    new_limit(2, Side::Sell, 10200, 30);
    take();
    new_market(3, Side::Buy, 40);
    EXPECT_EQ(take(), (Events{
                          "ACK,3,3",
                          "TRADE,ACME,10100,30,3,1,BUY",
                          "FILL,1,30,0",
                          "FILL,3,30,10",
                          "TRADE,ACME,10200,10,3,2,BUY",
                          "FILL,2,10,20",
                          "FILL,3,10,0",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:10200x20");
}

TEST_F(MatchingTest, MarketSellExactLiquidityNoCancel) {
    new_limit(1, Side::Buy, 10000, 25);
    take();
    new_market(2, Side::Sell, 25);
    EXPECT_EQ(take(), (Events{
                          "ACK,2,2",
                          "TRADE,ACME,10000,25,1,2,SELL",
                          "FILL,1,25,0",
                          "FILL,2,25,0",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:");
}

TEST_F(MatchingTest, MarketAgainstEmptyBookCancelsFullQuantity) {
    new_market(1, Side::Buy, 25);
    EXPECT_EQ(take(), (Events{"ACK,1,1", "CANCELLED,1,25,NO_LIQUIDITY"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:");
}

TEST_F(MatchingTest, MarketIgnoresSameSideLiquidity) {
    new_limit(1, Side::Sell, 10100, 10);
    take();
    new_market(2, Side::Sell, 10);
    EXPECT_EQ(take(), (Events{"ACK,2,2", "CANCELLED,2,10,NO_LIQUIDITY"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:10100x10");
}

TEST_F(MatchingTest, MarketPriceFieldIsIgnored) {
    // Preis einer MARKET-Order wird weder validiert noch als Grenze benutzt.
    new_limit(1, Side::Sell, 10100, 10);
    take();
    new_market(2, Side::Buy, 10, /*price=*/1);
    EXPECT_EQ(take(), (Events{
                          "ACK,2,2",
                          "TRADE,ACME,10100,10,2,1,BUY",
                          "FILL,1,10,0",
                          "FILL,2,10,0",
                      }));
    new_market(3, Side::Buy, 5, /*price=*/-1);
    EXPECT_EQ(take(), (Events{"ACK,3,3", "CANCELLED,3,5,NO_LIQUIDITY"}));
}

// ---------------------------------------------------------------------------
// Rejects

TEST_F(MatchingTest, RejectDuplicateIdOfRestingOrder) {
    new_limit(1, Side::Buy, 10000, 10);
    new_limit(1, Side::Sell, 10000, 10);
    EXPECT_EQ(take(), (Events{"ACK,1,1", "REJECT,1,DUPLICATE_ID"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:10000x10,ASKS:");
}

TEST_F(MatchingTest, RejectDuplicateIdOfFilledOrder) {
    new_limit(1, Side::Sell, 10100, 50);
    new_limit(2, Side::Buy, 10100, 50);
    take();
    new_limit(1, Side::Sell, 10100, 5);
    new_limit(2, Side::Buy, 10000, 5);
    EXPECT_EQ(take(), (Events{"REJECT,1,DUPLICATE_ID", "REJECT,2,DUPLICATE_ID"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:");
}

TEST_F(MatchingTest, RejectDuplicateIdOfCancelledMarketOrder) {
    new_market(1, Side::Buy, 10);
    take();
    new_limit(1, Side::Buy, 10000, 10);
    EXPECT_EQ(take(), (Events{"REJECT,1,DUPLICATE_ID"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:");
}

TEST_F(MatchingTest, RejectZeroQuantity) {
    new_limit(1, Side::Buy, 10000, 0);
    new_market(2, Side::Sell, 0);
    EXPECT_EQ(take(), (Events{"REJECT,1,INVALID_QUANTITY", "REJECT,2,INVALID_QUANTITY"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:");
}

TEST_F(MatchingTest, RejectNonPositiveLimitPrice) {
    new_limit(1, Side::Buy, 0, 10);
    new_limit(2, Side::Sell, -100, 10);
    new_limit(3, Side::Buy, 1, 10);  // kleinster gültiger Preis
    EXPECT_EQ(take(), (Events{"REJECT,1,INVALID_PRICE", "REJECT,2,INVALID_PRICE", "ACK,3,1"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:1x10,ASKS:");
}

TEST_F(MatchingTest, RejectUnknownSymbol) {
    new_order(1, kUnknown, Side::Buy, OrderType::Limit, 10000, 10);
    new_order(2, kUnknown, Side::Sell, OrderType::Market, 0, 10);
    EXPECT_EQ(take(), (Events{"REJECT,1,UNKNOWN_SYMBOL", "REJECT,2,UNKNOWN_SYMBOL"}));
}

TEST_F(MatchingTest, ValidationOrder) {
    new_limit(1, Side::Buy, 10000, 10);
    take();
    // Alle vier Fehler: DUPLICATE_ID gewinnt.
    new_order(1, kUnknown, Side::Buy, OrderType::Limit, 0, 0);
    // Unbekanntes Symbol + Menge 0 + Preis 0: UNKNOWN_SYMBOL.
    new_order(2, kUnknown, Side::Buy, OrderType::Limit, 0, 0);
    // Menge 0 + Preis 0: INVALID_QUANTITY.
    new_order(3, acme, Side::Buy, OrderType::Limit, 0, 0);
    // Menge 0 + negativer Preis: INVALID_QUANTITY.
    new_order(4, acme, Side::Sell, OrderType::Limit, -5, 0);
    // Duplikat + Menge 0: DUPLICATE_ID.
    new_order(1, acme, Side::Sell, OrderType::Limit, 10000, 0);
    EXPECT_EQ(take(), (Events{
                          "REJECT,1,DUPLICATE_ID",
                          "REJECT,2,UNKNOWN_SYMBOL",
                          "REJECT,3,INVALID_QUANTITY",
                          "REJECT,4,INVALID_QUANTITY",
                          "REJECT,1,DUPLICATE_ID",
                      }));
}

TEST_F(MatchingTest, SeqSkipsRejectsAndRejectedIdStaysFree) {
    new_limit(1, Side::Buy, 10000, 10);
    new_limit(2, Side::Buy, 10000, 0);   // REJECT, keine Seq, ID 2 bleibt frei
    new_limit(1, Side::Buy, 10000, 10);  // REJECT DUPLICATE_ID, keine Seq
    new_limit(3, Side::Buy, 0, 10);      // REJECT INVALID_PRICE
    new_limit(2, Side::Buy, 9900, 10);   // jetzt gültig
    new_market(4, Side::Sell, 5);        // MARKET bekommt ebenfalls eine Seq
    new_limit(3, Side::Sell, 10100, 7);  // ID 3 war abgelehnt, also frei
    EXPECT_EQ(take(), (Events{
                          "ACK,1,1",
                          "REJECT,2,INVALID_QUANTITY",
                          "REJECT,1,DUPLICATE_ID",
                          "REJECT,3,INVALID_PRICE",
                          "ACK,2,2",
                          "ACK,4,3",
                          "TRADE,ACME,10000,5,1,4,SELL",
                          "FILL,1,5,5",
                          "FILL,4,5,0",
                          "ACK,3,4",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:10000x5;9900x10,ASKS:10100x7");
}

TEST_F(MatchingTest, RejectedOrderDoesNotTouchBook) {
    new_limit(1, Side::Sell, 10100, 10);
    take();
    new_limit(2, Side::Buy, 10100, 0);
    new_limit(1, Side::Buy, 10100, 10);
    EXPECT_EQ(take(), (Events{"REJECT,2,INVALID_QUANTITY", "REJECT,1,DUPLICATE_ID"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:10100x10");
}

// ---------------------------------------------------------------------------
// Symbole und Snapshot

TEST_F(MatchingTest, HasSymbol) {
    EXPECT_TRUE(engine.has_symbol(acme));
    EXPECT_FALSE(engine.has_symbol(kUnknown));
}

TEST_F(MatchingTest, SnapshotOfUnknownSymbolIsEmpty) {
    new_limit(1, Side::Buy, 10000, 10);
    auto snap = engine.snapshot(kUnknown, 5);
    EXPECT_EQ(snap.symbol, kUnknown);
    EXPECT_TRUE(snap.bids.empty());
    EXPECT_TRUE(snap.asks.empty());
}

TEST_F(MatchingTest, SnapshotRespectsDepth) {
    for (int i = 0; i < 7; ++i) {
        new_limit(static_cast<engine::OrderId>(1 + i), Side::Buy, 10000 - (10 * i), 1 + i);
        new_limit(static_cast<engine::OrderId>(101 + i), Side::Sell, 10100 + (10 * i), 10);
    }
    EXPECT_EQ(book(5),
              "BOOK,ACME,BIDS:10000x1;9990x2;9980x3;9970x4;9960x5,"
              "ASKS:10100x10;10110x10;10120x10;10130x10;10140x10");
    EXPECT_EQ(book(1), "BOOK,ACME,BIDS:10000x1,ASKS:10100x10");
    EXPECT_EQ(book(0), "BOOK,ACME,BIDS:,ASKS:");
}

TEST_F(MatchingTest, SymbolsAreIsolatedAndSeqIsGlobal) {
    SymbolId beta = symbols.add("BETA");
    engine.add_symbol(beta);
    new_limit(1, Side::Sell, 10100, 10);
    new_order(2, beta, Side::Buy, OrderType::Limit, 10200, 10);  // darf ACME nicht kreuzen
    new_order(3, beta, Side::Sell, OrderType::Limit, 10200, 4);
    EXPECT_EQ(take(), (Events{
                          "ACK,1,1",
                          "ACK,2,2",
                          "ACK,3,3",
                          "TRADE,BETA,10200,4,2,3,SELL",
                          "FILL,2,4,6",
                          "FILL,3,4,0",
                      }));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:10100x10");
    EXPECT_EQ(engine::test::book_line(symbols, engine.snapshot(beta, 5)), "BOOK,BETA,BIDS:10200x6,ASKS:");
}

}  // namespace
