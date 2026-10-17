// SpscQueue testen.
// Portiert aus ~/dev/tst/ringbuffer_spsc.hpp (testRingBufferSpsc) und ergänzt.

#include "engine/spsc_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#include <gtest/gtest.h>

#include "engine/event.hpp"
#include "engine/types.hpp"

namespace {

using engine::SpscQueue;

// ---------------------------------------------------------------------------
// Single-threaded
// ---------------------------------------------------------------------------

TEST(SpscQueue, CapacityIsN) {
    EXPECT_EQ((SpscQueue<int, 1>::capacity()), 1u);
    EXPECT_EQ((SpscQueue<int, 5>::capacity()), 5u);  // keine Zweierpotenz nötig
    EXPECT_EQ((SpscQueue<int, 1024>::capacity()), 1024u);
    static_assert(SpscQueue<int, 3>::capacity() == 3);
}

TEST(SpscQueue, NewQueueIsEmpty) {
    SpscQueue<int, 4> q;
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.full());
    EXPECT_FALSE(q.pop().has_value());
}

// Original-Test: SpscQueue<int, 5> füllen, voll, leeren, leer.
TEST(SpscQueue, FillToCapacityThenDrainInOrder) {
    SpscQueue<int, 5> q;
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(q.full()) << i;
        ASSERT_TRUE(q.push(i));
        EXPECT_FALSE(q.empty()) << i;
    }
    EXPECT_TRUE(q.full());
    EXPECT_FALSE(q.push(5));
    EXPECT_TRUE(q.full());  // fehlgeschlagener Push ändert nichts

    for (int i = 0; i < 5; ++i) {
        auto v = q.pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, i);
        EXPECT_FALSE(q.full());
    }
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.pop().has_value());
}

TEST(SpscQueue, PopAfterFullFreesExactlyOneSlot) {
    SpscQueue<int, 3> q;
    ASSERT_TRUE(q.push(1));
    ASSERT_TRUE(q.push(2));
    ASSERT_TRUE(q.push(3));
    ASSERT_FALSE(q.push(4));
    EXPECT_EQ(q.pop(), std::optional<int>(1));
    EXPECT_TRUE(q.push(4));
    EXPECT_FALSE(q.push(5));
    EXPECT_EQ(q.pop(), std::optional<int>(2));
    EXPECT_EQ(q.pop(), std::optional<int>(3));
    EXPECT_EQ(q.pop(), std::optional<int>(4));
    EXPECT_FALSE(q.pop().has_value());
}

// Mehrere Runden über die volle Kapazität (Index-Wrap bei N + 1 Slots).
TEST(SpscQueue, SeveralLapsFillAndDrain) {
    SpscQueue<int, 5> q;
    for (int lap = 0; lap < 4; ++lap) {
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(q.push(lap * 10 + i)) << "lap " << lap;
        }
        EXPECT_TRUE(q.full()) << "lap " << lap;
        EXPECT_FALSE(q.push(99)) << "lap " << lap;
        for (int i = 0; i < 5; ++i) {
            EXPECT_EQ(q.pop(), std::optional<int>(lap * 10 + i)) << "lap " << lap;
        }
        EXPECT_TRUE(q.empty()) << "lap " << lap;
        EXPECT_FALSE(q.pop().has_value()) << "lap " << lap;
    }
}

TEST(SpscQueue, CapacityOne) {
    SpscQueue<int, 1> q;
    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(q.empty());
        ASSERT_TRUE(q.push(i));
        EXPECT_TRUE(q.full());
        EXPECT_FALSE(q.push(-1));
        EXPECT_EQ(q.pop(), std::optional<int>(i));
        EXPECT_FALSE(q.pop().has_value());
    }
}

