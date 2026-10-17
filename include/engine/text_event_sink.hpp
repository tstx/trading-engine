#pragma once

#include <ostream>

#include "engine/event_sink.hpp"
#include "engine/symbol_table.hpp"

namespace engine {

// Schreibt Events im Text-Format von plan.txt §4.1, eine Zeile pro Event ('\n').
//   ACK,<id>,<seq>
//   REJECT,<id>,<reason>
//   TRADE,<symbol>,<price>,<qty>,<buy_id>,<sell_id>,<BUY|SELL>
//   FILL,<id>,<filled_qty>,<remaining_qty>     (filled_qty = Menge DIESES Trades)
//   CANCELLED,<id>,<remaining_qty>,<reason>
//   BOOK,<symbol>,BIDS:<p>x<q>;<p>x<q>,ASKS:<p>x<q>
//     (Level mit ';' getrennt, kein abschließendes ';', leere Seite = "BIDS:")
class TextEventSink final : public EventSink {
public:
    TextEventSink(std::ostream& out, const SymbolTable& symbols) : out_(out), symbols_(symbols) {}

    void on_ack(OrderId id, SeqNo seq) override;
    void on_reject(OrderId id, RejectReason reason) override;
    void on_trade(const Trade& trade) override;
    void on_fill(OrderId id, Quantity filled, Quantity remaining) override;
    void on_cancelled(OrderId id, Quantity remaining, CancelReason reason) override;

    void on_book(const BookSnapshot& snapshot) override { write_book(snapshot); }

    void write_book(const BookSnapshot& snapshot);

private:
    std::ostream& out_;
    const SymbolTable& symbols_;
};

}  // namespace engine
