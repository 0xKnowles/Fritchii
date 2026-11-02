// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "common/literals.h"
#include "video_core/vulkan_common/memory_budget.h"

using namespace Common::Literals;

TEST_CASE("Reporting-only adjusted usage and budget preserve raw pressure", "[memory_budget]") {
    using Vulkan::AllocationAdjustedBytes;
    using Vulkan::AllocationAdjustedBudget;
    constexpr u64 usage = 9180_MiB;
    constexpr u64 budget = 8994_MiB;
    constexpr u64 blocks = 8745_MiB;
    constexpr u64 allocations = blocks - 1377_MiB;
    const auto adjusted_usage = AllocationAdjustedBytes(usage, blocks, allocations);
    const auto adjusted_budget = AllocationAdjustedBudget(budget, blocks, allocations);
    CHECK(adjusted_usage == 7803_MiB);
    CHECK(adjusted_budget == 7617_MiB);
    CHECK(adjusted_usage > adjusted_budget);
    CHECK(adjusted_usage - adjusted_budget == usage - budget);

    // Additional heaps cancel out rather than inventing room on the main heap.
    const u64 other_heap = AllocationAdjustedBytes(500_MiB, 400_MiB, 350_MiB);
    CHECK((adjusted_usage + other_heap) - (adjusted_budget + other_heap) == usage - budget);
    // Applying a simulated raw heap cap must precede the unit conversion.
    CHECK(AllocationAdjustedBudget(8_GiB, blocks, allocations) == 8_GiB - 1377_MiB);
}

TEST_CASE("Adjusted memory budgets saturate without losing known exhaustion", "[memory_budget]") {
    using Vulkan::AllocationAdjustedBytes;
    using Vulkan::AllocationAdjustedBudget;
    CHECK(AllocationAdjustedBytes(50, 100, 0) == 0);
    CHECK(AllocationAdjustedBytes(50, 10, 20) == 50);
    CHECK(AllocationAdjustedBudget(50, 100, 0) == 1);
    CHECK(AllocationAdjustedBudget(0, 100, 0) == 0);
    CHECK(AllocationAdjustedBudget(50, 10, 20) == 50);
    CHECK(AllocationAdjustedBudget(50, 0, 0) == 50);
    CHECK(AllocationAdjustedBudget(std::numeric_limits<u64>::max(), 100, 50) ==
          std::numeric_limits<u64>::max() - 50);
    for (u64 slack = 0; slack <= 300; ++slack) {
        const u64 adjusted_usage = AllocationAdjustedBytes(800, 700, 700 - slack);
        const u64 adjusted_budget = AllocationAdjustedBudget(1000, 700, 700 - slack);
        CHECK(adjusted_budget - adjusted_usage == 200);
    }
}

TEST_CASE("Host commit pressure limits reclaimable cache budgets", "[memory_budget]") {
    using Vulkan::LimitBudgetForHostCommit;
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 20_GiB, 4_GiB) == 10_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 5_GiB, 4_GiB) == 9_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 4_GiB, 4_GiB) == 8_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 3_GiB, 4_GiB) == 7_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 0, 4_GiB) == 4_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 1_GiB, 0, 4_GiB) == 0);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 5_GiB, 4_GiB) ==
            LimitBudgetForHostCommit(10_GiB, 7_GiB, 6_GiB, 4_GiB));
    REQUIRE(LimitBudgetForHostCommit(6_GiB, 8_GiB, 20_GiB, 4_GiB) == 6_GiB);
    REQUIRE(LimitBudgetForHostCommit(6_GiB, 8_GiB, 3_GiB, 4_GiB) == 6_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, std::numeric_limits<u64>::max(), 4_GiB) ==
            10_GiB);
}
