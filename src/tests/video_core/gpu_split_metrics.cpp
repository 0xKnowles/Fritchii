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

TEST_CASE("MME DMA timings are sampled while read counts remain exact",
          "[video_core][gpu_split_metrics]") {
    VideoCommon::NativeDmaReadMetrics metrics;
    for (u32 read = 0; read < 130; ++read) {
        const bool sampled = metrics.BeginRead(read);
        REQUIRE(sampled == (read == 0 || read == 64 || read == 128));
        if (read % 2 == 0) ++metrics.flushes;
        if (sampled) metrics.RecordSample(10, 20, 30, 40);
    }
    REQUIRE(metrics.calls == 130);
    REQUIRE(metrics.words == 129 * 130 / 2);
    REQUIRE(metrics.flushes == 65);
    REQUIRE(metrics.samples == 3);
    REQUIRE(metrics.drain_ns == 30);
    REQUIRE(metrics.dirty_check_ns == 60);
    REQUIRE(metrics.flush_ns == 90);
    REQUIRE(metrics.read_ns == 120);
    metrics = {};
    REQUIRE(metrics.BeginRead(4));
    REQUIRE(metrics.calls == 1);
    REQUIRE(metrics.words == 4);
    REQUIRE(metrics.samples == 0);
    REQUIRE(metrics.flushes == 0);
    REQUIRE(metrics.drain_ns == 0);
}
