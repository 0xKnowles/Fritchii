// SPDX-License-Identifier: GPL-3.0-or-later
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "video_core/buffer_cache/buffer_cache_base.h"

TEST_CASE("Unmapped graphics uniforms clear every binding field", "[buffer_address_guards]") {
    const auto valid = VideoCommon::MakeGraphicsUniformBinding(DAddr{0x123000}, 256);
    CHECK(valid.device_addr == 0x123000);
    CHECK(valid.size == 256);
    const auto disabled = VideoCommon::MakeGraphicsUniformBinding(std::nullopt, 256);
    CHECK(disabled.device_addr == 0);
    CHECK(disabled.gpu_addr == 0);
    CHECK(disabled.size == 0);
    CHECK(disabled.buffer_id == VideoCommon::NULL_BUFFER_ID);
    CHECK(disabled.segment_first == 0);
    CHECK(disabled.segment_count == 0);
}

TEST_CASE("Buffer page lookup rejects zero and out-of-table device addresses",
          "[buffer_address_guards]") {
    for (const u32 bits : {12U, 16U}) {
        const u64 count = (1ULL << 36) >> bits;
        CHECK_FALSE(VideoCommon::CheckedBufferPage(0, bits, count));
        CHECK(VideoCommon::CheckedBufferPage(1, bits, count) == 0);
        CHECK(VideoCommon::CheckedBufferPage((1ULL << 36) - 1, bits, count) == count - 1);
        CHECK_FALSE(VideoCommon::CheckedBufferPage(1ULL << 36, bits, count));
        // Address from upstream's reported MKW uniform-buffer crash.
        CHECK_FALSE(VideoCommon::CheckedBufferPage(0x72253b2f2890ULL, bits, count));
        CHECK_FALSE(VideoCommon::CheckedBufferPage(std::numeric_limits<DAddr>::max(), bits, count));
    }
}
