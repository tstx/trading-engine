// LevelStores (MapLevels, SortedVectorLevels, TickArrayLevels) direkt gegen den Vertrag aus
// level_store.hpp testen, jeweils für Bids (std::greater<>) und Asks (std::less<>).

#include "engine/level_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "engine/engine_config.hpp"
#include "engine/price_level.hpp"
#include "engine/sorted_vector_levels.hpp"
#include "engine/tick_array_levels.hpp"
#include "engine/types.hpp"

namespace {

using engine::EngineConfig;
using engine::Price;
using engine::PriceLevel;

template <typename Store>
std::vector<Price> prices_of(const Store& store) {
    std::vector<Price> out;
    store.for_each([&](const PriceLevel& level) {
        out.push_back(level.price);
        return true;
    });
    return out;
}

template <typename Store>
struct CompareOf;
template <template <typename> class S, typename C>
struct CompareOf<S<C>> {
    using type = C;
};

template <typename Store>
std::vector<Price> sorted_best_first(std::vector<Price> prices) {
    std::sort(prices.begin(), prices.end(), typename CompareOf<Store>::type{});
    return prices;
}

template <typename Store>
class LevelStoreTest : public ::testing::Test {
protected:
    using Compare = typename CompareOf<Store>::type;
    static bool better(Price a, Price b) { return Compare{}(a, b); }
};

using StoreTypes = ::testing::Types<engine::MapLevels<std::greater<>>, engine::MapLevels<std::less<>>,
                                    engine::SortedVectorLevels<std::greater<>>, engine::SortedVectorLevels<std::less<>>,
                                    engine::TickArrayLevels<std::greater<>>, engine::TickArrayLevels<std::less<>>>;
TYPED_TEST_SUITE(LevelStoreTest, StoreTypes);

TYPED_TEST(LevelStoreTest, EmptyStore) {
    const TypeParam store{EngineConfig{}};
    EXPECT_TRUE(store.empty());
    EXPECT_EQ(store.size(), 0U);
    EXPECT_EQ(store.best(), nullptr);
    EXPECT_TRUE(prices_of(store).empty());
}

TYPED_TEST(LevelStoreTest, FindOrCreateReturnsSameLevelForSamePrice) {
    TypeParam store{EngineConfig{}};
    PriceLevel* a = store.find_or_create(100);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->price, 100);
    EXPECT_TRUE(a->empty());
    EXPECT_EQ(a->total_quantity, 0U);
    EXPECT_EQ(store.find_or_create(100), a);
    EXPECT_EQ(store.size(), 1U);
}

TYPED_TEST(LevelStoreTest, ForEachIsSortedBestFirst) {
    TypeParam store{EngineConfig{}};
    const std::vector<Price> input{500, 100, 900, 300, 700, 200, 800, 101, 99, 64, 65, 63, 4096, 4095, 4097};
    for (Price p : input) {
        store.find_or_create(p);
    }
    EXPECT_EQ(store.size(), input.size());
    EXPECT_EQ(prices_of(store), sorted_best_first<TypeParam>(input));
}

TYPED_TEST(LevelStoreTest, ForEachStopsWhenCallbackReturnsFalse) {
    TypeParam store{EngineConfig{}};
    for (Price p = 10; p < 20; ++p) {
        store.find_or_create(p);
    }
    std::vector<Price> seen;
    store.for_each([&](const PriceLevel& level) {
        seen.push_back(level.price);
        return seen.size() < 3;
    });
    ASSERT_EQ(seen.size(), 3U);
    const Price first = TestFixture::better(10, 19) ? 10 : 19;
    EXPECT_EQ(seen[0], first);
}

TYPED_TEST(LevelStoreTest, BestFollowsInsertAndErase) {
    TypeParam store{EngineConfig{}};
    PriceLevel* mid = store.find_or_create(1000);
    EXPECT_EQ(store.best(), mid);

    PriceLevel* low = store.find_or_create(900);
    PriceLevel* high = store.find_or_create(1100);
    PriceLevel* top = TestFixture::better(900, 1100) ? low : high;
    PriceLevel* bottom = top == low ? high : low;
    EXPECT_EQ(store.best(), top);

    store.erase(top);  // bestes Level weg -> nächstes
    EXPECT_EQ(store.best(), mid);
    EXPECT_EQ(store.size(), 2U);

    store.erase(bottom);  // schlechtestes Level weg -> best unverändert
    EXPECT_EQ(store.best(), mid);

    store.erase(mid);
    EXPECT_EQ(store.best(), nullptr);
    EXPECT_TRUE(store.empty());

    PriceLevel* again = store.find_or_create(1000);  // nach Leerlaufen wieder nutzbar
    EXPECT_EQ(store.best(), again);
    EXPECT_EQ(again->price, 1000);
    EXPECT_TRUE(again->empty());
    EXPECT_EQ(store.size(), 1U);
}

