// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/command_fetch_sync_audit.h"
#include "video_core/command_write_frontier.h"
#include "video_core/nx2.h"

namespace {

using Audit = VideoCommon::CommandFetchSyncAudit;
constexpr u64 Page = u64{1} << Tegra::Nx2WrittenPageBits;

// A fake guest: `guest` is what an unflushed read sees, `gpu` what queued/host GPU work wrote.
struct FakeFetch {
    std::vector<u8> guest;
    std::vector<u8> gpu;
    bool pending = false;
    bool mark_on_drain = false; // The queued writer's first write to this page.
    u64 address = 0;
    int drains = 0;
    int flushes = 0;
    int reads = 0;
    u64 clock = 0;
    std::vector<const char*> order;

    bool Run(Audit& audit, bool sync_wait, bool contract = false) {
        return Tegra::Nx2SynchronizeGpuWrittenCommandsAudited(
            address, guest.size(), sync_wait, audit, [&] { return pending; },
            [&] {
                order.push_back("drain");
                ++drains;
                if (pending && mark_on_drain) {
                    Tegra::Nx2MarkGpuWritten(address, guest.size());
                }
                pending = false;
                clock += 700;
            },
            [&](GPUVAddr page) {
                order.push_back("check");
                return std::optional<u64>{page};
            },
            [&] {
                order.push_back("flush");
                ++flushes;
                guest = gpu;
            },
            [&] { return clock; }, contract);
    }
};

} // Anonymous namespace

TEST_CASE("Audited command sync still drains before the historical test of a first write",
          "[video_core][sync_audit]") {
    Tegra::Nx2ClearGpuWrittenPages();
    Audit audit;
    FakeFetch fetch;
    fetch.address = 40 * Page;
    fetch.guest.assign(20, 0x11);
    fetch.gpu.assign(20, 0x42);
    fetch.pending = true;
    fetch.mark_on_drain = true;

    // No SYNC_WAIT and no known GPU-written page: a candidate, but it is synchronized anyway.
    REQUIRE(fetch.Run(audit, false));
    REQUIRE(fetch.drains == 1);
    REQUIRE(fetch.flushes == 1);
    REQUIRE(fetch.guest == fetch.gpu);
    // Pre-check, drain, post-check, flush: the historical test that decides the flush follows
    // the drain.
    const auto drain_at = std::find(fetch.order.begin(), fetch.order.end(),
                                    std::string_view{"drain"}) - fetch.order.begin();
    const auto flush_at = std::find(fetch.order.begin(), fetch.order.end(),
                                    std::string_view{"flush"}) - fetch.order.begin();
    REQUIRE(drain_at < flush_at);
    REQUIRE(fetch.order[static_cast<size_t>(flush_at) - 1] == std::string_view{"check"});

    REQUIRE(audit.fetches == 1);
    REQUIRE(audit.candidates == 1);
    REQUIRE(audit.candidate_waits == 1);
    REQUIRE(audit.candidate_wait_ns == 700);
    REQUIRE(audit.first_write_hits == 1);
    REQUIRE(fetch.reads == 0); // Diagnostics never speculatively read command memory.
}

TEST_CASE("SYNC_WAIT and known GPU-written fetches are not candidates but still synchronize",
          "[video_core][sync_audit]") {
    Tegra::Nx2ClearGpuWrittenPages();
    Audit audit;

    FakeFetch synced;
    synced.address = 50 * Page;
    synced.guest.assign(8, 1);
    synced.gpu.assign(8, 2);
    synced.pending = true;
    synced.mark_on_drain = true;
    REQUIRE(synced.Run(audit, true));
    REQUIRE(synced.drains == 1);
    REQUIRE(synced.guest == synced.gpu);
    REQUIRE(synced.reads == 0); // Not a candidate: never copied or timed.

    FakeFetch known;
    known.address = 60 * Page;
    known.guest.assign(8, 3);
    known.gpu.assign(8, 4);
    known.pending = true;
    Tegra::Nx2MarkGpuWritten(known.address, 8);
    REQUIRE(known.Run(audit, false));
    REQUIRE(known.drains == 1);
    REQUIRE(known.flushes == 1);
    REQUIRE(known.guest == known.gpu);

    REQUIRE(audit.fetches == 2);
    REQUIRE(audit.sync_fetches == 1);
    REQUIRE(audit.known_before == 1);
    REQUIRE(audit.candidates == 0);
    REQUIRE(audit.candidate_waits == 0);
    REQUIRE(audit.first_write_hits == 0);
}

TEST_CASE("Candidates without queued writers drain, are not timed and keep unchanged words",
          "[video_core][sync_audit]") {
    Tegra::Nx2ClearGpuWrittenPages();
    Audit audit;
    FakeFetch fetch;
    fetch.address = 70 * Page;
    fetch.guest.assign(12, 7);
    fetch.gpu = fetch.guest;
    REQUIRE_FALSE(fetch.Run(audit, false));
    REQUIRE(fetch.drains == 1);
    REQUIRE(fetch.flushes == 0);
    REQUIRE(fetch.reads == 0);
    REQUIRE(audit.candidates == 1);
    REQUIRE(audit.candidate_waits == 0);
    REQUIRE(audit.candidate_wait_ns == 0);

    // A pending candidate is timed without inspecting unsynchronized command words.
    fetch.pending = true;
    REQUIRE_FALSE(fetch.Run(audit, false));
    REQUIRE(audit.candidate_waits == 1);
    REQUIRE(fetch.reads == 0);
    REQUIRE(audit.first_write_hits == 0);
}

