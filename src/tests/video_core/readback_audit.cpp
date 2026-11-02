// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <span>
#include <stdexcept>
#include <tuple>
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

/// Same fields as VideoCommon::BufferCopy: src_offset is the device address, dst_offset the
/// staging offset.
struct Copy {
    u64 src_offset;
    u64 dst_offset;
    size_t size;
    bool operator==(const Copy&) const = default;
};
using CopyList = std::vector<Copy>;
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
    std::deque<CopyList> fences;
    std::array<u8, 64> guest{};
    std::array<u8, 64> staged{};
    staged.fill(0xA1);
    const auto write = [&](DAddr address, const u8* source, u64 size) {
        std::copy_n(source, size, guest.begin() + (address - BASE));
    };

    // The GPU writes 64 bytes; a fence commits their copy.
    gpu_modified.Add(BASE, 64);
    fences.push_back({{BASE, 0, 64}});
    pending.Add(BASE, 64);
    // With fast GPU fences the guest already saw the fence and now writes 8 of those bytes.
    std::fill_n(guest.begin() + 8, 8, u8{0xC0});
    VideoCommon::DropFenceDownloads(BASE + 8, 8, fences, pending, uncommitted, committed);
    REQUIRE(fences.front() == CopyList{{BASE, 0, 8}, {BASE + 16, 16, 48}});
    u64 written = 0;
    for (const Copy& copy : fences.front()) {
        written += VideoCommon::WriteBackFenceCopy(copy.src_offset, copy.size,
                                                   staged.data() + copy.dst_offset, pending,
                                                   uncommitted, committed, gpu_modified, write);
    }
    fences.pop_front();
    REQUIRE(written == 56);
    for (size_t i = 0; i < guest.size(); ++i) {
        REQUIRE(guest[i] == (i >= 8 && i < 16 ? 0xC0 : 0xA1));
    }
    REQUIRE(pending.Empty());
    // The CPU bytes are still GPU-modified until CachedWriteMemory copies them in.
    REQUIRE(Enumerate(gpu_modified) == std::vector<Range>{{BASE + 8, BASE + 16}});

    // Head, tail and whole-copy cuts; other fences and disjoint copies are untouched.
    fences.push_back({{BASE, 0, 16}, {BASE + 32, 16, 16}});
    fences.push_back({{BASE + 4, 0, 8}});
    pending.Add(BASE, 16);
    pending.Add(BASE + 32, 16);
    pending.Add(BASE + 4, 8);
    VideoCommon::DropFenceDownloads(BASE + 12, 24, fences, pending, uncommitted, committed);
    REQUIRE(fences[0] == CopyList{{BASE, 0, 12}, {BASE + 36, 20, 12}});
    REQUIRE(fences[1] == CopyList{{BASE + 4, 0, 8}});
    VideoCommon::DropFenceDownloads(BASE, 64, fences, pending, uncommitted, committed);
    REQUIRE(fences[0].empty());
    REQUIRE(fences[1].empty());
    REQUIRE(pending.Empty());
    fences.clear();

    // Selections not yet committed lose those bytes as well.
    uncommitted.Add(BASE, 64);
    committed.emplace_back().Add(BASE + 32, 32);
    VideoCommon::DropFenceDownloads(BASE + 40, 4, fences, pending, uncommitted, committed);
    REQUIRE(Enumerate(uncommitted) ==
            std::vector<Range>{{BASE, BASE + 40}, {BASE + 44, BASE + 64}});
    REQUIRE(Enumerate(committed.front()) ==
            std::vector<Range>{{BASE + 32, BASE + 40}, {BASE + 44, BASE + 64}});
    VideoCommon::DropFenceDownloads(BASE, 0, fences, pending, uncommitted, committed);
    REQUIRE(Enumerate(uncommitted).size() == 2);
}

