#pragma once

#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

namespace engine {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // in Ticks, nie double
using Quantity = std::uint64_t;
using SymbolId = std::uint32_t;  // Symbol-String wird beim Parsen auf ID gemappt
using SeqNo = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

struct NewOrder {
    OrderId id;
    SymbolId symbol;
    Side side;
    OrderType type;
    Price price;  // 0 bei Market (wird ignoriert)
    Quantity quantity;
};

struct CancelOrder {
    OrderId id;
};

struct ModifyOrder {
    OrderId id;
    Price new_price;
    Quantity new_quantity;
};

using Command = std::variant<NewOrder, CancelOrder, ModifyOrder>;

enum class RejectReason : std::uint8_t {
    DuplicateId,
    InvalidQuantity,
    InvalidPrice,
    UnknownSymbol,
    UnknownOrder,
    InvalidModify,
};

enum class CancelReason : std::uint8_t { User, NoLiquidity };

struct Trade {
    SymbolId symbol;
    Price price;  // immer Preis der resting Order (FR-7)
    Quantity quantity;
    OrderId buy_order_id;
    OrderId sell_order_id;
    Side aggressor;
};

struct BookLevel {
    Price price;
    Quantity quantity;  // Summe aller Orders auf dem Level
};

struct BookSnapshot {
    SymbolId symbol;
    std::vector<BookLevel> bids;  // bester (höchster) zuerst
    std::vector<BookLevel> asks;  // bester (niedrigster) zuerst
};

// Protokoll-Schreibweisen: "BUY"/"SELL", "LIMIT"/"MARKET",
// "DUPLICATE_ID", ..., "USER"/"NO_LIQUIDITY".
std::string_view to_string(Side side) noexcept;
std::string_view to_string(OrderType type) noexcept;
std::string_view to_string(RejectReason reason) noexcept;
std::string_view to_string(CancelReason reason) noexcept;

}  // namespace engine
