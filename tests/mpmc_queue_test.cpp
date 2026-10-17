// MpmcQueue (Vyukov bounded MPMC) testen.
// Portiert aus ~/dev/tst/ringbuffer_mpmc.hpp (testRingBufferMpmc) und ergänzt.

#include "engine/mpmc_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "engine/event.hpp"
#include "engine/types.hpp"

namespace {

using engine::MpmcQueue;

// ---------------------------------------------------------------------------
// Single-threaded
// ---------------------------------------------------------------------------

TEST(MpmcQueue, CapacityIsN) {
    EXPECT_EQ((MpmcQueue<int, 2>::capacity()), 2u);
    EXPECT_EQ((MpmcQueue<int, 4>::capacity()), 4u);
    EXPECT_EQ((MpmcQueue<int, 1024>::capacity()), 1024u);
    static_assert(MpmcQueue<int, 8>::capacity() == 8);  // constexpr nutzbar
}

TEST(MpmcQueue, EmptyQueuePopReturnsNullopt) {
    MpmcQueue<int, 4> q;
    EXPECT_FALSE(q.pop().has_value());
    EXPECT_FALSE(q.pop().has_value());  // wiederholt, ohne Zustand zu verändern
    ASSERT_TRUE(q.push(7));
    EXPECT_EQ(q.pop(), std::optional<int>(7));
}

TEST(MpmcQueue, FifoOrder) {
    MpmcQueue<int, 8> q;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(q.push(i * 3));
    }
    for (int i = 0; i < 5; ++i) {
        auto v = q.pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, i * 3);
    }
    EXPECT_FALSE(q.pop().has_value());
}

TEST(MpmcQueue, FullQueueRejectsPush) {
    MpmcQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.push(i));
    }
    EXPECT_FALSE(q.push(99));
    EXPECT_FALSE(q.push(100));  // bleibt voll, nichts überschrieben

    // Ein Pop macht genau einen Slot frei.
    EXPECT_EQ(q.pop(), std::optional<int>(0));
    EXPECT_TRUE(q.push(4));
    EXPECT_FALSE(q.push(5));

    for (int i = 1; i <= 4; ++i) {
        EXPECT_EQ(q.pop(), std::optional<int>(i));
    }
    EXPECT_FALSE(q.pop().has_value());
}

// Original-Test: mehrere Runden über die volle Kapazität (seq = pos + N).
TEST(MpmcQueue, SeveralLapsFillAndDrain) {
    MpmcQueue<int, 4> q;
    for (int lap = 0; lap < 3; ++lap) {
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(q.push(lap * 10 + i)) << "lap " << lap << " i " << i;
        }
        EXPECT_FALSE(q.push(99)) << "lap " << lap;
        for (int i = 0; i < 4; ++i) {
            auto v = q.pop();
            ASSERT_TRUE(v.has_value()) << "lap " << lap << " i " << i;
            EXPECT_EQ(*v, lap * 10 + i);
        }
        EXPECT_FALSE(q.pop().has_value()) << "lap " << lap;
    }
}

TEST(MpmcQueue, MinimalCapacityTwo) {
    MpmcQueue<int, 2> q;
    for (int lap = 0; lap < 100; ++lap) {
        ASSERT_TRUE(q.push(2 * lap));
        ASSERT_TRUE(q.push(2 * lap + 1));
        ASSERT_FALSE(q.push(-1));
        ASSERT_EQ(q.pop(), std::optional<int>(2 * lap));
        ASSERT_EQ(q.pop(), std::optional<int>(2 * lap + 1));
        ASSERT_FALSE(q.pop().has_value());
    }
}

// Füllstand pendelt, Lese-/Schreibposition stehen an wechselnden Slots: viele Wraps.
TEST(MpmcQueue, InterleavedPushPopWrapsManyTimes) {
    MpmcQueue<int, 4> q;
    int next_push = 0;
    int next_pop = 0;
    for (int round = 0; round < 1000; ++round) {
        const int pushes = 1 + round % 3;  // 1..3
        for (int i = 0; i < pushes; ++i) {
            ASSERT_TRUE(q.push(next_push++));
        }
        const int pops = (round % 2 == 0) ? pushes - 1 : pushes + 1;
        for (int i = 0; i < pops && next_pop < next_push; ++i) {
            auto v = q.pop();
            ASSERT_TRUE(v.has_value());
            ASSERT_EQ(*v, next_pop++);
        }
    }
    while (next_pop < next_push) {
        auto v = q.pop();
        ASSERT_TRUE(v.has_value());
        ASSERT_EQ(*v, next_pop++);
    }
    EXPECT_FALSE(q.pop().has_value());
    EXPECT_GT(next_push, 4 * 100);  // deutlich mehr als 100 Runden über N
}

