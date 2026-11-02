// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Tegra::Engines {

/// Only the inline QMD's first 63 words are side-effect-free state. Its last word
/// (method 0x107) uploads the descriptor and may launch it, even if unchanged.
[[nodiscard]] constexpr bool IsInlineQmdDataMethod(u32 method) {
    return method >= 0xc8 && method < 0x107;
}

/// The front shadow holds the last value queued to the back, not a cached guest
/// pointer. Diagnostics can classify an otherwise plain field as observable.
/// Do not broaden this to all plain methods: dispatch changes the legacy
/// code_loc registers without changing the front shadow (methods 0x582/0x583).
[[nodiscard]] constexpr bool CanSkipInlineQmdWrite(u32 method, u32 previous, u32 value,
                                                bool plain_method) {
    return IsInlineQmdDataMethod(method) && plain_method && previous == value;
}

} // namespace Tegra::Engines
