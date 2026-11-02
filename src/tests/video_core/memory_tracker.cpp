// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <memory>
#include <stdexcept>
#include "common/container/unordered_map.h"
#include <tuple>
#include <vector>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/fence_download_ranges.h"
#include "video_core/nx2.h"
#include "core/device_memory.h"
#include "core/memory.h"
#include "video_core/host1x/gpu_device_memory_manager.h"

namespace {
using Range = std::pair<u64, u64>;

constexpr u64 PAGE = 4096;
constexpr u64 WORD = 4096 * 64;
constexpr u64 HIGH_PAGE_BITS = 22;
constexpr u64 HIGH_PAGE_SIZE = 1ULL << HIGH_PAGE_BITS;

constexpr DAddr c = 16 * HIGH_PAGE_SIZE;

class RasterizerInterface {
public:
    void UpdatePagesCachedCount(DAddr addr, size_t size, s32 delta) {
        ++update_calls;
        Apply(addr, size, delta);
    }

    void UpdatePagesCachedBatch(std::span<const std::pair<DAddr, size_t>> ranges, s32 delta) {
        ++update_calls;
        for (const auto& [addr, size] : ranges) {
            Apply(addr, size, delta);
        }
    }

    [[nodiscard]] size_t UpdateCalls() const noexcept { return update_calls; }
    [[nodiscard]] const std::vector<std::tuple<DAddr, u64, int>>& UpdateCallsList() const noexcept { return calls; }

    [[nodiscard]] int Count(DAddr addr) const noexcept {
        const auto it = page_table.find(addr >> Core::DEVICE_PAGEBITS);
        return it == page_table.end() ? 0 : it->second;
    }

    [[nodiscard]] unsigned Count() const noexcept {
        unsigned count = 0;
        for (const auto& [index, value] : page_table) {
            count += value;
        }
        return count;
    }

private:
    void Apply(DAddr addr, size_t size, s32 delta) {
        calls.emplace_back(addr, size, delta);
        const u64 page_start{addr >> Core::DEVICE_PAGEBITS};
        const u64 page_end{(addr + size + Core::DEVICE_PAGESIZE - 1) >> Core::DEVICE_PAGEBITS};
        for (u64 page = page_start; page < page_end; ++page) {
            int& value = page_table[page];
            value += delta;
            if (value == 0) {
                page_table.erase(page);
            } else if (value < 0) {
                throw std::logic_error{"negative page"};
            }
        }
    }

    ::Common::unordered_map<u64, int> page_table;
    std::vector<std::tuple<DAddr, u64, int>> calls;
    size_t update_calls = 0;
};

} // Anonymous namespace

using MemoryTracker = VideoCommon::MemoryTrackerBase<RasterizerInterface>;

TEST_CASE("MemoryTracker: Small region", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    REQUIRE(rasterizer.Count() == 0);
    memory_track->UnmarkRegionAsCpuModified(c, WORD);
    REQUIRE(rasterizer.Count() == WORD / PAGE);
    REQUIRE(memory_track->ModifiedCpuRegion(c, WORD) == Range{0, 0});

    memory_track->MarkRegionAsCpuModified(c + PAGE, 1);
    REQUIRE(memory_track->ModifiedCpuRegion(c, WORD) == Range{c + PAGE * 1, c + PAGE * 2});
}

TEST_CASE("MemoryTracker: Large region", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD * 32);
    memory_track->MarkRegionAsCpuModified(c + 4096, WORD * 4);
    REQUIRE(memory_track->ModifiedCpuRegion(c, WORD + PAGE * 2) == Range{c + PAGE, c + WORD + PAGE * 2});
    REQUIRE(memory_track->ModifiedCpuRegion(c + PAGE * 2, PAGE * 6) == Range{c + PAGE * 2, c + PAGE * 8});
    REQUIRE(memory_track->ModifiedCpuRegion(c, WORD * 32) == Range{c + PAGE, c + WORD * 4 + PAGE});
    REQUIRE(memory_track->ModifiedCpuRegion(c + WORD * 4, PAGE) == Range{c + WORD * 4, c + WORD * 4 + PAGE});
    REQUIRE(memory_track->ModifiedCpuRegion(c + WORD * 3 + PAGE * 63, PAGE) == Range{c + WORD * 3 + PAGE * 63, c + WORD * 4});

    memory_track->MarkRegionAsCpuModified(c + WORD * 5 + PAGE * 6, PAGE);
    memory_track->MarkRegionAsCpuModified(c + WORD * 5 + PAGE * 8, PAGE);
    REQUIRE(memory_track->ModifiedCpuRegion(c + WORD * 5, WORD) == Range{c + WORD * 5 + PAGE * 6, c + WORD * 5 + PAGE * 9});

    memory_track->UnmarkRegionAsCpuModified(c + WORD * 5 + PAGE * 8, PAGE);
    REQUIRE(memory_track->ModifiedCpuRegion(c + WORD * 5, WORD) == Range{c + WORD * 5 + PAGE * 6, c + WORD * 5 + PAGE * 7});

    memory_track->MarkRegionAsCpuModified(c + PAGE, WORD * 31 + PAGE * 63);
    REQUIRE(memory_track->ModifiedCpuRegion(c, WORD * 32) == Range{c + PAGE, c + WORD * 32});

    memory_track->UnmarkRegionAsCpuModified(c + PAGE * 4, PAGE);
    memory_track->UnmarkRegionAsCpuModified(c + PAGE * 6, PAGE);

    memory_track->UnmarkRegionAsCpuModified(c, WORD * 32);
    REQUIRE(memory_track->ModifiedCpuRegion(c, WORD * 32) == Range{0, 0});
}

