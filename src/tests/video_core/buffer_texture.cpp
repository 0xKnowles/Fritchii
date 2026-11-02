// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from Duck912/eden-fefw 8c6f0522b6ee07caa47ab4363823d3dc00aa8ac1.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "shader_recompiler/environment.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/ir_opt/buffer_texture.h"
#include "shader_recompiler/ir_opt/conditional_phi.h"
#include "video_core/buffer_cache/storage_buffer_size.h"
#include "video_core/shader_environment.h"

namespace {
namespace IR = Shader::IR;
class EmptyEnvironment : public Shader::Environment {
public:
    bool HasNativeInstructions() const override { return true; }
    bool IsCbufStorageBufferPointer(u32, u32) override { return true; }
    u64 ReadInstruction(u32) override { throw std::logic_error("unexpected instruction read"); }
    u32 ReadCbufValue(u32, u32) override { throw std::logic_error("unexpected constant folding"); }
    Shader::TextureType ReadTextureType(u32) override { return {}; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return local_size; }
    u32 local_size{};
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64, u64) override {}
};


TEST_CASE("Texture handle tracking cannot substitute a buffer address for loaded data", "[video_core][buffer_texture][ampere]") {
    const u32 mode = GENERATE(0U, 1U, 2U, 3U, 4U);
    class HandleEnvironment final : public EmptyEnvironment {
    public:
        u32 reads{};
        u32 ReadCbufValue(u32 bank, u32 offset) override {
            ++reads;
            REQUIRE(bank == 0);
            REQUIRE(offset == 0x510);
            return 7;
        }
        Shader::TextureType ReadTextureType(u32 handle) override {
            REQUIRE(handle == 7);
            return Shader::TextureType::Color2D;
        }
    } env;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const IR::U32 pointer{ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x510U))};
    IR::U32 handle = pointer;
    const auto storage_load = [&](IR::Opcode opcode) {
        const auto it = block.PrependNewInst(block.end(), opcode, {ir.Imm32(0U), pointer});
        return IR::Value{&*it};
    };
    if (mode == 1 || mode == 4) handle = IR::U32{storage_load(IR::Opcode::LoadStorage32)};
    if (mode == 2) handle = IR::U32{ir.CompositeExtract(storage_load(IR::Opcode::LoadStorage64), 0)};
    if (mode == 3) {
        const IR::U64 address{ir.PackUint2x32(ir.CompositeConstruct(pointer, ir.Imm32(0x19U)))};
        handle = ir.LoadGlobal32(address);
    }
    if (mode == 4) handle = ir.BitwiseOr(handle, pointer);
    IR::TextureInstInfo info{};
    info.type.Assign(Shader::TextureType::Color2D);
    (void)ir.ImageSampleImplicitLod(handle, ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(0.0f)), {}, {}, {}, info);
    IR::Program program;
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    Shader::HostTranslateInfo host;
    if (mode == 0) {
        REQUIRE_NOTHROW(Shader::Optimization::TexturePass(env, program, host));
        REQUIRE(program.info.texture_descriptors.size() == 1);
        REQUIRE(program.info.texture_descriptors[0].cbuf_index == 0);
        REQUIRE(program.info.texture_descriptors[0].cbuf_offset == 0x510);
        REQUIRE(env.reads == 1);
    } else {
        REQUIRE_THROWS_AS(Shader::Optimization::TexturePass(env, program, host), Shader::NotImplementedException);
        REQUIRE(env.reads == 0);
    }
}


TEST_CASE("Buffer-backed texture arrays retain record indices and root descriptor bounds", "[video_core][buffer_texture][ampere]") {
    const bool decomposed = GENERATE(false, true);
    const bool constant_index = GENERATE(false, true);
    const bool query = GENERATE(false, true);
    const u32 failure = GENERATE(0U, 1U, 2U, 3U, 4U);
    class BufferEnvironment final : public EmptyEnvironment {
    public:
        u32 size{40960};
        u32 high{};
        u32 ReadCbufValue(u32 bank, u32 offset) override {
            REQUIRE(bank == 0);
            if (offset == 0x518) return size;
            REQUIRE(offset == 0x51c);
            return high;
        }
        Shader::TextureType ReadTextureType(u32) override {
            FAIL("A buffer address must not be resolved as a texture handle");
            return {};
        }
    } env;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const IR::U32 index = constant_index ? ir.Imm32(4U) : ir.GetReg(IR::Reg::R1);
    const auto constant = [&](u32 value) { return ir.Imm32(value); };
    IR::U32 product = ir.IMul(index, constant(160));
    if (decomposed) {
        const auto low = ir.BitFieldExtract(index, constant(0), constant(16));
        const auto high = ir.BitFieldExtract(index, constant(16), constant(16));
        product = ir.IAdd(ir.ShiftLeftLogical(ir.IAdd(ir.IMul(low, constant(0)), ir.IMul(high, constant(160))), constant(16)),
                         ir.IMul(low, constant(160)));
    }
    const IR::U32 pointer{ir.CompositeExtract(ir.GetCbuf(constant(0), constant(0x510), 64, false), 0)};
    const auto relative = ir.IAdd(product, constant(152));
    const auto offset = ir.ISub(ir.IAdd(pointer, relative), ir.BitwiseAnd(ir.GetCbuf(constant(0), constant(0x510)), constant(0xfffffff0)));
    const auto load = block.PrependNewInst(block.end(), IR::Opcode::LoadStorage64, {constant(0), offset});
    const IR::U32 handle{ir.CompositeExtract(IR::Value{&*load}, 0)};
    IR::TextureInstInfo flags{};
    flags.type.Assign(Shader::TextureType::ColorCube);
    const auto coordinates = ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(1.0f), ir.Imm32(0.0f));
    if (query) {
        const auto dimensions = ir.ImageQueryDimension(handle, constant(0), ir.Imm1(true));
        ir.SetReg(IR::Reg::R2, IR::U32{ir.CompositeExtract(dimensions, 0)});
    } else {
        (void)ir.ImageSampleExplicitLod(handle, coordinates, ir.Imm32(0.0f), {}, flags);
    }
    IR::Program program;
    program.stage = Shader::Stage::Fragment;
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.info.storage_buffers_descriptors.push_back({0, 0x510, 1, false});
    const auto tracked = Shader::Optimization::BufferTexture::Track(handle, program.info);
    REQUIRE(tracked);
    REQUIRE(tracked->index.Resolve() == index.Resolve());
    REQUIRE(tracked->stride == 160);
    REQUIRE(tracked->offset == 152);
    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    host.support_buffer_texture_handles = true;
    if (failure == 1) env.size -= 1;
    if (failure == 2) env.high = 1;
    if (failure == 3) host.max_per_stage_descriptor_sampled_images = 255;
    if (failure == 4) host.support_buffer_texture_handles = false;
    if (failure) {
        REQUIRE_THROWS_AS(Shader::Optimization::TexturePass(env, program, host), Shader::NotImplementedException);
    } else {
        REQUIRE_NOTHROW(Shader::Optimization::TexturePass(env, program, host));
        REQUIRE(program.info.texture_descriptors.size() == 1);
        const auto& desc = program.info.texture_descriptors.front();
        REQUIRE(desc.count == 256);
        REQUIRE(desc.cbuf_index == 0);
        REQUIRE(desc.cbuf_offset == 0x510);
        REQUIRE(desc.buffer_source.enabled);
        REQUIRE(desc.buffer_source.offset == 152);
        REQUIRE(desc.buffer_source.stride == 160);
        REQUIRE(desc.buffer_source.size == 40960);
        REQUIRE(desc.buffer_source.width_query == query);
        for (const auto& inst : block.Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ImageSampleExplicitLod || inst.GetOpcode() == IR::Opcode::ImageQueryDimensions)
                REQUIRE(inst.Arg(0).Resolve() == index.Resolve());
        }
    }
}


