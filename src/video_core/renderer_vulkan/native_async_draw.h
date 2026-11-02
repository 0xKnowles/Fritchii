// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include "shader_recompiler/shader_info.h"

namespace Vulkan {

// Async shaders deliberately permit temporary missing raster output. Never extend that
// approximation to a shader's memory writes, atomics, transform feedback or query results.
inline bool HasAsyncDrawSideEffects(const Shader::Info& info) {
    if (info.stores_global_memory || info.uses_global_memory || info.uses_global_subword_write ||
        info.uses_global_atomics || info.uses_global_increment || info.uses_global_decrement ||
        info.uses_typeless_image_writes || info.uses_atomic_image_u32 ||
        info.uses_atomic_f32_add || info.uses_atomic_f16x2_add || info.uses_atomic_f16x2_min ||
        info.uses_atomic_f16x2_max || info.uses_atomic_f32x2_add || info.uses_atomic_f32x2_min ||
        info.uses_atomic_f32x2_max || info.uses_atomic_s32_min || info.uses_atomic_s32_max ||
        info.uses_int64_bit_atomics) {
        return true;
    }
    const auto written = [](const auto& descriptor) { return descriptor.is_written; };
    return std::ranges::any_of(info.storage_buffers_descriptors, written) ||
           std::ranges::any_of(info.image_buffer_descriptors, written) ||
           std::ranges::any_of(info.image_descriptors, written);
}

struct NativeAsyncDrawState {
    bool async_enabled{};
    bool color_output{};
    bool depth_enabled{};
    bool memory_side_effects{};
    bool transform_feedback{};
    bool query_enabled{};
    u32 vertex_count{};
};

constexpr bool CanSkipNativePipelineBuild(const NativeAsyncDrawState& state) {
    // Zero/unknown counts and small fullscreen passes stay synchronous. Depth-only passes
    // also stay synchronous, since their output can be a one-shot shadow/visibility map.
    // Color+depth can still be rendered offscreen: this remains a visual approximation.
    return state.async_enabled && state.color_output && state.depth_enabled &&
           !state.memory_side_effects && !state.transform_feedback && !state.query_enabled &&
           state.vertex_count > 6;
}

} // namespace Vulkan
