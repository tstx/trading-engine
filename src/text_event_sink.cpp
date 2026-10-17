#include "engine/text_event_sink.hpp"

namespace engine {

void TextEventSink::on_ack(OrderId /*id*/, SeqNo /*seq*/) {}
void TextEventSink::on_reject(OrderId /*id*/, RejectReason /*reason*/) {}
void TextEventSink::on_trade(const Trade& /*trade*/) {}
void TextEventSink::on_fill(OrderId /*id*/, Quantity /*filled*/, Quantity /*remaining*/) {}
void TextEventSink::on_cancelled(OrderId /*id*/, Quantity /*remaining*/, CancelReason /*reason*/) {}
void TextEventSink::write_book(const BookSnapshot& /*snapshot*/) {
    (void)out_;
    (void)symbols_;
}

}  // namespace engine