TEST_CASE("Buffer texture choices preserve their select and Phi indices", "[video_core][buffer_texture][ampere]") {
    const bool phi_choice = GENERATE(false, true);
    const u32 invalid = GENERATE(0U, 1U, 2U);
    class RootEnvironment final : public EmptyEnvironment {
    public:
        u32 ReadCbufValue(u32 bank, u32 offset) override {
            REQUIRE(bank == 0);
            if (offset == 0x518) return 40960;
            REQUIRE(offset == 0x51c);
            return 0;
        }
    } env;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block left{pool}, right{pool}, merge{pool};
    IR::Value first_index, second_index;
    const auto make_handle = [&](IR::Block& block, IR::Reg reg, u32 field, IR::Value& original_index) {
        IR::IREmitter ir{block};
        const auto pointer = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x510U));
        const auto index = ir.GetReg(reg);
        original_index = index;
        const auto relative = ir.IAdd(ir.IMul(index, ir.Imm32(160U)), ir.Imm32(field));
        const auto offset = ir.ISub(ir.IAdd(pointer, relative), ir.BitwiseAnd(pointer, ir.Imm32(0xfffffff0U)));
        const auto load = block.PrependNewInst(block.end(), IR::Opcode::LoadStorage64, {ir.Imm32(0U), offset});
        return IR::U32{ir.CompositeExtract(IR::Value{&*load}, 0)};
    };
    const auto first = make_handle(left, IR::Reg::R1, 152, first_index);
    const auto second = make_handle(phi_choice ? right : left, IR::Reg::R2, invalid == 2 ? 144 : 152, second_index);
    IR::IREmitter ir{merge};
    IR::IREmitter other_ir{phi_choice ? right : merge};
    const IR::U32 other = invalid == 1 ? other_ir.GetReg(IR::Reg::R8) : second;
    IR::U32 handle;
    if (phi_choice) {
        const auto phi = merge.PrependNewInst(merge.begin(), IR::Opcode::Phi);
        phi->SetFlags(IR::Type::U32);
        phi->AddPhiOperand(&left, first);
        phi->AddPhiOperand(&right, other);
        handle = IR::U32{IR::Value{&*phi}};
    } else handle = IR::U32{ir.Select(ir.GetPred(IR::Pred::P0), first, other)};
    IR::TextureInstInfo flags{};
    flags.type.Assign(Shader::TextureType::ColorCube);
    (void)ir.ImageSampleExplicitLod(handle,
        ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(1.0f), ir.Imm32(0.0f)), ir.Imm32(0.0f), {}, flags);
    IR::Program program;
    program.stage = Shader::Stage::Fragment;
    program.blocks = {&left, &right, &merge};
    program.post_order_blocks = {&merge, &right, &left};
    program.info.storage_buffers_descriptors.push_back({0, 0x510, 1, false});
    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    host.support_buffer_texture_handles = true;
    if (invalid) REQUIRE_THROWS_AS(Shader::Optimization::TexturePass(env, program, host), Shader::NotImplementedException);
    else {
        REQUIRE_NOTHROW(Shader::Optimization::TexturePass(env, program, host));
        u32 samples{};
        for (const auto& inst : merge.Instructions()) if (inst.GetOpcode() == IR::Opcode::ImageSampleExplicitLod) {
            ++samples;
            const auto* choice = inst.Arg(0).InstRecursive();
            REQUIRE(choice->GetOpcode() == (phi_choice ? IR::Opcode::Phi : IR::Opcode::SelectU32));
            if (phi_choice) {
                REQUIRE(choice->PhiBlock(0) == &left);
                REQUIRE(choice->PhiBlock(1) == &right);
                REQUIRE(choice->Arg(0).Resolve() == first_index.Resolve());
                REQUIRE(choice->Arg(1).Resolve() == second_index.Resolve());
            } else {
                REQUIRE(choice->Arg(0).InstRecursive()->GetOpcode() == IR::Opcode::GetPred);
                REQUIRE(choice->Arg(1).Resolve() == first_index.Resolve());
                REQUIRE(choice->Arg(2).Resolve() == second_index.Resolve());
            }
        }
        REQUIRE(samples == 1);
    }
}


