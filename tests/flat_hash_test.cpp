// FlatHashMap/FlatHashSet: Grundoperationen, Randschlüssel, Rehash-Verhalten,
// Backward-Shift-Deletion und randomisierter Vergleich gegen std::unordered_map/set.

#include "engine/flat_hash.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

using engine::FlatHashMap;
using engine::FlatHashSet;

TEST(FlatHashMapTest, InsertFindErase) {
    FlatHashMap<std::uint64_t, int> m(16);
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.find(5), nullptr);
    EXPECT_FALSE(m.contains(5));

    EXPECT_TRUE(m.insert(5, 50));
    EXPECT_TRUE(m.insert(6, 60));
    EXPECT_EQ(m.size(), 2U);
    ASSERT_NE(m.find(5), nullptr);
    EXPECT_EQ(*m.find(5), 50);
    EXPECT_TRUE(m.contains(6));

    *m.find(6) = 61;  // Wert über Zeiger änderbar
    const auto& cm = m;
    ASSERT_NE(cm.find(6), nullptr);
    EXPECT_EQ(*cm.find(6), 61);

    EXPECT_TRUE(m.erase(5));
    EXPECT_FALSE(m.erase(5));
    EXPECT_FALSE(m.contains(5));
    EXPECT_EQ(m.size(), 1U);
    EXPECT_TRUE(m.erase(6));
    EXPECT_TRUE(m.empty());
}

TEST(FlatHashMapTest, DuplicateInsertKeepsValue) {
    FlatHashMap<std::uint64_t, int> m;
    EXPECT_TRUE(m.insert(1, 10));
    EXPECT_FALSE(m.insert(1, 99));
    EXPECT_EQ(*m.find(1), 10);
    EXPECT_EQ(m.size(), 1U);
}

TEST(FlatHashMapTest, ExtremeKeys) {
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    FlatHashMap<std::uint64_t, int> m;
    EXPECT_FALSE(m.contains(0));
    EXPECT_FALSE(m.contains(kMax));
    EXPECT_EQ(m.find(kMax), nullptr);
    EXPECT_TRUE(m.insert(0, 1));
    EXPECT_TRUE(m.insert(kMax, 2));
    EXPECT_FALSE(m.insert(kMax, 3));
    EXPECT_EQ(m.size(), 2U);
    EXPECT_EQ(*m.find(0), 1);
    EXPECT_EQ(*m.find(kMax), 2);
    EXPECT_TRUE(m.erase(kMax));
    EXPECT_FALSE(m.erase(kMax));
    EXPECT_FALSE(m.contains(kMax));
    EXPECT_TRUE(m.contains(0));
    EXPECT_TRUE(m.erase(0));
    EXPECT_TRUE(m.empty());
}

TEST(FlatHashMapTest, SignedKeys) {
    FlatHashMap<std::int32_t, int> m;
    for (std::int32_t k :
         {std::numeric_limits<std::int32_t>::min(), -1, 0, 1, std::numeric_limits<std::int32_t>::max()}) {
        EXPECT_TRUE(m.insert(k, k / 2));
    }
    EXPECT_EQ(m.size(), 5U);
    EXPECT_EQ(*m.find(-1), 0);
    EXPECT_EQ(*m.find(std::numeric_limits<std::int32_t>::min()), std::numeric_limits<std::int32_t>::min() / 2);
    EXPECT_EQ(*m.find(std::numeric_limits<std::int32_t>::max()), std::numeric_limits<std::int32_t>::max() / 2);
}

TEST(FlatHashSetTest, Basics) {
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    FlatHashSet<std::uint64_t> s;
    EXPECT_TRUE(s.empty());
    EXPECT_TRUE(s.insert(0));
    EXPECT_TRUE(s.insert(kMax));
    EXPECT_TRUE(s.insert(42));
    EXPECT_FALSE(s.insert(42));
    EXPECT_FALSE(s.insert(kMax));
    EXPECT_EQ(s.size(), 3U);
    EXPECT_TRUE(s.contains(0));
    EXPECT_TRUE(s.contains(kMax));
    EXPECT_FALSE(s.contains(41));
    EXPECT_TRUE(s.erase(42));
    EXPECT_FALSE(s.erase(42));
    EXPECT_EQ(s.size(), 2U);
}

