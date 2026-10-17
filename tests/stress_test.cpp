// Pipeline-Stresstest (plan.txt §9 letzter Punkt, §10 Stufe 2):
// - mehrere Producer-Threads mit zufälligen (fest geseedeten) Orders, Prüfung globaler
//   Invarianten allein aus der aufgezeichneten Event-Folge;
// - doppelte IDs über Producer hinweg;
// - Determinismus: ein Producer liefert dieselbe Event-Folge wie MatchingEngine direkt.
// Jeder Test läuft kIterations-mal mit frischer Pipeline, um Races zu provozieren.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "engine/event.hpp"
#include "engine/event_sink.hpp"
#include "engine/matching_engine.hpp"
#include "engine/pipeline.hpp"
#include "engine/types.hpp"

namespace {

using engine::AckEvent;
using engine::AddSymbolRequest;
using engine::BookLevel;
using engine::BookRequest;
using engine::BookSnapshot;
using engine::CancelledEvent;
using engine::CancelOrder;
using engine::CancelReason;
using engine::Command;
using engine::Event;
using engine::EventSink;
using engine::FillEvent;
using engine::MatchingEngine;
using engine::NewOrder;
using engine::OrderId;
using engine::OrderType;
using engine::Pipeline;
using engine::Price;
using engine::Quantity;
using engine::RejectEvent;
using engine::RejectReason;
using engine::Request;
using engine::SeqNo;
using engine::Side;
using engine::SymbolId;
using engine::Trade;
using engine::TradeEvent;

// ---------------------------------------------------------------------------
// Zentrale Konstanten

constexpr int kIterations = 3;
constexpr int kProducers = 4;
constexpr int kOrdersPerProducer = 20'000;
constexpr OrderId kIdRangePerProducer = 10'000'000;  // Producer p: IDs ab (p + 1) * kIdRangePerProducer
constexpr int kSharedIds = 2'000;                    // Duplicate-Test: IDs 1..kSharedIds von allen Producern
constexpr int kDeterminismRequests = 20'000;
constexpr std::uint64_t kBaseSeed = 20261017;

constexpr std::array<SymbolId, 2> kSymbols{1, 2};
constexpr SymbolId kLateSymbol = 3;      // Determinismus: wird erst mitten im Lauf hinzugefügt
constexpr SymbolId kUnknownSymbol = 99;  // nie hinzugefügt
constexpr Price kMidPrice = 1000;
constexpr Price kPriceSpread = 8;  // Limits in [mid - spread, mid + spread]
constexpr Quantity kMaxQuantity = 50;
constexpr int kMarketPercent = 15;
constexpr int kZeroQuantityPercent = 1;  // erzeugt INVALID_QUANTITY-Rejects
constexpr std::size_t kMaxReportedFailures = 20;

// ---------------------------------------------------------------------------
// Sink: zeichnet alle Events strukturiert auf. Läuft nur im Logger-Thread; nach stop()
// (joint den Logger) liest der Test-Thread ohne Lock.

class CollectingSink final : public EventSink {
public:
    void on_ack(OrderId id, SeqNo seq) override { events.emplace_back(AckEvent{id, seq}); }
    void on_reject(OrderId id, RejectReason reason) override { events.emplace_back(RejectEvent{id, reason}); }
    void on_trade(const Trade& trade) override { events.emplace_back(TradeEvent{trade}); }
    void on_fill(OrderId id, Quantity filled, Quantity remaining) override {
        events.emplace_back(FillEvent{id, filled, remaining});
    }
    void on_cancelled(OrderId id, Quantity remaining, CancelReason reason) override {
        events.emplace_back(CancelledEvent{id, remaining, reason});
    }
    void on_book(const BookSnapshot& snapshot) override { books.push_back(snapshot); }