TEST_CASE("Empty command fetches are neither synchronized nor counted",
          "[video_core][sync_audit]") {
    Audit audit;
    FakeFetch fetch;
    fetch.pending = true;
    REQUIRE_FALSE(fetch.Run(audit, false));
    REQUIRE(fetch.drains == 0);
    REQUIRE(audit.fetches == 0);
}

TEST_CASE("Large command candidates are counted without speculative memory copies",
          "[video_core][sync_audit]") {
    Tegra::Nx2ClearGpuWrittenPages();
    Audit fresh;
    FakeFetch big;
    big.address = 80 * Page;
    big.guest.assign(65540, 1);
    big.gpu = big.guest;
    big.pending = true;
    big.Run(fresh, false);
    REQUIRE(big.reads == 0);
    REQUIRE(fresh.candidate_waits == 1);
    REQUIRE(fresh.fetches == 1);
}

TEST_CASE("Acquire polling frontier covers queued releases but not trailing state writes",
          "[video_core][sync_audit]") {
    // CanResumeAcquire drains to this target. A release, report, copy or callback is a writer;
    // proven register-only writes after it may keep running while the semaphore word is read.
    VideoCommon::CommandWriteFrontier frontier;
    frontier.Commit(3, true);  // Deferred semaphore release (Defer: always a writer).
    frontier.Commit(4, false); // Sink append.
    frontier.Commit(5, VideoCommon::CommandWriteFrontier::StateWriteMayExecute(true, false));
    REQUIRE(frontier.Target() == 3);
    // A state write while a draw may be pending can issue that draw: it stays a writer.
    frontier.Commit(6, VideoCommon::CommandWriteFrontier::StateWriteMayExecute(true, true));
    REQUIRE(frontier.Target() == 6);
}

TEST_CASE("Opt-in SYNC_WAIT contract skips only unsynchronized candidates",
          "[video_core][sync_audit]") {
    Tegra::Nx2ClearGpuWrittenPages();
    Audit audit;
    FakeFetch candidate;
    candidate.address = 90 * Page;
    candidate.guest.assign(16, 5);
    candidate.gpu.assign(16, 6);
    candidate.pending = true;
    candidate.mark_on_drain = true;
    // The documented risk: a queued FIRST GPU write to these pages is not waited for.
    REQUIRE_FALSE(candidate.Run(audit, false, true));
    REQUIRE(candidate.drains == 0);
    REQUIRE(candidate.flushes == 0);
    REQUIRE(candidate.guest != candidate.gpu);
    REQUIRE(candidate.reads == 0);
    REQUIRE(audit.fetches == 1);
    REQUIRE(audit.candidates == 1);
    REQUIRE(audit.contract_skips == 1);
    REQUIRE(audit.candidate_waits == 0);

    // SYNC_WAIT fetches keep the full drain-then-test ordering.
    FakeFetch synced;
    synced.address = 100 * Page;
    synced.guest.assign(8, 1);
    synced.gpu.assign(8, 2);
    synced.pending = true;
    synced.mark_on_drain = true;
    REQUIRE(synced.Run(audit, true, true));
    REQUIRE(synced.drains == 1);
    REQUIRE(synced.flushes == 1);
    REQUIRE(synced.guest == synced.gpu);

    // Pages already known to be GPU-written (culling-filled command lists) keep it too.
    FakeFetch known;
    known.address = 110 * Page;
    known.guest.assign(8, 3);
    known.gpu.assign(8, 4);
    known.pending = true;
    Tegra::Nx2MarkGpuWritten(known.address, 8);
    REQUIRE(known.Run(audit, false, true));
    REQUIRE(known.drains == 1);
    REQUIRE(known.flushes == 1);
    REQUIRE(known.guest == known.gpu);

    REQUIRE(audit.fetches == 3);
    REQUIRE(audit.contract_skips == 1);
    REQUIRE(audit.sync_fetches == 1);
    REQUIRE(audit.known_before == 1);
}

TEST_CASE("Contract mode off is the exact default ordering", "[video_core][sync_audit]") {
    Tegra::Nx2ClearGpuWrittenPages();
    Audit audit;
    FakeFetch fetch;
    fetch.address = 120 * Page;
    fetch.guest.assign(20, 0x11);
    fetch.gpu.assign(20, 0x42);
    fetch.pending = true;
    fetch.mark_on_drain = true;
    REQUIRE(fetch.Run(audit, false, false));
    REQUIRE(fetch.drains == 1);
    REQUIRE(fetch.guest == fetch.gpu);
    REQUIRE(audit.contract_skips == 0);
    REQUIRE(audit.first_write_hits == 1);
}
