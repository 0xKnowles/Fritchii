// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCommon {

// Evidence for (not an implementation of) a hardware-ordered command-fetch policy.
//
// NVIDIA's PBDMA manual (open-gpu-doc, dev_pbdma.ref, NV_PPBDMA_GP_ENTRY1_SYNC): with
// SYNC_PROCEED, Host may fetch a pushbuffer segment before it has processed the preceding one.
// Even SYNC_WAIT does not wait for engine work. Software that needs engine results first must
// put an engine-idle wait (WFI, SET_REF, SEM_EXECUTE with RELEASE_WFI) or an engine-released
// semaphore acquire in the preceding segment AND set SYNC_WAIT on the consuming segment. A
// guest that reads engine-written commands without SYNC_WAIT therefore races on hardware.
//
// A "candidate" is a fetch that the contract would not order behind queued engine work: its
// GP entry has no SYNC_WAIT and none of its pages was known to be GPU-written before
// synchronizing. The emulator still synchronizes every candidate (first-write protection is
// unchanged); these counters measure wait cost and newly observed writer pages, NOT whether
// skipping a wait is safe. Command contents are not sampled before synchronization because
// reading them can race with queued host-side writers.
struct CommandFetchSyncAudit {

    struct Fetch {
        bool sync_wait{};                ///< GP entry SYNC bit (bit 63)
        bool known_gpu_written_before{}; ///< Historical page hit before synchronizing
        bool writers_pending{};          ///< Queued possible writers before synchronizing
        bool gpu_written_after{};        ///< Historical page hit after synchronizing
        u64 wait_ns{};                   ///< Measured only for pending candidates
    };

    [[nodiscard]] static constexpr bool IsCandidate(bool sync_wait,
                                                    bool known_gpu_written_before) {
        return !sync_wait && !known_gpu_written_before;
    }

    void Record(const Fetch& fetch) {
        ++fetches;
        sync_fetches += fetch.sync_wait;
        known_before += fetch.known_gpu_written_before;
        if (!IsCandidate(fetch.sync_wait, fetch.known_gpu_written_before)) {
            return;
        }
        ++candidates;
        if (fetch.writers_pending) {
            ++candidate_waits;
            candidate_wait_ns += fetch.wait_ns;
        }
        // The page became GPU-written only while this fetch synchronized: a queued first write.
        first_write_hits += fetch.gpu_written_after;
    }

    u64 fetches{};
    u64 sync_fetches{};
    u64 known_before{};
    u64 candidates{};
    u64 candidate_waits{};
    u64 candidate_wait_ns{};
    u64 first_write_hits{};
};

} // namespace VideoCommon
