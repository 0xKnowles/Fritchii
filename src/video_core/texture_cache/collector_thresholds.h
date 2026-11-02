// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace VideoCommon {

struct CollectorThresholds {
    u64 expected{};
    u64 critical{};
};

// The collector's thresholds under a driver budget. Do not reduce this budget by allocator
// slack created by the collector's own evictions: the Vulkan runtime counts
// usage as allocation bytes and passes the driver's own budget (never reduced by free VMA block
// space, which every eviction increases). A margin of 1/8 of the budget, at most 1 GiB, keeps
// room for a frame's new images before allocations exceed the budget.
constexpr CollectorThresholds BudgetCollectorThresholds(u64 budget) {
    constexpr u64 MaxMargin = u64{1} << 30;
    const u64 margin = (std::min)(budget / 8, MaxMargin);
    return {.expected = budget - 2 * margin, .critical = budget - margin};
}

} // namespace VideoCommon
