#include "engine/text_event_sink.hpp"

#include <vector>

namespace engine {

namespace {

// "<p>x<q>;<p>x<q>", kein abschließendes ';', leer bei leerer Seite.
void write_levels(std::ostream& out, const std::vector<BookLevel>& levels) {
    bool first = true;
    for (const BookLevel& level : levels) {
        if (!first) {
            out << ';';
        }
        first = false;
        out << level.price << 'x' << level.quantity;
    }
}

}  // namespace

void TextEventSink::on_ack(OrderId id, SeqNo seq) { out_ << "ACK," << id << ',' << seq << '\n'; }

void TextEventSink::on_reject(OrderId id, RejectReason reason) {
    out_ << "REJECT," << id << ',' << to_string(reason) << '\n';
}

void TextEventSink::on_trade(const Trade& trade) {
    out_ << "TRADE," << symbols_.name(trade.symbol) << ',' << trade.price << ',' << trade.quantity << ','
         << trade.buy_order_id << ',' << trade.sell_order_id << ',' << to_string(trade.aggressor) << '\n';
}

void TextEventSink::on_fill(OrderId id, Quantity filled, Quantity remaining) {
    out_ << "FILL," << id << ',' << filled << ',' << remaining << '\n';
}

void TextEventSink::on_cancelled(OrderId id, Quantity remaining, CancelReason reason) {
    out_ << "CANCELLED," << id << ',' << remaining << ',' << to_string(reason) << '\n';
}

void TextEventSink::write_book(const BookSnapshot& snapshot) {
    out_ << "BOOK," << symbols_.name(snapshot.symbol) << ",BIDS:";
    write_levels(out_, snapshot.bids);
    out_ << ",ASKS:";
    write_levels(out_, snapshot.asks);
    out_ << '\n';
}

}  // namespace engine
