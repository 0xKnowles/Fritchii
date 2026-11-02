// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

#include "common/common_types.h"

namespace VideoCommon {

// Match the staging-copy alignment without overflowing a 32-bit image byte count.
constexpr u64 DownloadBatchStride(u32 bytes) {
    return (u64{bytes} + 63) & ~u64{63};
}

struct DownloadBatch {
    size_t count{};
    u64 bytes{};
};

// Keep modification order. An image larger than the budget gets a batch of its own;
// the budget bounds aggregation, not the size of an individual required download.
template <typename Size>
DownloadBatch PlanDownloadBatch(size_t count, Size size, u64 budget = 64ULL << 20) {
    DownloadBatch batch;
    while (batch.count < count) {
        const u64 bytes = DownloadBatchStride(size(batch.count));
        if (batch.count && bytes > budget - batch.bytes) {
            break;
        }
        batch.bytes += bytes;
        ++batch.count;
        if (batch.bytes >= budget) {
            break;
        }
    }
    return batch;
}

// The CPU must see no staging bytes before the single completion wait. Keep the same
// order for copies and writeback, including images whose guest-memory ranges overlap.
template <typename Size, typename Copy, typename Wait, typename Writeback>
void ExecuteDownloadBatch(size_t count, Size size, Copy copy, Wait wait, Writeback writeback) {
    if (!count) return;
    u64 offset = 0;
    for (size_t i = 0; i < count; ++i) {
        copy(i, offset);
        offset += DownloadBatchStride(size(i));
    }
    wait();
    offset = 0;
    for (size_t i = 0; i < count; ++i) {
        writeback(i, offset);
        offset += DownloadBatchStride(size(i));
    }
}

// Writes back and then evicts `count` images the collector selected, in selection order. For
// each batch, stage(bytes, run) acquires staging memory, calls run() and releases the staging
// after it returns; run() records every copy, waits ONCE, writes every copy back and only then
// evicts the batch's images (an image must outlive its copy). Previously the collector waited
// for the whole GPU once per downloaded image. A budget of 0 keeps one image per wait. Returns
// the number of waits.
template <typename Size, typename Stage, typename Copy, typename Wait, typename Writeback,
          typename Evict>
size_t DownloadAndEvict(size_t count, u64 budget, Size size, Stage stage, Copy copy, Wait wait,
                        Writeback writeback, Evict evict) {
    size_t waits = 0;
    for (size_t first = 0; first < count;) {
        const auto at = [&](size_t i) { return size(first + i); };
        const DownloadBatch batch = PlanDownloadBatch(count - first, at, budget);
        stage(batch.bytes, [&] {
            ExecuteDownloadBatch(
                batch.count, at, [&](size_t i, u64 offset) { copy(first + i, offset); },
                [&] {
                    wait();
                    ++waits;
                },
                [&](size_t i, u64 offset) { writeback(first + i, offset); });
            for (size_t i = 0; i < batch.count; ++i) {
                evict(first + i);
            }
        });
        first += batch.count;
    }
    return waits;
}

} // namespace VideoCommon
