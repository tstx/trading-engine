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

// Matching-Schleife für einen Aggressor (NEW und MODIFY mit Prioritätsverlust).
// Der Aggressor liegt währenddessen NICHT im Buch. Pro Match: TRADE, FILL(resting),
// FILL(aggressor). Vollständig gefüllte resting Orders verlassen Buch, live_orders und Pool.
// Danach entscheidet der Aufrufer über den Rest des Aggressors.
template <typename Book>
void match(Book& book, Order& aggressor, OrderType type, EventSink& sink, FlatHashMap<OrderId, Order*>& live_orders,
           ObjectPool<Order>& orders) {
    const bool buy = aggressor.side == Side::Buy;
    while (aggressor.remaining > 0) {
        Order* resting = buy ? book.best_ask() : book.best_bid();
        if (resting == nullptr || !crosses(aggressor, type, *resting)) {
            break;
        }

        const Quantity qty = std::min(aggressor.remaining, resting->remaining);
        book.fill(resting, qty);
        aggressor.remaining -= qty;
        aggressor.filled += qty;

        sink.on_trade(Trade{
            .symbol = aggressor.symbol,
            .price = resting->price,
            .quantity = qty,
            .buy_order_id = buy ? aggressor.id : resting->id,
            .sell_order_id = buy ? resting->id : aggressor.id,
            .aggressor = aggressor.side,
        });
        sink.on_fill(resting->id, qty, resting->remaining);
        sink.on_fill(aggressor.id, qty, aggressor.remaining);

        if (resting->remaining == 0) {
            book.remove(resting);
            live_orders.erase(resting->id);  // ID bleibt in known_ids_
            orders.destroy(resting);
        }
    }
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

    match(book, *aggressor, order.type, sink_, live_orders_, orders_);

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

// FR-11: nur liegende Orders sind cancelbar; alles andere ist UNKNOWN_ORDER.
template <typename Book>
void BasicMatchingEngine<Book>::handle(const CancelOrder& cancel) {
    Order* const* found = live_orders_.find(cancel.id);
    if (found == nullptr) {
        sink_.on_reject(cancel.id, RejectReason::UnknownOrder);
        return;
    }
    Order* order = *found;
    books_.find(order->symbol)->second->remove(order);
    live_orders_.erase(cancel.id);  // ID bleibt in known_ids_
    sink_.on_cancelled(cancel.id, order->remaining, CancelReason::User);
    orders_.destroy(order);
}

// FR-12: Cancel/Replace. new_quantity ist die neue Gesamtmenge inkl. filled.
template <typename Book>
void BasicMatchingEngine<Book>::handle(const ModifyOrder& modify) {
    Order* const* found = live_orders_.find(modify.id);
    if (found == nullptr) {
        sink_.on_reject(modify.id, RejectReason::UnknownOrder);
        return;
    }
    Order* order = *found;
    if (modify.new_quantity <= order->filled) {
        sink_.on_reject(modify.id, RejectReason::InvalidModify);
        return;
    }
    Book& book = *books_.find(order->symbol)->second;
    if (modify.new_price <= 0 || !book.accepts_price(modify.new_price)) {
        sink_.on_reject(modify.id, RejectReason::InvalidPrice);
        return;
    }

    const Quantity new_remaining = modify.new_quantity - order->filled;

    // Reduktion (oder unveränderte Menge) bei gleichem Preis: Position bleibt, alte Seq.
    if (modify.new_price == order->price && new_remaining <= order->remaining) {
        if (new_remaining < order->remaining) {
            book.reduce(order, new_remaining);
        }
        sink_.on_ack(modify.id, order->seq);
        return;
    }

    // Prioritätsverlust: raus aus dem Buch, neue Seq, als LIMIT-Aggressor matchen.
    // Die Order bleibt währenddessen in live_orders_ (sie liegt nicht im Buch und kann
    // daher nicht mit sich selbst matchen).
    book.remove(order);
    order->price = modify.new_price;
    order->remaining = new_remaining;
    order->seq = next_seq_++;
    sink_.on_ack(modify.id, order->seq);

    match(book, *order, OrderType::Limit, sink_, live_orders_, orders_);

    if (order->remaining == 0) {
        live_orders_.erase(modify.id);  // ID bleibt in known_ids_
        orders_.destroy(order);
        return;
    }
    book.add(order);
}

template class BasicMatchingEngine<MapOrderBook>;
template class BasicMatchingEngine<VectorOrderBook>;
template class BasicMatchingEngine<TickOrderBook>;

}  // namespace engine
