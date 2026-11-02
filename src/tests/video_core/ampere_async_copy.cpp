// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <unordered_map>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"

namespace {
using Shader::Ampere::Instruction;
constexpr Instruction Exit{{0x794d, 0, 0x03800000, 0}};

u32 Address(u32 index) {
    return 0x50 + index * 8 + ((index + 1) / 3) * 8;
}

Instruction Copy(u32 guard, u32 enable, s32 global_offset, s32 shared_offset, u32 shared = 6,
                 u32 global = 2) {
    return {
        {0xfae | (guard << 12) | (shared << 16) | (global << 24),
         (static_cast<u32>(global_offset) & 0xfff) | (static_cast<u32>(shared_offset / 16) << 16),
         0x0c101c08U | ((enable & 7) << 23) | ((enable >> 3) << 6), 0}};
}

struct CopyMachine {
    std::array<u32, 256> registers{};
    std::array<bool, 7> predicates{};
    std::unordered_map<u64, u32> global;
    std::unordered_map<u32, u32> shared;
    u32 reads{}, writes{};

    bool Predicate(u32 predicate) const {
        return ((predicate & 7) == 7 || predicates[predicate & 7]) != bool(predicate & 8);
    }

    void Run(std::span<const u64> code) {
        for (u32 pc = 0; pc < code.size(); ++pc) {
            const u64 word = code[pc];
            const auto bits = [&](u32 start, u32 count) {
                return static_cast<u32>((word >> start) & ((u64{1} << count) - 1));
            };
            if (!Predicate(bits(16, 4)))
                continue;
            const u32 dest = bits(0, 8), address_reg = bits(8, 8);
            const s32 offset = static_cast<s32>(bits(20, 24) << 8) >> 8;
            if ((word >> 56) == 0x01) {
                if (dest != 255)
                    registers[dest] = bits(20, 32);
            } else if ((word & 0xfff0000000000000ULL) == 0xe240000000000000ULL) {
                const u32 target = Address(pc) + 8 + offset;
                const auto start = pc;
                while (pc < code.size() && Address(pc) != target)
                    ++pc;
                REQUIRE(pc > start);
                REQUIRE(pc < code.size());
                --pc;
            } else if ((word & 0xfff0000000000000ULL) == 0xeed0000000000000ULL) {
                REQUIRE(bits(45, 1) == 1);
                REQUIRE(bits(48, 3) == 6);
                REQUIRE(dest % 4 == 0);
                REQUIRE(address_reg % 2 == 0);
                const u64 address =
                    registers[address_reg] | (u64{registers[address_reg + 1]} << 32);
                for (u32 i = 0; i < 4; ++i) {
                    // Missing memory is a test failure, including for zero-filled copies.
                    const u64 source = address + static_cast<u64>(offset) + i * 4;
                    REQUIRE(global.contains(source));
                    registers[dest + i] = global.at(source);
                    ++reads;
                }
            } else if ((word & 0xfff8000000000000ULL) == 0xef58000000000000ULL) {
                REQUIRE(bits(48, 3) == 6);
                REQUIRE(dest % 4 == 0);
                for (u32 i = 0; i < 4; ++i) {
                    shared[registers[address_reg] + static_cast<u32>(offset) + i * 4] =
                        registers[dest + i];
                    ++writes;
                }
            } else if ((word & 0xfff0000000000000ULL) == 0xe300000000000000ULL) {
                return;
            } else {
                FAIL("Unexpected copy instruction " << word);
            }
        }
    }
};

class CopyEnvironment final : public Shader::Environment {
public:
    explicit CopyEnvironment(std::span<const u64> code_) : code{code_} {
        stage = Shader::Stage::Compute;
    }
    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < code.size(); ++i)
            if (Address(i) == address)
                return code[i];
        FAIL("Frontend read outside lowered copy program at " << address);
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override {
        FAIL("Unexpected cbuf read");
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 4096;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {32, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    bool HasNativeInstructions() const override {
        return true;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> code;
};
} // namespace

TEST_CASE("Ampere global to shared copy survives production control flow and IR translation",
          "[video_core][ampere][async_copy][fire_emblem]") {
    for (u32 prefix = 0; prefix < 3; ++prefix) {
        for (const u32 guard : {0U, 7U, 8U}) {
            for (const u32 enable : {4U, 12U}) {
                std::vector<Instruction> code(prefix, Instruction{{0x00ff7802, 0, 0xf00, 0}});
                code.push_back(Copy(guard, enable, -16, 32));
                code.push_back(Exit);
                const auto lowered = Shader::Ampere::LowerToMaxwell(code);
                REQUIRE(lowered);
                CopyEnvironment env{lowered.instructions};
                Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
                Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
                Shader::ObjectPool<Shader::IR::Inst> inst_pool;
                Shader::ObjectPool<Shader::IR::Block> block_pool;
                const auto asl = Shader::Maxwell::BuildASL(inst_pool, block_pool, env, cfg,
                                                           Shader::HostTranslateInfo{});
                u32 loads = 0, stores = 0;
                for (const auto& node : asl) {
                    if (node.type != Shader::IR::AbstractSyntaxNode::Type::Block)
                        continue;
                    for (const auto& inst : *node.data.block) {
                        loads += inst.GetOpcode() == Shader::IR::Opcode::LoadGlobal128;
                        stores += inst.GetOpcode() == Shader::IR::Opcode::WriteSharedU128;
                        REQUIRE(inst.GetOpcode() != Shader::IR::Opcode::Barrier);
                    }
                }
                REQUIRE(loads == 1);
                REQUIRE(stores == 1);
            }
        }
    }
}

TEST_CASE("Ampere global to shared copy guards reads and zero fills disabled sources",
          "[video_core][ampere][async_copy][fire_emblem]") {
    for (u32 prefix = 0; prefix < 3; ++prefix) {
        for (const u32 guard : {0U, 7U, 8U, 15U}) {
            for (u32 enable = 0; enable < 16; ++enable) {
                for (const bool condition : {false, true}) {
                    for (const s32 offset : {-2048, -16, 0, 16, 2032}) {
                        std::vector<Instruction> code(prefix,
                                                      Instruction{{0x00ff7802, 0, 0xf00, 0}});
                        code.push_back(Copy(guard, enable, offset, offset));
                        code.push_back(Exit);
                        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
                        INFO(lowered.error);
                        REQUIRE(lowered);
                        CopyMachine machine;
                        machine.registers[2] = 0xfffffff0U;
                        machine.registers[3] = 3;
                        machine.registers[6] = 4096;
                        machine.predicates.fill(condition);
                        const auto before = machine.registers;
                        const bool active = machine.Predicate(guard);
                        const bool enabled = machine.Predicate(enable);
                        const std::array data{0x10203040U, 0x50607080U, 0x90a0b0c0U, 0xd0e0f000U};
                        for (u32 i = 0; i < 4; ++i) {
                            if (active && enabled)
                                machine.global[0x3fffffff0ULL + static_cast<u64>(offset) + 4 * i] =
                                    data[i];
                            machine.shared[4096 + static_cast<u32>(offset) + 4 * i] = 0xfeedface;
                        }
                        machine.Run(lowered.instructions);
                        REQUIRE(machine.reads == (active && enabled ? 4 : 0));
                        REQUIRE(machine.writes == (active ? 4 : 0));
                        REQUIRE(machine.shared.size() == 4);
                        for (u32 i = 0; i < 4; ++i)
                            REQUIRE(machine.shared.at(4096 + static_cast<u32>(offset) + 4 * i) ==
                                    (!active   ? 0xfeedfaceU
                                     : enabled ? data[i]
                                               : 0U));
                        for (u32 i = 0; i < 246; ++i)
                            REQUIRE(machine.registers[i] == before[i]);
                        for (bool value : machine.predicates)
                            REQUIRE(value == condition);
                    }
                }
            }
        }
    }
}

TEST_CASE("Ampere global to shared copy rejects unsupported addressing and modes",
          "[video_core][ampere][async_copy][fire_emblem]") {
    const auto valid = Copy(7, 7, 0, 0);
    REQUIRE(Shader::Ampere::LowerToMaxwell(std::array{valid, Exit}));
    for (const u32 bit :
         {0U, 3U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 20U, 21U, 22U, 26U, 27U, 28U, 31U}) {
        auto invalid = valid;
        invalid.words[2] ^= 1U << bit;
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(std::array{invalid, Exit}));
    }
    for (const u32 bit : {12U, 13U, 14U, 15U}) {
        auto invalid = valid;
        invalid.words[1] ^= 1U << bit;
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(std::array{invalid, Exit}));
    }
    for (const u32 address : {1U, 245U, 246U, 254U, 255U})
        REQUIRE_FALSE(
            Shader::Ampere::LowerToMaxwell(std::array{Copy(7, 7, 0, 0, 6, address), Exit}));
    for (const u32 address : {246U, 254U, 255U})
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(std::array{Copy(7, 7, 0, 0, address), Exit}));
}

