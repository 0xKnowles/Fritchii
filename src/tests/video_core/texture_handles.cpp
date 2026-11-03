// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <initializer_list>
#include <stdexcept>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;
using IR::Opcode;
using IR::Value;

class HandleEnvironment final : public Shader::Environment {
public:
    bool integer_texture{};
    u32 format_reads{};

    u64 ReadInstruction(u32) override { throw std::logic_error("Unexpected instruction read"); }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        REQUIRE(index == 0);
        REQUIRE(offset == 0);
        return 0xabc;
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        REQUIRE(handle == 0xabc);
        ++format_reads;
        return integer_texture;
    }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

struct HandleProgram {
    Shader::ObjectPool<IR::Inst> pool{256};
    IR::Block block{pool};
    IR::Block left{pool};
    IR::Block right{pool};
    IR::Block merge{pool};
    IR::Block* insertion_block{&block};
    IR::Program program;
    HandleEnvironment env;

    HandleProgram() {
        program.stage = Shader::Stage::Fragment;
        program.blocks = {&block};
        program.post_order_blocks = {&block};
    }

    Value Emit(Opcode opcode, std::initializer_list<Value> args = {}) {
        insertion_block->AppendNewInst(opcode, args);
        return Value{&insertion_block->back()};
    }

    Value Storage(u32 offset) {
        return Emit(Opcode::LoadStorage32, {Value{0U}, Value{offset}});
    }

    Value Add(Value lhs, Value rhs) {
        return Emit(Opcode::IAdd32, {lhs, rhs});
    }

    Value Phi(Value first, Value second) {
        block.AddBranch(&left);
        block.AddBranch(&right);
        left.AddBranch(&merge);
        right.AddBranch(&merge);
        program.blocks = {&block, &left, &right, &merge};
        program.post_order_blocks = {&merge, &right, &left, &block};
        insertion_block = &merge;
        const Value value{Emit(Opcode::Phi)};
        value.Inst()->SetFlags(IR::Type::U32);
        value.Inst()->AddPhiOperand(&left, first);
        value.Inst()->AddPhiOperand(&right, second);
        return value;
    }

    IR::Inst& Sample(Value handle) {
        IR::IREmitter ir{*insertion_block};
        IR::TextureInstInfo flags{};
        flags.type.Assign(Shader::TextureType::Color2D);
        return *ir.ImageSampleExplicitLod(handle,
                                         ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(0.0f)),
                                         ir.Imm32(0.0f), {}, flags).Inst();
    }

    void Run() {
        Shader::HostTranslateInfo host;
        host.ApplyDescriptorLimitPolicy();
        Shader::Optimization::TexturePass(env, program, host);
    }

    const Shader::TextureDescriptor& Descriptor(const IR::Inst& sample) const {
        REQUIRE(sample.GetOpcode() == Opcode::ImageSampleExplicitLod);
        const u32 index{sample.Flags<IR::TextureInstInfo>().descriptor_index};
        REQUIRE(index < program.info.texture_descriptors.size());
        return program.info.texture_descriptors[index];
    }

    u32 Evaluate(const IR::Inst& sample, u32 first = 0x1234, u32 second = 0x50) const {
        const u32 index{Descriptor(sample).handle_program};
        REQUIRE(index < program.info.handle_programs.size());
        const auto result{Shader::EvaluateHandleProgram(
            program.info.handle_programs[index],
            [](u32, u32) -> u32 { throw std::logic_error("Unexpected evaluator cbuf read"); },
            [=](u32 binding, u32 offset) {
                REQUIRE(binding == 0);
                REQUIRE((offset == 0 || offset == 4));
                return offset == 0 ? first : second;
            })};
        REQUIRE(result.has_value());
        return *result;
    }
};
}

