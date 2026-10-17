// CLI: trading_engine [--symbols A,B,...] [input-file]
// Liest das Text-Protokoll von Datei oder stdin, schreibt Events auf stdout.
#include <cstddef>
#include <fstream>
#include <iostream>
#include <istream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "engine/matching_engine.hpp"
#include "engine/parser.hpp"
#include "engine/symbol_table.hpp"
#include "engine/text_event_sink.hpp"

namespace {

constexpr std::size_t kBookDepth = 5;

void print_usage(std::ostream& out) { out << "usage: trading_engine [--symbols A,B,...] [input-file]\n"; }

// Registriert die kommagetrennten Symbole und macht sie handelbar; leere Einträge werden übersprungen.
void register_symbols(std::string_view list, engine::SymbolTable& symbols, engine::MatchingEngine& engine) {
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view name = list.substr(0, comma);
        if (!name.empty()) {
            engine.add_symbol(symbols.add(name));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);

    const char* symbol_list = nullptr;
    const char* input_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--symbols") {
            if (i + 1 >= argc) {
                print_usage(std::cerr);
                return 1;
            }
            symbol_list = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage(std::cout);
            return 0;
        } else if (input_path == nullptr) {
            input_path = argv[i];
        } else {
            print_usage(std::cerr);
            return 1;
        }
    }

    std::ifstream file;
    if (input_path != nullptr) {
        file.open(input_path);
        if (!file) {
            std::cerr << "cannot open input file '" << input_path << "'\n";
            return 1;
        }
    }
    std::istream& in = input_path != nullptr ? static_cast<std::istream&>(file) : std::cin;

    engine::SymbolTable symbols;
    engine::TextEventSink sink(std::cout, symbols);
    engine::MatchingEngine engine(sink);

    const bool auto_register = symbol_list == nullptr;
    if (!auto_register) {
        register_symbols(symbol_list, symbols, engine);
    }
    engine::Parser parser(symbols, auto_register);

    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const std::size_t known_before = symbols.size();
        const engine::ParseResult result = parser.parse_line(line);
        // Neu registrierte Symbole (IDs sind dicht) handelbar machen, bevor der Command verarbeitet wird.
        for (std::size_t id = known_before; id < symbols.size(); ++id) {
            engine.add_symbol(static_cast<engine::SymbolId>(id));
        }

        std::visit(
            [&](const auto& r) {
                using T = std::decay_t<decltype(r)>;
                if constexpr (std::is_same_v<T, engine::Command>) {
                    engine.process(r);
                } else if constexpr (std::is_same_v<T, engine::BookQuery>) {
                    sink.write_book(engine.snapshot(r.symbol, kBookDepth));
                } else if constexpr (std::is_same_v<T, engine::ParseError>) {
                    std::cerr << "ERROR," << line_no << ',' << r.message << '\n';
                }
                // EmptyLine: ignorieren
            },
            result);
    }

    std::cout.flush();
    return 0;
}
