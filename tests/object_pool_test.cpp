// ObjectPool: Freiliste, Wiederverwendung, Adressstabilität, Wachstum.

#include "engine/object_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "engine/price_level.hpp"

namespace {

using engine::ObjectPool;

struct Item {
    std::uint64_t id{0};
    std::uint64_t payload{0};
};

TEST(ObjectPoolTest, CreateDestroyReuse) {
    ObjectPool<Item> pool(4);
    EXPECT_EQ(pool.capacity(), 4U);
    EXPECT_EQ(pool.in_use(), 0U);

    Item* a = pool.create(Item{1, 10});
    Item* b = pool.create(Item{2, 20});
    EXPECT_NE(a, b);
    EXPECT_EQ(a->id, 1U);
    EXPECT_EQ(b->payload, 20U);
    EXPECT_EQ(pool.in_use(), 2U);

    pool.destroy(a);
    EXPECT_EQ(pool.in_use(), 1U);
    Item* c = pool.create(Item{3, 30});
    EXPECT_EQ(c, a);  // LIFO-Freiliste: zuletzt freigegebener Slot zuerst
    EXPECT_EQ(c->id, 3U);
    EXPECT_EQ(b->id, 2U);  // unberührt
    EXPECT_EQ(pool.growth_count(), 0U);
}

TEST(ObjectPoolTest, DefaultConstructedObjects) {
    ObjectPool<engine::Order> pool(2);
    engine::Order* o = pool.create();
    EXPECT_EQ(o->id, engine::Order{}.id);
    pool.destroy(o);
}

TEST(ObjectPoolTest, NoGrowthWithinInitialCapacity) {
    ObjectPool<Item> pool(100);
    for (int round = 0; round < 10; ++round) {
        std::vector<Item*> live;
        for (std::uint64_t i = 0; i < 100; ++i) {
            live.push_back(pool.create(Item{i, i}));
        }
        for (Item* p : live) {
            pool.destroy(p);
        }
    }
    EXPECT_EQ(pool.growth_count(), 0U);
    EXPECT_EQ(pool.capacity(), 100U);
}

TEST(ObjectPoolTest, GrowsExactlyWhenExceedingInitialCapacity) {
    ObjectPool<Item> pool(8);
    std::vector<Item*> live;
    for (std::uint64_t i = 0; i < 8; ++i) {
        live.push_back(pool.create(Item{i, 0}));
    }
    EXPECT_EQ(pool.growth_count(), 0U);
    live.push_back(pool.create(Item{8, 0}));
    EXPECT_EQ(pool.growth_count(), 1U);
    EXPECT_GE(pool.capacity(), 9U);
}

TEST(ObjectPoolTest, ZeroInitialCapacity) {
    ObjectPool<Item> pool(0);
    EXPECT_EQ(pool.capacity(), 0U);
    Item* a = pool.create(Item{1, 1});
    EXPECT_EQ(a->id, 1U);
    EXPECT_EQ(pool.growth_count(), 1U);
    pool.destroy(a);
    EXPECT_EQ(pool.in_use(), 0U);
}

TEST(ObjectPoolTest, AddressesStableAcrossGrowth) {
    ObjectPool<Item> pool(3);
    std::vector<Item*> live;
    for (std::uint64_t i = 0; i < 5000; ++i) {
        live.push_back(pool.create(Item{i, i * 7}));
    }
    EXPECT_GT(pool.growth_count(), 0U);
    EXPECT_EQ(pool.in_use(), 5000U);
    std::set<Item*> distinct(live.begin(), live.end());
    EXPECT_EQ(distinct.size(), live.size());
    for (std::uint64_t i = 0; i < live.size(); ++i) {
        ASSERT_EQ(live[i]->id, i);
        ASSERT_EQ(live[i]->payload, i * 7);
        ASSERT_EQ(reinterpret_cast<std::uintptr_t>(live[i]) % alignof(Item), 0U);
    }
}

TEST(ObjectPoolTest, RandomCyclesAgainstReferenceModel) {
    constexpr std::size_t kInitial = 256;
    ObjectPool<Item> pool(kInitial);
    std::mt19937_64 rng(42);
    std::unordered_map<Item*, Item> model;  // Adresse -> erwarteter Inhalt
    std::vector<Item*> live;
    std::size_t max_live = 0;
    std::uint64_t next_id = 0;

    for (int step = 0; step < 200000; ++step) {
        // Phasen mit Tendenz zum Wachsen bzw. Schrumpfen, damit die Kapazität überschritten wird.
        const bool grow_phase = (step / 20000) % 2 == 0;
        const bool do_create = live.empty() || (rng() % 100) < (grow_phase ? 60U : 40U);
        if (do_create) {
            const Item v{next_id++, rng()};
            Item* p = pool.create(v);
            ASSERT_FALSE(model.contains(p)) << "Slot doppelt vergeben";
            model.emplace(p, v);
            live.push_back(p);
        } else {
            const std::size_t idx = rng() % live.size();
            Item* p = live[idx];
            const Item& expected = model.at(p);
            ASSERT_EQ(p->id, expected.id);
            ASSERT_EQ(p->payload, expected.payload);
            model.erase(p);
            live[idx] = live.back();
            live.pop_back();
            pool.destroy(p);
        }
        max_live = std::max(max_live, live.size());
        ASSERT_EQ(pool.in_use(), live.size());
    }
    for (Item* p : live) {
        const Item& expected = model.at(p);
        ASSERT_EQ(p->id, expected.id);
        ASSERT_EQ(p->payload, expected.payload);
    }
    EXPECT_EQ(pool.growth_count() > 0, max_live > kInitial);
    EXPECT_GE(pool.capacity(), max_live);
}

}  // namespace
