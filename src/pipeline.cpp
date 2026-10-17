#include "engine/pipeline.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <variant>

#include "engine/matching_engine.hpp"

// Shutdown-Protokoll
// ------------------
// Voraussetzung (Header): alle submit()-Aufrufe sind zurückgekehrt, bevor stop() beginnt.
//
// 1. stop() setzt stop_requested_ mit memory_order_release.
// 2. Der Engine-Thread beendet sich erst, wenn er stop_requested_ (acquire) als gesetzt
//    gelesen hat UND danach ein pop() auf requests_ nichts mehr liefert. Weil jedes push()
//    vor dem release-Store in stop() liegt, sind nach dem acquire-Load alle Requests in der
//    Queue sichtbar — das pop() NACH dem Flag-Load findet also garantiert alles. (Ein
//    "leer, dann Flag gesetzt"-Test in umgekehrter Reihenfolge könnte ein Element verlieren,
//    das zwischen pop() und Flag-Load eingereiht wurde.)
// 3. Danach setzt der Engine-Thread engine_done_ (release); alle Events sind dann bereits
//    in events_ (der Engine-Thread ist der einzige Producer der SPSC-Queue).
// 4. Der Logger beendet sich nach demselben Muster: engine_done_ (acquire) gesetzt UND
//    danach events_ leer.
// 5. stop() joint beide Threads; join() synchronisiert, d. h. alle Ausgaben an output_
//    sind danach für den Aufrufer sichtbar.
//
// Kein Lock im Engine- oder Logger-Thread; im Leerlauf wird nur std::this_thread::yield()
// aufgerufen.

namespace engine {

namespace {

template <typename... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <typename Queue, typename T>
void push_blocking(Queue& queue, const T& value) {
    while (!queue.push(value)) {
        std::this_thread::yield();  // Backpressure: Consumer ist hinterher, nichts verwerfen
    }
}

// EventSink der Engine: übersetzt Engine-Callbacks in Event-Werte für die SPSC-Queue.
// Wird ausschließlich im Engine-Thread benutzt (einziger Producer von events_).
class QueueSink final : public EventSink {
public:
    explicit QueueSink(EventQueue& events) : events_(events) {}

    void on_ack(OrderId id, SeqNo seq) override { push(AckEvent{id, seq}); }
    void on_reject(OrderId id, RejectReason reason) override { push(RejectEvent{id, reason}); }
    void on_trade(const Trade& trade) override { push(TradeEvent{trade}); }
    void on_fill(OrderId id, Quantity filled, Quantity remaining) override { push(FillEvent{id, filled, remaining}); }
    void on_cancelled(OrderId id, Quantity remaining, CancelReason reason) override {
        push(CancelledEvent{id, remaining, reason});
    }
    void on_book(const BookSnapshot& snapshot) override { push(to_book_event(snapshot)); }

    void push(const Event& event) { push_blocking(events_, event); }

private:
    EventQueue& events_;
};

}  // namespace

Pipeline::Pipeline(EventSink& output)
    : output_(output), requests_(std::make_unique<RequestQueue>()), events_(std::make_unique<EventQueue>()) {}

Pipeline::~Pipeline() { stop(); }

void Pipeline::start() {
    if (running_) {
        return;
    }
    // Neustart nach stop() ist erlaubt: beide Queues sind dann leer, die Engine wird neu angelegt.
    stop_requested_.store(false, std::memory_order_relaxed);
    engine_done_.store(false, std::memory_order_relaxed);
    // Thread-Erzeugung synchronisiert: die Stores oben sind in den neuen Threads sichtbar.
    engine_thread_ = std::thread([this] { engine_loop(); });
    logger_thread_ = std::thread([this] { logger_loop(); });
    running_ = true;
}

void Pipeline::submit(const Request& request) { push_blocking(*requests_, request); }

void Pipeline::stop() {
    if (!running_) {
        return;  // idempotent; auch ohne start() aufrufbar
    }
    stop_requested_.store(true, std::memory_order_release);
    engine_thread_.join();
    logger_thread_.join();
    running_ = false;
}

void Pipeline::engine_loop() {
    // Die einzige MatchingEngine lebt auf dem Stack des Engine-Threads; kein anderer Thread
    // kann sie erreichen.
    QueueSink sink(*events_);
    MatchingEngine engine(sink);

    const auto handle = [&](const Request& request) {
        std::visit(Overloaded{
                       [&](const Command& cmd) { engine.process(cmd); },
                       [&](const AddSymbolRequest& r) { engine.add_symbol(r.symbol); },
                       [&](const BookRequest& r) {
                           const std::size_t depth = std::min<std::size_t>(r.depth, kMaxBookDepth);
                           sink.push(to_book_event(engine.snapshot(r.symbol, depth)));
                       },
                   },
                   request);
    };

    for (;;) {
        if (const std::optional<Request> request = requests_->pop()) {
            handle(*request);
            continue;
        }
        if (stop_requested_.load(std::memory_order_acquire)) {
            // Flag gesehen: alle Requests sind jetzt sichtbar. Restliche abarbeiten, dann Ende.
            while (const std::optional<Request> request = requests_->pop()) {
                handle(*request);
            }
            break;
        }
        std::this_thread::yield();
    }

    engine_done_.store(true, std::memory_order_release);
}

void Pipeline::logger_loop() {
    for (;;) {
        if (const std::optional<Event> event = events_->pop()) {
            dispatch(*event, output_);
            continue;
        }
        if (engine_done_.load(std::memory_order_acquire)) {
            while (const std::optional<Event> event = events_->pop()) {
                dispatch(*event, output_);
            }
            break;
        }
        std::this_thread::yield();
    }
}

}  // namespace engine
