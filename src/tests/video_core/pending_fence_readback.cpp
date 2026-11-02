// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <deque>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/buffer_cache/fence_download_ranges.h"
#include "video_core/buffer_cache/pending_fence_readback.h"

namespace {
using Set = Common::RangeSet<DAddr>;
using Overlap = Common::OverlapRangeSet<DAddr>;
using VideoCommon::PendingFenceRead;
using VideoCommon::PlanPendingFenceReads;

constexpr DAddr BASE = 16ULL << 22;

/// Same fields as VideoCommon::BufferCopy: src_offset is the device address, dst_offset the
/// staging offset.
struct Copy {
    u64 src_offset;
    u64 dst_offset;
    size_t size;
};
using CopyList = std::vector<Copy>;
using Reads = std::vector<PendingFenceRead>;

struct Random {
    u32 state;
    u32 operator()() {
        state = state * 1664525U + 1013904223U;
        return state >> 8;
    }
};

/// Page-level model of MemoryTrackerBase: GPU-modified pages (ForEachDownloadRange, cleared by
/// synchronous downloads) and Preflushable pages (never unmarked, as in the buffer cache).
template <size_t Bytes, size_t PageSize>
struct ModelTracker {
    static constexpr size_t Pages = Bytes / PageSize;
    std::array<bool, Pages> gpu{};
    std::array<bool, Pages> preflushable{};

    template <typename Func>
    void ForEachDownloadRange(DAddr address, u64 size, bool clear, Func&& func) {
        ForEachPage(address, size, [&](size_t page) {
            if (gpu[page]) {
                if (clear) {
                    gpu[page] = false;
                }
                func(BASE + page * PageSize, u64{PageSize});
            }
        });
    }

    template <typename Func>
    void ForEachPreflushableRange(DAddr address, u64 size, Func&& func) {
        ForEachPage(address, size, [&](size_t page) {
            if (preflushable[page]) {
                func(BASE + page * PageSize, u64{PageSize});
            }
        });
    }

    void MarkGpu(DAddr address, u64 size) {
        ForEachPage(address, size, [&](size_t page) { gpu[page] = true; });
    }

    template <typename Func>
    static void ForEachPage(DAddr address, u64 size, Func&& func) {
        if (size == 0) {
            return;
        }
        const size_t first = (address - BASE) / PageSize;
        const size_t last = (address + size - 1 - BASE) / PageSize;
        for (size_t page = first; page <= last; ++page) {
            func(page);
        }
    }
};

u64 Covered(const Reads& reads) {
    u64 total = 0;
    for (const auto& read : reads) {
        total += read.size;
    }
    return total;
}
} // namespace

TEST_CASE("Pending fence readback: each byte comes from its newest pending copy",
          "[video_core][buffer_readback][pending_fence_readback]") {
    // Fence 0 copied [0, 64) to staging 0; fence 1 copied [32, 96) to staging 1000.
    std::deque<CopyList> fences{
        CopyList{{BASE + 0, 0, 64}},
        CopyList{{BASE + 32, 1000, 64}},
    };
    const Set uncommitted;
    const std::deque<Set> committed;
    Reads reads;
    REQUIRE(PlanPendingFenceReads(BASE + 16, 64, fences, uncommitted, committed, reads));
    REQUIRE(Covered(reads) == 64);
    std::ranges::sort(reads, {}, &PendingFenceRead::address);
    REQUIRE(reads.size() == 2);
    REQUIRE(reads[0].address == BASE + 16);
    REQUIRE(reads[0].size == 16);
    REQUIRE(reads[0].fence == 0);
    REQUIRE(reads[0].staging_offset == 16);
    REQUIRE(reads[1].address == BASE + 32);
    REQUIRE(reads[1].size == 48);
    REQUIRE(reads[1].fence == 1);
    REQUIRE(reads[1].staging_offset == 1000);
}

