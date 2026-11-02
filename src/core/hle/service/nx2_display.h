// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>
#include "common/common_types.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/result.h"

namespace Service {

struct Nx2DisplayReply {
    Result result{Kernel::ResultInvalidCombination};
    std::array<u32, 6> words{};
    u32 word_count{};
};

// VI2 requests contain a TIPC header followed by the eight-byte display name.
// Payload layouts are adapted from Duck912/eden-fefw's graphics_supervisor.cpp.
// The current compositor exposes the default SDR display, not a host HDR display.
inline Nx2DisplayReply Nx2QueryDisplay(std::span<const u32> request, bool docked) {
    Nx2DisplayReply reply;
    if (request.size() < 4 || request[1] != 2) {
        return reply;
    }
    switch (request[0]) {
    case 0x78: // 104: u64 output format, u32 flags
        reply.word_count = 3;
        break;
    case 0x79: // 105: u16 display state, padded to a word
        reply.word_count = 1;
        break;
    case 0x7a:  // 106: u64 maximum buffer resolution mode
    case 0x172: // 354: u64 display state
        reply.word_count = 2;
        break;
    case 0x7b: // 107: three u64 display-driver values
        reply.word_count = 6;
        break;
    default:
        return reply;
    }
    const u64 name = u64{request[2]} | (u64{request[3]} << 32);
    if (name != 0x00746c7561666564ULL) { // "default"
        const bool state_query = request[0] == 0x79 || request[0] == 0x7b || request[0] == 0x172;
        reply.result = Result{ErrorModule::VI, name == 0 && state_query ? 4U : 7U};
        return reply;
    }
    reply.result = ResultSuccess;
    switch (request[0]) {
    case 0x78:
    case 0x172:
        reply.words[0] = 1;
        break;
    case 0x7a:
        reply.words[0] = docked ? 4 : 2;
        break;
    case 0x7b:
        reply.words[0] = 1;
        reply.words[2] = 1;
        break;
    default:
        break;
    }
    return reply;
}

} // namespace Service