TEST_CASE("Branch-edge facts preserve loop predicate definitions", "[video_core][buffer_texture][ampere]") {
    const bool initial = GENERATE(false, true);
    const bool reset = GENERATE(false, true);
    const bool describe_branch = GENERATE(false, true);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block entry{pool}, header{pool}, body{pool}, tail{pool};
    entry.AddBranch(&header);
    header.AddBranch(&body);
    header.AddBranch(&tail);
    body.AddBranch(&tail);
    tail.AddBranch(&header);
    const auto flag = header.PrependNewInst(header.begin(), IR::Opcode::Phi);
    const auto next = tail.PrependNewInst(tail.begin(), IR::Opcode::Phi);
    flag->SetFlags(IR::Type::U1);
    next->SetFlags(IR::Type::U1);
    flag->AddPhiOperand(&entry, IR::Value{initial});
    flag->AddPhiOperand(&tail, IR::Value{&*next});
    next->AddPhiOperand(&header, IR::Value{&*flag});
    next->AddPhiOperand(&body, IR::Value{reset});
    const auto numeric = header.PrependNewInst(header.end(), IR::Opcode::Phi);
    numeric->SetFlags(IR::Type::U32);
    numeric->AddPhiOperand(&entry, IR::Value{17U});
    numeric->AddPhiOperand(&tail, IR::Value{23U});
    IR::IREmitter ir{header};
    const auto condition = ir.ConditionRef(ir.LogicalOr(IR::U1{IR::Value{&*flag}}, ir.GetPred(IR::Pred::P0)));
    IR::Program program;
    program.blocks = {&entry, &header, &body, &tail};
    if (describe_branch) {
        IR::AbstractSyntaxNode node;
        node.type = IR::AbstractSyntaxNode::Type::If;
        node.data.if_node = {condition, &body, &tail};
        program.syntax_list.push_back(node);
    }
    const auto folded = Shader::Optimization::ConditionalPhi::Fold(program);
    bool after_phi{};
    for (const auto& inst : header.Instructions()) {
        if (inst.GetOpcode() != IR::Opcode::Phi) after_phi = true;
        else REQUIRE_FALSE(after_phi);
    }
    REQUIRE(numeric->GetOpcode() == IR::Opcode::Phi);
    const auto first = IR::Value{&*flag}.Resolve();
    const auto second = IR::Value{&*next}.Resolve();
    if (!describe_branch && initial == reset) {
        REQUIRE(first.IsImmediate());
        REQUIRE(second.IsImmediate());
        REQUIRE(first.U1() == initial);
        REQUIRE(second.U1() == initial);
        REQUIRE(folded == 2);
    } else if (describe_branch && !reset) {
        REQUIRE(second.IsImmediate());
        REQUIRE_FALSE(second.U1());
        if (!initial) {
            REQUIRE(first.IsImmediate());
            REQUIRE_FALSE(first.U1());
            REQUIRE(folded == 2);
        } else {
            REQUIRE_FALSE(first.IsImmediate());
            REQUIRE(folded == 1);
        }
    } else {
        REQUIRE_FALSE(first.IsImmediate());
        REQUIRE_FALSE(second.IsImmediate());
        REQUIRE(folded == 0);
    }
}


TEST_CASE("Boolean edge implications do not assume ambiguous operands", "[video_core][buffer_texture][ampere]") {
    const bool conjunction = GENERATE(false, true);
    const bool taken = GENERATE(false, true);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool}, yes{pool}, no{pool};
    block.AddBranch(&yes);
    block.AddBranch(&no);
    IR::IREmitter ir{block};
    const auto p = ir.GetPred(IR::Pred::P0), q = ir.GetPred(IR::Pred::P1);
    const auto condition = ir.ConditionRef(conjunction ? ir.LogicalAnd(p, ir.LogicalNot(q)) : ir.LogicalOr(p, ir.LogicalNot(q)));
    const Shader::Optimization::ConditionalPhi::Branch branch{condition, &yes, &no};
    const auto facts = Shader::Optimization::ConditionalPhi::EdgeFacts(branch, taken ? &yes : &no);
    if (conjunction == taken) {
        REQUIRE(facts.at(p.InstRecursive()) == taken);
        REQUIRE(facts.at(q.InstRecursive()) == !taken);
    } else {
        REQUIRE_FALSE(facts.contains(p.InstRecursive()));
        REQUIRE_FALSE(facts.contains(q.InstRecursive()));
    }
}


TEST_CASE("Unanchored boolean Phi cycles do not become constants", "[video_core][buffer_texture][ampere]") {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    block.AddBranch(&block);
    const auto phi = block.PrependNewInst(block.begin(), IR::Opcode::Phi);
    phi->SetFlags(IR::Type::U1);
    phi->AddPhiOperand(&block, IR::Value{&*phi});
    IR::Program program;
    program.blocks = {&block};
    REQUIRE(Shader::Optimization::ConditionalPhi::Fold(program) == 0);
    REQUIRE_FALSE(IR::Value{&*phi}.Resolve().IsImmediate());
}