TEST_CASE("Ampere synchronous copy commit and scoreboard wait retain memory ordering",
          "[video_core][ampere][async_copy][fire_emblem]") {
    const Instruction commit{{0x79af, 0, 0, 0}};
    const Instruction wait{{0x791a, 0x8000, 0, 0}};
    const auto lowered =
        Shader::Ampere::LowerToMaxwell(std::array{Copy(7, 12, 0, 0), commit, wait, Exit});
    REQUIRE(lowered);
    for (u32 index : {1U, 2U})
        REQUIRE(lowered.instructions[lowered.source_instruction_offsets[index]] ==
                0xef98000000070000ULL);
    for (u32 field = 0; field < 3; ++field) {
        auto invalid = commit;
        invalid.words[field] ^= 1;
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(std::array{invalid, Exit}));
    }
    auto invalid_wait = wait;
    invalid_wait.words[1] = 0x7000;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(std::array{invalid_wait, Exit}));
}

TEST_CASE("Ampere software matrix lowering compiles through the production SPIRV backend",
          "[video_core][ampere][matrix][fire_emblem]") {
    for (u32 mode : {0U, 0x800U}) {
        for (const bool compact : {false, true}) {
        const Instruction matrix{{0x0810723c, 12, mode | 0xff, 0}};
        auto lowered = Shader::Ampere::LowerToMaxwell(std::array{matrix, Exit}, false, compact);
        REQUIRE(lowered);
        // Initialize native operands from the lane id, and retain both output registers through
        // a shared-memory write so the optimizer cannot discard the matrix operation.
        std::vector<u64> code;
        for (u32 reg : {8U, 9U, 10U, 11U, 12U, 13U})
            code.push_back(0xf0c8000000070000ULL | reg); // S2R SR_LANEID
        code.insert(code.end(), lowered.instructions.begin(), lowered.instructions.end() - 1);
        code.push_back(0xef58000000070000ULL | (u64{5} << 48) | (u64{255} << 8) | 16);
        code.push_back(lowered.instructions.back());
        CopyEnvironment env{code};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
        Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
        Shader::ObjectPool<Shader::IR::Inst> inst_pool;
        Shader::ObjectPool<Shader::IR::Block> block_pool;
        auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg,
                                                         Shader::HostTranslateInfo{});
        REQUIRE_FALSE(program.info.uses_tensor_mul_add);
        Shader::Profile profile{};
        profile.supported_spirv = 0x00010300;
        profile.max_compute_shared_memory_size = 49152;
        profile.supported_subgroup_stages = 1U << static_cast<u32>(Shader::Stage::Compute);
        profile.max_subgroup_size = 32;
        REQUIRE_FALSE(profile.support_cooperative_matrix);
        const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
        REQUIRE(spirv.size() > 5);
        REQUIRE(spirv[0] == 0x07230203);
        }
    }
}