TEST(SpscQueue, InterleavedPushPopWrapsManyTimes) {
    SpscQueue<int, 5> q;
    int next_push = 0;
    int next_pop = 0;
    for (int round = 0; round < 1000; ++round) {
        const int pushes = 1 + round % 4;  // 1..4
        for (int i = 0; i < pushes && !q.full(); ++i) {
            ASSERT_TRUE(q.push(next_push++));
        }
        const int pops = (round % 3 == 0) ? 1 : pushes;
        for (int i = 0; i < pops && next_pop < next_push; ++i) {
            auto v = q.pop();
            ASSERT_TRUE(v.has_value());
            ASSERT_EQ(*v, next_pop++);
        }
        ASSERT_EQ(q.empty(), next_pop == next_push);
        ASSERT_EQ(q.full(), next_push - next_pop == 5);
    }
    while (next_pop < next_push) {
        ASSERT_EQ(q.pop(), std::optional<int>(next_pop++));
    }
    EXPECT_TRUE(q.empty());
    EXPECT_GT(next_push, 6 * 100);
}

// Original-Test: push, push, clear -> empty.
TEST(SpscQueue, ClearDropsAllElements) {
    SpscQueue<int, 5> q;
    q.push(1);
    q.push(2);
    q.clear();
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.full());
    EXPECT_FALSE(q.pop().has_value());
}

TEST(SpscQueue, ClearOnFullQueueAndReuse) {
    SpscQueue<int, 3> q;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(q.push(i));
    }
    ASSERT_TRUE(q.full());
    q.clear();
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.full());

    // Danach wieder volle Kapazität nutzbar, FIFO ab den neuen Werten.
    for (int i = 10; i < 13; ++i) {
        ASSERT_TRUE(q.push(i));
    }
    EXPECT_FALSE(q.push(13));
    for (int i = 10; i < 13; ++i) {
        EXPECT_EQ(q.pop(), std::optional<int>(i));
    }
    EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, ClearOnEmptyQueueIsNoOp) {
    SpscQueue<int, 2> q;
    q.clear();
    EXPECT_TRUE(q.empty());
    ASSERT_TRUE(q.push(5));
    EXPECT_EQ(q.pop(), std::optional<int>(5));
    q.clear();
    EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, NonTrivialTypeStdString) {
    SpscQueue<std::string, 3> q;
    const auto make = [](int i) { return std::string(64, static_cast<char>('a' + i % 26)) + std::to_string(i); };
    for (int lap = 0; lap < 5; ++lap) {
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(q.push(make(lap * 3 + i)));
        }
        EXPECT_FALSE(q.push("overflow"));
        for (int i = 0; i < 3; ++i) {
            EXPECT_EQ(q.pop(), std::optional<std::string>(make(lap * 3 + i)));
        }
        EXPECT_FALSE(q.pop().has_value());
    }
}

TEST(SpscQueue, EventVariantRoundTrip) {
    SpscQueue<engine::Event, 4> q;
    ASSERT_TRUE(q.push(engine::AckEvent{11, 1}));
    ASSERT_TRUE(q.push(engine::FillEvent{11, 3, 7}));
    ASSERT_TRUE(q.push(engine::RejectEvent{12, engine::RejectReason{}}));

    auto e = q.pop();
    ASSERT_TRUE(e.has_value());
    const auto* ack = std::get_if<engine::AckEvent>(&*e);
    ASSERT_NE(ack, nullptr);
    EXPECT_EQ(ack->id, 11u);
    EXPECT_EQ(ack->seq, 1u);

    e = q.pop();
    ASSERT_TRUE(e.has_value());
    const auto* fill = std::get_if<engine::FillEvent>(&*e);
    ASSERT_NE(fill, nullptr);
    EXPECT_EQ(fill->id, 11u);
    EXPECT_EQ(fill->filled, 3u);
    EXPECT_EQ(fill->remaining, 7u);

    e = q.pop();
    ASSERT_TRUE(e.has_value());
    const auto* rej = std::get_if<engine::RejectEvent>(&*e);
    ASSERT_NE(rej, nullptr);
    EXPECT_EQ(rej->id, 12u);
    EXPECT_FALSE(q.pop().has_value());
}

// ---------------------------------------------------------------------------
// Multi-threaded: 1 Producer, 1 Consumer
// ---------------------------------------------------------------------------

