// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"

namespace VideoCommon {

/// Indirect dimensions live in GPU memory, so the QMD's direct fields cannot
/// prove an indirect dispatch empty. This only suppresses shader/resource work;
/// the caller must retain preceding invalidations and synchronization.
[[nodiscard]] constexpr bool IsEmptyDirectComputeDispatch(
    bool indirect, const std::array<u32, 3>& dimensions) {
    return !indirect && (dimensions[0] == 0 || dimensions[1] == 0 || dimensions[2] == 0);
}

} // namespace VideoCommon