TEST_CASE("Fire Emblem remaining runtime instructions compile through SSA and SPIRV",
          "[video_core][ampere][fire_emblem]") {
    const std::array samples{
        Instruction{{0x07087227, 0xb, 0x07820008, 0x000fc800}},
        Instruction{{0x7960783b, 0x00508005, 0x08000200, 0x000fe800}},
        Instruction{{0x89d87230, 0x8000008d, 0, 0x040fe400}},
        Instruction{{0x00f67812, 0x10, 0x078ec0ff, 0x000fe200}},
        Instruction{{0x000c7308, 0xc, 0x2400, 0x000e3000}},
        Instruction{{0x12127433, 0x7c007c00, 0x0380a200, 0x000fe200}}};
    for (const auto sample : samples) {
        const std::array native{Instruction{{0xfc02, 0, 0, 0}}, sample, Exit};
        const auto lowered = Shader::Ampere::LowerToMaxwell(native);
        INFO(lowered.error);
        REQUIRE(lowered);
        std::vector<u64> code;
        for (u32 r : {0U, 7U, 8U, 9U, 11U, 12U, 18U, 121U, 137U, 141U, 221U})
            code.push_back(0xf0c8000000070000ULL | r);
        code.insert(code.end(), lowered.instructions.begin(), lowered.instructions.end() - 1);
        const u32 destination = sample.Bits(16, 8);
        code.push_back(Shader::Ampere::NativeRegisterCopy(0, 16, destination >= 216 ? 1 : 0, destination));
        code.push_back(0xef58000000070000ULL | (u64{4} << 48) | (u64{255} << 8) | 16);
        code.push_back(lowered.instructions.back());
        CopyEnvironment env{code};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
        Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
        Shader::ObjectPool<Shader::IR::Inst> inst_pool;
        Shader::ObjectPool<Shader::IR::Block> block_pool;
        auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg,
                                                         Shader::HostTranslateInfo{});
        Shader::Profile profile{};
        profile.supported_spirv = 0x00010300;
        profile.max_compute_shared_memory_size = 49152;
        profile.supported_subgroup_stages = 1U << static_cast<u32>(Shader::Stage::Compute);
        profile.max_subgroup_size = 32;
        REQUIRE(Shader::Backend::SPIRV::EmitSPIRV(profile, program).size() > 5);
    }
}

