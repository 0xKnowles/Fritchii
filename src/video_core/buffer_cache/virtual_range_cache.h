// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/container/unordered_map.h"
#include "video_core/memory_manager.h"

namespace VideoCommon {

struct VirtualSegment {
    GPUVAddr gpu_addr;
    DAddr device_addr;
    u32 size;
};

using VirtualSegments = boost::container::small_vector<VirtualSegment, 8>;

class VirtualRangeCache {
public:
    static constexpr size_t MAX_ENTRIES = 8192;
    static constexpr size_t MAX_DEFERRED = 4096;

    /// Native bare-pointer bindings need only the initial device-contiguous run.
    /// Cache its length, not guest pointers, and retain the entire queried extent:
    /// filling a hole after the returned run must invalidate this result too.
    /// Only use where every GPU map/remap/unmap calls Unmap below.
    template <typename Memory>
    u32 QueryFirst(Memory& memory, GPUVAddr gpu_addr, u32 size, bool* cache_hit = nullptr) {
        if (cache_hit) {
            *cache_hit = false;
        }
        if (has_deferred.load(std::memory_order_acquire)) {
            ApplyDeferred();
        }
        if (size == 0 || size > ~u64{0} - gpu_addr) {
            return 0;
        }
        const size_t as_id = memory.GetID();
        const u64 key = MakeKey(as_id, gpu_addr);
        const auto it = first_spans.find(key);
        if (it != first_spans.end() && it->second.as_id == as_id &&
            it->second.gpu_addr == gpu_addr && it->second.size == size) {
            if (cache_hit) {
                *cache_hit = true;
            }
            return it->second.span;
        }
        if (first_spans.size() >= MAX_ENTRIES) {
            first_spans.clear();
        }
        const u32 span = static_cast<u32>(memory.MaxContinuousRange(gpu_addr, size));
        first_spans.insert_or_assign(key, FirstSpanEntry{as_id, gpu_addr, size, span});
        return span;
    }

    const VirtualSegments* Query(Tegra::MemoryManager& memory, GPUVAddr gpu_addr, u32 size) {
        if (has_deferred.load(std::memory_order_acquire)) {
            ApplyDeferred();
        }
        if (entries.size() > MAX_ENTRIES) {
            entries.clear();
        }
        const size_t as_id = memory.GetID();
        const u64 key = MakeKey(as_id, gpu_addr);
        const auto it = entries.find(key);
        if (it != entries.end() && it->second.as_id == as_id &&
            it->second.gpu_addr == gpu_addr && it->second.size == size) {
            return &it->second.segments;
        }
        Entry entry{};
        entry.as_id = as_id;
        entry.gpu_addr = gpu_addr;
        entry.size = size;
        const auto ranges = memory.GetSubmappedRange(gpu_addr, size);
        GPUVAddr expected = gpu_addr;
        bool contiguous = true;
        for (const auto& [range_addr, range_size] : ranges) {
            if (range_addr != expected || range_size == 0) {
                contiguous = false;
                break;
            }
            const std::optional<DAddr> device_addr = memory.GpuToCpuAddress(range_addr);
            if (!device_addr || *device_addr == 0) {
                contiguous = false;
                break;
            }
            if (range_size > static_cast<size_t>((std::numeric_limits<u32>::max)())) {
                contiguous = false;
                break;
            }
            entry.segments.push_back(VirtualSegment{
                .gpu_addr = range_addr,
                .device_addr = *device_addr,
                .size = static_cast<u32>(range_size),
            });
            expected += range_size;
        }
        if (!contiguous || expected != gpu_addr + size) {
            entry.segments.clear();
        }
        const auto result = entries.insert_or_assign(key, std::move(entry));
        return &result.first->second.segments;
    }

    void Unmap(size_t as_id, GPUVAddr gpu_addr, u64 size) {
        if (size == 0) {
            return;
        }
        {
            std::scoped_lock lock{deferred_mutex};
            if (size > ~u64{0} - gpu_addr) {
                deferred.clear();
                deferred_overflow = true;
                has_deferred.store(true, std::memory_order_release);
                return;
            }
            if (!deferred.empty()) {
                DeferredUnmap& last = deferred.back();
                if (last.as_id == as_id && last.gpu_addr + last.size == gpu_addr &&
                    size <= ~u64{0} - last.size) {
                    last.size += size;
                    has_deferred.store(true, std::memory_order_release);
                    return;
                }
            }
            if (deferred.size() >= MAX_DEFERRED) {
                deferred.clear();
                deferred_overflow = true;
            } else {
                deferred.push_back(DeferredUnmap{
                    .as_id = as_id,
                    .gpu_addr = gpu_addr,
                    .size = size,
                });
            }
        }
        has_deferred.store(true, std::memory_order_release);
    }

private:
    struct Entry {
        VirtualSegments segments;
        size_t as_id{};
        GPUVAddr gpu_addr{};
        u32 size{};
    };

    struct DeferredUnmap {
        size_t as_id;
        GPUVAddr gpu_addr;
        u64 size;
    };

    struct FirstSpanEntry {
        size_t as_id;
        GPUVAddr gpu_addr;
        u32 size;
        u32 span;
    };

    static u64 MakeKey(size_t as_id, GPUVAddr gpu_addr) {
        return (static_cast<u64>(as_id) << 48) ^ gpu_addr;
    }

    void ApplyDeferred() {
        std::vector<DeferredUnmap> pending;
        bool overflow = false;
        {
            std::scoped_lock lock{deferred_mutex};
            has_deferred.store(false, std::memory_order_release);
            pending.swap(deferred);
            overflow = deferred_overflow;
            deferred_overflow = false;
        }
        if (overflow) {
            entries.clear();
            first_spans.clear();
            return;
        }
        if (pending.empty()) {
            return;
        }
        const auto invalidate = [&](auto& cached) {
            for (auto it = cached.begin(); it != cached.end();) {
                const auto& entry = it->second;
                const GPUVAddr entry_end = entry.gpu_addr + entry.size;
                bool overlaps = false;
                for (const DeferredUnmap& unmap : pending) {
                    if (unmap.as_id != entry.as_id) {
                        continue;
                    }
                    if (entry.gpu_addr < unmap.gpu_addr + unmap.size && unmap.gpu_addr < entry_end) {
                        overlaps = true;
                        break;
                    }
                }
                if (overlaps) {
                    it = cached.erase(it);
                } else {
                    ++it;
                }
            }
        };
        invalidate(entries);
        invalidate(first_spans);
    }

    ::Common::unordered_map<u64, Entry> entries;
    ::Common::unordered_map<u64, FirstSpanEntry> first_spans;
    std::vector<DeferredUnmap> deferred;
    std::mutex deferred_mutex;
    std::atomic<bool> has_deferred{false};
    bool deferred_overflow{};
};

} // namespace VideoCommon
