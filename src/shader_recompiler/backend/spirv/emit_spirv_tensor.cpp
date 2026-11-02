// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Backend::SPIRV {
namespace {

// HMMA.16816.F16 fragment layout (PTX m16n8k16), lane t = 4g + i:
//   A (16x16 row-major): a0={A[g][2i],A[g][2i+1]} a1={A[g+8][2i],..}
//                        a2={A[g][8+2i],..}       a3={A[g+8][8+2i],..}
//   B (16x8  row-major): b0={B[2i][g],B[2i+1][g]}  b1={B[8+2i][g],B[8+2i+1][g]}
//   C/D (16x8 row-major): d0={D[g][2i],D[g][2i+1]} d1={D[g+8][2i],D[g+8][2i+1]}
// The fragments are staged through a Workgroup f16 scratch and consumed with the
// NV cooperative-matrix instructions.
constexpr u32 A_BASE = 0;    // 16x16 f16
constexpr u32 B_BASE = 256;  // 16x8 f16
constexpr u32 C_BASE = 384;  // 16x8 f16
constexpr u32 D_BASE = 512;  // 16x8 f16

} // Anonymous namespace

void DefineSoftwareMatrixFunctions(EmitContext& ctx, const IR::Program& program) {
    std::array<bool, 2> needed{};
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::TensorMulAdd || inst.Flags<u32>() == 0) continue;
            const u32 depth = inst.Flags<u32>();
            if (depth != 8 && depth != 16) throw LogicError("Invalid software HMMA depth");
            needed[depth == 16] = true;
        }
    }
    for (u32 variant = 0; variant < 2; ++variant) {
        if (!needed[variant]) continue;
        ctx.AddCapability(spv::Capability::GroupNonUniform);
        ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
        if (!ctx.profile.SupportsSubgroupStage(program.stage) ||
            ctx.profile.has_broken_spirv_subgroup_shuffle)
            throw NotImplementedException("Software HMMA requires subgroup shuffle support");
        const Id u32ty = ctx.U32[1], f32ty = ctx.F32[1];
        const auto imm = [&](u32 v) { return ctx.Const(v); };
        const auto add = [&](Id a, Id b) { return ctx.OpIAdd(u32ty, a, b); };
        const auto and_ = [&](Id a, u32 b) { return ctx.OpBitwiseAnd(u32ty, a, imm(b)); };
        const auto unpack = [&](Id v) { return ctx.OpUnpackHalf2x16(ctx.F32[2], v); };
        const auto round_half = [&](Id value) {
            // Do not express the intermediate rounding as unpack(pack(value)): drivers may
            // eliminate that round trip. Round normal significands explicitly, ties to even;
            // half subnormals lie on the exactly representable 2^-24 grid.
            const Id bits = ctx.OpBitcast(u32ty, value);
            const Id magnitude = and_(bits, 0x7fffffffU);
            const Id tie = and_(ctx.OpShiftRightLogical(u32ty, magnitude, imm(13)), 1);
            const Id rounded = and_(add(add(magnitude, imm(0xfff)), tie), ~0x1fffU);
            const Id normal = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, rounded, imm(0x47800000)), imm(0x7f800000), rounded);
            const Id scaled = ctx.OpFMul(f32ty, ctx.OpBitcast(f32ty, magnitude), ctx.Const(16777216.0f));
            const Id subnormal = ctx.OpBitcast(u32ty,
                ctx.OpFMul(f32ty, ctx.OpRoundEven(f32ty, scaled), ctx.Const(0x1p-24f)));
            const Id finite = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x38800000)), normal, subnormal);
            const Id result = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x7f800000)), magnitude, finite);
            return ctx.OpBitcast(f32ty, ctx.OpBitwiseOr(u32ty, result, and_(bits, 0x80000000)));
        };
        const Id type = ctx.TypeFunction(ctx.U32[2], u32ty, u32ty, u32ty, u32ty,
                                         u32ty, u32ty, u32ty, u32ty);
        const Id fn = ctx.OpFunction(ctx.U32[2], spv::FunctionControlMask::DontInline, type);
        ctx.software_matrix_functions[variant] = fn;
        std::array<Id, 8> inputs;
        for (auto& input : inputs) input = ctx.OpFunctionParameter(u32ty);
        ctx.AddLabel();
        const Id counter = ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, u32ty),
                                           spv::StorageClass::Function);
        std::array<Id, 4> accumulators;
        for (auto& acc : accumulators)
            acc = ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, f32ty),
                                 spv::StorageClass::Function);
        for (u32 row = 0; row < 2; ++row) {
            const Id c = unpack(inputs[6 + row]);
            for (u32 col = 0; col < 2; ++col)
                ctx.OpStore(accumulators[row * 2 + col], ctx.OpCompositeExtract(f32ty, c, col));
        }
        ctx.OpStore(counter, imm(0));
        const Id lane = ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id);
        const Id warp_base = and_(lane, ~31U);
        const Id row_base = and_(lane, 28);
        const Id col_base = ctx.OpShiftLeftLogical(u32ty, and_(lane, 3), imm(3));
        const Id header = ctx.OpLabel(), body = ctx.OpLabel(), next = ctx.OpLabel(), end = ctx.OpLabel();
        ctx.OpBranch(header);
        ctx.AddLabel(header);
        const Id pair = ctx.OpLoad(u32ty, counter);
        const Id more = ctx.OpULessThan(ctx.U1, pair, imm(variant ? 8 : 4));
        ctx.OpLoopMerge(end, next, spv::LoopControlMask::DontUnroll);
        ctx.OpBranchConditional(more, body, end);
        ctx.AddLabel(body);
        const Id upper = ctx.OpUGreaterThanEqual(ctx.U1, pair, imm(4));
        const Id component = and_(pair, 3);
        const Id a_lane = add(warp_base, add(row_base, component));
        const Id b_lane = add(warp_base, add(col_base, component));
        const Id b = ctx.OpSelect(u32ty, upper, inputs[5], inputs[4]);
        std::array<Id, 2> bs;
        for (u32 col = 0; col < 2; ++col)
            bs[col] = unpack(ctx.OpGroupNonUniformShuffle(u32ty, imm(3), b,
                                                          add(b_lane, imm(col * 4))));
        const Id round_now = ctx.OpIEqual(ctx.U1, component, imm(3));
        for (u32 row = 0; row < 2; ++row) {
            const Id a = ctx.OpSelect(u32ty, upper, inputs[row + 2], inputs[row]);
            const Id as = unpack(ctx.OpGroupNonUniformShuffle(u32ty, imm(3), a, a_lane));
            for (u32 col = 0; col < 2; ++col) {
                Id value = ctx.OpLoad(f32ty, accumulators[row * 2 + col]);
                for (u32 half = 0; half < 2; ++half) {
                    value = ctx.OpFma(f32ty, ctx.OpCompositeExtract(f32ty, as, half),
                                      ctx.OpCompositeExtract(f32ty, bs[col], half), value);
                    ctx.Decorate(value, spv::Decoration::NoContraction);
                }
                ctx.OpStore(accumulators[row * 2 + col], value);
            }
        }
        // HMMA.F16 rounds after each K=8 group, including the midpoint of K=16.
        // OpSelect evaluates both operands: keep the expensive half conversion in
        // a branch so the other three pairs do no rounding work. The counter is
        // identical in every participating lane and all shuffles precede this branch.
        const Id round_block = ctx.OpLabel(), round_merge = ctx.OpLabel();
        ctx.OpSelectionMerge(round_merge, spv::SelectionControlMask::DontFlatten);
        ctx.OpBranchConditional(round_now, round_block, round_merge);
        ctx.AddLabel(round_block);
        for (const Id accumulator : accumulators) {
            ctx.OpStore(accumulator, round_half(ctx.OpLoad(f32ty, accumulator)));
        }
        ctx.OpBranch(round_merge);
        ctx.AddLabel(round_merge);
        ctx.OpBranch(next);
        ctx.AddLabel(next);
        ctx.OpStore(counter, add(pair, imm(1)));
        ctx.OpBranch(header);
        ctx.AddLabel(end);
        std::array<Id, 2> packed;
        for (u32 row = 0; row < 2; ++row)
            packed[row] = ctx.OpPackHalf2x16(u32ty, ctx.OpCompositeConstruct(ctx.F32[2],
                ctx.OpLoad(f32ty, accumulators[row * 2]), ctx.OpLoad(f32ty, accumulators[row * 2 + 1])));
        ctx.OpReturnValue(ctx.OpCompositeConstruct(ctx.U32[2], packed[0], packed[1]));
        ctx.OpFunctionEnd();
    }
}