// Original-Test (verkleinert für TSan): alle Werte kommen in Reihenfolge an.
template <std::size_t N>
void run_spsc_in_order(int count) {
    auto q = std::make_unique<SpscQueue<int, N>>();

    std::thread producer([&] {
        for (int i = 0; i < count; ++i) {
            while (!q->push(i)) {
                std::this_thread::yield();  // voll
            }
        }
    });

    int errors = 0;
    int expected = 0;
    while (expected < count) {
        if (auto v = q->pop()) {
            if (*v != expected && ++errors <= 10) {
                ADD_FAILURE() << "expected " << expected << ", got " << *v;
            }
            ++expected;
        } else {
            std::this_thread::yield();  // leer
        }
    }
    producer.join();

    EXPECT_EQ(errors, 0);
    EXPECT_TRUE(q->empty());
    EXPECT_FALSE(q->pop().has_value());
}

TEST(SpscQueue, ProducerConsumerThreadsAllValuesInOrder) { run_spsc_in_order<1024>(100'000); }

TEST(SpscQueue, ProducerConsumerThreadsCapacityOne) { run_spsc_in_order<1>(20'000); }

TEST(SpscQueue, ProducerConsumerThreadsSmallOddCapacity) { run_spsc_in_order<7>(50'000); }

// Mehrfeldiger, kopierbarer Typ: alle Felder aus der laufenden Nummer abgeleitet,
// damit zerrissene Werte (halb alter, halb neuer Inhalt) auffallen.
struct Payload {
    std::uint64_t seq;
    std::uint64_t square;
    std::int64_t negated;
    std::uint32_t low;
    std::uint8_t tag;
    std::string text;  // nicht trivial kopierbar
};

Payload make_payload(std::uint64_t i) {
    return Payload{i,
                   i * i,
                   -static_cast<std::int64_t>(i),
                   static_cast<std::uint32_t>(i & 0xFFFF'FFFFu),
                   static_cast<std::uint8_t>(i % 251),
                   "payload-number-" + std::to_string(i) + std::string(32, 'x')};
}

bool same_payload(const Payload& a, const Payload& b) {
    return a.seq == b.seq && a.square == b.square && a.negated == b.negated && a.low == b.low && a.tag == b.tag &&
           a.text == b.text;
}

TEST(SpscQueue, ProducerConsumerThreadsNonTrivialType) {
    constexpr std::uint64_t kCount = 50'000;
    auto q = std::make_unique<SpscQueue<Payload, 64>>();

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            const Payload p = make_payload(i);
            while (!q->push(p)) {
                std::this_thread::yield();
            }
        }
    });

    int errors = 0;
    for (std::uint64_t expected = 0; expected < kCount;) {
        if (auto v = q->pop()) {
            if (!same_payload(*v, make_payload(expected)) && ++errors <= 10) {
                ADD_FAILURE() << "mismatch at #" << expected << " (got seq " << v->seq << ")";
            }
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();

    EXPECT_EQ(errors, 0);
    EXPECT_TRUE(q->empty());
}

// Engine -> Logger: Events als Variant durch die Queue.
TEST(SpscQueue, ProducerConsumerThreadsEventVariant) {
    constexpr std::uint64_t kCount = 50'000;
    auto q = std::make_unique<SpscQueue<engine::Event, 256>>();

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            const engine::Event e = (i % 2 == 0) ? engine::Event{engine::AckEvent{i, i + 1}}
                                                 : engine::Event{engine::FillEvent{i, i * 2, i * 3}};
            while (!q->push(e)) {
                std::this_thread::yield();
            }
        }
    });

    int errors = 0;
    for (std::uint64_t expected = 0; expected < kCount;) {
        auto e = q->pop();
        if (!e) {
            std::this_thread::yield();
            continue;
        }
        bool ok = false;
        if (expected % 2 == 0) {
            const auto* ack = std::get_if<engine::AckEvent>(&*e);
            ok = ack != nullptr && ack->id == expected && ack->seq == expected + 1;
        } else {
            const auto* fill = std::get_if<engine::FillEvent>(&*e);
            ok = fill != nullptr && fill->id == expected && fill->filled == expected * 2 &&
                 fill->remaining == expected * 3;
        }
        if (!ok && ++errors <= 10) {
            ADD_FAILURE() << "mismatch at #" << expected;
        }
        ++expected;
    }
    producer.join();

    EXPECT_EQ(errors, 0);
    EXPECT_TRUE(q->empty());
}

}  // namespace