TEST_CASE("Texture Phi specialization requires an infeasible incoming edge", "[video_core][buffer_texture][ampere]") {
    const u32 mode = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U);
    class RootEnvironment final : public EmptyEnvironment {
    public:
        u32 ReadCbufValue(u32 bank, u32 offset) override {
            REQUIRE(bank == 0);
            if (offset == 0x518) return 40960;
            REQUIRE(offset == 0x51c);
            return 0;
        }
    } env;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block entry{pool}, root{pool}, clobber{pool}, merge{pool}, sample{pool}, exit{pool};
    entry.AddBranch(&root);
    if (mode == 4) entry.AddBranch(&sample);
    root.AddBranch(&clobber);
    root.AddBranch(&merge);
    clobber.AddBranch(&merge);
    merge.AddBranch(&sample);
    merge.AddBranch(&exit);
    sample.AddBranch(&exit);
    if (mode == 3) exit.AddBranch(&clobber);
    IR::IREmitter ir{root};
    const auto index = ir.GetReg(IR::Reg::R1);
    const auto pointer = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x510U));
    const auto relative = ir.IAdd(ir.IMul(index, ir.Imm32(160U)), ir.Imm32(152U));
    const auto offset = ir.ISub(ir.IAdd(pointer, relative), ir.BitwiseAnd(pointer, ir.Imm32(0xfffffff0U)));
    const auto load = root.PrependNewInst(root.end(), IR::Opcode::LoadStorage64, {ir.Imm32(0U), offset});
    const IR::U32 handle{ir.CompositeExtract(IR::Value{&*load}, 0)};
    const auto selector = ir.GetReg(IR::Reg::R2);
    const auto first_condition = ir.ConditionRef(ir.IEqual(selector, ir.Imm32(1U)));
    IR::IREmitter bad_ir{clobber};
    const IR::U32 invalid = mode == 5 ? bad_ir.Imm32(0U) : bad_ir.BitCast<IR::U32>(IR::F32{bad_ir.FPAdd(bad_ir.Imm32(0.1f), bad_ir.Imm32(0.2f))});
    const auto phi = merge.PrependNewInst(merge.begin(), IR::Opcode::Phi);
    phi->SetFlags(IR::Type::U32);
    phi->AddPhiOperand(&root, handle);
    phi->AddPhiOperand(&clobber, invalid);
    IR::IREmitter merge_ir{merge};
    IR::U32 second_selector = mode == 2 ? merge_ir.GetReg(IR::Reg::R3) : selector;
    if (mode == 6) {
        const auto selected = merge.PrependNewInst(merge.begin(), IR::Opcode::Phi);
        selected->SetFlags(IR::Type::U32);
        selected->AddPhiOperand(&root, selector);
        selected->AddPhiOperand(&clobber, selector);
        second_selector = IR::U32{IR::Value{&*selected}};
    }
    const auto second_condition = merge_ir.ConditionRef(merge_ir.IEqual(second_selector, merge_ir.Imm32(mode == 1 ? 1U : 2U)));
    IR::IREmitter sample_ir{sample};
    IR::TextureInstInfo flags{};
    flags.type.Assign(Shader::TextureType::Color2D);
    (void)sample_ir.ImageSampleExplicitLod(IR::U32{IR::Value{&*phi}},
        sample_ir.CompositeConstruct(sample_ir.Imm32(0.0f), sample_ir.Imm32(1.0f)), sample_ir.Imm32(0.0f), {}, flags);
    IR::Program program;
    program.stage = Shader::Stage::Fragment;
    program.blocks = {&entry, &root, &clobber, &merge, &sample, &exit};
    program.post_order_blocks = {&exit, &sample, &merge, &clobber, &root, &entry};
    program.info.storage_buffers_descriptors.push_back({0, 0x510, 1, false});
    IR::AbstractSyntaxNode first, second;
    first.type = second.type = IR::AbstractSyntaxNode::Type::If;
    first.data.if_node = {first_condition, &clobber, &merge};
    second.data.if_node = {second_condition, &sample, &exit};
    program.syntax_list = {first, second};
    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    host.support_buffer_texture_handles = true;
    if (mode == 0 || mode == 5 || mode == 6) {
        REQUIRE_NOTHROW(Shader::Optimization::TexturePass(env, program, host));
        REQUIRE(program.info.texture_descriptors.size() == 1);
        for (const auto& inst : sample.Instructions()) if (inst.GetOpcode() == IR::Opcode::ImageSampleExplicitLod)
            REQUIRE(inst.Arg(0).Resolve() == index.Resolve());
    } else {
        REQUIRE_THROWS_AS(Shader::Optimization::TexturePass(env, program, host), Shader::NotImplementedException);
    }
}


TEST_CASE("A repeated buffer handle keeps its original index through Phi", "[video_core][buffer_texture][ampere]") {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block root{pool}, left{pool}, right{pool}, merge{pool}, sample{pool}, exit{pool};
    root.AddBranch(&left);
    root.AddBranch(&right);
    left.AddBranch(&merge);
    right.AddBranch(&merge);
    merge.AddBranch(&sample);
    merge.AddBranch(&exit);
    sample.AddBranch(&exit);
    IR::IREmitter ir{root};
    const auto index = ir.GetReg(IR::Reg::R1);
    const auto pointer = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x510U));
    const auto relative = ir.IAdd(ir.IMul(index, ir.Imm32(160U)), ir.Imm32(152U));
    const auto offset = ir.ISub(ir.IAdd(pointer, relative), ir.BitwiseAnd(pointer, ir.Imm32(0xfffffff0U)));
    const auto load = root.PrependNewInst(root.end(), IR::Opcode::LoadStorage64, {ir.Imm32(0U), offset});
    const IR::U32 low{ir.CompositeExtract(IR::Value{&*load}, 0)};
    const auto first_condition = ir.ConditionRef(ir.GetPred(IR::Pred::P0));
    const auto phi = merge.PrependNewInst(merge.begin(), IR::Opcode::Phi);
    phi->SetFlags(IR::Type::U32);
    phi->AddPhiOperand(&left, low);
    phi->AddPhiOperand(&right, low);
    IR::IREmitter merge_ir{merge};
    const auto second_condition = merge_ir.ConditionRef(merge_ir.INotEqual(IR::U32{IR::Value{&*phi}}, merge_ir.Imm32(0U)));
    IR::Program program;
    program.blocks = {&root, &left, &right, &merge, &sample, &exit};
    program.info.storage_buffers_descriptors.push_back({0, 0x510, 1, false});
    IR::AbstractSyntaxNode first, second;
    first.type = second.type = IR::AbstractSyntaxNode::Type::If;
    first.data.if_node = {first_condition, &left, &right};
    second.data.if_node = {second_condition, &sample, &exit};
    program.syntax_list = {first, second};
    Shader::Optimization::BufferTexture::Tracker tracker{program};
    const auto source = tracker.FindForUse(IR::Value{&*phi}, &sample);
    REQUIRE(source);
    REQUIRE(source->index.Resolve() == index.Resolve());
}


