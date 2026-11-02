// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <deque>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/container/unordered_map.h"
#include "video_core/buffer_cache/fence_download_ranges.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/host1x/gpu_device_memory_manager.h"

namespace {
using Set = Common::RangeSet<DAddr>;
using Overlap = Common::OverlapRangeSet<DAddr>;
using Range = std::pair<DAddr, DAddr>;

constexpr u64 PAGE = 4096;
constexpr DAddr BASE = 16ULL << 22;

class AuditRasterizer {
public:
    void UpdatePagesCachedCount(DAddr addr, size_t size, s32 delta) {
        Apply(addr, size, delta);
    }
    void UpdatePagesCachedBatch(std::span<const std::pair<DAddr, size_t>> ranges, s32 delta) {
        for (const auto& [addr, size] : ranges) {
            Apply(addr, size, delta);
        }
    }

private:
    void Apply(DAddr addr, size_t size, s32 delta) {
        for (u64 page = addr >> 12; page < (addr + size + PAGE - 1) >> 12; ++page) {
            int& value = pages[page];
            value += delta;
            if (value < 0) {
                throw std::logic_error{"negative page"};
            }
        }
    }
    Common::unordered_map<u64, int> pages;
};

using AuditTracker = VideoCommon::MemoryTrackerBase<AuditRasterizer>;

std::vector<Range> Enumerate(const Set& set) {
    std::vector<Range> result;
    set.ForEach([&](DAddr begin, DAddr end) { result.emplace_back(begin, end); });
    return result;
}

template <typename T>
void MoveAssign(T& target, T& source) {
    target = std::move(source);
}

struct Random {
    u32 state;
    u32 operator()() {
        state = state * 1664525U + 1013904223U;
        return state >> 8;
    }
};
} // namespace

TEST_CASE("Readback audit: moved range sets are empty, reusable and assignable",
          "[video_core][readback_audit]") {
    Set a;
    a.Add(100, 10);
    Set b{std::move(a)};
    REQUIRE(a.Empty());
    REQUIRE(Enumerate(b) == std::vector<Range>{{100, 110}});

    // BufferCache::AccumulateFlushes hands its uncommitted set over by moving and then keeps
    // adding to it: nothing handed over may reappear in the source.
    std::deque<Set> committed;
    a.Add(200, 5);
    committed.emplace_back(std::move(a));
    REQUIRE(a.Empty());
    a.Add(300, 1);
    REQUIRE(Enumerate(a) == std::vector<Range>{{300, 301}});
    REQUIRE(Enumerate(committed.front()) == std::vector<Range>{{200, 205}});

    Set c;
    c.Add(1, 1);
    Set& result = (c = std::move(b));
    REQUIRE(&result == &c);
    REQUIRE(b.Empty());
    REQUIRE(Enumerate(c) == std::vector<Range>{{100, 110}});
    MoveAssign(c, c);
    REQUIRE(Enumerate(c) == std::vector<Range>{{100, 110}});

    Overlap pending;
    pending.Add(100, 10);
    pending.Add(105, 10);
    Overlap moved{std::move(pending)};
    REQUIRE(pending.Empty());
    std::vector<std::tuple<DAddr, DAddr, s32>> counts;
    moved.ForEach([&](DAddr begin, DAddr end, s32 count) { counts.emplace_back(begin, end, count); });
    REQUIRE(counts == std::vector<std::tuple<DAddr, DAddr, s32>>{
                          {100, 105, 1}, {105, 110, 2}, {110, 115, 1}});
    Overlap assigned;
    assigned.Add(1, 1);
    Overlap& assigned_result = (assigned = std::move(moved));
    REQUIRE(&assigned_result == &assigned);
    REQUIRE(moved.Empty());
    MoveAssign(assigned, assigned);
    counts.clear();
    assigned.ForEach(
        [&](DAddr begin, DAddr end, s32 count) { counts.emplace_back(begin, end, count); });
    REQUIRE(counts.size() == 3);
}

