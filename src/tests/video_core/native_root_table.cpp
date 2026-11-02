// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "video_core/native_root_table.h"

TEST_CASE("Native root-table uploads retain independent banks and sequential words",
          "[video_core][ampere][root_table]") {
    Tegra::NativeRootTables tables;
    for (u32 bank = 24; bank < 32; ++bank) {
        REQUIRE(Tegra::NativeRootTables::IsBank(bank));
        tables.Select(bank - 24);
        for (u32 offset = 0; offset < 256; offset += 4) {
            REQUIRE(tables.Read(bank, offset) == 0);
            // Repeated identical writes must still advance the upload cursor.
            REQUIRE(tables.Load(bank));
        }
        REQUIRE_FALSE(tables.Load(0xbad));
    }
    for (u32 bank = 24; bank < 32; ++bank) {
        for (u32 offset = 0; offset < 256; offset += 4) {
            REQUIRE(tables.Read(bank, offset) == bank);
            REQUIRE(tables.Data(bank)[offset / 4] == bank);
        }
    }
    const auto snapshot = tables;
    tables.Select(7 | (252 << 8));
    REQUIRE(tables.Load(0x12345678));
    REQUIRE(tables.Read(31, 252) == 0x12345678);
    REQUIRE(snapshot.Read(31, 252) == 31);
    REQUIRE_FALSE(tables.Load(0));
    REQUIRE(tables.Read(31, 0) == 31);
    tables.Select(7 | (1 << 8));
    REQUIRE_FALSE(tables.Load(0));
    for (u32 bank : {0U, 17U, 23U, 32U, ~0U}) {
        REQUIRE_FALSE(Tegra::NativeRootTables::IsBank(bank));
        REQUIRE(tables.Read(bank, 0) == 0);
    }
    for (u32 offset : {1U, 255U, 256U, ~0U}) {
        REQUIRE(tables.Read(24, offset) == 0);
    }
}

TEST_CASE("Native constant lowering preserves root bank identity",
          "[video_core][ampere][root_table]") {
    using Shader::Ampere::Instruction;
    // Captured FMUL.FTZ R17, R13, c[0x18][4], followed by EXIT.
    for (u32 bank = 0; bank < 32; ++bank) {
        const std::array<Instruction, 2> code{{
            {{0x0d117a20, (bank << 22) | 0x100, 0x00410000, 0x000fe200}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        const bool valid = bank < 18 || bank >= 24;
        REQUIRE(static_cast<bool>(lowered) == valid);
        if (valid) {
            REQUIRE(((lowered.instructions.front() >> 34) & 31) == bank);
            REQUIRE(((lowered.instructions.front() >> 20) & 0x3fff) == 1);
        }
    }
}

TEST_CASE("Native uniform and indexed loads retain root banks",
          "[video_core][ampere][root_table]") {
    using Shader::Ampere::Instruction;
    for (u32 bank : {17U, 18U, 23U, 24U, 31U}) {
        for (bool indexed : {false, true}) {
            const std::array<Instruction, 2> code{{
                {{indexed ? 0x00067b82U : 0x00047ab9U, (bank << 22) | 0x200,
                  indexed ? 0x00000800U : 0x00000800U, 0}},
                {{0x0000794d, 0, 0x03800000, 0}},
            }};
            const auto lowered = Shader::Ampere::LowerToMaxwell(code);
            const bool valid = bank < 18 || bank >= 24;
            REQUIRE(static_cast<bool>(lowered) == valid);
            if (valid) {
                REQUIRE(((lowered.instructions.front() >> (indexed ? 36 : 34)) & 31) == bank);
            }
        }
    }
}
