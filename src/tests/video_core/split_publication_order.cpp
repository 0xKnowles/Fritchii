// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Writer-hint publication of the GPU split ring (video_core/split_publication.h).
//
// The production GpuSplit needs a Core::System and the engines, so these tests drive the same
// policy functions, CommandWriteFrontier, SplitCompletion and the same atomic protocol (seq_cst
// full publish + back_waiting/wake Dekker pair, release-only hints, release/acquire consumed) in a
// model with a 64-word ring. They establish the ordering and visibility properties of the
// protocol; they do not measure in-game performance.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/command_fetch_sync_audit.h"
#include "video_core/command_write_frontier.h"
#include "video_core/nx2.h"
#include "video_core/split_completion.h"
#include "video_core/split_publication.h"

namespace {

using VideoCommon::CommandWriteFrontier;
using VideoCommon::SplitCompletion;
using VideoCommon::SplitPublication;
using Action = SplitPublication::Action;

TEST_CASE("Split publication hints only writers and keeps the full cadence",
          "[video_core][split_publication]") {
    // Below the interval: a writer is hinted, a register-only entry stays unpublished.
    REQUIRE(SplitPublication::AfterCommit(5, 0, 8, true) == Action::Hint);
    REQUIRE(SplitPublication::AfterCommit(5, 0, 8, false) == Action::None);
    // At the interval both get a full (wake-checked) publication, as before hints.
    REQUIRE(SplitPublication::AfterCommit(8, 0, 8, false) == Action::Full);
    REQUIRE(SplitPublication::AfterCommit(8, 0, 8, true) == Action::Full);
    REQUIRE(SplitPublication::AfterCommit(300, 100, 256, false) == Action::None);
    REQUIRE(SplitPublication::AfterCommit(356, 100, 256, false) == Action::Full);
    // Ring positions are monotonic u64s, not ring indices: nothing special at a wrap.
    constexpr u64 ring_end = u64{1} << 20;
    REQUIRE(SplitPublication::AfterCommit(ring_end + 3, ring_end - 2, 8, true) == Action::Hint);
    REQUIRE(SplitPublication::AfterCommit(ring_end + 6, ring_end - 2, 8, false) == Action::Full);

    REQUIRE(SplitPublication::NextInterval(SplitPublication::MinimumInterval) == 16);
    REQUIRE(SplitPublication::NextInterval(128) == 256);
    REQUIRE(SplitPublication::NextInterval(256) == SplitPublication::MaximumInterval);

    REQUIRE(SplitPublication::Withheld(10, 4, 0) == 6);
    REQUIRE(SplitPublication::Withheld(10, 4, 10) == 0);
    REQUIRE(SplitPublication::Withheld(10, 12, 3) == 0);
    REQUIRE(SplitPublication::Withheld(0, 0, 0) == 0);

    VideoCommon::SplitPublicationMetrics metrics;
    metrics.RecordPendingDrain(0, false);
    metrics.RecordPendingDrain(7, true);
    REQUIRE(metrics.pending_drains == 2);
    REQUIRE(metrics.withheld_drains == 1);
    REQUIRE(metrics.withheld_words == 7);
    REQUIRE(metrics.asleep_drains == 1);
}

// Single-threaded replay of GpuSplit's front bookkeeping (Commit, Publish, DrainTo's publish and
// interval reset, DrainMemoryWriters' trailing Publish). The back completes a drain instantly.
struct FrontModel {
    bool hints{};
    u64 write_position{};
    u64 published{};
    u64 hinted{};
    u64 consumed{};
    u64 interval{SplitPublication::MaximumInterval};
    CommandWriteFrontier frontier;
    std::vector<u64> full;
    std::vector<u64> hint_positions;
    u64 max_withheld{};
    bool ok = true;

