#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/types.hpp"

namespace engine {

// Bidirektionales Mapping Symbol-String <-> SymbolId. IDs sind dicht ab 0.
class SymbolTable {
public:
    // Liefert die bestehende ID oder vergibt eine neue.
    SymbolId add(std::string_view name);
    [[nodiscard]] std::optional<SymbolId> find(std::string_view name) const;
    [[nodiscard]] std::string_view name(SymbolId id) const;  // id muss gültig sein
    [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }

private:
    std::vector<std::string> names_;
    std::unordered_map<std::string, SymbolId> ids_;
};

}  // namespace engine
