// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <deque>
#include <utility>

#include "common/common_types.h"
#include "common/range_sets.inc"

namespace VideoCommon {

// All accumulated ranges are copied from the same current buffer contents at
// commit time, not from historical snapshots. Their union is sufficient: the
// old pairwise subtraction assigned overlapping bytes to the last range set,
// but did not change their values. Merge once instead of O(N^2) set walks.
inline Common::RangeSet<DAddr> MergeFenceDownloadRanges(
    std::deque<Common::RangeSet<DAddr>>& ranges) {
    if (ranges.empty()) {
        return {};
    }
    auto merged = std::move(ranges.front());
    ranges.pop_front();
    for (const auto& range : ranges) {
        range.ForEach([&](DAddr begin, DAddr end) { merged.Add(begin, end - begin); });
    }
    ranges.clear();
    return merged;
}

/// Enumerate actual fence-copy bytes, intersecting the selected interval with both
/// page-level tracking and byte-level GPU dirtiness. Page enumeration rounds outward;
/// do not let it widen the selection again or duplicate disjoint copies on one page.
/// Tracking is not cleared until the existing download completion path handles it.
template <typename Tracker, typename Func>
void ForEachFenceDownloadRange(DAddr address, u64 size, Tracker& tracker,
                              const Common::RangeSet<DAddr>& gpu_modified, Func&& func) {
    if (size == 0) {
        return;
    }
    const DAddr end = address + size;
    tracker.ForEachDownloadRange(address, size, false, [&](DAddr page_begin, u64 page_size) {
        const DAddr clipped_begin = (std::max)(address, page_begin);
        const DAddr clipped_end = (std::min)(end, page_begin + page_size);
        if (clipped_begin < clipped_end) {
            gpu_modified.ForEachInRange(clipped_begin, clipped_end - clipped_begin, func);
        }
    });
}

/// Queue the portions of a GPU write that must be visible to the CPU at a fence: the
/// Preflushable pages, which are every page a guest CPU read has asked the buffer cache to
/// flush (BufferCache::GetFlushArea). Returns whether read traps are still required.
template <typename Tracker>
bool QueueFenceDownloadRanges(bool download_all, DAddr address, u64 size, Tracker& tracker,
                              Common::RangeSet<DAddr>& downloads) {
    if (size == 0) {
        return false;
    }
    if (download_all) {
        downloads.Add(address, size);
        return false;
    }
    u64 preflushable_bytes = 0;
    tracker.ForEachPreflushableRange(address, size, [&](DAddr begin, u64 length) {
        downloads.Add(begin, length);
        preflushable_bytes += length;
    });
    // Keep trapping unread pages even when another page in this binding is preflushable.
    // An overlap must not promote an entire native storage window into eager readback.
    return preflushable_bytes != size;
}

/// As above, additionally selecting a separately recorded set of CPU-read ranges. The buffer
/// cache no longer keeps such a set: the one it kept was always a copy of the Preflushable pages.
template <typename Tracker>
bool QueueFenceDownloadRanges(bool download_all, DAddr address, u64 size, Tracker& tracker,
                              const Common::RangeSet<DAddr>& cpu_reads,
                              Common::RangeSet<DAddr>& downloads) {
    const bool needs_traps =
        QueueFenceDownloadRanges(download_all, address, size, tracker, downloads);
    if (!download_all && size != 0) {
        cpu_reads.ForEachInRange(address, size, [&](DAddr begin, DAddr end) {
            downloads.Add(begin, end - begin);
        });
    }
    return needs_traps;
}

/// Forget every fence copy of [address, address + size) that is not yet written back to guest
/// memory, and every queued selection that would take one. Used when guest memory there gets
/// newer contents than the GPU buffer's (a CPU write, an inline upload, a sync download).
inline void DropFenceDownloads(DAddr address, u64 size,
                               Common::OverlapRangeSet<DAddr>& pending_writebacks,
                               Common::RangeSet<DAddr>& uncommitted,
                               std::deque<Common::RangeSet<DAddr>>& committed) {
    if (size == 0) {
        return;
    }
    pending_writebacks.DeleteAll(address, size);
    uncommitted.Subtract(address, size);
    for (auto& ranges : committed) {
        ranges.Subtract(address, size);
    }
}

/// Write one completed fence copy of [address, address + size) (staged: its bytes) back to guest
/// memory, only where it is still pending (DropFenceDownloads removes bytes guest memory has
/// newer contents for). Bytes whose last pending copy this was, and which no queued selection
/// still covers, stop being GPU-modified. write(address, source, size). Returns bytes written.
template <typename Write>
u64 WriteBackFenceCopy(DAddr address, u64 size, const u8* staged,
                       Common::OverlapRangeSet<DAddr>& pending_writebacks,
                       const Common::RangeSet<DAddr>& uncommitted,
                       const std::deque<Common::RangeSet<DAddr>>& committed,
                       Common::RangeSet<DAddr>& gpu_modified, Write&& write) {
    if (size == 0) {
        return 0;
    }
    u64 written = 0;
    pending_writebacks.ForEachInRange(address, size, [&](DAddr start, DAddr end, s32) {
        write(start, staged + (start - address), end - start);
        written += end - start;
    });
    pending_writebacks.Subtract(address, size, [&](DAddr start, DAddr end) {
        if (uncommitted.Intersects(start, end - start) ||
            std::ranges::any_of(committed, [&](const auto& ranges) {
                return ranges.Intersects(start, end - start);
            })) {
            return;
        }
        gpu_modified.Subtract(start, end - start);
    });
    return written;
}

} // namespace VideoCommon
