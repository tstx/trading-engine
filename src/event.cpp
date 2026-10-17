#include "engine/event.hpp"

#include <algorithm>

namespace engine {

namespace {

template <typename... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

}  // namespace

BookEvent to_book_event(const BookSnapshot& snapshot) {
    BookEvent event{};
    event.symbol = snapshot.symbol;
    event.bid_count = static_cast<std::uint8_t>(std::min(snapshot.bids.size(), kMaxBookDepth));
    event.ask_count = static_cast<std::uint8_t>(std::min(snapshot.asks.size(), kMaxBookDepth));
    std::copy_n(snapshot.bids.begin(), event.bid_count, event.bids.begin());
    std::copy_n(snapshot.asks.begin(), event.ask_count, event.asks.begin());
    return event;
}

BookSnapshot to_snapshot(const BookEvent& event) {
    return BookSnapshot{event.symbol,
                        {event.bids.begin(), event.bids.begin() + event.bid_count},
                        {event.asks.begin(), event.asks.begin() + event.ask_count}};
}

void dispatch(const Event& event, EventSink& sink) {
    std::visit(Overloaded{
                   [&](const AckEvent& e) { sink.on_ack(e.id, e.seq); },
                   [&](const RejectEvent& e) { sink.on_reject(e.id, e.reason); },
                   [&](const TradeEvent& e) { sink.on_trade(e.trade); },
                   [&](const FillEvent& e) { sink.on_fill(e.id, e.filled, e.remaining); },
                   [&](const CancelledEvent& e) { sink.on_cancelled(e.id, e.remaining, e.reason); },
                   [&](const BookEvent& e) { sink.on_book(to_snapshot(e)); },
               },
               event);
}

}  // namespace engine
