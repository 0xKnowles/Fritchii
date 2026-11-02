// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "video_core/buffer_cache/storage_binding_sync.h"

TEST_CASE("Duplicate storage bindings preserve read-write upgrades", "[video_core][storage_binding_sync]") {
    VideoCommon::StorageBindingSync cache;
    auto actions = cache.Touch(1, 0x4000, 8192, false);
    REQUIRE(actions.synchronize);
    REQUIRE_FALSE(actions.mark_written);
    actions = cache.Touch(1, 0x4000, 8192, false);
    REQUIRE_FALSE(actions.synchronize);
    REQUIRE_FALSE(actions.mark_written);
    actions = cache.Touch(1, 0x4000, 8192, true);
    REQUIRE_FALSE(actions.synchronize);
    REQUIRE(actions.mark_written);
    actions = cache.Touch(1, 0x4000, 8192, true);
    REQUIRE_FALSE(actions.synchronize);
    REQUIRE_FALSE(actions.mark_written);
    actions = cache.Touch(1, 0x4000, 8192, false);
    REQUIRE_FALSE(actions.mark_written);
    cache.Clear();
    actions = cache.Touch(1, 0x4000, 8192, true);
    REQUIRE(actions.synchronize);
    REQUIRE(actions.mark_written);
}

TEST_CASE("Storage binding cache misses on changed extent address buffer and collisions", "[video_core][storage_binding_sync]") {
    VideoCommon::StorageBindingSync cache;
    REQUIRE(cache.Touch(1, 0x4000, 8192, true).synchronize);
    REQUIRE(cache.Touch(2, 0x4000, 8192, true).synchronize);
    REQUIRE(cache.Touch(1, 0x4000, 4096, true).synchronize);
    REQUIRE(cache.Touch(1, 0x8000, 8192, true).synchronize);
    for (u32 i = 0; i < 100; ++i) {
        REQUIRE(cache.Touch(1 + 16 * i, 0x10000, 8192, true).synchronize);
    }
    const auto replaced = cache.Touch(1, 0x10000, 8192, true);
    REQUIRE(replaced.synchronize);
    REQUIRE(replaced.mark_written);
}
