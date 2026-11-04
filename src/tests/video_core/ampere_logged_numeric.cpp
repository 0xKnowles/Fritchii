// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
namespace IR = Shader::IR;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
constexpr Instruction LoggedF2I{{0x00067b05, 0x40c03400, 0x00212800, 0x000e2200}};
constexpr std::array<Instruction, 2> LoggedShifts{{
    {{0xff107419, 0x000000ff, 0x0001060e, 0x080fe400}},
    {{0xff0c7419, 0x000000ff, 0x0001060b, 0x080fe400}},
}};

u32 Address(u32 index) {
    return 0x50 + index * 8 + ((index + 1) / 3) * 8;
}

class LoggedEnvironment final : public Shader::Environment {
public:
    explicit LoggedEnvironment(std::span<const u64> code_) : code{code_} {
        stage = Shader::Stage::Compute;
    }

    u64 ReadInstruction(u32 address) override {
        for (u32 index = 0; index < code.size(); ++index) {
            if (Address(index) == address)
                return code[index];
        }
        FAIL("Read outside logged instruction fixture at " << address);
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override {
        FAIL("Unexpected constant buffer specialization");
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 4096; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    bool HasNativeInstructions() const override { return true; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> code;
};

u32 EvaluateShift(IR::Value value) {
    value = value.Resolve();
    if (value.IsImmediate()) {
        return value.Type() == IR::Type::U1 ? u32{value.U1()} : value.U32();
    }
    const auto& inst = *value.Inst();
    const auto arg = [&](size_t index) { return EvaluateShift(inst.Arg(index)); };
    switch (inst.GetOpcode()) {
    case IR::Opcode::Identity:
        return arg(0);
    case IR::Opcode::BitwiseAnd32:
        return arg(0) & arg(1);
    case IR::Opcode::ULessThan:
        return arg(0) < arg(1);
    case IR::Opcode::SelectU32:
        return arg(0) ? arg(1) : arg(2);
    case IR::Opcode::ShiftLeftLogical32: {
        const u32 shift = arg(1);
        if (shift >= 32)
            throw std::domain_error("Unmasked out-of-range shift reaches the result");
        return arg(0) << shift;
    }
    default:
        throw std::logic_error(std::string{IR::NameOf(inst.GetOpcode())});
    }
}

void TranslateAndEmit(std::span<const u64> code, std::optional<u32> expected = std::nullopt) {
    LoggedEnvironment env{code};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
    Shader::ObjectPool<IR::Inst> inst_pool;
    Shader::ObjectPool<IR::Block> block_pool;
    auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg,
                                                    Shader::HostTranslateInfo{});
    u32 stores = 0;
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::WriteSharedU32)
                continue;
            ++stores;
            if (expected) {
                INFO(IR::DumpProgram(program));
                CHECK(EvaluateShift(inst.Arg(1)) == *expected);
            }
        }
    }
    REQUIRE(stores == 1);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010400;
    const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
    REQUIRE(spirv.size() > 5);
    CHECK(spirv.front() == 0x07230203);
}

u64 Store(u32 source) {
    return 0xef58000000070000ULL | source | (255ULL << 8) | (4ULL << 48);
}
} // namespace

