// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "core/memory.h"
#include "video_core/rasterizer_download_area.h"

namespace VideoCommon {

/// A texture may require a much wider flush than the CPU read that triggered it.
/// Keep that flush intact, but only let CPU read caches reuse pages actually observed:
/// an unseen page must still notify the buffer cache before its read trap is disarmed.
/// Call after flushing, never use this narrower range as the actual FlushRegion input.
[[nodiscard]] inline VideoCore::RasterizerDownloadArea LimitReadbackReuseCoverage(
    VideoCore::RasterizerDownloadArea flushed, DAddr address, u64 size) {
    constexpr u64 mask = Core::Memory::YUZU_PAGEMASK;
    if (size == 0 || size > ~u64{0} - address || address + size > ~u64{0} - mask) {
        return {};
    }
    const DAddr begin = address & ~mask;
    const DAddr end = (address + size + mask) & ~mask;
    flushed.start_address = (std::max)(flushed.start_address, begin);
    flushed.end_address = (std::min)(flushed.end_address, end);
    if (flushed.start_address >= flushed.end_address) {
        return {};
    }
    return flushed;
}

} // namespace VideoCommon