TEST_CASE("Buffer texture descriptors retain field and query identity", "[video_core][buffer_texture][ampere]") {
    const bool different_field = GENERATE(false, true);
    class RootEnvironment final : public EmptyEnvironment {
    public:
        u32 ReadCbufValue(u32 bank, u32 offset) override {
            REQUIRE(bank == 0);
            if (offset == 0x518) return 40960;
            REQUIRE(offset == 0x51c);
            return 0;
        }
    } env;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block root{pool}, sample{pool}, exit{pool};
    root.AddBranch(&sample);
    root.AddBranch(&exit);
    sample.AddBranch(&exit);
    IR::IREmitter ir{root};
    const auto index = ir.GetReg(IR::Reg::R1);
    const auto pointer = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x510U));
    const auto make_handle = [&](u32 field) {
        const auto relative = ir.IAdd(ir.IMul(index, ir.Imm32(160U)), ir.Imm32(field));
        const auto offset = ir.ISub(ir.IAdd(pointer, relative), ir.BitwiseAnd(pointer, ir.Imm32(0xfffffff0U)));
        const auto load = root.PrependNewInst(root.end(), IR::Opcode::LoadStorage64, {ir.Imm32(0U), offset});
        return IR::U32{ir.CompositeExtract(IR::Value{&*load}, 0)};
    };
    const auto first = make_handle(152);
    const auto second = different_field ? make_handle(144) : first;
    IR::TextureInstInfo flags{};
    flags.type.Assign(Shader::TextureType::Color2D);
    (void)ir.ImageSampleExplicitLod(first,
        ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(1.0f)), ir.Imm32(0.0f), {}, flags);
    const auto dimensions = ir.ImageQueryDimension(first, ir.Imm32(0U), ir.Imm1(true));
    ir.SetReg(IR::Reg::R5, IR::U32{ir.CompositeExtract(dimensions, 0)});
    const auto condition = ir.ConditionRef(ir.INotEqual(second, ir.Imm32(0U)));
    IR::IREmitter sample_ir{sample};
    (void)sample_ir.ImageSampleExplicitLod(second,
        sample_ir.CompositeConstruct(sample_ir.Imm32(0.0f), sample_ir.Imm32(1.0f)), sample_ir.Imm32(0.0f), {}, flags);
    IR::Program program;
    program.stage = Shader::Stage::Fragment;
    program.blocks = {&root, &sample, &exit};
    program.post_order_blocks = {&exit, &sample, &root};
    program.info.storage_buffers_descriptors.push_back({0, 0x510, 1, false});
    IR::AbstractSyntaxNode branch;
    branch.type = IR::AbstractSyntaxNode::Type::If;
    branch.data.if_node = {condition, &sample, &exit};
    program.syntax_list = {branch};
    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    host.support_buffer_texture_handles = true;
    REQUIRE_NOTHROW(Shader::Optimization::TexturePass(env, program, host));
    REQUIRE(program.info.texture_descriptors.size() == (different_field ? 3U : 2U));
    u32 primary{}, secondary{}, queries{};
    for (const auto& descriptor : program.info.texture_descriptors) {
        const auto& source = descriptor.buffer_source;
        if (!different_field && !source.width_query)
            REQUIRE(std::ranges::none_of(source.exclusions, [](const auto& guard) { return guard.mask != 0; }));
        if (source.width_query) { ++queries; REQUIRE(source.offset == 152); }
        else if (source.offset == 144) ++secondary;
        else { ++primary; REQUIRE(source.offset == 152); }
    }
    REQUIRE(primary == 1);
    REQUIRE(secondary == (different_field ? 1U : 0U));
    REQUIRE(queries == 1);
}


