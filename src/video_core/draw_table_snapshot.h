// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <vector>
#include "common/common_types.h"

namespace VideoCommon {

// Own copied bytes only. BeginDraw invalidates every snapshot, even at unchanged
// addresses: intervening GPU writes must be synchronized and reread.
class DrawTableSnapshotCache {
public:
    static constexpr size_t MaxTables = 8;
    struct Snapshot {
        std::span<const u8> bytes;
        bool reused;
    };
    void BeginDraw() { active = 0; }

    // Caller validates each descriptor root before lookup. Loader synchronizes
    // mapped segments and copies their bytes; only successful loads are cached.
    // Consume the returned span before the next Read/BeginDraw call.
    template <typename Load>
    Snapshot Read(size_t address_space, GPUVAddr address, size_t size, Load&& load) {
        for (size_t index = 0; index < active; ++index) {
            const auto& entry = entries[index];
            if (entry.address_space == address_space && entry.address == address &&
                entry.bytes.size() == size) {
                return {entry.bytes, true};
            }
        }
        if (active == MaxTables) {
            overflow.resize(size);
            load(std::span<u8>{overflow});
            return {overflow, false};
        }
        if (active == entries.size()) entries.emplace_back();
        auto& entry = entries[active];
        entry.bytes.resize(size);
        load(std::span<u8>{entry.bytes});
        entry.address_space = address_space;
        entry.address = address;
        ++active;
        return {entry.bytes, false};
    }
private:
    struct Entry {
        size_t address_space{};
        GPUVAddr address{};
        std::vector<u8> bytes;
    };
    std::vector<Entry> entries;
    std::vector<u8> overflow;
    size_t active{};
};

} // namespace VideoCommon
