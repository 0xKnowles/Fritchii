// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/common_types.h"

namespace VideoCommon {

// Producer-owned ring position of the latest entry that could affect command
// memory. Inert sink appends, engine switches and proven state-only writes may be
// omitted. A direct 3D state write is NOT inert if it could flush a deferred draw.
// Uploads, dispatches, draws, unknown methods and callbacks remain hazards.
class CommandWriteFrontier {
public:
    static constexpr bool StateWriteMayExecute(bool state_only, bool deferred_draw_pending) {
        return !state_only || deferred_draw_pending;
    }

    void Commit(u64 end, bool may_write) {
        if (may_write) last_writer = end;
    }
    u64 Target() const { return last_writer; }
    void Reset() { last_writer = 0; }

private:
    u64 last_writer{};
};

} // namespace VideoCommon
