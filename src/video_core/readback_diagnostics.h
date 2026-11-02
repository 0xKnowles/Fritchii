// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <string>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2.h"

namespace VideoCommon::ReadbackDiagnostics {

enum class Event : size_t {
    OrderedRead,
    FlushArea,
    TextureReadback,
    BufferReadback,
    QueryReadback,
    DownloadStaging,
    DeferredDownloadStaging,
    UploadStaging,
    BufferFinish,
    Submit,
    DeviceFinish,
    FenceDownloadCommit,
    FenceDownloadWriteback,
    /// Buffer pages a CPU read newly made eager (downloaded at every fence after a GPU write).
    EagerPagesAdded,
    FrontQueueWait,
    FrontPendingPoll,
    FrontPendingYield,
    FrontPendingWait,
    FrontDispatch,
    PendingFenceReadback,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Event::Count)> names{
    "ordered_read", "flush_area", "texture_readback", "buffer_readback", "query_readback",
    "download_staging", "deferred_download_staging", "upload_staging", "buffer_finish",
    "submit", "device_finish", "fence_download_commit", "fence_download_writeback",
    "eager_pages_added",
    "front_queue_wait", "front_pending_poll", "front_pending_yield", "front_pending_wait",
    "front_dispatch",
    "pending_fence_readback",
};

// Heavy-tailed waits cannot be extrapolated from one call in 256. Time these
// relatively low-frequency critical-path operations in full; keep sampling the
// very frequent cache probes and staging allocations. Durations are inclusive:
// dispatch/poll can contain a Finish, and BufferFinish contains DeviceFinish.
// Never add nested event totals together as independent wall time.
constexpr bool FullyTimed(Event event) {
    switch (event) {
    case Event::OrderedRead:
    case Event::BufferFinish:
    case Event::DeviceFinish:
    case Event::FrontQueueWait:
    case Event::FrontPendingPoll:
    case Event::FrontPendingYield:
    case Event::FrontPendingWait:
    case Event::FrontDispatch:
    case Event::PendingFenceReadback:
        return true;
    default:
        return false;
    }
}

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_READBACK_DIAGNOSTICS") != nullptr;
    // Thread-local timing only: do not enable the full GPU profiler and its shared
    // instruction/page tracking. Native titles need attribution of these stalls.
    return enabled || Tegra::g_nx2_ampere.load(std::memory_order_relaxed);
}

struct Measurement {
    u64 calls{};
    u64 bytes{};
    u64 samples{};
    u64 sampled_ns{};
    u64 max_ns{};
    u64 over_1ms{};
    u64 over_10ms{};

    void RecordSample(u64 elapsed_ns) {
        ++samples;
        sampled_ns += elapsed_ns;
        if (elapsed_ns > max_ns) max_ns = elapsed_ns;
        over_1ms += elapsed_ns > 1'000'000;
        over_10ms += elapsed_ns > 10'000'000;
    }
};

struct ThreadData {
    std::array<Measurement, static_cast<size_t>(Event::Count)> events{};
    std::chrono::steady_clock::time_point begin{};
    size_t depth{};
    bool report_due{};
};

inline thread_local ThreadData data;

class Scope {
public:
    explicit Scope(Event event_, u64 bytes = 0) : event{event_}, enabled{Enabled()} {
        if (!enabled) {
            return;
        }
        ++data.depth;
        auto& measurement = data.events[static_cast<size_t>(event)];
        measurement.bytes += bytes;
        const u64 call = measurement.calls++;
        sampled = FullyTimed(event) || (call & 255) == 0;
        if (sampled) {
            start = std::chrono::steady_clock::now();
            if (data.begin == std::chrono::steady_clock::time_point{}) {
                data.begin = start;
            }
        }
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        if (sampled) {
            const auto now = std::chrono::steady_clock::now();
            auto& measurement = data.events[static_cast<size_t>(event)];
            measurement.RecordSample(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count());
            data.report_due |= now - data.begin >= std::chrono::seconds{10};
        }
        if (--data.depth != 0 || !data.report_due) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        std::string summary;
        for (size_t i = 0; i < data.events.size(); ++i) {
            const auto& value = data.events[i];
            if (value.calls != 0) {
                summary += fmt::format(" | {} calls={} bytes={} samples={} sampled_ns={}", names[i],
                                       value.calls, value.bytes, value.samples, value.sampled_ns);
                summary += fmt::format(" timing={} max_ns={} over_1ms={} over_10ms={}",
                                       FullyTimed(static_cast<Event>(i)) ? "full_inclusive"
                                                                        : "sampled_1_256",
                                       value.max_ns, value.over_1ms, value.over_10ms);
            }
        }
        LOG_INFO(HW_GPU, "ReadbackDiag window_ns={}{}",
                 std::chrono::duration_cast<std::chrono::nanoseconds>(now - data.begin).count(),
                 summary);
        data = {};
        data.begin = now;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Event event;
    bool enabled;
    bool sampled{};
    std::chrono::steady_clock::time_point start{};
};

} // namespace VideoCommon::ReadbackDiagnostics
