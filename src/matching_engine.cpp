#include "engine/matching_engine.hpp"

#include <algorithm>

namespace engine {

MatchingEngine::MatchingEngine(EventSink& sink) : sink_(sink) {}
MatchingEngine::~MatchingEngine() = default;

void MatchingEngine::add_symbol(SymbolId symbol) {
    if (!books_.contains(symbol)) {
        books_.emplace(symbol, std::make_unique<OrderBook>(symbol));
    }
}

bool MatchingEngine::has_symbol(SymbolId symbol) const { return books_.contains(symbol); }

void MatchingEngine::process(const Command& cmd) {
    std::visit([this](const auto& c) { handle(c); }, cmd);
}

BookSnapshot MatchingEngine::snapshot(SymbolId symbol, std::size_t depth) const {
    auto it = books_.find(symbol);
    if (it == books_.end()) {
        return BookSnapshot{symbol, {}, {}};
    }
    return it->second->snapshot(depth);
}

namespace {

// Kreuzt der Aggressor mit der resting Order? MARKET kreuzt immer (FR-6).
bool crosses(const Order& aggressor, OrderType type, const Order& resting) noexcept {
    if (type == OrderType::Market) {
        return true;
    }
    return aggressor.side == Side::Buy ? resting.price <= aggressor.price : resting.price >= aggressor.price;
}

}  // namespace

void MatchingEngine::handle(const NewOrder& order) {
    // Validierung, Reihenfolge verbindlich (siehe Header).
    if (known_ids_.contains(order.id)) {
        sink_.on_reject(order.id, RejectReason::DuplicateId);
        return;
    }
    auto book_it = books_.find(order.symbol);
    if (book_it == books_.end()) {
        sink_.on_reject(order.id, RejectReason::UnknownSymbol);
        return;
    }
    if (order.quantity == 0) {
        sink_.on_reject(order.id, RejectReason::InvalidQuantity);
        return;
    }
    if (order.type == OrderType::Limit && order.price <= 0) {
        sink_.on_reject(order.id, RejectReason::InvalidPrice);
        return;
    }

    OrderBook& book = *book_it->second;
    const SeqNo seq = next_seq_++;
    known_ids_.insert(order.id);

    auto aggressor = std::make_unique<Order>();
    aggressor->id = order.id;
    aggressor->side = order.side;
    aggressor->price = order.type == OrderType::Market ? Price{0} : order.price;
    aggressor->remaining = order.quantity;
    aggressor->seq = seq;

    sink_.on_ack(order.id, seq);

    while (aggressor->remaining > 0) {
        Order* resting = order.side == Side::Buy ? book.best_ask() : book.best_bid();
        if (resting == nullptr || !crosses(*aggressor, order.type, *resting)) {
            break;
        }

        const Quantity qty = std::min(aggressor->remaining, resting->remaining);
        book.fill(resting, qty);
        aggressor->remaining -= qty;
        aggressor->filled += qty;

        const bool buy = order.side == Side::Buy;
        sink_.on_trade(Trade{
            .symbol = order.symbol,
            .price = resting->price,
            .quantity = qty,
            .buy_order_id = buy ? aggressor->id : resting->id,
            .sell_order_id = buy ? resting->id : aggressor->id,
            .aggressor = order.side,
        });
        sink_.on_fill(resting->id, qty, resting->remaining);
        sink_.on_fill(aggressor->id, qty, aggressor->remaining);

        if (resting->remaining == 0) {
            book.remove(resting);
            live_orders_.erase(resting->id);  // gibt die Order frei; ID bleibt in known_ids_
        }
    }

    if (aggressor->remaining == 0) {
        return;
    }
    if (order.type == OrderType::Market) {
        sink_.on_cancelled(aggressor->id, aggressor->remaining, CancelReason::NoLiquidity);
        return;
    }
    Order* resting = aggressor.get();
    live_orders_.emplace(order.id, std::move(aggressor));
    book.add(resting);
}

void MatchingEngine::handle(const CancelOrder& cancel) { sink_.on_reject(cancel.id, RejectReason::UnknownOrder); }
void MatchingEngine::handle(const ModifyOrder& modify) { sink_.on_reject(modify.id, RejectReason::UnknownOrder); }

}  // namespace engine
