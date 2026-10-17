#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>

#include "engine/event_sink.hpp"
#include "engine/types.hpp"

namespace engine {

// Events als Werte, damit sie durch die SPSC-Queue Engine -> Logger passen.
// Alle Typen sind trivial kopierbar (kein std::vector, kein std::string).
struct AckEvent {
    OrderId id;
    SeqNo seq;
};
struct RejectEvent {
    OrderId id;
    RejectReason reason;
};
struct TradeEvent {
    Trade trade;
};
struct FillEvent {
    OrderId id;
    Quantity filled;
    Quantity remaining;
};
struct CancelledEvent {
    OrderId id;
    Quantity remaining;
    CancelReason reason;
};

inline constexpr std::size_t kMaxBookDepth = 5;

// Snapshot mit fester Maximaltiefe; nur die ersten bid_count/ask_count Einträge sind gültig.
struct BookEvent {
    SymbolId symbol;
    std::uint8_t bid_count;
    std::uint8_t ask_count;
    std::array<BookLevel, kMaxBookDepth> bids;
    std::array<BookLevel, kMaxBookDepth> asks;
};

using Event = std::variant<AckEvent, RejectEvent, TradeEvent, FillEvent, CancelledEvent, BookEvent>;
static_assert(std::is_trivially_copyable_v<Event>);

// Schneidet auf kMaxBookDepth Level je Seite ab.
BookEvent to_book_event(const BookSnapshot& snapshot);
BookSnapshot to_snapshot(const BookEvent& event);

// Ruft die passende EventSink-Methode auf (BookEvent -> on_book).
void dispatch(const Event& event, EventSink& sink);

// Eingang der Pipeline (MPMC-Queue, trivial kopierbar).
// AddSymbol und Book laufen durch dieselbe Queue wie die Orders, damit nur der
// Engine-Thread die Engine anfasst und die Reihenfolge pro Producer erhalten bleibt.
struct AddSymbolRequest {
    SymbolId symbol;
};
struct BookRequest {
    SymbolId symbol;
    std::uint32_t depth;  // wird auf kMaxBookDepth begrenzt
};

using Request = std::variant<Command, AddSymbolRequest, BookRequest>;
static_assert(std::is_trivially_copyable_v<Request>);

}  // namespace engine
