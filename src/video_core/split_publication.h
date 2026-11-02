// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCommon {

// When the GPU split's front makes committed ring entries visible to the back (gpu_split.h).
//
// Publication changes only WHEN the back may start entries that are already committed, in order;
// it never changes which entries run, their order, their contents or what a drain waits for.
//
// * Full: `published` seq_cst store, then a seq_cst look at back_waiting (the Dekker pair with the
//   back's sleep). Only a full publication can wake a sleeping back. Every place where the front
//   waits for the back (DrainTo, WaitForSpace) or stops producing (end of DispatchCalls, each GPU
//   thread command) performs one first, so liveness never depends on a hint.
// * Hint: `published` release store of the same monotonically increasing position, without the
//   wake handshake. The back's acquire load of a hinted position synchronizes with it, so every
//   ring word written before the hint is visible to the back. A back that misses a hint (asleep or
//   already past its last look) is in the same state as before hints existed: that position is
//   simply unpublished until the next full publication.
//
// Why writers are hinted: the next command fetch drains to the latest possible command-memory
// writer (command_write_frontier.h). With publication only every 8..256 words, that writer was
// normally still unpublished when the drain began, so the idle back started executing it only
// once the front asked to wait for it. Hinting it when committed lets the back run it while the
// front finishes the entry and prepares the next fetch. The full-publication cadence for
// register-only runs is unchanged.
struct SplitPublication {
    static constexpr u64 MinimumInterval = 8;   ///< Words between full publications after a drain
    static constexpr u64 MaximumInterval = 256; ///< ...growing to this while no drain waits

    enum class Action { None, Hint, Full };

    /// What to do after committing an entry that ends at `write_position`. `published` is the last
    /// FULL publication: hints never postpone the next full one.
    [[nodiscard]] static constexpr Action AfterCommit(u64 write_position, u64 published,
                                                      u64 interval, bool may_write) {
        if (write_position - published >= interval) {
            return Action::Full;
        }
        return may_write ? Action::Hint : Action::None;
    }

    [[nodiscard]] static constexpr u64 NextInterval(u64 interval) {
        return interval * 2 < MaximumInterval ? interval * 2 : MaximumInterval;
    }

    /// Words a drain to `target` waits for that the back could not yet see when the drain began.
    [[nodiscard]] static constexpr u64 Withheld(u64 target, u64 published, u64 hinted) {
        const u64 visible = published > hinted ? published : hinted;
        return target > visible ? target - visible : 0;
    }
};

/// Front-thread totals for drains that had to wait (reporting only; never changes behaviour).
struct SplitPublicationMetrics {
    u64 pending_drains{};  ///< Drains that found their target not yet consumed
    u64 withheld_drains{}; ///< ... whose target the back could not yet see
    u64 withheld_words{};  ///< ... summed words not yet visible at drain start
    u64 asleep_drains{};   ///< ... that found the back sleeping (a wake syscall and its latency)

    void RecordPendingDrain(u64 withheld, bool back_asleep) {
        ++pending_drains;
        withheld_drains += withheld != 0;
        withheld_words += withheld;
        asleep_drains += back_asleep;
    }
};

} // namespace VideoCommon
