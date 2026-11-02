// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/memory/read_traps.h"
#include "core/memory/readback_cache.h"
#include "video_core/buffer_cache/fence_download_ranges.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/readback_coverage.h"

namespace {
constexpr u64 Page = Core::Memory::YUZU_PAGESIZE;
constexpr DAddr Base = 0x8000;

struct PageTracker {
    void UpdatePagesCachedCount(DAddr, size_t, s32) {}
    void UpdatePagesCachedBatch(std::span<const std::pair<DAddr, u64>>, s32) {}
};
using Tracker = VideoCommon::MemoryTrackerBase<PageTracker>;

bool Contains(const VideoCore::RasterizerDownloadArea& area, DAddr address, u64 size) {
    return area.start_address <= address && address + size <= area.end_address;
}
} // namespace

TEST_CASE("Readback reuse coverage cannot widen an observed CPU read to a texture",
          "[video_core][readback_coverage]") {
    const VideoCore::RasterizerDownloadArea flushed{Base, Base + 4 * Page, true, 17};
    const auto coverage = VideoCommon::LimitReadbackReuseCoverage(flushed, Base + Page + 3, 4);
    REQUIRE(coverage.start_address == Base + Page);
    REQUIRE(coverage.end_address == Base + 2 * Page);
    REQUIRE(coverage.preemtive);
    REQUIRE(coverage.completed_generation == 17);
    REQUIRE(flushed.start_address == Base);
    REQUIRE(flushed.end_address == Base + 4 * Page); // Actual texture flush stays wide.
    const auto crossing = VideoCommon::LimitReadbackReuseCoverage(flushed, Base + Page - 1, 2);
    REQUIRE(crossing.start_address == Base);
    REQUIRE(crossing.end_address == Base + 2 * Page);
    REQUIRE_FALSE(Contains(coverage, Base + 2 * Page, 4));
}

TEST_CASE("Readback reuse coverage preserves completion proof without inventing bytes",
          "[video_core][readback_coverage]") {
    const VideoCore::RasterizerDownloadArea partial{Base + 1, Base + Page - 1, true, 19};
    const auto coverage = VideoCommon::LimitReadbackReuseCoverage(partial, Base + 3, 4);
    REQUIRE(coverage.start_address == partial.start_address);
    REQUIRE(coverage.end_address == partial.end_address);
    REQUIRE(coverage.completed_generation == 19);
    for (const auto [address, size] : std::array<std::pair<DAddr, u64>, 4>{{
             {Base, 0}, {~u64{0} - 1, 4}, {~u64{0} - 1, 1}, {Base + 2 * Page, 4}}}) {
        const auto empty = VideoCommon::LimitReadbackReuseCoverage(partial, address, size);
        REQUIRE(empty.start_address == empty.end_address);
        REQUIRE(empty.completed_generation == 0);
    }
    auto uncertified = partial;
    uncertified.completed_generation = 0;
    REQUIRE(VideoCommon::LimitReadbackReuseCoverage(uncertified, Base + 3, 4)
                .completed_generation == 0);
}