TEST_CASE("MemoryTracker: Rasterizer counting", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    REQUIRE(rasterizer.Count() == 0);
    memory_track->UnmarkRegionAsCpuModified(c, PAGE);
    REQUIRE(rasterizer.Count() == 1);
    memory_track->MarkRegionAsCpuModified(c, PAGE * 2);
    REQUIRE(rasterizer.Count() == 0);
    memory_track->UnmarkRegionAsCpuModified(c, PAGE);
    memory_track->UnmarkRegionAsCpuModified(c + PAGE, PAGE);
    REQUIRE(rasterizer.Count() == 2);
    memory_track->MarkRegionAsCpuModified(c, PAGE * 2);
    REQUIRE(rasterizer.Count() == 0);
}

TEST_CASE("MemoryTracker: Basic range", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD);
    memory_track->MarkRegionAsCpuModified(c, PAGE);
    int num = 0;
    memory_track->ForEachUploadRange(c, WORD, [&](u64 offset, u64 size) {
        REQUIRE(offset == c);
        REQUIRE(size == PAGE);
        ++num;
    });
    REQUIRE(num == 1U);
}

TEST_CASE("MemoryTracker: Border upload", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD * 2);
    memory_track->MarkRegionAsCpuModified(c + WORD - PAGE, PAGE * 2);
    memory_track->ForEachUploadRange(c, WORD * 2, [](u64 offset, u64 size) {
        REQUIRE(offset == c + WORD - PAGE);
        REQUIRE(size == PAGE * 2);
    });
}

TEST_CASE("MemoryTracker: Border upload range", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD * 2);
    memory_track->MarkRegionAsCpuModified(c + WORD - PAGE, PAGE * 2);
    memory_track->ForEachUploadRange(c + WORD - PAGE, PAGE * 2, [](u64 offset, u64 size) {
        REQUIRE(offset == c + WORD - PAGE);
        REQUIRE(size == PAGE * 2);
    });
    memory_track->MarkRegionAsCpuModified(c + WORD - PAGE, PAGE * 2);
    memory_track->ForEachUploadRange(c + WORD - PAGE, PAGE, [](u64 offset, u64 size) {
        REQUIRE(offset == c + WORD - PAGE);
        REQUIRE(size == PAGE);
    });
    memory_track->ForEachUploadRange(c + WORD, PAGE, [](u64 offset, u64 size) {
        REQUIRE(offset == c + WORD);
        REQUIRE(size == PAGE);
    });
}

TEST_CASE("MemoryTracker: Border upload partial range", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD * 2);
    memory_track->MarkRegionAsCpuModified(c + WORD - PAGE, PAGE * 2);
    memory_track->ForEachUploadRange(c + WORD - 1, 2, [](u64 offset, u64 size) {
        REQUIRE(offset == c + WORD - PAGE);
        REQUIRE(size == PAGE * 2);
    });
    memory_track->MarkRegionAsCpuModified(c + WORD - PAGE, PAGE * 2);
    memory_track->ForEachUploadRange(c + WORD - 1, 1, [](u64 offset, u64 size) {
        REQUIRE(offset == c + WORD - PAGE);
        REQUIRE(size == PAGE);
    });
    memory_track->ForEachUploadRange(c + WORD + 50, 1, [](u64 offset, u64 size) {
        REQUIRE(offset == c + WORD);
        REQUIRE(size == PAGE);
    });
}

TEST_CASE("MemoryTracker: Partial word uploads", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    int num = 0;
    memory_track->ForEachUploadRange(c, WORD, [&](u64 offset, u64 size) {
        REQUIRE(offset == c);
        REQUIRE(size == WORD);
        ++num;
    });
    REQUIRE(num == 1);
    memory_track->ForEachUploadRange(c + WORD, WORD, [&](u64 offset, u64 size) {
        REQUIRE(offset == c + WORD);
        REQUIRE(size == WORD);
        ++num;
    });
    REQUIRE(num == 2);
    memory_track->ForEachUploadRange(c + 0x79000, 0x24000, [&](u64 offset, u64 size) {
        REQUIRE(offset == c + WORD * 2);
        REQUIRE(size == PAGE * 0x1d);
        ++num;
    });
    REQUIRE(num == 3);
}

TEST_CASE("MemoryTracker: Partial page upload", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD);
    int num = 0;
    memory_track->MarkRegionAsCpuModified(c + PAGE * 2, PAGE);
    memory_track->MarkRegionAsCpuModified(c + PAGE * 9, PAGE);
    memory_track->ForEachUploadRange(c, PAGE * 3, [&](u64 offset, u64 size) {
        REQUIRE(offset == c + PAGE * 2);
        REQUIRE(size == PAGE);
        ++num;
    });
    REQUIRE(num == 1);
    memory_track->ForEachUploadRange(c + PAGE * 7, PAGE * 3, [&](u64 offset, u64 size) {
        REQUIRE(offset == c + PAGE * 9);
        REQUIRE(size == PAGE);
        ++num;
    });
    REQUIRE(num == 2);
}