    std::vector<Event> events;  // ohne BookEvents
    std::vector<BookSnapshot> books;
};

// Text-Sink für den Determinismus-Vergleich (gleiches Format für Pipeline und Engine direkt).
class TextSink final : public EventSink {
public:
    void on_ack(OrderId id, SeqNo seq) override { lines.push_back("ACK," + num(id) + "," + num(seq)); }
    void on_reject(OrderId id, RejectReason reason) override {
        lines.push_back("REJECT," + num(id) + "," + std::string(to_string(reason)));
    }
    void on_trade(const Trade& t) override {
        lines.push_back("TRADE," + num(t.symbol) + "," + num(t.price) + "," + num(t.quantity) + "," +
                        num(t.buy_order_id) + "," + num(t.sell_order_id) + "," + std::string(to_string(t.aggressor)));
    }
    void on_fill(OrderId id, Quantity filled, Quantity remaining) override {
        lines.push_back("FILL," + num(id) + "," + num(filled) + "," + num(remaining));
    }
    void on_cancelled(OrderId id, Quantity remaining, CancelReason reason) override {
        lines.push_back("CANCELLED," + num(id) + "," + num(remaining) + "," + std::string(to_string(reason)));
    }
    void on_book(const BookSnapshot& s) override {
        std::string line = "BOOK," + num(s.symbol) + ",BIDS:" + levels(s.bids) + ",ASKS:" + levels(s.asks);
        lines.push_back(std::move(line));
    }

    std::vector<std::string> lines;

private:
    template <typename T>
    static std::string num(T value) {
        return std::to_string(value);
    }
    static std::string levels(const std::vector<BookLevel>& ls) {
        std::string out;
        for (const BookLevel& l : ls) {
            out += num(l.price) + "x" + num(l.quantity) + ";";
        }
        return out;
    }
};

// ---------------------------------------------------------------------------
// Order-Erzeugung

int percent(std::mt19937_64& rng) { return static_cast<int>(rng() % 100); }

NewOrder random_order(std::mt19937_64& rng, OrderId id) {
    NewOrder o{};
    o.id = id;
    o.symbol = kSymbols[rng() % kSymbols.size()];
    o.side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
    o.type = percent(rng) < kMarketPercent ? OrderType::Market : OrderType::Limit;
    o.price =
        o.type == OrderType::Market
            ? 0
            : kMidPrice - kPriceSpread + static_cast<Price>(rng() % static_cast<std::uint64_t>(2 * kPriceSpread + 1));
    o.quantity = percent(rng) < kZeroQuantityPercent ? 0 : 1 + rng() % kMaxQuantity;
    return o;
}

// Erwarteter Reject-Grund bei Erstverarbeitung (ohne DUPLICATE_ID), nullopt = gültig.
std::optional<RejectReason> expected_reject(const NewOrder& o) {
    if (std::find(kSymbols.begin(), kSymbols.end(), o.symbol) == kSymbols.end()) {
        return RejectReason::UnknownSymbol;
    }
    if (o.quantity == 0) {
        return RejectReason::InvalidQuantity;
    }
    if (o.type == OrderType::Limit && o.price <= 0) {
        return RejectReason::InvalidPrice;
    }
    return std::nullopt;
}

struct SentOrder {
    NewOrder order;
    int copies = 0;  // wie oft (über alle Producer) gesendet
};

// Was die Producer gesendet haben (vor dem Start komplett erzeugt).
struct Workload {
    std::vector<std::vector<NewOrder>> per_producer;
    std::unordered_map<OrderId, SentOrder> orders;

    void add(std::size_t producer, const NewOrder& order) {
        per_producer[producer].push_back(order);
        SentOrder& sent = orders[order.id];
        sent.order = order;
        ++sent.copies;
    }
};

// Läuft eine Pipeline mit einem Thread pro Producer-Liste; AddSymbol vorher und BookRequests
// nachher aus dem Test-Thread. Alle Producer starten gleichzeitig (Latch), um Races zu provozieren.
void run_pipeline(const Workload& workload, CollectingSink& sink) {
    Pipeline pipeline(sink);
    pipeline.start();
    for (SymbolId s : kSymbols) {
        pipeline.submit(Request{AddSymbolRequest{s}});
    }

    std::latch go(static_cast<std::ptrdiff_t>(workload.per_producer.size()));
    std::vector<std::thread> producers;
    producers.reserve(workload.per_producer.size());
    for (const std::vector<NewOrder>& orders : workload.per_producer) {
        producers.emplace_back([&pipeline, &orders, &go] {
            go.arrive_and_wait();
            for (const NewOrder& o : orders) {
                pipeline.submit(Request{Command{o}});
            }
        });
    }
    for (std::thread& t : producers) {
        t.join();
    }
    for (SymbolId s : kSymbols) {
        pipeline.submit(Request{BookRequest{s, static_cast<std::uint32_t>(engine::kMaxBookDepth)}});
    }
    pipeline.stop();
}

// ---------------------------------------------------------------------------
// Invarianten-Prüfung aus der Event-Folge

class Checker {
public:
    explicit Checker(const Workload& workload) : workload_(workload) {}