TEST_CASE("Heterogeneous buffer texture records require a proven matching tag branch", "[video_core][buffer_texture][ampere]") {
    const u32 mode = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block entry{pool}, root{pool}, sample{pool}, exit{pool};
    entry.AddBranch(&root);
    if (mode == 4) entry.AddBranch(&sample);
    root.AddBranch(&sample);
    root.AddBranch(&exit);
    sample.AddBranch(&exit);
    if (mode == 6) sample.AddBranch(&sample);
    IR::IREmitter ir{root};
    const auto index = ir.GetReg(IR::Reg::R1);
    const auto pointer = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x510U));
    const auto load_field = [&](IR::U32 record, u32 field, IR::Opcode opcode) {
        const auto relative = ir.IAdd(ir.IMul(record, ir.Imm32(160U)), ir.Imm32(field));
        const auto offset = ir.ISub(ir.IAdd(pointer, relative), ir.BitwiseAnd(pointer, ir.Imm32(0xfffffff0U)));
        const auto load = root.PrependNewInst(root.end(), opcode, {ir.Imm32(0U), offset});
        return IR::Value{&*load};
    };
    const IR::U32 handle{ir.CompositeExtract(load_field(index, 152U, IR::Opcode::LoadStorage64), 0)};
    IR::U32 tag{load_field(mode == 3 ? ir.GetReg(IR::Reg::R2) : index, 144U, IR::Opcode::LoadStorage32)};
    if (mode == 2) tag = ir.BitwiseAnd(tag, ir.Imm32(7U));
    auto condition = mode == 1 ? ir.INotEqual(tag, ir.Imm32(1U)) : ir.IEqual(tag, ir.Imm32(1U));
    if (mode == 7) condition = ir.GetPred(IR::Pred::P0);
    if (mode >= 8) {
        const auto slot = ir.ShiftLeftLogical(ir.UMin(ir.ISub(tag, ir.Imm32(1U)), ir.Imm32(4U)), ir.Imm32(2U));
        const auto target = ir.GetCbuf(ir.Imm32(1U), ir.IAdd(slot, ir.Imm32(0xf0U)));
        condition = ir.IEqual(target, ir.Imm32(0x100U));
    }
    const auto reference = ir.ConditionRef(condition);
    IR::Program program;
    program.blocks = {&entry, &root, &sample, &exit};
    program.info.storage_buffers_descriptors.push_back({0, 0x510, 1, false});
    IR::AbstractSyntaxNode branch;
    branch.type = IR::AbstractSyntaxNode::Type::If;
    branch.data.if_node = {reference, &sample, &exit};
    program.syntax_list = {branch};
    Shader::Optimization::BufferTexture::Tracker tracker{program};
    const auto source = tracker.Find(handle);
    REQUIRE(source);
    const auto exclusions = tracker.Exclusions(*source, mode == 5 ? &root : &sample,
        [&](u32 bank, u32 offset) -> std::optional<u32> {
            if (mode != 8 || bank != 1 || offset < 0xf0 || offset > 0x100 || (offset & 3)) return {};
            return offset == 0xf0 ? 0x100U : offset == 0xf4 ? 0x200U : 0x300U;
        });
    const auto count = std::ranges::count_if(exclusions, [](const auto& guard) { return guard.mask != 0; });
    if (mode <= 2) {
        REQUIRE(count == 1);
        const auto& guard = exclusions[0];
        REQUIRE(guard.offset == 144);
        REQUIRE(guard.mask == (mode == 2 ? 7U : ~0U));
        REQUIRE(guard.not_equal == (mode != 1));
        REQUIRE(guard.Matches(2U) == (mode != 1));
        REQUIRE(guard.Matches(1U) == (mode == 1));
        if (mode == 2) REQUIRE_FALSE(guard.Matches(0x80000001U));
    } else if (mode == 8) {
        REQUIRE(count > 0);
        const auto excluded = [&](u32 tag_value) {
            return std::ranges::any_of(exclusions, [&](const auto& guard) { return guard.Matches(tag_value); });
        };
        REQUIRE(excluded(2U));
        REQUIRE_FALSE(excluded(1U));
        REQUIRE_FALSE(excluded(0x100U)); // Unproved tags retain strict binding validation.
    } else {
        REQUIRE(count == 0);
    }
}


TEST_CASE("Native global pointers do not infer buffer length from adjacent constants", "[video_core][ampere][storage-ranges]") {
    constexpr u64 mapped = 0x708000;
    // Captured DLSS feature output pointer at CB0+0x2d8 is followed by TIC266.
    // The last workgroup accesses megabytes of features, not a 266-byte descriptor.
    for (u64 following : {u64{0}, u64{1}, u64{266}, u64{1280}, u64{0x8110000000}}) {
        REQUIRE(VideoCommon::StorageBufferSize(mapped, following, true) == mapped);
    }
    REQUIRE(VideoCommon::StorageBufferSize(0, 266, true) == 0);
    REQUIRE(VideoCommon::StorageBufferSize(128, 266, true) == 128);
    REQUIRE(VideoCommon::StorageBufferSize(u64{1} << 32, 266, true) == 8 * 1024 * 1024);
    REQUIRE(VideoCommon::StorageBufferSize(mapped, 266, false) == 266);
    REQUIRE(VideoCommon::StorageBufferSize(mapped, 0, false) == mapped);
    REQUIRE(VideoCommon::StorageBufferSize(mapped, mapped + 1, false) == mapped);
    REQUIRE(VideoCommon::StorageBufferSize(mapped, 0x8110000000, false) == mapped);
}

TEST_CASE("Native raw storage pointers do not inherit NVN descriptor spacing", "[video_core][ampere][buffer_texture]") {
    const u32 pointer_offset = GENERATE(0x168U, 0x178U, 0x198U, 0x208U, 0x258U, 0x2d8U);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto pointer = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(pointer_offset));
    const auto high = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(pointer_offset + 4));
    const auto stride = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x300U));
    const auto relative = ir.IMul(ir.GetReg(IR::Reg::R1), stride);
    const auto address = ir.PackUint2x32(ir.CompositeConstruct(ir.IAdd(pointer, relative), high));
    (void)ir.LoadGlobal32(address);
    IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    EmptyEnvironment env;
    Shader::HostTranslateInfo host;
    Shader::Optimization::GlobalMemoryToStorageBufferPass(env, program, host);
    REQUIRE(program.info.storage_buffers_descriptors.size() == 1);
    REQUIRE(program.info.storage_buffers_descriptors[0].cbuf_index == 0);
    REQUIRE(program.info.storage_buffers_descriptors[0].cbuf_offset == pointer_offset);
}

TEST_CASE("Native pointer tracing rejects a nearer additive stride", "[video_core][ampere][buffer_texture]") {
    class PointerEnvironment final : public EmptyEnvironment {
    public:
        bool IsCbufStorageBufferPointer(u32 bank, u32 offset) override {
            return bank == 0 && offset == 0x168;
        }
    } env;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto pointer = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x168U));
    const auto stride = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x200U));
    const auto high = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x16cU));
    const auto low = ir.IAdd(ir.IAdd(pointer, ir.GetReg(IR::Reg::R1)), stride);
    (void)ir.LoadGlobal32(ir.PackUint2x32(ir.CompositeConstruct(low, high)));
    IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    Shader::HostTranslateInfo host;
    Shader::Optimization::GlobalMemoryToStorageBufferPass(env, program, host);
    REQUIRE(program.info.storage_buffers_descriptors.size() == 1);
    REQUIRE(program.info.storage_buffers_descriptors[0].cbuf_offset == 0x168);
    for (const auto& inst : block.Instructions()) REQUIRE(inst.GetOpcode() != IR::Opcode::LoadGlobal32);
}

