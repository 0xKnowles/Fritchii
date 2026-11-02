// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

TEST_CASE("Ampere reciprocal accepts the captured uniform source form", "[video_core][ampere]") {
    for (const u32 source : {6U, 7U, 8U, 22U, 27U, 63U}) {
        const std::array<Instruction, 2> code{{
            {{0x00187d08, source, 0x08001000, 0x000e6600}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        const u32 mapped = source < 22 ? 216 + source : source < 28 ? 218 + source : 255;
        REQUIRE(lowered.instructions.front() ==
                (0x5080000000070000ULL | 24ULL | (u64{mapped} << 8) | (4ULL << 20)));
    }
    const std::array<Instruction, 2> code{{
        {{0x00181d08, 8, 0x08001000, 0x000e6600}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto predicated = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(predicated));
    REQUIRE(((predicated.instructions.front() >> 16) & 15) == 1);
    for (const u32 source : {28U, 62U, 64U, 0x80000008U}) {
        auto invalid = code;
        invalid[0].words[1] = source;
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    for (const u32 mode : {0x08000000U, 0x08001400U, 0x08001200U, 0x08011000U}) {
        auto invalid = code;
        invalid[0].words[2] = mode;
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    auto alias = code;
    alias[0].words[0] = 0x00dc1d08;
    const auto shadowed = LowerToMaxwell(alias);
    REQUIRE(shadowed);
    REQUIRE(shadowed.instructions[9] == Shader::Ampere::NativeRegisterCopy(1, 220, 0, 212));
}

TEST_CASE("Ampere clip-state operations remain explicitly unsupported", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x0000780d, 0xc4321111, 0x03fe0000, 0x000fc800}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE_FALSE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.empty());
}

TEST_CASE("Ampere floating comparisons accept uniform sources without losing FTZ",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0xff007c0b, 4, 0x0bf1c000, 0x001fe400}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.front() ==
            (0x5bb0000000070000ULL | 7ULL | (255ULL << 8) | (220ULL << 20) | (7ULL << 39) |
             (1ULL << 47) | (12ULL << 48)));
    auto invalid = code;
    invalid[0].words[1] = 28;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    auto alias = code;
    alias[0].words[0] = 0xdc007c0b;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(alias)));
}

TEST_CASE("Ampere local loads preserve register addresses and predicates", "[video_core][ampere]") {
    for (const u32 mode : {0x00100000U, 0x00300000U}) {
        for (const u32 type : {4U, 5U, 6U}) {
            const std::array<Instruction, 2> code{{
                {{0x12102983, 0x2000, mode | (type << 9), 0x000e2200}},
                {{0x0000794d, 0, 0x03800000, 0}},
            }};
            const auto lowered = LowerToMaxwell(code);
            REQUIRE(static_cast<bool>(lowered));
            REQUIRE(lowered.instructions.front() == (0xef40000000020000ULL | 16ULL | (18ULL << 8) |
                                                     (0x20ULL << 20) | (u64{type} << 48)));
        }
    }
}

TEST_CASE("Ampere local spills preserve vector widths and zero-register stores",
          "[video_core][ampere]") {
    for (const u32 type : {4U, 5U, 6U}) {
        for (const bool zero : {false, true}) {
            const u32 source = zero ? 255U : 4U;
            const std::array<Instruction, 2> code{{
                {{0xff007387, source | (0x20U << 8), 0x00100000U | (type << 9), 0x000fe200}},
                {{0x0000794d, 0, 0x03800000, 0}},
            }};
            const auto lowered = LowerToMaxwell(code);
            REQUIRE(static_cast<bool>(lowered));
            const u32 components = 1U << (type - 4);
            REQUIRE(lowered.instructions.size() == (zero ? components + 2 : 2));
            const u64 store = 0xef50000000070000ULL | (zero ? 248ULL : 4ULL) | (255ULL << 8) |
                              (0x20ULL << 20) | (u64{type} << 48);
            REQUIRE(lowered.instructions[lowered.instructions.size() - 2] == store);
            if (zero) {
                for (u32 word = 0; word < components; ++word)
                    REQUIRE(lowered.instructions[word] ==
                            (0x0100000000070000ULL | (248ULL + word) | (15ULL << 12)));
            }
        }
    }
}

TEST_CASE("Ampere local vector spills reject misalignment and shadow high registers",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> misaligned{{
        {{0xff007387, 0x404, 0x00100c00, 0x000fe200}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(misaligned)));
    const std::array<Instruction, 2> scratch{{
        {{0xff007387, 244, 0x00100c00, 0x000fe200}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto shadowed = LowerToMaxwell(scratch);
    REQUIRE(shadowed);
    REQUIRE(shadowed.instructions.size() == 18);
    REQUIRE((shadowed.instructions[8] & 255) == 240);
    for (u32 component = 0; component < 4; ++component) {
        REQUIRE(shadowed.instructions[component * 2 + 1] ==
                Shader::Ampere::NativeRegisterCopy(0, 240 + component, 1, 244 + component));
    }
}

TEST_CASE("Ampere warp reductions retain their operation and active-lane mask",
          "[video_core][ampere]") {
    for (const u32 mode : {0xc000U, 0x14000U, 0x4000U}) {
        const std::array<Instruction, 2> code{{
            {{0x030673c4, 0, mode, 0x000ea200}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.instructions.size() == 195);
        REQUIRE(lowered.instructions.front() ==
                (0x50d8000000070000ULL | 250ULL | (7ULL << 39) | (7ULL << 45) | (1ULL << 48)));
        const u64 operation = mode == 0xc000 ? 0x5c10000000070000ULL
                              : mode == 0x14000
                                  ? 0x5c20000000070000ULL | (15ULL << 39)
                                  : 0x5c40000000070000ULL | (1ULL << 41) | (7ULL << 48);
        const u64 merge = operation | 222ULL | (222ULL << 8) | (251ULL << 20);
        REQUIRE(std::count(lowered.instructions.begin(), lowered.instructions.end(), merge) == 32);
    }
}

TEST_CASE("Ampere warp reductions reject unsupported modes and uniform destinations",
          "[video_core][ampere]") {
    for (const u32 mode : {0U, 0x8000U, 0x14200U}) {
        const std::array<Instruction, 2> code{{
            {{0x030673c4, 0, mode, 0x000ea200}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    }
    for (const u32 destination : {28U, 63U}) {
        const std::array<Instruction, 2> code{{
            {{0x030073c4U | (destination << 16), 0, 0x4000, 0x000ea200}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    }
}

TEST_CASE("Ampere warp match supports 32-bit and 64-bit sources", "[video_core][ampere]") {
    for (const u32 width : {1U, 2U}) {
        const u32 source = width == 1 ? 5U : 4U;
        const std::array<Instruction, 2> code{{
            {{0x00ff73a1U | (source << 24), 0, width == 2 ? 0x200U : 0U, 0x000e2400}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.instructions.size() == 8ULL + 2ULL * width);
        const u64 shuffle = 0xef10000000070000ULL | 251ULL | (u64{source} << 8) | (250ULL << 20) |
                            (1ULL << 29) | (31ULL << 34) | (7ULL << 48);
        REQUIRE(std::find(lowered.instructions.begin(), lowered.instructions.end(), shuffle) !=
                lowered.instructions.end());
    }
}

TEST_CASE("Ampere warp match still rejects unsupported modes", "[video_core][ampere]") {
    for (const u32 mode : {0x200U, 0x8000U}) {
        const std::array<Instruction, 2> code{{
            {{0x05ff73a1, 0, mode, 0x000e2400}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    }
}

TEST_CASE("Ampere uniform W-scale reads use the Maxwell special registers",
          "[video_core][ampere]") {
    for (const u32 special : {30U, 31U}) {
        const std::array<Instruction, 2> code{{
            {{0x000479c3, 0, special << 8, 0x000e7000}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        const u64 read = 0xf0c8000000070000ULL | 220ULL | (u64{special} << 20);
        REQUIRE(std::find(lowered.instructions.begin(), lowered.instructions.end(), read) !=
                lowered.instructions.end());
    }
}
TEST_CASE("Ampere shared atomic OR preserves its operation and predicate", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x05ff838c, 3, 0x03000000, 0x0011e200}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.front() == (0xec00000000000000ULL | (8ULL << 16) | 255ULL |
                                             (5ULL << 8) | (3ULL << 20) | (6ULL << 52)));
}

TEST_CASE("Ampere segmented constant loads preserve their addressing mode",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x00067b82, 0x200, 0x8a00, 0x000e7000}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.front() == (0xef90000000000000ULL | (7ULL << 16) | 6ULL |
                                             (8ULL << 20) | (2ULL << 44) | (5ULL << 48)));
}
TEST_CASE("Ampere packed half comparisons preserve all register-form controls",
          "[video_core][ampere][fire_emblem]") {
    for (u32 cmp = 0; cmp < 16; ++cmp) {
        for (u32 combine = 0; combine < 3; ++combine) {
            for (u32 swizzle = 0; swizzle < 4; ++swizzle) {
                for (u32 flags = 0; flags < 64; ++flags) {
                    const u32 a_neg = flags & 1, a_abs = (flags >> 1) & 1;
                    const u32 b_neg = (flags >> 2) & 1, b_abs = (flags >> 3) & 1;
                    const u32 bf = (flags >> 4) & 1, ftz = (flags >> 5) & 1;
                    const std::array<Instruction, 2> code{{
                        {{0x02017233, 3U | (swizzle << 28) | (b_abs << 30) | (b_neg << 31),
                          (combine << 5) | (bf << 7) | (a_neg << 8) | (a_abs << 9) |
                          (swizzle << 10) | (cmp << 12) | (ftz << 16) | (10U << 23), 0}},
                        {{0x794d, 0, 0x03800000, 0}},
                    }};
                    const auto lowered = LowerToMaxwell(code);
                    REQUIRE(lowered);
                    const u64 expected = 0x5d18000000070001ULL | (2ULL << 8) | (3ULL << 20) |
                        (u64{swizzle} << 28) | (u64{b_abs} << 30) | (u64{b_neg} << 31) |
                        (u64{cmp} << 35) | (10ULL << 39) | (u64{a_neg} << 43) |
                        (u64{a_abs} << 44) | (u64{combine} << 45) | (u64{swizzle} << 47) |
                        (u64{bf} << 49) | (u64{ftz} << 50);
                    REQUIRE(lowered.instructions.front() == expected);
                }
            }
        }
    }
    std::array<Instruction, 2> invalid{{{{0x02017233, 3, 3U << 5, 0}}, {{0x794d, 0, 0x03800000, 0}}}};
    REQUIRE_FALSE(LowerToMaxwell(invalid));
}

TEST_CASE("Ampere texture query keeps mip-count selection packed in one register",
          "[video_core][ampere][fire_emblem]") {
    for (const u32 mask : {1U, 2U, 3U, 8U}) {
        for (const u32 pred : {0U, 7U, 9U}) {
            const std::array<Instruction, 2> code{{
                {{0x02040b6fU | (pred << 12), 0x00800800, 255U | (mask << 8), 0}},
                {{0x794d, 0, 0x03800000, 0}},
            }};
            const auto lowered = LowerToMaxwell(code);
            REQUIRE(lowered);
            REQUIRE(lowered.required_texture_buffer == 2);
            REQUIRE(lowered.instructions.front() ==
                    (0xdf48000000000004ULL | (2ULL << 8) | (u64{pred} << 16) |
                     (1ULL << 22) | (u64{mask} << 31) | (8ULL << 36)));
        }
    }
}

TEST_CASE("Ampere gradient samples pack every channel mask and split destination",
          "[video_core][ampere][fire_emblem]") {
    for (u32 mask = 1; mask < 16; ++mask) {
        for (const u32 dimension : {1U, 5U}) {
            for (const u32 upper : {6U, 12U}) {
                const u32 count = std::popcount(mask), high = count > 2 ? upper : 255U;
                const std::array<Instruction, 2> code{{
                    {{0x14047b6c, 24U | (8U << 8) | (2U << 22) | (dimension << 29),
                      0x001e0000U | (mask << 8) | high, 0}},
                    {{0x794d, 0, 0x03800000, 0}},
                }};
                const auto lowered = LowerToMaxwell(code);
                REQUIRE(lowered);
                const bool split = count > 2 && upper != 6;
                REQUIRE(lowered.instructions.size() == 2 + (split ? count : 0));
                REQUIRE(lowered.required_texture_buffer == 2);
                REQUIRE(lowered.instructions.front() ==
                        (0xde00000000070000ULL | (split ? 248ULL : 4ULL) | (20ULL << 8) |
                         (24ULL << 20) | (u64{dimension == 5 ? 3U : 2U} << 28) |
                         (u64{mask} << 31) | (8ULL << 36) | (7ULL << 51)));
                if (split) {
                    for (u32 i = 0; i < count; ++i) {
                        const u32 target = i < 2 ? 4 + i : upper + i - 2;
                        REQUIRE(lowered.instructions[i + 1] ==
                                (0x5c98000000070000ULL | target | (15ULL << 39) |
                                 (u64{248 + i} << 20)));
                    }
                }
            }
        }
    }
}
} // namespace
