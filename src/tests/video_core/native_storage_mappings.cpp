// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdexcept>
#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/buffer_cache/virtual_range_cache.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace {
// Mapping tests need cache-invalidation callbacks, not a renderer or GPU device.
class MappingRasterizer final : public VideoCore::RasterizerInterface {
public:
    VideoCommon::VirtualRangeCache* mapping_cache{};
    void Draw(bool, u32) override {}
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32, u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&&) override {}
    void SyncOperation(std::function<void()>&&) override {}
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override { return false; }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr, u64) override { return {}; }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(PAddr, u64) override {}
    bool OnCPUWrite(PAddr, u64) override { return false; }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t as_id, GPUVAddr address, u64 size) override {
        if (mapping_cache) mapping_cache->Unmap(as_id, address, size);
    }
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        throw std::logic_error("Mapping test must not access DMA");
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}
};
} // namespace

TEST_CASE("Native storage first-run mapping matches full segmentation", "[video_core][storage_mapping]") {
    Core::System system;
    Core::DeviceMemory device_memory;
    Tegra::MaxwellDeviceMemoryManager device_manager(device_memory);
    MappingRasterizer rasterizer;
    VideoCommon::VirtualRangeCache cache;
    rasterizer.mapping_cache = &cache;
    Tegra::MemoryManager memory(system, device_manager, 32, 1ULL << 31, 16, 12);
    memory.BindRasterizer(&rasterizer);
    constexpr GPUVAddr base = 0x100000;
    constexpr DAddr backing = 0x1000000;
    constexpr size_t page = 4096;
    const auto check = [&](GPUVAddr address, size_t size) {
        const auto segments = memory.GetSubmappedRange(address, size);
        const size_t expected = !segments.empty() && segments.front().first == address
                                    ? segments.front().second : 0;
        REQUIRE(memory.MaxContinuousRange(address, size) == expected);
        REQUIRE(cache.QueryFirst(memory, address, static_cast<u32>(size)) == expected);
        // The full-segment cache and first-run cache share invalidation but not results.
        GPUVAddr covered = address;
        for (const auto& [start, length] : segments) {
            if (start != covered) break;
            covered += length;
        }
        const auto* full = cache.Query(memory, address, static_cast<u32>(size));
        REQUIRE(full->size() == (covered == address + size ? segments.size() : 0));
    };

    check(base, 8 * page); // Unmapped start.
    memory.Map(base, backing, 4 * page, Tegra::PTEKind::GENERIC_16BX2, false);
    check(base, 0);
    check(base, 8 * page);
    check(base + 17, 3 * page - 31);
    REQUIRE(memory.MaxContinuousRange(base, 8 * page) == 4 * page);
    memory.Map(base + 5 * page, backing + 5 * page, page, Tegra::PTEKind::GENERIC_16BX2, false);
    check(base, 8 * page); // Hole followed by another mapped segment.
    memory.Map(base + 4 * page, backing + 9 * page, page, Tegra::PTEKind::GENERIC_16BX2, false);
    check(base + 17, 7 * page); // Device-discontinuous backing.
    REQUIRE(memory.MaxContinuousRange(base, 8 * page) == 4 * page);
    memory.Map(base + 4 * page, backing + 4 * page, page, Tegra::PTEKind::GENERIC_16BX2, false);
    check(base, 8 * page); // Remap joins the first six pages.
    REQUIRE(memory.MaxContinuousRange(base, 8 * page) == 6 * page);

    constexpr GPUVAddr big = base + 0x20000;
    memory.Map(big, backing + 0x20000, 0x10000, Tegra::PTEKind::GENERIC_16BX2, true);
    memory.Map(big + 0x10000, backing + 0x30000, 2 * page,
               Tegra::PTEKind::GENERIC_16BX2, false);
    check(big + 3, 0x10000 + 2 * page - 17); // Big page into contiguous small pages.
    REQUIRE(memory.MaxContinuousRange(big, 0x20000) == 0x10000 + 2 * page);
    memory.Map(big + 0x11000, backing + 0x60000, page, Tegra::PTEKind::GENERIC_16BX2, false);
    check(big, 0x20000);
    REQUIRE(memory.MaxContinuousRange(big, 0x20000) == 0x11000);
    memory.Unmap(big + 0x10000, 2 * page);
    check(big + 9, 0x20000);
    REQUIRE(memory.MaxContinuousRange(big, 0x20000) == 0x10000);
    memory.Map(big + 0x10000, backing + 0x80000, 0x10000, Tegra::PTEKind::GENERIC_16BX2, true);
    check(big, 0x20000); // Big-page discontinuity.
    REQUIRE(memory.MaxContinuousRange(big, 0x20000) == 0x10000);
}
