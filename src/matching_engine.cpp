#include "engine/matching_engine.hpp"

namespace engine {

MatchingEngine::MatchingEngine(EventSink& sink) : sink_(sink) {}
MatchingEngine::~MatchingEngine() = default;

void MatchingEngine::add_symbol(SymbolId /*symbol*/) {}
bool MatchingEngine::has_symbol(SymbolId /*symbol*/) const { return false; }

void MatchingEngine::process(const Command& cmd) {
    std::visit([this](const auto& c) { handle(c); }, cmd);
}

BookSnapshot MatchingEngine::snapshot(SymbolId symbol, std::size_t /*depth*/) const {
    return BookSnapshot{symbol, {}, {}};
}

void MatchingEngine::handle(const NewOrder& /*order*/) { (void)next_seq_; }
void MatchingEngine::handle(const CancelOrder& cancel) { sink_.on_reject(cancel.id, RejectReason::UnknownOrder); }
void MatchingEngine::handle(const ModifyOrder& modify) { sink_.on_reject(modify.id, RejectReason::UnknownOrder); }

}  // namespace engine
