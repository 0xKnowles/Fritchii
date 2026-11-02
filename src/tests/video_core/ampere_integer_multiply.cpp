// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <iomanip>
#include <random>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction Exit{{0x794d, 0, 0x03800000, 0}};

struct IntegerMachine {
    std::array<u32, 768> registers{};
    std::array<bool, 7> predicates{};
    std::array<std::array<u32, 64>, 18> constants{};
    bool carry{};

    u32 Read(u32 index) const {
        return index == 255 ? 0 : registers[index];
    }

    void Run(std::span<const u64> code) {
        for (const u64 word : code) {
            const auto bits = [&](u32 offset, u32 count) {
                return static_cast<u32>((word >> offset) & ((u64{1} << count) - 1));
            };
            const u32 guard = bits(16, 3);
            if (!((guard == 7 || predicates[guard]) != bool(bits(19, 1))))
                continue;
            const u32 dest = bits(0, 8);
            if ((word & 0xfff8000000000000ULL) == 0x5c98000000000000ULL &&
                bits(39, 4) == 0 && bits(44, 4) == 10) {
                const u32 source = bits(20, 8) + bits(10, 2) * 256;
                const u32 target = dest + bits(8, 2) * 256;
                if (target != 255) registers[target] = Read(source);
                continue;
            }
            const u32 a = Read(bits(8, 8)), b = Read(bits(20, 8)), c = Read(bits(39, 8));
            const u64 opcode = word & 0xfff8000000000000ULL;
            u32 result{};
            if ((word & 0xffc0000000000000ULL) == 0x5b00000000000000ULL || (word >> 56) == 0x36) {
                const u32 x = (a >> (bits(53, 1) * 16)) & 0xffff;
                const u32 y =
                    (word >> 56) == 0x36 ? bits(20, 16) : (b >> (bits(35, 1) * 16)) & 0xffff;
                const u32 mode = bits(50, 3);
                REQUIRE(mode <= 2);
                result = x * y + (mode == 0 ? c : mode == 1 ? c & 0xffff : c >> 16);
            } else if (opcode == 0x5c10000000000000ULL) {
                const u32 x = bits(49, 1) ? 0U - a : a;
                const u32 y = bits(48, 1) ? 0U - b : b;
                const u64 sum = u64{x} + y + (bits(43, 1) && carry);
                result = static_cast<u32>(sum);
                if (bits(47, 1))
                    carry = (sum >> 32) != 0;
            } else if (opcode == 0x5c18000000000000ULL) {
                result = (a << bits(39, 5)) + b;
            } else if (opcode == 0x3828000000000000ULL) {
                const u32 shift = bits(20, 19);
                REQUIRE(shift < 32);
                result =
                    bits(48, 1) ? static_cast<u32>(std::bit_cast<s32>(a) >> shift) : a >> shift;
            } else if (opcode == 0x5c40000000000000ULL) {
                REQUIRE(bits(41, 2) <= 1);
                result = bits(41, 2) == 0 ? a & b : a | b;
            } else if ((word >> 52) == 0x5b5 || (word >> 52) == 0x5b6) {
                REQUIRE(bits(48, 1) == 0); // unsigned
                const u32 relation = bits(49, 3);
                REQUIRE((relation == 1 || relation == 2 || relation == 5 || relation == 6));
                const bool comparison = relation == 1 ? a < b : relation == 2 ? a == b : relation == 6 ? a >= b : a != b;
                if ((word >> 52) == 0x5b6) {
                    REQUIRE(bits(0, 3) == 7);
                    if (bits(3, 3) != 7) predicates[bits(3, 3)] = comparison;
                    continue;
                }
                result = comparison ? ~0U : 0U;
            } else if (opcode == 0x3800000000000000ULL) {
                const u32 offset = bits(20, 8), width = bits(28, 8);
                REQUIRE((width == 8 || width == 16));
                REQUIRE(offset + width <= 32);
                const u32 mask = (1U << width) - 1;
                result = (a >> offset) & mask;
                if (bits(48, 1) && (result & (1U << (width - 1))))
                    result |= ~mask;
            } else if ((word >> 56) == 0x04) {
                REQUIRE(bits(53, 2) == 2);
                result = a ^ bits(20, 32);
            } else if (opcode == 0x5ca0000000000000ULL) {
                const u32 condition = bits(39, 3);
                result = ((condition == 7 || predicates[condition]) != bool(bits(42, 1))) ? a : b;
            } else if (opcode == 0x5c98000000000000ULL) {
                result = b;
            } else if ((word >> 56) == 0x01) {
                result = bits(20, 32);
            } else if (opcode == 0x4c98000000000000ULL) {
                const u32 bank = bits(34, 5), offset = bits(20, 14);
                REQUIRE(bank < constants.size());
                REQUIRE(offset < constants[bank].size());
                result = constants[bank][offset];
            } else if ((word >> 56) == 0xef) {
                REQUIRE(bits(48, 3) == 5);
                const u32 bank = bits(36, 5), offset = bits(20, 16) + a;
                REQUIRE(bank < constants.size());
                REQUIRE(offset % 4 == 0);
                REQUIRE(offset / 4 + 1 < constants[bank].size());
                result = constants[bank][offset / 4];
                registers[dest + 1] = constants[bank][offset / 4 + 1];
            } else if ((word >> 56) == 0xe3) {
                break;
            } else {
                FAIL("Unexpected emitted instruction " << std::hex << word);
            }
            if (dest != 255)
                registers[dest] = result;
        }
    }
};

