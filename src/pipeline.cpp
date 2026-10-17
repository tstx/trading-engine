#include "engine/pipeline.hpp"

namespace engine {

// Stub (Stufe 2, wird implementiert).
Pipeline::Pipeline(EventSink& output) : output_(output) {}
Pipeline::~Pipeline() { stop(); }

void Pipeline::start() { running_ = true; }
void Pipeline::submit(const Request& /*request*/) {}
void Pipeline::stop() { running_ = false; }

void Pipeline::engine_loop() {}
void Pipeline::logger_loop() { (void)output_; }

}  // namespace engine
