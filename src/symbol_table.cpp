#include "engine/symbol_table.hpp"

namespace engine {

SymbolId SymbolTable::add(std::string_view name) {
    if (auto existing = find(name)) {
        return *existing;
    }
    auto id = static_cast<SymbolId>(names_.size());
    names_.emplace_back(name);
    ids_.emplace(names_.back(), id);
    return id;
}

std::optional<SymbolId> SymbolTable::find(std::string_view name) const {
    auto it = ids_.find(std::string(name));
    if (it == ids_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::string_view SymbolTable::name(SymbolId id) const { return names_.at(id); }

}  // namespace engine
