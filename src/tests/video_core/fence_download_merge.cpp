// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <deque>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "video_core/buffer_cache/fence_download_ranges.h"

namespace {
using Set = Common::RangeSet<DAddr>;
using Range = std::pair<DAddr, DAddr>;

auto Enumerate(const Set& set) {
    std::vector<Range> result;
    set.ForEach([&](DAddr begin, DAddr end) { result.emplace_back(begin, end); });
    return result;
}
} // namespace

TEST_CASE("Fence download merge handles empty, touching, duplicate and disjoint ranges",
          "[video_core][buffer_readback][fence_merge]") {
    std::deque<Set> sets;
    REQUIRE(VideoCommon::MergeFenceDownloadRanges(sets).Empty());
    sets.emplace_back().Add(100, 10);
    sets.emplace_back().Add(110, 10);
    sets.emplace_back().Add(103, 5);
    sets.emplace_back().Add(200, 3);
    sets.emplace_back();
    const auto merged = VideoCommon::MergeFenceDownloadRanges(sets);
    REQUIRE(sets.empty());
    REQUIRE(Enumerate(merged) == std::vector<Range>{{100, 120}, {200, 203}});
    sets.emplace_back().Add(1ULL << 40, 7);
    const auto single = VideoCommon::MergeFenceDownloadRanges(sets);
    REQUIRE(Enumerate(single) == std::vector<Range>{{1ULL << 40, (1ULL << 40) + 7}});
    REQUIRE(sets.empty());
}

TEST_CASE("Fence union copies exactly the bytes selected by last-write deduplication",
          "[video_core][buffer_readback][fence_merge]") {
    constexpr DAddr base = 1ULL << 36;
    u32 seed = 0x71935;
    const auto random = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };
    for (size_t trial = 0; trial < 100; ++trial) {
        std::deque<Set> original, candidate;
        for (size_t batch = 0; batch < 1 + trial % 32; ++batch) {
            auto& old = original.emplace_back();
            auto& next = candidate.emplace_back();
            for (size_t n = 0; n < 8; ++n) {
                const auto offset = random() % 480;
                const auto size = 1 + random() % 32;
                old.Add(base + offset, size);
                next.Add(base + offset, size);
            }
        }
        // Retain the former algorithm as a byte-coverage oracle, including its
        // fragmented ownership across multiple accumulated fence sets.
        for (size_t i = 0; i < original.size(); ++i) {
            for (size_t j = i + 1; j < original.size(); ++j) {
                original[j].ForEach(
                    [&](DAddr begin, DAddr end) { original[i].Subtract(begin, end - begin); });
            }
        }
        std::array<u32, 512> expected{}, actual{};
        const auto count = [&](const Set& set, auto& bytes) {
            set.ForEach([&](DAddr begin, DAddr end) {
                for (auto at = begin; at < end; ++at)
                    ++bytes.at(at - base);
            });
        };
        for (const auto& set : original)
            count(set, expected);
        const auto merged = VideoCommon::MergeFenceDownloadRanges(candidate);
        count(merged, actual);
        REQUIRE(candidate.empty());
        REQUIRE(actual == expected);
        for (const auto copies : actual)
            REQUIRE(copies <= 1);
    }
}
