// SPDX-License-Identifier: GPL-3.0-or-later
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/block_aware_eviction.h"

using VideoCommon::ImageMemoryBlock;

TEST_CASE("Block-aware eviction offers the emptiest blocks first", "[block_aware_eviction]") {
    // Candidates arrive oldest first. Block 1 is 90% full, block 2 10%, block 3 50%.
    const std::unordered_map<u64, u64> bytes{{1, 90}, {2, 10}, {3, 50}};
    const std::unordered_map<u32, ImageMemoryBlock> blocks{
        {10, {1, 100, 30}}, {11, {2, 100, 10}}, {12, {3, 100, 20}}, {13, {2, 100, 0}}};
    const std::vector<u32> candidates{10, 11, 12, 13};
    const auto ordered = VideoCommon::OrderByBlockFill<u32>(
        candidates, bytes, [&](u32 id) { return blocks.at(id); });
    // Ties (11 and 13 share block 2) keep their age order.
    CHECK(ordered == std::vector<u32>{11, 13, 12, 10});
}

TEST_CASE("Block-aware eviction keeps every candidate exactly once", "[block_aware_eviction]") {
    const std::unordered_map<u64, u64> bytes{{7, 64}};
    std::vector<u32> candidates;
    for (u32 id = 0; id < 200; ++id) {
        candidates.push_back(id);
    }
    const auto ordered = VideoCommon::OrderByBlockFill<u32>(
        candidates, bytes, [](u32 id) -> ImageMemoryBlock {
            switch (id % 4) {
            case 0:
                return {7, 128, 1}; // 50% fill
            case 1:
                return {0, 128, 1}; // dedicated: middle
            case 2:
                return {9, 128, 1}; // unknown block: counted empty
            default:
                return {7, 0, 1}; // unknown size: middle
            }
        });
    REQUIRE(ordered.size() == candidates.size());
    std::vector<bool> seen(candidates.size());
    for (const u32 id : ordered) {
        REQUIRE_FALSE(seen[id]);
        seen[id] = true;
    }
    // Unknown (absent) blocks are empty and go first, still oldest first among themselves.
    CHECK(ordered.front() == 2);
    CHECK(ordered[1] == 6);
}

TEST_CASE("Block-aware eviction without block data is the age order", "[block_aware_eviction]") {
    const std::unordered_map<u64, u64> bytes;
    const std::vector<u32> candidates{5, 3, 9, 1};
    const auto ordered = VideoCommon::OrderByBlockFill<u32>(
        candidates, bytes, [](u32) { return ImageMemoryBlock{}; });
    CHECK(ordered == candidates);
}
