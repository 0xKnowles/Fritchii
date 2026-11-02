// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace VideoCommon {

template <typename Drain, typename WasWritten, typename Flush>
bool SynchronizeCommandFetch(Drain&& drain, WasWritten&& was_written, Flush&& flush) {
    // A queued writer can mark the page for the first time. Check only after it has run.
    drain();
    if (!was_written()) {
        return false;
    }
    flush();
    return true;
}

} // namespace VideoCommon
