// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::ProgramCaptureState;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0x000fea00}};
constexpr Instruction Nop{{0x00007918, 0, 0, 0x000fc000}};
}

TEST_CASE("Ampere branch convergence flags are not address bits", "[ampere][fast_fusion_branch]") {
    for (u32 modifier = 0; modifier < 4; ++modifier) {
        std::array<Instruction, 6> code;
        code.fill(Nop);
        code[0] = Instruction{{0xff007947, 0x40U | modifier, 0x03800000, 0x000fea00}};
        code.back() = Exit;
        ProgramCaptureState capture;
        for (u32 index = 0; index < code.size(); ++index) {
            const auto expected = index + 1 == code.size() ? ProgramCaptureState::Result::Complete
                                                          : ProgramCaptureState::Result::Continue;
            CHECK(capture.Observe(code[index], index) == expected);
        }
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        INFO(lowered.error);
        REQUIRE(lowered);
        CHECK(lowered.source_size == sizeof(code));
        CHECK(lowered.instructions.front() == (0xe24000000007000fULL | (0x30ULL << 20)));
    }
}

TEST_CASE("Fast Fusion convergence-mask branch captures its direct target",
          "[ampere][fast_fusion_branch]") {
    const Instruction branch{{0x06007947, 0x00000043, 0x0b800000, 0x000fea00}};
    ProgramCaptureState capture;
    CHECK(capture.Observe(branch, 0x3f0 / 16) == ProgramCaptureState::Result::Continue);
    CHECK(capture.Observe(Exit, 0x400 / 16) == ProgramCaptureState::Result::Continue);
    CHECK(capture.Observe(Exit, 0x440 / 16) == ProgramCaptureState::Result::Complete);

    std::array<Instruction, 6> code;
    code.fill(Nop);
    code[0] = branch;
    code.back() = Exit;
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(lowered);
    CHECK(lowered.instructions.front() == (0xe24000000007000fULL | (0x30ULL << 20)));
}

TEST_CASE("Ampere branch flags preserve negative and invalid destinations",
          "[ampere][fast_fusion_branch]") {
    for (u32 modifier = 0; modifier < 4; ++modifier) {
        ProgramCaptureState backward;
        CHECK(backward.Observe(Instruction{{0xff007947, 0xffffffe0U | modifier, 0x0383ffff,
                                            0x000fea00}}, 1) == ProgramCaptureState::Result::Continue);
        ProgramCaptureState before_start;
        CHECK(before_start.Observe(Instruction{{0xff007947, 0xffffffe0U | modifier, 0x0383ffff,
                                                0x000fea00}}, 0) == ProgramCaptureState::Result::InvalidTarget);
        ProgramCaptureState unaligned;
        CHECK(unaligned.Observe(Instruction{{0xff007947, 0x44U | modifier, 0x03800000,
                                             0x000fea00}}, 0) == ProgramCaptureState::Result::InvalidTarget);
    }
}
