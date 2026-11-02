// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"
#include "common/range_sets.inc"

namespace VideoCommon {

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

/// Queue the portions of a GPU write that must be visible to the CPU at a fence.
/// Returns whether reactive read traps are still required for the write range.
template <typename Tracker>
bool QueueFenceDownloadRanges(bool download_all, DAddr address, u64 size, Tracker& tracker,
                              const Common::RangeSet<DAddr>& cpu_reads,
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
    cpu_reads.ForEachInRange(address, size, [&](DAddr begin, DAddr end) {
        downloads.Add(begin, end - begin);
    });
    // Keep trapping unread pages even when another page in this binding is preflushable.
    // An overlap must not promote an entire native storage window into eager readback.
    return preflushable_bytes != size;
}

} // namespace VideoCommon
