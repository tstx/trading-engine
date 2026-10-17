#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>

#include "engine/event.hpp"
#include "engine/event_sink.hpp"
#include "engine/mpmc_queue.hpp"
#include "engine/spsc_queue.hpp"

namespace engine {

inline constexpr std::size_t kRequestQueueCapacity = 1 << 16;  // Zweierpotenz (MPMC)
inline constexpr std::size_t kEventQueueCapacity = 1 << 16;

using RequestQueue = MpmcQueue<Request, kRequestQueueCapacity>;
using EventQueue = SpscQueue<Event, kEventQueueCapacity>;

// Nebenläufige Pipeline (plan.txt Stufe 2):
//
//   Producer-Threads --submit()--> RequestQueue (MPMC, als MPSC genutzt)
//     --> Engine-Thread (einzige Instanz von MatchingEngine, single-threaded Matching)
//     --> EventQueue (SPSC) --> Logger-Thread --> output (EventSink des Aufrufers)
//
// Garantien:
// - output wird ausschließlich vom Logger-Thread aufgerufen (braucht selbst keine Synchronisation).
// - Pro Producer werden Requests in submit()-Reihenfolge verarbeitet. Bei genau einem
//   Producer ist die Ausgabe bitgenau gleich der single-threaded Verarbeitung (NFR-5).
// - Volle Queues: Producer bzw. Engine-Thread warten aktiv (yield), nichts wird verworfen.
//
// Lebenszyklus: start() -> beliebig viele submit() aus beliebigen Threads -> stop().
// stop() darf erst aufgerufen werden, wenn alle submit()-Aufrufe zurückgekehrt sind;
// es wartet, bis alle Requests verarbeitet und alle Events an output geliefert sind,
// und joint dann beide Threads. Der Destruktor ruft stop(), falls nötig.
// submit() vor start() blockiert, sobald die RequestQueue voll ist.
class Pipeline {
public:
    explicit Pipeline(EventSink& output);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void start();
    void submit(const Request& request);  // thread-sicher
    void stop();

private:
    // Der private Teil darf bei der Implementierung frei angepasst werden.
    void engine_loop();
    void logger_loop();

    EventSink& output_;
    std::unique_ptr<RequestQueue> requests_;
    std::unique_ptr<EventQueue> events_;
    std::thread engine_thread_;
    std::thread logger_thread_;
    // Shutdown-Protokoll (siehe pipeline.cpp): stop() -> stop_requested_ (release)
    // -> Engine leert requests_ -> engine_done_ (release) -> Logger leert events_.
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> engine_done_{false};
    bool running_{false};  // nur vom steuernden Thread (start/stop/Destruktor) benutzt
};

}  // namespace engine