TEST_CASE("Pending fence readback: partial coverage or later GPU writes refuse the plan",
          "[video_core][buffer_readback][pending_fence_readback]") {
    std::deque<CopyList> fences{CopyList{{BASE + 0, 0, 32}, {BASE + 48, 64, 16}}};
    Set uncommitted;
    std::deque<Set> committed;
    Reads reads{PendingFenceRead{.address = 1, .size = 1, .fence = 9, .staging_offset = 0}};

    // [32, 48) was never copied: refused, and the earlier read stays the only one.
    REQUIRE_FALSE(PlanPendingFenceReads(BASE + 16, 48, fences, uncommitted, committed, reads));
    REQUIRE(reads.size() == 1);
    // Both covered parts alone are fine.
    REQUIRE(PlanPendingFenceReads(BASE + 0, 32, fences, uncommitted, committed, reads));
    REQUIRE(PlanPendingFenceReads(BASE + 48, 16, fences, uncommitted, committed, reads));
    REQUIRE(reads.size() == 3);

    // A GPU write recorded after the copies (still uncommitted) refuses the bytes it selected.
    uncommitted.Add(BASE + 8, 1);
    REQUIRE_FALSE(PlanPendingFenceReads(BASE + 0, 32, fences, uncommitted, committed, reads));
    REQUIRE(PlanPendingFenceReads(BASE + 9, 23, fences, uncommitted, committed, reads));
    // ...as does one accumulated by SignalOrdering but not yet committed.
    committed.emplace_back(std::move(uncommitted));
    REQUIRE_FALSE(PlanPendingFenceReads(BASE + 8, 1, fences, Set{}, committed, reads));

    // No pending fences: nothing to plan.
    const std::deque<CopyList> none;
    REQUIRE_FALSE(PlanPendingFenceReads(BASE + 0, 1, none, Set{}, std::deque<Set>{}, reads));
}

TEST_CASE("Pending fence readback: bytes cut by a newer CPU write are never served",
          "[video_core][buffer_readback][pending_fence_readback]") {
    std::deque<CopyList> fences{CopyList{{BASE + 0, 0, 64}}, CopyList{{BASE + 0, 64, 64}}};
    Overlap pending;
    pending.Add(BASE, 64);
    pending.Add(BASE, 64);
    Set uncommitted;
    std::deque<Set> committed;
    // BufferCache::OnCPUWrite: guest memory holds the newest [20, 24).
    VideoCommon::DropFenceDownloads(BASE + 20, 4, fences, pending, uncommitted, committed);
    Reads reads;
    REQUIRE_FALSE(PlanPendingFenceReads(BASE + 16, 16, fences, uncommitted, committed, reads));
    REQUIRE(reads.empty());
    REQUIRE(PlanPendingFenceReads(BASE + 0, 20, fences, uncommitted, committed, reads));
    REQUIRE(PlanPendingFenceReads(BASE + 24, 40, fences, uncommitted, committed, reads));
    for (const auto& read : reads) {
        REQUIRE(read.fence == 1);
        REQUIRE(read.staging_offset == 64 + (read.address - BASE));
    }
}

TEST_CASE("Pending fence readback: a selection rule change retires older fences",
          "[video_core][buffer_readback][pending_fence_readback]") {
    VideoCommon::DownloadSelectionEpochs epochs;
    // Fence A: selected under "every GPU write".
    epochs.NoteSelection(true, true);
    const u64 fence_a = epochs.OldestSelection();
    REQUIRE(epochs.IsCurrent(fence_a));
    // The rule changes (another channel's memory manager); a write there is not selected.
    epochs.NoteSelection(false, true);
    REQUIRE_FALSE(epochs.IsCurrent(fence_a));
    // Fence B commits selections all made under the new rule.
    const u64 fence_b = epochs.OldestSelection();
    REQUIRE(epochs.IsCurrent(fence_b));
    // A selection already queued when the rule changes keeps its older epoch for its fence.
    epochs.NoteSelection(false, true);
    epochs.NoteSelection(true, false);
    REQUIRE_FALSE(epochs.IsCurrent(epochs.OldestSelection()));
    // Repeating the same rule is no change.
    epochs.NoteSelection(true, true);
    const u64 fence_c = epochs.OldestSelection();
    epochs.NoteSelection(true, false);
    REQUIRE(epochs.IsCurrent(fence_c));
}