TEST_CASE("MemoryTracker: Partial page upload with multiple words on the right") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD * 9);
    memory_track->MarkRegionAsCpuModified(c + PAGE * 13, WORD * 7);
    int num = 0;
    memory_track->ForEachUploadRange(c + PAGE * 10, WORD * 7, [&](u64 offset, u64 size) {
        REQUIRE(offset == c + PAGE * 13);
        REQUIRE(size == WORD * 7 - PAGE * 3);
        ++num;
    });
    REQUIRE(num == 1);
    memory_track->ForEachUploadRange(c + PAGE, WORD * 8, [&](u64 offset, u64 size) {
        REQUIRE(offset == c + WORD * 7 + PAGE * 10);
        REQUIRE(size == PAGE * 3);
        ++num;
    });
    REQUIRE(num == 2);
}

TEST_CASE("MemoryTracker: Partial page upload with multiple words on the left", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD * 8);
    memory_track->MarkRegionAsCpuModified(c + PAGE * 13, WORD * 7);
    int num = 0;
    memory_track->ForEachUploadRange(c + PAGE * 16, WORD * 7, [&](u64 offset, u64 size) {
        REQUIRE(offset == c + PAGE * 16);
        REQUIRE(size == WORD * 7 - PAGE * 3);
        ++num;
    });
    REQUIRE(num == 1);
    memory_track->ForEachUploadRange(c + PAGE, WORD, [&](u64 offset, u64 size) {
        REQUIRE(offset == c + PAGE * 13);
        REQUIRE(size == PAGE * 3);
        ++num;
    });
    REQUIRE(num == 2);
}

TEST_CASE("MemoryTracker: Partial page upload with multiple words in the middle", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD * 8);
    memory_track->MarkRegionAsCpuModified(c + PAGE * 13, PAGE * 140);
    int num = 0;
    memory_track->ForEachUploadRange(c + PAGE * 16, WORD, [&](u64 offset, u64 size) {
        REQUIRE(offset == c + PAGE * 16);
        REQUIRE(size == WORD);
        ++num;
    });
    REQUIRE(num == 1);
    memory_track->ForEachUploadRange(c, WORD, [&](u64 offset, u64 size) {
        REQUIRE(offset == c + PAGE * 13);
        REQUIRE(size == PAGE * 3);
        ++num;
    });
    REQUIRE(num == 2);
    memory_track->ForEachUploadRange(c, WORD * 8, [&](u64 offset, u64 size) {
        REQUIRE(offset == c + WORD + PAGE * 16);
        REQUIRE(size == PAGE * 73);
        ++num;
    });
    REQUIRE(num == 3);
}

TEST_CASE("MemoryTracker: Empty right bits", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD * 2048);
    memory_track->MarkRegionAsCpuModified(c + WORD - PAGE, PAGE * 2);
    memory_track->ForEachUploadRange(c, WORD * 2048, [](u64 offset, u64 size) {
        REQUIRE(offset == c + WORD - PAGE);
        REQUIRE(size == PAGE * 2);
    });
}

TEST_CASE("MemoryTracker: Out of bound ranges 1", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c - WORD, 3 * WORD);
    memory_track->MarkRegionAsCpuModified(c, PAGE);
    REQUIRE(rasterizer.Count() == (3 * WORD - PAGE) / PAGE);
    int num = 0;
    memory_track->ForEachUploadRange(c - WORD, WORD, [&](u64 offset, u64 size) { ++num; });
    memory_track->ForEachUploadRange(c + WORD, WORD, [&](u64 offset, u64 size) { ++num; });
    memory_track->ForEachUploadRange(c - PAGE, PAGE, [&](u64 offset, u64 size) { ++num; });
    REQUIRE(num == 0);
    memory_track->ForEachUploadRange(c - PAGE, PAGE * 2, [&](u64 offset, u64 size) { ++num; });
    REQUIRE(num == 1);
    memory_track->MarkRegionAsCpuModified(c, WORD);
    REQUIRE(rasterizer.Count() == 2 * WORD / PAGE);
}

TEST_CASE("MemoryTracker: Out of bound ranges 2", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    REQUIRE_NOTHROW(memory_track->UnmarkRegionAsCpuModified(c + 0x22000, PAGE));
    REQUIRE_NOTHROW(memory_track->UnmarkRegionAsCpuModified(c + 0x28000, PAGE));
    REQUIRE(rasterizer.Count() == 2);
    REQUIRE_NOTHROW(memory_track->UnmarkRegionAsCpuModified(c + 0x21100, PAGE - 0x100));
    REQUIRE(rasterizer.Count() == 3);
    REQUIRE_NOTHROW(memory_track->UnmarkRegionAsCpuModified(c - PAGE, PAGE * 2));
    memory_track->UnmarkRegionAsCpuModified(c - PAGE * 3, PAGE * 2);
    memory_track->UnmarkRegionAsCpuModified(c - PAGE * 2, PAGE * 2);
    REQUIRE(rasterizer.Count() == 7);
}

TEST_CASE("MemoryTracker: Out of bound ranges 3", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, 0x310720);
    REQUIRE(rasterizer.Count(c) == 1);
    REQUIRE(rasterizer.Count(c + PAGE) == 1);
    REQUIRE(rasterizer.Count(c + WORD) == 1);
    REQUIRE(rasterizer.Count(c + WORD + PAGE) == 1);
}

TEST_CASE("MemoryTracker: Sparse regions 1", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD);
    memory_track->MarkRegionAsCpuModified(c + PAGE * 1, PAGE);
    memory_track->MarkRegionAsCpuModified(c + PAGE * 3, PAGE * 4);
    memory_track->ForEachUploadRange(c, WORD, [i = 0](u64 offset, u64 size) mutable {
        static constexpr std::array<u64, 2> offsets{c + PAGE, c + PAGE * 3};
        static constexpr std::array<u64, 2> sizes{PAGE, PAGE * 4};
        REQUIRE(offset == offsets.at(i));
        REQUIRE(size == sizes.at(i));
        ++i;
    });
}

