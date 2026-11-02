// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <memory>
#include <stdexcept>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "video_core/buffer_cache/uniform_upload_cache.h"

namespace {
using Ref = std::shared_ptr<const std::vector<u8>>;
using Cache = VideoCommon::UniformUploadCache<Ref>;
Ref Allocate(std::span<const u8> bytes) {
    return std::make_shared<const std::vector<u8>>(bytes.begin(), bytes.end());
}
}

TEST_CASE("Uniform uploads compare current bytes and preserve older allocations", "[video_core][uniform_upload]") {
    auto cache = std::make_unique<Cache>();
    std::array<u8, 4> bytes{1, 2, 3, 4};
    const auto first = cache->Get(0, 0, 10, bytes, Allocate);
    REQUIRE(first);
    REQUIRE_FALSE(first->reused);
    const auto repeated = cache->Get(0, 0, 10, bytes, Allocate);
    REQUIRE(repeated->reused);
    REQUIRE(repeated->ref == first->ref);
    bytes[0] = 9;
    const auto changed = cache->Get(0, 0, 10, bytes, Allocate);
    REQUIRE_FALSE(changed->reused);
    REQUIRE(changed->ref != first->ref);
    REQUIRE(first->ref->at(0) == 1); // In-flight previous draw remains immutable.
    REQUIRE(changed->ref->at(0) == 9);
    REQUIRE_FALSE(cache->Get(0, 0, 11, bytes, Allocate)->reused); // New tick cannot reuse old storage.
    REQUIRE_FALSE(cache->Get(0, 0, 11, std::span{bytes}.first(2), Allocate)->reused);
}

TEST_CASE("Uniform cache separates graphics compute and banks with bounded fallback", "[video_core][uniform_upload]") {
    auto cache = std::make_unique<Cache>();
    std::array<u8, 4> bytes{5, 6, 7, 8};
    const auto vertex = cache->Get(0, 0, 1, bytes, Allocate);
    const auto compute = cache->Get(Cache::ComputeStage, 0, 1, bytes, Allocate);
    REQUIRE_FALSE(compute->reused);
    REQUIRE(compute->ref != vertex->ref);
    REQUIRE_FALSE(cache->Get(0, 1, 1, bytes, Allocate)->reused);
    REQUIRE(cache->Get(0, 0, 1, bytes, Allocate)->reused);
    REQUIRE_FALSE(cache->Get(Cache::Stages, 0, 1, bytes, Allocate));
    REQUIRE_FALSE(cache->Get(0, Cache::Banks, 1, bytes, Allocate));
    REQUIRE_FALSE(cache->Get(0, 0, 1, {}, Allocate));
    std::vector<u8> large(Cache::MaxBytes + 1);
    REQUIRE_FALSE(cache->Get(0, 0, 1, large, Allocate));
}

TEST_CASE("Failed uniform allocation cannot publish new bytes", "[video_core][uniform_upload]") {
    auto cache = std::make_unique<Cache>();
    std::array<u8, 1> bytes{2};
    const auto first = cache->Get(0, 0, 1, bytes, Allocate);
    bytes[0] = 3;
    REQUIRE_THROWS_AS(cache->Get(0, 0, 1, bytes, [](std::span<const u8>) -> Ref {
        throw std::runtime_error("allocation failed");
    }), std::runtime_error);
    REQUIRE_FALSE(cache->Get(0, 0, 1, bytes, Allocate)->reused);
    REQUIRE(first->ref->at(0) == 2);
}

TEST_CASE("GPU-written uniforms download before upload-cache comparison", "[video_core][uniform_upload]") {
    auto cache = std::make_unique<Cache>();
    std::array<u8, 4> guest{1, 2, 3, 4};
    const std::array<u8, 4> gpu{9, 8, 7, 6};
    for (size_t stage : {size_t{0}, Cache::ComputeStage}) {
        cache->Get(stage, 0, 1, guest, Allocate);
        bool downloaded = false;
        const auto current = VideoCommon::WithSynchronizedUniformRead(true, [&] {
            guest = gpu;
            downloaded = true;
        }, [&] {
            REQUIRE(downloaded);
            return cache->Get(stage, 0, 1, guest, Allocate);
        });
        REQUIRE(current->ref->at(0) == 9);
        VideoCommon::WithSynchronizedUniformRead(false,
            [] { FAIL("Clean uniform must not force a download"); }, [&] {
                REQUIRE(cache->Get(stage, 0, 1, guest, Allocate)->reused);
            });
        REQUIRE_THROWS_AS(VideoCommon::WithSynchronizedUniformRead(true,
            [] { throw std::runtime_error("download failed"); },
            [] { FAIL("Must not read stale guest bytes after failure"); }), std::runtime_error);
        guest = {1, 2, 3, 4};
    }
}
