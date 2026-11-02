// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/native_qmd_write.h"

namespace {

struct Write {
    u32 method;
    u32 value;
    bool observable{};
    bool multi{};
};

struct ReplayResult {
    std::array<u32, 0x1000> registers{};
    std::vector<std::array<u32, 64>> launches;
    std::vector<std::pair<u32, u32>> side_effects;
    size_t sent{};
};

// Model the front's queued shadow separately from back execution. MultiMethod's
// existing bulk path deliberately forwards every word and must update that shadow.
ReplayResult Replay(std::span<const Write> input, bool coalesce) {
    std::array<u32, 0x1000> front{};
    std::vector<Write> queued;
    for (const auto& write : input) {
        const bool skip = coalesce && !write.multi && Tegra::Engines::CanSkipInlineQmdWrite(
            write.method, front[write.method], write.value, !write.observable);
        front[write.method] = write.value;
        if (!skip) {
            queued.push_back(write);
        }
    }
    ReplayResult result;
    result.sent = queued.size();
    for (const auto& write : queued) {
        result.registers[write.method] = write.value;
        if (write.method == 0x107) {
            std::array<u32, 64> qmd;
            for (size_t i = 0; i < qmd.size(); ++i) {
                qmd[i] = result.registers[0xc8 + i];
            }
            result.launches.push_back(qmd);
            // Dispatch changes legacy code_loc independently of native front state.
            result.registers[0x582] = qmd[49] & 0x1ffff;
            result.registers[0x583] = qmd[48];
        }
        if (!Tegra::Engines::IsInlineQmdDataMethod(write.method) || write.observable) {
            result.side_effects.emplace_back(write.method, write.value);
        }
    }
    return result;
}

void RequireEquivalent(const ReplayResult& baseline, const ReplayResult& coalesced) {
    REQUIRE(coalesced.registers == baseline.registers);
    REQUIRE(coalesced.launches == baseline.launches);
    REQUIRE(coalesced.side_effects == baseline.side_effects);
}

} // namespace

TEST_CASE("Native QMD coalescing admits only unchanged unobserved data words",
          "[video_core][native_qmd_write]") {
    for (u32 method = 0; method < 0x1000; ++method) {
        const bool data = method >= 0xc8 && method <= 0x106;
        REQUIRE(Tegra::Engines::IsInlineQmdDataMethod(method) == data);
        REQUIRE(Tegra::Engines::CanSkipInlineQmdWrite(method, 7, 7, true) == data);
        REQUIRE_FALSE(Tegra::Engines::CanSkipInlineQmdWrite(method, 7, 8, true));
        REQUIRE_FALSE(Tegra::Engines::CanSkipInlineQmdWrite(method, 7, 7, false));
    }
    REQUIRE_FALSE(Tegra::Engines::IsInlineQmdDataMethod(~u32{0}));
}

TEST_CASE("Native QMD replay preserves every launch and changing descriptor word",
          "[video_core][native_qmd_write]") {
    std::vector<Write> stream;
    for (u32 frame = 0; frame < 64; ++frame) {
        for (u32 method = 0xc8; method < 0x107; ++method) {
            const u32 value = method == 0xd4 ? frame + 1 : method * 31;
            stream.push_back({method, value});
        }
        // The last word triggers an upload/dispatch; identical triggers cannot be dropped.
        stream.push_back({0x107, 0});
        stream.push_back({0x107, 0});
    }
    const auto baseline = Replay(stream, false);
    const auto coalesced = Replay(stream, true);
    RequireEquivalent(baseline, coalesced);
    REQUIRE(coalesced.launches.size() == 128);
    REQUIRE(coalesced.sent == 63 + 63 + 128);
    REQUIRE(baseline.sent == 64 * 65);
}

TEST_CASE("Native QMD coalescing preserves observers side effects and legacy code restoration",
          "[video_core][native_qmd_write]") {
    std::vector<Write> stream;
    for (u32 repeat = 0; repeat < 3; ++repeat) {
        // With diagnostics enabled, every QMD word remains observable, even unchanged.
        for (u32 method = 0xc8; method < 0x107; ++method) {
            stream.push_back({method, method, true});
        }
        for (u32 method : {0x45U, 0x46U, 0x47U, 0x48U, 0x6cU, 0x6dU,
                           0xaeU, 0xafU, 0xb0U, 0x107U, 0x6c3U}) {
            stream.push_back({method, 0});
        }
        stream.push_back({0x582, 0});
        stream.push_back({0x583, 0});
    }
    const auto baseline = Replay(stream, false);
    const auto coalesced = Replay(stream, true);
    RequireEquivalent(baseline, coalesced);
    REQUIRE(coalesced.sent == baseline.sent);
    REQUIRE(coalesced.registers[0x582] == 0);
    REQUIRE(coalesced.registers[0x583] == 0);
}

TEST_CASE("Native QMD single and bulk writes share queued state without crossing engines",
          "[video_core][native_qmd_write]") {
    const std::array<Write, 8> stream{{
        {0xc8, 10, false, true}, {0xc8, 20, false, true}, {0xc8, 20}, {0x107, 0},
        {0xc8, 10}, {0xc8, 20}, {0xc8, 20}, {0x107, 0},
    }};
    const auto baseline = Replay(stream, false);
    const auto first_engine = Replay(stream, true);
    const auto second_engine = Replay(stream, true);
    RequireEquivalent(baseline, first_engine);
    RequireEquivalent(baseline, second_engine);
    REQUIRE(first_engine.sent == 6);
    REQUIRE(second_engine.sent == 6); // A new engine must not reuse another engine's shadow.
    REQUIRE(first_engine.launches[0][0] == 20);
    REQUIRE(first_engine.launches[1][0] == 20);
}
