// CLI: trading_engine [--threaded] [--symbols A,B,...] [input-file]
// Liest das Text-Protokoll von Datei oder stdin, schreibt Events auf stdout.
//
// --threaded: Verarbeitung über die Pipeline (Engine- und Logger-Thread); der Haupt-Thread
// ist der einzige Producer. Die Ausgabe ist bitgenau gleich dem single-threaded Modus (NFR-5).
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <istream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "engine/event.hpp"
#include "engine/matching_engine.hpp"
#include "engine/parser.hpp"
#include "engine/pipeline.hpp"
#include "engine/symbol_table.hpp"
#include "engine/text_event_sink.hpp"

namespace {

constexpr std::size_t kBookDepth = 5;

void print_usage(std::ostream& out) { out << "usage: trading_engine [--threaded] [--symbols A,B,...] [input-file]\n"; }

// Ziel der geparsten Eingabe: direkt die Engine (single-threaded) oder eine Request-Liste
// für die Pipeline (--threaded).
class Frontend {
public:
    virtual ~Frontend() = default;
    virtual void add_symbol(engine::SymbolId symbol) = 0;
    virtual void command(const engine::Command& cmd) = 0;
    virtual void book(engine::SymbolId symbol) = 0;
};

class DirectFrontend final : public Frontend {
public:
    DirectFrontend(engine::MatchingEngine& engine, engine::TextEventSink& sink) : engine_(engine), sink_(sink) {}

    void add_symbol(engine::SymbolId symbol) override { engine_.add_symbol(symbol); }
    void command(const engine::Command& cmd) override { engine_.process(cmd); }
    void book(engine::SymbolId symbol) override { sink_.write_book(engine_.snapshot(symbol, kBookDepth)); }

private:
    engine::MatchingEngine& engine_;
    engine::TextEventSink& sink_;
};

class RequestCollector final : public Frontend {
public:
    void add_symbol(engine::SymbolId symbol) override { requests.emplace_back(engine::AddSymbolRequest{symbol}); }
    void command(const engine::Command& cmd) override { requests.emplace_back(cmd); }
    void book(engine::SymbolId symbol) override {
        requests.emplace_back(engine::BookRequest{symbol, static_cast<std::uint32_t>(kBookDepth)});
    }

    std::vector<engine::Request> requests;
};

// Registriert die kommagetrennten Symbole und macht sie handelbar; leere Einträge werden übersprungen.
void register_symbols(std::string_view list, engine::SymbolTable& symbols, Frontend& frontend) {
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view name = list.substr(0, comma);
        if (!name.empty()) {
            frontend.add_symbol(symbols.add(name));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }
}

// Parst die gesamte Eingabe und reicht jede Zeile an frontend weiter; ParseErrors gehen sofort auf stderr.
void parse_input(std::istream& in, engine::SymbolTable& symbols, engine::Parser& parser, Frontend& frontend) {
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const std::size_t known_before = symbols.size();
        const engine::ParseResult result = parser.parse_line(line);
        // Neu registrierte Symbole (IDs sind dicht) handelbar machen, bevor der Command verarbeitet wird.
        for (std::size_t id = known_before; id < symbols.size(); ++id) {
            frontend.add_symbol(static_cast<engine::SymbolId>(id));
        }

        std::visit(
            [&](const auto& r) {
                using T = std::decay_t<decltype(r)>;
                if constexpr (std::is_same_v<T, engine::Command>) {
                    frontend.command(r);
                } else if constexpr (std::is_same_v<T, engine::BookQuery>) {
                    frontend.book(r.symbol);
                } else if constexpr (std::is_same_v<T, engine::ParseError>) {
                    std::cerr << "ERROR," << line_no << ',' << r.message << '\n';
                }
                // EmptyLine: ignorieren
            },
            result);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);

    bool threaded = false;
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
        } else if (arg == "--threaded") {
            threaded = true;
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
    const bool auto_register = symbol_list == nullptr;
    engine::Parser parser(symbols, auto_register);

    if (!threaded) {
        engine::MatchingEngine engine(sink);
        DirectFrontend frontend(engine, sink);
        if (!auto_register) {
            register_symbols(symbol_list, symbols, frontend);
        }
        parse_input(in, symbols, parser, frontend);
    } else {
        // Data Race vermeiden: TextEventSink liest SymbolTable::name() im Logger-Thread, der Parser
        // registriert neue Symbole im Haupt-Thread (Reallokation des Namens-Vektors). Deshalb wird
        // die Eingabe hier erst vollständig geparst und danach submittet: die SymbolTable ist
        // ab start() unveränderlich, und die Thread-Erzeugung in start() stellt die nötige
        // happens-before-Beziehung her — ohne Lock im Logger und ohne Änderung an SymbolTable.
        // Preis: die Requests der gesamten Eingabe liegen einmal im Speicher (trivial kopierbar,
        // sizeof(engine::Request) Byte pro Zeile).
        RequestCollector collector;
        if (!auto_register) {
            register_symbols(symbol_list, symbols, collector);
        }
        parse_input(in, symbols, parser, collector);

        engine::Pipeline pipeline(sink);
        pipeline.start();
        for (const engine::Request& request : collector.requests) {
            pipeline.submit(request);
        }
        pipeline.stop();  // wartet, bis alle Events auf std::cout geschrieben sind
    }

    std::cout.flush();
    return 0;
}
