// SPDX-License-Identifier: GPL-3.0-or-later
#include <optional>
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