TEST(FlatHashTest, NoRehashUpToExpectedSize) {
    for (std::size_t expected : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8}, std::size_t{9},
                                 std::size_t{1000}, std::size_t{1} << 16}) {
        FlatHashSet<std::uint64_t> s(expected);
        FlatHashMap<std::uint64_t, std::uint64_t> m(expected);
        const std::size_t cap = s.capacity();
        EXPECT_TRUE(std::has_single_bit(cap));
        EXPECT_GE(cap, 2 * expected);  // Lastfaktor <= 0.5
        for (std::uint64_t k = 0; k < expected; ++k) {
            s.insert(k);
            m.insert(k * 977, k);
        }
        EXPECT_EQ(s.rehash_count(), 0U) << expected;
        EXPECT_EQ(m.rehash_count(), 0U) << expected;
        EXPECT_EQ(s.capacity(), cap);
        EXPECT_EQ(s.size(), expected);
    }
}

TEST(FlatHashTest, RehashBeyondExpectedSize) {
    FlatHashSet<std::uint64_t> s(1000);
    const std::size_t cap = s.capacity();
    std::uint64_t k = 0;
    while (s.rehash_count() == 0) {
        ASSERT_LE(k, cap) << "Lastfaktor > 0.5 ohne Rehash";
        s.insert(k++);
    }
    EXPECT_EQ(s.capacity(), cap * 2);
    EXPECT_EQ(k, cap / 2 + 1);
    for (std::uint64_t i = 0; i < k; ++i) {
        ASSERT_TRUE(s.contains(i));
    }
    // Weiteres Wachstum über mehrere Verdopplungen
    for (std::uint64_t i = k; i < 100000; ++i) {
        s.insert(i);
    }
    EXPECT_GT(s.rehash_count(), 1U);
    EXPECT_EQ(s.size(), 100000U);
    EXPECT_LE(s.size() * 2, s.capacity());
    for (std::uint64_t i = 0; i < 100000; ++i) {
        ASSERT_TRUE(s.contains(i));
    }
}

TEST(FlatHashTest, ManyInsertEraseCyclesDoNotRehash) {
    // Ohne Tombstones darf ein Dauerbetrieb mit konstanter Größe nie rehashen.
    FlatHashMap<std::uint64_t, std::uint64_t> m(1024);
    for (std::uint64_t id = 0; id < 1'000'000; ++id) {
        ASSERT_TRUE(m.insert(id, id));
        if (id >= 1000) {
            ASSERT_TRUE(m.erase(id - 1000));
        }
    }
    EXPECT_EQ(m.size(), 1000U);
    EXPECT_EQ(m.rehash_count(), 0U);
    for (std::uint64_t id = 1'000'000 - 1000; id < 1'000'000; ++id) {
        ASSERT_NE(m.find(id), nullptr);
        ASSERT_EQ(*m.find(id), id);
    }
}

// Liefert n Schlüssel, deren Heimat-Slot in einer Tabelle mit 2^bits Slots gleich target ist.
std::vector<std::uint64_t> keys_with_home(std::size_t target, unsigned bits, std::size_t n) {
    std::vector<std::uint64_t> keys;
    for (std::uint64_t k = 1; keys.size() < n; ++k) {
        if (engine::detail::flat_hash_home(k, 64 - bits) == target) {
            keys.push_back(k);
        }
    }
    return keys;
}

