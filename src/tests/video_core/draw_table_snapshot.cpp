// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <stdexcept>
#include <catch2/catch_test_macros.hpp>
#include "video_core/draw_table_snapshot.h"

TEST_CASE("Draw-local table snapshots reuse owned bytes only within one draw",
          "[video_core][draw_table_snapshot]") {
    VideoCommon::DrawTableSnapshotCache cache;
    std::array<u8, 16> guest{};
    for (size_t i = 0; i < guest.size(); ++i) guest[i] = static_cast<u8>(i);
    unsigned loads = 0;
    const auto loader = [&](std::span<u8> out) {
        ++loads;
        // A table can span two device-mapped segments.
        std::copy_n(guest.begin(), 8, out.begin());
        std::copy_n(guest.begin() + 8, 8, out.begin() + 8);
    };
    cache.BeginDraw();
    const auto first = cache.Read(1, 0x8000, 16, loader);
    REQUIRE_FALSE(first.reused);
    REQUIRE(std::equal(first.bytes.begin(), first.bytes.end(), guest.begin()));
    const auto second_descriptor = cache.Read(1, 0x8000, 16, loader);
    REQUIRE(second_descriptor.reused);
    REQUIRE(loads == 1);
    REQUIRE(second_descriptor.bytes[3] == 3);
    REQUIRE(second_descriptor.bytes[11] == 11); // Different descriptor fields share one snapshot.
    guest[3] = 99;
    REQUIRE(second_descriptor.bytes[3] == 3); // Not a retained guest host pointer.
    cache.BeginDraw();
    const auto next_draw = cache.Read(1, 0x8000, 16, loader);
    REQUIRE_FALSE(next_draw.reused);
    REQUIRE(loads == 2);
    REQUIRE(next_draw.bytes[3] == 99);
}

TEST_CASE("Draw-local table keys include address space base and extent",
          "[video_core][draw_table_snapshot]") {
    VideoCommon::DrawTableSnapshotCache cache;
    unsigned loads = 0;
    const auto loader = [&](std::span<u8> out) { std::ranges::fill(out, ++loads); };
    cache.BeginDraw();
    REQUIRE_FALSE(cache.Read(1, 0x8000, 16, loader).reused);
    REQUIRE_FALSE(cache.Read(2, 0x8000, 16, loader).reused);
    REQUIRE_FALSE(cache.Read(1, 0x9000, 16, loader).reused);
    REQUIRE_FALSE(cache.Read(1, 0x8000, 8, loader).reused);
    const auto original = cache.Read(1, 0x8000, 16, loader);
    REQUIRE(original.reused);
    REQUIRE(original.bytes.size() == 16);
    REQUIRE(original.bytes[0] == 1);
    REQUIRE(loads == 4);
}

TEST_CASE("Draw-local snapshot failures and capacity limits never reuse invalid bytes",
          "[video_core][draw_table_snapshot]") {
    VideoCommon::DrawTableSnapshotCache cache;
    cache.BeginDraw();
    REQUIRE_THROWS_AS(cache.Read(1, 0x1000, 8, [](std::span<u8> bytes) {
        bytes[0] = 99;
        throw std::runtime_error("Unmapped table segment");
    }), std::runtime_error);
    unsigned loads = 0;
    const auto loader = [&](std::span<u8> out) { std::ranges::fill(out, ++loads); };
    const auto retry = cache.Read(1, 0x1000, 8, loader);
    REQUIRE_FALSE(retry.reused);
    REQUIRE(retry.bytes[0] == 1);
    for (size_t i = 1; i < VideoCommon::DrawTableSnapshotCache::MaxTables; ++i) {
        REQUIRE_FALSE(cache.Read(1, 0x1000 + i * 4096, 8, loader).reused);
    }
    REQUIRE_FALSE(cache.Read(1, 0x100000, 16, loader).reused);
    REQUIRE_FALSE(cache.Read(1, 0x100000, 16, loader).reused); // Bounded fallback refreshes.
    REQUIRE(loads == VideoCommon::DrawTableSnapshotCache::MaxTables + 2);
    const auto first = cache.Read(1, 0x1000, 8, loader);
    REQUIRE(first.reused);
    REQUIRE(first.bytes[0] == 1);
    cache.BeginDraw();
    REQUIRE_FALSE(cache.Read(1, 0x1000, 8, loader).reused);
}
