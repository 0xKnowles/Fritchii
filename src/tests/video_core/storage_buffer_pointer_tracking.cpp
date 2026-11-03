// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;

struct KernelProgram {
    Shader::ObjectPool<IR::Inst> pool{256};
    IR::Block entry{pool};
    IR::Block loop{pool};
    IR::Program program;

    KernelProgram() {
        program.stage = Shader::Stage::Compute;
        program.blocks = {&entry};
        program.post_order_blocks = {&entry};
    }

    std::vector<u32> TrackedOffsets() {
        Shader::HostTranslateInfo host{};
        host.min_ssbo_alignment = 16;
        Shader::Optimization::GlobalMemoryToStorageBufferPass(program, host);
        std::vector<u32> offsets;
        for (const auto& descriptor : program.info.storage_buffers_descriptors) {
            offsets.push_back(descriptor.cbuf_offset);
        }
        return offsets;
    }
};

IR::U32 Word(IR::IREmitter& ir, u32 offset) {
    return ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset));
}

IR::U64 Address(IR::IREmitter& ir, const IR::U32& low, const IR::U32& high) {
    return ir.PackUint2x32(ir.CompositeConstruct(low, high));
}
} // Anonymous namespace

TEST_CASE("Kernel parameter pointers win over 16-byte aligned strides",
          "[shader][storage_buffer]") {
    KernelProgram fixture;
    IR::IREmitter ir{fixture.entry};
    const IR::U32 lane{ir.LocalInvocationIdX()};
    const IR::U32 row{ir.IAdd(ir.ShiftLeftLogical(lane, ir.Imm32(3)), Word(ir, 0x1c0))};
    const IR::U32 low{ir.IAdd(ir.IAdd(row, Word(ir, 0x178)), ir.ShiftLeftLogical(lane, ir.Imm32(4)))};
    ir.WriteGlobal32(Address(ir, low, Word(ir, 0x17c)), lane);
    CHECK(fixture.TrackedOffsets() == std::vector<u32>{0x178});
}

TEST_CASE("Filter tap offset tables are not taken for the weight pointer",
          "[shader][storage_buffer]") {
    KernelProgram fixture;
    IR::IREmitter ir{fixture.entry};
    const IR::U32 lane{ir.LocalInvocationIdX()};
    const IR::Value weights{ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x168), 64, false)};
    const IR::U32 taps_low{ir.IAdd(ir.IAdd(Word(ir, 0x210), Word(ir, 0x218)), Word(ir, 0x220))};
    const IR::U32 taps_high{ir.IAdd(ir.IAdd(Word(ir, 0x214), Word(ir, 0x21c)), Word(ir, 0x224))};
    const IR::U32 low{ir.IAdd(ir.IAdd(IR::U32{ir.CompositeExtract(weights, 0)},
                                     ir.ShiftLeftLogical(lane, ir.Imm32(1))),
                              taps_low)};
    const IR::U32 high{ir.IAdd(IR::U32{ir.CompositeExtract(weights, 1)}, taps_high)};
    static_cast<void>(ir.LoadGlobal128(Address(ir, low, high)));
    CHECK(fixture.TrackedOffsets() == std::vector<u32>{0x168});
}

TEST_CASE("Loop increments are not taken for the iterated pointer", "[shader][storage_buffer]") {
    KernelProgram fixture;
    fixture.entry.AddBranch(&fixture.loop);
    fixture.loop.AddBranch(&fixture.loop);
    fixture.program.blocks = {&fixture.entry, &fixture.loop};
    fixture.program.post_order_blocks = {&fixture.loop, &fixture.entry};
    IR::IREmitter entry{fixture.entry};
    const IR::Value activations{entry.GetCbuf(entry.Imm32(0), entry.Imm32(0x160), 64, false)};
    const IR::Value step{entry.GetCbuf(entry.Imm32(0), entry.Imm32(0x200), 64, false)};
    const IR::U32 first{entry.IAdd(IR::U32{entry.CompositeExtract(activations, 0)},
                                   IR::U32{entry.CompositeExtract(step, 0)})};
    const IR::U32 first_high{entry.IAdd(IR::U32{entry.CompositeExtract(activations, 1)},
                                        IR::U32{entry.CompositeExtract(step, 1)})};
    IR::IREmitter body{fixture.loop};
    fixture.loop.AppendNewInst(IR::Opcode::Phi, {});
    IR::Inst* const low{&fixture.loop.back()};
    low->SetFlags(IR::Type::U32);
    fixture.loop.AppendNewInst(IR::Opcode::Phi, {});
    IR::Inst* const high{&fixture.loop.back()};
    high->SetFlags(IR::Type::U32);
    static_cast<void>(
        body.LoadGlobal128(Address(body, IR::U32{IR::Value{low}}, IR::U32{IR::Value{high}})));
    const IR::U32 next{body.IAdd(IR::U32{IR::Value{low}}, Word(body, 0x200))};
    const IR::U32 next_high{body.IAdd(IR::U32{IR::Value{high}}, Word(body, 0x204))};
    low->AddPhiOperand(&fixture.entry, first);
    low->AddPhiOperand(&fixture.loop, next);
    high->AddPhiOperand(&fixture.entry, first_high);
    high->AddPhiOperand(&fixture.loop, next_high);
    CHECK(fixture.TrackedOffsets() == std::vector<u32>{0x160});
}

TEST_CASE("NVN storage buffer records keep their tracking", "[shader][storage_buffer]") {
    KernelProgram fixture;
    IR::IREmitter ir{fixture.entry};
    const IR::U32 lane{ir.LocalInvocationIdX()};
    const IR::U32 low{ir.IAdd(Word(ir, 0x310), ir.ShiftLeftLogical(lane, ir.Imm32(2)))};
    static_cast<void>(ir.LoadGlobal32(Address(ir, low, Word(ir, 0x314))));
    CHECK(fixture.TrackedOffsets() == std::vector<u32>{0x310});
}
