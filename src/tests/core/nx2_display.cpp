// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "core/hle/service/nx2_display.h"

TEST_CASE("Native display queries return complete SDR payloads", "[core][nx2]") {
    std::array<u32, 4> request{0x78, 2, 0x61666564, 0x00746c75};
    auto reply = Service::Nx2QueryDisplay(request, true);
    REQUIRE(reply.result.IsSuccess());
    REQUIRE(reply.word_count == 3);
    REQUIRE(reply.words == std::array<u32, 6>{1, 0, 0, 0, 0, 0});
    request[0] = 0x79;
    reply = Service::Nx2QueryDisplay(request, true);
    REQUIRE(reply.result.IsSuccess());
    REQUIRE(reply.word_count == 1);
    REQUIRE(reply.words == std::array<u32, 6>{});
    request[0] = 0x7a;
    reply = Service::Nx2QueryDisplay(request, true);
    REQUIRE(reply.result.IsSuccess());
    REQUIRE(reply.word_count == 2);
    REQUIRE(reply.words == std::array<u32, 6>{4, 0, 0, 0, 0, 0});
    REQUIRE(Service::Nx2QueryDisplay(request, false).words[0] == 2);
    request[0] = 0x7b;
    reply = Service::Nx2QueryDisplay(request, true);
    REQUIRE(reply.result.IsSuccess());
    REQUIRE(reply.word_count == 6);
    REQUIRE(reply.words == std::array<u32, 6>{1, 0, 1, 0, 0, 0});
    request[0] = 0x172;
    reply = Service::Nx2QueryDisplay(request, true);
    REQUIRE(reply.result.IsSuccess());
    REQUIRE(reply.word_count == 2);
    REQUIRE(reply.words == std::array<u32, 6>{1, 0, 0, 0, 0, 0});
}

TEST_CASE("Native display queries reject malformed and unknown requests", "[core][nx2]") {
    std::array<u32, 4> request{0x78, 2, 0x61666564, 0x00746c75};
    for (size_t size = 0; size < request.size(); ++size) {
        REQUIRE(Service::Nx2QueryDisplay(std::span{request}.first(size), true).result.IsError());
    }
    request[1] = 3;
    REQUIRE(Service::Nx2QueryDisplay(request, true).result.IsError());
    request[1] = 2;
    request[0] = 0x123;
    REQUIRE(Service::Nx2QueryDisplay(request, true).result.IsError());
    for (const u32 header : {0x78U, 0x79U, 0x7aU, 0x7bU, 0x172U}) {
        request = {header, 2, 0, 0};
        const auto empty = Service::Nx2QueryDisplay(request, true);
        REQUIRE(empty.result.IsError());
        const bool state_query = header == 0x79 || header == 0x7b || header == 0x172;
        REQUIRE(empty.result.raw == (Result{ErrorModule::VI, state_query ? 4U : 7U}).raw);
        REQUIRE(empty.words == std::array<u32, 6>{});
        request[2] = 1;
        const auto unknown = Service::Nx2QueryDisplay(request, true);
        REQUIRE(unknown.result.IsError());
        REQUIRE(unknown.result.raw == (Result{ErrorModule::VI, 7}).raw);
        REQUIRE(unknown.words == std::array<u32, 6>{});
    }
}