TEST_CASE("MemoryTracker: Sparse regions 2", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, PAGE * 0x23);
    REQUIRE(rasterizer.Count() == 0x23);
    memory_track->MarkRegionAsCpuModified(c + PAGE * 0x1B, PAGE);
    memory_track->MarkRegionAsCpuModified(c + PAGE * 0x21, PAGE);
    memory_track->ForEachUploadRange(c, PAGE * 0x23, [i = 0](u64 offset, u64 size) mutable {
        static constexpr std::array<u64, 3> offsets{c + PAGE * 0x1B, c + PAGE * 0x21};
        static constexpr std::array<u64, 3> sizes{PAGE, PAGE};
        REQUIRE(offset == offsets.at(i));
        REQUIRE(size == sizes.at(i));
        ++i;
    });
}

TEST_CASE("MemoryTracker: Single page modified range", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    REQUIRE(memory_track->IsRegionCpuModified(c, PAGE));
    memory_track->UnmarkRegionAsCpuModified(c, PAGE);
    REQUIRE(!memory_track->IsRegionCpuModified(c, PAGE));
}

TEST_CASE("MemoryTracker: Two page modified range", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    REQUIRE(memory_track->IsRegionCpuModified(c, PAGE));
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE, PAGE));
    REQUIRE(memory_track->IsRegionCpuModified(c, PAGE * 2));
    memory_track->UnmarkRegionAsCpuModified(c, PAGE);
    REQUIRE(!memory_track->IsRegionCpuModified(c, PAGE));
}

TEST_CASE("MemoryTracker: Multi word modified ranges", "[video_core]") {
    for (int offset = 0; offset < 4; ++offset) {
        const DAddr address = c + WORD * offset;
        RasterizerInterface rasterizer;
        std::optional<MemoryTracker> memory_track(rasterizer);
        REQUIRE(memory_track->IsRegionCpuModified(address, PAGE));
        REQUIRE(memory_track->IsRegionCpuModified(address + PAGE * 48, PAGE));
        REQUIRE(memory_track->IsRegionCpuModified(address + PAGE * 56, PAGE));

        memory_track->UnmarkRegionAsCpuModified(address + PAGE * 32, PAGE);
        REQUIRE(memory_track->IsRegionCpuModified(address + PAGE, WORD));
        REQUIRE(memory_track->IsRegionCpuModified(address + PAGE * 31, PAGE));
        REQUIRE(!memory_track->IsRegionCpuModified(address + PAGE * 32, PAGE));
        REQUIRE(memory_track->IsRegionCpuModified(address + PAGE * 33, PAGE));
        REQUIRE(memory_track->IsRegionCpuModified(address + PAGE * 31, PAGE * 2));
        REQUIRE(memory_track->IsRegionCpuModified(address + PAGE * 32, PAGE * 2));

        memory_track->UnmarkRegionAsCpuModified(address + PAGE * 33, PAGE);
        REQUIRE(!memory_track->IsRegionCpuModified(address + PAGE * 32, PAGE * 2));
    }
}

TEST_CASE("MemoryTracker: Single page in large region", "[video_core]") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD * 16);
    REQUIRE(!memory_track->IsRegionCpuModified(c, WORD * 16));

    memory_track->MarkRegionAsCpuModified(c + WORD * 12 + PAGE * 8, PAGE);
    REQUIRE(memory_track->IsRegionCpuModified(c, WORD * 16));
    REQUIRE(!memory_track->IsRegionCpuModified(c + WORD * 10, WORD * 2));
    REQUIRE(memory_track->IsRegionCpuModified(c + WORD * 11, WORD * 2));
    REQUIRE(memory_track->IsRegionCpuModified(c + WORD * 12, WORD * 2));
    REQUIRE(memory_track->IsRegionCpuModified(c + WORD * 12 + PAGE * 4, PAGE * 8));
    REQUIRE(memory_track->IsRegionCpuModified(c + WORD * 12 + PAGE * 6, PAGE * 8));
    REQUIRE(!memory_track->IsRegionCpuModified(c + WORD * 12 + PAGE * 6, PAGE));
    REQUIRE(memory_track->IsRegionCpuModified(c + WORD * 12 + PAGE * 7, PAGE * 2));
    REQUIRE(memory_track->IsRegionCpuModified(c + WORD * 12 + PAGE * 8, PAGE * 2));
}

TEST_CASE("MemoryTracker: Wrap word regions") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD * 32);
    memory_track->MarkRegionAsCpuModified(c + PAGE * 63, PAGE * 2);
    REQUIRE(memory_track->IsRegionCpuModified(c, WORD * 2));
    REQUIRE(!memory_track->IsRegionCpuModified(c + PAGE * 62, PAGE));
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE * 63, PAGE));
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE * 64, PAGE));
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE * 63, PAGE * 2));
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE * 63, PAGE * 8));
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE * 60, PAGE * 8));

    REQUIRE(!memory_track->IsRegionCpuModified(c + PAGE * 127, WORD * 16));
    memory_track->MarkRegionAsCpuModified(c + PAGE * 127, PAGE);
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE * 127, WORD * 16));
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE * 127, PAGE));
    REQUIRE(!memory_track->IsRegionCpuModified(c + PAGE * 126, PAGE));
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE * 126, PAGE * 2));
    REQUIRE(!memory_track->IsRegionCpuModified(c + PAGE * 128, WORD * 16));
}