TYPED_TEST(LevelStoreTest, LevelAddressStableWhileOthersChange) {
    EngineConfig config{};
    config.level_capacity = 4;  // erzwingt Wachstum von Vektor/Pool
    TypeParam store{config};
    PriceLevel* anchor = store.find_or_create(50000);
    anchor->total_quantity = 4242;  // Markierung

    std::vector<PriceLevel*> others;
    for (Price i = 1; i <= 2000; ++i) {
        others.push_back(store.find_or_create(50000 + i));
        others.push_back(store.find_or_create(50000 - i));
    }
    // Jedes zweite wieder löschen, dann neue dazwischen.
    for (std::size_t i = 0; i < others.size(); i += 2) {
        store.erase(others[i]);
    }
    for (Price i = 2001; i <= 3000; ++i) {
        store.find_or_create(50000 + i);
        store.find_or_create(50000 - i);
    }

    EXPECT_EQ(store.find_or_create(50000), anchor);
    EXPECT_EQ(anchor->price, 50000);
    EXPECT_EQ(anchor->total_quantity, 4242U);
    EXPECT_EQ(store.size(), 1U + 2000U + 2000U);

    bool found = false;
    store.for_each([&](const PriceLevel& level) {
        if (&level == anchor) {
            found = true;
            return false;
        }
        return true;
    });
    EXPECT_TRUE(found);
}

// 10k zufällige Operationen, Referenz: MapLevels mit gleicher Compare-Richtung. Preise liegen
// geclustert (um einen wandernden Mittelpunkt) plus gelegentliche Ausreißer über das ganze Band.
TYPED_TEST(LevelStoreTest, RandomizedAgainstMapLevels) {
    using Compare = typename TestFixture::Compare;
    EngineConfig config{};
    config.level_capacity = 16;
    TypeParam store{config};
    engine::MapLevels<Compare> reference{config};
    std::map<Price, PriceLevel*> addresses;  // Adressen der lebenden Levels im getesteten Store

    std::mt19937_64 rng{12345};
    const Price max_price = config.max_tick_price;
    Price center = 60000;
    for (int op = 0; op < 10000; ++op) {
        const auto roll = rng() % 100;
        if (roll < 55 || addresses.empty()) {
            Price p = 0;
            if (rng() % 20 == 0) {
                p = static_cast<Price>(1 + rng() % static_cast<std::uint64_t>(max_price));
            } else {
                center = std::clamp<Price>(center + static_cast<Price>(rng() % 21) - 10, 200, max_price - 200);
                p = center + static_cast<Price>(rng() % 101) - 50;
            }
            PriceLevel* level = store.find_or_create(p);
            reference.find_or_create(p);
            ASSERT_EQ(level->price, p);
            auto [it, inserted] = addresses.try_emplace(p, level);
            ASSERT_EQ(it->second, level) << "Adresse für Preis " << p << " hat sich geändert";
        } else {
            // Bevorzugt das beste Level löschen (typisch fürs Matching), sonst ein zufälliges.
            auto it = addresses.begin();
            if (roll < 75) {
                it = addresses.find(store.best()->price);
            } else {
                std::advance(it, static_cast<std::ptrdiff_t>(rng() % addresses.size()));
            }
            store.erase(it->second);
            reference.erase(reference.find_or_create(it->first));
            addresses.erase(it);
        }

        ASSERT_EQ(store.size(), reference.size());
        ASSERT_EQ(store.empty(), reference.empty());
        if (reference.empty()) {
            ASSERT_EQ(store.best(), nullptr);
        } else {
            ASSERT_NE(store.best(), nullptr);
            ASSERT_EQ(store.best()->price, reference.best()->price) << "op " << op;
        }
        if (op % 100 == 0) {
            ASSERT_EQ(prices_of(store), prices_of(reference)) << "op " << op;
        }
    }
    EXPECT_EQ(prices_of(store), prices_of(reference));
}