TEST_CASE("Native storage pointer classifications survive shader cache serialization", "[video_core][ampere][buffer_texture]") {
    class CacheEnvironment final : public VideoCommon::GenericEnvironment {
    public:
        CacheEnvironment() {
            stage = Shader::Stage::Compute;
            code = {0};
            cached_lowest = cached_highest = 0;
            storage_pointer_types.emplace(0x168, 1);
            storage_pointer_types.emplace(0x200, 0);
            storage_pointer_snapshot_complete = true;
            cbuf_values.emplace(0x168, 0x12340000);
            workgroup_size = {32, 2, 1};
            shared_memory_size = 4096;
        }
        u32 ReadCbufValue(u32, u32) override { return 0; }
        Shader::TextureType ReadTextureType(u32) override { return {}; }
        Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
        bool IsTexturePixelFormatInteger(u32) override { return false; }
        u32 ReadViewportTransformState() override { return 1; }
        std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    } env;
    // No GPU memory manager exists here: detached classification must use only the snapshot.
    REQUIRE_FALSE(env.IsCbufStorageBufferPointer(0, 0x208));
    const auto path = std::filesystem::temp_directory_path() / ("eglantine-pointer-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        env.Serialize(output);
        REQUIRE(output.good());
    }
    VideoCommon::FileEnvironment restored;
    {
        std::ifstream input(path, std::ios::binary);
        restored.Deserialize(input);
        REQUIRE(input.good());
    }
    std::filesystem::remove(path);
    REQUIRE(restored.IsCbufStorageBufferPointer(0, 0x168));
    REQUIRE_FALSE(restored.IsCbufStorageBufferPointer(0, 0x200));
    REQUIRE_FALSE(restored.IsCbufStorageBufferPointer(0, 0x208));
    REQUIRE_THROWS_AS(restored.IsCbufStorageBufferPointer(0, 0x210), Shader::LogicError);
    REQUIRE(restored.ReadCbufValue(0, 0x168) == 0x12340000);
    REQUIRE(restored.WorkgroupSize() == std::array<u32, 3>{32, 2, 1});
    REQUIRE(restored.SharedMemorySize() == 4096);
}

TEST_CASE("Native selected pointers retain distinct banks and repeated accesses",
          "[video_core][ampere][buffer_texture]") {
    const bool different_bank = GENERATE(false, true);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto first = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x168U));
    const auto second = ir.GetCbuf(ir.Imm32(different_bank ? 1U : 0U),
                                   ir.Imm32(different_bank ? 0x168U : 0x178U));
    const auto low = ir.Select(ir.GetPred(IR::Pred::P0), first, second);
    const auto address = ir.PackUint2x32(ir.CompositeConstruct(
        low, ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x16cU))));
    (void)ir.LoadGlobal32(address);
    (void)ir.LoadGlobal32(address);
    IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    EmptyEnvironment env;
    Shader::HostTranslateInfo host;
    host.min_ssbo_alignment = 16;
    Shader::Optimization::GlobalMemoryToStorageBufferPass(env, program, host);
    REQUIRE(program.info.native_global_bounds);
    REQUIRE(program.info.storage_buffers_descriptors.size() == 2);
    const auto& first_desc = program.info.storage_buffers_descriptors[0];
    const auto& second_desc = program.info.storage_buffers_descriptors[1];
    REQUIRE(first_desc.cbuf_index == 0);
    REQUIRE(first_desc.cbuf_offset == 0x168);
    REQUIRE(second_desc.cbuf_index == (different_bank ? 1U : 0U));
    REQUIRE(second_desc.cbuf_offset == (different_bank ? 0x168U : 0x178U));
    u32 global_loads{};
    for (const auto& inst : block.Instructions())
        global_loads += inst.GetOpcode() == IR::Opcode::LoadGlobal32;
    REQUIRE(global_loads == 2);
}

TEST_CASE("Fragment PrimitiveId declares its required SPIRV capability",
          "[video_core][spirv_validation]") {
    const bool primitive_id = GENERATE(false, true);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = Shader::Stage::Fragment;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.info.loads.Set(IR::Attribute::PrimitiveId, primitive_id);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
    bool geometry_capability{}, primitive_decoration{};
    for (size_t offset = 5; offset < code.size();) {
        const u32 count = code[offset] >> 16;
        REQUIRE(count > 0);
        REQUIRE(offset + count <= code.size());
        // OpCapability Geometry; OpDecorate BuiltIn PrimitiveId.
        if ((code[offset] & 0xffffU) == 17 && count == 2 && code[offset + 1] == 2)
            geometry_capability = true;
        if ((code[offset] & 0xffffU) == 71 && count == 4 &&
            code[offset + 2] == 11 && code[offset + 3] == 7)
            primitive_decoration = true;
        offset += count;
    }
    REQUIRE(geometry_capability == primitive_id);
    REQUIRE(primitive_decoration == primitive_id);
    if (const char* folder = std::getenv("EDEN_POINTER_TEST_OUTPUT")) {
        const auto path = std::filesystem::path{folder} /
                          (primitive_id ? "fragment-primitive-id.spv" : "fragment-no-primitive.spv");
        std::ofstream output{path, std::ios::binary};
        output.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
        REQUIRE(output.good());
    }
}

