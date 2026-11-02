// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/range_sets.inc"

namespace VideoCommon {

/// Serving a synchronous buffer download from fence copies that are already queued.
///
/// A fence commit (BufferCache::CommitAsyncFlushesHigh) records a GPU copy of the selected
/// GPU-written bytes into staging memory. Until the fence is released, those bytes stay
/// GPU-modified, so a synchronous download of them (BufferCache::DownloadBufferMemory, e.g. a
/// command fetch from GPU-written command memory) used to record a second copy of the same
/// bytes behind all queued work and wait for that submission (Scheduler::Finish).
///
/// The queued copy holds the current GPU contents of byte b when:
///  - b is still in that fence's copy list. Every CPU write, inline upload, DMA copy or clear
///    of b since the commit has cut b out of every pending fence's copies (DropFenceDownloads).
///  - no GPU write to b was recorded after the newest pending copy of b. Every GPU write goes
///    through BufferCache::QueueDownload, which adds b to the uncommitted selection whenever
///    b is selected for fence downloads. A selection either is still uncommitted/committed
///    (checked here: the plan is refused) or was consumed by a newer commit, which copied b
///    (then that newer fence is the newest pending copy of b) unless b stopped being
///    GPU-modified or tracked in between, in which case the synchronous download does not
///    ask for b at all.
///  - b being selected once implies every later GPU write to b is selected as well. That holds
///    while the selection mode does not change: with AsyncBufferDownloads every write is
///    selected; otherwise the Preflushable pages are, which are never unmarked.
///    DownloadSelectionEpochs below refuses fences whose selections predate a change of that
///    rule.
///
/// These are the same conditions under which releasing the pending fences would write their
/// copies back and retire b's GPU modification (WriteBackFenceCopy): the plan only does it
/// earlier, for the requested bytes, and only after the copies' submission has completed.
/// The copy itself must still have executed: the caller waits for the newest used fence's
/// tick, which is never later than the tick Scheduler::Finish would wait for.

/// One planned piece: bytes [address, address + size) are staged in pending fence `fence`
/// (index into the oldest-first fence copy lists) at staging offset `staging_offset` (the
/// copies' dst_offset domain).
struct PendingFenceRead {
    DAddr address;
    u64 size;
    size_t fence;
    u64 staging_offset;
};

/// Plans serving the GPU-modified bytes [address, address + size) from the newest pending fence
/// copy of each byte. fence_copies: per pending fence, oldest first, {src_offset = device
/// address, dst_offset = staging offset, size}, disjoint within a fence (as
/// BufferCache::pending_downloads). Appends the pieces to `reads` and returns true only when
/// every byte is covered and no queued selection (a GPU write recorded after the copies)
/// intersects the range; otherwise leaves `reads` unchanged and returns false.
template <typename CopyList, typename Reads>
[[nodiscard]] bool PlanPendingFenceReads(DAddr address, u64 size,
                                         const std::deque<CopyList>& fence_copies,
                                         const Common::RangeSet<DAddr>& uncommitted,
                                         const std::deque<Common::RangeSet<DAddr>>& committed,
                                         Reads& reads) {
    if (size == 0) {
        return true;
    }
    if (fence_copies.empty() || uncommitted.Intersects(address, size) ||
        std::ranges::any_of(committed, [&](const auto& ranges) {
            return ranges.Intersects(address, size);
        })) {
        return false;
    }
    const size_t first_read = reads.size();
    boost::container::small_vector<std::pair<DAddr, DAddr>, 8> uncovered{
        {address, address + size}};
    for (size_t fence = fence_copies.size(); fence-- > 0 && !uncovered.empty();) {
        for (const auto& copy : fence_copies[fence]) {
            const DAddr copy_begin = static_cast<DAddr>(copy.src_offset);
            const DAddr copy_end = copy_begin + static_cast<DAddr>(copy.size);
            for (size_t index = 0; index < uncovered.size();) {
                const auto [begin, end] = uncovered[index];
                const DAddr cut_begin = (std::max)(begin, copy_begin);
                const DAddr cut_end = (std::min)(end, copy_end);
                if (cut_begin >= cut_end) {
                    ++index;
                    continue;
                }
                reads.push_back(PendingFenceRead{
                    .address = cut_begin,
                    .size = cut_end - cut_begin,
                    .fence = fence,
                    .staging_offset = static_cast<u64>(copy.dst_offset) + (cut_begin - copy_begin),
                });
                const bool left = begin < cut_begin;
                const bool right = cut_end < end;
                if (left && right) {
                    uncovered[index] = {begin, cut_begin};
                    uncovered.insert(uncovered.begin() + static_cast<std::ptrdiff_t>(index + 1),
                                     {cut_end, end});
                    index += 2;
                } else if (left) {
                    uncovered[index] = {begin, cut_begin};
                    ++index;
                } else if (right) {
                    uncovered[index] = {cut_end, end};
                    ++index;
                } else {
                    uncovered.erase(uncovered.begin() + static_cast<std::ptrdiff_t>(index));
                }
            }
            if (uncovered.empty()) {
                break;
            }
        }
    }
    if (!uncovered.empty()) {
        reads.erase(reads.begin() + static_cast<std::ptrdiff_t>(first_read), reads.end());
        return false;
    }
    return true;
}

/// Which selection rule (all GPU writes, or the Preflushable pages; see
/// BufferCache::AsyncBufferDownloads, which depends on the channel's memory manager) each
/// pending fence's copies and every later GPU write were selected with. After a rule change, a
/// byte copied under the old rule may be written again without being selected under the new
/// one, so a fence whose selections predate the current rule must not serve planned reads.
class DownloadSelectionEpochs {
public:
    /// A GPU write is about to be selected with this rule. nothing_selected: no uncommitted or
    /// committed selection exists yet (this write may start one).
    void NoteSelection(bool download_all, bool nothing_selected) noexcept {
        if (known && download_all != last) {
            ++current;
        }
        known = true;
        last = download_all;
        if (nothing_selected) {
            oldest = current;
        }
    }

    /// Epoch of the oldest uncommitted or committed selection: record it with the fence that
    /// commits them.
    [[nodiscard]] u64 OldestSelection() const noexcept {
        return oldest;
    }

    /// Whether a fence recorded with `epoch` had all its selections, and every GPU write since,
    /// under the current rule.
    [[nodiscard]] bool IsCurrent(u64 epoch) const noexcept {
        return epoch == current;
    }

private:
    bool known = false;
    bool last = false;
    u64 current = 0;
    u64 oldest = 0;
};

} // namespace VideoCommon
