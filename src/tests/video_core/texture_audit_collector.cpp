// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <deque>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/literals.h"
#include "video_core/texture_cache/collector_thresholds.h"
#include "video_core/texture_cache/download_batch.h"
#include "video_core/vulkan_common/memory_budget.h"

using namespace Common::Literals;

namespace {

// A device-local heap in VMA's terms: fixed-size blocks, images placed round-robin across them
// (as streamed textures end up interleaved), and an empty block returned to the driver.
class FragmentedHeap {
public:
    static constexpr u64 BlockBytes = 64_MiB;
    static constexpr u64 ImageBytes = 4_MiB;

    FragmentedHeap(size_t blocks, size_t images_per_block, u64 external_)
        : live(blocks, images_per_block), external{external_} {
        for (size_t image = 0; image < blocks * images_per_block; ++image) {
            lru.push_back(image % blocks);
        }
    }

    u64 Blocks() const {
        u64 bytes = 0;
        for (const size_t count : live) {
            bytes += count != 0 ? BlockBytes : 0;
        }
        return bytes;
    }
    u64 Allocations() const {
        return lru.size() * ImageBytes;
    }
    // What the driver charges (VK_EXT_memory_budget heapUsage).
    u64 DriverUsage() const {
        return Blocks() + external;
    }
    // Device::GetDeviceMemoryUsage: driver usage without unused block space.
    u64 CollectorUsage() const {
        return Vulkan::AllocationAdjustedBytes(DriverUsage(), Blocks(), Allocations());
    }
    bool EvictOldest() {
        if (lru.empty()) {
            return false;
        }
        --live[lru.front()];
        lru.pop_front();
        return true;
    }

private:
    std::vector<size_t> live;
    std::deque<size_t> lru;
    u64 external;
};

struct CollectorRun {
    size_t evictions{};
    size_t evictions_after_first_reread{};
    size_t rereads_that_evicted{};
};

// TextureCache::TickFrame's control loop: thresholds from the budget re-read every 30 frames,
// then at most 40 aggressive evictions a frame while the usage stays over the critical threshold.
CollectorRun RunCollector(bool adjust_budget_for_free_block_space, u64 raw_budget,
                          size_t frames) {
    FragmentedHeap heap{64, 16, 512_MiB};
    CollectorRun run;
    VideoCommon::CollectorThresholds thresholds;
    std::vector<bool> window_evicted(frames / 30 + 1);
    for (size_t frame = 0; frame < frames; ++frame) {
        if (frame % 30 == 0) {
            const u64 budget = adjust_budget_for_free_block_space
                                   ? Vulkan::AllocationAdjustedBudget(raw_budget, heap.Blocks(),
                                                                      heap.Allocations())
                                   : raw_budget;
            thresholds = VideoCommon::BudgetCollectorThresholds(budget);
        }
        for (size_t i = 0; i < 40 && heap.CollectorUsage() >= thresholds.critical; ++i) {
            if (!heap.EvictOldest()) {
                break;
            }
            ++run.evictions;
            window_evicted[frame / 30] = true;
            if (frame >= 30) {
                ++run.evictions_after_first_reread;
            }
        }
    }
    // Windows after the first re-read in which the collector still had to evict.
    for (size_t window = 1; window < window_evicted.size(); ++window) {
        run.rereads_that_evicted += window_evicted[window] ? 1 : 0;
    }
    return run;
}

} // Anonymous namespace

TEST_CASE("Collector thresholds keep a bounded margin under the budget",
          "[video_core][texture_audit]") {
    using VideoCommon::BudgetCollectorThresholds;
    const auto small = BudgetCollectorThresholds(4608_MiB);
    CHECK(small.critical == 4032_MiB);
    CHECK(small.expected == 3456_MiB);
    // The margin stops growing at 1 GiB: the stutter log's 8862 MiB budget.
    const auto large = BudgetCollectorThresholds(8862_MiB);
    CHECK(large.critical == 8862_MiB - 1_GiB);
    CHECK(large.expected == 8862_MiB - 2_GiB);
    CHECK(BudgetCollectorThresholds(0).critical == 0);
    CHECK(BudgetCollectorThresholds(1).critical == 1);
    CHECK(BudgetCollectorThresholds(1).expected == 1);
}

TEST_CASE("Collector converges against the driver budget despite fragmented blocks",
          "[video_core][texture_audit]") {
    // 64 blocks of 16 interleaved 4 MiB images plus 512 MiB outside VMA: 4608 MiB in use, the
    // whole budget. 145 evictions (580 MiB) bring the allocation count under 4032 MiB.
    const CollectorRun run = RunCollector(false, 4608_MiB, 600);
    CHECK(run.evictions == 145);
    CHECK(run.evictions_after_first_reread == 0);
    CHECK(run.rereads_that_evicted == 0);
}