TEST_CASE("Native global memory emits bounded SPIRV candidate selection",
          "[video_core][ampere][buffer_texture]") {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto address = ir.PackUint2x32(ir.CompositeConstruct(
        ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x168U)),
        ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x16cU))));
    const auto value = ir.LoadGlobal32(address);
    block.AppendNewInst(IR::Opcode::WriteStorage32, {ir.Imm32(0U), ir.Imm32(0U), value});
    IR::Program program;
    program.stage = Shader::Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.info.native_global_bounds = true;
    program.info.storage_buffers_descriptors = {{0, 0x168, 1, true}, {0, 0x178, 1, false}};
    EmptyEnvironment env;
    Shader::Optimization::CollectShaderInfoPass(env, program);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = 16;
    const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
    REQUIRE(code.size() > 5);
    REQUIRE(code[0] == 0x07230203);
    u32 selections{}, bounds{};
    for (size_t offset = 5; offset < code.size();) {
        const u32 count = code[offset] >> 16;
        REQUIRE(count > 0);
        REQUIRE(offset + count <= code.size());
        selections += (code[offset] & 0xffffU) == 169; // OpSelect
        bounds += (code[offset] & 0xffffU) == 176; // OpULessThan
        if ((code[offset] & 0xffffU) == 247) { // OpSelectionMerge
            REQUIRE(offset + count < code.size());
            REQUIRE((code[offset + count] & 0xffffU) == 250); // OpBranchConditional
        }
        offset += count;
    }
    REQUIRE(selections >= 4);
    REQUIRE(bounds >= 4);
    if (const char* directory = std::getenv("EDEN_POINTER_TEST_OUTPUT")) {
        std::filesystem::create_directories(directory);
        std::ofstream output{std::filesystem::path{directory} / "native-pointer-selection.spv",
                             std::ios::binary};
        output.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
        REQUIRE(output.good());
    }
}

TEST_CASE("Raw global sub-word and integer atomic paths emit valid operations",
          "[video_core][ampere][global_memory]") {
    const u32 kind = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U,
                             10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U);
    constexpr std::array operations{
        IR::Opcode::LoadGlobalU8, IR::Opcode::LoadGlobalS8,
        IR::Opcode::LoadGlobalU16, IR::Opcode::LoadGlobalS16,
        IR::Opcode::WriteGlobalU8, IR::Opcode::WriteGlobalS8,
        IR::Opcode::WriteGlobalU16, IR::Opcode::WriteGlobalS16,
        IR::Opcode::GlobalAtomicIAdd32, IR::Opcode::GlobalAtomicSMin32,
        IR::Opcode::GlobalAtomicUMin32, IR::Opcode::GlobalAtomicSMax32,
        IR::Opcode::GlobalAtomicUMax32, IR::Opcode::GlobalAtomicAnd32,
        IR::Opcode::GlobalAtomicOr32, IR::Opcode::GlobalAtomicXor32,
        IR::Opcode::GlobalAtomicExchange32, IR::Opcode::GlobalAtomicInc32,
        IR::Opcode::GlobalAtomicDec32};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto lane = ir.LocalInvocationIdX();
    const auto base = ir.PackUint2x32(ir.CompositeConstruct(
        ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x100U)),
        ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x104U))));
    const bool half = kind == 2 || kind == 3 || kind == 6 || kind == 7;
    IR::U32 offset;
    if (kind < 4)
        offset = half ? IR::U32{ir.ShiftLeftLogical(ir.BitwiseAnd(lane, ir.Imm32(1U)), ir.Imm32(1U))}
                      : ir.BitwiseAnd(lane, ir.Imm32(3U));
    else if (kind < 8)
        offset = half ? IR::U32{ir.ShiftLeftLogical(lane, ir.Imm32(1U))} : lane;
    else
        offset = kind == 8 ? ir.Imm32(0U) : IR::U32{ir.ShiftLeftLogical(lane, ir.Imm32(2U))};
    const auto address = ir.IAdd(base, ir.UConvert(64, offset));
    const auto operand = kind < 8 ? IR::U32{ir.IAdd(lane, ir.Imm32(half ? 0x8000U : 0x80U))}
                                  : ir.Imm32(kind == 8 ? 1U : 3U);
    IR::Value result = operand;
    if (kind < 4) {
        const auto load = block.PrependNewInst(block.end(), operations[kind], {address});
        result = IR::Value{&*load};
    } else if (kind < 8) {
        block.AppendNewInst(operations[kind], {address, operand});
    } else {
        const auto atomic = block.PrependNewInst(block.end(), operations[kind], {address, operand});
        result = IR::Value{&*atomic};
    }
    block.AppendNewInst(IR::Opcode::WriteStorage32,
        {ir.Imm32(1U), ir.IAdd(ir.Imm32(256U), ir.ShiftLeftLogical(lane, ir.Imm32(2U))), result});
    IR::Program program;
    program.stage = Shader::Stage::Compute;
    program.workgroup_size = {64, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.info.native_global_bounds = true;
    program.info.storage_buffers_descriptors = {{0, 0x100, 1, kind >= 4}, {0, 0x108, 1, true}};
    EmptyEnvironment env;
    Shader::Optimization::CollectShaderInfoPass(env, program);
    REQUIRE(program.info.uses_global_subword_write == (kind >= 4 && kind < 8));
    REQUIRE(program.info.uses_global_atomics == (kind >= 8));
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = 16;
    const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
    REQUIRE(code.size() > 5);
    REQUIRE(code[0] == 0x07230203);
    u32 atomic_reads{}, exchanges{};
    for (size_t pos = 5; pos < code.size();) {
        const u32 count = code[pos] >> 16;
        REQUIRE(count > 0);
        REQUIRE(pos + count <= code.size());
        atomic_reads += (code[pos] & 0xffffU) == 227; // OpAtomicLoad
        exchanges += (code[pos] & 0xffffU) == 230; // OpAtomicCompareExchange
        if ((code[pos] & 0xffffU) == 247) {
            REQUIRE(pos + count < code.size());
            REQUIRE((code[pos + count] & 0xffffU) == 250);
        }
        pos += count;
    }
    if (kind >= 4) {
        REQUIRE(atomic_reads > 0);
        REQUIRE(exchanges > 0);
    }
    if (const char* directory = std::getenv("EDEN_POINTER_TEST_OUTPUT")) {
        std::filesystem::create_directories(directory);
        std::ofstream output{std::filesystem::path{directory} /
                                 ("raw-memory-" + std::to_string(kind) + ".spv"), std::ios::binary};
        output.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
        REQUIRE(output.good());
    }
}

} // namespace

