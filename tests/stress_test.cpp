// Pipeline-Stresstest (plan.txt §9 letzter Punkt, §10 Stufe 2/3):
// - mehrere Producer-Threads mit zufälligen (fest geseedeten) NEW/CANCEL/MODIFY-Requests,
//   CANCEL/MODIFY nur auf eigene, früher gesendete IDs; Prüfung globaler Invarianten
//   allein aus der aufgezeichneten Event-Folge;
// - doppelte IDs über Producer hinweg;
// - Determinismus: ein Producer liefert dieselbe Event-Folge wie MatchingEngine direkt,
//   und alle drei Engine-Varianten liefern direkt dieselbe Event-Folge.
// Jeder Test läuft kIterations-mal mit frischer Pipeline, um Races zu provozieren.
//
// Prüfprinzip: Alle Requests auf eine ID stammen (außer bei den absichtlich geteilten
// NEW-IDs) von genau einem Producer; die Pipeline erhält die Reihenfolge pro Producer.
// Daher erscheinen die Antworten (ACK / REJECT / CANCELLED(USER)) auf eine ID in
// Sende-Reihenfolge, und der Checker kann jede Antwort ihrer Anfrage zuordnen und das
// erwartete Ergebnis aus dem bis dahin hergeleiteten Order-Zustand exakt vorhersagen.

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
using engine::ModifyOrder;
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
constexpr int kRequestsPerProducer = 5'000;
constexpr OrderId kIdRangePerProducer = 10'000'000;              // Producer p: IDs ab (p + 1) * kIdRangePerProducer
constexpr OrderId kNeverUsedIdOffset = kIdRangePerProducer / 2;  // CANCEL/MODIFY auf nie gesendete IDs
constexpr int kSharedIds = 2'000;  // Duplicate-Test: IDs 1..kSharedIds von allen Producern
constexpr int kDeterminismRequests = 8'000;
constexpr std::uint64_t kBaseSeed = 20261017;

constexpr std::array<SymbolId, 2> kSymbols{1, 2};
constexpr SymbolId kLateSymbol = 3;      // Determinismus: wird erst mitten im Lauf hinzugefügt
constexpr SymbolId kUnknownSymbol = 99;  // nie hinzugefügt
constexpr Price kMidPrice = 1000;
constexpr Price kPriceSpread = 8;  // Limits in [mid - spread, mid + spread]
constexpr Quantity kMaxQuantity = 50;
constexpr int kMarketPercent = 15;
constexpr int kZeroQuantityPercent = 1;  // erzeugt INVALID_QUANTITY-Rejects

// Anteile der Request-Arten pro Producer (Rest: NEW).
constexpr int kCancelPercent = 10;
constexpr int kModifyPercent = 15;
constexpr int kNeverUsedPercent = 1;       // CANCEL/MODIFY auf nie gesendete ID -> UNKNOWN_ORDER
constexpr std::size_t kRecentWindow = 32;  // CANCEL/MODIFY-Ziele: die letzten N eigenen NEW-IDs

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
// Request-Erzeugung

int percent(std::mt19937_64& rng) { return static_cast<int>(rng() % 100); }

Price random_limit_price(std::mt19937_64& rng) {
    return kMidPrice - kPriceSpread + static_cast<Price>(rng() % static_cast<std::uint64_t>(2 * kPriceSpread + 1));
}

NewOrder random_order(std::mt19937_64& rng, OrderId id) {
    NewOrder o{};
    o.id = id;
    o.symbol = kSymbols[rng() % kSymbols.size()];
    o.side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
    o.type = percent(rng) < kMarketPercent ? OrderType::Market : OrderType::Limit;
    o.price = o.type == OrderType::Market ? 0 : random_limit_price(rng);
    o.quantity = percent(rng) < kZeroQuantityPercent ? 0 : 1 + rng() % kMaxQuantity;
    return o;
}

