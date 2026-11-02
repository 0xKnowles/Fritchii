// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "video_core/renderer_vulkan/native_async_draw.h"

TEST_CASE("Native async omission requires every raster eligibility guard", "[native_async_draw]") {
    for (u32 bits = 0; bits < 128; ++bits) {
        const Vulkan::NativeAsyncDrawState state{
            .async_enabled = (bits & 1) != 0,
            .color_output = (bits & 2) != 0,
            .depth_enabled = (bits & 4) != 0,
            .memory_side_effects = (bits & 8) != 0,
            .transform_feedback = (bits & 16) != 0,
            .query_enabled = (bits & 32) != 0,
            .vertex_count = (bits & 64) ? 30U : 6U,
        };
        CHECK(Vulkan::CanSkipNativePipelineBuild(state) == (bits == (1 | 2 | 4 | 64)));
    }
}

TEST_CASE("Unknown and small native draws never skip compilation", "[native_async_draw]") {
    Vulkan::NativeAsyncDrawState state{.async_enabled = true, .color_output = true,
                                     .depth_enabled = true};
    for (u32 count = 0; count <= 6; ++count) {
        state.vertex_count = count;
        CHECK_FALSE(Vulkan::CanSkipNativePipelineBuild(state));
    }
    state.vertex_count = 7;
    CHECK(Vulkan::CanSkipNativePipelineBuild(state));
}

TEST_CASE("Native shader global and atomic effects prohibit async omission", "[native_async_draw]") {
    Shader::Info info{};
    CHECK_FALSE(Vulkan::HasAsyncDrawSideEffects(info));
    for (const auto flag : {
             &Shader::Info::stores_global_memory, &Shader::Info::uses_global_memory,
             &Shader::Info::uses_global_subword_write, &Shader::Info::uses_global_atomics,
             &Shader::Info::uses_global_increment, &Shader::Info::uses_global_decrement,
             &Shader::Info::uses_typeless_image_writes, &Shader::Info::uses_atomic_image_u32,
             &Shader::Info::uses_atomic_f32_add, &Shader::Info::uses_atomic_f16x2_add,
             &Shader::Info::uses_atomic_f16x2_min, &Shader::Info::uses_atomic_f16x2_max,
             &Shader::Info::uses_atomic_f32x2_add, &Shader::Info::uses_atomic_f32x2_min,
             &Shader::Info::uses_atomic_f32x2_max, &Shader::Info::uses_atomic_s32_min,
             &Shader::Info::uses_atomic_s32_max, &Shader::Info::uses_int64_bit_atomics}) {
        info.*flag = true;
        CHECK(Vulkan::HasAsyncDrawSideEffects(info));
        info.*flag = false;
    }
}

TEST_CASE("Native shader descriptor writers prohibit async omission", "[native_async_draw]") {
    Shader::Info info{};
    info.storage_buffers_descriptors.push_back({});
    info.image_buffer_descriptors.push_back({});
    info.image_descriptors.push_back({});
    CHECK_FALSE(Vulkan::HasAsyncDrawSideEffects(info));
    info.storage_buffers_descriptors.back().is_written = true;
    CHECK(Vulkan::HasAsyncDrawSideEffects(info));
    info.storage_buffers_descriptors.back().is_written = false;
    info.image_buffer_descriptors.back().is_written = true;
    CHECK(Vulkan::HasAsyncDrawSideEffects(info));
    info.image_buffer_descriptors.back().is_written = false;
    info.image_descriptors.back().is_written = true;
    CHECK(Vulkan::HasAsyncDrawSideEffects(info));
}
