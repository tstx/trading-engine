#include <gtest/gtest.h>

#include "engine/types.hpp"

TEST(Smoke, EnumNames) {
    EXPECT_EQ(engine::to_string(engine::Side::Buy), "BUY");
    EXPECT_EQ(engine::to_string(engine::RejectReason::DuplicateId), "DUPLICATE_ID");
    EXPECT_EQ(engine::to_string(engine::CancelReason::NoLiquidity), "NO_LIQUIDITY");
}
