#include "engine/parser.hpp"

#include <string>
#include <string_view>
#include <variant>

#include <gtest/gtest.h>

#include "engine/symbol_table.hpp"

using namespace engine;

namespace {

NewOrder expect_new(const ParseResult& r) {
    const auto* cmd = std::get_if<Command>(&r);
    EXPECT_NE(cmd, nullptr);
    if (cmd == nullptr) {
        return {};
    }
    const auto* order = std::get_if<NewOrder>(cmd);
    EXPECT_NE(order, nullptr);
    return order != nullptr ? *order : NewOrder{};
}

bool is_error(const ParseResult& r) { return std::holds_alternative<ParseError>(r); }

}  // namespace

TEST(Parser, NewLimitOrder) {
    SymbolTable symbols;
    Parser parser(symbols, true);
    const NewOrder o = expect_new(parser.parse_line("NEW,1,ACME,SELL,LIMIT,10100,50"));
    EXPECT_EQ(o.id, 1U);
    EXPECT_EQ(symbols.name(o.symbol), "ACME");
    EXPECT_EQ(o.side, Side::Sell);
    EXPECT_EQ(o.type, OrderType::Limit);
    EXPECT_EQ(o.price, 10100);
    EXPECT_EQ(o.quantity, 50U);
}

TEST(Parser, NewMarketOrder) {
    SymbolTable symbols;
    Parser parser(symbols, true);
    const NewOrder o = expect_new(parser.parse_line("NEW,11,ACME,BUY,MARKET,0,100"));
    EXPECT_EQ(o.side, Side::Buy);
    EXPECT_EQ(o.type, OrderType::Market);
    EXPECT_EQ(o.price, 0);
    EXPECT_EQ(o.quantity, 100U);
}

TEST(Parser, NegativePriceAndZeroQuantityAreSyntacticallyValid) {
    SymbolTable symbols;
    Parser parser(symbols, true);
    const NewOrder o = expect_new(parser.parse_line("NEW,1,ACME,BUY,LIMIT,-5,0"));
    EXPECT_EQ(o.price, -5);
    EXPECT_EQ(o.quantity, 0U);
}

TEST(Parser, LargeNumbers) {
    SymbolTable symbols;
    Parser parser(symbols, true);
    const NewOrder o =
        expect_new(parser.parse_line("NEW,18446744073709551615,A,BUY,LIMIT,9223372036854775807,18446744073709551615"));
    EXPECT_EQ(o.id, 18446744073709551615ULL);
    EXPECT_EQ(o.price, 9223372036854775807LL);
    EXPECT_EQ(o.quantity, 18446744073709551615ULL);
    EXPECT_TRUE(is_error(parser.parse_line("NEW,18446744073709551616,A,BUY,LIMIT,1,1")));
    EXPECT_TRUE(is_error(parser.parse_line("NEW,1,A,BUY,LIMIT,9223372036854775808,1")));
}

TEST(Parser, TrailingCarriageReturnIgnored) {
    SymbolTable symbols;
    Parser parser(symbols, true);
    const NewOrder o = expect_new(parser.parse_line("NEW,1,ACME,SELL,LIMIT,10100,50\r"));
    EXPECT_EQ(o.quantity, 50U);
    EXPECT_TRUE(std::holds_alternative<EmptyLine>(parser.parse_line("\r")));
}

TEST(Parser, EmptyAndCommentLines) {
    SymbolTable symbols;
    Parser parser(symbols, true);
    EXPECT_TRUE(std::holds_alternative<EmptyLine>(parser.parse_line("")));
    EXPECT_TRUE(std::holds_alternative<EmptyLine>(parser.parse_line("   ")));
    EXPECT_TRUE(std::holds_alternative<EmptyLine>(parser.parse_line("# comment,with,commas")));
    EXPECT_TRUE(std::holds_alternative<EmptyLine>(parser.parse_line("#")));
}

TEST(Parser, Cancel) {
    SymbolTable symbols;
    Parser parser(symbols, true);
    const ParseResult r = parser.parse_line("CANCEL,42");
    ASSERT_TRUE(std::holds_alternative<Command>(r));
    const auto& cmd = std::get<Command>(r);
    ASSERT_TRUE(std::holds_alternative<CancelOrder>(cmd));
    EXPECT_EQ(std::get<CancelOrder>(cmd).id, 42U);
}

TEST(Parser, Modify) {
    SymbolTable symbols;
    Parser parser(symbols, true);
    const ParseResult r = parser.parse_line("MODIFY,7,-100,25");
    ASSERT_TRUE(std::holds_alternative<Command>(r));
    const auto& cmd = std::get<Command>(r);
    ASSERT_TRUE(std::holds_alternative<ModifyOrder>(cmd));
    const auto& m = std::get<ModifyOrder>(cmd);
    EXPECT_EQ(m.id, 7U);
    EXPECT_EQ(m.new_price, -100);
    EXPECT_EQ(m.new_quantity, 25U);
}

