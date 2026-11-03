// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace VideoCommon::DlssContract {

constexpr std::uint64_t FastFusionProgramId = 0x040041E0212AE000ULL;
constexpr std::array FastFusionCandidates{0xb8ed9b1cd5c24e6dULL, 0x7f0f03c0529ca569ULL,
                                          0x7a06618494a29bcdULL};

constexpr bool IsFastFusionCandidate(std::uint64_t title, std::uint64_t shader) {
    if (title != FastFusionProgramId) {
        return false;
    }
    for (const auto candidate : FastFusionCandidates) {
        if (candidate == shader) {
            return true;
        }
    }
    return false;
}

constexpr bool FitsExtent(std::array<std::uint32_t, 2> requested,
                           std::array<std::uint32_t, 2> available) {
    return requested[0] <= available[0] && requested[1] <= available[1];
}

inline bool ValidGeometry(std::array<std::uint32_t, 2> render,
                          std::array<std::uint32_t, 2> output,
                          std::array<float, 2> jitter, std::array<float, 2> motion_scale) {
    return render[0] >= 2 && render[1] >= 2 && FitsExtent(render, output) &&
           std::isfinite(jitter[0]) && std::isfinite(jitter[1]) &&
           std::isfinite(motion_scale[0]) && std::isfinite(motion_scale[1]);
}

} // namespace VideoCommon::DlssContract
