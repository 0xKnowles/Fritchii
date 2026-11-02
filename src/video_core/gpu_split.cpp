// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <thread>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdio>

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon {

namespace {

void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

[[maybe_unused]] std::atomic<u32> g_back_thread_id{};
std::atomic<bool> g_back_busy{};

// Back-thread totals since process start (monotonic; the front reports per-window deltas).
// Idle covers both spinning and sleeping with nothing published. If idle stays near zero while
// the front waits in drains, the back's own work limits throughput and fewer drains would not
// help; a large idle share means the two halves are alternating instead of overlapping.
struct BackActivity {
    std::atomic<u64> idle_ns{};
    std::atomic<u64> sleeps{};
    std::atomic<u64> batches{};
};
BackActivity g_back_activity;

/// Only the back thread writes these: avoid a locked read-modify-write per batch.
void AddBackActivity(std::atomic<u64>& counter, u64 amount) {
    counter.store(counter.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}

struct BackActivitySnapshot {
    u64 idle_ns{};
    u64 sleeps{};
    u64 batches{};

    static BackActivitySnapshot Take() {
        return {g_back_activity.idle_ns.load(std::memory_order_relaxed),
                g_back_activity.sleeps.load(std::memory_order_relaxed),
                g_back_activity.batches.load(std::memory_order_relaxed)};
    }
};

struct DrainProfileData {
    SplitDrainMetrics metrics;
    CommandFetchSyncAudit audit;
    BackActivitySnapshot back_start;
    u64 window_start{};
    u32 events{};
};
thread_local DrainProfileData drain_profile;

void StartDrainProfileWindow(DrainProfileData& data, u64 now) {
    data = {};
    data.window_start = now;
    data.back_start = BackActivitySnapshot::Take();
}

void ReportDrainProfile() {
    auto& data = drain_profile;
    if ((++data.events & 255) != 0) {
        return;
    }
    const u64 now = Nx2Profile::Now();
    if (data.window_start == 0) {
        StartDrainProfileWindow(data, now);
        return;
    }
    const u64 window = now - data.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    static constexpr std::array names{
        "other", "command fetch", "parameter flush", "parameter dirty", "submission fence", "ring space"};
    std::string line;
    for (size_t i = 0; i < names.size(); ++i) {
        const auto& entry = data.metrics.drains[i];
        if (entry.calls == 0) continue;
        line += fmt::format(" | {} {:.1f} ms/s ({:.0f}/s, {:.0f} pending/s, {:.0f} queued words/s)",
                            names[i], static_cast<double>(entry.elapsed_ns) * 1000.0 / window,
                            static_cast<double>(entry.calls) * 1e9 / window,
                            static_cast<double>(entry.pending_calls) * 1e9 / window,
                            static_cast<double>(entry.queued_words) * 1e9 / window);
    }
    const auto& metrics = data.metrics;
    LOG_INFO(HW_GPU,
             "GPU split drain attribution:{} | command fetches {:.0f}/s, {:.0f} KiB/s, "
             "historical GPU-page hits {:.0f}/s, dirty flushes {:.0f}/s ({:.0f} KiB requested/s)",
             line, static_cast<double>(metrics.command_fetches) * 1e9 / window,
             static_cast<double>(metrics.command_bytes) * 1e9 / 1024.0 / window,
             static_cast<double>(metrics.historical_hits) * 1e9 / window,
             static_cast<double>(metrics.dirty_flushes) * 1e9 / window,
             static_cast<double>(metrics.flushed_bytes) * 1e9 / 1024.0 / window);
    const auto back = BackActivitySnapshot::Take();
    const auto& audit = data.audit;
    const auto per_second = [window](u64 count) {
        return static_cast<double>(count) * 1e9 / static_cast<double>(window);
    };
    // Evidence only: every fetch below was still synchronized (command_fetch_sync_audit.h).
    LOG_INFO(HW_GPU,
             "GPU split overlap: back idle {:.1f} ms/s ({:.0f} sleeps/s, {:.0f} batches/s) | "
             "command fetch SYNC_WAIT {:.0f}/s of {:.0f}/s, known GPU-written before sync "
             "{:.0f}/s | unsynchronized candidates {:.0f}/s, {:.0f} waited/s for {:.1f} ms/s, "
             "first GPU writes seen during their sync {} (command contents not sampled)",
             static_cast<double>(back.idle_ns - data.back_start.idle_ns) * 1000.0 / window,
             per_second(back.sleeps - data.back_start.sleeps),
             per_second(back.batches - data.back_start.batches), per_second(audit.sync_fetches),
             per_second(audit.fetches), per_second(audit.known_before),
             per_second(audit.candidates), per_second(audit.candidate_waits),
             static_cast<double>(audit.candidate_wait_ns) * 1000.0 / window,
             audit.first_write_hits);
    StartDrainProfileWindow(data, now);
}

class DrainProfileScope {
public:
    DrainProfileScope(SplitDrainReason reason_, u64 queued_words_)
        : reason{reason_}, queued_words{queued_words_},
          enabled{Tegra::g_nx2_ampere.load(std::memory_order_relaxed) || Nx2Profile::Enabled()} {
        if (enabled && queued_words != 0) start = Nx2Profile::Now();
    }
    ~DrainProfileScope() {
        if (!enabled) return;
        const u64 elapsed = queued_words != 0 ? Nx2Profile::Now() - start : 0;
        drain_profile.metrics.RecordDrain(reason, queued_words, elapsed);
        ReportDrainProfile();
    }
private:
    SplitDrainReason reason;
    u64 queued_words;
    bool enabled;
    u64 start{};
};

} // Anonymous namespace

void GpuSplit::CheckOwner(const char* where) {
#ifdef _WIN32
    if (!g_back_busy.load(std::memory_order_acquire)) {
        return;
    }
    const DWORD thread = GetCurrentThreadId();
    if (thread == g_back_thread_id.load(std::memory_order_relaxed)) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    std::fprintf(stderr, "GPU split owner check: %s on thread %lu while the back runs\n", where,
                 thread);
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module{};
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base_name = std::strrchr(name, '\\');
        std::fprintf(stderr, "  frame %s+%#llx\n", base_name ? base_name + 1 : name,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                     reinterpret_cast<uintptr_t>(module)));
    }
    std::fflush(stderr);
#else
    (void)where;
#endif
}

