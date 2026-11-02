// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include "video_core/split_completion.h"

TEST_CASE("Split completion acknowledges a requested frontier before a batch tail",
          "[video_core][split_completion]") {
    VideoCommon::SplitCompletion completion;
    for (u64 target = 1; target <= 128; ++target) {
        completion.Reset();
        completion.Request(target);
        for (u64 completed = 0; completed < target; ++completed) {
            REQUIRE_FALSE(completion.ShouldPublish(completed, 0));
        }
        REQUIRE(completion.ShouldPublish(target, 0));
        REQUIRE_FALSE(completion.ShouldPublish(target + 1, target));
        REQUIRE(completion.ShouldPublish(target + 4096, target));
    }
}

TEST_CASE("Split completion handles late requests, wrap positions, and restart",
          "[video_core][split_completion]") {
    VideoCommon::SplitCompletion completion;
    constexpr u64 ring_end = 1ULL << 20;
    REQUIRE_FALSE(completion.ShouldPublish(64, 0));
    completion.Request(32); // Request arrives after execution but before publication.
    REQUIRE(completion.ShouldPublish(65, 0));
    REQUIRE_FALSE(completion.ShouldPublish(66, 65));
    completion.Request(ring_end + 2);
    REQUIRE_FALSE(completion.ShouldPublish(ring_end, ring_end - 4));
    REQUIRE(completion.ShouldPublish(ring_end + 2, ring_end - 4));
    completion.Reset();
    REQUIRE_FALSE(completion.ShouldPublish(2, 0));
    REQUIRE(completion.ShouldPublish(4096, 0));
}

TEST_CASE("Split completion release makes writer data visible without executing the tail",
          "[video_core][split_completion]") {
    VideoCommon::SplitCompletion completion;
    std::atomic<u64> published{}, consumed{}, acknowledged{};
    u64 payload{};
    bool visible = true;
    bool finished = true;
    constexpr u64 rounds = 2048;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    std::jthread back([&](std::stop_token stop) {
        for (u64 round = 1; round <= rounds; ++round) {
            while (published.load(std::memory_order_acquire) < round) {
                if (stop.stop_requested())
                    return;
                std::this_thread::yield();
            }
            payload = round * 17;
            if (completion.ShouldPublish(round, round - 1)) {
                consumed.store(round, std::memory_order_release);
            }
            // Simulate a tail blocked until the front has observed the writer.
            // Batch-end-only publication would time out here.
            while (acknowledged.load(std::memory_order_acquire) < round) {
                if (stop.stop_requested())
                    return;
                std::this_thread::yield();
            }
        }
    });
    for (u64 round = 1; round <= rounds; ++round) {
        completion.Request(round);
        published.store(round, std::memory_order_release);
        while (consumed.load(std::memory_order_acquire) < round) {
            if (std::chrono::steady_clock::now() > deadline) {
                finished = false;
                break;
            }
            std::this_thread::yield();
        }
        if (!finished)
            break;
        visible &= payload == round * 17;
        acknowledged.store(round, std::memory_order_release);
    }
    back.request_stop();
    back.join();
    REQUIRE(finished);
    REQUIRE(visible);
}