TEST_CASE("Readback audit: a dropped older fence copy cannot claim a newer fence's bytes",
          "[video_core][readback_audit]") {
    // GPU write, fence 1, CPU write (drop), GPU write, fence 2. Dropping only the shared counts
    // left fence 1's copy to find fence 2's count: it wrote its stale bytes over the CPU write,
    // retired their GPU modification, and fence 2 then had nothing left to write.
    Overlap pending;
    Set uncommitted;
    std::deque<Set> committed;
    Set gpu_modified;
    std::deque<CopyList> fences;
    std::array<u8, 16> guest{};
    std::array<u8, 16> first{};
    std::array<u8, 16> second{};
    first.fill(1);
    second.fill(2);
    const auto release = [&](const std::array<u8, 16>& staged) {
        for (const Copy& copy : fences.front()) {
            VideoCommon::WriteBackFenceCopy(
                copy.src_offset, copy.size, staged.data() + copy.dst_offset, pending, uncommitted,
                committed, gpu_modified, [&](DAddr address, const u8* source, u64 size) {
                    std::copy_n(source, size, guest.begin() + (address - BASE));
                });
        }
        fences.pop_front();
    };
    gpu_modified.Add(BASE, 16);
    fences.push_back({{BASE, 0, 16}});
    pending.Add(BASE, 16);
    guest.fill(9);
    VideoCommon::DropFenceDownloads(BASE, 16, fences, pending, uncommitted, committed);
    gpu_modified.Add(BASE, 16);
    fences.push_back({{BASE, 0, 16}});
    pending.Add(BASE, 16);
    release(first);
    REQUIRE(guest == std::array<u8, 16>{9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9});
    REQUIRE(Enumerate(gpu_modified) == std::vector<Range>{{BASE, BASE + 16}});
    release(second);
    REQUIRE(guest == second);
    REQUIRE(gpu_modified.Empty());
    REQUIRE(pending.Empty());
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
    pending.Add(BASE, 32);      // Fence 1.
    pending.Add(BASE + 16, 16); // Fence 2, rewritten half.
    REQUIRE(VideoCommon::WriteBackFenceCopy(BASE, 32, first.data(), pending, uncommitted,
                                            committed, gpu_modified, write) == 32);
    REQUIRE(Enumerate(gpu_modified) == std::vector<Range>{{BASE + 16, BASE + 32}});
    // Written again by the GPU since fence 2: still GPU-modified after its writeback.
    uncommitted.Add(BASE + 24, 4);
    REQUIRE(VideoCommon::WriteBackFenceCopy(BASE + 16, 16, second.data() + 16, pending,
                                            uncommitted, committed, gpu_modified, write) == 16);
    REQUIRE(pending.Empty());
    REQUIRE(Enumerate(gpu_modified) == std::vector<Range>{{BASE + 24, BASE + 28}});
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
        CAPTURE(seed);
        Random random{seed * 7919U};
        Overlap pending;
        Set uncommitted;
        std::deque<Set> committed;
        Set gpu_modified;
        std::array<u8, N> guest{};
        std::array<u8, N> gpu{};
        std::array<u64, N> drop_time{};
        std::array<u64, N> cpu_write_time{};
        struct Fence {
            u64 commit_time;
            CopyList original;
            std::array<u8, N> staged;
        };
        std::deque<Fence> fences;
        std::deque<CopyList> fence_copies; // As BufferCache::pending_downloads.
        std::vector<Range> pending_inlines;
        u64 time = 1;
        u8 token = 1;
        const auto random_range = [&] {
            const DAddr begin = random() % N;
            const DAddr end = begin + 1 + random() % (std::min<u64>)(N - begin, 48);
            return Range{BASE + begin, BASE + end};
        };
        const auto drop = [&](DAddr begin, DAddr end) {
            VideoCommon::DropFenceDownloads(begin, end - begin, fence_copies, pending,
                                            uncommitted, committed);
            for (DAddr at = begin; at < end; ++at) {
                drop_time[at - BASE] = time;
            }
        };
        const auto check_counts = [&] {
            std::array<s32, N> expected{};
            for (const CopyList& copies : fence_copies) {
                for (const Copy& copy : copies) {
                    for (u64 at = copy.src_offset; at < copy.src_offset + copy.size; ++at) {
                        ++expected[at - BASE];
                    }
                }
            }
            std::array<s32, N> actual{};
            pending.ForEach([&](DAddr begin, DAddr end, s32 count) {
                for (DAddr at = begin; at < end; ++at) {
                    actual[at - BASE] += count;
                }
            });
            REQUIRE(actual == expected);
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
                    fence.original.push_back({b, b - BASE, e - b});
                    pending.Add(b, e - b);
                });
            });
            if (!fence.original.empty()) {
                fence_copies.push_back(fence.original);
                fences.push_back(std::move(fence));
            }
        };
        const auto release = [&] {
            const Fence& fence = fences.front();
            std::array<bool, N> written{};
            for (const Copy& copy : fence_copies.front()) {
                const auto write = [&](DAddr address, const u8* source, u64 size) {
                    for (u64 i = 0; i < size; ++i) {
                        const size_t at = address + i - BASE;
                        // Never over a CPU write made after this fence's copy was taken.
                        REQUIRE(cpu_write_time[at] < fence.commit_time);
                        REQUIRE(source[i] == fence.staged[at]);
                        guest[at] = source[i];
                        written[at] = true;
                    }
                };
                VideoCommon::WriteBackFenceCopy(copy.src_offset, copy.size,
                                                fence.staged.data() + copy.dst_offset, pending,
                                                uncommitted, committed, gpu_modified, write);
            }
            for (const Copy& copy : fence.original) {
                for (u64 at = copy.src_offset; at < copy.src_offset + copy.size; ++at) {
                    // Everything of the copy not superseded since its commit reaches guest memory.
                    REQUIRE(written[at - BASE] == (drop_time[at - BASE] < fence.commit_time));
                }
            }
            fence_copies.pop_front();
            fences.pop_front();
        };
        const auto inline_cpu_writes = [&] {
            for (const auto [begin, end] : pending_inlines) {
                // InlineMemoryImplementation: guest bytes into the GPU buffer, no longer dirty.
                drop(begin, end);
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
                drop(begin, end);
                pending_inlines.emplace_back(begin, end);
                break;
            }
            default:
                inline_cpu_writes();
                break;
            }
            check_counts();
        }
        ++time;
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