GpuSplit GpuSplit::instance;

void GpuSplit::MaybeStart(Core::System& system_) {
    static const char mode = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT");
        return value != nullptr ? value[0] : '2';
    }();
    const bool wanted = mode == '1' || mode == '2';
    if (!wanted || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        return;
    }
    // The front reads command lists straight from guest memory; a read that flushes the caches
    // first (safe DMA, accurate GPU) would touch back-owned state. The settings can change
    // mid-game: hand the engines back to the front (and start again when they allow it).
    const bool safe_reads = Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh()
                                                          : Settings::IsDMALevelSafe();
    const bool allowed = !safe_reads && !Settings::IsGPULevelHigh() &&
                         Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
    if (active && !allowed) {
        Drain();
        Stop();
        LOG_INFO(HW_GPU, "GPU thread split: stopped, the accuracy settings need the caches on the front");
        return;
    }
    if (active || !allowed) {
        return;
    }
    system = &system_;
    drain_profile = {};
    check_owner = std::getenv("EDEN_NX2_SPLIT_CHECK") != nullptr;
    ring = std::make_unique<u64[]>(RingWords);
    back_thread = std::jthread([this](std::stop_token stop_token) { BackLoop(stop_token); });
    active = true;
#ifdef ARCHITECTURE_x86_64
    // Front macros run compiled (macro_turing_jit.h) only.
    const char* jit = std::getenv("EDEN_NX2_MME_JIT");
    front_macros = mode == '2' && (jit == nullptr || jit[0] != '0');
#endif
    LOG_INFO(HW_GPU, "GPU thread split: engine methods run on a second thread{}",
             front_macros ? ", macros on the first" : "");
}