u64 Product(u32 a, u32 b, bool is_signed) {
    if (!is_signed)
        return u64{a} * b;
    return static_cast<u64>(s64{std::bit_cast<s32>(a)} * std::bit_cast<s32>(b));
}

IntegerMachine SeedMachine() {
    IntegerMachine machine;
    for (u32 i = 0; i < machine.registers.size(); ++i)
        machine.registers[i] = 0xdead0000U + i;
    machine.predicates = {false, true, false, true, false, true, true};
    return machine;
}

void CheckUnchanged(const IntegerMachine& before, const IntegerMachine& after, u32 dest,
                    u32 count) {
    REQUIRE(after.predicates == before.predicates);
    for (u32 i = 0; i < 246; ++i) {
        if (i < dest || i >= dest + count)
            REQUIRE(after.registers[i] == before.registers[i]);
    }
}
} // namespace

TEST_CASE("Ampere high register shadows preserve borrowed registers and false guards",
          "[video_core][ampere][fire_emblem]") {
    for (u32 guard : {0U, 7U, 8U}) {
        for (bool active_predicate : {false, true}) {
            // Set high registers through the default lowering, then use a high aliased source
            // and destination in a 64-bit multiply-add. No physical high guest GPR is borrowed.
            const std::array input{
                Instruction{{0x00f47802, 0xffffffff, 0xf00, 0}},
                Instruction{{0x00f57802, 0xffffffff, 0xf00, 0}},
                Instruction{{0x00fa7802, 2, 0xf00, 0}},
                Instruction{{0x00fc7802, 3, 0xf00, 0}},
                Instruction{{0xfa000227U | (244U << 16) | (guard << 12), 252, 0x078e00f4, 0}},
                Exit};
            const auto lowered = LowerToMaxwell(input);
            INFO(lowered.error);
            REQUIRE(lowered);
            auto machine = SeedMachine();
            machine.predicates[0] = active_predicate;
            const auto before = machine;
            machine.Run(lowered.instructions);
            const bool enabled = guard == 7 || (active_predicate != bool(guard & 8));
            REQUIRE(machine.registers[256 + 244] == (enabled ? 0U : ~0U));
            REQUIRE(machine.registers[256 + 245] == ~0U);
            REQUIRE(machine.registers[256 + 250] == 2);
            REQUIRE(machine.registers[256 + 252] == 3);
            for (u32 r = 0; r < 244; ++r) REQUIRE(machine.registers[r] == before.registers[r]);
            REQUIRE(machine.predicates == before.predicates);
        }
    }
}