TEST_CASE("MemoryTracker: Unaligned page region query") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD);
    memory_track->MarkRegionAsCpuModified(c + 4000, 1000);
    REQUIRE(memory_track->IsRegionCpuModified(c, PAGE));
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE, PAGE));
    REQUIRE(memory_track->IsRegionCpuModified(c + 4000, 1000));
    REQUIRE(memory_track->IsRegionCpuModified(c + 4000, 1));
}

TEST_CASE("MemoryTracker: Cached write") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD);
    memory_track->CachedCpuWrite(c + PAGE, c + PAGE);
    REQUIRE(!memory_track->IsRegionCpuModified(c + PAGE, PAGE));
    memory_track->FlushCachedWrites();
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE, PAGE));
    memory_track->MarkRegionAsCpuModified(c, WORD);
    REQUIRE(rasterizer.Count() == 0);
}

TEST_CASE("MemoryTracker: Multiple cached write") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD);
    memory_track->CachedCpuWrite(c + PAGE, PAGE);
    memory_track->CachedCpuWrite(c + PAGE * 3, PAGE);
    REQUIRE(!memory_track->IsRegionCpuModified(c + PAGE, PAGE));
    REQUIRE(!memory_track->IsRegionCpuModified(c + PAGE * 3, PAGE));
    memory_track->FlushCachedWrites();
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE, PAGE));
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE * 3, PAGE));
    memory_track->MarkRegionAsCpuModified(c, WORD);
    REQUIRE(rasterizer.Count() == 0);
}

TEST_CASE("MemoryTracker: Cached write unmarked") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD);
    memory_track->CachedCpuWrite(c + PAGE, PAGE);
    memory_track->UnmarkRegionAsCpuModified(c + PAGE, PAGE);
    REQUIRE(!memory_track->IsRegionCpuModified(c + PAGE, PAGE));
    memory_track->FlushCachedWrites();
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE, PAGE));
    memory_track->MarkRegionAsCpuModified(c, WORD);
    REQUIRE(rasterizer.Count() == 0);
}

TEST_CASE("MemoryTracker: Cached write iterated") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD);
    memory_track->CachedCpuWrite(c + PAGE, PAGE);
    int num = 0;
    memory_track->ForEachUploadRange(c, WORD, [&](u64 offset, u64 size) { ++num; });
    REQUIRE(num == 0);
    REQUIRE(!memory_track->IsRegionCpuModified(c + PAGE, PAGE));
    memory_track->FlushCachedWrites();
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE, PAGE));
    memory_track->MarkRegionAsCpuModified(c, WORD);
    REQUIRE(rasterizer.Count() == 0);
}

TEST_CASE("MemoryTracker: Cached write downloads") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD);
    REQUIRE(rasterizer.Count() == 64);
    memory_track->CachedCpuWrite(c + PAGE, PAGE);
    REQUIRE(rasterizer.Count() == 63);
    memory_track->MarkRegionAsGpuModified(c + PAGE, PAGE);
    int num = 0;
    memory_track->ForEachDownloadRangeAndClear(c, WORD, [&](u64 offset, u64 size) { ++num; });
    REQUIRE(num == 0);
    num = 0;
    memory_track->ForEachUploadRange(c, WORD, [&](u64 offset, u64 size) { ++num; });
    REQUIRE(num == 0);
    REQUIRE(!memory_track->IsRegionCpuModified(c + PAGE, PAGE));
    REQUIRE(memory_track->IsRegionGpuModified(c + PAGE, PAGE));
    memory_track->FlushCachedWrites();
    REQUIRE(memory_track->IsRegionCpuModified(c + PAGE, PAGE));
    REQUIRE(!memory_track->IsRegionGpuModified(c + PAGE, PAGE));
    memory_track->MarkRegionAsCpuModified(c, WORD);
    REQUIRE(rasterizer.Count() == 0);
}

TEST_CASE("MemoryTracker: FlushCachedWrites batching") {
    RasterizerInterface rasterizer;
    std::optional<MemoryTracker> memory_track(rasterizer);
    memory_track->UnmarkRegionAsCpuModified(c, WORD * 2);
    memory_track->CachedCpuWrite(c + PAGE, PAGE);
    memory_track->CachedCpuWrite(c + PAGE * 2, PAGE);
    memory_track->CachedCpuWrite(c + PAGE * 4, PAGE);
    // Cached writes stop tracking their pages right away; tracking them again leaves the
    // pending cached writes for FlushCachedWrites to report
    REQUIRE(rasterizer.Count() == WORD * 2 / PAGE - 3);
    memory_track->UnmarkRegionAsCpuModified(c, WORD * 2);
    REQUIRE(rasterizer.Count() == WORD * 2 / PAGE);
    const size_t calls_before_flush = rasterizer.UpdateCalls();
    const size_t ranges_before_flush = rasterizer.UpdateCallsList().size();
    memory_track->FlushCachedWrites();
    // Now we expect a single batch call (coalesced ranges) to the device memory manager
    REQUIRE(rasterizer.UpdateCalls() == calls_before_flush + 1);
    const auto& calls = rasterizer.UpdateCallsList();
    REQUIRE(calls.size() == ranges_before_flush + 2);
    REQUIRE(calls[ranges_before_flush] == std::tuple<DAddr, u64, int>{c + PAGE, PAGE * 2, -1});
    REQUIRE(calls[ranges_before_flush + 1] == std::tuple<DAddr, u64, int>{c + PAGE * 4, PAGE, -1});
    REQUIRE(rasterizer.Count() == WORD * 2 / PAGE - 3);
}