TEST_CASE("Readback audit: Preflushable-only eager selection equals the former duplicated set",
          "[video_core][readback_audit]") {
    // GetFlushArea marked exactly the pages NoteCpuRead recorded (same page-aligned range, same
    // condition), so the CPU-read set was always the Preflushable pages: model that, then check
    // both selection forms agree byte for byte, including the read-trap result.
    AuditRasterizer rasterizer;
    auto tracker = std::make_unique<AuditTracker>(rasterizer);
    constexpr u64 pages = 4096; // Crosses word and 4 MiB manager boundaries.
    const DAddr start = BASE - 37 * PAGE;
    tracker->UnmarkRegionAsCpuModified(start, pages * PAGE);
    Set cpu_reads;
    Random random{0x5eed1234};
    for (int read = 0; read < 300; ++read) {
        const u64 first = random() % pages;
        const u64 address = start + first * PAGE + random() % PAGE;
        const u64 size = 1 + random() % (3 * PAGE);
        const DAddr aligned_begin = Common::AlignDown(address, PAGE);
        const DAddr aligned_end = Common::AlignUp(address + size, PAGE);
        cpu_reads.Add(aligned_begin, aligned_end - aligned_begin);
        tracker->MarkRegionAsPreflushable(aligned_begin, aligned_end - aligned_begin);
    }
    for (int write = 0; write < 400; ++write) {
        const u64 first = random() % (pages - 1);
        const DAddr address = start + first * PAGE + random() % PAGE;
        const u64 limit = start + pages * PAGE - address;
        const u64 size = write % 50 == 0 ? 0 : 1 + random() % (std::min<u64>)(limit, 600 * PAGE);
        const bool download_all = write % 37 == 0;
        Set former;
        Set current;
        const bool former_traps = VideoCommon::QueueFenceDownloadRanges(
            download_all, address, size, *tracker, cpu_reads, former);
        const bool current_traps = VideoCommon::QueueFenceDownloadRanges(
            download_all, address, size, *tracker, current);
        REQUIRE(former_traps == current_traps);
        REQUIRE(Enumerate(former) == Enumerate(current));
        current.ForEach([&](DAddr begin, DAddr end) {
            REQUIRE(begin >= address);
            REQUIRE(end <= address + size);
        });
    }
}

TEST_CASE("Readback audit: a CPU write after a fence commit survives that fence's writeback",
          "[video_core][readback_audit]") {
    Overlap pending;
    Set uncommitted;
    std::deque<Set> committed;
    Set gpu_modified;
    std::array<u8, 64> guest{};
    std::array<u8, 64> staged{};
    staged.fill(0xA1);
    const auto write = [&](DAddr address, const u8* source, u64 size) {
        std::copy_n(source, size, guest.begin() + (address - BASE));
    };

    // The GPU writes 64 bytes; a fence commits their copy.
    gpu_modified.Add(BASE, 64);
    pending.Add(BASE, 64);
    // With fast GPU fences the guest already saw the fence and now writes 8 of those bytes.
    std::fill_n(guest.begin() + 8, 8, u8{0xC0});
    VideoCommon::DropFenceDownloads(BASE + 8, 8, pending, uncommitted, committed);
    const u64 written = VideoCommon::WriteBackFenceCopy(
        BASE, 64, staged.data(), pending, uncommitted, committed, gpu_modified, write);
    REQUIRE(written == 56);
    for (size_t i = 0; i < guest.size(); ++i) {
        REQUIRE(guest[i] == (i >= 8 && i < 16 ? 0xC0 : 0xA1));
    }
    REQUIRE(pending.Empty());
    // The CPU bytes are still GPU-modified until CachedWriteMemory copies them in.
    REQUIRE(Enumerate(gpu_modified) == std::vector<Range>{{BASE + 8, BASE + 16}});

    // Selections not yet committed lose those bytes as well.
    uncommitted.Add(BASE, 64);
    committed.emplace_back().Add(BASE + 32, 32);
    VideoCommon::DropFenceDownloads(BASE + 40, 4, pending, uncommitted, committed);
    REQUIRE(Enumerate(uncommitted) == std::vector<Range>{{BASE, BASE + 40}, {BASE + 44, BASE + 64}});
    REQUIRE(Enumerate(committed.front()) ==
            std::vector<Range>{{BASE + 32, BASE + 40}, {BASE + 44, BASE + 64}});
    VideoCommon::DropFenceDownloads(BASE, 0, pending, uncommitted, committed);
    REQUIRE(Enumerate(uncommitted).size() == 2);
}

TEST_CASE("Readback audit: overlapping fence copies retire GPU modification after the last",
          "[video_core][readback_audit]") {
    Overlap pending;
    Set uncommitted;
    std::deque<Set> committed;
    Set gpu_modified;
    std::array<u8, 32> guest{};
    std::array<u8, 32> first{};
    std::array<u8, 32> second{};
    first.fill(1);
    second.fill(2);
    const auto write = [&](DAddr address, const u8* source, u64 size) {
        std::copy_n(source, size, guest.begin() + (address - BASE));
    };
    gpu_modified.Add(BASE, 32);
    pending.Add(BASE, 32); // Fence 1.
    pending.Add(BASE + 16, 16); // Fence 2, rewritten half.
    REQUIRE(VideoCommon::WriteBackFenceCopy(BASE, 32, first.data(), pending, uncommitted,
                                            committed, gpu_modified, write) == 32);
    REQUIRE(Enumerate(gpu_modified) == std::vector<Range>{{BASE + 16, BASE + 32}});
    // Written again by the GPU since fence 2: still GPU-modified after its writeback.
    uncommitted.Add(BASE + 24, 4);
    REQUIRE(VideoCommon::WriteBackFenceCopy(BASE + 16, 16, second.data() + 16, pending,
                                            uncommitted, committed, gpu_modified, write) == 16);
    REQUIRE(pending.Empty());
    REQUIRE(Enumerate(gpu_modified) == std::vector<Range>{{BASE + 16, BASE + 32}});
    for (size_t i = 0; i < guest.size(); ++i) {
        REQUIRE(guest[i] == (i < 16 ? 1 : 2));
    }
    REQUIRE(VideoCommon::WriteBackFenceCopy(BASE, 0, first.data(), pending, uncommitted,
                                            committed, gpu_modified, write) == 0);
}

