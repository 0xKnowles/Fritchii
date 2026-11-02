// SPDX-License-Identifier: GPL-3.0-or-later

#include <unordered_map>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/virtual_range_cache.h"

namespace {
constexpr u64 Page = 4096;
constexpr GPUVAddr Base = 0x10000;

struct MappingMemory {
    size_t id = 1;
    size_t walks = 0;
    std::unordered_map<GPUVAddr, DAddr> pages;

    size_t GetID() const { return id; }
    size_t MaxContinuousRange(GPUVAddr address, size_t size) {
        ++walks;
        size_t result = 0;
        DAddr expected{};
        while (result < size) {
            const auto page = pages.find(address & ~(Page - 1));
            if (page == pages.end()) break;
            const DAddr device = page->second + (address & (Page - 1));
            if (result != 0 && device != expected) break;
            const size_t bytes = (std::min)(size - result, Page - (address & (Page - 1)));
            result += bytes;
            address += bytes;
            expected = device + bytes;
        }
        return result;
    }
    void Map(GPUVAddr address, DAddr device, size_t count) {
        for (size_t page = 0; page < count; ++page) {
            pages[address + page * Page] = device + page * Page;
        }
    }
};
} // namespace

TEST_CASE("Native first-span cache reuses bounded queries without walking mappings",
          "[video_core][storage_mapping][virtual_range_cache]") {
    VideoCommon::VirtualRangeCache cache;
    MappingMemory memory;
    memory.Map(Base, 0x100000, 3);
    bool hit = true;
    REQUIRE(cache.QueryFirst(memory, Base + 13, 5 * Page, &hit) == 3 * Page - 13);
    REQUIRE_FALSE(hit);
    for (int repeat = 0; repeat < 20; ++repeat) {
        REQUIRE(cache.QueryFirst(memory, Base + 13, 5 * Page, &hit) == 3 * Page - 13);
        REQUIRE(hit);
    }
    REQUIRE(memory.walks == 1);
    cache.Unmap(memory.id + 1, Base, 5 * Page); // Different address space.
    cache.Unmap(memory.id, Base + 8 * Page, Page); // Outside full queried interval.
    REQUIRE(cache.QueryFirst(memory, Base + 13, 5 * Page, &hit) == 3 * Page - 13);
    REQUIRE(hit);
    REQUIRE(memory.walks == 1);
    REQUIRE(cache.QueryFirst(memory, Base + 13, Page, &hit) == Page);
    REQUIRE_FALSE(hit); // A different bound is a different query.
    REQUIRE(memory.walks == 2);
}

TEST_CASE("Native first-span cache invalidates holes negative results and remapped backing",
          "[video_core][storage_mapping][virtual_range_cache]") {
    VideoCommon::VirtualRangeCache cache;
    MappingMemory memory;
    memory.Map(Base, 0x100000, 2);
    memory.Map(Base + 3 * Page, 0x103000, 1);
    REQUIRE(cache.QueryFirst(memory, Base, 4 * Page) == 2 * Page);
    memory.Map(Base + 2 * Page, 0x102000, 1);
    cache.Unmap(memory.id, Base + 2 * Page, Page); // Fill a hole beyond the cached first run.
    REQUIRE(cache.QueryFirst(memory, Base, 4 * Page) == 4 * Page);
    memory.Map(Base, 0x400000, 1);
    cache.Unmap(memory.id, Base, Page);
    REQUIRE(cache.QueryFirst(memory, Base, 4 * Page) == Page);
    memory.pages.erase(Base);
    cache.Unmap(memory.id, Base, Page);
    REQUIRE(cache.QueryFirst(memory, Base, 4 * Page) == 0);
    const size_t walks = memory.walks;
    REQUIRE(cache.QueryFirst(memory, Base, 4 * Page) == 0);
    REQUIRE(memory.walks == walks);
    memory.Map(Base, 0x100000, 1);
    cache.Unmap(memory.id, Base, Page);
    REQUIRE(cache.QueryFirst(memory, Base, 4 * Page) == 4 * Page);
}

TEST_CASE("Native first-span cache validates identity despite key collisions",
          "[video_core][storage_mapping][virtual_range_cache]") {
    VideoCommon::VirtualRangeCache cache;
    MappingMemory a;
    MappingMemory b;
    a.id = 1;
    b.id = 0;
    a.Map(Base, 0x100000, 1);
    b.Map((1ULL << 48) | Base, 0x200000, 2);
    // These pairs collide under the existing (as_id << 48) xor gpu_addr hash key.
    REQUIRE(cache.QueryFirst(a, Base, 2 * Page) == Page);
    REQUIRE(cache.QueryFirst(b, (1ULL << 48) | Base, 2 * Page) == 2 * Page);
    REQUIRE(cache.QueryFirst(a, Base, 2 * Page) == Page);
    REQUIRE(a.walks == 2);
    REQUIRE(b.walks == 1);
}

TEST_CASE("Native first-span cache bounds entries and conservatively handles invalidation overflow",
          "[video_core][storage_mapping][virtual_range_cache]") {
    VideoCommon::VirtualRangeCache cache;
    MappingMemory memory;
    for (size_t i = 0; i <= VideoCommon::VirtualRangeCache::MAX_ENTRIES; ++i) {
        REQUIRE(cache.QueryFirst(memory, Base + i * Page, Page) == 0);
    }
    const size_t walks = memory.walks;
    REQUIRE(cache.QueryFirst(memory, Base, Page) == 0);
    REQUIRE(memory.walks == walks + 1); // Bounded cache retired the first entry.
    for (size_t i = 0; i <= VideoCommon::VirtualRangeCache::MAX_DEFERRED; ++i) {
        cache.Unmap(memory.id, 0x10000000 + i * 2 * Page, Page);
    }
    REQUIRE(cache.QueryFirst(memory, Base, Page) == 0);
    REQUIRE(memory.walks == walks + 2); // Too many disjoint changes retire all entries.
    cache.Unmap(memory.id, ~u64{0} - Page, 2 * Page);
    REQUIRE(cache.QueryFirst(memory, Base, Page) == 0);
    REQUIRE(memory.walks == walks + 3); // Overflowing invalidation cannot wrap past an entry.
    REQUIRE(cache.QueryFirst(memory, ~u64{0} - 2, 4) == 0);
    REQUIRE(cache.QueryFirst(memory, Base, 0) == 0);
    REQUIRE(memory.walks == walks + 3);
}
