// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/readback_diagnostics.h"

namespace Diag = VideoCommon::ReadbackDiagnostics;

TEST_CASE("Critical synchronization waits are timed on every call", "[readback_timing]") {
    for (const auto event : {Diag::Event::OrderedRead, Diag::Event::BufferFinish,
                            Diag::Event::DeviceFinish, Diag::Event::FrontQueueWait,
                            Diag::Event::FrontPendingPoll, Diag::Event::FrontPendingYield,
                            Diag::Event::FrontPendingWait, Diag::Event::FrontDispatch,
                            Diag::Event::PendingFenceReadback}) {
        CHECK(Diag::FullyTimed(event));
    }
    for (const auto event : {Diag::Event::FlushArea, Diag::Event::TextureReadback,
                            Diag::Event::BufferReadback, Diag::Event::QueryReadback,
                            Diag::Event::DownloadStaging, Diag::Event::UploadStaging,
                            Diag::Event::FenceDownloadCommit, Diag::Event::EagerPagesAdded}) {
        CHECK_FALSE(Diag::FullyTimed(event));
    }
}

TEST_CASE("Wait timing retains long-tail totals instead of extrapolating", "[readback_timing]") {
    Diag::Measurement measurement;
    for (int i = 0; i < 255; ++i) measurement.RecordSample(100);
    measurement.RecordSample(50'000'000);
    CHECK(measurement.samples == 256);
    CHECK(measurement.sampled_ns == 50'025'500);
    CHECK(measurement.max_ns == 50'000'000);
    CHECK(measurement.over_1ms == 1);
    CHECK(measurement.over_10ms == 1);
}

TEST_CASE("Wait histogram thresholds and reset are exact", "[readback_timing]") {
    Diag::Measurement measurement;
    measurement.RecordSample(1'000'000);
    measurement.RecordSample(1'000'001);
    measurement.RecordSample(10'000'000);
    measurement.RecordSample(10'000'001);
    CHECK(measurement.over_1ms == 3);
    CHECK(measurement.over_10ms == 1);
    CHECK(measurement.max_ns == 10'000'001);
    measurement = {};
    CHECK(measurement.samples == 0);
    CHECK(measurement.sampled_ns == 0);
    CHECK(measurement.max_ns == 0);
    CHECK(measurement.over_1ms == 0);
    CHECK(measurement.over_10ms == 0);
}
