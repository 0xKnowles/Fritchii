// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/common_types.h"

namespace VideoCommon {

// Producer-owned ring position of the latest entry that could affect command
// memory. Only non-executing sink appends and engine switches may be omitted.
// Direct macro writes, multi-methods and callbacks remain conservative hazards:
// even an ordinary direct state write may flush a deferred draw.
class CommandWriteFrontier {
public:
    void Commit(u64 end, bool may_write) {
        if (may_write) last_writer = end;
    }
    u64 Target() const { return last_writer; }
    void Reset() { last_writer = 0; }

private:
    u64 last_writer{};
};

} // namespace VideoCommon
