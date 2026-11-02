// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"

namespace VideoCommon {

enum class SplitDrainReason : size_t {
    Other,
    CommandFetch,
    ParameterFlush,
    ParameterDirty,
    SubmissionFence,
    RingSpace,
    Count,
};

struct SplitDrainMetrics {
    struct Entry {
        u64 calls{};
        u64 pending_calls{};
        u64 queued_words{};
        u64 elapsed_ns{};
    };
    std::array<Entry, static_cast<size_t>(SplitDrainReason::Count)> drains{};
    u64 command_fetches{};
    u64 command_bytes{};
    u64 historical_hits{};
    u64 dirty_flushes{};
    u64 flushed_bytes{};

    void RecordDrain(SplitDrainReason reason, u64 queued_words, u64 elapsed_ns) {
        auto& entry = drains[static_cast<size_t>(reason)];
        ++entry.calls;
        entry.pending_calls += queued_words != 0;
        entry.queued_words += queued_words;
        entry.elapsed_ns += elapsed_ns;
    }

    void RecordCommandFetch(u64 bytes, bool historical_hit, bool flushed) {
        ++command_fetches;
        command_bytes += bytes;
        historical_hits += historical_hit;
        dirty_flushes += flushed;
        if (flushed) {
            flushed_bytes += bytes;
        }
    }
};

} // namespace VideoCommon
