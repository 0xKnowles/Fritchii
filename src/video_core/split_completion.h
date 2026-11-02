// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include "common/common_types.h"

namespace VideoCommon {

// One front-side requester and one back-side publisher. The request is only a
// notification hint; visibility of completed work still uses consumed's
// release/acquire pair. Never acknowledge work merely because it was requested.
class SplitCompletion {
public:
    void Request(u64 position) {
        requested.store(position, std::memory_order_relaxed);
    }

    bool ShouldPublish(u64 completed, u64 last_published) const {
        const u64 target = requested.load(std::memory_order_relaxed);
        return completed - last_published >= 4096 ||
               (last_published < target && completed >= target);
    }

    // Only after the back thread has stopped.
    void Reset() {
        requested.store(0, std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<u64> requested{};
};

} // namespace VideoCommon