TEST_CASE("Readback audit: random fence, CPU write and inline sequences stay coherent",
          "[video_core][readback_audit]") {
    // Byte model of the buffer cache's eager-download bookkeeping (every GPU write eager):
    // GPU writes, SignalOrdering accumulation, commits, fence releases, guest CPU writes (dropped
    // from pending copies at once, copied into the GPU buffer later as CachedWriteMemory does).
    constexpr size_t N = 384;
    for (u32 seed = 1; seed <= 200; ++seed) {
        Random random{seed * 7919U};
        Overlap pending;
        Set uncommitted;
        std::deque<Set> committed;
        Set gpu_modified;
        std::array<u8, N> guest{};
        std::array<u8, N> gpu{};
        std::array<u64, N> cpu_write_time{};
        struct Fence {
            u64 commit_time;
            std::vector<Range> copies;
            std::array<u8, N> staged;
        };
        std::deque<Fence> fences;
        std::vector<Range> pending_inlines;
        u64 time = 1;
        u8 token = 1;
        const auto random_range = [&] {
            const DAddr begin = random() % N;
            const DAddr end = begin + 1 + random() % (std::min<u64>)(N - begin, 48);
            return Range{BASE + begin, BASE + end};
        };
        const auto commit = [&] {
            if (!uncommitted.Empty()) {
                committed.emplace_back(std::move(uncommitted));
            }
            if (committed.empty()) {
                return;
            }
            const Set selected = VideoCommon::MergeFenceDownloadRanges(committed);
            Fence fence{time, {}, gpu};
            selected.ForEach([&](DAddr begin, DAddr end) {
                gpu_modified.ForEachInRange(begin, end - begin, [&](DAddr b, DAddr e) {
                    fence.copies.emplace_back(b, e);
                    pending.Add(b, e - b);
                });
            });
            if (!fence.copies.empty()) {
                fences.push_back(std::move(fence));
            }
        };
        const auto release = [&] {
            Fence& fence = fences.front();
            for (const auto [begin, end] : fence.copies) {
                std::vector<bool> written(end - begin);
                const auto write = [&](DAddr address, const u8* source, u64 size) {
                    for (u64 i = 0; i < size; ++i) {
                        const size_t at = address + i - BASE;
                        // Never over a CPU write made after this fence's copy was taken.
                        REQUIRE(cpu_write_time[at] < fence.commit_time);
                        guest[at] = source[i];
                        written[address + i - begin] = true;
                    }
                };
                VideoCommon::WriteBackFenceCopy(begin, end - begin,
                                                fence.staged.data() + (begin - BASE), pending,
                                                uncommitted, committed, gpu_modified, write);
                for (DAddr at = begin; at < end; ++at) {
                    // Everything else of the copy reaches guest memory.
                    REQUIRE(written[at - begin] == (cpu_write_time[at - BASE] < fence.commit_time));
                }
            }
            fences.pop_front();
        };
        const auto inline_cpu_writes = [&] {
            for (const auto [begin, end] : pending_inlines) {
                // InlineMemoryImplementation: guest bytes into the GPU buffer, no longer dirty.
                VideoCommon::DropFenceDownloads(begin, end - begin, pending, uncommitted,
                                                committed);
                gpu_modified.Subtract(begin, end - begin);
                for (DAddr at = begin; at < end; ++at) {
                    gpu[at - BASE] = guest[at - BASE];
                }
            }
            pending_inlines.clear();
        };
        for (int step = 0; step < 300; ++step) {
            ++time;
            switch (random() % 6) {
            case 0:
            case 1: {
                const auto [begin, end] = random_range();
                ++token;
                for (DAddr at = begin; at < end; ++at) {
                    gpu[at - BASE] = token;
                }
                gpu_modified.Add(begin, end - begin);
                uncommitted.Add(begin, end - begin);
                break;
            }
            case 2:
                if (random() % 2 == 0 && !uncommitted.Empty()) {
                    committed.emplace_back(std::move(uncommitted));
                    REQUIRE(uncommitted.Empty());
                } else {
                    commit();
                }
                break;
            case 3:
                if (!fences.empty()) {
                    release();
                }
                break;
            case 4: {
                const auto [begin, end] = random_range();
                ++token;
                for (DAddr at = begin; at < end; ++at) {
                    guest[at - BASE] = token;
                    cpu_write_time[at - BASE] = time;
                }
                // BufferCache::OnCPUWrite on a GPU-modified page.
                VideoCommon::DropFenceDownloads(begin, end - begin, pending, uncommitted,
                                                committed);
                pending_inlines.emplace_back(begin, end);
                break;
            }
            default:
                inline_cpu_writes();
                break;
            }
        }
        inline_cpu_writes();
        ++time;
        commit();
        while (!fences.empty()) {
            release();
        }
        REQUIRE(pending.Empty());
        REQUIRE(gpu_modified.Empty());
        REQUIRE(guest == gpu);
    }
}
