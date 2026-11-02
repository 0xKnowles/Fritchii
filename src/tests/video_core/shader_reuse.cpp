// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <barrier>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/translate/translate.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "video_core/renderer_vulkan/vk_shader_reuse.h"

namespace {
class NativeEnvironment final : public Shader::Environment {
public:
    bool native{true};
    bool pointer{true};
    bool missing_pointer_query{};
    u32 surface_bank{2};
    u32 pointer_queries{};
    u64 ReadInstruction(u32 address) override {
        REQUIRE(address == 0x50);
        // Native copy R260 <- R3. It is not an ordinary masked Maxwell MOV.
        return 0x5c98000000000000ULL | (10ULL << 44) | (7ULL << 16) |
               (1ULL << 8) | (3ULL << 20) | 4;
    }
    u32 ReadCbufValue(u32 bank, u32 offset) override {
        REQUIRE(bank == surface_bank);
        REQUIRE(offset == 0x120);
        return 0x123;
    }
    bool IsCbufStorageBufferPointer(u32 bank, u32 offset) override {
        REQUIRE(bank == 4);
        REQUIRE(offset == 0x20);
        ++pointer_queries;
        if (missing_pointer_query) throw Shader::RuntimeError("Uncached pointer query");
        return pointer;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        REQUIRE(handle == 0x123);
        return Shader::TextureType::ColorArray2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        REQUIRE(handle == 0x123);
        return Shader::TexturePixelFormat::R32_UINT;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        REQUIRE(handle == 0x123);
        return true;
    }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 5; }
    u32 SurfaceBoundBuffer() const override { return surface_bank; }
    bool HasNativeInstructions() const override { return native; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64, u64) override {}
};

TEST_CASE("Shader reuse forwards and compares native environment properties",
          "[video_core][ampere][shader_reuse]") {
    NativeEnvironment env;
    env.native = GENERATE(false, true);
    env.pointer = GENERATE(false, true);
    Vulkan::RecordingEnvironment recorded{env};
    REQUIRE(recorded.HasNativeInstructions() == env.native);
    REQUIRE(recorded.TextureBoundBuffer() == 5);
    REQUIRE(recorded.SurfaceBoundBuffer() == 2);
    REQUIRE(recorded.IsCbufStorageBufferPointer(4, 0x20) == env.pointer);
    REQUIRE(env.pointer_queries == 1);
    const auto reads = recorded.TakeReads();
    REQUIRE(reads.Matches(env));
    REQUIRE(env.pointer_queries == 2);
    env.native = !env.native;
    REQUIRE_FALSE(reads.Matches(env));
    env.native = !env.native;
    env.surface_bank = 5;
    REQUIRE_FALSE(reads.Matches(env));
    env.surface_bank = 2;
    env.pointer = !env.pointer;
    REQUIRE_FALSE(reads.Matches(env));
    env.pointer = !env.pointer;
    env.missing_pointer_query = true;
    REQUIRE_FALSE(reads.Matches(env));
    env.missing_pointer_query = false;
    REQUIRE(reads.Matches(env));
}

TEST_CASE("Recorded native translation retains extended register copies",
          "[video_core][ampere][shader_reuse]") {
    NativeEnvironment env;
    Vulkan::RecordingEnvironment recorded{env};
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::Maxwell::Translate(recorded, &block, 0x50, 0x58);
    const auto copy = std::ranges::find_if(block, [](const auto& inst) {
        return inst.GetOpcode() == Shader::IR::Opcode::SetRegister;
    });
    REQUIRE(copy != block.end());
    REQUIRE(static_cast<u32>(copy->Arg(0).Reg()) == 260);
    REQUIRE(copy->Arg(1).InstRecursive()->GetOpcode() == Shader::IR::Opcode::GetRegister);
    REQUIRE(copy->Arg(1).InstRecursive()->Arg(0).Reg() == Shader::IR::Reg::R3);
}

TEST_CASE("Recorded native surface writes retain a different descriptor bank",
          "[video_core][ampere][shader_reuse]") {
    NativeEnvironment env;
    Vulkan::RecordingEnvironment recorded{env};
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::IR::IREmitter ir{block};
    Shader::IR::TextureInstInfo flags{};
    flags.type.Assign(Shader::TextureType::ColorArray2D);
    ir.ImageWrite(ir.Imm32(0x120U), ir.CompositeConstruct(ir.Imm32(0U), ir.Imm32(0U),
                  ir.Imm32(0U)), ir.CompositeConstruct(ir.Imm32(1U), ir.Imm32(2U),
                  ir.Imm32(3U), ir.Imm32(4U)), flags);
    Shader::IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    REQUIRE_NOTHROW(Shader::Optimization::TexturePass(recorded, program, host));
    REQUIRE(program.info.image_descriptors.size() == 1);
    const auto& descriptor = program.info.image_descriptors.front();
    REQUIRE(descriptor.cbuf_index == 2);
    REQUIRE(descriptor.cbuf_offset == 0x120);
    REQUIRE(descriptor.is_written);
    REQUIRE(descriptor.is_integer);
    REQUIRE(recorded.TakeReads().Matches(env));
}
} // namespace

