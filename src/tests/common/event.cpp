// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/thread.h"

using namespace std::chrono_literals;

TEST_CASE("Timed events consume one signal and reject nonpositive timeouts", "[common][event]") {
    Common::Event event;
    CHECK_FALSE(event.WaitFor(0ns));
    CHECK_FALSE(event.WaitFor(-1ns));
    event.Set();
    CHECK(event.WaitFor(0ns));
    CHECK_FALSE(event.WaitFor(0ns));
    event.Set();
    CHECK(event.WaitFor(-1ns));
    CHECK_FALSE(event.IsSet());
}

TEST_CASE("Resetting an unsignalled event does not complete a timed wait", "[common][event]") {
    Common::Event event;
    // Reset writes to the monitored cache line without setting the predicate. On
    // MONITORX/UMONITOR this can wake the instruction, but is not a timeout/signal.
    std::jthread resetter([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(1ms);
            event.Reset();
        }
    });
    const auto begin = std::chrono::steady_clock::now();
    const bool signalled = event.WaitFor(25ms);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    resetter.request_stop();
    resetter.join();
    CHECK_FALSE(signalled);
    // Allow clock calibration/scheduling variation; a cache-line wake after ~1 ms
    // must not be treated as the requested 25 ms deadline.
    CHECK(elapsed >= 20ms);
}

TEST_CASE("Timed events wake when signalled and auto-reset", "[common][event]") {
    Common::Event event;
    std::jthread producer([&] {
        std::this_thread::sleep_for(2ms);
        event.Set();
    });
    CHECK(event.WaitFor(1s));
    producer.join();
    CHECK_FALSE(event.WaitFor(0ns));
}
