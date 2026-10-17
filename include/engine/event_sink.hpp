#pragma once

#include "engine/types.hpp"

namespace engine {

// Empfänger aller Engine-Events. Reihenfolge pro NEW-Order:
//   ACK (oder REJECT, dann Ende)
//   pro Match: on_trade, on_fill(resting), on_fill(aggressor)
//   ggf. on_cancelled(aggressor, rest, NoLiquidity) bei MARKET-Rest
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void on_ack(OrderId id, SeqNo seq) = 0;
    virtual void on_reject(OrderId id, RejectReason reason) = 0;
    virtual void on_trade(const Trade& trade) = 0;
    virtual void on_fill(OrderId id, Quantity filled, Quantity remaining) = 0;
    virtual void on_cancelled(OrderId id, Quantity remaining, CancelReason reason) = 0;
};

}  // namespace engine