TEST_CASE("Readback coverage makes unseen pages notify before their traps are disarmed",
          "[video_core][readback_coverage][buffer_readback]") {
    PageTracker page_tracker;
    auto tracker = std::make_unique<Tracker>(page_tracker);
    tracker->UnmarkRegionAsCpuModified(Base, 3 * Page);
    tracker->MarkRegionAsGpuModified(Base, 3 * Page);
    Core::Memory::ReadTraps traps;
    const u64 generation = traps.NewGeneration();
    for (u64 index = 0; index < 3; ++index) {
        traps.Arm(Base / Page + index, generation);
    }
    Common::RangeSet<DAddr> cpu_reads;
    std::array<VideoCore::RasterizerDownloadArea, 2> core_areas{};
    std::array<unsigned, 3> gpu{10, 20, 30};
    std::array<unsigned, 3> cpu{};
    std::vector<std::pair<DAddr, u64>> flushed_ranges;
    unsigned ordered_reads = 0;
    const auto read = [&](size_t core, size_t page) {
        const DAddr address = Base + page * Page;
        if (!Contains(core_areas[core], address, 4)) {
            ++ordered_reads;
            // The rasterizer observes only this CPU page, but flushing the aliased
            // texture still covers every page before the reuse area is narrowed.
            cpu_reads.Add(address, Page);
            tracker->MarkRegionAsPreflushable(address, Page);
            VideoCore::RasterizerDownloadArea actual{Base, Base + 3 * Page, true, 1};
            flushed_ranges.emplace_back(actual.start_address,
                                         actual.end_address - actual.start_address);
            cpu = gpu;
            core_areas[core] = VideoCommon::LimitReadbackReuseCoverage(actual, address, 4);
        }
        traps.Disarm(address / Page, address / Page + 1, generation, [](u64) {});
        return cpu[page];
    };
    REQUIRE(read(0, 0) == 10);
    REQUIRE(ordered_reads == 1);
    REQUIRE(tracker->IsRegionFullyPreflushable(Base, Page));
    REQUIRE_FALSE(tracker->IsRegionPreflushable(Base + Page, 2 * Page));
    std::vector<u64> armed;
    traps.ForEach(Base / Page, Base / Page + 3, [&](u64 page) { armed.push_back(page); });
    REQUIRE(armed == std::vector<u64>{Base / Page + 1, Base / Page + 2});

    // B changes after A's wide texture readback. It must not hit A's reused area.
    gpu[1] = 21;
    size_t reader_core = 0;
    SECTION("the same CPU core reads B") {}
    SECTION("another CPU core reads B") { reader_core = 1; }
    REQUIRE(read(reader_core, 1) == 21);
    REQUIRE(ordered_reads == 2);
    REQUIRE(tracker->IsRegionFullyPreflushable(Base + Page, Page));
    REQUIRE_FALSE(tracker->IsRegionPreflushable(Base + 2 * Page, Page));
    REQUIRE(flushed_ranges == std::vector<std::pair<DAddr, u64>>{
                                  {Base, 3 * Page}, {Base, 3 * Page}});

    // Having genuinely observed B, later GPU writes must retain eager fence visibility.
    gpu[1] = 22;
    tracker->MarkRegionAsGpuModified(Base + Page, Page);
    Common::RangeSet<DAddr> queued;
    Common::RangeSet<DAddr> dirty;
    dirty.Add(Base + Page, Page);
    REQUIRE_FALSE(VideoCommon::QueueFenceDownloadRanges(false, Base + Page, Page,
                                                         *tracker, cpu_reads, queued));
    u64 copied = 0;
    queued.ForEach([&](DAddr begin, DAddr end) {
        VideoCommon::ForEachFenceDownloadRange(begin, end - begin, *tracker, dirty,
            [&](DAddr first, DAddr last) {
                REQUIRE(first == Base + Page);
                REQUIRE(last == Base + 2 * Page);
                copied += last - first;
                cpu[1] = gpu[1];
            });
    });
    REQUIRE(copied == Page);
    REQUIRE(read(reader_core, 1) == 22);
    REQUIRE(ordered_reads == 2);
}

TEST_CASE("Readback host certificates cannot reuse an unobserved neighbor",
          "[video_core][readback_coverage]") {
    int owner{};
    std::array<u8, 2 * Page> backing{};
    using Core::Memory::HostReadbackCache;
    HostReadbackCache::Page a{&owner, backing.data(), Base, 3, 1};
    HostReadbackCache::Page b{&owner, backing.data() + Page, Base + Page, 3, 1};
    const VideoCore::RasterizerDownloadArea flushed{Base, Base + 2 * Page, true, 17};
    const auto a_coverage = VideoCommon::LimitReadbackReuseCoverage(flushed, Base + 7, 4);
    HostReadbackCache cache;
    cache.Remember(a, a_coverage, a, 1, 17);
    REQUIRE(cache.CanReuse(a, 1, 17, 8, 4));
    REQUIRE_FALSE(cache.CanReuse(b, 1, 17, 8, 4));
    cache.Remember(b, a_coverage, b, 1, 17);
    REQUIRE_FALSE(cache.CanReuse(b, 1, 17, 8, 4));
    const auto b_coverage = VideoCommon::LimitReadbackReuseCoverage(flushed, Base + Page, Page);
    cache.Remember(b, b_coverage, b, 1, 17);
    REQUIRE(cache.CanReuse(b, 1, 17, 8, 4));
    REQUIRE_FALSE(cache.CanReuse(b, 1, 18, 8, 4)); // A newly published GPU write invalidates proof.
}
