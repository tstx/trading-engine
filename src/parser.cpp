#include "engine/parser.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <optional>
#include <system_error>

namespace engine {

namespace {

// Eins mehr als NEW braucht, damit "zu viele Felder" erkennbar ist.
constexpr std::size_t kMaxFields = 8;

struct Fields {
    std::array<std::string_view, kMaxFields> items{};
    std::size_t count{0};
    bool overflow{false};
};

Fields split(std::string_view line) {
    Fields f;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = line.find(',', start);
        const std::string_view field =
            line.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        if (f.count == kMaxFields) {
            f.overflow = true;
            return f;
        }
        f.items[f.count++] = field;
        if (comma == std::string_view::npos) {
            return f;
        }
        start = comma + 1;
    }
}

// Ganzes Feld muss konsumiert werden; kein Whitespace, kein '+'.
template <typename T>
std::optional<T> parse_number(std::string_view s) {
    if (s.empty()) {
        return std::nullopt;
    }
    T value{};
    const char* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), last, value);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

bool is_blank(std::string_view s) {
    for (char c : s) {
        if (c != ' ' && c != '\t') {
            return false;
        }
    }
    return true;
}

ParseError wrong_field_count(std::string_view keyword, std::size_t expected) {
    return ParseError{std::string(keyword) + ": expected " + std::to_string(expected) + " fields"};
}

ParseError bad_field(std::string_view what, std::string_view value) {
    return ParseError{"invalid " + std::string(what) + " '" + std::string(value) + "'"};
}

}  // namespace

ParseResult Parser::parse_line(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    if (is_blank(line) || line.front() == '#') {
        return EmptyLine{};
    }

    const Fields f = split(line);
    const std::string_view keyword = f.items[0];

    if (keyword == "NEW") {
        if (f.overflow || f.count != 7) {
            return wrong_field_count(keyword, 7);
        }
        const auto id = parse_number<OrderId>(f.items[1]);
        if (!id) {
            return bad_field("order id", f.items[1]);
        }
        const std::string_view sym_name = f.items[2];
        if (sym_name.empty()) {
            return ParseError{"empty symbol"};
        }
        Side side{};
        if (f.items[3] == "BUY") {
            side = Side::Buy;
        } else if (f.items[3] == "SELL") {
            side = Side::Sell;
        } else {
            return bad_field("side", f.items[3]);
        }
        OrderType type{};
        if (f.items[4] == "LIMIT") {
            type = OrderType::Limit;
        } else if (f.items[4] == "MARKET") {
            type = OrderType::Market;
        } else {
            return bad_field("order type", f.items[4]);
        }
        const auto price = parse_number<Price>(f.items[5]);
        if (!price) {
            return bad_field("price", f.items[5]);
        }
        const auto qty = parse_number<Quantity>(f.items[6]);
        if (!qty) {
            return bad_field("quantity", f.items[6]);
        }
        // Symbol erst nach vollständiger Syntaxprüfung registrieren,
        // damit fehlerhafte Zeilen keine Symbole anlegen.
        SymbolId symbol = kUnknownSymbol;
        if (auto_register_) {
            symbol = symbols_.add(sym_name);
        } else if (auto found = symbols_.find(sym_name)) {
            symbol = *found;
        }
        return Command{NewOrder{*id, symbol, side, type, *price, *qty}};
    }

    if (keyword == "CANCEL") {
        if (f.overflow || f.count != 2) {
            return wrong_field_count(keyword, 2);
        }
        const auto id = parse_number<OrderId>(f.items[1]);
        if (!id) {
            return bad_field("order id", f.items[1]);
        }
        return Command{CancelOrder{*id}};
    }

    if (keyword == "MODIFY") {
        if (f.overflow || f.count != 4) {
            return wrong_field_count(keyword, 4);
        }
        const auto id = parse_number<OrderId>(f.items[1]);
        if (!id) {
            return bad_field("order id", f.items[1]);
        }
        const auto price = parse_number<Price>(f.items[2]);
        if (!price) {
            return bad_field("price", f.items[2]);
        }
        const auto qty = parse_number<Quantity>(f.items[3]);
        if (!qty) {
            return bad_field("quantity", f.items[3]);
        }
        return Command{ModifyOrder{*id, *price, *qty}};
    }

    if (keyword == "BOOK") {
        if (f.overflow || f.count != 2) {
            return wrong_field_count(keyword, 2);
        }
        const std::string_view sym_name = f.items[1];
        if (sym_name.empty()) {
            return ParseError{"empty symbol"};
        }
        if (auto_register_) {
            return BookQuery{symbols_.add(sym_name)};
        }
        if (auto found = symbols_.find(sym_name)) {
            return BookQuery{*found};
        }
        return ParseError{"unknown symbol '" + std::string(sym_name) + "'"};
    }

    return ParseError{"unknown message type '" + std::string(keyword) + "'"};
}

}  // namespace engine
