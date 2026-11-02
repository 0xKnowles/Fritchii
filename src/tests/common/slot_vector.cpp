// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "common/slot_vector.h"

TEST_CASE("SlotVector occupancy checks reject invalid and erased slots", "[common][slot_vector]") {
    Common::SlotVector<u32> slots;
    const auto& view = slots;
    REQUIRE_FALSE(view.contains({}));
    REQUIRE_FALSE(view.contains(Common::SlotId{0}));
    REQUIRE_FALSE(view.contains(Common::SlotId{Common::SlotId::TAGGED_MASK}));

    std::array<Common::SlotId, 130> ids;
    for (u32 i = 0; i < ids.size(); ++i) {
        ids[i] = slots.insert(i);
        REQUIRE(view.contains(ids[i]));
        REQUIRE(view.contains(Common::SlotId{ids[i].index | Common::SlotId::TAGGED_VALUE}));
        REQUIRE(view[ids[i]] == i);
    }
    REQUIRE_FALSE(view.contains({}));
    REQUIRE_FALSE(view.contains(Common::SlotId{Common::SlotId::TAGGED_MASK}));
    for (u32 i = 0; i < ids.size(); i += 2) {
        slots.erase(ids[i]);
        REQUIRE_FALSE(view.contains(ids[i]));
        REQUIRE_FALSE(view.contains(Common::SlotId{ids[i].index | Common::SlotId::TAGGED_VALUE}));
        REQUIRE(view.contains(ids[i + 1]));
    }
    const auto reused = slots.insert(999);
    REQUIRE(reused == ids[128]);
    REQUIRE(view.contains(reused));
    REQUIRE(view[reused] == 999);
}
