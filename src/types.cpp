#include "engine/types.hpp"

namespace engine {

std::string_view to_string(Side side) noexcept {
    switch (side) {
        case Side::Buy: return "BUY";
        case Side::Sell: return "SELL";
    }
    return "?";
}

std::string_view to_string(OrderType type) noexcept {
    switch (type) {
        case OrderType::Limit: return "LIMIT";
        case OrderType::Market: return "MARKET";
    }
    return "?";
}

std::string_view to_string(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::DuplicateId: return "DUPLICATE_ID";
        case RejectReason::InvalidQuantity: return "INVALID_QUANTITY";
        case RejectReason::InvalidPrice: return "INVALID_PRICE";
        case RejectReason::UnknownSymbol: return "UNKNOWN_SYMBOL";
        case RejectReason::UnknownOrder: return "UNKNOWN_ORDER";
        case RejectReason::InvalidModify: return "INVALID_MODIFY";
    }
    return "?";
}

std::string_view to_string(CancelReason reason) noexcept {
    switch (reason) {
        case CancelReason::User: return "USER";
        case CancelReason::NoLiquidity: return "NO_LIQUIDITY";
    }
    return "?";
}

}  // namespace engine