TEST_CASE("Pending fence readback: without the rule epoch a stale copy would be planned",
          "[video_core][buffer_readback][pending_fence_readback]") {
    // Why the epoch is needed: a byte copied while every write was selected, then written while
    // only Preflushable pages are, leaves no selection behind.
    constexpr size_t N = 64;
    ModelTracker<N, 16> tracker;
    VideoCommon::DownloadSelectionEpochs epochs;
    Set uncommitted;
    std::deque<Set> committed;
    Set gpu_modified;
    const auto gpu_write = [&](bool download_all) {
        epochs.NoteSelection(download_all, uncommitted.Empty() && committed.empty());
        gpu_modified.Add(BASE, 16);
        tracker.MarkGpu(BASE, 16);
        (void)VideoCommon::QueueFenceDownloadRanges(download_all, BASE, 16, tracker, uncommitted);
    };
    gpu_write(true);
    committed.emplace_back(std::move(uncommitted));
    const Set selected = VideoCommon::MergeFenceDownloadRanges(committed);
    std::deque<CopyList> fences{CopyList{}};
    VideoCommon::ForEachFenceDownloadRange(BASE, 16, tracker, gpu_modified,
                                           [&](DAddr begin, DAddr end) {
                                               fences.back().push_back({begin, 0, end - begin});
                                           });
    REQUIRE_FALSE(selected.Empty());
    const u64 fence_epoch = epochs.OldestSelection();
    gpu_write(false); // Not Preflushable: nothing is selected.
    REQUIRE(uncommitted.Empty());
    Reads reads;
    REQUIRE(PlanPendingFenceReads(BASE, 16, fences, uncommitted, committed, reads));
    REQUIRE_FALSE(epochs.IsCurrent(fence_epoch));
}

TEST_CASE("Old-rule fence release preserves dirty bytes for a fresh read",
          "[video_core][buffer_readback][pending_fence_readback]") {
    Overlap pending;
    Set dirty;
    Set uncommitted;
    std::deque<Set> committed;
    pending.Add(BASE, 16);
    dirty.Add(BASE, 16);
    std::array<u8, 16> staged{};
    u64 written = 0;
    const auto write = [&](DAddr, const u8*, u64 size) { written += size; };
    REQUIRE(VideoCommon::WriteBackFenceCopy(BASE, 16, staged.data(), pending, uncommitted,
                                           committed, dirty, write, false) == 16);
    REQUIRE(written == 16);
    REQUIRE(pending.Empty());
    REQUIRE(dirty.Intersects(BASE, 16));

    // A new-rule copy can retire the bytes normally once it contains their current value.
    pending.Add(BASE, 16);
    REQUIRE(VideoCommon::WriteBackFenceCopy(BASE, 16, staged.data(), pending, uncommitted,
                                           committed, dirty, write, true) == 16);
    REQUIRE(pending.Empty());
    REQUIRE(dirty.Empty());
}

