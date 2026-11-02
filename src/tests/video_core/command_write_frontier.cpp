// SPDX-License-Identifier: GPL-3.0-or-later
#include <optional>
#include <array>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "video_core/command_write_frontier.h"
#include "video_core/nx2.h"

TEST_CASE("Command frontier excludes only inert tails and resets between sessions", "[video_core][command_frontier]") {
    VideoCommon::CommandWriteFrontier frontier;
    frontier.Commit(2, false); // Engine switch: no sink consumed.
    frontier.Commit(3, false); // Non-executable sink append.
    REQUIRE(frontier.Target() == 0);
    frontier.Commit(4, true); // Executable method consumes the entire previous sink.
    frontier.Commit(5, false);
    REQUIRE(frontier.Target() == 4);
    frontier.Commit(6, true); // Direct macro state write may flush a deferred draw.
    REQUIRE(frontier.Target() == 6);
    frontier.Commit(24, true); // Multi-method (including constant-buffer uploads).
    frontier.Commit(28, true); // Arbitrary deferred callback is never assumed inert.
    frontier.Commit(30, false);
    REQUIRE(frontier.Target() == 28);
    frontier.Reset();
    REQUIRE(frontier.Target() == 0);
}

TEST_CASE("Command frontier retains first queued writer before historical-page check", "[video_core][command_frontier]") {
    constexpr u64 address = 0x50000;
    Tegra::Nx2ClearGpuWrittenPages();
    VideoCommon::CommandWriteFrontier frontier;
    frontier.Commit(1, true);
    frontier.Commit(2, false);
    u32 guest = 0;
    u32 gpu = 0;
    u64 consumed = 0;
    Tegra::Nx2SynchronizeGpuWrittenCommands(address, sizeof(guest), [&] {
        REQUIRE(frontier.Target() == 1);
        gpu = 42;
        Tegra::Nx2MarkGpuWritten(address, sizeof(guest));
        consumed = frontier.Target();
    }, [&](GPUVAddr page) {
        REQUIRE(consumed >= frontier.Target());
        return std::optional{page};
    }, [&] { guest = gpu; });
    REQUIRE(guest == 42);
    REQUIRE(consumed == 1); // Inert tail was not needed to make data visible.
}

TEST_CASE("Only proven state writes without a pending draw bypass command drains",
          "[video_core][command_frontier]") {
    using Frontier = VideoCommon::CommandWriteFrontier;
    REQUIRE_FALSE(Frontier::StateWriteMayExecute(true, false));
    REQUIRE(Frontier::StateWriteMayExecute(true, true));
    REQUIRE(Frontier::StateWriteMayExecute(false, false));
    REQUIRE(Frontier::StateWriteMayExecute(false, true));

    Frontier frontier;
    frontier.Commit(1, true); // Dispatch writes a command buffer for the first time.
    for (u64 position = 2; position <= 4096; ++position) {
        frontier.Commit(position, Frontier::StateWriteMayExecute(true, false));
    }
    REQUIRE(frontier.Target() == 1); // Keep the dispatch, not thousands of QMD/state writes.
    frontier.Commit(4097, Frontier::StateWriteMayExecute(false, false)); // Next dispatch/upload.
    REQUIRE(frontier.Target() == 4097);
}

TEST_CASE("State-only command frontier preserves deferred draw and callback ordering",
          "[video_core][command_frontier]") {
    using Frontier = VideoCommon::CommandWriteFrontier;
    enum class Action { State, MultiState, Draw, FinishMacro, Upload };
    // Compare the visible command-memory result after each prefix with executing
    // the complete prefix. The front may conservatively retain 'pending' after a
    // state write, but may clear it only after queuing FinishMacro as a hazard.
    constexpr std::array actions{Action::State, Action::MultiState, Action::Draw,
                                 Action::FinishMacro, Action::Upload};
    constexpr size_t Length = 5;
    constexpr size_t Combinations = 3125; // 5^5
    for (size_t sequence = 0; sequence < Combinations; ++sequence) {
        Frontier frontier;
        bool front_pending = true; // A newly adopted engine can already have a draw.
        std::vector<Action> queued;
        size_t digits = sequence;
        for (size_t index = 0; index < Length; ++index) {
            const auto action = actions[digits % actions.size()];
            digits /= actions.size();
            queued.push_back(action);
            const bool state_only = action == Action::State || action == Action::MultiState;
            frontier.Commit(index + 1,
                            Frontier::StateWriteMayExecute(state_only, front_pending));
            if (action == Action::Draw) front_pending = true;
            if (action == Action::FinishMacro) front_pending = false;

            const auto execute = [&](size_t count) {
                bool back_pending = true;
                u32 written = 0;
                for (size_t i = 0; i < count; ++i) {
                    const auto current = queued[i];
                    if (current == Action::Draw) {
                        back_pending = true;
                    } else {
                        if (back_pending) {
                            ++written; // A state change or callback flushes the draw.
                            back_pending = false;
                        }
                        if (current == Action::Upload) ++written;
                    }
                }
                return written;
            };
            REQUIRE(execute(frontier.Target()) == execute(queued.size()));
        }
    }
}