TEST_CASE("Ampere high multiply respects operand forms signs aliases and predicates",
          "[video_core][ampere][integer_multiply]") {
    std::mt19937 random{0x949a921};
    for (const u32 opcode : {0x227U, 0x827U, 0xa27U}) {
        for (const bool is_signed : {false, true}) {
            for (const u32 dest : {0U, 2U, 4U, 6U}) {
                for (const u32 guard : {0U, 5U, 6U, 7U, 8U, 13U, 14U}) {
                    for (u32 test = 0; test < 24; ++test) {
                        auto machine = SeedMachine();
                        machine.registers[2] =
                            test < 4 ? std::array{0U, 1U, ~0U, 0x80000000U}[test] : random();
                        machine.registers[4] =
                            test < 4 ? std::array{~0U, 2U, ~0U, 0x80000000U}[test] : random();
                        machine.registers[6] = random();
                        machine.constants[2][4] = machine.registers[4];
                        const Instruction instruction{
                            {opcode | (guard << 12) | (dest << 16) | (2U << 24),
                             opcode == 0x227   ? 4U
                             : opcode == 0x827 ? machine.registers[4]
                                               : (2U << 22) | (16U << 6),
                             0x078e0006U | (u32{is_signed} << 9), 0}};
                        const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
                        INFO(lowered.error);
                        REQUIRE(lowered);
                        const auto before = machine;
                        const bool active =
                            ((guard & 7) == 7 || before.predicates[guard & 7]) != bool(guard & 8);
                        const u64 product = Product(before.Read(2), before.Read(4), is_signed);
                        const u32 result = opcode == 0x227
                            ? static_cast<u32>((product + before.Read(6) + (u64{before.Read(7)} << 32)) >> 32)
                            : static_cast<u32>(product >> 32) + before.Read(6);
                        const u32 expected = active ? result : before.Read(dest);
                        machine.Run(lowered.instructions);
                        REQUIRE(machine.Read(dest) == expected);
                        CheckUnchanged(before, machine, dest, 1);
                    }
                }
            }
        }
    }
}

TEST_CASE("Ampere wide multiply preserves the full sum for all operand forms and aliases",
          "[video_core][ampere][integer_multiply]") {
    std::mt19937 random{0x825625};
    for (const u32 opcode : {0x225U, 0x625U, 0x825U}) {
        for (const bool is_signed : {false, true}) {
            for (const u32 dest : {0U, 2U, 4U, 6U}) {
                for (const u32 guard : {0U, 5U, 6U, 7U, 8U, 13U, 14U}) {
                    for (u32 test = 0; test < 24; ++test) {
                        auto machine = SeedMachine();
                        machine.registers[2] =
                            test < 4 ? std::array{0U, 1U, ~0U, 0x80000000U}[test] : random();
                        machine.registers[4] =
                            test < 4 ? std::array{~0U, 2U, ~0U, 0x80000000U}[test] : random();
                        machine.registers[6] = test < 4 ? ~0U : random();
                        machine.registers[7] = random();
                        machine.constants[2][4] = machine.registers[6];
                        machine.constants[2][5] = machine.registers[7];
                        const Instruction instruction{
                            {opcode | (guard << 12) | (dest << 16) | (2U << 24),
                             opcode == 0x225   ? 4U
                             : opcode == 0x825 ? machine.registers[4]
                                               : (2U << 22) | (16U << 6),
                             0x078e0000U | (opcode == 0x625 ? 4U : 6U) | (u32{is_signed} << 9), 0}};
                        const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
                        INFO(lowered.error);
                        REQUIRE(lowered);
                        const auto before = machine;
                        const bool active =
                            ((guard & 7) == 7 || before.predicates[guard & 7]) != bool(guard & 8);
                        const u64 expected =
                            active ? Product(before.Read(2), before.Read(4), is_signed) +
                                         before.Read(6) + (u64{before.Read(7)} << 32)
                                   : before.Read(dest) | (u64{before.Read(dest + 1)} << 32);
                        machine.Run(lowered.instructions);
                        REQUIRE((machine.Read(dest) | (u64{machine.Read(dest + 1)} << 32)) ==
                                expected);
                        CheckUnchanged(before, machine, dest, 2);
                    }
                }
            }
        }
    }
}

