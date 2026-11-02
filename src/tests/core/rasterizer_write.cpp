// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <deque>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "core/memory/rasterizer_write.h"
#include "video_core/buffer_cache/fence_download_ranges.h"

TEST_CASE("Every CPU write protects its bytes from pending fence snapshots",
          "[core][rasterizer_write]") {
    constexpr DAddr base = 0x10000;
    struct Copy { u64 src_offset, dst_offset, size; };
    const auto run = [&](bool legacy_page_skip) {
        std::array<u8, 64> guest{}, staged{};
        staged.fill(1);
        std::deque<std::vector<Copy>> copies{{{base, 0, 64}}};
        Common::OverlapRangeSet<DAddr> pending;
        Common::RangeSet<DAddr> uncommitted, dirty;
        std::deque<Common::RangeSet<DAddr>> committed;
        pending.Add(base, 64);
        dirty.Add(base, 64);
        unsigned protections = 0, collections = 0;
        const auto protect = [&](DAddr at, size_t size) {
            ++protections;
            VideoCommon::DropFenceDownloads(at, size, copies, pending, uncommitted, committed);
            return true;
        };
        const auto collect = [&](DAddr, size_t) { ++collections; };
        Core::Memory::RecordRasterizerWrite(base, 4, protect, collect);
        std::fill_n(guest.begin(), 4, 9);
        if (legacy_page_skip) {
            collect(base + 8, 4);
        } else {
            Core::Memory::RecordRasterizerWrite(base + 8, 4, protect, collect);
        }
        std::fill_n(guest.begin() + 8, 4, 7);
        for (const auto& copy : copies.front()) {
            VideoCommon::WriteBackFenceCopy(
                copy.src_offset, copy.size, staged.data() + copy.dst_offset, pending,
                uncommitted, committed, dirty, [&](DAddr at, const u8* data, u64 size) {
                    std::copy_n(data, size, guest.begin() + (at - base));
                });
        }
        CHECK(guest[0] == 9);
        CHECK(collections == 2);
        CHECK(protections == (legacy_page_skip ? 1 : 2));
        CHECK(pending.Empty());
        return guest;
    };
    // Reproduce the previous same-page bypass, then verify the production write helper.
    CHECK(run(true)[8] == 1);
    CHECK(run(false)[8] == 7);
}

TEST_CASE("Repeated writes and aliases do not cache a protection decision",
          "[core][rasterizer_write]") {
    unsigned checks = 0, collects = 0;
    const auto protect = [&](DAddr, size_t) { return ++checks != 2; };
    const auto collect = [&](DAddr, size_t) { ++collects; };
    for (int i = 0; i < 3; ++i) {
        Core::Memory::RecordRasterizerWrite(0x20000, 4, protect, collect);
    }
    CHECK(checks == 3);
    CHECK(collects == 2);
    Core::Memory::RecordRasterizerWrite(0, 4, protect, collect);
    Core::Memory::RecordRasterizerWrite(0x20000, 0, protect, collect);
    CHECK(checks == 3);
}
