// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/gpu_split_metrics.h"

TEST_CASE("Split drain attribution separates no-op and pending waits", "[video_core][gpu_split_metrics]") {
    using namespace VideoCommon;
    SplitDrainMetrics metrics;
    metrics.RecordDrain(SplitDrainReason::CommandFetch, 0, 0);
    metrics.RecordDrain(SplitDrainReason::CommandFetch, 17, 100);
    metrics.RecordDrain(SplitDrainReason::ParameterDirty, 3, 20);
    const auto& commands = metrics.drains[static_cast<size_t>(SplitDrainReason::CommandFetch)];
    REQUIRE(commands.calls == 2);
    REQUIRE(commands.pending_calls == 1);
    REQUIRE(commands.queued_words == 17);
    REQUIRE(commands.elapsed_ns == 100);
    const auto& parameters = metrics.drains[static_cast<size_t>(SplitDrainReason::ParameterDirty)];
    REQUIRE(parameters.calls == 1);
    REQUIRE(parameters.queued_words == 3);
    REQUIRE(parameters.elapsed_ns == 20);
    REQUIRE(metrics.drains[static_cast<size_t>(SplitDrainReason::Other)].calls == 0);
}

TEST_CASE("Command fetch attribution distinguishes historical pages from actual flushes",
          "[video_core][gpu_split_metrics]") {
    VideoCommon::SplitDrainMetrics metrics;
    metrics.RecordCommandFetch(16, false, false);
    metrics.RecordCommandFetch(32, true, false);
    metrics.RecordCommandFetch(64, true, true);
    REQUIRE(metrics.command_fetches == 3);
    REQUIRE(metrics.command_bytes == 112);
    REQUIRE(metrics.historical_hits == 2);
    REQUIRE(metrics.dirty_flushes == 1);
    REQUIRE(metrics.flushed_bytes == 64);
    metrics = {};
    REQUIRE(metrics.command_fetches == 0);
    REQUIRE(metrics.flushed_bytes == 0);
}