TEST_CASE("Ampere wide multiply writes carry after consuming all aliased inputs",
          "[video_core][ampere][fire_emblem]") {
    std::mt19937 random{0x225ca77};
    for (u32 opcode : {0x225U, 0x227U, 0x625U, 0x825U})
        for (u32 dest : {0U, 2U, 4U, 6U})
            for (u32 guard : {0U, 5U, 7U, 8U, 13U})
                for (u32 output = 0; output < 7; ++output)
                    for (u32 trial = 0; trial < 24; ++trial) {
                        auto machine = SeedMachine();
                        machine.registers[2] = trial < 2 ? ~0U : random();
                        machine.registers[4] = trial < 2 ? ~0U : random();
                        machine.registers[6] = trial == 0 ? ~0U : random();
                        machine.registers[7] = trial == 0 ? ~0U : trial == 1 ? 0 : random();
                        machine.constants[2][4] = machine.registers[6];
                        machine.constants[2][5] = machine.registers[7];
                        const Instruction instruction{{opcode | (guard << 12) | (dest << 16) | (2U << 24),
                            opcode == 0x225 || opcode == 0x227 ? 4U : opcode == 0x825 ? machine.Read(4) : (2U << 22) | (16U << 6),
                            0x07800000U | (output << 17) | (opcode == 0x625 ? 4U : 6U), 0}};
                        const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
                        REQUIRE(lowered);
                        const auto before = machine;
                        const u64 product = u64{before.Read(2)} * before.Read(4);
                        const u64 addend = before.Read(6) | (u64{before.Read(7)} << 32);
                        const bool active = ((guard & 7) == 7 || before.predicates[guard & 7]) != bool(guard & 8);
                        machine.Run(lowered.instructions);
                        if (opcode == 0x227) {
                            REQUIRE(machine.Read(dest) == (active ? u32((product + addend) >> 32) : before.Read(dest)));
                        } else {
                            REQUIRE((machine.Read(dest) | (u64{machine.Read(dest + 1)} << 32)) ==
                                (active ? product + addend : before.Read(dest) | (u64{before.Read(dest + 1)} << 32)));
                        }
                        auto expected = before.predicates;
                        if (active) expected[output] = product + addend < product;
                        REQUIRE(machine.predicates == expected);
                        machine.predicates = before.predicates;
                        CheckUnchanged(before, machine, dest, opcode == 0x227 ? 1 : 2);
                    }
}

TEST_CASE("Ampere IDP4 accumulates signed and unsigned bytes without scratch or alias corruption",
          "[video_core][ampere][integer_multiply]") {
    for (const bool signed_a : {false, true}) {
        for (const bool signed_b : {false, true}) {
            for (const u32 dest : {0U, 2U, 4U, 6U}) {
                for (const u32 guard : {0U, 5U, 6U, 7U, 8U, 13U, 14U}) {
                    for (const u32 input : {0x01010101U, 0x80ff7f01U, ~0U, 0U}) {
                        auto machine = SeedMachine();
                        machine.registers[2] = input;
                        machine.registers[4] = 0xff7f8001U;
                        machine.registers[6] = 7;
                        const auto before = machine;
                        const Instruction instruction{
                            {0x226U | (guard << 12) | (dest << 16) | (2U << 24), 4,
                             6U | (u32{signed_a} << 9) | (u32{signed_b} << 10), 0}};
                        const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
                        INFO(lowered.error);
                        REQUIRE(lowered);
                        u32 expected = before.Read(6);
                        for (u32 i = 0; i < 4; ++i) {
                            const auto byte = [i](u32 value, bool is_signed) -> s32 {
                                const u32 part = (value >> (8 * i)) & 255;
                                return is_signed && part >= 128 ? static_cast<s32>(part) - 256
                                                                : part;
                            };
                            expected += static_cast<u32>(byte(before.Read(2), signed_a) *
                                                         byte(before.Read(4), signed_b));
                        }
                        const bool active =
                            ((guard & 7) == 7 || before.predicates[guard & 7]) != bool(guard & 8);
                        machine.Run(lowered.instructions);
                        REQUIRE(machine.Read(dest) == (active ? expected : before.Read(dest)));
                        CheckUnchanged(before, machine, dest, 1);
                    }
                }
            }
        }
    }
}

TEST_CASE("Ampere runtime traps are not silently erased",
          "[video_core][ampere][integer_multiply]") {
    const auto trap = LowerToMaxwell(std::array{Instruction{{0x795c, 4, 0x00300000, 0}}, Exit});
    REQUIRE_FALSE(trap);
    REQUIRE(trap.error == "native runtime breakpoint/trap");
    for (const u32 guard : {0U, 7U, 8U}) {
        for (const u32 mode : {0U, 1U, 2U, 0xffffffffU}) {
            const auto lowered =
                LowerToMaxwell(std::array{Instruction{{0x95cU | (guard << 12), 0, mode, 0}}, Exit});
            REQUIRE_FALSE(lowered);
            REQUIRE(lowered.error == "native runtime breakpoint/trap");
        }
    }
    REQUIRE(LowerToMaxwell(std::array{Instruction{{0xf95c, 0, 0, 0}}, Exit}));
}

