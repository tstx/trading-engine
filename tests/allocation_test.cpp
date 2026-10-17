// NFR-6: Der heiße Pfad (process() mit NEW/CANCEL/MODIFY) allokiert innerhalb der
// EngineConfig-Kapazitäten nicht; darüber wächst die Engine statt zu rejecten.
//
// Messprinzip: Dieses Executable ersetzt den globalen operator new/delete (alle Varianten:
// skalar/Array, aligned, nothrow) durch malloc/free-Wrapper mit Zähler. Gezählt wird nur,
// solange auf dem AKTUELLEN Thread ein AllocationScope lebt (thread_local Flag); gtest,
// Warmup und Testaufbau bleiben dadurch außen vor.
//
// Sanitizer: ASan/TSan ersetzen malloc/free und stellen eigene operator new bereit. Eine
// Ersetzung im Executable hat Vorrang (reguläre Ersetzung nach [replacement.functions]); da
// unsere Varianten auf malloc/free abbilden, prüft der Sanitizer weiterhin jeden Zugriff
// (nur die new/delete-vs.-malloc/free-Mismatch-Diagnose entfällt). Der Test läuft deshalb in
// allen Presets. Eigenes Executable (siehe CMakeLists.txt), damit die Ersetzung die übrigen
// Tests nicht berührt.
//
// Bewusst nicht gezählt: direkte malloc-Aufrufe. Die Engine allokiert ausschließlich über
// operator new (std::allocator, ObjectPool, FlatHashMap).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "engine/engine_config.hpp"
#include "engine/event_sink.hpp"
#include "engine/matching_engine.hpp"
#include "engine/parser.hpp"
#include "engine/types.hpp"

// ---------------------------------------------------------------------------------------------
// Zählender globaler operator new/delete
// ---------------------------------------------------------------------------------------------

namespace {

// Konstant initialisiert: Zugriff ist auch aus operator new heraus (z. B. während der
// statischen Initialisierung anderer Übersetzungseinheiten) sicher.
thread_local bool t_counting = false;
thread_local std::size_t t_allocations = 0;
thread_local std::size_t t_bytes = 0;

void note_allocation(std::size_t size) noexcept {
    if (t_counting) {
        ++t_allocations;
        t_bytes += size;
    }
}

void* allocate(std::size_t size) noexcept {
    note_allocation(size);
    return std::malloc(size == 0 ? 1 : size);
}

void* allocate_aligned(std::size_t size, std::align_val_t al) noexcept {
    note_allocation(size);
    const auto alignment = static_cast<std::size_t>(al);
    // aligned_alloc verlangt ein Vielfaches der Ausrichtung als Größe.
    const std::size_t rounded = size == 0 ? alignment : (size + alignment - 1) / alignment * alignment;
    return std::aligned_alloc(alignment, rounded);
}

void* allocate_or_throw(std::size_t size) {
    void* p = allocate(size);
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    return p;
}

void* allocate_aligned_or_throw(std::size_t size, std::align_val_t al) {
    void* p = allocate_aligned(size, al);
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    return p;
}

}  // namespace