TEST(FlatHashTest, BackwardShiftAcrossTableEnd) {
    FlatHashMap<std::uint64_t, int> m(8);  // 16 Slots
    ASSERT_EQ(m.capacity(), 16U);
    constexpr unsigned kBits = 4;

    // Kette: A, B, C mit Heimat 15 -> Slots 15, 0, 1; D mit Heimat 0 -> Slot 2;
    // E mit Heimat 2 -> Slot 3; F mit Heimat 1 -> Slot 4.
    const auto h15 = keys_with_home(15, kBits, 3);
    const auto h0 = keys_with_home(0, kBits, 1);
    const auto h2 = keys_with_home(2, kBits, 1);
    const auto h1 = keys_with_home(1, kBits, 1);
    const std::vector<std::uint64_t> keys{h15[0], h15[1], h15[2], h0[0], h2[0], h1[0]};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        ASSERT_TRUE(m.insert(keys[i], static_cast<int>(i)));
    }

    std::vector<bool> present(keys.size(), true);
    auto check_all = [&] {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const int* v = m.find(keys[i]);
            if (present[i]) {
                ASSERT_NE(v, nullptr) << "Schlüssel " << i << " verloren";
                EXPECT_EQ(*v, static_cast<int>(i));
            } else {
                EXPECT_EQ(v, nullptr) << "Schlüssel " << i << " noch da";
            }
        }
    };
    check_all();

    // A (Slot 15) löschen: die Kette muss über das Tabellenende zurückrücken.
    ASSERT_TRUE(m.erase(keys[0]));
    present[0] = false;
    check_all();

    // Mittleres Element (D) löschen und wieder einfügen.
    ASSERT_TRUE(m.erase(keys[3]));
    present[3] = false;
    check_all();
    ASSERT_TRUE(m.insert(keys[3], 3));
    present[3] = true;
    check_all();

    // Alles in umgekehrter Reihenfolge löschen.
    for (std::size_t i = keys.size(); i-- > 1;) {
        ASSERT_TRUE(m.erase(keys[i]));
        present[i] = false;
        check_all();
    }
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.rehash_count(), 0U);
}

TEST(FlatHashTest, RandomizedAgainstStdUnorderedMap) {
    std::mt19937_64 rng(1234);
    for (int variant = 0; variant < 4; ++variant) {
        // Varianten: kleine vs. passende Startgröße, fortlaufende vs. zufällige Schlüssel.
        const bool small_table = (variant & 1) != 0;
        const bool sequential = (variant & 2) != 0;
        SCOPED_TRACE(variant);
        FlatHashMap<std::uint64_t, std::uint64_t> m(small_table ? 4 : 4096);
        FlatHashSet<std::uint64_t> s(small_table ? 4 : 4096);
        std::unordered_map<std::uint64_t, std::uint64_t> ref;
        std::vector<std::uint64_t> used;  // bereits benutzte Schlüssel, um Treffer zu erzeugen
        std::uint64_t next_seq = 0;

        auto random_key = [&]() -> std::uint64_t {
            const auto r = rng() % 100;
            if (!used.empty() && r < 50) {
                return used[rng() % used.size()];
            }
            if (r < 52) {
                return (rng() & 1) != 0 ? 0 : std::numeric_limits<std::uint64_t>::max();
            }
            if (sequential) {
                return next_seq++;
            }
            // Zufällig, teils mit gleichen niedrigen Bits (kollisionsfreudig)
            return (rng() % 3 == 0) ? (rng() << 20) : rng();
        };

        for (int op = 0; op < 100000; ++op) {
            const auto r = rng() % 100;
            const std::uint64_t k = random_key();
            if (r < 50) {
                const std::uint64_t v = rng();
                const bool expect = ref.emplace(k, v).second;
                ASSERT_EQ(m.insert(k, v), expect);
                ASSERT_EQ(s.insert(k), expect);
                if (expect) {
                    used.push_back(k);
                }
            } else if (r < 85) {
                const bool expect = ref.erase(k) > 0;
                ASSERT_EQ(m.erase(k), expect);
                ASSERT_EQ(s.erase(k), expect);
            } else {
                const auto it = ref.find(k);
                const std::uint64_t* v = m.find(k);
                if (it == ref.end()) {
                    ASSERT_EQ(v, nullptr);
                    ASSERT_FALSE(s.contains(k));
                } else {
                    ASSERT_NE(v, nullptr);
                    ASSERT_EQ(*v, it->second);
                    ASSERT_TRUE(s.contains(k));
                }
            }
            ASSERT_EQ(m.size(), ref.size());
            ASSERT_EQ(s.size(), ref.size());
            if (used.size() > 20000) {
                used.erase(used.begin(), used.begin() + 10000);
            }
        }
        // Vollständiger Abgleich am Ende
        for (const auto& [k, v] : ref) {
            const std::uint64_t* found = m.find(k);
            ASSERT_NE(found, nullptr);
            ASSERT_EQ(*found, v);
            ASSERT_TRUE(s.contains(k));
        }
        if (small_table) {
            EXPECT_GT(m.rehash_count(), 0U);
        }
    }
}

}  // namespace
