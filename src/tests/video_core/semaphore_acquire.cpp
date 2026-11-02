// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"
#include "video_core/semaphore_acquire.h"

namespace {
using Tegra::SemaphoreAcquire;
constexpr bool Matches(SemaphoreAcquire operation, u32 current, u32 expected) {
    return Tegra::SemaphoreMatches(static_cast<u32>(operation), current, expected);
}
} // namespace

TEST_CASE("Ampere acquire opcodes are not legacy semaphore opcodes", "[video_core][semaphore]") {
    using Tegra::DecodeSemaphoreAcquire32;
    REQUIRE(DecodeSemaphoreAcquire32(0) == SemaphoreAcquire::Equal);
    REQUIRE(DecodeSemaphoreAcquire32(2) == SemaphoreAcquire::GreaterEqual);
    REQUIRE(DecodeSemaphoreAcquire32(3) == SemaphoreAcquire::CircularGreaterEqual);
    REQUIRE(DecodeSemaphoreAcquire32(4) == SemaphoreAcquire::Mask);
    REQUIRE(DecodeSemaphoreAcquire32(5) == SemaphoreAcquire::MaskZero);
    REQUIRE(DecodeSemaphoreAcquire32(0x1002) == SemaphoreAcquire::GreaterEqual);
    for (u32 opcode = 0; opcode != 8; ++opcode) {
        REQUIRE_FALSE(DecodeSemaphoreAcquire32(opcode | (1U << 24)));
    }
    for (const u32 opcode : {1U, 6U, 7U}) {
        REQUIRE_FALSE(DecodeSemaphoreAcquire32(opcode));
    }
    REQUIRE_FALSE(Tegra::SemaphoreMatches(0xffffffffU, 0, 0));
}

TEST_CASE("Semaphore acquire comparisons preserve masks and circular counter wrap", "[video_core][semaphore]") {
    REQUIRE(Matches(SemaphoreAcquire::Equal, 17, 17));
    REQUIRE_FALSE(Matches(SemaphoreAcquire::Equal, 18, 17));
    REQUIRE(Matches(SemaphoreAcquire::GreaterEqual, 0xffffffffU, 0));
    REQUIRE_FALSE(Matches(SemaphoreAcquire::GreaterEqual, 0, 0xffffffffU));
    REQUIRE(Matches(SemaphoreAcquire::CircularGreaterEqual, 0, 0xffffffffU));
    REQUIRE_FALSE(Matches(SemaphoreAcquire::CircularGreaterEqual, 0xffffffffU, 0));
    REQUIRE(Matches(SemaphoreAcquire::CircularGreaterEqual, 0x7fffffffU, 0));
    REQUIRE_FALSE(Matches(SemaphoreAcquire::CircularGreaterEqual, 0x80000000U, 0));
    REQUIRE(Matches(SemaphoreAcquire::CircularGreaterEqual, 0x80000000U, 0x80000000U));
    REQUIRE(Matches(SemaphoreAcquire::Mask, 0x12, 0x10));
    REQUIRE_FALSE(Matches(SemaphoreAcquire::Mask, 0x12, 0x04));
    REQUIRE_FALSE(Matches(SemaphoreAcquire::Mask, 0xffffffffU, 0));
    REQUIRE(Matches(SemaphoreAcquire::MaskZero, 0x12, 0x04));
    REQUIRE_FALSE(Matches(SemaphoreAcquire::MaskZero, 0x12, 0x10));
    REQUIRE(Matches(SemaphoreAcquire::MaskZero, 0xffffffffU, 0));
}

TEST_CASE("Every acquire predicate latches an intermediate matching GPU release", "[video_core][semaphore]") {
    struct Example {
        SemaphoreAcquire operation;
        u32 expected;
        u32 matching;
        u32 not_matching;
    };
    constexpr std::array examples{
        Example{SemaphoreAcquire::Equal, 1, 1, 2},
        Example{SemaphoreAcquire::GreaterEqual, 3, 4, 2},
        Example{SemaphoreAcquire::Mask, 4, 5, 2},
        Example{SemaphoreAcquire::CircularGreaterEqual, 0xffffffffU, 0, 0xfffffffeU},
        Example{SemaphoreAcquire::MaskZero, 4, 2, 4},
    };
    Tegra::Nx2SemaphoreWaiters waiters;
    int owner{};
    for (const auto& example : examples) {
        waiters.Add(&owner, 0x1000, example.expected, static_cast<u32>(example.operation));
        waiters.OnWrite(0x2000, example.matching);
        REQUIRE_FALSE(waiters.Latched(&owner));
        waiters.OnWrite(0x1000, example.not_matching);
        REQUIRE_FALSE(waiters.Latched(&owner));
        waiters.OnWrite(0x1000, example.matching);
        REQUIRE(waiters.Latched(&owner));
        waiters.OnWrite(0x1000, example.not_matching);
        REQUIRE(waiters.Latched(&owner));
        waiters.Remove(&owner);
        REQUIRE_FALSE(waiters.Latched(&owner));
    }
}

TEST_CASE("Replacing a channel acquire does not inherit a previous latch", "[video_core][semaphore]") {
    Tegra::Nx2SemaphoreWaiters waiters;
    int owner{};
    waiters.Add(&owner, 0x1000, 5, Tegra::Nx2SemaphoreWaiters::AcquireEqual);
    waiters.OnWrite(0x1000, 5);
    REQUIRE(waiters.Latched(&owner));
    waiters.Add(&owner, 0x2000, 8, Tegra::Nx2SemaphoreWaiters::AcquireEqual);
    REQUIRE_FALSE(waiters.Latched(&owner));
    waiters.OnWrite(0x1000, 8);
    REQUIRE_FALSE(waiters.Latched(&owner));
    waiters.OnWrite(0x2000, 8);
    REQUIRE(waiters.Latched(&owner));
}