// ---------------------------------------------------------------------------
// TickArrayLevels-spezifisch

template <typename Store>
class TickArrayLevelsTest : public ::testing::Test {
protected:
    static bool better(Price a, Price b) { return typename CompareOf<Store>::type{}(a, b); }
};

using TickTypes = ::testing::Types<engine::TickArrayLevels<std::greater<>>, engine::TickArrayLevels<std::less<>>>;
TYPED_TEST_SUITE(TickArrayLevelsTest, TickTypes);

TYPED_TEST(TickArrayLevelsTest, AcceptsOnlyPriceBand) {
    const EngineConfig def{};
    const TypeParam store{def};
    EXPECT_FALSE(store.accepts(0));
    EXPECT_FALSE(store.accepts(-1));
    EXPECT_FALSE(store.accepts(std::numeric_limits<Price>::min()));
    EXPECT_TRUE(store.accepts(1));
    EXPECT_TRUE(store.accepts(def.max_tick_price));
    EXPECT_FALSE(store.accepts(def.max_tick_price + 1));
    EXPECT_FALSE(store.accepts(std::numeric_limits<Price>::max()));

    EngineConfig small{};
    small.max_tick_price = 100;
    const TypeParam small_store{small};
    EXPECT_TRUE(small_store.accepts(100));
    EXPECT_FALSE(small_store.accepts(101));
}

TYPED_TEST(TickArrayLevelsTest, EdgePricesOneAndMax) {
    const EngineConfig config{};
    const Price max = config.max_tick_price;
    TypeParam store{config};
    PriceLevel* one = store.find_or_create(1);
    PriceLevel* top = store.find_or_create(max);
    EXPECT_EQ(one->price, 1);
    EXPECT_EQ(top->price, max);
    EXPECT_EQ(store.size(), 2U);

    PriceLevel* best = TestFixture::better(1, max) ? one : top;
    PriceLevel* worst = best == one ? top : one;
    EXPECT_EQ(store.best(), best);
    EXPECT_EQ(prices_of(store), (std::vector<Price>{best->price, worst->price}));

    // Bestes Level löschen -> Suche über die komplette Lücke bis ans andere Ende.
    store.erase(best);
    EXPECT_EQ(store.best(), worst);
    store.erase(worst);
    EXPECT_EQ(store.best(), nullptr);
    EXPECT_TRUE(store.empty());
}

TYPED_TEST(TickArrayLevelsTest, BestSearchAcrossLargeGaps) {
    const EngineConfig config{};
    TypeParam store{config};
    // Preise an Wort- (64) und Summary-Grenzen (4096) sowie mit großen Lücken dazwischen.
    std::vector<Price> prices{
        2, 63, 64, 65, 127, 128, 4095, 4096, 4097, 8191, 70000, 131000, config.max_tick_price - 1};
    for (Price p : prices) {
        store.find_or_create(p);
    }
    const std::vector<Price> expected = sorted_best_first<TypeParam>(prices);
    EXPECT_EQ(prices_of(store), expected);
    // Immer das beste Level löschen; best() muss jeweils über die Lücke zum nächsten springen.
    for (std::size_t i = 0; i < expected.size(); ++i) {
        ASSERT_NE(store.best(), nullptr);
        ASSERT_EQ(store.best()->price, expected[i]);
        store.erase(store.best());
    }
    EXPECT_TRUE(store.empty());
    EXPECT_EQ(store.best(), nullptr);
}

TYPED_TEST(TickArrayLevelsTest, WorseInsertDoesNotChangeBest) {
    TypeParam store{EngineConfig{}};
    PriceLevel* first = store.find_or_create(1000);
    const Price worse = TestFixture::better(999, 1000) ? 1001 : 999;
    const Price better = worse == 999 ? 1001 : 999;
    store.find_or_create(worse);
    EXPECT_EQ(store.best(), first);
    PriceLevel* b = store.find_or_create(better);
    EXPECT_EQ(store.best(), b);
}

TYPED_TEST(TickArrayLevelsTest, TinyBand) {
    EngineConfig config{};
    config.max_tick_price = 1;
    TypeParam store{config};
    EXPECT_TRUE(store.accepts(1));
    EXPECT_FALSE(store.accepts(2));
    PriceLevel* level = store.find_or_create(1);
    EXPECT_EQ(store.best(), level);
    store.erase(level);
    EXPECT_TRUE(store.empty());
}

}  // namespace