TEST_CASE("DeviceMemoryManager: UpdatePagesCachedBatch basic") {
    Core::DeviceMemory device_memory;
    Tegra::MaxwellDeviceMemoryManager manager(device_memory);
    // empty should be a no-op
    std::vector<std::pair<DAddr, size_t>> empty;
    manager.UpdatePagesCachedBatch(empty, 1);

    // small ranges should be accepted and not crash
    std::vector<std::pair<DAddr, size_t>> ranges;
    ranges.emplace_back(0, Core::Memory::YUZU_PAGESIZE);
    ranges.emplace_back(Core::Memory::YUZU_PAGESIZE, Core::Memory::YUZU_PAGESIZE);
    manager.UpdatePagesCachedBatch(ranges, 1);
    SUCCEED("UpdatePagesCachedBatch executed without error");
}

TEST_CASE("MemoryTracker: repeated indirect rewrites keep historical command-page tracking",
          "[video_core][nx2_command_sync][mkw_sync]") {
    Tegra::Nx2ClearGpuWrittenPages();
    RasterizerInterface rasterizer;
    auto tracker = std::make_unique<MemoryTracker>(rasterizer);
    tracker->UnmarkRegionAsCpuModified(c, PAGE);
    const auto translate = [](GPUVAddr address) -> std::optional<DAddr> {
        return c + address - 0x8600000000ULL;
    };
    REQUIRE_FALSE(Tegra::Nx2HasGpuWrittenPage(0x8600000000ULL, 5 * sizeof(u32), translate));
    for (u32 frame = 0; frame < 3; ++frame) {
        tracker->MarkRegionAsGpuModified(c, 5 * sizeof(u32));
        Tegra::Nx2MarkGpuWritten(c, 5 * sizeof(u32));
        REQUIRE(Tegra::Nx2HasGpuWrittenPage(0x8600000000ULL, 5 * sizeof(u32), translate));
        REQUIRE(tracker->IsRegionGpuModified(c, 5 * sizeof(u32)));
        std::vector<Range> downloads;
        tracker->ForEachDownloadRangeAndClear(c, PAGE, [&](u64 address, u64 size) {
            downloads.emplace_back(address, size);
        });
        REQUIRE(downloads == std::vector<Range>{{c, PAGE}});
        REQUIRE_FALSE(tracker->IsRegionGpuModified(c, 5 * sizeof(u32)));
        REQUIRE(Tegra::Nx2HasGpuWrittenPage(0x8600000000ULL, 5 * sizeof(u32), translate));
        downloads.clear();
        tracker->ForEachDownloadRangeAndClear(c, PAGE, [&](u64 address, u64 size) {
            downloads.emplace_back(address, size);
        });
        REQUIRE(downloads.empty());
    }
    tracker->MarkRegionAsCpuModified(c, PAGE);
    REQUIRE(rasterizer.Count() == 0);
}

TEST_CASE("MemoryTracker: preflushable enumeration clips pages and preserves sparse state",
          "[video_core][buffer_readback]") {
    RasterizerInterface rasterizer;
    auto tracker = std::make_unique<MemoryTracker>(rasterizer);
    const u64 boundary = c + HIGH_PAGE_SIZE;
    tracker->MarkRegionAsPreflushable(c + PAGE, PAGE);
    tracker->MarkRegionAsPreflushable(boundary - PAGE, 2 * PAGE);
    const auto enumerate = [&](u64 address, u64 size) {
        std::vector<Range> ranges;
        tracker->ForEachPreflushableRange(address, size, [&](u64 begin, u64 length) {
            ranges.emplace_back(begin, length);
        });
        return ranges;
    };
    for (int repeat = 0; repeat < 3; ++repeat) {
        REQUIRE(enumerate(c + PAGE + 7, HIGH_PAGE_SIZE) ==
                std::vector<Range>{{c + PAGE + 7, PAGE - 7},
                                   {boundary - PAGE, PAGE}, {boundary, PAGE}});
        REQUIRE(enumerate(boundary - 7, 14) ==
                std::vector<Range>{{boundary - 7, 7}, {boundary, 7}});
        REQUIRE(enumerate(c + 2 * PAGE, PAGE).empty());
        REQUIRE(enumerate(c + 4 * HIGH_PAGE_SIZE, PAGE).empty());
        REQUIRE(enumerate(c, 0).empty());
        REQUIRE(tracker->IsRegionPreflushable(c, 8 * 1024 * 1024));
        REQUIRE_FALSE(tracker->IsRegionFullyPreflushable(c, 8 * 1024 * 1024));
        REQUIRE(tracker->IsRegionFullyPreflushable(boundary - 7, 14));
        REQUIRE_FALSE(tracker->IsRegionFullyPreflushable(boundary - PAGE - 1, 2));
    }
    tracker->UnmarkRegionAsPreflushable(boundary, PAGE);
    REQUIRE_FALSE(tracker->IsRegionFullyPreflushable(boundary - 7, 14));
    REQUIRE(rasterizer.UpdateCalls() == 0);
}