    void check(const CollectingSink& sink) {
        const std::vector<Event>& ev = sink.events;
        for (std::size_t i = 0; i < ev.size(); ++i) {
            std::visit([&](const auto& e) { on_event(i, ev, e); }, ev[i]);
        }
        check_per_order();
        check_seqs();
        check_totals();
        check_producer_order();
        check_books(sink.books);
        if (failures_ > kMaxReportedFailures) {
            ADD_FAILURE() << (failures_ - kMaxReportedFailures) << " weitere Verletzungen nicht ausgegeben";
        }
        EXPECT_EQ(failures_, 0U) << "Invarianten verletzt";
    }

private:
    struct State {
        int acks = 0;
        int duplicate_rejects = 0;
        int other_rejects = 0;
        std::optional<RejectReason> other_reason;
        std::size_t first_response = std::numeric_limits<std::size_t>::max();
        Quantity remaining = 0;
        Quantity filled = 0;
        Quantity cancelled = 0;
        int cancel_events = 0;
    };

    template <typename... Args>
    void fail(const Args&... args) {
        ++failures_;
        if (failures_ <= kMaxReportedFailures) {
            std::string msg;
            ((msg += to_text(args)), ...);
            ADD_FAILURE() << msg;
        }
    }
    template <typename T>
    static std::string to_text(const T& value) {
        if constexpr (std::is_arithmetic_v<T>) {
            return std::to_string(value);
        } else {
            return std::string(value);
        }
    }

    const SentOrder* sent(OrderId id) const {
        auto it = workload_.orders.find(id);
        return it == workload_.orders.end() ? nullptr : &it->second;
    }

    // Order muss gesendet und geACKt sein.
    const SentOrder* accepted(std::size_t i, OrderId id, const char* what) {
        const SentOrder* s = sent(id);
        if (s == nullptr) {
            fail("Event ", i, ": ", what, " für nie gesendete Order ", id);
            return nullptr;
        }
        if (states_[id].acks != 1) {
            fail("Event ", i, ": ", what, " für nicht (genau einmal) geACKte Order ", id);
            return nullptr;
        }
        return s;
    }

    void on_event(std::size_t i, const std::vector<Event>& /*ev*/, const AckEvent& e) {
        if (sent(e.id) == nullptr) {
            fail("Event ", i, ": ACK für nie gesendete Order ", e.id);
            return;
        }
        State& st = states_[e.id];
        ++st.acks;
        st.first_response = std::min(st.first_response, i);
        st.remaining = workload_.orders.at(e.id).order.quantity;
        seqs_.push_back(e.seq);
        last_acked_ = e.id;
    }

    void on_event(std::size_t i, const std::vector<Event>& /*ev*/, const RejectEvent& e) {
        if (sent(e.id) == nullptr) {
            fail("Event ", i, ": REJECT für nie gesendete Order ", e.id);
            return;
        }
        State& st = states_[e.id];
        st.first_response = std::min(st.first_response, i);
        if (e.reason == RejectReason::DuplicateId) {
            ++st.duplicate_rejects;
        } else {
            ++st.other_rejects;
            st.other_reason = e.reason;
        }
    }

