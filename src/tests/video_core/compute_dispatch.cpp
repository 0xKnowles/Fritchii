// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/compute_dispatch.h"

TEST_CASE("Empty compute detection never uses direct dimensions for indirect dispatch",
          "[video_core][compute_dispatch]") {
    for (u32 mask = 0; mask < 8; ++mask) {
        const std::array<u32, 3> dimensions{
            (mask & 1) ? 0U : 3U, (mask & 2) ? 0U : 7U, (mask & 4) ? 0U : 11U};
        REQUIRE(VideoCommon::IsEmptyDirectComputeDispatch(false, dimensions) == (mask != 0));
        REQUIRE_FALSE(VideoCommon::IsEmptyDirectComputeDispatch(true, dimensions));
    }
    REQUIRE_FALSE(VideoCommon::IsEmptyDirectComputeDispatch(false, {1, 1, 1}));
    REQUIRE_FALSE(VideoCommon::IsEmptyDirectComputeDispatch(false, {~u32{0}, 1, 1}));
}
