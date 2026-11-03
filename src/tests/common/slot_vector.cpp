// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/slot_vector.h"

TEST_CASE("SlotVector begins at the first live entry after empty bitmap words", "[common]") {
    Common::SlotVector<u32> slots;
    std::vector<Common::SlotId> ids;
    for (u32 i = 0; i < 513; ++i) {
        ids.push_back(slots.insert(i));
    }
    for (u32 i = 0; i < 512; ++i) {
        slots.erase(ids[i]);
    }

    auto it = slots.begin();
    REQUIRE(it != slots.end());
    REQUIRE((*it).first == ids.back());
    REQUIRE(*(*it).second == 512);
    REQUIRE(++it == slots.end());

    slots.erase(ids.back());
    REQUIRE(slots.begin() == slots.end());
}