    void on_event(std::size_t i, const std::vector<Event>& ev, const TradeEvent& e) {
        const Trade& t = e.trade;
        ++trades_;
        traded_ += t.quantity;
        if (t.quantity == 0) {
            fail("Event ", i, ": TRADE mit Menge 0");
        }
        const OrderId aggressor = t.aggressor == Side::Buy ? t.buy_order_id : t.sell_order_id;
        const OrderId resting = t.aggressor == Side::Buy ? t.sell_order_id : t.buy_order_id;
        if (aggressor != last_acked_) {
            fail("Event ", i, ": Aggressor ", aggressor, " ist nicht die zuletzt geACKte Order ", last_acked_);
        }

        // Genau zwei FILLs direkt dahinter: resting, dann Aggressor, jeweils mit der Trade-Menge.
        if (i + 2 >= ev.size() || !std::holds_alternative<FillEvent>(ev[i + 1]) ||
            !std::holds_alternative<FillEvent>(ev[i + 2])) {
            fail("Event ", i, ": TRADE nicht von genau zwei FILLs gefolgt");
        } else {
            const auto& f1 = std::get<FillEvent>(ev[i + 1]);
            const auto& f2 = std::get<FillEvent>(ev[i + 2]);
            if (f1.id != resting || f2.id != aggressor || f1.filled != t.quantity || f2.filled != t.quantity) {
                fail("Event ", i, ": FILLs passen nicht zum TRADE (", f1.id, "/", f1.filled, ", ", f2.id, "/",
                     f2.filled, ")");
            }
            if (i + 3 < ev.size() && std::holds_alternative<FillEvent>(ev[i + 3])) {
                fail("Event ", i, ": mehr als zwei FILLs nach TRADE");
            }
        }

        const SentOrder* buy = accepted(i, t.buy_order_id, "TRADE (Käufer)");
        const SentOrder* sell = accepted(i, t.sell_order_id, "TRADE (Verkäufer)");
        if (buy == nullptr || sell == nullptr) {
            return;
        }
        if (buy->order.side != Side::Buy || sell->order.side != Side::Sell) {
            fail("Event ", i, ": Seiten im TRADE vertauscht");
        }
        if (buy->order.symbol != t.symbol || sell->order.symbol != t.symbol) {
            fail("Event ", i, ": TRADE über Symbolgrenzen");
        }
        const NewOrder& rest = (resting == t.buy_order_id ? buy : sell)->order;
        const NewOrder& aggr = (aggressor == t.buy_order_id ? buy : sell)->order;
        if (rest.type != OrderType::Limit) {
            fail("Event ", i, ": resting Order ", rest.id, " ist keine LIMIT-Order");
        } else if (t.price != rest.price) {
            fail("Event ", i, ": Trade-Preis ", t.price, " != Preis der resting Order ", rest.price);
        }
        if (aggr.type == OrderType::Limit) {
            const bool worse = aggr.side == Side::Buy ? t.price > aggr.price : t.price < aggr.price;
            if (worse) {
                fail("Event ", i, ": Trade-Preis ", t.price, " schlechter als Limit ", aggr.price, " des Aggressors ",
                     aggr.id);
            }
        }
    }

    void on_event(std::size_t i, const std::vector<Event>& /*ev*/, const FillEvent& e) {
        ++fills_;
        const SentOrder* s = accepted(i, e.id, "FILL");
        if (s == nullptr) {
            return;
        }
        State& st = states_[e.id];
        if (st.remaining == 0) {
            fail("Event ", i, ": FILL für Order ", e.id, " nach Restmenge 0");
            return;
        }
        if (e.filled == 0 || e.filled > st.remaining) {
            fail("Event ", i, ": FILL-Menge ", e.filled, " ungültig bei Rest ", st.remaining, " (Order ", e.id, ")");
            return;
        }
        st.remaining -= e.filled;
        st.filled += e.filled;
        if (e.remaining != st.remaining) {
            fail("Event ", i, ": FILL meldet Rest ", e.remaining, ", erwartet ", st.remaining, " (Order ", e.id, ")");
        }
        (s->order.side == Side::Buy ? buy_filled_ : sell_filled_) += e.filled;
    }

