// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/download_batch.h"

TEST_CASE("Texture readbacks share aligned staging space", "[video_core][download_batch]") {
    const std::array<u32, 4> sizes{128, 128, 65, 1};
    const auto batch = VideoCommon::PlanDownloadBatch(sizes.size(), [&](size_t i) { return sizes[i]; });
    CHECK(batch.count == sizes.size());
    CHECK(batch.bytes == 448);
    CHECK(VideoCommon::DownloadBatchStride(0) == 0);
    CHECK(VideoCommon::DownloadBatchStride(std::numeric_limits<u32>::max()) == (1ULL << 32));
    CHECK(VideoCommon::PlanDownloadBatch(0, [](size_t) -> u32 {
        FAIL("An empty batch must not read image sizes");
        return 0;
    }).count == 0);
}

TEST_CASE("Texture batches preserve order and isolate oversized images", "[video_core][download_batch]") {
    constexpr u32 cap = 64U << 20;
    const std::array<u32, 7> sizes{128, 128, cap, 144U << 20, 65, cap - 128, 64};
    std::vector<size_t> counts;
    size_t first = 0;
    u64 bytes = 0;
    while (first < sizes.size()) {
        const auto batch = VideoCommon::PlanDownloadBatch(sizes.size() - first,
            [&](size_t i) { return sizes[first + i]; });
        REQUIRE(batch.count != 0);
        REQUIRE(batch.count <= sizes.size() - first);
        CHECK((batch.bytes <= cap || batch.count == 1));
        u64 offset = 0;
        for (size_t i = 0; i < batch.count; ++i) {
            CHECK(offset % 64 == 0);
            CHECK(offset + sizes[first + i] <= batch.bytes);
            offset += VideoCommon::DownloadBatchStride(sizes[first + i]);
        }
        CHECK(offset == batch.bytes);
        counts.push_back(batch.count);
        bytes += batch.bytes;
        first += batch.count;
    }
    CHECK(counts == std::vector<size_t>{2, 1, 1, 2, 1});
    u64 expected = 0;
    for (const auto size : sizes) expected += VideoCommon::DownloadBatchStride(size);
    CHECK(bytes == expected);
}

TEST_CASE("Readback copies precede a single wait and ordered writeback", "[video_core][download_batch]") {
    const std::array<u32, 3> sizes{65, 1, 128};
    std::array<u64, 3> offsets{};
    std::vector<std::string> events;
    unsigned waits = 0;
    u32 overlapping_guest_value = 0;
    VideoCommon::ExecuteDownloadBatch(sizes.size(), [&](size_t i) { return sizes[i]; },
        [&](size_t i, u64 offset) {
            CHECK(waits == 0);
            offsets[i] = offset;
            events.push_back("copy" + std::to_string(i));
        },
        [&] { ++waits; events.push_back("wait"); },
        [&](size_t i, u64 offset) {
            CHECK(waits == 1);
            CHECK(offset == offsets[i]);
            overlapping_guest_value = static_cast<u32>(i + 1);
            events.push_back("write" + std::to_string(i));
        });
    CHECK(offsets == std::array<u64, 3>{0, 128, 192});
    CHECK(events == std::vector<std::string>{"copy0", "copy1", "copy2", "wait", "write0", "write1", "write2"});
    CHECK(overlapping_guest_value == 3);
    CHECK(waits == 1);
    VideoCommon::ExecuteDownloadBatch(0, [](size_t) -> u32 { return 0; },
        [](size_t, u64) { FAIL("empty copy"); },
        [] { FAIL("empty wait"); },
        [](size_t, u64) { FAIL("empty writeback"); });
}
