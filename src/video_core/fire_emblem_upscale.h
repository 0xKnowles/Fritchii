// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>
#include "common/common_types.h"
#include "video_core/textures/texture.h"

namespace VideoCommon::FireEmblemUpscale {

constexpr u64 ProgramId = 0x04007EF01F8AE000ULL;
// NativeProgramKey of the captured DLSS final output pass, not its lowered shader hash.
constexpr u64 OutputProgramKey = 0x14b4222bbe23abeeULL;
constexpr u64 PrepassProgramKey = 0x4cbf6eff32525aecULL;

struct JitterSample {
    GPUVAddr color{};
    u32 width{}, height{};
    std::array<f32, 2> offset{};
};

class FrameJitter {
public:
    void Capture(GPUVAddr color, u32 width, u32 height, std::array<f32, 2> offset) {
        sample.reset();
        if (color == 0 || width == 0 || height == 0 ||
            !std::isfinite(offset[0]) || !std::isfinite(offset[1]) ||
            std::abs(offset[0]) > 0.5f || std::abs(offset[1]) > 0.5f) {
            return;
        }
        sample = JitterSample{color, width, height, offset};
    }

    // Consume on every output attempt, even if its resources cannot be used.
    std::optional<JitterSample> Take() {
        return std::exchange(sample, std::nullopt);
    }

private:
    std::optional<JitterSample> sample;
};

inline bool UseReplacement(u64 program_id, u64 native_key, const char* option) {
    return program_id == ProgramId && native_key == OutputProgramKey &&
           (option == nullptr || (std::string_view{option} == "fsr" || std::string_view{option} == "dlss"));
}

inline bool ValidPair(const Tegra::Texture::TICEntry& input,
                      const Tegra::Texture::TICEntry& output) {
    using namespace Tegra::Texture;
    const auto plain_image = [](const TICEntry& image) {
        return (image.texture_type == TextureType::Texture2D ||
                image.texture_type == TextureType::Texture2DNoMipmap) &&
               image.format == TextureFormat::B10G11R11 && image.IsBlockLinear() &&
               image.Depth() == 1 && image.BaseLayer() == 0 &&
               image.max_mip_level == 0 && image.Address() != 0;
    };
    return plain_image(input) && plain_image(output) &&
           input.Address() != output.Address() && output.Width() > input.Width() &&
           output.Height() > input.Height();
}

// A successful substitute dispatch, not a shader rejection. Never cache it as a failure.
struct DispatchReplaced {};

} // namespace VideoCommon::FireEmblemUpscale
