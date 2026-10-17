// MatchingEngine: Testfälle aus plan.txt §9 (ohne Cancel/Modify/Stress) und
// die Beispiele aus §4.2/§4.3 als exakte Event-Sequenzen, typisiert über alle drei
// Engine-Varianten (Map/Vector/Tick); dazu Tick-spezifisch das Preisband.

#include <limits>

#include <gtest/gtest.h>

#include "engine/matching_engine.hpp"
#include "engine/types.hpp"
#include "test_sink.hpp"

namespace {

using engine::OrderType;
using engine::Price;
using engine::Side;
using engine::SymbolId;
using engine::test::EngineFixture;
using engine::test::EngineTypeNames;
using engine::test::EngineTypes;
using engine::test::Events;

constexpr SymbolId kUnknown = 999;

template <typename Engine>
class MatchingTest : public EngineFixture<Engine> {};

TYPED_TEST_SUITE(MatchingTest, EngineTypes, EngineTypeNames);

// ---------------------------------------------------------------------------
// Beispiele aus plan.txt §4

TYPED_TEST(MatchingTest, Example_4_2) {
    this->new_limit(1, Side::Sell, 10100, 50);
    this->new_limit(2, Side::Sell, 10100, 30);
    this->new_limit(3, Side::Sell, 10200, 100);
    this->new_limit(4, Side::Buy, 10150, 70);
    EXPECT_EQ(this->take(), (Events{
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
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x10;10200x100");
}

TYPED_TEST(MatchingTest, Example_4_3_MarketWithoutEnoughLiquidity) {
    this->new_limit(10, Side::Buy, 9900, 40);
    this->new_market(11, Side::Sell, 100);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,10,1",
                                "ACK,11,2",
                                "TRADE,ACME,9900,40,10,11,SELL",
                                "FILL,10,40,0",
                                "FILL,11,40,60",
                                "CANCELLED,11,60,NO_LIQUIDITY",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

// ---------------------------------------------------------------------------
// Ruhen ohne Gegenseite

TYPED_TEST(MatchingTest, LimitBuyWithoutCounterpartyRests) {
    this->new_limit(1, Side::Buy, 10000, 50);
    EXPECT_EQ(this->take(), (Events{"ACK,1,1"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x50,ASKS:");
}

TYPED_TEST(MatchingTest, LimitSellWithoutCounterpartyRests) {
    this->new_limit(1, Side::Sell, 10100, 25);
    EXPECT_EQ(this->take(), (Events{"ACK,1,1"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x25");
}

TYPED_TEST(MatchingTest, SameSideOrdersNeverMatch) {
    this->new_limit(1, Side::Buy, 10000, 10);
    this->new_limit(2, Side::Buy, 10100, 10);
    this->new_limit(3, Side::Buy, 10000, 5);
    EXPECT_EQ(this->take(), (Events{"ACK,1,1", "ACK,2,2", "ACK,3,3"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10100x10;10000x15,ASKS:");
}

// ---------------------------------------------------------------------------
// Exakter und partieller Match

TYPED_TEST(MatchingTest, ExactMatchFillsBothAndEmptiesBook) {
    this->new_limit(1, Side::Sell, 10100, 50);
    this->new_limit(2, Side::Buy, 10100, 50);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,1,1",
                                "ACK,2,2",
                                "TRADE,ACME,10100,50,2,1,BUY",
                                "FILL,1,50,0",
                                "FILL,2,50,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(MatchingTest, PartialFillAggressorLargerRestsRemainder) {
    this->new_limit(1, Side::Sell, 10100, 30);
    this->new_limit(2, Side::Buy, 10100, 50);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,1,1",
                                "ACK,2,2",
                                "TRADE,ACME,10100,30,2,1,BUY",
                                "FILL,1,30,0",
                                "FILL,2,30,20",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10100x20,ASKS:");
}

TYPED_TEST(MatchingTest, PartialFillRestingLargerKeepsRemainder) {
    this->new_limit(1, Side::Buy, 10000, 100);
    this->new_limit(2, Side::Sell, 10000, 40);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,1,1",
                                "ACK,2,2",
                                "TRADE,ACME,10000,40,1,2,SELL",
                                "FILL,1,40,60",
                                "FILL,2,40,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x60,ASKS:");
}

TYPED_TEST(MatchingTest, FillReportsQuantityOfThisTradeOnly) {
    this->new_limit(1, Side::Buy, 10000, 100);
    this->new_limit(2, Side::Sell, 10000, 30);
    this->new_limit(3, Side::Sell, 10000, 30);
    EXPECT_EQ(this->take(), (Events{
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
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x40,ASKS:");
}

TYPED_TEST(MatchingTest, RestedRemainderOfAggressorCanBeMatchedLater) {
    this->new_limit(1, Side::Sell, 10100, 30);
    this->new_limit(2, Side::Buy, 10100, 50);  // 20 bleiben liegen
    this->take();
    this->new_limit(3, Side::Sell, 10050, 25);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,3,3",
                                "TRADE,ACME,10100,20,2,3,SELL",
                                "FILL,2,20,0",
                                "FILL,3,20,5",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10050x5");
}

// ---------------------------------------------------------------------------
// Priorität

TYPED_TEST(MatchingTest, TimePriorityOlderOrderFilledFirst) {
    this->new_limit(1, Side::Sell, 10100, 10);
    this->new_limit(2, Side::Sell, 10100, 10);
    this->take();
    this->new_limit(3, Side::Buy, 10100, 10);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,3,3",
                                "TRADE,ACME,10100,10,3,1,BUY",
                                "FILL,1,10,0",
                                "FILL,3,10,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x10");
    this->new_limit(4, Side::Buy, 10100, 10);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,4,4",
                                "TRADE,ACME,10100,10,4,2,BUY",
                                "FILL,2,10,0",
                                "FILL,4,10,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(MatchingTest, TimePriorityOnBidSide) {
    this->new_limit(7, Side::Buy, 10000, 10);
    this->new_limit(5, Side::Buy, 10000, 10);  // kleinere ID, aber jünger
    this->take();
    this->new_limit(9, Side::Sell, 10000, 15);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,9,3",
                                "TRADE,ACME,10000,10,7,9,SELL",
                                "FILL,7,10,0",
                                "FILL,9,10,5",
                                "TRADE,ACME,10000,5,5,9,SELL",
                                "FILL,5,5,5",
                                "FILL,9,5,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x5,ASKS:");
}

TYPED_TEST(MatchingTest, PricePriorityBeatsTimePriorityForAsks) {
    this->new_limit(1, Side::Sell, 10200, 10);  // älter, schlechterer Preis
    this->new_limit(2, Side::Sell, 10100, 10);  // jünger, besserer Preis
    this->take();
    this->new_limit(3, Side::Buy, 10200, 10);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,3,3",
                                "TRADE,ACME,10100,10,3,2,BUY",
                                "FILL,2,10,0",
                                "FILL,3,10,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10200x10");
}

TYPED_TEST(MatchingTest, PricePriorityBeatsTimePriorityForBids) {
    this->new_limit(1, Side::Buy, 9900, 10);   // älter, schlechterer Preis
    this->new_limit(2, Side::Buy, 10000, 10);  // jünger, besserer Preis
    this->take();
    this->new_limit(3, Side::Sell, 9900, 10);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,3,3",
                                "TRADE,ACME,10000,10,2,3,SELL",
                                "FILL,2,10,0",
                                "FILL,3,10,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:9900x10,ASKS:");
}

// ---------------------------------------------------------------------------
// Sweep und Ausführungspreis

TYPED_TEST(MatchingTest, SweepMultipleLevelsStopsAtLimit) {
    this->new_limit(1, Side::Sell, 10100, 10);
    this->new_limit(2, Side::Sell, 10200, 20);
    this->new_limit(3, Side::Sell, 10300, 30);
    this->new_limit(4, Side::Sell, 10400, 40);
    this->take();
    this->new_limit(5, Side::Buy, 10300, 50);
    EXPECT_EQ(this->take(), (Events{
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
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10300x10;10400x40");
}

TYPED_TEST(MatchingTest, SweepWithRemainderRestingAtLimit) {
    this->new_limit(1, Side::Sell, 10100, 10);
    this->new_limit(2, Side::Sell, 10200, 20);
    this->new_limit(3, Side::Sell, 10300, 30);
    this->take();
    this->new_limit(4, Side::Buy, 10250, 50);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,4,4",
                                "TRADE,ACME,10100,10,4,1,BUY",
                                "FILL,1,10,0",
                                "FILL,4,10,40",
                                "TRADE,ACME,10200,20,4,2,BUY",
                                "FILL,2,20,0",
                                "FILL,4,20,20",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10250x20,ASKS:10300x30");
}

TYPED_TEST(MatchingTest, SellSweepAcrossBidLevels) {
    this->new_limit(1, Side::Buy, 10000, 5);
    this->new_limit(2, Side::Buy, 9900, 5);
    this->new_limit(3, Side::Buy, 9800, 5);
    this->take();
    this->new_limit(4, Side::Sell, 9900, 20);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,4,4",
                                "TRADE,ACME,10000,5,1,4,SELL",
                                "FILL,1,5,0",
                                "FILL,4,5,15",
                                "TRADE,ACME,9900,5,2,4,SELL",
                                "FILL,2,5,0",
                                "FILL,4,5,10",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:9800x5,ASKS:9900x10");
}

TYPED_TEST(MatchingTest, ExecutionPriceIsRestingPriceForBuyAggressor) {
    this->new_limit(1, Side::Sell, 10000, 10);
    this->new_limit(2, Side::Buy, 10500, 10);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,1,1",
                                "ACK,2,2",
                                "TRADE,ACME,10000,10,2,1,BUY",
                                "FILL,1,10,0",
                                "FILL,2,10,0",
                            }));
}

TYPED_TEST(MatchingTest, ExecutionPriceIsRestingPriceForSellAggressor) {
    this->new_limit(1, Side::Buy, 10050, 10);
    this->new_limit(2, Side::Sell, 9900, 10);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,1,1",
                                "ACK,2,2",
                                "TRADE,ACME,10050,10,1,2,SELL",
                                "FILL,1,10,0",
                                "FILL,2,10,0",
                            }));
}

TYPED_TEST(MatchingTest, LimitDoesNotCrossNoTrade) {
    this->new_limit(1, Side::Buy, 10000, 10);
    this->new_limit(2, Side::Sell, 10001, 10);
    this->new_limit(3, Side::Buy, 10000, 5);
    EXPECT_EQ(this->take(), (Events{"ACK,1,1", "ACK,2,2", "ACK,3,3"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x15,ASKS:10001x10");
}

// ---------------------------------------------------------------------------
// Market-Orders

TYPED_TEST(MatchingTest, MarketBuyWithEnoughLiquidity) {
    this->new_limit(1, Side::Sell, 10100, 30);
    this->new_limit(2, Side::Sell, 10200, 30);
    this->take();
    this->new_market(3, Side::Buy, 40);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,3,3",
                                "TRADE,ACME,10100,30,3,1,BUY",
                                "FILL,1,30,0",
                                "FILL,3,30,10",
                                "TRADE,ACME,10200,10,3,2,BUY",
                                "FILL,2,10,20",
                                "FILL,3,10,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10200x20");
}

TYPED_TEST(MatchingTest, MarketSellExactLiquidityNoCancel) {
    this->new_limit(1, Side::Buy, 10000, 25);
    this->take();
    this->new_market(2, Side::Sell, 25);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,2,2",
                                "TRADE,ACME,10000,25,1,2,SELL",
                                "FILL,1,25,0",
                                "FILL,2,25,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(MatchingTest, MarketAgainstEmptyBookCancelsFullQuantity) {
    this->new_market(1, Side::Buy, 25);
    EXPECT_EQ(this->take(), (Events{"ACK,1,1", "CANCELLED,1,25,NO_LIQUIDITY"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(MatchingTest, MarketIgnoresSameSideLiquidity) {
    this->new_limit(1, Side::Sell, 10100, 10);
    this->take();
    this->new_market(2, Side::Sell, 10);
    EXPECT_EQ(this->take(), (Events{"ACK,2,2", "CANCELLED,2,10,NO_LIQUIDITY"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x10");
}

TYPED_TEST(MatchingTest, MarketPriceFieldIsIgnored) {
    // Preis einer MARKET-Order wird weder validiert noch als Grenze benutzt.
    this->new_limit(1, Side::Sell, 10100, 10);
    this->take();
    this->new_market(2, Side::Buy, 10, /*price=*/1);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,2,2",
                                "TRADE,ACME,10100,10,2,1,BUY",
                                "FILL,1,10,0",
                                "FILL,2,10,0",
                            }));
    this->new_market(3, Side::Buy, 5, /*price=*/-1);
    EXPECT_EQ(this->take(), (Events{"ACK,3,3", "CANCELLED,3,5,NO_LIQUIDITY"}));
}

// ---------------------------------------------------------------------------
// Rejects

TYPED_TEST(MatchingTest, RejectDuplicateIdOfRestingOrder) {
    this->new_limit(1, Side::Buy, 10000, 10);
    this->new_limit(1, Side::Sell, 10000, 10);
    EXPECT_EQ(this->take(), (Events{"ACK,1,1", "REJECT,1,DUPLICATE_ID"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x10,ASKS:");
}

TYPED_TEST(MatchingTest, RejectDuplicateIdOfFilledOrder) {
    this->new_limit(1, Side::Sell, 10100, 50);
    this->new_limit(2, Side::Buy, 10100, 50);
    this->take();
    this->new_limit(1, Side::Sell, 10100, 5);
    this->new_limit(2, Side::Buy, 10000, 5);
    EXPECT_EQ(this->take(), (Events{"REJECT,1,DUPLICATE_ID", "REJECT,2,DUPLICATE_ID"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(MatchingTest, RejectDuplicateIdOfCancelledMarketOrder) {
    this->new_market(1, Side::Buy, 10);
    this->take();
    this->new_limit(1, Side::Buy, 10000, 10);
    EXPECT_EQ(this->take(), (Events{"REJECT,1,DUPLICATE_ID"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(MatchingTest, RejectZeroQuantity) {
    this->new_limit(1, Side::Buy, 10000, 0);
    this->new_market(2, Side::Sell, 0);
    EXPECT_EQ(this->take(), (Events{"REJECT,1,INVALID_QUANTITY", "REJECT,2,INVALID_QUANTITY"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(MatchingTest, RejectNonPositiveLimitPrice) {
    this->new_limit(1, Side::Buy, 0, 10);
    this->new_limit(2, Side::Sell, -100, 10);
    this->new_limit(3, Side::Buy, 1, 10);  // kleinster gültiger Preis
    EXPECT_EQ(this->take(), (Events{"REJECT,1,INVALID_PRICE", "REJECT,2,INVALID_PRICE", "ACK,3,1"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:1x10,ASKS:");
}

TYPED_TEST(MatchingTest, RejectUnknownSymbol) {
    this->new_order(1, kUnknown, Side::Buy, OrderType::Limit, 10000, 10);
    this->new_order(2, kUnknown, Side::Sell, OrderType::Market, 0, 10);
    EXPECT_EQ(this->take(), (Events{"REJECT,1,UNKNOWN_SYMBOL", "REJECT,2,UNKNOWN_SYMBOL"}));
}

TYPED_TEST(MatchingTest, ValidationOrder) {
    this->new_limit(1, Side::Buy, 10000, 10);
    this->take();
    // Alle vier Fehler: DUPLICATE_ID gewinnt.
    this->new_order(1, kUnknown, Side::Buy, OrderType::Limit, 0, 0);
    // Unbekanntes Symbol + Menge 0 + Preis 0: UNKNOWN_SYMBOL.
    this->new_order(2, kUnknown, Side::Buy, OrderType::Limit, 0, 0);
    // Menge 0 + Preis 0: INVALID_QUANTITY.
    this->new_order(3, this->acme, Side::Buy, OrderType::Limit, 0, 0);
    // Menge 0 + negativer Preis: INVALID_QUANTITY.
    this->new_order(4, this->acme, Side::Sell, OrderType::Limit, -5, 0);
    // Duplikat + Menge 0: DUPLICATE_ID.
    this->new_order(1, this->acme, Side::Sell, OrderType::Limit, 10000, 0);
    EXPECT_EQ(this->take(), (Events{
                                "REJECT,1,DUPLICATE_ID",
                                "REJECT,2,UNKNOWN_SYMBOL",
                                "REJECT,3,INVALID_QUANTITY",
                                "REJECT,4,INVALID_QUANTITY",
                                "REJECT,1,DUPLICATE_ID",
                            }));
}

TYPED_TEST(MatchingTest, SeqSkipsRejectsAndRejectedIdStaysFree) {
    this->new_limit(1, Side::Buy, 10000, 10);
    this->new_limit(2, Side::Buy, 10000, 0);   // REJECT, keine Seq, ID 2 bleibt frei
    this->new_limit(1, Side::Buy, 10000, 10);  // REJECT DUPLICATE_ID, keine Seq
    this->new_limit(3, Side::Buy, 0, 10);      // REJECT INVALID_PRICE
    this->new_limit(2, Side::Buy, 9900, 10);   // jetzt gültig
    this->new_market(4, Side::Sell, 5);        // MARKET bekommt ebenfalls eine Seq
    this->new_limit(3, Side::Sell, 10100, 7);  // ID 3 war abgelehnt, also frei
    EXPECT_EQ(this->take(), (Events{
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
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x5;9900x10,ASKS:10100x7");
}

TYPED_TEST(MatchingTest, RejectedOrderDoesNotTouchBook) {
    this->new_limit(1, Side::Sell, 10100, 10);
    this->take();
    this->new_limit(2, Side::Buy, 10100, 0);
    this->new_limit(1, Side::Buy, 10100, 10);
    EXPECT_EQ(this->take(), (Events{"REJECT,2,INVALID_QUANTITY", "REJECT,1,DUPLICATE_ID"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x10");
}

// ---------------------------------------------------------------------------
// Symbole und Snapshot

TYPED_TEST(MatchingTest, HasSymbol) {
    EXPECT_TRUE(this->engine.has_symbol(this->acme));
    EXPECT_FALSE(this->engine.has_symbol(kUnknown));
}

TYPED_TEST(MatchingTest, SnapshotOfUnknownSymbolIsEmpty) {
    this->new_limit(1, Side::Buy, 10000, 10);
    auto snap = this->engine.snapshot(kUnknown, 5);
    EXPECT_EQ(snap.symbol, kUnknown);
    EXPECT_TRUE(snap.bids.empty());
    EXPECT_TRUE(snap.asks.empty());
}

TYPED_TEST(MatchingTest, SnapshotRespectsDepth) {
    for (int i = 0; i < 7; ++i) {
        this->new_limit(static_cast<engine::OrderId>(1 + i), Side::Buy, 10000 - (10 * i), 1 + i);
        this->new_limit(static_cast<engine::OrderId>(101 + i), Side::Sell, 10100 + (10 * i), 10);
    }
    EXPECT_EQ(this->book(5),
              "BOOK,ACME,BIDS:10000x1;9990x2;9980x3;9970x4;9960x5,"
              "ASKS:10100x10;10110x10;10120x10;10130x10;10140x10");
    EXPECT_EQ(this->book(1), "BOOK,ACME,BIDS:10000x1,ASKS:10100x10");
    EXPECT_EQ(this->book(0), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(MatchingTest, SymbolsAreIsolatedAndSeqIsGlobal) {
    SymbolId beta = this->symbols.add("BETA");
    this->engine.add_symbol(beta);
    this->new_limit(1, Side::Sell, 10100, 10);
    this->new_order(2, beta, Side::Buy, OrderType::Limit, 10200, 10);  // darf ACME nicht kreuzen
    this->new_order(3, beta, Side::Sell, OrderType::Limit, 10200, 4);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,1,1",
                                "ACK,2,2",
                                "ACK,3,3",
                                "TRADE,BETA,10200,4,2,3,SELL",
                                "FILL,2,4,6",
                                "FILL,3,4,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x10");
    EXPECT_EQ(engine::test::book_line(this->symbols, this->engine.snapshot(beta, 5)), "BOOK,BETA,BIDS:10200x6,ASKS:");
}

// ---------------------------------------------------------------------------
// Tick-Array: nur Preise 1..EngineConfig::max_tick_price (sonst INVALID_PRICE)

class TickPriceBandTest : public EngineFixture<engine::TickMatchingEngine> {
protected:
    static constexpr Price kMaxTick = 1000;
    static engine::EngineConfig small_config() {
        engine::EngineConfig config;
        config.max_tick_price = kMaxTick;
        return config;
    }
    TickPriceBandTest() : EngineFixture<engine::TickMatchingEngine>(small_config()) {}
};

TEST_F(TickPriceBandTest, LimitAboveMaxTickPriceIsRejected) {
    new_limit(1, Side::Buy, kMaxTick + 1, 10);
    new_limit(2, Side::Sell, kMaxTick + 500, 10);
    new_limit(3, Side::Sell, std::numeric_limits<Price>::max(), 10);
    EXPECT_EQ(take(), (Events{"REJECT,1,INVALID_PRICE", "REJECT,2,INVALID_PRICE", "REJECT,3,INVALID_PRICE"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:,ASKS:");
}

TEST_F(TickPriceBandTest, BandLimitsAreAccepted) {
    new_limit(1, Side::Buy, 1, 10);
    new_limit(2, Side::Sell, kMaxTick, 10);
    EXPECT_EQ(take(), (Events{"ACK,1,1", "ACK,2,2"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:1x10,ASKS:1000x10");
}

TEST_F(TickPriceBandTest, RejectedOutOfBandIdStaysFreeAndGetsNoSeq) {
    new_limit(1, Side::Buy, kMaxTick + 1, 10);
    new_limit(1, Side::Buy, kMaxTick, 10);
    EXPECT_EQ(take(), (Events{"REJECT,1,INVALID_PRICE", "ACK,1,1"}));
}

TEST_F(TickPriceBandTest, MarketPriceIsNotCheckedAgainstBand) {
    new_limit(1, Side::Sell, 500, 10);
    take();
    new_market(2, Side::Buy, 10, /*price=*/kMaxTick + 1);
    EXPECT_EQ(take(), (Events{"ACK,2,2", "TRADE,ACME,500,10,2,1,BUY", "FILL,1,10,0", "FILL,2,10,0"}));
}

TEST_F(TickPriceBandTest, ValidationOrderQuantityBeforeBand) {
    new_limit(1, Side::Buy, kMaxTick + 1, 0);
    EXPECT_EQ(take(), (Events{"REJECT,1,INVALID_QUANTITY"}));
}

}  // namespace
