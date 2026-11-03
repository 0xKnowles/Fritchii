// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/glasm/emit_glasm.h"
#include "shader_recompiler/backend/glsl/emit_glsl.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
using namespace Shader;

class TestEnvironment final : public Environment {
public:
    explicit TestEnvironment(bool native_ = false) : native{native_} { stage = Stage::Compute; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return TextureType::Color2D; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    bool HasNativeInstructions() const override { return native; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}
    bool native;
};

struct Program {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Profile profile{};

    Program() {
        program.stage = Stage::Compute;
        program.workgroup_size = {32, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        profile.supported_spirv = 0x00010300;
        profile.min_ssbo_alignment = 16;
    }
};

std::vector<std::vector<u32>> Instructions(const std::vector<u32>& code, u32 opcode) {
    std::vector<std::vector<u32>> result;
    for (size_t i = 5; i < code.size();) {
        const u32 count = code[i] >> 16;
        REQUIRE(count > 0);
        REQUIRE(i + count <= code.size());
        if ((code[i] & 0xffff) == opcode) {
            result.emplace_back(code.begin() + i + 1, code.begin() + i + count);
        }
        i += count;
    }
    return result;
}
} // namespace

TEST_CASE("Native LDG cache policy reaches every global load width", "[video_core][coherence]") {
    for (const bool native : {false, true}) {
        for (u32 cache = 0; cache < 4; ++cache) {
            for (u32 size = 0; size < 7; ++size) {
                TestEnvironment env{native};
                Program fixture;
                Maxwell::TranslatorVisitor visitor{env, fixture.block};
                visitor.LDG((u64{cache} << 46) | (u64{size} << 48));
                const auto& instructions = fixture.block.Instructions();
                const auto load = std::ranges::find_if(instructions, [](const IR::Inst& inst) {
                    return inst.GetOpcode() >= IR::Opcode::LoadGlobalU8 &&
                           inst.GetOpcode() <= IR::Opcode::LoadGlobal128;
                });
                REQUIRE(load != instructions.end());
                CHECK(load->Flags<bool>() == (native && cache == 1));
                Optimization::CollectShaderInfoPass(env, fixture.program);
                CHECK(fixture.program.info.requires_coherent_global_memory ==
                      (native && cache == 1));
            }
        }
    }
}

TEST_CASE("Global load coherency survives storage buffer tracking", "[video_core][coherence]") {
    for (const bool tracked : {false, true}) {
        for (const bool coherent : {false, true}) {
            for (u32 size = 0; size < 7; ++size) {
                TestEnvironment env;
                Program fixture;
                IR::IREmitter ir{fixture.block};
                const auto low = tracked ? ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x310))
                                         : ir.LocalInvocationIdX();
                const auto address = ir.PackUint2x32(ir.CompositeConstruct(low, ir.Imm32(0)));
                switch (size) {
                case 0: (void)ir.LoadGlobalU8(address, coherent); break;
                case 1: (void)ir.LoadGlobalS8(address, coherent); break;
                case 2: (void)ir.LoadGlobalU16(address, coherent); break;
                case 3: (void)ir.LoadGlobalS16(address, coherent); break;
                case 4: (void)ir.LoadGlobal32(address, coherent); break;
                case 5: (void)ir.LoadGlobal64(address, coherent); break;
                case 6: (void)ir.LoadGlobal128(address, coherent); break;
                }
                HostTranslateInfo host{};
                host.min_ssbo_alignment = 16;
                Optimization::GlobalMemoryToStorageBufferPass(fixture.program, host);
                const auto expected = static_cast<IR::Opcode>(
                    static_cast<u32>(tracked ? IR::Opcode::LoadStorageU8 : IR::Opcode::LoadGlobalU8) + size);
                const auto& instructions = fixture.block.Instructions();
                const auto load = std::ranges::find_if(instructions, [expected](const IR::Inst& inst) {
                    return inst.GetOpcode() == expected;
                });
                REQUIRE(load != instructions.end());
                CHECK(load->Flags<bool>() == coherent);
                Optimization::CollectShaderInfoPass(env, fixture.program);
                CHECK(fixture.program.info.requires_coherent_global_memory == coherent);
            }
        }
    }
}