TEST_CASE("Ampere IDP2 low accumulates halfwords times low bytes",
          "[video_core][ampere][integer_multiply][fire_emblem]") {
    std::mt19937 random{0x2a161608};
    for (const bool signed_a : {false, true}) {
        for (const bool signed_b : {false, true}) {
            for (const u32 dest : {0U, 2U, 4U, 6U, 255U}) {
                for (const u32 guard : {0U, 5U, 7U, 8U, 13U, 15U}) {
                    for (u32 trial = 0; trial < 16; ++trial) {
                        auto machine = SeedMachine();
                        machine.registers[2] = trial < 4
                            ? std::array{0U, 0x8000ffffU, 0x7fff0001U, ~0U}[trial] : random();
                        machine.registers[4] = trial < 4
                            ? std::array{~0U, 0xff7f80ffU, 0x12340000U, 0xffff7f80U}[trial] : random();
                        machine.registers[6] = random();
                        const Instruction instruction{
                            {0x226U | (guard << 12) | (dest << 16) | (2U << 24), 4,
                             0x1006U | (u32{signed_a} << 9) | (u32{signed_b} << 10), 0}};
                        const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
                        INFO(lowered.error);
                        REQUIRE(lowered);
                        const auto before = machine;
                        u32 expected = before.Read(6);
                        for (u32 i = 0; i < 2; ++i) {
                            const u32 a = (before.Read(2) >> (16 * i)) & 0xffff;
                            const u32 b = (before.Read(4) >> (8 * i)) & 0xff;
                            const s32 x = signed_a && a >= 0x8000 ? s32(a) - 0x10000 : s32(a);
                            const s32 y = signed_b && b >= 0x80 ? s32(b) - 0x100 : s32(b);
                            expected += static_cast<u32>(x * y);
                        }
                        const bool active =
                            ((guard & 7) == 7 || before.predicates[guard & 7]) != bool(guard & 8);
                        machine.Run(lowered.instructions);
                        REQUIRE(machine.Read(dest) ==
                                (dest == 255 ? 0U : active ? expected : before.Read(dest)));
                        CheckUnchanged(before, machine, dest, 1);
                    }
                }
            }
        }
    }
}

TEST_CASE("Ampere extended add complements sources and consumes both carry predicates",
          "[video_core][ampere][integer_multiply][fire_emblem]") {
    std::mt19937 random{0xadd32026};
    for (const u32 opcode : {0x210U, 0x810U, 0xa10U, 0xc10U}) {
        for (const u32 dest : {0U, 2U, 4U, 6U, 255U}) {
            for (const u32 guard : {0U, 5U, 7U, 8U, 13U, 15U}) {
                for (u32 trial = 0; trial < 128; ++trial) {
                    auto machine = SeedMachine();
                    machine.registers[2] = trial < 4 ? std::array{0U, ~0U, 1U, 0x80000000U}[trial] : random();
                    machine.registers[4] = trial < 4 ? ~0U : random();
                    machine.registers[6] = random();
                    machine.registers[220] = machine.Read(4); // UR4 for 0xc10
                    machine.constants[2][4] = machine.Read(4);
                    for (u32 p = 0; p < 7; ++p) machine.predicates[p] = (trial >> p) & 1;
                    const u32 first = trial % 16, second = (trial / 8 + 3) % 16;
                    const bool not_a = trial & 1, not_b = opcode != 0x810 && (trial & 2);
                    const bool not_c = trial & 4;
                    const u32 operand_b = opcode == 0x810 ? machine.Read(4)
                        : (opcode == 0xa10 ? (2U << 22) | (16U << 6) : 4U) |
                          (u32{not_b} << 31);
                    const Instruction instruction{
                        {opcode | (guard << 12) | (dest << 16) | (2U << 24), operand_b,
                         6U | (1U << 10) | (u32{not_a} << 8) | (u32{not_c} << 11) |
                         (first << 23) | (second << 13) | (7U << 17) | (7U << 20) |
                         (opcode == 0xc10 ? 0x08000000U : 0U), 0}};
                    const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
                    INFO(lowered.error);
                    REQUIRE(lowered);
                    const auto before = machine;
                    const auto pred = [&](u32 p) {
                        return ((p & 7) == 7 || before.predicates[p & 7]) != bool(p & 8);
                    };
                    const u32 expected = (not_a ? ~before.Read(2) : before.Read(2)) +
                                         (not_b ? ~before.Read(4) : before.Read(4)) +
                                         (not_c ? ~before.Read(6) : before.Read(6)) +
                                         u32{pred(first)} + u32{pred(second)};
                    machine.Run(lowered.instructions);
                    REQUIRE(machine.Read(dest) ==
                            (dest == 255 ? 0U : pred(guard) ? expected : before.Read(dest)));
                    CheckUnchanged(before, machine, dest, 1);
                }
            }
        }
    }
}

