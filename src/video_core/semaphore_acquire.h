// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include "common/common_types.h"

namespace Tegra {

// The first three values retain the legacy SEMAPHORE_OPERATION encoding. The remaining
// values are internal predicates, not hardware operation numbers.
enum class SemaphoreAcquire : u32 {
    Equal = 1,
    GreaterEqual = 4,
    Mask = 8,
    CircularGreaterEqual = 16,
    MaskZero = 32,
};

[[nodiscard]] constexpr bool SemaphoreMatches(u32 operation, u32 current, u32 expected) {
    switch (static_cast<SemaphoreAcquire>(operation)) {
    case SemaphoreAcquire::Equal:
        return current == expected;
    case SemaphoreAcquire::GreaterEqual:
        return current >= expected;
    case SemaphoreAcquire::Mask:
        return (current & expected) != 0;
    case SemaphoreAcquire::CircularGreaterEqual:
        // Modular subtraction is intentional; avoid signed overflow at counter wraparound.
        return (current - expected) < 0x80000000U;
    case SemaphoreAcquire::MaskZero:
        return (current & expected) == 0;
    default:
        return false;
    }
}

// AMPERE_CHANNEL_GPFIFO SEM_EXECUTE has a different opcode layout from the legacy method.
// Wide-payload commands must not accidentally enter this 32-bit acquire path.
[[nodiscard]] constexpr std::optional<SemaphoreAcquire> DecodeSemaphoreAcquire32(u32 argument) {
    if ((argument & (1U << 24)) != 0) {
        return std::nullopt;
    }
    switch (argument & 7) {
    case 0: return SemaphoreAcquire::Equal;
    case 2: return SemaphoreAcquire::GreaterEqual;
    case 3: return SemaphoreAcquire::CircularGreaterEqual;
    case 4: return SemaphoreAcquire::Mask;
    case 5: return SemaphoreAcquire::MaskZero;
    default: return std::nullopt;
    }
}

} // namespace Tegra
