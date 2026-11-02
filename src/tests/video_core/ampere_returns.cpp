// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

TEST_CASE("Ampere returns do not write the link register", "[video_core][ampere]") {
    for (const u32 link : {0U, 55U}) {
        const std::array<Instruction, 4> code{{
            {{0x00007802U | (link << 16), 0x20, 0x00000f00, 0}},
            {{0x00007944, 0x10, 0x03c00000, 0}},
            {{0x0000794d, 0, 0x03800000, 0}},
            {{0x00007950U | (link << 24), 0xffffffc0, 0x03c3ffff, 0}},
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.source_size == sizeof(code));
        REQUIRE(lowered.instructions.back() == 0xe32000000007000fULL);
    }
}

TEST_CASE("Ampere returns can read a copy of the R0 link", "[video_core][ampere]") {
    const std::array<Instruction, 5> code{{
        {{0x00007802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
        {{0x00017202, 0, 0x00000f00, 0}},
        {{0x01007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));
}

TEST_CASE("Ampere callees still reject writes to the link", "[video_core][ampere]") {
    const std::array<Instruction, 5> code{{
        {{0x00007802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
        {{0x00007802, 0, 0x00000f00, 0}},
        {{0x00007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "native callee overwrites link register");
    REQUIRE(lowered.error_offset == 0x30);
}
TEST_CASE("Ampere predicated NOINC exits a loop in a program containing returns", "[video_core][ampere]") {
    for (u32 guard : {0U, 6U, 8U, 14U}) {
        std::array<Instruction, 4> code{{
            {{0x944U | (guard << 12), 16, 0x03c00000, 0}},
            {{0x7947, 0xffffffe0U, 0x0383ffff, 0}},
            {{0x794d, 0, 0x03800000, 0}},
            {{0x7950, 0xffffffc0U, 0x03c3ffff, 0}},
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(lowered);
        REQUIRE(lowered.source_size == 48);
        REQUIRE(((lowered.instructions[0] >> 16) & 15) == guard);
        REQUIRE((lowered.instructions[0] >> 52) == 0xe24);
        code[1].words[1] = 16; // Not a loop back edge; do not pretend the CALL is a branch.
        code[1].words[2] = 0x03800000;
        REQUIRE_FALSE(LowerToMaxwell(code));
    }
}
} // namespace