void GpuSplit::Stop() {
    if (!back_thread.joinable()) {
        return;
    }
    back_thread.request_stop();
    wake.fetch_add(1, std::memory_order_seq_cst);
    wake.notify_one();
    back_thread.join();
    active = false;
    // The split is static and outlives the emulation session. The next session's back thread
    // starts reading at 0: with the old positions kept it skipped through the new, empty ring a
    // wrap at a time and never reached the old write position (the next game's first GPU work
    // never ran, it waited on its syncpoint forever), and the engines it held were freed.
    ring.reset();
    write_position = 0;
    published_local = 0;
    consumed_cache = 0;
    command_writes.Reset();
    completion.Reset();
    publish_every = PublishEvery;
    front_engine = nullptr;
    back_engine = nullptr;
    front_macros = false;
    published.store(0, std::memory_order_relaxed);
    consumed.store(0, std::memory_order_relaxed);
    back_waiting.store(false, std::memory_order_relaxed);
}

u64* GpuSplit::ReserveSlow(size_t words) {
    size_t index = write_position & RingMask;
    if (index + words > RingWords) {
        // Entries never wrap: skip to the start of the ring.
        const size_t rest = RingWords - index;
        WaitForSpace(rest);
        ring[index] = static_cast<u64>(Kind::Wrap);
        write_position += rest;
        index = 0;
    }
    WaitForSpace(words);
    return &ring[index];
}

void GpuSplit::WaitForSpace(size_t words) {
    if (write_position + words - consumed_cache <= RingWords) {
        return;
    }
    Publish();
    DrainProfileScope attribution{SplitDrainReason::RingSpace, write_position - consumed_cache};
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (write_position + words - consumed_cache <= RingWords) {
            return;
        }
        if (spins < 4096) {
            Pause();
        } else {
            std::this_thread::yield();
        }
    }
}

void GpuSplit::Publish() {
    if (published_local == write_position) {
        return;
    }
    published_local = write_position;
    publish_every = std::min<size_t>(publish_every * 2, PublishEvery);
    published.store(write_position, std::memory_order_seq_cst);
    if (back_waiting.load(std::memory_order_seq_cst)) {
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }
}

void GpuSplit::Drain(SplitDrainReason reason) {
    DrainTo(write_position, reason);
}

void GpuSplit::DrainCommandReads() {
    DrainMemoryWriters(SplitDrainReason::CommandFetch);
}

void GpuSplit::DrainMemoryWriters(SplitDrainReason reason) {
    if (command_writes.Target() < write_position) {
        Nx2Profile::Add(Nx2Profile::Counter::CommandStateTails, 1);
    }
    DrainTo(command_writes.Target(), reason);
    // Do not starve state-only runs when the reader never needs a full drain.
    Publish();
}

void GpuSplit::DrainTo(u64 target, SplitDrainReason reason) {
    consumed_cache = consumed.load(std::memory_order_acquire);
    DrainProfileScope attribution{reason, target > consumed_cache ? target - consumed_cache : 0};
    if (consumed_cache >= target) {
        return;
    }
    // A frontier can be in the middle of an already published batch. Ask the
    // back to acknowledge it without waiting for the unrelated tail or 4096
    // more words. Publish remains the release of the ring's actual contents.
    completion.Request(target);
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache >= target) {
            publish_every = 8;
            return;
        }
        if (spins < 16384) {
            Pause();
        } else {
            std::this_thread::yield();
        }
    }
}

CommandFetchSyncAudit& GpuSplit::FetchSyncAudit() {
    return drain_profile.audit;
}

void GpuSplit::NoteCommandFetch(u64 bytes, bool historical_hit, bool flushed) {
    if (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) && !Nx2Profile::Enabled()) return;
    drain_profile.metrics.RecordCommandFetch(bytes, historical_hit, flushed);
    ReportDrainProfile();
}

void GpuSplit::SwitchEngine(Tegra::Engines::EngineInterface* engine) {
    front_engine = engine;
    u64* const entry = Reserve(2);
    entry[0] = static_cast<u64>(Kind::Engine);
    entry[1] = reinterpret_cast<u64>(engine);
    Commit(2, false);
}

