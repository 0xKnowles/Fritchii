// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

/// Where an image's memory lies: its device memory block (0 for none or a dedicated allocation),
/// that block's size and the image's part of it.
struct ImageMemoryBlock {
    u64 block{};
    u64 block_size{};
    u64 size{};
};

// Ported from official Eglantine c0cb3325 (e37b4b9). Deleting purely by age leaves every
// device-memory block partly used: new images land in the holes, so blocks seldom empty and the
// driver keeps counting them (MKW here: VMA heap usage 9.0-9.7 GiB over an 8.8 GiB budget while
// the collector deleted thousands of images per 600 frames). Among already age-eligible
// candidates (oldest first), those in the emptiest blocks are offered first, so blocks can drain
// and return to the driver. Ties keep their age order; images without a known shared block sit
// in the middle. This only reorders candidates: eligibility and the per-pass budget are unchanged.
template <typename Id, typename BlockOf>
[[nodiscard]] std::vector<Id> OrderByBlockFill(std::span<const Id> candidates,
                                               const std::unordered_map<u64, u64>& block_bytes,
                                               BlockOf&& block_of) {
    std::vector<std::pair<double, Id>> ordered;
    ordered.reserve(candidates.size());
    for (const Id id : candidates) {
        const ImageMemoryBlock block = block_of(id);
        double fill = 0.5;
        if (block.block != 0 && block.block_size != 0) {
            const auto it = block_bytes.find(block.block);
            fill = it != block_bytes.end()
                       ? static_cast<double>(it->second) / static_cast<double>(block.block_size)
                       : 0.0;
        }
        ordered.emplace_back(fill, id);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Id> result;
    result.reserve(ordered.size());
    for (const auto& entry : ordered) {
        result.push_back(entry.second);
    }
    return result;
}

} // namespace VideoCommon
