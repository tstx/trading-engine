#include "engine/matching_engine.hpp"

#include <algorithm>

namespace engine {

template <typename Book>
BasicMatchingEngine<Book>::BasicMatchingEngine(EventSink& sink, const EngineConfig& config)
    : config_(config),
      sink_(sink),
      orders_(config.order_capacity),
      live_orders_(config.order_capacity),
      known_ids_(config.id_capacity) {
    books_.reserve(config.symbol_capacity);
}

template <typename Book>
BasicMatchingEngine<Book>::~BasicMatchingEngine() = default;

template <typename Book>
void BasicMatchingEngine<Book>::add_symbol(SymbolId symbol) {
    if (!books_.contains(symbol)) {
        books_.emplace(symbol, std::make_unique<Book>(symbol, config_));
    }
}

template <typename Book>
bool BasicMatchingEngine<Book>::has_symbol(SymbolId symbol) const {
    return books_.contains(symbol);
}

template <typename Book>
void BasicMatchingEngine<Book>::process(const Command& cmd) {
    std::visit([this](const auto& c) { this->handle(c); }, cmd);
}

template <typename Book>
BookSnapshot BasicMatchingEngine<Book>::snapshot(SymbolId symbol, std::size_t depth) const {
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

template <typename Book>
void BasicMatchingEngine<Book>::handle(const NewOrder& order) {
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
    Book& book = *book_it->second;
    if (order.type == OrderType::Limit && (order.price <= 0 || !book.accepts_price(order.price))) {
        sink_.on_reject(order.id, RejectReason::InvalidPrice);
        return;
    }

    const SeqNo seq = next_seq_++;
    known_ids_.insert(order.id);

    Order* aggressor = orders_.create();
    aggressor->id = order.id;
    aggressor->symbol = order.symbol;
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
            live_orders_.erase(resting->id);  // ID bleibt in known_ids_
            orders_.destroy(resting);
        }
    }

    if (aggressor->remaining == 0) {
        orders_.destroy(aggressor);
        return;
    }
    if (order.type == OrderType::Market) {
        sink_.on_cancelled(aggressor->id, aggressor->remaining, CancelReason::NoLiquidity);
        orders_.destroy(aggressor);
        return;
    }
    live_orders_.insert(order.id, aggressor);
    book.add(aggressor);
}

// Stufe 3: CANCEL/MODIFY (siehe Header) — STUB.
template <typename Book>
void BasicMatchingEngine<Book>::handle(const CancelOrder& cancel) {
    sink_.on_reject(cancel.id, RejectReason::UnknownOrder);
}

template <typename Book>
void BasicMatchingEngine<Book>::handle(const ModifyOrder& modify) {
    sink_.on_reject(modify.id, RejectReason::UnknownOrder);
}

template class BasicMatchingEngine<MapOrderBook>;
template class BasicMatchingEngine<VectorOrderBook>;
template class BasicMatchingEngine<TickOrderBook>;

}  // namespace engine