TEST_CASE("Logged Pokopia absolute constant F2I retains U16 rounding and reaches SPIRV",
          "[video_core][ampere][logged_numeric]") {
    const auto lowered = LowerToMaxwell(std::array{LoggedF2I, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    const std::array<u64, 6> expected{
        0x4c98000000070000ULL | 252 | (15ULL << 39) | (0x34ULL << 20) | (3ULL << 34),
        0x0400000000070000ULL | 252 | (252ULL << 8) | (0x7fffffffULL << 20),
        0x5800000000070000ULL | 250 | (252ULL << 8) | (252ULL << 20) |
            (7ULL << 39) | (7ULL << 48),
        0x5c40000000070000ULL | 250 | (252ULL << 8) | (250ULL << 20) | (7ULL << 48),
        0x5cb0000000070000ULL | 6 | (1ULL << 8) | (2ULL << 10) | (250ULL << 20) |
            (1ULL << 44),
        0xe30000000007000fULL,
    };
    REQUIRE(lowered.instructions.size() == expected.size());
    CHECK(std::equal(expected.begin(), expected.end(), lowered.instructions.begin()));
    auto code = lowered.instructions;
    code.insert(code.end() - 1, Store(6));
    TranslateAndEmit(code);
}

TEST_CASE("Logged Pokopia F2I preserves guards and rejects unrelated modifiers",
          "[video_core][ampere][logged_numeric]") {
    for (const u32 guard : {0U, 6U, 7U, 8U, 14U}) {
        auto instruction = LoggedF2I;
        instruction.words[0] = (instruction.words[0] & ~0xf000U) | (guard << 12);
        const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
        REQUIRE(lowered);
        for (size_t index = 0; index + 1 < lowered.instructions.size(); ++index)
            CHECK(((lowered.instructions[index] >> 16) & 15) == guard);
    }
    for (const u32 source : {0x80c03400U, 0xc0c03400U, 0x44c03400U, 0x40c03440U,
                             0x48c03400U, 0x50c03400U, 0x60c03400U}) {
        auto invalid = LoggedF2I;
        invalid.words[1] = source;
        CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
    }
    for (const u32 bit : {0U, 8U, 30U, 31U}) {
        auto invalid = LoggedF2I;
        invalid.words[3] ^= 1U << bit;
        CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
    }
    auto invalid = LoggedF2I;
    invalid.words[0] |= 1U << 24;
    CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
    invalid = LoggedF2I;
    invalid.words[2] |= 1U << 27;
    CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
    invalid.words = {0x00157b05, 0x40400400, 0x0021e100, 0x0001e400};
    CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
}

TEST_CASE("Logged Pokopia SHF clamps instead of wrapping through production translation",
          "[video_core][ampere][logged_numeric]") {
    for (const auto instruction : LoggedShifts) {
        for (const bool wrap : {false, true}) {
            auto variant = instruction;
            variant.words[2] |= u32{wrap} << 11;
            const auto lowered = LowerToMaxwell(std::array{variant, Exit});
            INFO(lowered.error);
            REQUIRE(lowered);
            REQUIRE(lowered.instructions.size() == 3);
            CHECK(lowered.instructions[0] ==
                  (0x0100000000070000ULL | 250 | (15ULL << 12) | (255ULL << 20)));
            CHECK(lowered.instructions[1] ==
                  (0x5c48000000070000ULL | instruction.Bits(16, 8) | (250ULL << 8) |
                   (u64{instruction.Bits(64, 8)} << 20) | (u64{wrap} << 39)));
            for (const u32 shift : {0U, 1U, 7U, 24U, 31U, 32U, 33U, 63U, 0xffffffffU}) {
                auto code = lowered.instructions;
                code.insert(code.begin(), 0x0100000000070000ULL | instruction.Bits(64, 8) |
                                              (15ULL << 12) | (u64{shift} << 20));
                code.insert(code.end() - 1, Store(instruction.Bits(16, 8)));
                const u32 effective = wrap ? shift & 31 : std::min(shift, 32U);
                const u32 expected = effective == 32 ? 0U : 255U << effective;
                CAPTURE(shift, wrap);
                TranslateAndEmit(code, expected);
            }
        }
    }
}

TEST_CASE("Logged Pokopia SHF register pressure identifies the register shift operand",
          "[video_core][ampere][logged_numeric]") {
    for (const u32 high : {216U, 217U, 240U, 246U, 254U}) {
        auto instruction = LoggedShifts[0];
        instruction.words[0] = (instruction.words[0] & ~0x00ff0000U) | (high << 16);
        instruction.words[2] = (instruction.words[2] & ~255U) | high;
        const auto pressure = Shader::Ampere::PrepareRegisterPressure(instruction, 216);
        REQUIRE(pressure.valid);
        CHECK(pressure.prefix.size() == 8);
        CHECK(pressure.suffix.size() == 8);
        CHECK(pressure.instruction.Bits(16, 8) < 216);
        CHECK(pressure.instruction.Bits(64, 8) == pressure.instruction.Bits(16, 8));
        CHECK(pressure.instruction.Bits(24, 8) == 255);
        CHECK(pressure.instruction.words[1] == 255);
        REQUIRE(LowerToMaxwell(std::array{pressure.instruction, Exit}));
    }
    for (const u32 bit : {8U, 12U, 17U, 27U}) {
        auto invalid = LoggedShifts[0];
        invalid.words[2] ^= 1U << bit;
        CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
    }
    auto invalid = LoggedShifts[0];
    invalid.words[0] &= ~0xff000000U;
    CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
}

TEST_CASE("Logged Pokopia masked dependency barrier fences shared memory and rejects unknown modes",
          "[video_core][ampere][logged_numeric]") {
    constexpr Instruction wait{{0x0000791a, 0x0000800e, 0, 0x000fce00}};
    for (const u32 mask : {0x800eU, 0x8000U}) {
        auto supported = wait;
        supported.words[1] = mask;
        const auto ordered = LowerToMaxwell(std::array{supported, Exit});
        REQUIRE(ordered);
        REQUIRE(ordered.instructions.size() == 1);
        CHECK(ordered.instructions.back() == 0xe30000000007000fULL);
        const Instruction copy{{0x02067fae, 0, 0x0f901c08, 0}};
        const auto copying = LowerToMaxwell(std::array{copy, supported, Exit});
        REQUIRE(copying);
        CHECK(copying.instructions[copying.source_instruction_offsets[1]] ==
              0xef98000000070000ULL);
        CHECK(copying.source_instruction_offsets[2] == copying.source_instruction_offsets[1] + 1);
    }
    for (u32 bit = 15; bit < 32; ++bit) {
        auto invalid = wait;
        invalid.words[1] ^= 1U << bit;
        const auto rejected = LowerToMaxwell(std::array{invalid, Exit});
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error == "native dependency barrier mode");
        CHECK(rejected.instructions.empty());
        CHECK(rejected.error_offset == 0);
    }
    auto invalid = wait;
    invalid.words[2] = 1;
    const auto rejected = LowerToMaxwell(std::array{invalid, Exit});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error == "native dependency barrier mode");
}