TEST_CASE("Pending fence readback: random sequences serve exactly the fresh download's bytes",
          "[video_core][buffer_readback][pending_fence_readback]") {
    // Byte model of BufferCache's fence downloads and synchronous downloads. GPU writes select
    // their Preflushable pages (or everything), SignalOrdering accumulates, commits copy at a
    // scheduler tick, fences are released after their tick, guest CPU writes are cut out of
    // pending copies and applied to the GPU buffer later (CachedWriteMemory), pages become
    // Preflushable, and the selection rule occasionally changes. A synchronous download of a
    // range takes every GPU-modified byte of its GPU-modified pages. Whenever the plan serves
    // it from pending copies, each staged byte must equal the GPU buffer's current byte (what
    // the fresh copy plus Scheduler::Finish would have returned), and the copies used must not
    // be newer than the tick waited for.
    constexpr size_t N = 512;
    constexpr size_t PAGE = 32;
    u64 planned_downloads = 0;
    u64 fresh_downloads = 0;
    u64 epoch_refusals = 0;
    for (u32 seed = 1; seed <= 300; ++seed) {
        CAPTURE(seed);
        Random random{seed * 104729U};
        ModelTracker<N, PAGE> tracker;
        VideoCommon::DownloadSelectionEpochs epochs;
        Overlap pending;
        Set uncommitted;
        std::deque<Set> committed;
        Set gpu_modified;
        std::array<u8, N> guest{};
        std::array<u8, N> gpu{};
        struct Fence {
            u64 tick;
            u64 epoch;
            std::array<u8, N> staged;
        };
        std::deque<Fence> fences;
        std::deque<CopyList> fence_copies; // As BufferCache::pending_downloads.
        std::vector<std::pair<DAddr, DAddr>> pending_inlines;
        bool download_all = random() % 2 == 0;
        u64 recording_tick = 1; // Scheduler::CurrentTick
        u64 completed_tick = 0; // Known GPU tick
        u8 token = 1;
        const auto random_range = [&] {
            const DAddr begin = random() % N;
            const DAddr end = begin + 1 + random() % (std::min<u64>)(N - begin, 80);
            return std::pair<DAddr, DAddr>{BASE + begin, BASE + end};
        };
        const auto drop = [&](DAddr begin, DAddr end) {
            VideoCommon::DropFenceDownloads(begin, end - begin, fence_copies, pending,
                                            uncommitted, committed);
        };
        const auto gpu_write = [&](DAddr begin, DAddr end) {
            ++token;
            for (DAddr at = begin; at < end; ++at) {
                gpu[at - BASE] = token;
            }
            // BufferCache::MarkWrittenBuffer then QueueDownload.
            tracker.MarkGpu(begin, end - begin);
            gpu_modified.Add(begin, end - begin);
            epochs.NoteSelection(download_all, uncommitted.Empty() && committed.empty());
            (void)VideoCommon::QueueFenceDownloadRanges(download_all, begin, end - begin, tracker,
                                                        uncommitted);
        };
        const auto commit = [&] {
            if (!uncommitted.Empty()) {
                committed.emplace_back(std::move(uncommitted));
            }
            if (committed.empty()) {
                return;
            }
            const Set selected = VideoCommon::MergeFenceDownloadRanges(committed);
            CopyList copies;
            selected.ForEach([&](DAddr begin, DAddr end) {
                VideoCommon::ForEachFenceDownloadRange(
                    begin, end - begin, tracker, gpu_modified, [&](DAddr b, DAddr e) {
                        copies.push_back({b, b - BASE, e - b});
                        pending.Add(b, e - b);
                    });
            });
            if (copies.empty()) {
                return;
            }
            // The copy executes after all earlier recorded work: it stages today's GPU bytes.
            fences.push_back({recording_tick, epochs.OldestSelection(), gpu});
            fence_copies.push_back(std::move(copies));
            ++recording_tick; // The fence is queued and flushed.
        };
        const auto release = [&] {
            const Fence& fence = fences.front();
            completed_tick = (std::max)(completed_tick, fence.tick);
            for (const Copy& copy : fence_copies.front()) {
                const auto write = [&](DAddr address, const u8* source, u64 size) {
                    for (u64 i = 0; i < size; ++i) {
                        guest[address + i - BASE] = source[i];
                    }
                };
                VideoCommon::WriteBackFenceCopy(copy.src_offset, copy.size,
                                                fence.staged.data() + copy.dst_offset, pending,
                                                uncommitted, committed, gpu_modified, write,
                                                epochs.IsCurrent(fence.epoch));
            }
            fence_copies.pop_front();
            fences.pop_front();
        };
        const auto inline_cpu_writes = [&] {
            for (const auto [begin, end] : pending_inlines) {
                drop(begin, end);
                gpu_modified.Subtract(begin, end - begin);
                for (DAddr at = begin; at < end; ++at) {
                    gpu[at - BASE] = guest[at - BASE];
                }
            }
            pending_inlines.clear();
        };
        // BufferCache::DownloadBufferMemory, Vulkan path.
        const auto sync_download = [&](DAddr address, u64 size) {
            Reads reads;
            bool from_fences = !fence_copies.empty();
            std::vector<std::pair<DAddr, DAddr>> wanted;
            tracker.ForEachDownloadRange(address, size, true, [&](DAddr range, u64 range_size) {
                gpu_modified.ForEachInRange(range, range_size, [&](DAddr begin, DAddr end) {
                    wanted.emplace_back(begin, end);
                    if (from_fences) {
                        from_fences = PlanPendingFenceReads(begin, end - begin, fence_copies,
                                                            uncommitted, committed, reads);
                    }
                });
                drop(range, range + range_size);
                gpu_modified.Subtract(range, range_size);
            });
            if (wanted.empty()) {
                return;
            }
            u64 wait_tick = 0;
            if (from_fences) {
                for (const auto& read : reads) {
                    REQUIRE(read.fence < fences.size());
                    if (!epochs.IsCurrent(fences[read.fence].epoch)) {
                        from_fences = false;
                        ++epoch_refusals;
                        break;
                    }
                    wait_tick = (std::max)(wait_tick, fences[read.fence].tick);
                }
            }
            if (!from_fences) {
                ++fresh_downloads;
                // Fresh copy behind all recorded work, then Finish.
                completed_tick = recording_tick;
                ++recording_tick;
                for (const auto [begin, end] : wanted) {
                    for (DAddr at = begin; at < end; ++at) {
                        guest[at - BASE] = gpu[at - BASE];
                    }
                }
                return;
            }
            ++planned_downloads;
            REQUIRE(wait_tick < recording_tick);
            completed_tick = (std::max)(completed_tick, wait_tick);
            std::array<bool, N> served{};
            for (const auto& read : reads) {
                const Fence& fence = fences[read.fence];
                REQUIRE(fence.tick <= completed_tick);
                for (u64 i = 0; i < read.size; ++i) {
                    const size_t at = read.address + i - BASE;
                    REQUIRE_FALSE(served[at]);
                    served[at] = true;
                    const u8 staged = fence.staged[read.staging_offset + i];
                    // Exactly what the fresh download would have produced.
                    REQUIRE(staged == gpu[at]);
                    guest[at] = staged;
                }
            }
            for (const auto [begin, end] : wanted) {
                for (DAddr at = begin; at < end; ++at) {
                    REQUIRE(served[at - BASE]);
                }
            }
        };
        for (int step = 0; step < 400; ++step) {
            switch (random() % 12) {
            case 0:
            case 1:
            case 2: {
                const auto [begin, end] = random_range();
                gpu_write(begin, end);
                break;
            }
            case 3:
                if (!uncommitted.Empty() && random() % 3 == 0) {
                    committed.emplace_back(std::move(uncommitted)); // SignalOrdering
                } else {
                    commit();
                }
                break;
            case 4:
                if (!fences.empty()) {
                    release();
                }
                break;
            case 5: {
                const auto [begin, end] = random_range();
                ++token;
                for (DAddr at = begin; at < end; ++at) {
                    guest[at - BASE] = token;
                }
                // BufferCache::OnCPUWrite on a GPU-modified page.
                drop(begin, end);
                pending_inlines.emplace_back(begin, end);
                break;
            }
            case 6:
                inline_cpu_writes();
                break;
            case 7:
                // GetFlushArea: a guest CPU read makes a page Preflushable for good.
                tracker.preflushable[random() % tracker.Pages] = true;
                break;
            case 8:
                if (random() % 8 == 0) {
                    download_all = !download_all;
                }
                break;
            case 9:
                completed_tick = (std::max)(completed_tick,
                                            (std::min)(recording_tick - 1,
                                                       completed_tick + random() % 3));
                break;
            default: {
                const auto [begin, end] = random_range();
                sync_download(begin, end - begin);
                break;
            }
            }
        }
        inline_cpu_writes();
        commit();
        while (!fences.empty()) {
            release();
        }
        sync_download(BASE, N);
        REQUIRE(pending.Empty());
        REQUIRE(gpu_modified.Empty());
        // Old-rule fences must not retire bytes a later rule could have omitted.
        // This also checks every seed with rule changes, including the old seed-13 failure.
        REQUIRE(guest == gpu);
    }
    // The model must exercise both paths, and the rule epoch.
    REQUIRE(planned_downloads > 100);
    REQUIRE(fresh_downloads > 100);
    REQUIRE(epoch_refusals > 0);
}