    void on_event(std::size_t i, const std::vector<Event>& /*ev*/, const CancelledEvent& e) {
        const SentOrder* s = accepted(i, e.id, "CANCELLED");
        if (s == nullptr) {
            return;
        }
        State& st = states_[e.id];
        ++st.cancel_events;
        if (s->order.type != OrderType::Market || e.reason != CancelReason::NoLiquidity) {
            fail("Event ", i, ": unerwartetes CANCELLED für Order ", e.id);
        }
        if (e.id != last_acked_) {
            fail("Event ", i, ": CANCELLED für ", e.id, " nicht direkt nach deren Verarbeitung");
        }
        if (e.remaining == 0 || e.remaining != st.remaining) {
            fail("Event ", i, ": CANCELLED-Rest ", e.remaining, ", erwartet ", st.remaining, " (Order ", e.id, ")");
        }
        st.cancelled += e.remaining;
        st.remaining = 0;
    }

    void on_event(std::size_t i, const std::vector<Event>& /*ev*/, const engine::BookEvent& /*e*/) {
        fail("Event ", i, ": BookEvent im Event-Strom (Sink speichert Books separat)");
    }

    void check_per_order() {
        for (const auto& [id, s] : workload_.orders) {
            const State st = states_.contains(id) ? states_.at(id) : State{};
            const std::optional<RejectReason> invalid = expected_reject(s.order);
            if (invalid) {
                if (st.acks != 0 || st.duplicate_rejects != 0 || st.other_rejects != s.copies ||
                    st.other_reason != invalid) {
                    fail("Order ", id, ": erwartet ", s.copies, "x REJECT ", to_string(*invalid), ", erhalten ",
                         st.acks, " ACK, ", st.other_rejects, " andere/", st.duplicate_rejects, " DUPLICATE_ID");
                }
                continue;
            }
            if (st.acks != 1 || st.duplicate_rejects != s.copies - 1 || st.other_rejects != 0) {
                fail("Order ", id, ": erwartet 1 ACK + ", s.copies - 1, " DUPLICATE_ID, erhalten ", st.acks, " ACK, ",
                     st.duplicate_rejects, " DUPLICATE_ID, ", st.other_rejects, " andere REJECTs");
                continue;
            }
            // Fills + CANCELLED-Rest + Buchrest == Ursprungsmenge. LIMIT: Buchrest = Menge - Fills,
            // nie CANCELLED (kein CANCEL in diesem Test). MARKET: Buchrest 0, Rest muss CANCELLED sein.
            const Quantity qty = s.order.quantity;
            if (s.order.type == OrderType::Market) {
                if (st.filled + st.cancelled != qty || st.remaining != 0 || st.cancel_events != (st.cancelled > 0)) {
                    fail("MARKET ", id, ": Menge ", qty, " != Fills ", st.filled, " + CANCELLED ", st.cancelled);
                }
            } else {
                if (st.cancel_events != 0 || st.filled + st.remaining != qty) {
                    fail("LIMIT ", id, ": Menge ", qty, " != Fills ", st.filled, " + Buchrest ", st.remaining);
                }
            }
        }
    }

    void check_seqs() {
        std::vector<SeqNo> sorted = seqs_;
        std::sort(sorted.begin(), sorted.end());
        for (std::size_t k = 0; k < sorted.size(); ++k) {
            if (sorted[k] != k + 1) {
                fail("ACK-Seqs nicht lückenlos/eindeutig 1..", sorted.size(), ": an Position ", k, " steht ",
                     sorted[k]);
                return;
            }
        }
        // Seqs in Event-Reihenfolge streng aufsteigend (Engine vergibt sie in Verarbeitungsreihenfolge).
        if (!std::is_sorted(seqs_.begin(), seqs_.end())) {
            fail("ACK-Seqs nicht in Event-Reihenfolge aufsteigend");
        }
    }

    void check_totals() {
        if (fills_ != 2 * trades_) {
            fail("FILLs ", fills_, " != 2 * TRADEs ", trades_);
        }
        if (traded_ != buy_filled_ || traded_ != sell_filled_) {
            fail("Trade-Summe ", traded_, ", FILLs Käufer ", buy_filled_, ", FILLs Verkäufer ", sell_filled_);
        }
        if (trades_ == 0 && !seqs_.empty()) {
            fail("Kein einziger TRADE - Workload prüft das Matching nicht");
        }
    }