TEST_CASE("Coherent storage aliases and barrier semantics are emitted in SPIR-V",
          "[video_core][coherence]") {
    for (const bool coherent : {false, true}) {
        for (const bool aliasing : {false, true}) {
            Program fixture;
            fixture.profile.support_descriptor_aliasing = aliasing;
            fixture.program.info.requires_coherent_global_memory = coherent;
            fixture.program.info.used_storage_buffer_types = IR::Type::U32 | IR::Type::F32;
            fixture.program.info.storage_buffers_descriptors = {{0, 0, 1, true}, {0, 16, 1, true}};
            IR::IREmitter ir{fixture.block};
            ir.Barrier();
            const auto code = Backend::SPIRV::EmitSPIRV(fixture.profile, fixture.program);
            std::set<u32> blocks, coherent_members, storage_structs;
            std::map<u32, u32> pointers, constants;
            for (const auto& op : Instructions(code, 71)) {
                if (op.size() >= 2 && op[1] == 2) blocks.insert(op[0]);
            }
            for (const auto& op : Instructions(code, 72)) {
                if (op.size() >= 3 && op[1] == 0 && op[2] == 23) coherent_members.insert(op[0]);
            }
            for (const auto& op : Instructions(code, 32)) {
                if (op.size() >= 3 && op[1] == 12) pointers[op[0]] = op[2];
            }
            for (const auto& op : Instructions(code, 59)) {
                if (op.size() >= 3 && op[2] == 12) {
                    REQUIRE(pointers.contains(op[0]));
                    storage_structs.insert(pointers.at(op[0]));
                }
            }
            REQUIRE(storage_structs.size() == (aliasing ? 2 : 1));
            for (const u32 type : storage_structs) {
                CHECK(blocks.contains(type));
                CHECK(coherent_members.contains(type) == coherent);
            }
            for (const auto& op : Instructions(code, 43)) {
                if (op.size() >= 3) constants[op[1]] = op[2];
            }
            const auto barriers = Instructions(code, 224);
            REQUIRE(barriers.size() == 1);
            REQUIRE(barriers[0].size() == 3);
            REQUIRE(constants.contains(barriers[0][2]));
            CHECK(constants.at(barriers[0][2]) == (0x108U | (coherent ? 0x40U : 0U)));
        }
    }
}

TEST_CASE("GLSL and GLASM emit coherent storage and barrier operations",
          "[video_core][coherence]") {
    for (const bool coherent : {false, true}) {
        for (const bool pointer : {false, true}) {
            for (const bool fallback : {false, true}) {
              for (const bool glsl_backend : {false, true}) {
                Program fixture;
                auto& info = fixture.program.info;
                info.requires_coherent_global_memory = coherent;
                info.used_storage_buffer_types = IR::Type::U32;
                info.storage_buffers_descriptors = {{0, 0, 1, true}};
                info.nvn_buffer_used.set(0);
                info.uses_global_memory = fallback;
                info.constant_buffer_descriptors = {{0, 1}};
                fixture.profile.support_int64 = true;
                IR::IREmitter ir{fixture.block};
                const auto offset = ir.LocalInvocationIdX();
                info.uses_local_invocation_id = true;
                if (fallback) {
                    const auto addr = ir.PackUint2x32(ir.CompositeConstruct(offset, ir.Imm32(0)));
                    const auto value = ir.LoadGlobal32(addr, coherent);
                    ir.WriteGlobal32(addr, value);
                } else {
                    fixture.block.AppendNewInst(IR::Opcode::LoadStorage32, {ir.Imm32(0), offset});
                    fixture.block.AppendNewInst(IR::Opcode::WriteStorage32,
                                                {ir.Imm32(0), offset, ir.Imm32(42)});
                }
                ir.Barrier();
                if (glsl_backend) {
                    const auto glsl = Backend::GLSL::EmitGLSL(fixture.profile, fixture.program);
                    CHECK((glsl.find("coherent ") != std::string::npos) == coherent);
                    CHECK((glsl.find("groupMemoryBarrier();") != std::string::npos) == coherent);
                    CHECK(glsl.find("barrier();") != std::string::npos);
                    if (fallback) {
                        CHECK(glsl.find("uint LoadGlobal32(uint64_t addr)") != std::string::npos);
                        CHECK(glsl.find("void WriteGlobal32(uint64_t addr,uint data)") != std::string::npos);
                    }
                } else {
                    RuntimeInfo runtime{};
                    runtime.glasm_use_storage_buffers = !pointer;
                    const auto code = Backend::GLASM::EmitGLASM(fixture.profile, runtime, fixture.program);
                    const std::string load = pointer ? "LOAD" : "LDB";
                    const std::string store = pointer ? "STORE" : "STB";
                    CHECK(code.find(load + (coherent ? ".COH.U32" : ".U32")) != std::string::npos);
                    CHECK(code.find(store + (coherent ? ".COH.U32" : ".U32")) != std::string::npos);
                    CHECK((code.find("MEMBAR.CTA;\nBAR;") != std::string::npos) == coherent);
                }
              }
            }
        }
    }
}

TEST_CASE("Joining vertex storage preserves coherent memory requirement", "[video_core][coherence]") {
    for (const bool a : {false, true}) {
        for (const bool b : {false, true}) {
            Info vertex_a{}, vertex_b{};
            vertex_a.requires_coherent_global_memory = a;
            vertex_b.requires_coherent_global_memory = b;
            Optimization::JoinStorageInfo(vertex_a, vertex_b);
            CHECK(vertex_a.requires_coherent_global_memory == (a || b));
        }
    }
}