    void Publish() {
        if (published == write_position) return;
        published = write_position;
        interval = SplitPublication::NextInterval(interval);
        full.push_back(write_position);
    }
    void Commit(u64 words, bool may_write) {
        write_position += words;
        frontier.Commit(write_position, may_write);
        switch (SplitPublication::AfterCommit(write_position, published, interval, may_write)) {
        case Action::Full:
            Publish();
            break;
        case Action::Hint:
            if (hints) {
                hinted = write_position;
                hint_positions.push_back(write_position);
            }
            break;
        case Action::None:
            break;
        }
        ok &= published <= write_position && hinted <= write_position;
        ok &= write_position - published < interval || published == write_position;
    }
    void DrainMemoryWriters() {
        const u64 target = frontier.Target();
        if (consumed < target) {
            max_withheld = std::max(max_withheld,
                                    SplitPublication::Withheld(target, published, hinted));
            Publish();
            consumed = target; // The writer ran; the register-only tail may still be queued.
            interval = SplitPublication::MinimumInterval;
        }
        Publish(); // Do not starve the inert tail.
        ok &= published == write_position && consumed <= published;
    }
};

TEST_CASE("Writer hints never withhold the drained writer and leave every full publication intact",
          "[video_core][split_publication]") {
    // All 5^8 sequences of: state (1 or 3 words), writer (1 or 3 words), command fetch drain.
    constexpr size_t Length = 8;
    constexpr size_t Kinds = 5;
    size_t total = 1;
    for (size_t i = 0; i < Length; ++i) total *= Kinds;
    bool legacy_withheld_seen = false;
    bool all_ok = true;
    for (size_t sequence = 0; sequence < total; ++sequence) {
        FrontModel legacy;
        FrontModel hinted;
        hinted.hints = true;
        size_t digits = sequence;
        for (size_t step = 0; step < Length; ++step) {
            const size_t kind = digits % Kinds;
            digits /= Kinds;
            for (FrontModel* model : {&legacy, &hinted}) {
                switch (kind) {
                case 0: model->Commit(1, false); break;
                case 1: model->Commit(3, false); break;
                case 2: model->Commit(1, true); break;
                case 3: model->Commit(3, true); break;
                default: model->DrainMemoryWriters(); break;
                }
            }
            // The frontier never lags behind a hint: a hinted position is a committed writer.
            all_ok &= hinted.frontier.Target() == legacy.frontier.Target();
            all_ok &= hinted.hinted <= hinted.frontier.Target();
        }
        all_ok &= legacy.ok && hinted.ok;
        // Targeted change only: identical full publications (wake handshakes), identical
        // frontier, and the drained writer was always visible before its drain began.
        all_ok &= legacy.full == hinted.full;
        all_ok &= hinted.max_withheld == 0;
        legacy_withheld_seen |= legacy.max_withheld != 0;
    }
    REQUIRE(all_ok);
    // The previous policy did withhold a drained writer from the back (what the hints remove).
    REQUIRE(legacy_withheld_seen);
}

// ---- Threaded protocol model ----------------------------------------------------------------

constexpr u64 RingWords = 64;
constexpr u64 Page = u64{1} << Tegra::Nx2WrittenPageBits;
constexpr GPUVAddr CommandBase = 0x9'0000'0000ULL; // Inside the 36-bit history space
constexpr u32 CommandPages = 4;

constexpr u64 Payload(u64 seq, u64 word) {
    return seq * 0x9E3779B97F4A7C15ULL + word * 0x632BE59BD9B4E019ULL + 1;
}

struct Shared {
    std::array<u64, RingWords> ring{}; // Plain words: visibility comes only from published/consumed
    alignas(64) std::atomic<u64> published{};
    alignas(64) std::atomic<u64> consumed{};
    SplitCompletion completion;
    alignas(64) std::atomic<bool> back_waiting{};
    std::atomic<u32> wake{};
    // "GPU memory" behind the command pages: written by queued writers on the back, read by the
    // front only after a drain (plain, so a missing happens-before is a real data race).
    std::array<u64, CommandPages> gpu_words{};
    // Back-only results, read after join.
    bool payload_ok = true;
    bool order_ok = true;
    u64 executed{};
    std::atomic<u64> sleeps{};

