// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <random>
#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/descriptor_table.h"
#include "video_core/textures/texture.h"

TEST_CASE("Descriptor tables allocate geometrically and retain capacity", "[descriptor_table]") {
    VideoCommon::DescriptorTable<Tegra::Texture::TICEntry> table;
    constexpr std::array<u32, 8> limits{0, 1, 63, 64, 255, 1024, 0xfffff, 127};
    size_t high_water{};
    for (const u32 limit : limits) {
        table.Refresh(0x1000, limit);
        high_water = (std::max)(high_water, std::bit_ceil(size_t{limit} + 1));
        REQUIRE(table.descriptors.size() == high_water);
        REQUIRE(table.bound_ids.size() == high_water);
        REQUIRE(table.read_descriptors.size() ==
                Common::DivCeil(std::bit_ceil(size_t{limit} + 1), size_t{64}));
        REQUIRE(std::ranges::all_of(table.read_descriptors, [](u64 bits) { return bits == 0; }));
        std::ranges::fill(table.read_descriptors, ~u64{0});
    }
    VideoCommon::DescriptorTable<Tegra::Texture::TICEntry> small;
    small.Refresh(0x1000, 255);
    REQUIRE(small.descriptors.size() * sizeof(Tegra::Texture::TICEntry) == 8192);
}

TEST_CASE("Descriptor invalidations coalesce without reusing stale bindings", "[descriptor_table]") {
    VideoCommon::DescriptorTable<u32> table;
    table.Refresh(0x1000, 127);
    REQUIRE(table.UpdateDescriptor(0, 42));
    table.bound_ids[0] = 9;
    REQUIRE(table.UpdateDescriptor(64, 77));
    REQUIRE_FALSE(table.UpdateDescriptor(0, 42));
    const auto marks = table.read_descriptors;
    for (int i = 0; i < 1000; ++i) {
        table.Invalidate();
    }
    REQUIRE(table.read_descriptors == marks); // no clearing work during repeated evictions
    REQUIRE(table.UpdateDescriptor(64, 77));
    REQUIRE_FALSE(table.invalidated);
    REQUIRE(table.read_descriptors[0] == 0);
    REQUIRE(table.UpdateDescriptor(0, 42)); // old bound ID must be resolved again
    table.bound_ids[0] = 10;
    REQUIRE_FALSE(table.UpdateDescriptor(0, 42));
    REQUIRE(table.bound_ids[0] == 10);
    REQUIRE(table.UpdateDescriptor(0, 43)); // actual guest changes are never hidden
}

TEST_CASE("Descriptor table remap shrink and regrowth invalidate old IDs", "[descriptor_table]") {
    VideoCommon::DescriptorTable<u32> table;
    table.Refresh(0x1000, 127);
    REQUIRE(table.UpdateDescriptor(100, 7));
    table.bound_ids[100] = 99;
    REQUIRE_FALSE(table.Synchronize(0x1000, 127));
    REQUIRE_FALSE(table.UpdateDescriptor(100, 7));
    table.Invalidate();
    REQUIRE(table.Synchronize(0x2000, 15));
    REQUIRE_FALSE(table.invalidated);
    REQUIRE(table.Synchronize(0x2000, 127));
    REQUIRE(table.UpdateDescriptor(100, 7));
    VideoCommon::DescriptorTable<u32> other_channel;
    other_channel.Refresh(0x2000, 127);
    REQUIRE(other_channel.UpdateDescriptor(100, 7));
    REQUIRE(other_channel.bound_ids[100] == 0);
}

TEST_CASE("Lazy descriptor bookkeeping matches eager invalidation", "[descriptor_table]") {
    std::mt19937 random{0x4127};
    VideoCommon::DescriptorTable<u32> table;
    std::array<u32, 256> values{};
    std::array<bool, 256> read{};
    table.Refresh(0x1000, 255);
    for (int step = 0; step < 50000; ++step) {
        const u32 op = random() % 16;
        if (op == 0) {
            table.Invalidate();
            read.fill(false);
        } else if (op == 1) {
            table.Refresh(0x1000 + (random() % 4) * 4096, 255);
            read.fill(false);
        } else {
            const u32 index = random() % 256;
            const u32 value = random() % 4;
            const bool changed = !read[index] || values[index] != value;
            REQUIRE(table.UpdateDescriptor(index, value) == changed);
            read[index] = true;
            values[index] = value;
        }
    }
}