TEST_CASE("A budget reduced by free block space makes the collector chase its own holes",
          "[video_core][texture_audit]") {
    // The regression this audit removed from Device::GetDeviceMemoryBudget: every eviction leaves
    // a hole, every re-read lowers the threshold by it, so no pass ever finishes the job. In the
    // game this was a burst of costly-texture evictions (and re-uploads) at every re-read.
    const CollectorRun converged = RunCollector(false, 4608_MiB, 600);
    const CollectorRun chasing = RunCollector(true, 4608_MiB, 600);
    CHECK(chasing.evictions >= 4 * converged.evictions);
    CHECK(chasing.evictions_after_first_reread > 0);
    CHECK(chasing.rereads_that_evicted >= 10);

    // The two comparisons are the same until fragmentation appears...
    FragmentedHeap heap{64, 16, 512_MiB};
    CHECK(Vulkan::AllocationAdjustedBudget(4608_MiB, heap.Blocks(), heap.Allocations()) ==
          4608_MiB);
    // ...after which an eviction lowers the usage and the adjusted budget by the same bytes.
    const u64 usage_before = heap.CollectorUsage();
    const u64 budget_before =
        Vulkan::AllocationAdjustedBudget(4608_MiB, heap.Blocks(), heap.Allocations());
    REQUIRE(heap.EvictOldest());
    CHECK(usage_before - heap.CollectorUsage() == FragmentedHeap::ImageBytes);
    CHECK(budget_before - Vulkan::AllocationAdjustedBudget(4608_MiB, heap.Blocks(),
                                                           heap.Allocations()) ==
          FragmentedHeap::ImageBytes);
    CHECK(heap.DriverUsage() == 4608_MiB);
}

TEST_CASE("Collector downloads wait once per batch and evict only after write-back",
          "[video_core][texture_audit]") {
    constexpr u64 cap = 64_MiB;
    const std::array<u32, 6> sizes{100, 128, static_cast<u32>(cap), 200, 64, 1};
    std::vector<std::string> events;
    const auto size = [&](size_t i) { return sizes[i]; };
    const size_t waits = VideoCommon::DownloadAndEvict(
        sizes.size(), cap, size,
        [&](u64 bytes, auto&& run) {
            events.push_back("stage " + std::to_string(bytes));
            run();
            events.push_back("release");
        },
        [&](size_t i, u64 offset) {
            events.push_back("copy " + std::to_string(i) + "@" + std::to_string(offset));
        },
        [&] { events.push_back("wait"); },
        [&](size_t i, u64 offset) {
            events.push_back("write " + std::to_string(i) + "@" + std::to_string(offset));
        },
        [&](size_t i) { events.push_back("evict " + std::to_string(i)); });
    // Batches: {0, 1} (128 + 128 bytes; the cap-sized image does not fit after them), {2} alone,
    // {3, 4, 5}.
    CHECK(waits == 3);
    const std::vector<std::string> expected{
        "stage 256", "copy 0@0", "copy 1@128", "wait", "write 0@0", "write 1@128", "evict 0",
        "evict 1", "release",
        "stage " + std::to_string(cap), "copy 2@0", "wait", "write 2@0", "evict 2", "release",
        "stage 384", "copy 3@0", "copy 4@256", "copy 5@320", "wait", "write 3@0", "write 4@256",
        "write 5@320", "evict 3", "evict 4", "evict 5", "release",
    };
    CHECK(events == expected);
}

TEST_CASE("Collector downloads without a batch budget keep one wait per image",
          "[video_core][texture_audit]") {
    const std::array<u32, 3> sizes{4096, 64, 1};
    size_t stages = 0;
    std::vector<size_t> evicted;
    const size_t waits = VideoCommon::DownloadAndEvict(
        sizes.size(), 0, [&](size_t i) { return sizes[i]; },
        [&](u64 bytes, auto&& run) {
            CHECK(bytes == VideoCommon::DownloadBatchStride(sizes[stages]));
            ++stages;
            run();
        },
        [&](size_t, u64 offset) { CHECK(offset == 0); }, [] {},
        [&](size_t, u64 offset) { CHECK(offset == 0); },
        [&](size_t i) { evicted.push_back(i); });
    CHECK(waits == 3);
    CHECK(stages == 3);
    CHECK(evicted == std::vector<size_t>{0, 1, 2});

    // Nothing selected: no staging, no wait.
    CHECK(VideoCommon::DownloadAndEvict(
              0, 64_MiB, [](size_t) -> u32 { return 0; },
              [](u64, auto&&) { FAIL("No staging for an empty selection"); },
              [](size_t, u64) {}, [] { FAIL("No wait for an empty selection"); },
              [](size_t, u64) {}, [](size_t) {}) == 0);
}