void* operator new(std::size_t size) { return allocate_or_throw(size); }
void* operator new[](std::size_t size) { return allocate_or_throw(size); }
void* operator new(std::size_t size, std::align_val_t al) { return allocate_aligned_or_throw(size, al); }
void* operator new[](std::size_t size, std::align_val_t al) { return allocate_aligned_or_throw(size, al); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void* operator new(std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
    return allocate_aligned(size, al);
}
void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
    return allocate_aligned(size, al);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

using namespace engine;

// Zählt Allokationen dieses Threads zwischen Konstruktion und stop() bzw. Destruktion.
// Innerhalb des Scopes keine gtest-Makros verwenden (die allokieren ggf. selbst).
class AllocationScope {
public:
    AllocationScope() noexcept {
        t_allocations = 0;
        t_bytes = 0;
        t_counting = true;
    }
    ~AllocationScope() { stop(); }
    AllocationScope(const AllocationScope&) = delete;
    AllocationScope& operator=(const AllocationScope&) = delete;

    void stop() noexcept {
        if (t_counting) {
            t_counting = false;
            allocations_ = t_allocations;
            bytes_ = t_bytes;
        }
    }
    [[nodiscard]] std::size_t allocations() noexcept {
        stop();
        return allocations_;
    }
    [[nodiscard]] std::size_t bytes() noexcept {
        stop();
        return bytes_;
    }

private:
    std::size_t allocations_{0};
    std::size_t bytes_{0};
};

// Sink, die selbst nicht allokiert: nur Zähler.
class CountingSink final : public EventSink {
public:
    void on_ack(OrderId /*id*/, SeqNo /*seq*/) override { ++acks; }
    void on_reject(OrderId /*id*/, RejectReason reason) override {
        ++rejects;
        ++reject_by_reason.at(static_cast<std::size_t>(reason));
    }
    void on_trade(const Trade& trade) override {
        ++trades;
        traded_quantity += trade.quantity;
    }
    void on_fill(OrderId /*id*/, Quantity /*filled*/, Quantity /*remaining*/) override { ++fills; }
    void on_cancelled(OrderId /*id*/, Quantity /*remaining*/, CancelReason reason) override {
        if (reason == CancelReason::User) {
            ++user_cancels;
        } else {
            ++no_liquidity;
        }
    }

    [[nodiscard]] std::size_t reject_count(RejectReason reason) const {
        return reject_by_reason.at(static_cast<std::size_t>(reason));
    }

    std::size_t acks = 0;
    std::size_t rejects = 0;
    std::array<std::size_t, 6> reject_by_reason{};
    std::size_t trades = 0;
    Quantity traded_quantity = 0;
    std::size_t fills = 0;
    std::size_t user_cancels = 0;
    std::size_t no_liquidity = 0;
};

// Deterministischer Zufall (kein <random>, damit die Sequenz plattformunabhängig ist).
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) : state_(seed) {}
    std::uint64_t below(std::uint64_t n) {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return (state_ >> 33) % n;
    }

private:
    std::uint64_t state_;
};

constexpr SymbolId kDense = 0;
constexpr SymbolId kSparse = 100'000;  // >= kDenseSymbolLimit: FlatHashMap-Pfad
constexpr Price kMid = 1'000;

// Realistischer Mix auf zwei Symbolen. Erzeugt die Commands VOR dem Messscope (der Vektor
// selbst allokiert). Enthält:
// - NEW LIMIT um kMid herum (Bids/Asks überlappen -> Trades, teils über mehrere Level),
// - NEW MARKET (Sweeps über mehrere Level, Rest -> NO_LIQUIDITY),
// - CANCEL und MODIFY (Reduktion bei gleichem Preis und Preisänderung/Erhöhung) auf jüngere IDs,
// - Rejects: DUPLICATE_ID, UNKNOWN_SYMBOL, INVALID_QUANTITY, INVALID_PRICE, UNKNOWN_ORDER,
//   INVALID_MODIFY.
class MixGenerator {
public:
    MixGenerator(std::uint64_t seed, OrderId first_id, Price max_price)
        : rng_(seed), next_id_(first_id), first_id_(first_id), max_price_(max_price) {}

    std::vector<Command> generate(std::size_t count) {
        std::vector<Command> out;
        out.reserve(count);
        while (out.size() < count) {
            out.push_back(next());
        }
        return out;
    }

private:
    struct Issued {
        Price price;
        Quantity quantity;
    };

    SymbolId symbol() { return rng_.below(2) == 0 ? kDense : kSparse; }
    Side side() { return rng_.below(2) == 0 ? Side::Buy : Side::Sell; }

    // Bids leicht unter, Asks leicht über kMid: meist liegend, oft kreuzend.
    Price limit_price(Side s) {
        const auto offset = static_cast<Price>(rng_.below(25));
        return s == Side::Buy ? kMid - 14 + offset : kMid - 10 + offset;
    }

    Quantity quantity() {
        // Gelegentlich große Mengen, die mehrere Level räumen.
        return rng_.below(10) == 0 ? 100 + rng_.below(400) : 1 + rng_.below(60);
    }

