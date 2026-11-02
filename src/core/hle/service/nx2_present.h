// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>

#include "common/common_types.h"
#include "common/math_util.h"

namespace Service {

// Present word 12: (swap interval << 16) | refresh rate. Default-path
// interpretation from upstream f8d3860; no forced-interval override is needed.
inline std::chrono::nanoseconds Nx2PresentInterval(u32 timing, u32 speed_limit) {
    if (speed_limit == 0) {
        return std::chrono::nanoseconds{0};
    }
    const u32 swap_interval = std::clamp<u32>(timing >> 16, 1, 4);
    const u32 rate = timing & 0xffff;
    const u32 refresh_rate = rate >= 30 && rate <= 240 ? rate : 60;
    return std::chrono::nanoseconds{100'000'000'000LL * swap_interval / refresh_rate / speed_limit};
}

inline Common::Rectangle<int> Nx2PresentCrop(u32 origin, u32 end, u32 flags, u32 width,
                                            u32 height) {
    const Common::Rectangle<int> full{0, 0, static_cast<int>(width), static_cast<int>(height)};
    if ((flags & 2) == 0) {
        return full;
    }
    const u32 left = std::min(origin & 0xffff, width);
    const u32 top = std::min(origin >> 16, height);
    const u32 right = std::min((end & 0xffff) + 1, width);
    const u32 bottom = std::min((end >> 16) + 1, height);
    if (left >= right || top >= bottom) {
        return full;
    }
    return {static_cast<int>(left), static_cast<int>(top), static_cast<int>(right),
            static_cast<int>(bottom)};
}

} // namespace Service