TEST(Parser, BookAutoRegister) {
    SymbolTable symbols;
    Parser parser(symbols, true);
    const ParseResult r = parser.parse_line("BOOK,ACME");
    ASSERT_TRUE(std::holds_alternative<BookQuery>(r));
    EXPECT_EQ(symbols.name(std::get<BookQuery>(r).symbol), "ACME");
    EXPECT_EQ(symbols.size(), 1U);
}

TEST(Parser, AutoRegisterReusesIds) {
    SymbolTable symbols;
    Parser parser(symbols, true);
    const NewOrder a = expect_new(parser.parse_line("NEW,1,ACME,BUY,LIMIT,1,1"));
    const NewOrder b = expect_new(parser.parse_line("NEW,2,FOO,BUY,LIMIT,1,1"));
    const NewOrder c = expect_new(parser.parse_line("NEW,3,ACME,BUY,LIMIT,1,1"));
    EXPECT_EQ(a.symbol, c.symbol);
    EXPECT_NE(a.symbol, b.symbol);
    EXPECT_EQ(symbols.size(), 2U);
}

TEST(Parser, FixedSymbolsUnknownNewGivesUnknownSymbolId) {
    SymbolTable symbols;
    const SymbolId acme = symbols.add("ACME");
    Parser parser(symbols, false);
    EXPECT_EQ(expect_new(parser.parse_line("NEW,1,ACME,BUY,LIMIT,1,1")).symbol, acme);
    EXPECT_EQ(expect_new(parser.parse_line("NEW,2,XYZ,BUY,LIMIT,1,1")).symbol, kUnknownSymbol);
    EXPECT_EQ(symbols.size(), 1U);
}

TEST(Parser, FixedSymbolsUnknownBookIsError) {
    SymbolTable symbols;
    const SymbolId acme = symbols.add("ACME");
    Parser parser(symbols, false);
    const ParseResult ok = parser.parse_line("BOOK,ACME");
    ASSERT_TRUE(std::holds_alternative<BookQuery>(ok));
    EXPECT_EQ(std::get<BookQuery>(ok).symbol, acme);
    EXPECT_TRUE(is_error(parser.parse_line("BOOK,XYZ")));
    EXPECT_EQ(symbols.size(), 1U);
}

TEST(Parser, ErroneousLineDoesNotRegisterSymbol) {
    SymbolTable symbols;
    Parser parser(symbols, true);
    EXPECT_TRUE(is_error(parser.parse_line("NEW,1,ACME,BUY,LIMIT,abc,1")));
    EXPECT_EQ(symbols.size(), 0U);
}

class ParserErrorTest : public ::testing::TestWithParam<std::string_view> {};

TEST_P(ParserErrorTest, IsParseError) {
    SymbolTable symbols;
    Parser parser(symbols, true);
    const ParseResult r = parser.parse_line(GetParam());
    ASSERT_TRUE(is_error(r)) << GetParam();
    EXPECT_FALSE(std::get<ParseError>(r).message.empty());
}

INSTANTIATE_TEST_SUITE_P(Malformed, ParserErrorTest,
                         ::testing::Values("FOO,1",                           // unbekanntes Keyword
                                           "new,1,ACME,BUY,LIMIT,1,1",        // Groß-/Kleinschreibung
                                           "NEW",                             // zu wenig Felder
                                           "NEW,1,ACME,BUY,LIMIT,1",          // zu wenig Felder
                                           "NEW,1,ACME,BUY,LIMIT,1,1,",       // zu viele Felder
                                           "NEW,1,ACME,BUY,LIMIT,1,1,9,9,9",  // viel zu viele Felder
                                           "NEW,x,ACME,BUY,LIMIT,1,1",        // id keine Zahl
                                           "NEW,-1,ACME,BUY,LIMIT,1,1",       // id negativ
                                           "NEW,+1,ACME,BUY,LIMIT,1,1",       // '+' nicht erlaubt
                                           "NEW,1,,BUY,LIMIT,1,1",            // leeres Symbol
                                           "NEW,1,ACME,BUYY,LIMIT,1,1",       // Seite
                                           "NEW,1,ACME,BUY,STOP,1,1",         // Typ
                                           "NEW,1,ACME,BUY,LIMIT,,1",         // leerer Preis
                                           "NEW,1,ACME,BUY,LIMIT,1.5,1",      // Dezimalpreis
                                           "NEW,1,ACME,BUY,LIMIT,10 ,1",      // Whitespace
                                           "NEW,1,ACME,BUY,LIMIT,1,-1",       // negative Menge
                                           "NEW,1,ACME,BUY,LIMIT,1,1x",       // Müll am Ende
                                           "CANCEL",                          // zu wenig Felder
                                           "CANCEL,1,2",                      // zu viele Felder
                                           "CANCEL,abc",                      // keine Zahl
                                           "MODIFY,1,2",                      // zu wenig Felder
                                           "MODIFY,1,2,3,4",                  // zu viele Felder
                                           "MODIFY,1,a,3",                    // Preis
                                           "MODIFY,1,2,-3",                   // Menge
                                           "BOOK",                            // zu wenig Felder
                                           "BOOK,",                           // leeres Symbol
                                           "BOOK,ACME,1",                     // zu viele Felder
                                           " NEW,1,ACME,BUY,LIMIT,1,1"));     // führendes Leerzeichen