    // Eine kürzlich vergebene ID (Cancel/Modify zielen meist auf noch liegende Orders).
    OrderId recent_id() {
        const OrderId issued = next_id_ - first_id_;
        return next_id_ - 1 - rng_.below(issued < 64 ? issued : 64);
    }

    Command new_limit() {
        const Side s = side();
        const Price p = limit_price(s);
        const Quantity q = quantity();
        issued_.push_back(Issued{p, q});
        return NewOrder{next_id_++, symbol(), s, OrderType::Limit, p, q};
    }

    Command next() {
        if (issued_.empty()) {
            return new_limit();  // recent_id() braucht mindestens eine vergebene ID
        }
        const auto r = rng_.below(100);
        if (r < 45) {
            return new_limit();
        }
        if (r < 52) {
            issued_.push_back(Issued{0, 0});
            return NewOrder{next_id_++, symbol(), side(), OrderType::Market, 0, 50 + rng_.below(250)};
        }
        if (r < 66) {
            return CancelOrder{recent_id()};
        }
        if (r < 74) {
            // Reduktion bei gleichem Preis (Position bleibt) - sofern die Order noch dort liegt.
            const OrderId id = recent_id();
            const Issued& orig = issued_.at(id - first_id_);
            return ModifyOrder{id, orig.price == 0 ? kMid : orig.price, orig.quantity / 2 + 1};
        }
        if (r < 82) {
            // Preisänderung bzw. Erhöhung: Prioritätsverlust, ggf. Matching als Aggressor.
            const OrderId id = recent_id();
            return ModifyOrder{id, limit_price(side()), 1 + rng_.below(150)};
        }
        switch (rng_.below(8)) {
            case 0: return NewOrder{recent_id(), symbol(), side(), OrderType::Limit, kMid, 1};  // DUPLICATE_ID
            case 1: {
                const std::array<SymbolId, 4> unknown{kUnknownSymbol, 1, SymbolId{1} << 31, kSparse + 1};
                const SymbolId sym = unknown.at(rng_.below(unknown.size()));
                return NewOrder{next_id_ + 1'000'000'000, sym, side(), OrderType::Limit, kMid, 1};
            }
            case 2: return NewOrder{next_id_ + 1'000'000'000, symbol(), side(), OrderType::Limit, kMid, 0};
            case 3: {
                const std::array<Price, 3> bad{0, -5, max_price_ + 1};
                return NewOrder{next_id_ + 1'000'000'000,       symbol(), side(), OrderType::Limit,
                                bad.at(rng_.below(bad.size())), 5};
            }
            case 4: return CancelOrder{next_id_ + 2'000'000'000};           // UNKNOWN_ORDER
            case 5: return ModifyOrder{next_id_ + 2'000'000'000, kMid, 5};  // UNKNOWN_ORDER
            case 6: return ModifyOrder{recent_id(), kMid, 0};               // INVALID_MODIFY (oder UNKNOWN_ORDER)
            default: return ModifyOrder{recent_id(), -1, 500};              // INVALID_PRICE (oder UNKNOWN_ORDER)
        }
    }

    Lcg rng_;
    OrderId next_id_;
    OrderId first_id_;
    Price max_price_;
    std::vector<Issued> issued_;
};

EngineConfig mix_config() {
    EngineConfig config;
    config.order_capacity = std::size_t{1} << 14;  // > alle im Test je gleichzeitig liegenden Orders
    config.id_capacity = std::size_t{1} << 16;     // > alle im Test akzeptierten IDs
    config.level_capacity = 256;                   // Mix nutzt ~25 Preise je Seite, Warmup 80
    config.symbol_capacity = 4;
    config.max_tick_price = 4'096;
    return config;
}

// Füllt beide Bücher über viele Level (Pools/Vektoren auf Arbeitsgröße) und lässt einen
// Vorlauf-Mix laufen.
template <typename Engine>
void warm_up(Engine& engine, OrderId& next_id) {
    engine.add_symbol(kDense);
    engine.add_symbol(kSparse);
    for (const SymbolId sym : {kDense, kSparse}) {
        for (Price level = 1; level <= 80; ++level) {
            for (int k = 0; k < 3; ++k) {
                engine.process(NewOrder{next_id++, sym, Side::Buy, OrderType::Limit, kMid - 20 - level, 10});
                engine.process(NewOrder{next_id++, sym, Side::Sell, OrderType::Limit, kMid + 20 + level, 10});
            }
        }
    }
    MixGenerator warmup(7, next_id, mix_config().max_tick_price);
    const std::vector<Command> commands = warmup.generate(5'000);
    for (const Command& cmd : commands) {
        engine.process(cmd);
    }
    next_id += 10'000;  // Vorlauf hat höchstens so viele IDs vergeben
}

template <typename Engine>
void expect_mix_without_allocation() {
    CountingSink sink;
    Engine engine(sink, mix_config());
    OrderId next_id = 1;
    warm_up(engine, next_id);

    MixGenerator gen(42, next_id, mix_config().max_tick_price);
    const std::vector<Command> commands = gen.generate(20'000);
    const CountingSink before = sink;

    AllocationScope scope;
    for (const Command& cmd : commands) {
        engine.process(cmd);
    }
    const std::size_t allocations = scope.allocations();
    const std::size_t bytes = scope.bytes();

    EXPECT_EQ(allocations, 0U) << bytes << " Byte im heißen Pfad allokiert";

    // Der Mix hat tatsächlich alle Pfade durchlaufen.
    EXPECT_GT(sink.acks - before.acks, 5'000U);
    EXPECT_GT(sink.trades - before.trades, 1'000U);
    EXPECT_GT(sink.user_cancels - before.user_cancels, 100U);
    EXPECT_GT(sink.no_liquidity - before.no_liquidity, 0U);
    for (const RejectReason reason :
         {RejectReason::DuplicateId, RejectReason::UnknownSymbol, RejectReason::InvalidQuantity,
          RejectReason::InvalidPrice, RejectReason::UnknownOrder, RejectReason::InvalidModify}) {
        EXPECT_GT(sink.reject_count(reason), before.reject_count(reason)) << to_string(reason);
    }
}

TEST(AllocationTest, VectorEngineHotPathDoesNotAllocate) { expect_mix_without_allocation<VectorMatchingEngine>(); }

TEST(AllocationTest, TickEngineHotPathDoesNotAllocate) { expect_mix_without_allocation<TickMatchingEngine>(); }

// Gegenprobe, dass der Zähler greift.
TEST(AllocationTest, CounterSeesAllocations) {
    // Explizite Aufrufe statt new-Ausdrücken: die darf der Compiler nicht wegoptimieren.
    AllocationScope scope;
    void* a = ::operator new(16);
    void* b = ::operator new(64, std::align_val_t{64});
    ::operator delete(b, std::align_val_t{64});
    ::operator delete(a);
    const std::size_t allocations = scope.allocations();
    const std::size_t bytes = scope.bytes();
    EXPECT_EQ(allocations, 2U);
    EXPECT_EQ(bytes, 80U);
}

// MapLevels (std::map) ist die Referenzvariante: Jedes NEU angelegte Price Level kostet einen
// Map-Knoten. Alles andere (NEW auf bestehendes Level, Teil-Trades, CANCEL, MODIFY, Rejects)
// ist auch hier allokationsfrei.
TEST(AllocationTest, MapEngineAllocatesOnlyForNewPriceLevels) {
    CountingSink sink;
    MapMatchingEngine engine(sink, mix_config());
    engine.add_symbol(kDense);
    OrderId id = 1;
    for (Price p = 1; p <= 10; ++p) {
        for (int k = 0; k < 3; ++k) {
            engine.process(NewOrder{id++, kDense, Side::Buy, OrderType::Limit, kMid - p, 100});
            engine.process(NewOrder{id++, kDense, Side::Sell, OrderType::Limit, kMid + p, 100});
        }
    }
    // ids 1..60: Level kMid-1..kMid-10 (Bids) und kMid+1..kMid+10 (Asks), je 3 Orders a 100.
    const std::vector<Command> existing_levels{
        Command{NewOrder{100, kDense, Side::Buy, OrderType::Limit, kMid - 5, 7}},   // bestehendes Level
        Command{NewOrder{101, kDense, Side::Sell, OrderType::Market, 0, 50}},       // Teil-Trade
        Command{NewOrder{102, kDense, Side::Buy, OrderType::Limit, kMid + 1, 30}},  // Teil-Trade, voll gefüllt
        Command{CancelOrder{9}},                                                    // Mitte eines Levels
        Command{ModifyOrder{11, kMid - 2, 40}},                                     // Reduktion
        Command{ModifyOrder{13, kMid - 3, 150}},                                    // Preisänderung, bestehendes Level
        Command{NewOrder{1, kDense, Side::Buy, OrderType::Limit, kMid - 1, 1}},     // DUPLICATE_ID
        Command{NewOrder{103, kUnknownSymbol, Side::Buy, OrderType::Limit, kMid, 1}},
        Command{NewOrder{104, kDense, Side::Buy, OrderType::Limit, kMid, 0}},
        Command{NewOrder{105, kDense, Side::Buy, OrderType::Limit, -1, 1}},
        Command{CancelOrder{9}},                // UNKNOWN_ORDER
        Command{ModifyOrder{15, kMid - 8, 0}},  // INVALID_MODIFY
    };
    std::size_t allocations = 0;
    {
        AllocationScope scope;
        for (const Command& cmd : existing_levels) {
            engine.process(cmd);
        }
        allocations = scope.allocations();
    }
    EXPECT_EQ(allocations, 0U);
    EXPECT_EQ(sink.rejects, 6U);

    const Command new_level{NewOrder{106, kDense, Side::Buy, OrderType::Limit, kMid - 50, 10}};
    {
        AllocationScope scope;
        engine.process(new_level);
        allocations = scope.allocations();
    }
    EXPECT_EQ(allocations, 1U) << "erwartet: genau ein std::map-Knoten für das neue Level";
}

// Über den Kapazitäten: Wachstum statt Reject; Ergebnis identisch zur großzügig
// dimensionierten Engine.
template <typename Engine>
void expect_growth_beyond_capacity() {
    EngineConfig tiny;
    tiny.order_capacity = 2;
    tiny.id_capacity = 2;
    tiny.level_capacity = 1;
    tiny.symbol_capacity = 1;
    tiny.max_tick_price = 4'096;

    CountingSink sink;
    Engine engine(sink, tiny);
    engine.add_symbol(kDense);

    constexpr int kLevels = 500;
    constexpr int kPerLevel = 4;
    OrderId id = 1;
    std::size_t allocations = 0;
    {
        AllocationScope scope;
        for (int level = 0; level < kLevels; ++level) {
            for (int k = 0; k < kPerLevel; ++k) {
                engine.process(NewOrder{id++, kDense, Side::Buy, OrderType::Limit, kMid - 1 - level, 1});
                engine.process(NewOrder{id++, kDense, Side::Sell, OrderType::Limit, kMid + 1 + level, 1});
            }
        }
        allocations = scope.allocations();
    }
    constexpr std::size_t kOrders = 2U * kLevels * kPerLevel;
    EXPECT_GT(allocations, 0U) << "Kapazität überschritten, also muss gewachsen worden sein";
    EXPECT_EQ(sink.acks, kOrders);
    EXPECT_EQ(sink.rejects, 0U);

    const BookSnapshot snap = engine.snapshot(kDense, kLevels + 1);
    ASSERT_EQ(snap.bids.size(), static_cast<std::size_t>(kLevels));
    ASSERT_EQ(snap.asks.size(), static_cast<std::size_t>(kLevels));
    EXPECT_EQ(snap.bids.front().price, kMid - 1);
    EXPECT_EQ(snap.bids.back().price, kMid - kLevels);
    EXPECT_EQ(snap.asks.front().price, kMid + 1);
    EXPECT_EQ(snap.asks.back().price, kMid + kLevels);
    for (const BookLevel& level : snap.bids) {
        EXPECT_EQ(level.quantity, static_cast<Quantity>(kPerLevel));
    }

    // Alles wieder abräumen: jede gewachsene Struktur bleibt konsistent.
    engine.process(NewOrder{id++, kDense, Side::Sell, OrderType::Market, 0, kOrders});
    engine.process(NewOrder{id++, kDense, Side::Buy, OrderType::Market, 0, kOrders});
    EXPECT_EQ(sink.trades, kOrders);
    EXPECT_EQ(sink.no_liquidity, 2U);
    const BookSnapshot empty = engine.snapshot(kDense, 10);
    EXPECT_TRUE(empty.bids.empty());
    EXPECT_TRUE(empty.asks.empty());
}

TEST(AllocationTest, VectorEngineGrowsBeyondCapacity) { expect_growth_beyond_capacity<VectorMatchingEngine>(); }

TEST(AllocationTest, TickEngineGrowsBeyondCapacity) { expect_growth_beyond_capacity<TickMatchingEngine>(); }

TEST(AllocationTest, MapEngineGrowsBeyondCapacity) { expect_growth_beyond_capacity<MapMatchingEngine>(); }

// Symbol-Lookup: unbekannte und sehr große IDs ergeben UNKNOWN_SYMBOL ohne Allokation;
// add_symbol mit sehr großer ID allokiert nur das Buch, keinen ID-großen Vektor.
TEST(AllocationTest, SymbolLookupHandlesHugeIdsWithoutLargeAllocation) {
    CountingSink sink;
    VectorMatchingEngine engine(sink, mix_config());
    engine.add_symbol(3);

    const std::array<SymbolId, 6> unknown{0, 2, 4, kUnknownSymbol, SymbolId{1} << 31, SymbolId{1} << 16};
    std::size_t allocations = 0;
    {
        AllocationScope scope;
        OrderId id = 1;
        for (const SymbolId sym : unknown) {
            engine.process(NewOrder{id++, sym, Side::Buy, OrderType::Limit, kMid, 1});
        }
        allocations = scope.allocations();
    }
    EXPECT_EQ(allocations, 0U);
    EXPECT_EQ(sink.reject_count(RejectReason::UnknownSymbol), unknown.size());
    for (const SymbolId sym : unknown) {
        EXPECT_FALSE(engine.has_symbol(sym)) << sym;
        EXPECT_TRUE(engine.snapshot(sym, 5).bids.empty());
    }

    // Sehr große IDs (auch kUnknownSymbol selbst) sind als Symbole zulässig: sparse Pfad.
    std::size_t bytes = 0;
    {
        AllocationScope scope;
        engine.add_symbol(0xFFFF'FFFEU);
        engine.add_symbol(kUnknownSymbol);
        bytes = scope.bytes();
    }
    EXPECT_LT(bytes, std::size_t{1} << 20) << "kein Vektor über den ID-Raum";
    EXPECT_TRUE(engine.has_symbol(0xFFFF'FFFEU));
    EXPECT_TRUE(engine.has_symbol(kUnknownSymbol));
    EXPECT_FALSE(engine.has_symbol(0xFFFF'FFFDU));

    const std::size_t acks_before = sink.acks;
    {
        AllocationScope scope;
        engine.process(NewOrder{100, 0xFFFF'FFFEU, Side::Buy, OrderType::Limit, kMid, 5});
        engine.process(NewOrder{101, 0xFFFF'FFFEU, Side::Sell, OrderType::Limit, kMid, 5});
        engine.process(NewOrder{102, kUnknownSymbol, Side::Buy, OrderType::Limit, kMid, 5});
        allocations = scope.allocations();
    }
    EXPECT_EQ(allocations, 0U);
    EXPECT_EQ(sink.acks - acks_before, 3U);
    EXPECT_EQ(sink.trades, 1U);

    // Höchste dichte ID: Vektor wächst auf 2^16 Einträge (512 KiB), nicht mehr.
    engine.add_symbol((SymbolId{1} << 16) - 1);
    EXPECT_TRUE(engine.has_symbol((SymbolId{1} << 16) - 1));
    EXPECT_FALSE(engine.has_symbol(SymbolId{1} << 16));
}

}  // namespace
