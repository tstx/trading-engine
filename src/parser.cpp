#include "engine/parser.hpp"

namespace engine {

ParseResult Parser::parse_line(std::string_view /*line*/) {
    (void)symbols_;
    (void)auto_register_;
    return ParseError{"not implemented"};
}

}  // namespace engine
