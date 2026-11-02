// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include "common/common_types.h"

namespace VideoCommon {

// Only valid within one storage-binding pass, with no intervening guest writes
// or dispatches. Hash collisions merely lose a reuse opportunity.
class StorageBindingSync {
public:
    struct Actions {
        bool synchronize;
        bool mark_written;
    };
    Actions Touch(u32 buffer, DAddr address, u32 size, bool written) {
        auto& entry = entries[(buffer ^ (address >> 4) ^ (address >> 12) ^ size) & 15];
        if (entry.valid && entry.buffer == buffer && entry.address == address && entry.size == size) {
            const bool mark = written && !entry.written;
            entry.written |= written;
            return {false, mark};
        }
        entry = {address, buffer, size, true, written};
        return {true, written};
    }
    void Clear() { entries = {}; }

private:
    struct Entry {
        DAddr address{};
        u32 buffer{};
        u32 size{};
        bool valid{};
        bool written{};
    };
    std::array<Entry, 16> entries{};
};

} // namespace VideoCommon