// MODIFY auf target (NEW-Parameter bekannt, aktueller Zustand nicht): Reduktion / gleiche Menge /
// Erhöhung bei gleichem Preis, Preisänderung (kann kreuzen), selten ungültig (Preis 0, Menge 0).
ModifyOrder random_modify(std::mt19937_64& rng, const NewOrder& target) {
    ModifyOrder m{};
    m.id = target.id;
    const Price price = target.type == OrderType::Limit ? target.price : random_limit_price(rng);
    const Quantity qty = std::max<Quantity>(target.quantity, 1);
    const int variant = percent(rng);
    if (variant < 30) {  // gleicher Preis, Reduktion oder gleiche Menge (evtl. <= filled)
        m.new_price = price;
        m.new_quantity = 1 + rng() % qty;
    } else if (variant < 50) {  // gleicher Preis, Erhöhung
        m.new_price = price;
        m.new_quantity = qty + 1 + rng() % kMaxQuantity;
    } else if (variant < 95) {  // Preisänderung (auch zufällig gleicher Preis)
        m.new_price = random_limit_price(rng);
        m.new_quantity = 1 + rng() % kMaxQuantity;
    } else if (variant < 98) {  // INVALID_PRICE (falls liegend und Menge > filled)
        m.new_price = 0;
        m.new_quantity = qty + 1;
    } else {  // INVALID_MODIFY (falls liegend)
        m.new_price = price;
        m.new_quantity = 0;
    }
    return m;
}

// Request-Strom eines Producers: NEW mit IDs base + k, CANCEL/MODIFY auf eine der letzten
// kRecentWindow eigenen NEW-IDs, selten auf nie gesendete IDs.
std::vector<Command> producer_stream(std::mt19937_64& rng, OrderId base, int count) {
    std::vector<Command> out;
    out.reserve(static_cast<std::size_t>(count));
    std::vector<NewOrder> recent;
    std::size_t next_recent = 0;  // Ringpuffer-Position
    for (int k = 0; k < count; ++k) {
        const int roll = percent(rng);
        if (!recent.empty() && roll < kCancelPercent) {
            out.emplace_back(CancelOrder{recent[rng() % recent.size()].id});
        } else if (!recent.empty() && roll < kCancelPercent + kModifyPercent) {
            out.emplace_back(random_modify(rng, recent[rng() % recent.size()]));
        } else if (roll < kCancelPercent + kModifyPercent + kNeverUsedPercent) {
            const OrderId never = base + kNeverUsedIdOffset + static_cast<OrderId>(k);
            if (rng() % 2 == 0) {
                out.emplace_back(CancelOrder{never});
            } else {
                out.emplace_back(ModifyOrder{never, kMidPrice, 10});
            }
        } else {
            const NewOrder o = random_order(rng, base + static_cast<OrderId>(k));
            out.emplace_back(o);
            if (recent.size() < kRecentWindow) {
                recent.push_back(o);
            } else {
                recent[next_recent] = o;
                next_recent = (next_recent + 1) % kRecentWindow;
            }
        }
    }
    return out;
}

OrderId command_id(const Command& cmd) {
    return std::visit([](const auto& c) { return c.id; }, cmd);
}

// Erwarteter Reject-Grund einer NEW-Order bei Erstverarbeitung (ohne DUPLICATE_ID), nullopt = gültig.
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

struct SentRequest {
    Command cmd;
    std::size_t producer;
    std::size_t position;  // Index in per_producer[producer]
};

// Was die Producer gesendet haben (vor dem Start komplett erzeugt).
struct Workload {
    std::vector<std::vector<Command>> per_producer;
    std::unordered_map<OrderId, std::vector<SentRequest>> by_id;  // je ID in Sende-Reihenfolge
    std::unordered_map<OrderId, int> new_copies;                  // wie oft NEW mit dieser ID gesendet

    explicit Workload(std::size_t producers) : per_producer(producers) {}

