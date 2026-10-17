// CANCEL (FR-11) und MODIFY (FR-12): Testfälle aus plan.txt §9 plus Randfälle, als exakte
// Event-Sequenzen, typisiert über alle drei Engine-Varianten (Map/Vector/Tick).
// Semantik: Header-Kommentar in include/engine/matching_engine.hpp.

#include <gtest/gtest.h>

#include "engine/engine_config.hpp"
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

template <typename Engine>
class CancelTest : public EngineFixture<Engine> {};
TYPED_TEST_SUITE(CancelTest, EngineTypes, EngineTypeNames);

template <typename Engine>
class ModifyTest : public EngineFixture<Engine> {};
TYPED_TEST_SUITE(ModifyTest, EngineTypes, EngineTypeNames);

// ===========================================================================
// CANCEL

TYPED_TEST(CancelTest, CancelRestingOrderRemovesItAndEmptyLevel) {
    this->new_limit(1, Side::Buy, 10000, 50);
    this->new_limit(2, Side::Buy, 9900, 10);
    this->new_limit(3, Side::Sell, 10100, 5);
    this->take();
    this->cancel(1);
    EXPECT_EQ(this->take(), (Events{"CANCELLED,1,50,USER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:9900x10,ASKS:10100x5");
    this->cancel(3);
    EXPECT_EQ(this->take(), (Events{"CANCELLED,3,5,USER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:9900x10,ASKS:");
}

TYPED_TEST(CancelTest, CancelledOrderNoLongerMatches) {
    this->new_limit(1, Side::Sell, 10100, 10);
    this->cancel(1);
    this->new_limit(2, Side::Buy, 10100, 10);
    EXPECT_EQ(this->take(), (Events{"ACK,1,1", "CANCELLED,1,10,USER", "ACK,2,2"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10100x10,ASKS:");
}

TYPED_TEST(CancelTest, CancelUnknownIdIsRejected) {
    this->new_limit(1, Side::Buy, 10000, 50);
    this->take();
    this->cancel(42);
    EXPECT_EQ(this->take(), (Events{"REJECT,42,UNKNOWN_ORDER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x50,ASKS:");
}

TYPED_TEST(CancelTest, CancelOfRejectedNewIsUnknown) {
    this->new_limit(1, Side::Buy, 10000, 0);  // INVALID_QUANTITY, ID nie akzeptiert
    this->cancel(1);
    EXPECT_EQ(this->take(), (Events{"REJECT,1,INVALID_QUANTITY", "REJECT,1,UNKNOWN_ORDER"}));
}

TYPED_TEST(CancelTest, CancelFullyFilledOrderIsRejected) {
    this->new_limit(1, Side::Sell, 10100, 50);
    this->new_limit(2, Side::Buy, 10100, 50);
    this->take();
    this->cancel(1);  // resting, voll gefüllt
    this->cancel(2);  // Aggressor, voll gefüllt
    EXPECT_EQ(this->take(), (Events{"REJECT,1,UNKNOWN_ORDER", "REJECT,2,UNKNOWN_ORDER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(CancelTest, CancelAfterPartialFillReportsRemainder) {
    this->new_limit(1, Side::Buy, 10000, 100);
    this->new_limit(2, Side::Sell, 10000, 30);
    this->take();
    this->cancel(1);
    EXPECT_EQ(this->take(), (Events{"CANCELLED,1,70,USER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(CancelTest, CancelOfRestedAggressorRemainder) {
    this->new_limit(1, Side::Sell, 10100, 30);
    this->new_limit(2, Side::Buy, 10100, 50);  // 20 bleiben liegen
    this->take();
    this->cancel(2);
    EXPECT_EQ(this->take(), (Events{"CANCELLED,2,20,USER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(CancelTest, DoubleCancelIsRejected) {
    this->new_limit(1, Side::Buy, 10000, 50);
    this->take();
    this->cancel(1);
    this->cancel(1);
    EXPECT_EQ(this->take(), (Events{"CANCELLED,1,50,USER", "REJECT,1,UNKNOWN_ORDER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(CancelTest, CancelOfMarketIdIsRejected) {
    this->new_limit(1, Side::Sell, 10100, 10);
    this->new_market(2, Side::Buy, 25);  // 10 gehandelt, 15 NO_LIQUIDITY
    this->new_market(3, Side::Sell, 5);  // komplett NO_LIQUIDITY
    this->take();
    this->cancel(2);
    this->cancel(3);
    EXPECT_EQ(this->take(), (Events{"REJECT,2,UNKNOWN_ORDER", "REJECT,3,UNKNOWN_ORDER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(CancelTest, IdStaysLockedAfterCancel) {
    this->new_limit(1, Side::Buy, 10000, 50);
    this->cancel(1);
    this->new_limit(1, Side::Buy, 10000, 50);
    this->new_limit(1, Side::Sell, 9000, 5);
    EXPECT_EQ(this->take(),
              (Events{"ACK,1,1", "CANCELLED,1,50,USER", "REJECT,1,DUPLICATE_ID", "REJECT,1,DUPLICATE_ID"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(CancelTest, CancelConsumesNoSeq) {
    this->new_limit(1, Side::Buy, 10000, 10);
    this->cancel(1);
    this->cancel(77);
    this->new_limit(2, Side::Buy, 10000, 10);
    EXPECT_EQ(this->take(), (Events{"ACK,1,1", "CANCELLED,1,10,USER", "REJECT,77,UNKNOWN_ORDER", "ACK,2,2"}));
}

TYPED_TEST(CancelTest, CancelFromMiddleOfLevelKeepsFifoAndTotal) {
    this->new_limit(1, Side::Buy, 10000, 10);
    this->new_limit(2, Side::Buy, 10000, 20);
    this->new_limit(3, Side::Buy, 10000, 30);
    this->new_limit(4, Side::Buy, 9900, 40);
    this->take();
    this->cancel(2);
    EXPECT_EQ(this->take(), (Events{"CANCELLED,2,20,USER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x40;9900x40,ASKS:");

    this->new_limit(5, Side::Sell, 10000, 35);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,5,5",
                                "TRADE,ACME,10000,10,1,5,SELL",
                                "FILL,1,10,0",
                                "FILL,5,10,25",
                                "TRADE,ACME,10000,25,3,5,SELL",
                                "FILL,3,25,5",
                                "FILL,5,25,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x5;9900x40,ASKS:");
}

TYPED_TEST(CancelTest, CancelHeadAndTailOfLevel) {
    this->new_limit(1, Side::Sell, 10100, 10);
    this->new_limit(2, Side::Sell, 10100, 20);
    this->new_limit(3, Side::Sell, 10100, 30);
    this->take();
    this->cancel(1);
    this->cancel(3);
    EXPECT_EQ(this->take(), (Events{"CANCELLED,1,10,USER", "CANCELLED,3,30,USER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x20");
    this->cancel(2);
    EXPECT_EQ(this->take(), (Events{"CANCELLED,2,20,USER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
    // Level ist wirklich weg: neue Order legt es frisch an.
    this->new_limit(4, Side::Sell, 10100, 7);
    EXPECT_EQ(this->take(), (Events{"ACK,4,4"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x7");
}

TYPED_TEST(CancelTest, CancelWorksAcrossSymbols) {
    const SymbolId beta = this->symbols.add("BETA");
    this->engine.add_symbol(beta);
    this->new_order(1, beta, Side::Buy, OrderType::Limit, 10000, 10);
    this->new_limit(2, Side::Buy, 10000, 20);
    this->take();
    this->cancel(1);
    EXPECT_EQ(this->take(), (Events{"CANCELLED,1,10,USER"}));
    EXPECT_EQ(engine::test::book_line(this->symbols, this->engine.snapshot(beta, 5)), "BOOK,BETA,BIDS:,ASKS:");
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x20,ASKS:");
}

// ===========================================================================
// MODIFY: Rejects

TYPED_TEST(ModifyTest, ModifyUnknownIdIsRejected) {
    this->modify(42, 10000, 10);
    EXPECT_EQ(this->take(), (Events{"REJECT,42,UNKNOWN_ORDER"}));
}

TYPED_TEST(ModifyTest, ModifyFilledCancelledOrMarketIdIsRejected) {
    this->new_limit(1, Side::Sell, 10100, 10);
    this->new_limit(2, Side::Buy, 10100, 10);  // 1 und 2 voll gefüllt
    this->new_limit(3, Side::Buy, 9000, 10);
    this->cancel(3);
    this->new_market(4, Side::Buy, 10);  // NO_LIQUIDITY
    this->take();
    this->modify(1, 10100, 20);
    this->modify(2, 10100, 20);
    this->modify(3, 9000, 20);
    this->modify(4, 10100, 20);
    EXPECT_EQ(this->take(), (Events{"REJECT,1,UNKNOWN_ORDER", "REJECT,2,UNKNOWN_ORDER", "REJECT,3,UNKNOWN_ORDER",
                                    "REJECT,4,UNKNOWN_ORDER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(ModifyTest, NewQuantityEqualToFilledIsInvalidModify) {
    this->new_limit(1, Side::Buy, 10000, 100);
    this->new_limit(2, Side::Sell, 10000, 30);  // 1: filled 30, Rest 70
    this->take();
    this->modify(1, 10000, 30);
    this->modify(1, 10000, 10);
    this->modify(1, 9900, 30);
    EXPECT_EQ(this->take(), (Events{"REJECT,1,INVALID_MODIFY", "REJECT,1,INVALID_MODIFY", "REJECT,1,INVALID_MODIFY"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x70,ASKS:");
    this->cancel(1);  // Order unverändert
    EXPECT_EQ(this->take(), (Events{"CANCELLED,1,70,USER"}));
}

TYPED_TEST(ModifyTest, ZeroQuantityIsInvalidModify) {
    this->new_limit(1, Side::Buy, 10000, 100);
    this->take();
    this->modify(1, 10000, 0);
    EXPECT_EQ(this->take(), (Events{"REJECT,1,INVALID_MODIFY"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x100,ASKS:");
}

TYPED_TEST(ModifyTest, NonPositivePriceIsInvalidPrice) {
    this->new_limit(1, Side::Sell, 10100, 50);
    this->take();
    this->modify(1, 0, 50);
    this->modify(1, -10, 20);
    EXPECT_EQ(this->take(), (Events{"REJECT,1,INVALID_PRICE", "REJECT,1,INVALID_PRICE"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x50");
}

TYPED_TEST(ModifyTest, ValidationOrder) {
    this->new_limit(1, Side::Buy, 10000, 100);
    this->new_limit(2, Side::Sell, 10000, 40);  // 1: filled 40, Rest 60
    this->take();
    // Unbekannt + Menge 0 + Preis 0: UNKNOWN_ORDER.
    this->modify(99, 0, 0);
    // Menge <= filled + Preis 0: INVALID_MODIFY.
    this->modify(1, 0, 40);
    // Menge 0 + negativer Preis: INVALID_MODIFY.
    this->modify(1, -1, 0);
    // Gültige Menge + Preis 0: INVALID_PRICE.
    this->modify(1, 0, 41);
    // Gefüllte Order (2) + ungültige Menge: UNKNOWN_ORDER.
    this->modify(2, 10000, 0);
    EXPECT_EQ(this->take(), (Events{
                                "REJECT,99,UNKNOWN_ORDER",
                                "REJECT,1,INVALID_MODIFY",
                                "REJECT,1,INVALID_MODIFY",
                                "REJECT,1,INVALID_PRICE",
                                "REJECT,2,UNKNOWN_ORDER",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x60,ASKS:");
}

TYPED_TEST(ModifyTest, RejectedModifyKeepsPriorityAndConsumesNoSeq) {
    this->new_limit(1, Side::Sell, 10100, 10);
    this->new_limit(2, Side::Sell, 10100, 10);
    this->take();
    this->modify(1, 0, 20);  // INVALID_PRICE, 1 bleibt vorne
    this->new_limit(3, Side::Buy, 10100, 10);
    EXPECT_EQ(this->take(), (Events{
                                "REJECT,1,INVALID_PRICE",
                                "ACK,3,3",
                                "TRADE,ACME,10100,10,3,1,BUY",
                                "FILL,1,10,0",
                                "FILL,3,10,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x10");
}

// ===========================================================================
// MODIFY: Priorität

TYPED_TEST(ModifyTest, ReductionKeepsPriority) {
    this->new_limit(1, Side::Sell, 10100, 50);
    this->new_limit(2, Side::Sell, 10100, 50);  // jünger, gleicher Preis
    this->take();
    this->modify(1, 10100, 20);
    EXPECT_EQ(this->take(), (Events{"ACK,1,1"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x70");

    this->new_limit(3, Side::Buy, 10100, 30);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,3,3",
                                "TRADE,ACME,10100,20,3,1,BUY",
                                "FILL,1,20,0",
                                "FILL,3,20,10",
                                "TRADE,ACME,10100,10,3,2,BUY",
                                "FILL,2,10,40",
                                "FILL,3,10,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x40");
}

TYPED_TEST(ModifyTest, ReductionOnBidSideKeepsPriority) {
    this->new_limit(1, Side::Buy, 10000, 40);
    this->new_limit(2, Side::Buy, 10000, 40);
    this->take();
    this->modify(1, 10000, 5);
    this->new_limit(3, Side::Sell, 10000, 10);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,1,1",
                                "ACK,3,3",
                                "TRADE,ACME,10000,5,1,3,SELL",
                                "FILL,1,5,0",
                                "FILL,3,5,5",
                                "TRADE,ACME,10000,5,2,3,SELL",
                                "FILL,2,5,35",
                                "FILL,3,5,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x35,ASKS:");
}

TYPED_TEST(ModifyTest, SameQuantitySamePriceIsNoOpWithOldSeq) {
    this->new_limit(1, Side::Sell, 10100, 50);
    this->new_limit(2, Side::Sell, 10100, 50);
    this->take();
    this->modify(1, 10100, 50);
    this->new_limit(3, Side::Buy, 10100, 10);  // keine Seq verbraucht -> 3
    EXPECT_EQ(this->take(), (Events{
                                "ACK,1,1",
                                "ACK,3,3",
                                "TRADE,ACME,10100,10,3,1,BUY",
                                "FILL,1,10,40",
                                "FILL,3,10,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x90");
}

TYPED_TEST(ModifyTest, IncreaseLosesPriority) {
    this->new_limit(1, Side::Sell, 10100, 50);
    this->new_limit(2, Side::Sell, 10100, 50);
    this->take();
    this->modify(1, 10100, 60);
    EXPECT_EQ(this->take(), (Events{"ACK,1,3"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x110");

    this->new_limit(3, Side::Buy, 10100, 60);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,3,4",
                                "TRADE,ACME,10100,50,3,2,BUY",
                                "FILL,2,50,0",
                                "FILL,3,50,10",
                                "TRADE,ACME,10100,10,3,1,BUY",
                                "FILL,1,10,50",
                                "FILL,3,10,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x50");
}

TYPED_TEST(ModifyTest, PriceChangeWithoutCrossingLosesPriority) {
    this->new_limit(1, Side::Buy, 10000, 10);
    this->new_limit(2, Side::Buy, 9900, 10);
    this->new_limit(3, Side::Sell, 10200, 10);
    this->take();
    this->modify(1, 9900, 10);
    EXPECT_EQ(this->take(), (Events{"ACK,1,4"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:9900x20,ASKS:10200x10");

    this->new_limit(4, Side::Sell, 9900, 15);  // 2 ist jetzt älter als 1 auf 9900
    EXPECT_EQ(this->take(), (Events{
                                "ACK,4,5",
                                "TRADE,ACME,9900,10,2,4,SELL",
                                "FILL,2,10,0",
                                "FILL,4,10,5",
                                "TRADE,ACME,9900,5,1,4,SELL",
                                "FILL,1,5,5",
                                "FILL,4,5,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:9900x5,ASKS:10200x10");
}

TYPED_TEST(ModifyTest, PriceChangeWithReductionLosesPriority) {
    this->new_limit(1, Side::Sell, 10200, 50);
    this->new_limit(2, Side::Sell, 10100, 50);
    this->take();
    this->modify(1, 10100, 30);  // anderer Preis -> hinter 2
    EXPECT_EQ(this->take(), (Events{"ACK,1,3"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x80");

    this->new_limit(3, Side::Buy, 10100, 60);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,3,4",
                                "TRADE,ACME,10100,50,3,2,BUY",
                                "FILL,2,50,0",
                                "FILL,3,50,10",
                                "TRADE,ACME,10100,10,3,1,BUY",
                                "FILL,1,10,20",
                                "FILL,3,10,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x20");
}

// ===========================================================================
// MODIFY: sofortiges Matching

TYPED_TEST(ModifyTest, RepriceSweepsMultipleLevelsAndRestsRemainder) {
    this->new_limit(1, Side::Sell, 10100, 10);
    this->new_limit(2, Side::Sell, 10200, 20);
    this->new_limit(3, Side::Sell, 10300, 30);
    this->new_limit(4, Side::Buy, 10000, 50);
    this->take();
    this->modify(4, 10250, 50);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,4,5",
                                "TRADE,ACME,10100,10,4,1,BUY",
                                "FILL,1,10,0",
                                "FILL,4,10,40",
                                "TRADE,ACME,10200,20,4,2,BUY",
                                "FILL,2,20,0",
                                "FILL,4,20,20",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10250x20,ASKS:10300x30");

    this->new_limit(5, Side::Buy, 10250, 1);  // Seq läuft nach dem Modify weiter
    EXPECT_EQ(this->take(), (Events{"ACK,5,6"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10250x21,ASKS:10300x30");
}

TYPED_TEST(ModifyTest, RepriceSellSweepsBidsUntilFilled) {
    this->new_limit(1, Side::Buy, 10000, 10);
    this->new_limit(2, Side::Buy, 9900, 10);
    this->new_limit(3, Side::Sell, 10100, 15);
    this->take();
    this->modify(3, 9900, 15);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,3,4",
                                "TRADE,ACME,10000,10,1,3,SELL",
                                "FILL,1,10,0",
                                "FILL,3,10,5",
                                "TRADE,ACME,9900,5,2,3,SELL",
                                "FILL,2,5,5",
                                "FILL,3,5,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:9900x5,ASKS:");
    this->cancel(3);  // voll gefüllt -> nicht mehr liegend
    EXPECT_EQ(this->take(), (Events{"REJECT,3,UNKNOWN_ORDER"}));
}

TYPED_TEST(ModifyTest, IncreaseAtSamePriceDoesNotCrossOwnSide) {
    this->new_limit(1, Side::Buy, 10000, 10);
    this->new_limit(2, Side::Sell, 10100, 10);
    this->take();
    this->modify(1, 10000, 25);
    EXPECT_EQ(this->take(), (Events{"ACK,1,3"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x25,ASKS:10100x10");
}

// ===========================================================================
// MODIFY: teilgefüllte Orders (new_quantity = Gesamtmenge inkl. filled)

TYPED_TEST(ModifyTest, ReductionOfPartiallyFilledOrder) {
    this->new_limit(1, Side::Buy, 10000, 100);
    this->new_limit(2, Side::Sell, 10000, 30);  // 1: filled 30, Rest 70
    this->new_limit(3, Side::Buy, 10000, 10);   // jünger auf demselben Level
    this->take();
    this->modify(1, 10000, 50);  // Gesamt 50 -> Rest 20, Priorität bleibt
    EXPECT_EQ(this->take(), (Events{"ACK,1,1"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x30,ASKS:");

    this->new_limit(4, Side::Sell, 10000, 25);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,4,4",
                                "TRADE,ACME,10000,20,1,4,SELL",
                                "FILL,1,20,0",
                                "FILL,4,20,5",
                                "TRADE,ACME,10000,5,3,4,SELL",
                                "FILL,3,5,5",
                                "FILL,4,5,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x5,ASKS:");
}

TYPED_TEST(ModifyTest, ReductionToJustAboveFilled) {
    this->new_limit(1, Side::Sell, 10100, 100);
    this->new_limit(2, Side::Buy, 10100, 60);  // 1: filled 60, Rest 40
    this->take();
    this->modify(1, 10100, 61);
    EXPECT_EQ(this->take(), (Events{"ACK,1,1"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x1");
    this->cancel(1);
    EXPECT_EQ(this->take(), (Events{"CANCELLED,1,1,USER"}));
}

TYPED_TEST(ModifyTest, IncreaseOfPartiallyFilledOrder) {
    this->new_limit(1, Side::Buy, 10000, 100);
    this->new_limit(2, Side::Sell, 10000, 30);  // 1: filled 30, Rest 70
    this->take();
    this->modify(1, 10000, 130);  // Gesamt 130 -> Rest 100, neue Seq
    EXPECT_EQ(this->take(), (Events{"ACK,1,3"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:10000x100,ASKS:");
    this->new_limit(3, Side::Sell, 10000, 100);
    EXPECT_EQ(this->take(), (Events{
                                "ACK,3,4",
                                "TRADE,ACME,10000,100,1,3,SELL",
                                "FILL,1,100,0",
                                "FILL,3,100,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(ModifyTest, RepriceOfPartiallyFilledOrderMatchesWithNewRemainder) {
    this->new_limit(1, Side::Buy, 10000, 100);
    this->new_limit(2, Side::Sell, 10000, 30);  // 1: filled 30, Rest 70
    this->new_limit(3, Side::Sell, 10100, 50);
    this->take();
    this->modify(1, 10100, 60);  // Gesamt 60 -> Rest 30, kreuzt mit 3
    EXPECT_EQ(this->take(), (Events{
                                "ACK,1,4",
                                "TRADE,ACME,10100,30,1,3,BUY",
                                "FILL,3,30,20",
                                "FILL,1,30,0",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x20");
}

// ===========================================================================
// MODIFY: Seq-Vergabe und Buch

TYPED_TEST(ModifyTest, SeqAssignmentAcrossModifies) {
    this->new_limit(1, Side::Buy, 10000, 10);   // Seq 1
    this->new_limit(2, Side::Buy, 9900, 10);    // Seq 2
    this->modify(1, 9950, 10);                  // Preisänderung -> Seq 3
    this->modify(2, 9900, 5);                   // Reduktion -> alte Seq 2
    this->new_limit(3, Side::Buy, 9800, 10);    // Seq 4
    this->modify(1, 9950, 8);                   // Reduktion -> Seq 3 (die neue)
    this->modify(99, 9950, 8);                  // REJECT, keine Seq
    this->modify(1, 9950, 0);                   // REJECT, keine Seq
    this->modify(2, 9900, 6);                   // Erhöhung -> Seq 5
    this->new_limit(4, Side::Sell, 10500, 10);  // Seq 6
    EXPECT_EQ(this->take(), (Events{
                                "ACK,1,1",
                                "ACK,2,2",
                                "ACK,1,3",
                                "ACK,2,2",
                                "ACK,3,4",
                                "ACK,1,3",
                                "REJECT,99,UNKNOWN_ORDER",
                                "REJECT,1,INVALID_MODIFY",
                                "ACK,2,5",
                                "ACK,4,6",
                            }));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:9950x8;9900x6;9800x10,ASKS:10500x10");
}

TYPED_TEST(ModifyTest, BookQuantitiesAfterModifyAndCancel) {
    this->new_limit(1, Side::Sell, 10100, 10);
    this->new_limit(2, Side::Sell, 10100, 20);
    this->new_limit(3, Side::Sell, 10200, 30);
    this->take();
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x30;10200x30");
    this->modify(2, 10100, 5);  // Level-Total sinkt
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x15;10200x30");
    this->modify(3, 10100, 30);  // Level 10200 verschwindet
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x45");
    this->cancel(1);
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x35");
    this->modify(2, 10300, 5);
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10100x30;10300x5");
    this->cancel(3);
    this->cancel(2);
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
    EXPECT_EQ(this->take(), (Events{
                                "ACK,2,2",
                                "ACK,3,4",
                                "CANCELLED,1,10,USER",
                                "ACK,2,5",
                                "CANCELLED,3,30,USER",
                                "CANCELLED,2,5,USER",
                            }));
}

TYPED_TEST(ModifyTest, ModifyThenCancelReportsNewRemainder) {
    this->new_limit(1, Side::Buy, 10000, 100);
    this->modify(1, 10000, 40);
    this->cancel(1);
    this->modify(1, 10000, 50);  // nach Cancel nicht mehr liegend
    EXPECT_EQ(this->take(), (Events{"ACK,1,1", "ACK,1,1", "CANCELLED,1,40,USER", "REJECT,1,UNKNOWN_ORDER"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:");
}

TYPED_TEST(ModifyTest, ModifyStaysInOwnSymbol) {
    const SymbolId beta = this->symbols.add("BETA");
    this->engine.add_symbol(beta);
    this->new_limit(1, Side::Sell, 10000, 10);                        // ACME
    this->new_order(2, beta, Side::Buy, OrderType::Limit, 9000, 10);  // BETA
    this->modify(2, 10500, 10);                                       // darf ACME nicht kreuzen
    EXPECT_EQ(this->take(), (Events{"ACK,1,1", "ACK,2,2", "ACK,2,3"}));
    EXPECT_EQ(this->book(), "BOOK,ACME,BIDS:,ASKS:10000x10");
    EXPECT_EQ(engine::test::book_line(this->symbols, this->engine.snapshot(beta, 5)), "BOOK,BETA,BIDS:10500x10,ASKS:");
}

// ===========================================================================
// Tick-Array: MODIFY außerhalb des Preisbands -> INVALID_PRICE

class TickModifyTest : public EngineFixture<engine::TickMatchingEngine> {
protected:
    static constexpr Price kMaxTick = 1000;
    static engine::EngineConfig small_config() {
        engine::EngineConfig config;
        config.max_tick_price = kMaxTick;
        return config;
    }
    TickModifyTest() : EngineFixture<engine::TickMatchingEngine>(small_config()) {}
};

TEST_F(TickModifyTest, ModifyAboveMaxTickPriceIsInvalidPrice) {
    new_limit(1, Side::Buy, 900, 10);
    new_limit(2, Side::Buy, 900, 10);
    take();
    modify(1, kMaxTick + 1, 10);
    modify(1, kMaxTick + 1, 5);
    EXPECT_EQ(take(), (Events{"REJECT,1,INVALID_PRICE", "REJECT,1,INVALID_PRICE"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:900x20,ASKS:");
    // Order unverändert und weiter vorne.
    new_limit(3, Side::Sell, 900, 10);
    EXPECT_EQ(take(), (Events{"ACK,3,3", "TRADE,ACME,900,10,1,3,SELL", "FILL,1,10,0", "FILL,3,10,0"}));
}

TEST_F(TickModifyTest, ModifyToMaxTickPriceIsAccepted) {
    new_limit(1, Side::Buy, 900, 10);
    modify(1, kMaxTick, 10);
    EXPECT_EQ(take(), (Events{"ACK,1,1", "ACK,1,2"}));
    EXPECT_EQ(book(), "BOOK,ACME,BIDS:1000x10,ASKS:");
}

TEST_F(TickModifyTest, InvalidModifyBeatsOutOfBandPrice) {
    new_limit(1, Side::Buy, 900, 10);
    take();
    modify(1, kMaxTick + 1, 0);
    EXPECT_EQ(take(), (Events{"REJECT,1,INVALID_MODIFY"}));
}

}  // namespace