TEST_CASE("Ampere three-input sums preserve full carry count and predicate aliases",
          "[video_core][ampere][fire_emblem]") {
    std::mt19937 random{0xadd3ca77};
    for (u32 opcode : {0x210U, 0x810U, 0xa10U, 0xc10U})
        for (u32 guard : {0U, 5U, 7U, 8U, 13U})
            for (bool extended : {false, true})
                for (u32 modifiers = 0; modifiers < 8; ++modifiers)
                    for (u32 trial = 0; trial < 32; ++trial) {
                        auto machine = SeedMachine();
                        machine.registers[2] = trial < 2 ? ~0U : random();
                        machine.registers[4] = trial < 2 ? ~0U : random();
                        machine.registers[6] = trial < 2 ? ~0U : random();
                        machine.registers[220] = machine.Read(4);
                        machine.constants[2][4] = machine.Read(4);
                        for (u32 p = 0; p < 7; ++p) machine.predicates[p] = (trial >> p) & 1;
                        const bool na = modifiers & 1, nb = opcode != 0x810 && (modifiers & 2), nc = modifiers & 4;
                        const u32 ca = extended ? trial % 16 : 15, cb = extended ? (trial + 5) % 16 : 15;
                        const u32 operand = opcode == 0x810 ? machine.Read(4) :
                            (opcode == 0xa10 ? (2U << 22) | (16U << 6) : 4U) | (u32(nb) << 31);
                        const Instruction instruction{{opcode | (guard << 12) | (2U << 16) | (2U << 24), operand,
                            6U | (u32(na) << 8) | (u32(extended) << 10) | (u32(nc) << 11) |
                            (cb << 13) | (0U << 17) | (5U << 20) | (ca << 23) |
                            (opcode == 0xc10 ? 0x08000000U : 0U), 0}};
                        const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
                        INFO(lowered.error);
                        REQUIRE(lowered);
                        const auto before = machine;
                        const auto pred = [&](u32 p) { return ((p & 7) == 7 || before.predicates[p & 7]) != bool(p & 8); };
                        const u64 sum = u64{na ? ~before.Read(2) : before.Read(2)} +
                            (nb ? ~before.Read(4) : before.Read(4)) + (nc ? ~before.Read(6) : before.Read(6)) +
                            (extended ? u32(pred(ca)) + u32(pred(cb)) : u32(na) + u32(nb) + u32(nc));
                        machine.Run(lowered.instructions);
                        REQUIRE(machine.Read(2) == (pred(guard) ? u32(sum) : before.Read(2)));
                        auto predicates = before.predicates;
                        if (pred(guard)) { predicates[0] = (sum >> 32) >= 1; predicates[5] = (sum >> 32) >= 2; }
                        REQUIRE(machine.predicates == predicates);
                        machine.predicates = before.predicates;
                        CheckUnchanged(before, machine, 2, 1);
                    }
}

TEST_CASE("Ampere extended add rejects reserved fields",
          "[video_core][ampere][integer_multiply][fire_emblem]") {
    const Instruction instruction{{0x02007210, 4, 0x017e6406, 0}};
    REQUIRE(LowerToMaxwell(std::array{instruction, Exit}));
    for (const u32 bit : {9U, 12U, 27U, 31U}) {
        auto invalid = instruction;
        invalid.words[2] ^= 1U << bit;
        REQUIRE_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
    }
    auto invalid = instruction;
    invalid.words[1] |= 0x100;
    REQUIRE_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
}

TEST_CASE("Ampere integer multiplication rejects unsupported modifiers",
          "[video_core][ampere][integer_multiply]") {
    for (const u32 opcode : {0x225U, 0x625U, 0x825U, 0x227U, 0x827U, 0xa27U}) {
        const Instruction valid{
            {opcode | 0x02007000U, opcode == 0x625 || opcode == 0xa27 ? 0U : 4U, 0x078e0006, 0}};
        REQUIRE(LowerToMaxwell(std::array{valid, Exit}));
        for (const u32 bit : {8U, 10U, 13U, 15U, 26U}) {
            auto invalid = valid;
            invalid.words[2] ^= 1U << bit;
            REQUIRE_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
        }
    }
    for (const u32 bit : {8U, 11U, 13U, 15U}) {
        const Instruction invalid{{0x02007226, 4, 6U | (1U << bit), 0}};
        REQUIRE_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
    }
}
