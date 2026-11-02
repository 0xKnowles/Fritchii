// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/assert.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace VideoCommon {

template <typename T, typename Id = u32>
class DescriptorTable {
public:
    [[nodiscard]] bool Synchronize(GPUVAddr gpu_addr, u32 limit) noexcept {
        bool ret = !(current_gpu_addr == gpu_addr && current_limit == limit);
        if (ret) {
            Refresh(gpu_addr, limit);
        }
        return ret;
    }

    void Invalidate() noexcept {
        // Deleting many images invalidates every channel repeatedly. Clear once on reuse,
        // not once per eviction. Descriptor contents are still read from guest memory each time.
        invalidated = true;
    }

    [[nodiscard]] std::pair<T, bool> Read(Tegra::MemoryManager const& gpu_memory, u32 index) noexcept {
        DEBUG_ASSERT(index <= current_limit);
        const GPUVAddr gpu_addr = current_gpu_addr + index * sizeof(T);
        std::pair<T, bool> result;
        gpu_memory.ReadBlockUnsafe(gpu_addr, std::addressof(result.first), sizeof(T));
        result.second = UpdateDescriptor(index, result.first);
        return result;
    }

    // Return whether a freshly read descriptor needs its binding resolved again.
    [[nodiscard]] bool UpdateDescriptor(u32 index, const T& descriptor) noexcept {
        DEBUG_ASSERT(index <= current_limit);
        if (invalidated) {
            std::ranges::fill(read_descriptors, 0);
            invalidated = false;
        }
        auto& bits = read_descriptors[index / 64];
        const u64 mask = 1ULL << (index % 64);
        const bool changed = (bits & mask) == 0 || descriptor != descriptors[index];
        bits |= mask;
        if (changed) {
            descriptors[index] = descriptor;
        }
        return changed;
    }

    void Refresh(GPUVAddr gpu_addr, u32 limit) noexcept {
        current_gpu_addr = gpu_addr;
        current_limit = limit;
        // Geometric growth retains amortized allocation without a 16 MiB minimum per table.
        // Widen before adding so an inclusive u32 limit cannot wrap.
        const size_t num_descriptors = std::bit_ceil(static_cast<size_t>(limit) + 1);
        size_t old_size = read_descriptors.size();
        read_descriptors.resize(Common::DivCeil(num_descriptors, 64U));
        old_size = (std::min)(old_size, read_descriptors.size());
        std::fill(read_descriptors.begin(), read_descriptors.begin() + old_size, 0);
        invalidated = false;
        if (descriptors.size() < num_descriptors) {
            descriptors.resize(num_descriptors);
            bound_ids.resize(num_descriptors, Id{0});
        }
    }

    std::vector<u64> read_descriptors;
    std::vector<T> descriptors;
    std::vector<Id> bound_ids;
    GPUVAddr current_gpu_addr{};
    u32 current_limit{};
    bool invalidated{};
};

} // namespace VideoCommon