    // Reihenfolge pro Producer: erste Antwort (ACK/REJECT) jeder nur einmal gesendeten Order
    // erscheint in submit()-Reihenfolge.
    void check_producer_order() {
        for (std::size_t p = 0; p < workload_.per_producer.size(); ++p) {
            std::size_t last = 0;
            bool have_last = false;
            for (const NewOrder& o : workload_.per_producer[p]) {
                if (workload_.orders.at(o.id).copies != 1) {
                    continue;
                }
                auto it = states_.find(o.id);
                if (it == states_.end()) {
                    continue;  // fehlende Antwort wird in check_per_order gemeldet
                }
                const std::size_t pos = it->second.first_response;
                if (have_last && pos <= last) {
                    fail("Producer ", p, ": Order ", o.id, " vor ihrem Vorgänger verarbeitet");
                    break;
                }
                last = pos;
                have_last = true;
            }
        }
    }

    // Aus den Events hergeleitetes Buch (alle LIMIT-Reste) muss ungekreuzt sein und in den obersten
    // Leveln mit der BOOK-Antwort am Ende übereinstimmen.
    void check_books(const std::vector<BookSnapshot>& books) {
        for (SymbolId sym : kSymbols) {
            std::map<Price, Quantity, std::greater<>> bids;
            std::map<Price, Quantity> asks;
            for (const auto& [id, s] : workload_.orders) {
                const NewOrder& o = s.order;
                auto it = states_.find(id);
                if (o.symbol != sym || o.type != OrderType::Limit || it == states_.end() || it->second.acks != 1 ||
                    it->second.remaining == 0) {
                    continue;
                }
                (o.side == Side::Buy ? bids[o.price] : asks[o.price]) += it->second.remaining;
            }
            if (!bids.empty() && !asks.empty() && bids.begin()->first >= asks.begin()->first) {
                fail("Symbol ", sym, ": hergeleitetes Buch gekreuzt (Bid ", bids.begin()->first, " >= Ask ",
                     asks.begin()->first, ")");
            }

            const auto snap =
                std::find_if(books.rbegin(), books.rend(), [sym](const BookSnapshot& b) { return b.symbol == sym; });
            if (snap == books.rend()) {
                fail("Symbol ", sym, ": keine BOOK-Antwort erhalten");
                continue;
            }
            compare_levels(sym, "BIDS", bids, snap->bids);
            compare_levels(sym, "ASKS", asks, snap->asks);
        }
    }

    template <typename Map>
    void compare_levels(SymbolId sym, const char* side, const Map& derived, const std::vector<BookLevel>& actual) {
        std::vector<BookLevel> expected;
        for (const auto& [price, qty] : derived) {
            if (expected.size() == engine::kMaxBookDepth) {
                break;
            }
            expected.push_back(BookLevel{price, qty});
        }
        bool equal = expected.size() == actual.size();
        for (std::size_t k = 0; equal && k < expected.size(); ++k) {
            equal = expected[k].price == actual[k].price && expected[k].quantity == actual[k].quantity;
        }
        if (!equal) {
            fail("Symbol ", sym, " ", side, ": BOOK-Antwort weicht vom hergeleiteten Buch ab (", actual.size(),
                 " statt ", expected.size(), " Level oder andere Werte)");
        }
    }