void GpuSplit::MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                           u32 amount, u32 methods_pending, const GPUVAddr* segment,
                           bool may_write) {
    SelectEngine(engine);
    // Long runs (constant buffer and inline uploads) go in pieces, as when a method's data
    // continues in the next GPFIFO entry.
    while (amount > 0) {
        const u32 piece = std::min(amount, MaxMultiWords);
        const size_t head = segment != nullptr ? 3 : 2;
        const size_t size = head + (piece + 1) / 2;
        u64* const entry = Reserve(size);
        entry[0] = static_cast<u64>(Kind::Multi) | (segment != nullptr ? HasSegment : 0) |
                   (u64{method & 0xffff} << 8) | (u64{piece} << 32);
        entry[1] = methods_pending;
        if (segment != nullptr) {
            entry[2] = *segment;
        }
        entry[size - 1] = 0;
        std::memcpy(entry + head, words, size_t{piece} * sizeof(u32));
        Commit(size, may_write);
        words += piece;
        amount -= piece;
        methods_pending -= piece;
    }
}

void GpuSplit::DeferFunction(std::function<void()>&& func) {
    auto* const heap = new std::function<void()>(std::move(func));
    Defer([heap] {
        (*heap)();
        delete heap;
    });
}

size_t GpuSplit::Execute(const u64* entry) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        if ((head & Direct) != 0) {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            engine->CallMethod(*system, method, argument, true);
        } else if (!engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, argument);
        } else {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
        }
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        if ((head & HasSegment) != 0) {
            engine->current_dma_segment = entry[2];
        }
        engine->CallMultiMethod(*system, method, reinterpret_cast<const u32*>(entry + data),
                                amount, static_cast<u32>(entry[1]));
        return data + (amount + 1) / 2;
    }
    case Kind::Call: {
        const size_t payload = static_cast<size_t>((head >> 8) & 0xffffff);
        reinterpret_cast<void (*)(const u64*)>(entry[1])(entry + 2);
        return 2 + payload;
    }
    }
    return 1;
}

void GpuSplit::BackLoop(std::stop_token stop_token) {
    back_thread_context = true;
    Common::SetCurrentThreadName("GPUBack");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    system->RegisterHostThread();
#ifdef _WIN32
    g_back_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
#endif
    u64 read_position = 0;
    while (!stop_token.stop_requested()) {
        Nx2Profile::Report("GPU back thread");
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            const u64 idle_start = Nx2Profile::Now();
            const auto note_idle = [idle_start] {
                AddBackActivity(g_back_activity.idle_ns, Nx2Profile::Now() - idle_start);
            };
            // The front usually sends more within microseconds: spin briefly before sleeping.
            for (u32 spins = 0; spins < 8192 && available == read_position; ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once.
                back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = wake.load(std::memory_order_seq_cst);
                available = published.load(std::memory_order_seq_cst);
                if (available == read_position && !stop_token.stop_requested()) {
                    AddBackActivity(g_back_activity.sleeps, 1);
                    wake.wait(seen, std::memory_order_seq_cst);
                }
                back_waiting.store(false, std::memory_order_relaxed);
                note_idle();
                continue;
            }
            note_idle();
        }
        AddBackActivity(g_back_activity.batches, 1);
        u64 last_store = read_position;
        g_back_busy.store(true, std::memory_order_relaxed);
        while (read_position != available) {
            const size_t index = read_position & RingMask;
            const size_t size = Execute(&ring[index]);
            read_position += size != 0 ? size : RingWords - index;
            // Hand ring space back now and then while working through a long batch.
            if (read_position != available && completion.ShouldPublish(read_position, last_store)) {
                consumed.store(read_position, std::memory_order_release);
                last_store = read_position;
            }
        }
        g_back_busy.store(false, std::memory_order_release);
        consumed.store(read_position, std::memory_order_release);
    }
}

} // namespace VideoCommon