TEST(MpmcQueue, NonTrivialTypeStdString) {
    MpmcQueue<std::string, 4> q;
    // Lang genug, um die Small-String-Optimierung zu umgehen (echte Heap-Allokation).
    const auto make = [](int i) { return std::string(64, static_cast<char>('a' + i % 26)) + std::to_string(i); };
    for (int lap = 0; lap < 5; ++lap) {
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(q.push(make(lap * 4 + i)));
        }
        EXPECT_FALSE(q.push("overflow"));
        for (int i = 0; i < 4; ++i) {
            EXPECT_EQ(q.pop(), std::optional<std::string>(make(lap * 4 + i)));
        }
        EXPECT_FALSE(q.pop().has_value());
    }
}

TEST(MpmcQueue, RequestVariantRoundTrip) {
    MpmcQueue<engine::Request, 8> q;
    const engine::NewOrder order{42, 3, engine::Side::Sell, engine::OrderType::Limit, 10'050, 17};
    ASSERT_TRUE(q.push(engine::Command{order}));
    ASSERT_TRUE(q.push(engine::Command{engine::CancelOrder{42}}));
    ASSERT_TRUE(q.push(engine::Command{engine::ModifyOrder{43, 9'900, 5}}));
    ASSERT_TRUE(q.push(engine::AddSymbolRequest{7}));
    ASSERT_TRUE(q.push(engine::BookRequest{7, 3}));

    {
        auto r = q.pop();
        ASSERT_TRUE(r.has_value());
        const auto* cmd = std::get_if<engine::Command>(&*r);
        ASSERT_NE(cmd, nullptr);
        const auto* got = std::get_if<engine::NewOrder>(cmd);
        ASSERT_NE(got, nullptr);
        EXPECT_EQ(got->id, 42u);
        EXPECT_EQ(got->symbol, 3u);
        EXPECT_EQ(got->side, engine::Side::Sell);
        EXPECT_EQ(got->type, engine::OrderType::Limit);
        EXPECT_EQ(got->price, 10'050);
        EXPECT_EQ(got->quantity, 17u);
    }
    {
        auto r = q.pop();
        ASSERT_TRUE(r.has_value());
        const auto* cmd = std::get_if<engine::Command>(&*r);
        ASSERT_NE(cmd, nullptr);
        const auto* got = std::get_if<engine::CancelOrder>(cmd);
        ASSERT_NE(got, nullptr);
        EXPECT_EQ(got->id, 42u);
    }
    {
        auto r = q.pop();
        ASSERT_TRUE(r.has_value());
        const auto* cmd = std::get_if<engine::Command>(&*r);
        ASSERT_NE(cmd, nullptr);
        const auto* got = std::get_if<engine::ModifyOrder>(cmd);
        ASSERT_NE(got, nullptr);
        EXPECT_EQ(got->id, 43u);
        EXPECT_EQ(got->new_price, 9'900);
        EXPECT_EQ(got->new_quantity, 5u);
    }
    {
        auto r = q.pop();
        ASSERT_TRUE(r.has_value());
        const auto* got = std::get_if<engine::AddSymbolRequest>(&*r);
        ASSERT_NE(got, nullptr);
        EXPECT_EQ(got->symbol, 7u);
    }
    {
        auto r = q.pop();
        ASSERT_TRUE(r.has_value());
        const auto* got = std::get_if<engine::BookRequest>(&*r);
        ASSERT_NE(got, nullptr);
        EXPECT_EQ(got->symbol, 7u);
        EXPECT_EQ(got->depth, 3u);
    }
    EXPECT_FALSE(q.pop().has_value());
}

// ---------------------------------------------------------------------------
// Multi-threaded
// ---------------------------------------------------------------------------

// Original-Test (verkleinert für TSan): 4 Producer / 4 Consumer, jeder Wert genau einmal.
template <std::size_t N>
void run_mpmc_exactly_once(int producers, int consumers, int per_producer) {
    const int total = producers * per_producer;
    auto q = std::make_unique<MpmcQueue<int, N>>();
    std::vector<std::atomic<int>> seen(static_cast<std::size_t>(total));  // wie oft gepoppt
    std::atomic<int> consumed{0};
    std::atomic<int> out_of_range{0};

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(producers + consumers));
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            for (int i = 0; i < per_producer; ++i) {
                while (!q->push(p * per_producer + i)) {
                    std::this_thread::yield();  // voll
                }
            }
        });
    }
    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&] {
            while (consumed.load(std::memory_order_relaxed) < total) {
                if (auto v = q->pop()) {
                    if (*v < 0 || *v >= total) {
                        out_of_range.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        seen[static_cast<std::size_t>(*v)].fetch_add(1, std::memory_order_relaxed);
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();  // leer
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(out_of_range.load(), 0);
    EXPECT_EQ(consumed.load(), total);
    int wrong = 0;
    for (int v = 0; v < total; ++v) {
        const int n = seen[static_cast<std::size_t>(v)].load();
        if (n != 1) {
            if (++wrong <= 10) {
                ADD_FAILURE() << "value " << v << " popped " << n << " times";
            }
        }
    }
    EXPECT_EQ(wrong, 0);
    EXPECT_FALSE(q->pop().has_value());
}

TEST(MpmcQueue, FourProducersFourConsumersEachValueExactlyOnce) { run_mpmc_exactly_once<1024>(4, 4, 25'000); }

// Kleine Queue: ständig voll/leer, Producer und Consumer kämpfen um dieselben Slots.
TEST(MpmcQueue, FourProducersFourConsumersSmallQueueHighContention) { run_mpmc_exactly_once<4>(4, 4, 10'000); }

// Pipeline-Nutzung: N Producer, 1 Consumer. Wert = (Producer-ID << 32) | laufende Nummer.
// Pro Producer muss die Reihenfolge erhalten bleiben.
template <std::size_t N>
void run_mpsc_per_producer_order(int producers, std::uint32_t per_producer) {
    auto q = std::make_unique<MpmcQueue<std::uint64_t, N>>();
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(producers));
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            const std::uint64_t id = static_cast<std::uint64_t>(p) << 32;
            for (std::uint32_t i = 0; i < per_producer; ++i) {
                while (!q->push(id | i)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Consumer läuft im Test-Thread.
    std::vector<std::uint32_t> next(static_cast<std::size_t>(producers), 0);
    const std::uint64_t total = static_cast<std::uint64_t>(producers) * per_producer;
    int errors = 0;
    for (std::uint64_t received = 0; received < total;) {
        auto v = q->pop();
        if (!v) {
            std::this_thread::yield();
            continue;
        }
        ++received;
        const auto p = static_cast<std::size_t>(*v >> 32);
        const auto seq = static_cast<std::uint32_t>(*v & 0xFFFF'FFFFu);
        if (p >= next.size()) {
            if (++errors <= 10) {
                ADD_FAILURE() << "unknown producer " << p;
            }
            continue;
        }
        if (seq != next[p]) {
            if (++errors <= 10) {
                ADD_FAILURE() << "producer " << p << ": expected " << next[p] << ", got " << seq;
            }
        }
        next[p] = seq + 1;
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(errors, 0);
    for (std::size_t p = 0; p < next.size(); ++p) {
        EXPECT_EQ(next[p], per_producer) << "producer " << p;
    }
    EXPECT_FALSE(q->pop().has_value());
}

TEST(MpmcQueue, MultiProducerSingleConsumerKeepsPerProducerOrder) { run_mpsc_per_producer_order<1024>(4, 25'000); }

TEST(MpmcQueue, MultiProducerSingleConsumerSmallQueueKeepsPerProducerOrder) {
    run_mpsc_per_producer_order<8>(6, 10'000);
}

// Nicht-trivialer, kopierbarer Typ (engine::Request) im MPSC-Betrieb.
// Alle Felder sind aus (Producer, Nummer) abgeleitet; so fallen zerrissene oder
// vermischte Werte auf, und die Reihenfolge pro Producer wird geprüft.
// Request hat kein operator== — feldweiser Vergleich für die Tests.
bool same_fields(const engine::NewOrder& a, const engine::NewOrder& b) {
    return a.id == b.id && a.symbol == b.symbol && a.side == b.side && a.type == b.type && a.price == b.price &&
           a.quantity == b.quantity;
}
bool same_fields(const engine::CancelOrder& a, const engine::CancelOrder& b) { return a.id == b.id; }
bool same_fields(const engine::ModifyOrder& a, const engine::ModifyOrder& b) {
    return a.id == b.id && a.new_price == b.new_price && a.new_quantity == b.new_quantity;
}
bool same_fields(const engine::AddSymbolRequest& a, const engine::AddSymbolRequest& b) { return a.symbol == b.symbol; }
bool same_fields(const engine::BookRequest& a, const engine::BookRequest& b) {
    return a.symbol == b.symbol && a.depth == b.depth;
}
bool same_fields(const engine::Command& a, const engine::Command& b) {
    if (a.index() != b.index()) {
        return false;
    }
    return std::visit([&](const auto& x) { return same_fields(x, std::get<std::remove_cvref_t<decltype(x)>>(b)); }, a);
}
bool same_request(const engine::Request& a, const engine::Request& b) {
    if (a.index() != b.index()) {
        return false;
    }
    return std::visit([&](const auto& x) { return same_fields(x, std::get<std::remove_cvref_t<decltype(x)>>(b)); }, a);
}

engine::Request make_request(std::uint32_t producer, std::uint32_t i) {
    const engine::OrderId id = (static_cast<std::uint64_t>(producer) << 32) | i;
    switch (i % 4) {
        case 0:
            return engine::Command{engine::NewOrder{id, producer, (i % 8 == 0) ? engine::Side::Buy : engine::Side::Sell,
                                                    engine::OrderType::Limit, static_cast<engine::Price>(i) * 3 + 1,
                                                    static_cast<engine::Quantity>(i) + 100}};
        case 1: return engine::Command{engine::ModifyOrder{id, static_cast<engine::Price>(i) * 7, i + 5ull}};
        case 2: return engine::Command{engine::CancelOrder{id}};
        default: return engine::BookRequest{producer, i};
    }
}

TEST(MpmcQueue, RequestMultiProducerSingleConsumer) {
    constexpr std::uint32_t kProducers = 4;
    constexpr std::uint32_t kPerProducer = 20'000;
    auto q = std::make_unique<MpmcQueue<engine::Request, 256>>();

    std::vector<std::thread> threads;
    threads.reserve(kProducers);
    for (std::uint32_t p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            for (std::uint32_t i = 0; i < kPerProducer; ++i) {
                const engine::Request r = make_request(p, i);
                while (!q->push(r)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::uint32_t> next(kProducers, 0);
    int errors = 0;
    for (std::uint32_t received = 0; received < kProducers * kPerProducer;) {
        auto r = q->pop();
        if (!r) {
            std::this_thread::yield();
            continue;
        }
        ++received;
        // Producer aus der Nachricht ermitteln (BookRequest trägt ihn im symbol-Feld).
        std::uint32_t p = 0;
        if (const auto* book = std::get_if<engine::BookRequest>(&*r)) {
            p = book->symbol;
        } else if (const auto* cmd = std::get_if<engine::Command>(&*r)) {
            const engine::OrderId id = std::visit([](const auto& c) { return c.id; }, *cmd);
            p = static_cast<std::uint32_t>(id >> 32);
        } else {
            if (++errors <= 10) {
                ADD_FAILURE() << "unexpected alternative " << r->index();
            }
            continue;
        }
        if (p >= kProducers) {
            if (++errors <= 10) {
                ADD_FAILURE() << "unknown producer " << p;
            }
            continue;
        }
        const engine::Request expected = make_request(p, next[p]);
        if (!same_request(*r, expected)) {
            if (++errors <= 10) {
                ADD_FAILURE() << "producer " << p << ": mismatch at #" << next[p];
            }
        }
        ++next[p];
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(errors, 0);
    for (std::uint32_t p = 0; p < kProducers; ++p) {
        EXPECT_EQ(next[p], kPerProducer) << "producer " << p;
    }
    EXPECT_FALSE(q->pop().has_value());
}

}  // namespace
