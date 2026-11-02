// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include "common/common_types.h"

namespace VideoCommon {

// Refs are immutable upload allocations, valid for the current scheduler tick.
// Always compare freshly read bytes: guest address reuse is not content identity.
template <typename Ref>
class UniformUploadCache {
public:
    static constexpr u32 MaxBytes = 4096;
    static constexpr size_t Stages = 6; // Five graphics stages plus compute.
    static constexpr size_t Banks = 18;
    static constexpr size_t ComputeStage = 5;
    struct Result {
        Ref ref;
        bool reused;
    };

    template <typename Allocate>
    std::optional<Result> Get(size_t stage, size_t bank, u64 tick,
                              std::span<const u8> bytes, Allocate&& allocate) {
        if (stage >= Stages || bank >= Banks || bytes.empty() || bytes.size() > MaxBytes) {
            return std::nullopt;
        }
        auto& slot = slots[stage][bank];
        if (slot.size == bytes.size() && slot.tick == tick &&
            std::memcmp(slot.bytes.data(), bytes.data(), bytes.size()) == 0) {
            return Result{slot.ref, true};
        }
        // Allocate/copy first: failure must not publish a valid cache entry.
        const Ref ref = allocate(bytes);
        slot.ref = ref;
        std::memcpy(slot.bytes.data(), bytes.data(), bytes.size());
        slot.tick = tick;
        slot.size = static_cast<u32>(bytes.size());
        return Result{slot.ref, false};
    }

private:
    struct Slot {
        Ref ref{};
        u64 tick{};
        u32 size{};
        std::array<u8, MaxBytes> bytes{};
    };
    std::array<std::array<Slot, Banks>, Stages> slots{};
};

// Alignment may require streaming even a GPU-written uniform. Bring that data
// back before either comparing cached bytes or copying the upload allocation.
template <typename Download, typename Read>
decltype(auto) WithSynchronizedUniformRead(bool gpu_modified, Download&& download, Read&& read) {
    if (gpu_modified) {
        download();
    }
    return read();
}

} // namespace VideoCommon