Id EmitTensorMulAdd(EmitContext& ctx, IR::Inst* inst, Id a0, Id a1, Id a2, Id a3, Id b0, Id b1,
                    Id c0, Id c1) {
    if (const u32 depth = inst->Flags<u32>(); depth != 0) {
        return ctx.OpFunctionCall(ctx.U32[2], ctx.software_matrix_functions[depth == 16],
                                  a0, a1, a2, a3, b0, b1, c0, c1);
    }
    const Id u32ty{ctx.U32[1]};
    const Id f16{ctx.F16[1]};
    const Id f16x2{ctx.F16[2]};

    const auto add = [&](Id x, Id y) { return ctx.OpIAdd(u32ty, x, y); };
    const auto shl = [&](Id x, Id y) { return ctx.OpShiftLeftLogical(u32ty, x, y); };
    const auto shr = [&](Id x, Id y) { return ctx.OpShiftRightLogical(u32ty, x, y); };
    const auto and_ = [&](Id x, Id y) { return ctx.OpBitwiseAnd(u32ty, x, y); };
    const auto imm = [&](u32 v) { return ctx.Const(v); };

    const auto ptr = [&](Id index) {
        return ctx.uses_explicit_workgroup_layout
                   ? ctx.OpAccessChain(ctx.coop_scratch_f16, ctx.coop_scratch, imm(0U), index)
                   : ctx.OpAccessChain(ctx.coop_scratch_f16, ctx.coop_scratch, index);
    };
    const auto lo_f16 = [&](Id value) {
        return ctx.OpCompositeExtract(f16, ctx.OpBitcast(f16x2, value), 0U);
    };
    const auto hi_f16 = [&](Id value) {
        return ctx.OpCompositeExtract(f16, ctx.OpBitcast(f16x2, value), 1U);
    };
    const auto store = [&](Id index, Id value_f16) { ctx.OpStore(ptr(index), value_f16); };
    const auto load = [&](Id index) { return ctx.OpLoad(f16, ptr(index)); };
    const auto combine = [&](Id lo, Id hi) {
        return ctx.OpBitcast(u32ty, ctx.OpCompositeConstruct(f16x2, lo, hi));
    };
    const auto barrier = [&]() {
        ctx.OpControlBarrier(imm(3U), imm(3U), imm(0x108U));
    };

    const Id lane{ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id)};
    const Id sub_base{ctx.OpIMul(u32ty, ctx.OpLoad(u32ty, ctx.subgroup_id), imm(640U))};
    const Id abase{add(imm(A_BASE), sub_base)};
    const Id bbase{add(imm(B_BASE), sub_base)};
    const Id cbase{add(imm(C_BASE), sub_base)};
    const Id dbase{add(imm(D_BASE), sub_base)};
    const Id g{shr(lane, imm(2U))};   // lane / 4
    const Id i{and_(lane, imm(3U))};  // lane % 4
    const Id i2{shl(i, imm(1U))};     // 2i
    const Id i4_2{shl(add(i, imm(4U)), imm(1U))};  // 8+2i
    const Id g16{shl(g, imm(4U))};                 // g*16
    const Id g8_16{shl(add(g, imm(8U)), imm(4U))}; // (g+8)*16
    const Id g3{shl(g, imm(3U))};                  // g*8
    const Id g8_3{shl(add(g, imm(8U)), imm(3U))};  // (g+8)*8

    barrier();
    // Stage A (row-major, 16x16).
    {
        const Id idx0{add(abase, add(g16, i2))};
        const Id idx1{add(abase, add(g8_16, i2))};
        const Id idx2{add(abase, add(g16, i4_2))};
        const Id idx3{add(abase, add(g8_16, i4_2))};
        store(idx0, lo_f16(a0));
        store(add(idx0, imm(1U)), hi_f16(a0));
        store(idx1, lo_f16(a1));
        store(add(idx1, imm(1U)), hi_f16(a1));
        store(idx2, lo_f16(a2));
        store(add(idx2, imm(1U)), hi_f16(a2));
        store(idx3, lo_f16(a3));
        store(add(idx3, imm(1U)), hi_f16(a3));
    }
    // Stage B (row-major, 16x8): b0={B[2i][g],B[2i+1][g]}, b1={B[8+2i][g],B[8+2i+1][g]}.
    {
        const Id idx0{add(bbase, add(shl(i, imm(4U)), g))};  // (2i)*8+g
        const Id idx1{add(bbase, add(shl(add(i, imm(4U)), imm(4U)), g))}; // (8+2i)*8+g
        store(idx0, lo_f16(b0));
        store(add(idx0, imm(8U)), hi_f16(b0));
        store(idx1, lo_f16(b1));
        store(add(idx1, imm(8U)), hi_f16(b1));
    }
    // Stage C (row-major, 16x8).
    {
        const Id idx0{add(cbase, add(g3, i2))};
        const Id idx1{add(cbase, add(g8_3, i2))};
        store(idx0, lo_f16(c0));
        store(add(idx0, imm(1U)), hi_f16(c0));
        store(idx1, lo_f16(c1));
        store(add(idx1, imm(1U)), hi_f16(c1));
    }
    barrier();

    if (!Sirit::ValidId(ctx.cooperative_matrix_type_a)) {
        ctx.cooperative_matrix_type_a =
            ctx.TypeCooperativeMatrixNV(f16, imm(3U), imm(16U), imm(16U)); // Subgroup scope
    }
    if (!Sirit::ValidId(ctx.cooperative_matrix_type_b)) {
        ctx.cooperative_matrix_type_b =
            ctx.TypeCooperativeMatrixNV(f16, imm(3U), imm(16U), imm(8U));
    }
    ctx.cooperative_matrix_type_c = ctx.cooperative_matrix_type_b;

    const Id mat_a =
        ctx.OpCoopMatrixLoadNV(ctx.cooperative_matrix_type_a, ptr(abase), imm(16U),
                               ctx.false_value);
    const Id mat_b =
        ctx.OpCoopMatrixLoadNV(ctx.cooperative_matrix_type_b, ptr(bbase), imm(8U),
                               ctx.false_value);
    const Id mat_c =
        ctx.OpCoopMatrixLoadNV(ctx.cooperative_matrix_type_c, ptr(cbase), imm(8U),
                               ctx.false_value);
    const Id mat_d{ctx.OpCoopMatrixMulAddNV(ctx.cooperative_matrix_type_c, mat_a, mat_b, mat_c)};
    ctx.OpCoopMatrixStoreNV(ptr(dbase), mat_d, imm(8U), ctx.false_value);
    barrier();

    const Id d0_idx{add(dbase, add(g3, i2))};
    const Id d1_idx{add(dbase, add(g8_3, i2))};
    const Id d0{combine(load(d0_idx), load(add(d0_idx, imm(1U))))};
    const Id d1{combine(load(d1_idx), load(add(d1_idx, imm(1U))))};
    return ctx.OpCompositeConstruct(ctx.U32[2], d0, d1);
}

} // namespace Shader::Backend::SPIRV