TEST_CASE("Buffer readback: one CPU-visible page does not download an entire native window",
          "[video_core][buffer_readback]") {
    RasterizerInterface rasterizer;
    auto tracker = std::make_unique<MemoryTracker>(rasterizer);
    constexpr u64 window = 8 * 1024 * 1024;
    tracker->UnmarkRegionAsCpuModified(c, window);
    tracker->MarkRegionAsPreflushable(c + PAGE, PAGE);
    Common::RangeSet<DAddr> cpu_reads;
    Common::RangeSet<DAddr> downloads;
    cpu_reads.Add(c + PAGE + 10, 20); // Already included by the preflushable page.
    cpu_reads.Add(c + 7 * PAGE + 3, 13);
    cpu_reads.Add(c + window + PAGE, PAGE); // Outside this write.
    for (int frame = 0; frame < 3; ++frame) {
        downloads.Clear();
        tracker->MarkRegionAsGpuModified(c, window);
        REQUIRE(VideoCommon::QueueFenceDownloadRanges(false, c, window, *tracker,
                                                       cpu_reads, downloads));
        std::vector<Range> ranges;
        downloads.ForEach([&](DAddr begin, DAddr end) { ranges.emplace_back(begin, end - begin); });
        REQUIRE(ranges == std::vector<Range>{{c + PAGE, PAGE}, {c + 7 * PAGE + 3, 13}});
        REQUIRE(tracker->IsRegionGpuModified(c + 2 * PAGE, PAGE));
        REQUIRE(tracker->IsRegionFullyPreflushable(c + PAGE, PAGE));
        REQUIRE_FALSE(tracker->IsRegionFullyPreflushable(c + PAGE, 2 * PAGE));
        tracker->UnmarkRegionAsGpuModified(c + PAGE, PAGE);
        // Enumerating fence ranges must not clear dirty or preflushable state elsewhere.
        REQUIRE(tracker->IsRegionGpuModified(c + 2 * PAGE, PAGE));
    }
}

TEST_CASE("Buffer readback: full-download policy and fully covered writes retain eager visibility",
          "[video_core][buffer_readback]") {
    RasterizerInterface rasterizer;
    auto tracker = std::make_unique<MemoryTracker>(rasterizer);
    Common::RangeSet<DAddr> cpu_reads;
    Common::RangeSet<DAddr> downloads;
    REQUIRE_FALSE(VideoCommon::QueueFenceDownloadRanges(true, c + 7, 3 * PAGE, *tracker,
                                                         cpu_reads, downloads));
    std::vector<Range> ranges;
    downloads.ForEach([&](DAddr begin, DAddr end) { ranges.emplace_back(begin, end - begin); });
    REQUIRE(ranges == std::vector<Range>{{c + 7, 3 * PAGE}});
    downloads.Clear();
    tracker->MarkRegionAsPreflushable(c, 2 * PAGE);
    REQUIRE_FALSE(VideoCommon::QueueFenceDownloadRanges(false, c + 7, PAGE, *tracker,
                                                         cpu_reads, downloads));
    ranges.clear();
    downloads.ForEach([&](DAddr begin, DAddr end) { ranges.emplace_back(begin, end - begin); });
    REQUIRE(ranges == std::vector<Range>{{c + 7, PAGE}});
    downloads.Clear();
    REQUIRE_FALSE(VideoCommon::QueueFenceDownloadRanges(false, c, 0, *tracker, cpu_reads, downloads));
    REQUIRE(downloads.Empty());
    REQUIRE(VideoCommon::QueueFenceDownloadRanges(false, c + 4 * PAGE, PAGE, *tracker,
                                                  cpu_reads, downloads));
    REQUIRE(downloads.Empty());
}

TEST_CASE("Buffer readback: final copies preserve disjoint selections on a dirty page",
          "[video_core][buffer_readback]") {
    RasterizerInterface rasterizer;
    auto tracker = std::make_unique<MemoryTracker>(rasterizer);
    tracker->UnmarkRegionAsCpuModified(c, PAGE);
    tracker->MarkRegionAsGpuModified(c, PAGE);
    Common::RangeSet<DAddr> gpu_modified;
    gpu_modified.Add(c, PAGE);
    Common::RangeSet<DAddr> cpu_reads;
    cpu_reads.Add(c + 3, 13);
    cpu_reads.Add(c + 101, 7);
    Common::RangeSet<DAddr> selected;
    REQUIRE(VideoCommon::QueueFenceDownloadRanges(false, c, PAGE, *tracker, cpu_reads, selected));

    // Exercise selection through the same final-copy enumerator the fence builder uses.
    for (int repeat = 0; repeat < 3; ++repeat) {
        std::vector<Range> copies;
        selected.ForEach([&](DAddr begin, DAddr end) {
            VideoCommon::ForEachFenceDownloadRange(begin, end - begin, *tracker, gpu_modified,
                [&](DAddr first, DAddr last) { copies.emplace_back(first, last - first); });
        });
        REQUIRE(copies == std::vector<Range>{{c + 3, 13}, {c + 101, 7}});
        REQUIRE(tracker->IsRegionGpuModified(c, PAGE));
        REQUIRE(gpu_modified.Intersects(c + 1000, 1));
    }
}

