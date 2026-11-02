// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <stdexcept>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/delayed_destruction_ring.h"
#include "video_core/texture_cache/retired_resource.h"

namespace {
struct Resource {
    explicit Resource(unsigned& destroyed_, VideoCommon::RetirementMemory& memory_, u64 bytes_)
        : destroyed{&destroyed_}, memory{&memory_}, bytes{bytes_} {}
    Resource(Resource&& other) noexcept
        : destroyed{std::exchange(other.destroyed, nullptr)}, memory{other.memory}, bytes{other.bytes} {}
    ~Resource() {
        if (destroyed) {
            // The credit must survive through resource destruction.
            CHECK(memory->PendingBytes() >= bytes);
            ++*destroyed;
        }
    }
    unsigned* destroyed;
    VideoCommon::RetirementMemory* memory;
    u64 bytes;
};
}

TEST_CASE("Retired image bytes stay credited until their destruction tick", "[video_core][retired_resource]") {
    VideoCommon::RetirementMemory memory;
    unsigned destroyed = 0;
    {
        VideoCommon::DelayedDestructionRing<VideoCommon::RetiredResource<Resource>, 8> ring;
        ring.Push({memory, 200, Resource{destroyed, memory, 200}});
        CHECK(memory.PendingBytes() == 200);
        for (unsigned tick = 0; tick < 7; ++tick) {
            // The driver's next frame still includes the same 200 bytes. They must
            // not become pressure for selecting another 200-byte eviction each tick.
            CHECK(memory.RemainingUsage(1000) == 800);
            ring.Tick();
            CHECK(destroyed == 0);
            CHECK(memory.PendingBytes() == 200);
        }
        CHECK(memory.RemainingUsage(1100) == 900); // New allocations still count.
        ring.Tick();
        CHECK(destroyed == 1);
        CHECK(memory.PendingBytes() == 0);
        CHECK(memory.RemainingUsage(800) == 800);
    }
}

TEST_CASE("Retirement moves and staggered lifetimes cannot duplicate credits", "[video_core][retired_resource]") {
    VideoCommon::RetirementMemory memory;
    unsigned destroyed = 0;
    {
        VideoCommon::DelayedDestructionRing<VideoCommon::RetiredResource<Resource>, 3> ring;
        ring.Push({memory, 100, Resource{destroyed, memory, 100}});
        ring.Tick();
        for (unsigned i = 0; i < 32; ++i) {
            ring.Push({memory, 10, Resource{destroyed, memory, 10}});
        }
        CHECK(memory.PendingBytes() == 420);
        CHECK(memory.RemainingUsage(50) == 0);
        ring.Tick();
        CHECK(memory.PendingBytes() == 420);
        ring.Tick();
        CHECK(memory.PendingBytes() == 320);
        CHECK(destroyed == 1);
        // Cache/ring teardown retires the rest even before their scheduled tick.
    }
    CHECK(destroyed == 33);
    CHECK(memory.PendingBytes() == 0);
    CHECK(memory.RemainingUsage(700) == 700);
}

TEST_CASE("Failed retired resource moves preserve the original credit", "[video_core][retired_resource]") {
    struct ThrowingMove {
        bool* fail;
        explicit ThrowingMove(bool& fail_) : fail{&fail_} {}
        ThrowingMove(ThrowingMove&& other) : fail{other.fail} {
            if (*fail) throw std::runtime_error("move failed");
        }
    };
    VideoCommon::RetirementMemory memory;
    bool fail = false;
    {
        VideoCommon::RetiredResource<ThrowingMove> resource{memory, 100, ThrowingMove{fail}};
        fail = true;
        CHECK_THROWS_AS(VideoCommon::RetiredResource<ThrowingMove>{std::move(resource)},
                        std::runtime_error);
        CHECK(memory.PendingBytes() == 100);
    }
    CHECK(memory.PendingBytes() == 0);
}