TEST_CASE("Ampere uniform F32 reduction reaches the atomic IR with FTZ and RN",
          "[video_core][ampere][fire_emblem]") {
    for (const u32 source : {1U, 220U, 254U}) {
        const std::array native{Instruction{{0xff00798e, source, 0x0c10e704, 0x000fe200}}, Exit};
        const auto lowered = Shader::Ampere::LowerToMaxwell(native);
        REQUIRE(lowered);
        CopyEnvironment env{lowered.instructions};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
        Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
        Shader::ObjectPool<Shader::IR::Inst> inst_pool;
        Shader::ObjectPool<Shader::IR::Block> block_pool;
        const auto asl = Shader::Maxwell::BuildASL(inst_pool, block_pool, env, cfg,
                                                   Shader::HostTranslateInfo{});
        u32 atomics = 0;
        for (const auto& node : asl) {
            if (node.type != Shader::IR::AbstractSyntaxNode::Type::Block) continue;
            for (const auto& inst : *node.data.block) {
                if (inst.GetOpcode() != Shader::IR::Opcode::GlobalAtomicAddF32) continue;
                ++atomics;
                const auto control = inst.Flags<Shader::IR::FpControl>();
                REQUIRE(control.rounding == Shader::IR::FpRounding::RN);
                REQUIRE(control.fmz_mode == Shader::IR::FmzMode::FTZ);
                REQUIRE(inst.MayHaveSideEffects());
            }
        }
        REQUIRE(atomics == 1);
    }
    for (const u32 type : {1U, 2U, 4U, 5U, 6U, 7U}) {
        const std::array native{Instruction{{0xff00798e, 1, 0x0c10e104U | (type << 9), 0}}, Exit};
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(native));
    }
}
