#pragma once

#include <string>
#include <string_view>
#include <variant>

#include "engine/symbol_table.hpp"
#include "engine/types.hpp"

namespace engine {

// "BOOK,<symbol>" — Abfrage, kein Command an die Engine.
struct BookQuery {
    SymbolId symbol;
};

// Syntaktisch ungültige Zeile (falsche Feldanzahl, keine Zahl, unbekanntes Keyword ...).
struct ParseError {
    std::string message;
};

// Leerzeile oder Kommentar (beginnt mit '#').
struct EmptyLine {};

using ParseResult = std::variant<Command, BookQuery, ParseError, EmptyLine>;

// Parst eine Zeile des Text-Protokolls (plan.txt §3.1). Ein abschließendes '\r'
// wird ignoriert. Symbole werden über `symbols` aufgelöst:
// - auto_register == true: unbekannte Symbole werden in `symbols` angelegt.
// - auto_register == false: unbekanntes Symbol in NEW ergibt eine NewOrder mit
//   symbol = kUnknownSymbol (die Engine rejected dann mit UNKNOWN_SYMBOL);
//   unbekanntes Symbol in BOOK ergibt ParseError.
// Semantische Prüfungen (quantity > 0, price > 0) macht die Engine, nicht der Parser.
inline constexpr SymbolId kUnknownSymbol = static_cast<SymbolId>(-1);

class Parser {
public:
    Parser(SymbolTable& symbols, bool auto_register) : symbols_(symbols), auto_register_(auto_register) {}

    [[nodiscard]] ParseResult parse_line(std::string_view line);

private:
    SymbolTable& symbols_;
    bool auto_register_;
};

}  // namespace engine