    const Workload& workload_;
    std::unordered_map<OrderId, State> states_;
    std::vector<SeqNo> seqs_;
    OrderId last_acked_ = 0;
    std::size_t trades_ = 0;
    std::size_t fills_ = 0;
    Quantity traded_ = 0;
    Quantity buy_filled_ = 0;
    Quantity sell_filled_ = 0;
    std::size_t failures_ = 0;
};

std::uint64_t seed_for(int iteration, std::size_t producer) {
    return kBaseSeed + static_cast<std::uint64_t>(iteration) * 1'000 + producer;
}

std::size_t count_responses(const CollectingSink& sink) {
    return static_cast<std::size_t>(std::count_if(sink.events.begin(), sink.events.end(), [](const Event& e) {
        return std::holds_alternative<AckEvent>(e) || std::holds_alternative<RejectEvent>(e);
    }));
}

// ---------------------------------------------------------------------------
// 1. N Producer, disjunkte ID-Bereiche, zufällige LIMIT/MARKET-Orders auf zwei Symbole

TEST(PipelineStress, ManyProducersPreserveInvariants) {
    for (int iter = 0; iter < kIterations; ++iter) {
        SCOPED_TRACE("Iteration " + std::to_string(iter));
        Workload workload;
        workload.per_producer.resize(kProducers);
        for (std::size_t p = 0; p < kProducers; ++p) {
            std::mt19937_64 rng(seed_for(iter, p));
            const OrderId base = (p + 1) * kIdRangePerProducer;
            for (int k = 0; k < kOrdersPerProducer; ++k) {
                workload.add(p, random_order(rng, base + static_cast<OrderId>(k)));
            }
        }

        CollectingSink sink;
        run_pipeline(workload, sink);

        ASSERT_EQ(count_responses(sink), static_cast<std::size_t>(kProducers) * kOrdersPerProducer)
            << "nicht jede gesendete Order hat genau eine Antwort (ACK/REJECT)";
        Checker(workload).check(sink);
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// 2. Doppelte IDs über Producer hinweg: alle Producer senden zusätzlich dieselben IDs
//    1..kSharedIds (identische Order) an zufälligen Positionen -> je 1 ACK + (N-1) DUPLICATE_ID.

TEST(PipelineStress, DuplicateIdsAcrossProducers) {
    for (int iter = 0; iter < kIterations; ++iter) {
        SCOPED_TRACE("Iteration " + std::to_string(iter));
        Workload workload;
        workload.per_producer.resize(kProducers);
        for (std::size_t p = 0; p < kProducers; ++p) {
            std::mt19937_64 rng(seed_for(iter, p) + 500);
            const OrderId base = (p + 1) * kIdRangePerProducer;
            std::vector<NewOrder> orders;
            for (int k = 0; k < kOrdersPerProducer / 4; ++k) {
                orders.push_back(random_order(rng, base + static_cast<OrderId>(k)));
            }
            for (int k = 1; k <= kSharedIds; ++k) {
                // Gleiche Order für alle Producer: Parameter nur aus der ID abgeleitet, immer gültig.
                std::mt19937_64 shared_rng(kBaseSeed * 7 + static_cast<std::uint64_t>(k));
                NewOrder o = random_order(shared_rng, static_cast<OrderId>(k));
                o.quantity = std::max<Quantity>(o.quantity, 1);
                orders.push_back(o);
            }
            std::shuffle(orders.begin(), orders.end(), rng);
            for (const NewOrder& o : orders) {
                workload.add(p, o);
            }
        }

        CollectingSink sink;
        run_pipeline(workload, sink);

        std::unordered_map<OrderId, std::pair<int, int>> shared;  // id -> (ACKs, DUPLICATE_ID)
        for (const Event& e : sink.events) {
            if (const auto* ack = std::get_if<AckEvent>(&e); ack != nullptr && ack->id <= kSharedIds) {
                ++shared[ack->id].first;
            } else if (const auto* rej = std::get_if<RejectEvent>(&e);
                       rej != nullptr && rej->id <= kSharedIds && rej->reason == RejectReason::DuplicateId) {
                ++shared[rej->id].second;
            }
        }
        int bad = 0;
        for (OrderId id = 1; id <= kSharedIds; ++id) {
            const auto [acks, dups] = shared[id];
            if (acks != 1 || dups != kProducers - 1) {
                if (++bad <= static_cast<int>(kMaxReportedFailures)) {
                    ADD_FAILURE() << "ID " << id << ": " << acks << " ACK, " << dups << " DUPLICATE_ID (erwartet 1 / "
                                  << kProducers - 1 << ")";
                }
            }
        }
        EXPECT_EQ(bad, 0) << "IDs mit falscher ACK/DUPLICATE_ID-Verteilung";

        ASSERT_EQ(count_responses(sink), static_cast<std::size_t>(kProducers) * (kOrdersPerProducer / 4 + kSharedIds));
        Checker(workload).check(sink);
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// 3. Determinismus: genau ein Producer -> Event-Folge identisch zu MatchingEngine direkt.

std::vector<Request> deterministic_requests(std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<Request> out;
    for (SymbolId s : kSymbols) {
        out.emplace_back(AddSymbolRequest{s});
    }
    std::vector<OrderId> used;
    for (int k = 0; k < kDeterminismRequests; ++k) {
        if (k == kDeterminismRequests / 2) {
            out.emplace_back(AddSymbolRequest{kLateSymbol});
        }
        if (k % 500 == 499) {
            const SymbolId sym = (k / 500) % 4 == 3 ? kLateSymbol : kSymbols[(k / 500) % kSymbols.size()];
            out.emplace_back(BookRequest{sym, static_cast<std::uint32_t>(1 + rng() % 10)});
            continue;
        }
        const int roll = percent(rng);
        if (roll < 2 && !used.empty()) {  // Wiederverwendung einer ID -> DUPLICATE_ID
            NewOrder o = random_order(rng, used[rng() % used.size()]);
            out.emplace_back(Command{o});
        } else if (roll < 3) {  // CANCEL (Stufe 2: UNKNOWN_ORDER; identisch in beiden Pfaden)
            out.emplace_back(Command{CancelOrder{used.empty() ? 1 : used[rng() % used.size()]}});
        } else {
            NewOrder o = random_order(rng, 1'000'000 + static_cast<OrderId>(k));
            const int variant = percent(rng);
            if (variant < 1) {
                o.symbol = kUnknownSymbol;
            } else if (variant < 4) {
                o.symbol = kLateSymbol;  // vor dem AddSymbol: UNKNOWN_SYMBOL, danach gültig
            } else if (variant < 5 && o.type == OrderType::Limit) {
                o.price = 0;  // INVALID_PRICE
            }
            used.push_back(o.id);
            out.emplace_back(Command{o});
        }
    }
    return out;
}

void apply_direct(MatchingEngine& eng, EventSink& sink, const Request& request) {
    if (const auto* cmd = std::get_if<Command>(&request)) {
        eng.process(*cmd);
    } else if (const auto* add = std::get_if<AddSymbolRequest>(&request)) {
        eng.add_symbol(add->symbol);
    } else if (const auto* book = std::get_if<BookRequest>(&request)) {
        const std::size_t depth = std::min<std::size_t>(book->depth, engine::kMaxBookDepth);
        sink.on_book(engine::to_snapshot(engine::to_book_event(eng.snapshot(book->symbol, depth))));
    }
}

TEST(PipelineStress, SingleProducerMatchesDirectEngine) {
    for (int iter = 0; iter < kIterations; ++iter) {
        SCOPED_TRACE("Iteration " + std::to_string(iter));
        const std::vector<Request> requests = deterministic_requests(seed_for(iter, 0) + 900);

        TextSink expected;
        {
            MatchingEngine eng(expected);
            for (const Request& r : requests) {
                apply_direct(eng, expected, r);
            }
        }

        TextSink actual;
        {
            Pipeline pipeline(actual);
            pipeline.start();
            std::thread producer([&pipeline, &requests] {
                for (const Request& r : requests) {
                    pipeline.submit(r);
                }
            });
            producer.join();
            pipeline.stop();
        }

        const std::vector<std::string>& exp = expected.lines;
        const std::vector<std::string>& act = actual.lines;
        const auto mismatch = std::mismatch(exp.begin(), exp.end(), act.begin(), act.end());
        if (mismatch.first != exp.end() || mismatch.second != act.end()) {
            const auto pos = static_cast<std::size_t>(mismatch.first - exp.begin());
            ADD_FAILURE() << "Event-Folge weicht ab an Position " << pos << " (erwartet " << exp.size()
                          << " Events, erhalten " << act.size() << "): erwartet '"
                          << (mismatch.first != exp.end() ? *mismatch.first : "<Ende>") << "', erhalten '"
                          << (mismatch.second != act.end() ? *mismatch.second : "<Ende>") << "'";
            return;
        }
    }
}

}  // namespace