    void add(std::size_t producer, const Command& cmd) {
        const std::size_t position = per_producer[producer].size();
        per_producer[producer].push_back(cmd);
        const OrderId id = command_id(cmd);
        by_id[id].push_back(SentRequest{cmd, producer, position});
        if (std::holds_alternative<NewOrder>(cmd)) {
            ++new_copies[id];
        }
    }
    [[nodiscard]] bool shared(OrderId id) const {
        auto it = new_copies.find(id);
        return it != new_copies.end() && it->second > 1;
    }
    [[nodiscard]] std::size_t total() const {
        std::size_t n = 0;
        for (const auto& reqs : per_producer) {
            n += reqs.size();
        }
        return n;
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
    for (const std::vector<Command>& commands : workload.per_producer) {
        producers.emplace_back([&pipeline, &commands, &go] {
            go.arrive_and_wait();
            for (const Command& c : commands) {
                pipeline.submit(Request{c});
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
    explicit Checker(const Workload& workload) : workload_(workload) {
        response_pos_.resize(workload.per_producer.size());
        for (std::size_t p = 0; p < workload.per_producer.size(); ++p) {
            response_pos_[p].assign(workload.per_producer[p].size(), kNoResponse);
        }
    }

    void check(const CollectingSink& sink) {
        const std::vector<Event>& ev = sink.events;
        for (std::size_t i = 0; i < ev.size(); ++i) {
            std::visit([&](const auto& e) { on_event(i, ev, e); }, ev[i]);
        }
        check_per_order();
        check_totals();
        check_producer_order();
        check_books(sink.books);
        if (failures_ > kMaxReportedFailures) {
            ADD_FAILURE() << (failures_ - kMaxReportedFailures) << " weitere Verletzungen nicht ausgegeben";
        }
        EXPECT_EQ(failures_, 0U) << "Invarianten verletzt";
    }

private:
    static constexpr std::size_t kNoResponse = std::numeric_limits<std::size_t>::max();

    // Hergeleiteter Zustand einer ID.
    struct State {
        std::size_t answered = 0;  // Anzahl beantworteter Requests (Index in by_id[id])
        bool accepted = false;     // NEW geACKt
        NewOrder order{};          // die akzeptierte NEW-Order
        Price price = 0;           // aktueller Limitpreis (nach MODIFY)
        SeqNo seq = 0;             // aktuelle Seq (nach MODIFY)
        Quantity remaining = 0;    // aktueller Rest (im Buch bzw. als Aggressor)
        Quantity filled = 0;
        Quantity no_liquidity = 0;  // CANCELLED(NO_LIQUIDITY)-Rest (MARKET)
        int no_liquidity_events = 0;
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
        if constexpr (std::is_same_v<T, RejectReason>) {
            return std::string(to_string(value));
        } else if constexpr (std::is_arithmetic_v<T>) {
            return std::to_string(value);
        } else {
            return std::string(value);
        }
    }

    static bool live(const State& st) { return st.accepted && st.order.type == OrderType::Limit && st.remaining > 0; }

    static const char* kind(const Command& cmd) {
        if (std::holds_alternative<NewOrder>(cmd)) {
            return "NEW";
        }
        return std::holds_alternative<CancelOrder>(cmd) ? "CANCEL" : "MODIFY";
    }

    // Erwartetes Ergebnis der Anfrage im aktuellen Zustand: Reject-Grund oder nullopt (Erfolg).
    static std::optional<RejectReason> expected_outcome(const Command& cmd, const State& st) {
        if (const auto* n = std::get_if<NewOrder>(&cmd)) {
            if (st.accepted) {
                return RejectReason::DuplicateId;
            }
            return expected_reject(*n);
        }
        if (!live(st)) {
            return RejectReason::UnknownOrder;
        }
        if (const auto* m = std::get_if<ModifyOrder>(&cmd)) {
            if (m->new_quantity <= st.filled) {
                return RejectReason::InvalidModify;
            }
            if (m->new_price <= 0) {
                return RejectReason::InvalidPrice;
            }
        }
        return std::nullopt;
    }

    // Nächste unbeantwortete Anfrage auf id (Antworten kommen in Sende-Reihenfolge).
    const SentRequest* next_request(std::size_t i, OrderId id, const char* what) {
        auto it = workload_.by_id.find(id);
        State& st = states_[id];
        if (it == workload_.by_id.end() || st.answered >= it->second.size()) {
            fail("Event ", i, ": ", what, " für ID ", id, " ohne offene Anfrage");
            return nullptr;
        }
        const SentRequest& req = it->second[st.answered++];
        if (!workload_.shared(id)) {
            response_pos_[req.producer][req.position] = i;
        }
        return &req;
    }

    const State* accepted_state(std::size_t i, OrderId id, const char* what) {
        auto it = states_.find(id);
        if (it == states_.end() || !it->second.accepted) {
            fail("Event ", i, ": ", what, " für nicht akzeptierte Order ", id);
            return nullptr;
        }
        return &it->second;
    }

    void check_new_seq(std::size_t i, OrderId id, SeqNo seq) {
        if (seq != next_seq_) {
            fail("Event ", i, ": ACK ", id, " mit Seq ", seq, ", erwartet neue Seq ", next_seq_);
        }
        next_seq_ = seq + 1;  // weiterzählen ab der gemeldeten Seq (Folgefehler vermeiden)
    }

    void on_event(std::size_t i, const std::vector<Event>& /*ev*/, const AckEvent& e) {
        last_aggressor_ = 0;
        const SentRequest* req = next_request(i, e.id, "ACK");
        if (req == nullptr) {
            return;
        }
        State& st = states_[e.id];
        const std::optional<RejectReason> expected = expected_outcome(req->cmd, st);
        if (std::holds_alternative<CancelOrder>(req->cmd)) {
            fail("Event ", i, ": ACK als Antwort auf CANCEL ", e.id);
            return;
        }
        if (expected) {
            fail("Event ", i, ": ACK auf ", kind(req->cmd), " ", e.id, ", erwartet REJECT ", *expected);
            return;
        }
        if (const auto* n = std::get_if<NewOrder>(&req->cmd)) {
            st.accepted = true;
            st.order = *n;
            st.price = n->type == OrderType::Limit ? n->price : 0;
            st.remaining = n->quantity;
            st.seq = e.seq;
            check_new_seq(i, e.id, e.seq);
            last_aggressor_ = e.id;
            ++new_acks_;
            return;
        }
        const ModifyOrder& m = std::get<ModifyOrder>(req->cmd);
        const bool keeps_priority = m.new_price == st.price && m.new_quantity <= st.filled + st.remaining;
        st.remaining = m.new_quantity - st.filled;
        if (keeps_priority) {
            if (e.seq != st.seq) {
                fail("Event ", i, ": MODIFY-Reduktion ", e.id, " mit Seq ", e.seq, ", erwartet alte Seq ", st.seq);
            }
            ++modify_keep_;
            return;  // kein Matching -> kein TRADE darf folgen (last_aggressor_ = 0)
        }
        st.price = m.new_price;
        st.seq = e.seq;
        check_new_seq(i, e.id, e.seq);
        last_aggressor_ = e.id;
        ++modify_requeue_;
    }

    void on_event(std::size_t i, const std::vector<Event>& /*ev*/, const RejectEvent& e) {
        last_aggressor_ = 0;
        const SentRequest* req = next_request(i, e.id, "REJECT");
        if (req == nullptr) {
            return;
        }
        const std::optional<RejectReason> expected = expected_outcome(req->cmd, states_[e.id]);
        if (!expected) {
            fail("Event ", i, ": REJECT ", e.reason, " auf ", kind(req->cmd), " ", e.id, ", erwartet Erfolg");
        } else if (*expected != e.reason) {
            fail("Event ", i, ": REJECT ", e.reason, " auf ", kind(req->cmd), " ", e.id, ", erwartet ", *expected);
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
        if (aggressor != last_aggressor_) {
            fail("Event ", i, ": Aggressor ", aggressor, " ist nicht die zuletzt (neu) geACKte Order ",
                 last_aggressor_);
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

        const State* buy = accepted_state(i, t.buy_order_id, "TRADE (Käufer)");
        const State* sell = accepted_state(i, t.sell_order_id, "TRADE (Verkäufer)");
        if (buy == nullptr || sell == nullptr) {
            return;
        }
        if (buy->order.side != Side::Buy || sell->order.side != Side::Sell) {
            fail("Event ", i, ": Seiten im TRADE vertauscht");
        }
        if (buy->order.symbol != t.symbol || sell->order.symbol != t.symbol) {
            fail("Event ", i, ": TRADE über Symbolgrenzen");
        }
        const State& rest = resting == t.buy_order_id ? *buy : *sell;
        const State& aggr = aggressor == t.buy_order_id ? *buy : *sell;
        if (!live(rest)) {
            fail("Event ", i, ": resting Order ", resting, " liegt nicht im Buch");
        } else if (t.price != rest.price) {
            fail("Event ", i, ": Trade-Preis ", t.price, " != aktueller Preis der resting Order ", rest.price);
        }
        if (aggr.order.type == OrderType::Limit) {
            const bool worse = aggr.order.side == Side::Buy ? t.price > aggr.price : t.price < aggr.price;
            if (worse) {
                fail("Event ", i, ": Trade-Preis ", t.price, " schlechter als Limit ", aggr.price, " des Aggressors ",
                     aggressor);
            }
        }
    }

    void on_event(std::size_t i, const std::vector<Event>& /*ev*/, const FillEvent& e) {
        ++fills_;
        if (accepted_state(i, e.id, "FILL") == nullptr) {
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
        (st.order.side == Side::Buy ? buy_filled_ : sell_filled_) += e.filled;
    }

    void on_event(std::size_t i, const std::vector<Event>& /*ev*/, const CancelledEvent& e) {
        if (e.reason == CancelReason::NoLiquidity) {
            if (accepted_state(i, e.id, "CANCELLED(NO_LIQUIDITY)") == nullptr) {
                return;
            }
            State& st = states_[e.id];
            ++st.no_liquidity_events;
            if (st.order.type != OrderType::Market) {
                fail("Event ", i, ": CANCELLED(NO_LIQUIDITY) für LIMIT-Order ", e.id);
            }
            if (e.id != last_aggressor_) {
                fail("Event ", i, ": CANCELLED für ", e.id, " nicht direkt nach deren Verarbeitung");
            }
            if (e.remaining == 0 || e.remaining != st.remaining) {
                fail("Event ", i, ": CANCELLED-Rest ", e.remaining, ", erwartet ", st.remaining, " (Order ", e.id, ")");
            }
            st.no_liquidity += e.remaining;
            st.remaining = 0;
            last_aggressor_ = 0;
            return;
        }

        last_aggressor_ = 0;
        const SentRequest* req = next_request(i, e.id, "CANCELLED(USER)");
        if (req == nullptr) {
            return;
        }
        State& st = states_[e.id];
        if (!std::holds_alternative<CancelOrder>(req->cmd)) {
            fail("Event ", i, ": CANCELLED(USER) als Antwort auf ", kind(req->cmd), " ", e.id);
            return;
        }
        if (!live(st)) {
            fail("Event ", i, ": CANCELLED(USER) für nicht liegende Order ", e.id, ", erwartet REJECT UNKNOWN_ORDER");
            return;
        }
        if (e.remaining != st.remaining) {
            fail("Event ", i, ": CANCELLED(USER)-Rest ", e.remaining, ", erwartet ", st.remaining, " (Order ", e.id,
                 ")");
        }
        st.remaining = 0;
        ++user_cancels_;
    }

    void on_event(std::size_t i, const std::vector<Event>& /*ev*/, const engine::BookEvent& /*e*/) {
        fail("Event ", i, ": BookEvent im Event-Strom (Sink speichert Books separat)");
    }

    void check_per_order() {
        for (const auto& [id, reqs] : workload_.by_id) {
            const State st = states_.contains(id) ? states_.at(id) : State{};
            if (st.answered != reqs.size()) {
                fail("ID ", id, ": ", reqs.size() - std::min(st.answered, reqs.size()), " von ", reqs.size(),
                     " Requests ohne Antwort");
            }
            if (!st.accepted) {
                continue;
            }
            // MARKET: Fills + CANCELLED(NO_LIQUIDITY) == Menge, nie liegend.
            // LIMIT: nie NO_LIQUIDITY (Mengenerhalt wird über remaining/FILL/CANCELLED laufend geprüft).
            if (st.order.type == OrderType::Market) {
                if (st.filled + st.no_liquidity != st.order.quantity || st.remaining != 0 ||
                    st.no_liquidity_events != (st.no_liquidity > 0 ? 1 : 0)) {
                    fail("MARKET ", id, ": Menge ", st.order.quantity, " != Fills ", st.filled, " + CANCELLED ",
                         st.no_liquidity);
                }
            } else if (st.no_liquidity_events != 0) {
                fail("LIMIT ", id, ": CANCELLED(NO_LIQUIDITY)");
            }
        }
    }

    void check_totals() {
        if (fills_ != 2 * trades_) {
            fail("FILLs ", fills_, " != 2 * TRADEs ", trades_);
        }
        if (traded_ != buy_filled_ || traded_ != sell_filled_) {
            fail("Trade-Summe ", traded_, ", FILLs Käufer ", buy_filled_, ", FILLs Verkäufer ", sell_filled_);
        }
        if (new_acks_ > 0 && trades_ == 0) {
            fail("Kein einziger TRADE - Workload prüft das Matching nicht");
        }
        bool any_cancel_or_modify = false;
        for (const auto& reqs : workload_.per_producer) {
            any_cancel_or_modify = any_cancel_or_modify || std::any_of(reqs.begin(), reqs.end(), [](const Command& c) {
                                       return !std::holds_alternative<NewOrder>(c);
                                   });
        }
        if (any_cancel_or_modify && (user_cancels_ == 0 || modify_keep_ == 0 || modify_requeue_ == 0)) {
            fail("Workload prüft CANCEL/MODIFY nicht: ", user_cancels_, " CANCELLED(USER), ", modify_keep_,
                 " MODIFY mit Prioritätserhalt, ", modify_requeue_, " MODIFY mit neuer Seq");
        }
    }

    // Reihenfolge pro Producer: Antworten auf nicht geteilte IDs erscheinen in submit()-Reihenfolge.
    void check_producer_order() {
        for (std::size_t p = 0; p < response_pos_.size(); ++p) {
            std::size_t last = 0;
            bool have_last = false;
            for (std::size_t k = 0; k < response_pos_[p].size(); ++k) {
                const std::size_t pos = response_pos_[p][k];
                if (pos == kNoResponse) {
                    continue;  // geteilte ID oder fehlende Antwort (wird in check_per_order gemeldet)
                }
                if (have_last && pos <= last) {
                    fail("Producer ", p, ": Request ", k, " (ID ", command_id(workload_.per_producer[p][k]),
                         ") vor seinem Vorgänger verarbeitet");
                    break;
                }
                last = pos;
                have_last = true;
            }
        }
    }

    // Aus den Events hergeleitetes Buch (alle liegenden LIMIT-Reste zum aktuellen Preis) muss
    // ungekreuzt sein und in den obersten Leveln mit der BOOK-Antwort am Ende übereinstimmen.
    void check_books(const std::vector<BookSnapshot>& books) {
        for (SymbolId sym : kSymbols) {
            std::map<Price, Quantity, std::greater<>> bids;
            std::map<Price, Quantity> asks;
            for (const auto& [id, st] : states_) {
                if (!live(st) || st.order.symbol != sym) {
                    continue;
                }
                (st.order.side == Side::Buy ? bids[st.price] : asks[st.price]) += st.remaining;
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
    std::vector<std::vector<std::size_t>> response_pos_;  // [producer][position] -> Event-Index der Antwort
    SeqNo next_seq_ = 1;
    OrderId last_aggressor_ = 0;  // 0: kein TRADE erlaubt
    std::size_t trades_ = 0;
    std::size_t fills_ = 0;
    std::size_t new_acks_ = 0;
    std::size_t user_cancels_ = 0;
    std::size_t modify_keep_ = 0;
    std::size_t modify_requeue_ = 0;
    Quantity traded_ = 0;
    Quantity buy_filled_ = 0;
    Quantity sell_filled_ = 0;
    std::size_t failures_ = 0;
};

std::uint64_t seed_for(int iteration, std::size_t producer) {
    return kBaseSeed + static_cast<std::uint64_t>(iteration) * 1'000 + producer;
}

// Antworten = ACK + REJECT + CANCELLED(USER); genau eine pro Request.
std::size_t count_responses(const CollectingSink& sink) {
    return static_cast<std::size_t>(std::count_if(sink.events.begin(), sink.events.end(), [](const Event& e) {
        if (const auto* c = std::get_if<CancelledEvent>(&e)) {
            return c->reason == CancelReason::User;
        }
        return std::holds_alternative<AckEvent>(e) || std::holds_alternative<RejectEvent>(e);
    }));
}

// ---------------------------------------------------------------------------
// 1. N Producer, disjunkte ID-Bereiche, zufällige NEW (LIMIT/MARKET) auf zwei Symbole sowie
//    CANCEL/MODIFY auf eigene, früher gesendete IDs

TEST(PipelineStress, ManyProducersPreserveInvariants) {
    for (int iter = 0; iter < kIterations; ++iter) {
        SCOPED_TRACE("Iteration " + std::to_string(iter));
        Workload workload(kProducers);
        for (std::size_t p = 0; p < kProducers; ++p) {
            std::mt19937_64 rng(seed_for(iter, p));
            for (const Command& c : producer_stream(rng, (p + 1) * kIdRangePerProducer, kRequestsPerProducer)) {
                workload.add(p, c);
            }
        }

        CollectingSink sink;
        run_pipeline(workload, sink);

        EXPECT_EQ(count_responses(sink), workload.total())
            << "nicht jeder Request hat genau eine Antwort (ACK/REJECT/CANCELLED(USER))";
        Checker(workload).check(sink);
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// 2. Doppelte IDs über Producer hinweg: alle Producer senden zusätzlich dieselben NEW-IDs
//    1..kSharedIds (identische Order) an zufälligen Positionen -> je 1 ACK + (N-1) DUPLICATE_ID.
//    Daneben eigener Strom mit CANCEL/MODIFY (nie auf geteilte IDs).

TEST(PipelineStress, DuplicateIdsAcrossProducers) {
    for (int iter = 0; iter < kIterations; ++iter) {
        SCOPED_TRACE("Iteration " + std::to_string(iter));
        Workload workload(kProducers);
        for (std::size_t p = 0; p < kProducers; ++p) {
            std::mt19937_64 rng(seed_for(iter, p) + 500);
            std::vector<Command> own = producer_stream(rng, (p + 1) * kIdRangePerProducer, kRequestsPerProducer / 4);
            std::vector<Command> shared;
            for (int k = 1; k <= kSharedIds; ++k) {
                // Gleiche Order für alle Producer: Parameter nur aus der ID abgeleitet, immer gültig.
                std::mt19937_64 shared_rng(kBaseSeed * 7 + static_cast<std::uint64_t>(k));
                NewOrder o = random_order(shared_rng, static_cast<OrderId>(k));
                o.quantity = std::max<Quantity>(o.quantity, 1);
                shared.emplace_back(o);
            }
            // Geteilte NEWs zufällig einstreuen; die Reihenfolge des eigenen Stroms bleibt erhalten
            // (CANCEL/MODIFY müssen nach ihrem NEW kommen).
            std::vector<Command> merged;
            merged.reserve(own.size() + shared.size());
            std::size_t a = 0;
            std::size_t b = 0;
            while (a < own.size() || b < shared.size()) {
                const bool take_own = b == shared.size() ||
                                      (a < own.size() && rng() % (own.size() - a + shared.size() - b) < own.size() - a);
                merged.push_back(take_own ? own[a++] : shared[b++]);
            }
            for (const Command& c : merged) {
                workload.add(p, c);
            }
        }

        CollectingSink sink;
        run_pipeline(workload, sink);

        std::unordered_map<OrderId, std::pair<int, int>> counts;  // id -> (ACKs, DUPLICATE_ID)
        for (const Event& e : sink.events) {
            if (const auto* ack = std::get_if<AckEvent>(&e); ack != nullptr && ack->id <= kSharedIds) {
                ++counts[ack->id].first;
            } else if (const auto* rej = std::get_if<RejectEvent>(&e);
                       rej != nullptr && rej->id <= kSharedIds && rej->reason == RejectReason::DuplicateId) {
                ++counts[rej->id].second;
            }
        }
        int bad = 0;
        for (OrderId id = 1; id <= kSharedIds; ++id) {
            const auto [acks, dups] = counts[id];
            if (acks != 1 || dups != kProducers - 1) {
                if (++bad <= static_cast<int>(kMaxReportedFailures)) {
                    ADD_FAILURE() << "ID " << id << ": " << acks << " ACK, " << dups << " DUPLICATE_ID (erwartet 1 / "
                                  << kProducers - 1 << ")";
                }
            }
        }
        EXPECT_EQ(bad, 0) << "IDs mit falscher ACK/DUPLICATE_ID-Verteilung";

        EXPECT_EQ(count_responses(sink), workload.total());
        Checker(workload).check(sink);
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// 3. Determinismus: genau ein Producer -> Event-Folge identisch zu MatchingEngine direkt;
//    außerdem liefern alle drei Engine-Varianten direkt dieselbe Event-Folge.

std::vector<Request> deterministic_requests(std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<Request> out;
    for (SymbolId s : kSymbols) {
        out.emplace_back(AddSymbolRequest{s});
    }
    std::vector<OrderId> used;
    std::vector<NewOrder> recent;  // letzte kRecentWindow NEWs (CANCEL/MODIFY-Ziele)
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
        } else if (roll < 3) {  // CANCEL einer beliebigen (oft längst erledigten) ID
            out.emplace_back(Command{CancelOrder{used.empty() ? 1 : used[rng() % used.size()]}});
        } else if (roll < 10 && !recent.empty()) {  // CANCEL einer jungen ID
            out.emplace_back(Command{CancelOrder{recent[rng() % recent.size()].id}});
        } else if (roll < 22 && !recent.empty()) {  // MODIFY einer jungen ID
            out.emplace_back(Command{random_modify(rng, recent[rng() % recent.size()])});
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
            if (recent.size() == kRecentWindow) {
                recent.erase(recent.begin());
            }
            recent.push_back(o);
            out.emplace_back(Command{o});
        }
    }
    return out;
}

template <typename Engine>
void apply_direct(Engine& eng, EventSink& sink, const Request& request) {
    if (const auto* cmd = std::get_if<Command>(&request)) {
        eng.process(*cmd);
    } else if (const auto* add = std::get_if<AddSymbolRequest>(&request)) {
        eng.add_symbol(add->symbol);
    } else if (const auto* book = std::get_if<BookRequest>(&request)) {
        const std::size_t depth = std::min<std::size_t>(book->depth, engine::kMaxBookDepth);
        sink.on_book(engine::to_snapshot(engine::to_book_event(eng.snapshot(book->symbol, depth))));
    }
}

template <typename Engine>
std::vector<std::string> run_direct(const std::vector<Request>& requests) {
    TextSink sink;
    Engine eng(sink);
    for (const Request& r : requests) {
        apply_direct(eng, sink, r);
    }
    return std::move(sink.lines);
}

// Meldet die erste Abweichung; true bei Gleichheit.
bool same_lines(const std::vector<std::string>& exp, const std::vector<std::string>& act, const char* what) {
    const auto mismatch = std::mismatch(exp.begin(), exp.end(), act.begin(), act.end());
    if (mismatch.first == exp.end() && mismatch.second == act.end()) {
        return true;
    }
    const auto pos = static_cast<std::size_t>(mismatch.first - exp.begin());
    ADD_FAILURE() << what << ": Event-Folge weicht ab an Position " << pos << " (erwartet " << exp.size()
                  << " Events, erhalten " << act.size() << "): erwartet '"
                  << (mismatch.first != exp.end() ? *mismatch.first : "<Ende>") << "', erhalten '"
                  << (mismatch.second != act.end() ? *mismatch.second : "<Ende>") << "'";
    return false;
}

std::size_t count_matching(const std::vector<std::string>& lines, const std::string& prefix,
                           const std::string& suffix) {
    return static_cast<std::size_t>(std::count_if(lines.begin(), lines.end(), [&](const std::string& l) {
        return l.starts_with(prefix) && l.ends_with(suffix);
    }));
}

TEST(PipelineStress, SingleProducerMatchesDirectEngine) {
    for (int iter = 0; iter < kIterations; ++iter) {
        SCOPED_TRACE("Iteration " + std::to_string(iter));
        const std::vector<Request> requests = deterministic_requests(seed_for(iter, 0) + 900);

        const std::vector<std::string> expected = run_direct<MatchingEngine>(requests);
        // Der Strom muss Cancel/Modify tatsächlich ausüben (nicht nur Rejects).
        EXPECT_GT(count_matching(expected, "CANCELLED,", ",USER"), 0U) << "kein erfolgreicher CANCEL im Strom";
        EXPECT_GT(count_matching(expected, "REJECT,", ",UNKNOWN_ORDER"), 0U);

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

        if (!same_lines(expected, actual.lines, "Pipeline vs. Engine direkt")) {
            return;
        }
    }
}

TEST(EngineVariants, DirectEventStreamsAreIdentical) {
    for (int iter = 0; iter < kIterations; ++iter) {
        SCOPED_TRACE("Iteration " + std::to_string(iter));
        const std::vector<Request> requests = deterministic_requests(seed_for(iter, 0) + 1900);
        const std::vector<std::string> map_lines = run_direct<engine::MapMatchingEngine>(requests);
        const bool vector_ok =
            same_lines(map_lines, run_direct<engine::VectorMatchingEngine>(requests), "Vector vs. Map");
        const bool tick_ok = same_lines(map_lines, run_direct<engine::TickMatchingEngine>(requests), "Tick vs. Map");
        if (!vector_ok || !tick_ok) {
            return;
        }
    }
}

}  // namespace