TEST_CASE("Sampled descriptors distinguish static and computed handles",
          "[video_core][texture_handles]") {
    HandleProgram shader;
    auto& first{shader.Sample(shader.Storage(0))};
    auto& second{shader.Sample(shader.Storage(4))};
    auto& repeated{shader.Sample(shader.Storage(0))};
    auto& fixed{shader.Sample(shader.Emit(Opcode::GetCbufU32, {Value{0U}, Value{0U}}))};
    shader.Run();

    CHECK(shader.program.info.handle_programs.size() == 2);
    CHECK(shader.program.info.texture_descriptors.size() == 3);
    CHECK(shader.Descriptor(first).handle_program != shader.Descriptor(second).handle_program);
    CHECK(shader.Descriptor(first).handle_program == shader.Descriptor(repeated).handle_program);
    CHECK(shader.Descriptor(fixed).handle_program == ~0U);
    CHECK(first.Flags<IR::TextureInstInfo>().descriptor_index ==
          repeated.Flags<IR::TextureInstInfo>().descriptor_index);
    CHECK(first.Flags<IR::TextureInstInfo>().descriptor_index !=
          fixed.Flags<IR::TextureInstInfo>().descriptor_index);
    CHECK(shader.Evaluate(first) == 0x1234);
    CHECK(shader.Evaluate(second) == 0x50);
    CHECK(shader.Evaluate(repeated, 0x5678) == 0x5678);
}

TEST_CASE("Computed handles do not share the static texture format cache",
          "[video_core][texture_handles]") {
    for (const bool dynamic_first : {false, true}) {
        CAPTURE(dynamic_first);
        HandleProgram shader;
        shader.env.integer_texture = true;
        const Value dynamic{shader.Storage(0)};
        const Value fixed{shader.Emit(Opcode::GetCbufU32, {Value{0U}, Value{0U}})};
        auto& first{shader.Sample(dynamic_first ? dynamic : fixed)};
        auto& second{shader.Sample(dynamic_first ? fixed : dynamic)};
        auto& third{shader.Sample(fixed)};
        shader.Run();

        const auto& static_sample{dynamic_first ? second : first};
        const auto& dynamic_sample{dynamic_first ? first : second};
        CHECK(shader.program.info.texture_descriptors.size() == 2);
        CHECK(static_sample.Flags<IR::TextureInstInfo>().is_integer == 1);
        CHECK(third.Flags<IR::TextureInstInfo>().is_integer == 1);
        CHECK(dynamic_sample.Flags<IR::TextureInstInfo>().is_integer == 0);
        CHECK(shader.Descriptor(static_sample).is_integer);
        CHECK_FALSE(shader.Descriptor(dynamic_sample).is_integer);
        CHECK(shader.env.format_reads == 1);
    }
}

TEST_CASE("Failed Phi handle subtrees discard their cached values",
          "[video_core][texture_handles]") {
    for (const bool nested : {false, true}) {
        for (const bool retained_slot : {false, true}) {
            CAPTURE(nested, retained_slot);
            HandleProgram shader;
            const Value shared{shader.Storage(0)};
            const Value retained{shader.Storage(4)};
            const Value unsupported{shader.Emit(Opcode::GetRegister, {Value{IR::Reg::R0}})};
            const Value subtree{nested ? shader.Add(shared, retained) : shared};
            const Value failed{shader.Add(subtree, unsupported)};
            Value fallback{shader.Add(subtree, Value{7U})};
            if (retained_slot) {
                fallback = shader.Add(fallback, retained);
            }
            Value handle{shader.Phi(failed, fallback)};
            if (retained_slot) {
                handle = shader.Add(retained, handle);
            }
            auto& sample{shader.Sample(handle)};
            shader.Run();

            REQUIRE(shader.program.info.handle_programs.size() == 1);
            CHECK(shader.Evaluate(sample) ==
                  0x1234 + 7 + (nested ? 0x50 : 0) + (retained_slot ? 2 * 0x50 : 0));
            CHECK(shader.Evaluate(sample, 0x5678, 0x90) ==
                  0x5678 + 7 + (nested ? 0x90 : 0) + (retained_slot ? 2 * 0x90 : 0));
        }
    }
}
