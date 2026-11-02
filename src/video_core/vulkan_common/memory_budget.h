// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace Vulkan {

// Driver usage minus the unused portions of VMA blocks: what the caches' evictions control.
constexpr u64 AllocationAdjustedBytes(u64 bytes, u64 block_bytes, u64 allocation_bytes) {
    const u64 unused = block_bytes > allocation_bytes ? block_bytes - allocation_bytes : 0;
    return bytes - (std::min)(bytes, unused);
}

// The budget left for allocations if no new block could reuse free block space. For reporting
// only: NOT a collector threshold. Comparing AllocationAdjustedBytes usage with it is the same as
// comparing raw driver usage with the raw budget, which evicting an image does not change until
// its whole block empties, so a collector driven by it evicts without ever converging.
constexpr u64 AllocationAdjustedBudget(u64 budget, u64 block_bytes, u64 allocation_bytes) {
    if (budget == 0) {
        return 0; // Budget unavailable.
    }
    // A known but exhausted budget must not turn into the "unavailable" sentinel.
    return (std::max)(u64{1}, AllocationAdjustedBytes(budget, block_bytes, allocation_bytes));
}

constexpr u64 LimitBudgetForHostCommit(u64 device_budget, u64 device_usage,
                                       u64 available_commit, u64 reserve) {
    if (available_commit >= reserve) {
        const u64 headroom = available_commit - reserve;
        if (device_usage >= device_budget || headroom >= device_budget - device_usage) {
            return device_budget;
        }
        return device_usage + headroom;
    }
    const u64 shortage = reserve - available_commit;
    return shortage >= device_usage ? 0 : (std::min)(device_budget, device_usage - shortage);
}

} // namespace Vulkan