TEST_CASE("Parallel shader cache misses build a matching translation once",
          "[video_core][shader_reuse]") {
    Vulkan::ShaderReuseCache cache;
    Vulkan::ShaderTranslationKey key{};
    constexpr size_t WorkerCount = 8;
    std::barrier start{static_cast<std::ptrdiff_t>(WorkerCount)};
    std::atomic<u32> builds{};
    std::array<std::shared_ptr<const Vulkan::ReusableShaders>, WorkerCount> results;
    std::array<std::jthread, WorkerCount> workers;
    for (size_t i = 0; i < WorkerCount; ++i) {
        workers[i] = std::jthread([&, i] {
            // All workers observe the same cold miss before anyone may publish it.
            auto shaders = cache.Find(key, {}).shaders;
            start.arrive_and_wait();
            if (!shaders) {
                auto guard = cache.LockBuild(key);
                shaders = cache.Find(key, {}).shaders;
                if (!shaders) {
                    ++builds;
                    shaders = std::make_shared<Vulkan::ReusableShaders>();
                    cache.Add(key, shaders);
                }
            }
            results[i] = std::move(shaders);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    REQUIRE(builds == 1);
    REQUIRE(cache.Contains(key));
    REQUIRE(results.front());
    for (const auto& result : results) {
        REQUIRE(result == results.front());
    }
}

TEST_CASE("Shader build reservations survive growth and release after failure",
          "[video_core][shader_reuse]") {
    Vulkan::ShaderReuseCache cache;
    Vulkan::ShaderTranslationKey key{};
    {
        auto guard = cache.LockBuild(key);
        REQUIRE_FALSE(cache.Contains(key));
        REQUIRE_FALSE(cache.Find(key, {}).shaders);
        REQUIRE_FALSE(cache.Find(key, {}).rejected);
        // Other keys must not wait for this key; rehashing must not move its mutex.
        for (u64 i = 1; i <= 128; ++i) {
            auto other = key;
            other.unique_hashes[0] = i;
            auto other_guard = cache.LockBuild(other);
            REQUIRE(other_guard.owns_lock());
        }
    }
    REQUIRE_THROWS_AS([&] {
        auto guard = cache.LockBuild(key);
        throw Shader::RuntimeError("translation failed");
    }(), Shader::RuntimeError);
    auto retry = cache.LockBuild(key);
    REQUIRE(retry.owns_lock());
    cache.Add(key, std::make_shared<Vulkan::ReusableShaders>());
    REQUIRE(cache.Find(key, {}).shaders);
}

TEST_CASE("Shader build reservations still validate environment variants",
          "[video_core][shader_reuse]") {
    Vulkan::ShaderReuseCache cache;
    Vulkan::ShaderTranslationKey key{};
    NativeEnvironment env;
    std::array<Shader::Environment*, 1> envs{&env};
    {
        auto guard = cache.LockBuild(key);
        Vulkan::RecordingEnvironment recorded{env};
        recorded.IsCbufStorageBufferPointer(4, 0x20);
        auto shaders = std::make_shared<Vulkan::ReusableShaders>();
        shaders->reads.push_back(recorded.TakeReads());
        cache.Add(key, shaders);
    }
    REQUIRE(cache.Find(key, envs).shaders);
    env.pointer = false;
    auto guard = cache.LockBuild(key);
    const auto different = cache.Find(key, envs);
    REQUIRE_FALSE(different.shaders);
    REQUIRE(different.rejected);
}

TEST_CASE("Native root constants survive translation and shader-info collection",
          "[video_core][ampere][root_table]") {
    for (bool native : {false, true}) {
        for (u32 bank : {17U, 18U, 23U, 24U, 31U}) {
            NativeEnvironment env;
            env.native = native;
            Vulkan::RecordingEnvironment recorded{env};
            Shader::ObjectPool<Shader::IR::Inst> pool;
            Shader::IR::Block block{pool};
            Shader::Maxwell::TranslatorVisitor visitor{recorded, block};
            const u64 instruction = (u64{bank} << 34) | (2ULL << 20);
            if (bank >= 18 && !(native && bank >= 24)) {
                REQUIRE_THROWS_AS(visitor.GetCbuf(instruction), Shader::NotImplementedException);
                REQUIRE_THROWS_AS(visitor.GetFloatCbuf(instruction), Shader::NotImplementedException);
                REQUIRE_THROWS_AS(visitor.GetPackedCbuf(instruction), Shader::NotImplementedException);
                continue;
            }
            REQUIRE(visitor.GetCbuf(instruction).InstRecursive()->Arg(0).U32() == bank);
            REQUIRE(visitor.GetFloatCbuf(instruction).InstRecursive()->Arg(0).U32() == bank);
            REQUIRE_NOTHROW(visitor.GetPackedCbuf(instruction));
            Shader::IR::Program program;
            program.stage = Shader::Stage::VertexB;
            program.blocks = {&block};
            program.post_order_blocks = {&block};
            REQUIRE_NOTHROW(Shader::Optimization::CollectShaderInfoPass(recorded, program));
            REQUIRE(program.info.constant_buffer_descriptors.size() == 1);
            REQUIRE(program.info.constant_buffer_descriptors.front().index == bank);
            REQUIRE(program.info.constant_buffer_mask == (1U << bank));
            REQUIRE(program.info.constant_buffer_used_sizes[bank] == 16);
            program.stage = Shader::Stage::Compute;
            REQUIRE_THROWS_AS(Shader::Optimization::CollectShaderInfoPass(recorded, program),
                              Shader::NotImplementedException);
        }
    }
}
