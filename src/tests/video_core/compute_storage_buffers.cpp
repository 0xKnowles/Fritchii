// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <limits>
#include <tuple>
#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/buffer_cache_base.h"

TEST_CASE("Compute storage bindings cover every shader descriptor and mask bit",
          "[video_core][compute_storage]") {
    using State = VideoCommon::BufferCacheChannelInfo;
    constexpr size_t count = std::tuple_size_v<decltype(State::compute_storage_buffers)>;
    STATIC_REQUIRE(count == Shader::Info::MAX_SSBOS);
    STATIC_REQUIRE(count <= std::numeric_limits<decltype(State::enabled_compute_storage_buffers)>::digits);
    STATIC_REQUIRE(count <= std::numeric_limits<decltype(State::written_compute_storage_buffers)>::digits);
    std::array<VideoCommon::Binding, count> bindings{};
    u32 enabled{};
    u32 written{};
    for (u32 index = 0; index < count; ++index) {
        bindings[index].size = index + 1;
        enabled |= 1U << index;
        written |= (index % 2) << index;
    }
    size_t visited{};
    while (enabled != 0) {
        const u32 index = std::countr_zero(enabled);
        REQUIRE(bindings[index].size == index + 1);
        REQUIRE(((written >> index) & 1) == index % 2);
        enabled &= enabled - 1;
        ++visited;
    }
    REQUIRE(visited == count);
}

TEST_CASE("Compute storage rebind clears a disabled source without shifting descriptor slots",
          "[video_core][compute_storage]") {
    using namespace VideoCommon;
    std::array<Binding, 3> bindings{NULL_BINDING, NULL_BINDING, NULL_BINDING};
    unsigned resolved = 0;
    const auto resolve = [&] {
        ++resolved;
        Binding result = NULL_BINDING;
        result.device_addr = 0x20000;
        result.size = 4096;
        result.gpu_addr = 0x40000;
        result.segment_first = 7;
        result.segment_count = 2;
        return result;
    };
    RefreshComputeStorageBinding(bindings[1], true, resolve);
    REQUIRE(resolved == 1);
    REQUIRE(bindings[1].device_addr == 0x20000);
    bindings[2].device_addr = 0x80000;
    bindings[2].size = 128;

    RefreshComputeStorageBinding(bindings[1], false, resolve);
    REQUIRE(resolved == 1);
    REQUIRE(bindings[1].device_addr == 0);
    REQUIRE(bindings[1].size == 0);
    REQUIRE(bindings[1].buffer_id == NULL_BUFFER_ID);
    REQUIRE(bindings[1].gpu_addr == 0);
    REQUIRE(bindings[1].segment_first == 0);
    REQUIRE(bindings[1].segment_count == 0);
    REQUIRE(bindings[2].device_addr == 0x80000);
    REQUIRE(bindings[2].size == 128);
    RefreshComputeStorageBinding(bindings[1], true, resolve);
    REQUIRE(resolved == 2);
    REQUIRE(bindings[1].device_addr == 0x20000);
}