TEST_CASE("Buffer readback: final copies intersect buffer bounds and byte-level dirtiness",
          "[video_core][buffer_readback]") {
    RasterizerInterface rasterizer;
    auto tracker = std::make_unique<MemoryTracker>(rasterizer);
    const DAddr boundary = c + HIGH_PAGE_SIZE;
    tracker->UnmarkRegionAsCpuModified(boundary - PAGE, 3 * PAGE);
    tracker->MarkRegionAsGpuModified(boundary - PAGE, 2 * PAGE);
    Common::RangeSet<DAddr> gpu_modified;
    gpu_modified.Add(boundary - PAGE, PAGE - 2);
    gpu_modified.Add(boundary + 2, 2 * PAGE - 2);
    std::vector<Range> copies;
    const auto append = [&](DAddr first, DAddr last) { copies.emplace_back(first, last - first); };
    // This interval represents a selected range clipped to a host buffer at both ends.
    VideoCommon::ForEachFenceDownloadRange(boundary - 7, 14, *tracker, gpu_modified, append);
    REQUIRE(copies == std::vector<Range>{{boundary - 7, 5}, {boundary + 2, 5}});
    copies.clear();
    VideoCommon::ForEachFenceDownloadRange(boundary + PAGE, PAGE, *tracker, gpu_modified, append);
    REQUIRE(copies.empty()); // Byte-level dirtiness alone does not bypass page tracking.
    VideoCommon::ForEachFenceDownloadRange(boundary, 0, *tracker, gpu_modified, append);
    REQUIRE(copies.empty());
    REQUIRE(tracker->IsRegionGpuModified(boundary - PAGE, PAGE));
    REQUIRE(gpu_modified.Intersects(boundary + 8, 1));
}

TEST_CASE("Buffer readback: full and selective final copies match a byte reference",
          "[video_core][buffer_readback]") {
    constexpr u64 size = 3 * PAGE;
    constexpr DAddr address = c + WORD - PAGE;
    RasterizerInterface rasterizer;
    auto tracker = std::make_unique<MemoryTracker>(rasterizer);
    tracker->UnmarkRegionAsCpuModified(address, size);
    tracker->MarkRegionAsGpuModified(address, size);
    tracker->MarkRegionAsPreflushable(address + PAGE, PAGE);
    Common::RangeSet<DAddr> cpu_reads;
    Common::RangeSet<DAddr> gpu_modified;
    std::array<bool, size> requested{};
    std::array<bool, size> dirty{};
    for (u64 offset = 0; offset < size; ++offset) {
        requested[offset] = (offset >= PAGE && offset < 2 * PAGE) || offset % 101 < 17;
        dirty[offset] = offset % 79 < 53;
        if (offset % 101 < 17) {
            cpu_reads.Add(address + offset, 1);
        }
        if (dirty[offset]) {
            gpu_modified.Add(address + offset, 1);
        }
    }
    for (const bool full : {false, true}) {
        Common::RangeSet<DAddr> selected;
        VideoCommon::QueueFenceDownloadRanges(full, address + 3, size - 10, *tracker,
                                             cpu_reads, selected);
        std::array<unsigned, size> visits{};
        selected.ForEach([&](DAddr begin, DAddr end) {
            VideoCommon::ForEachFenceDownloadRange(begin, end - begin, *tracker, gpu_modified,
                [&](DAddr first, DAddr last) {
                    REQUIRE(first >= begin);
                    REQUIRE(last <= end);
                    for (DAddr byte = first; byte < last; ++byte) {
                        ++visits[byte - address];
                    }
                });
        });
        for (u64 offset = 0; offset < size; ++offset) {
            const bool expected = offset >= 3 && offset < size - 7 && dirty[offset] &&
                                  (full || requested[offset]);
            REQUIRE(visits[offset] == (expected ? 1U : 0U));
        }
    }
}

TEST_CASE("Buffer readback: selected pages match a reference across word and manager boundaries",
          "[video_core][buffer_readback]") {
    RasterizerInterface rasterizer;
    auto tracker = std::make_unique<MemoryTracker>(rasterizer);
    constexpr u64 pages = 2048;
    constexpr u64 start = c - 13 * PAGE;
    std::array<bool, pages> preflushable{};
    std::array<bool, pages> cpu_visible{};
    u32 random = 0x19fef00d;
    Common::RangeSet<DAddr> cpu_reads;
    for (u64 page = 0; page < pages; ++page) {
        random = random * 1664525U + 1013904223U;
        preflushable[page] = (random & 31) == 0;
        cpu_visible[page] = (random & 15) == 1;
        if (preflushable[page]) tracker->MarkRegionAsPreflushable(start + page * PAGE, PAGE);
        if (cpu_visible[page]) cpu_reads.Add(start + page * PAGE, PAGE);
    }
    for (u64 query = 0; query < 40; ++query) {
        const u64 first = query * 41;
        const u64 count = (std::min<u64>)(pages - first, 97 + query);
        const u64 address = start + first * PAGE + 17;
        const u64 size = count * PAGE - 31;
        Common::RangeSet<DAddr> downloads;
        const bool needs_traps = VideoCommon::QueueFenceDownloadRanges(
            false, address, size, *tracker, cpu_reads, downloads);
        u64 expected_bytes = 0;
        bool all_preflushable = true;
        for (u64 page = first; page < first + count; ++page) {
            const u64 begin = (std::max)(address, start + page * PAGE);
            const u64 end = (std::min)(address + size, start + (page + 1) * PAGE);
            const bool expected = preflushable[page] || cpu_visible[page];
            REQUIRE(downloads.Intersects(begin, end - begin) == expected);
            if (expected) expected_bytes += end - begin;
            all_preflushable &= preflushable[page];
        }
        u64 actual_bytes = 0;
        downloads.ForEach([&](DAddr begin, DAddr end) {
            REQUIRE(begin >= address);
            REQUIRE(end <= address + size);
            actual_bytes += end - begin;
        });
        REQUIRE(actual_bytes == expected_bytes);
        REQUIRE(needs_traps == !all_preflushable);
        REQUIRE(tracker->IsRegionFullyPreflushable(address, size) == all_preflushable);
    }
}