    // GpuSplit::Stop after joining the back thread.
    void ResetSession() {
        published.store(0, std::memory_order_relaxed);
        consumed.store(0, std::memory_order_relaxed);
        back_waiting.store(false, std::memory_order_relaxed);
        completion.Reset();
        gpu_words = {};
        payload_ok = true;
        order_ok = true;
        executed = 0;
    }
};

// Entry: word 0 = 1 | may_write << 1 | page << 2 | size << 16 | seq << 32; payload follows.
// Word 0 == 0 is Wrap: continue at the ring's start (GpuSplit::Kind::Wrap).
void BackLoop(Shared& s, std::stop_token stop) {
    u64 read = 0;
    u64 expected_seq = 1;
    while (!stop.stop_requested()) {
        u64 available = s.published.load(std::memory_order_acquire);
        if (available == read) {
            for (u32 spins = 0; spins < 64 && available == read; ++spins) {
                std::this_thread::yield();
                available = s.published.load(std::memory_order_acquire);
            }
            if (available == read) {
                s.back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = s.wake.load(std::memory_order_seq_cst);
                available = s.published.load(std::memory_order_seq_cst);
                if (available == read && !stop.stop_requested()) {
                    s.sleeps.fetch_add(1, std::memory_order_relaxed);
                    s.wake.wait(seen, std::memory_order_seq_cst);
                }
                s.back_waiting.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        u64 last_store = read;
        while (read != available) {
            const u64 index = read % RingWords;
            const u64 head = s.ring[index];
            if ((head & 1) == 0) {
                read += RingWords - index;
            } else {
                const u64 size = (head >> 16) & 0xffff;
                const u64 seq = head >> 32;
                s.order_ok &= seq == expected_seq++;
                for (u64 word = 1; word < size; ++word) {
                    s.payload_ok &= s.ring[index + word] == Payload(seq, word);
                }
                if ((head & 2) != 0) {
                    const u32 page = static_cast<u32>((head >> 2) & 0xff);
                    s.gpu_words[page] = seq;
                    // The buffer cache records the written page when the writer executes.
                    Tegra::Nx2MarkGpuWritten(CommandBase + page * Page, sizeof(u64));
                }
                ++s.executed;
                read += size;
            }
            if (read != available && s.completion.ShouldPublish(read, last_store)) {
                s.consumed.store(read, std::memory_order_release);
                last_store = read;
            }
        }
        s.consumed.store(read, std::memory_order_release);
    }
}

class Front {
public:
    Front(Shared& s_, std::chrono::steady_clock::time_point deadline_)
        : s{s_}, deadline{deadline_} {}

    void Emit(bool may_write, u32 page, u64 payload_words) {
        const u64 size = 1 + payload_words;
        u64* const entry = Reserve(size);
        if (timed_out) {
            return; // Never write ring slots the back may still be reading.
        }
        const u64 seq = ++next_seq;
        entry[0] = 1 | (may_write ? 2 : 0) | (u64{page} << 2) | (size << 16) | (seq << 32);
        for (u64 word = 1; word < size; ++word) entry[word] = Payload(seq, word);
        Commit(size, may_write);
        if (may_write) last_writer[page] = seq;
    }

    void Publish() {
        if (published_local == write_position) return;
        published_local = write_position;
        interval = SplitPublication::NextInterval(interval);
        s.published.store(write_position, std::memory_order_seq_cst);
        if (s.back_waiting.load(std::memory_order_seq_cst)) {
            s.wake.fetch_add(1, std::memory_order_seq_cst);
            s.wake.notify_one();
        }
    }

    void DrainTo(u64 target, bool command_fetch) {
        consumed_cache = s.consumed.load(std::memory_order_acquire);
        if (consumed_cache >= target) return;
        if (command_fetch) {
            max_withheld = std::max(max_withheld,
                                    SplitPublication::Withheld(target, published_local, hinted));
        }
        s.completion.Request(target);
        Publish();
        while (true) {
            consumed_cache = s.consumed.load(std::memory_order_acquire);
            if (consumed_cache >= target) {
                interval = SplitPublication::MinimumInterval;
                return;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                timed_out = true;
                return;
            }
            std::this_thread::yield();
        }
    }
    void DrainMemoryWriters() {
        DrainTo(frontier.Target(), true);
        Publish();
    }
    void Drain() {
        DrainTo(write_position, false);
    }

    /// One GP-entry fetch of `page`, through the production synchronization helper.
    void Fetch(u32 page, bool sync_wait) {
        const GPUVAddr address = CommandBase + page * Page;
        bool drained = false;
        Tegra::Nx2SynchronizeGpuWrittenCommandsAudited(
            address, sizeof(u64), sync_wait, audit,
            [&] { return frontier.Target() > s.consumed.load(std::memory_order_acquire); },
            [&] {
                drained = true;
                DrainMemoryWriters();
            },
            [](GPUVAddr gpu) { return std::optional<u64>{gpu}; },
            [&] { guest_words[page] = s.gpu_words[page]; }, // Download the GPU-written words
            [] { return u64{}; });
        (sync_wait ? sync_drains : plain_drains) += drained;
        (sync_wait ? sync_fetches : plain_fetches) += 1;
        // A queued writer committed before this fetch, including a first-ever write to the page.
        fetch_ok &= timed_out || guest_words[page] == last_writer[page];
        tail_published_ok &= timed_out || published_local == write_position;
    }

    Shared& s;
    std::chrono::steady_clock::time_point deadline;
    u64 write_position{};
    u64 published_local{};
    u64 hinted{};
    u64 consumed_cache{};
    u64 interval{SplitPublication::MaximumInterval};
    u64 next_seq{};
    u64 hints{};
    u64 max_withheld{};
    CommandWriteFrontier frontier;
    VideoCommon::CommandFetchSyncAudit audit;
    std::array<u64, CommandPages> last_writer{};
    std::array<u64, CommandPages> guest_words{};
    u64 sync_fetches{}, plain_fetches{}, sync_drains{}, plain_drains{};
    bool fetch_ok = true;
    bool tail_published_ok = true;
    bool timed_out = false;

private:
    u64* Reserve(u64 words) {
        u64 index = write_position % RingWords;
        if (index + words > RingWords) {
            const u64 rest = RingWords - index;
            WaitForSpace(rest);
            if (timed_out) {
                return nullptr;
            }
            s.ring[index] = 0; // Wrap
            write_position += rest;
            index = 0;
        }
        WaitForSpace(words);
        return timed_out ? nullptr : &s.ring[index];
    }
    void WaitForSpace(u64 words) {
        if (write_position + words - consumed_cache <= RingWords) return;
        Publish();
        while (true) {
            consumed_cache = s.consumed.load(std::memory_order_acquire);
            if (write_position + words - consumed_cache <= RingWords) return;
            if (std::chrono::steady_clock::now() > deadline) {
                timed_out = true; // Emit abandons the entry; the checks report the timeout.
                return;
            }
            std::this_thread::yield();
        }
    }
    void Commit(u64 words, bool may_write) {
        write_position += words;
        frontier.Commit(write_position, may_write);
        switch (SplitPublication::AfterCommit(write_position, published_local, interval,
                                              may_write)) {
        case Action::Full:
            Publish();
            break;
        case Action::Hint:
            hinted = write_position;
            ++hints;
            s.published.store(write_position, std::memory_order_release);
            if (s.back_waiting.load(std::memory_order_relaxed)) Publish();
            break;
        case Action::None:
            break;
        }
    }
};

TEST_CASE("Split writer hints keep release/acquire visibility, first writes, wrap and restart",
          "[video_core][split_publication]") {
    Shared shared;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    for (u32 session = 0; session < 2; ++session) {
        Tegra::Nx2ClearGpuWrittenPages();
        shared.ResetSession();
        Front front{shared, deadline};
        std::jthread back([&](std::stop_token stop) { BackLoop(shared, stop); });
        std::mt19937 rng{1234u + session};
        constexpr u32 Operations = 30000;
        for (u32 op = 0; op < Operations && !front.timed_out; ++op) {
            const u32 roll = rng() % 100;
            if (roll < 55) {
                front.Emit(false, 0, rng() % 6); // Register-only state, up to a 7-word entry
            } else if (roll < 80) {
                front.Emit(true, rng() % CommandPages, rng() % 6); // Possible writer
            } else {
                front.Fetch(rng() % CommandPages, (rng() & 1) != 0);
            }
            if (op % 4096 == 2048) {
                // Let the back fall asleep, then commit a writer while it sleeps (its hint
                // must not be the only publication) and fetch the page it writes.
                std::this_thread::sleep_for(std::chrono::milliseconds{3});
                front.Emit(true, 1, 2);
                front.Fetch(1, false);
            }
        }
        front.Drain(); // The whole state tail, as GpuSplit::MaybeStart does before Stop.
        const bool drained_all =
            !front.timed_out && shared.consumed.load(std::memory_order_acquire) ==
                                    front.write_position;
        back.request_stop();
        shared.wake.fetch_add(1, std::memory_order_seq_cst);
        shared.wake.notify_one();
        back.join();

        REQUIRE_FALSE(front.timed_out);
        REQUIRE(drained_all);
        REQUIRE(shared.payload_ok); // Release/acquire: entries complete when executed
        REQUIRE(shared.order_ok);   // FIFO order through hints, full publishes and wraps
        REQUIRE(shared.executed == front.next_seq);
        REQUIRE(front.write_position > 4 * RingWords); // Wrapped many times
        REQUIRE(front.hints > 0);
        REQUIRE(front.fetch_ok); // Includes each page's first-ever GPU write
        REQUIRE(front.tail_published_ok);
        REQUIRE(front.max_withheld == 0);
        // SYNC_WAIT and SYNC_PROCEED fetches synchronize identically (no relaxation).
        REQUIRE(front.sync_fetches > 0);
        REQUIRE(front.plain_fetches > 0);
        REQUIRE(front.sync_drains == front.sync_fetches);
        REQUIRE(front.plain_drains == front.plain_fetches);
        REQUIRE(front.audit.fetches == front.sync_fetches + front.plain_fetches);
        REQUIRE(front.audit.sync_fetches == front.sync_fetches);
    }
    REQUIRE(shared.sleeps.load() > 0); // The sleeping-back path was exercised
}

} // Anonymous namespace
