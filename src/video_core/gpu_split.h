// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>

#include "common/common_types.h"
#include "video_core/gpu_split_metrics.h"
#include "video_core/command_fetch_sync_audit.h"
#include "video_core/command_write_frontier.h"
#include "video_core/split_completion.h"
#include "video_core/split_publication.h"
#include "video_core/engines/engine_interface.h"

namespace Core {
class System;
}

namespace Tegra::Engines {
class EngineInterface;
}

namespace VideoCommon {

// Splits the GPU thread in two (NX2_GPU_THREAD_SPLIT.md). The GPU thread (the front) takes the
// CPU's commands, parses command lists and runs the puller; engine method calls and everything
// behind them (register state, macros, draws, the rasterizer and its caches) run in order on a
// second thread (the back), fed through a single-producer single-consumer ring. Work the front
// would do on back-owned state is deferred into the same ring; the front waits for the back to
// catch up (Drain) only where it needs a result from that state, e.g. a semaphore it acquires.
// On for Switch 2 titles with the 3D and compute engines' macros on the front, against copies of
// the registers they read (Maxwell3D and KeplerCompute Front*), which sends the writes they make.
// EDEN_NX2_SPLIT=1 keeps the macros on the back; EDEN_NX2_SPLIT=0 runs everything on one thread.
class GpuSplit {
public:
    static GpuSplit& Get() {
        return instance;
    }

    static bool IsBackThread() {
        return back_thread_context;
    }

    /// Whether the front sends work to the back (front thread only).
    [[nodiscard]] bool Active() const {
        return active;
    }
    /// Whether the front runs the engines' macros (EDEN_NX2_SPLIT=2).
    [[nodiscard]] bool FrontMacros() const {
        return front_macros;
    }

    /// Starts the back thread once the split is wanted (EDEN_NX2_SPLIT=1, a Switch 2 title,
    /// commands read without cache flushes). Called by the front between commands.
    void MaybeStart(Core::System& system);
    void Stop();

    /// An engine method write, executed as DmaPusher would (method sink, ConsumeSink, call).
    /// `segment`: the engine's current_dma_segment for the call, when the engine reads it.
    void Method(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument, bool is_last,
                const GPUVAddr* segment, bool may_write = true) {
        SelectEngine(engine);
        const size_t words = segment != nullptr ? 2 : 1;
        u64* const entry = Reserve(words);
        entry[0] = static_cast<u64>(Kind::Method) | (is_last ? IsLast : 0) |
                   (segment != nullptr ? HasSegment : 0) | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        if (segment != nullptr) {
            entry[1] = *segment;
        }
        Commit(words, may_write && engine->execution_mask[method]);
    }
    void MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                     u32 amount, u32 methods_pending, const GPUVAddr* segment,
                     bool may_write = true);
    /// A write made by a macro: CallMethod without the method sink, as the engines' macros do.
    // Only the owning front engine may prove a write inert, including ConsumeSink's
    // effects. Unknown callers retain the conservative default.
    void MacroWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument,
                    bool may_write = true) {
        SelectEngine(engine);
        u64* const entry = Reserve(1);
        entry[0] = static_cast<u64>(Kind::Method) | IsLast | Direct | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        Commit(1, may_write);
    }

    /// Runs `func` on the back after everything sent before it. Must be trivially copyable.
    template <typename Func>
    void Defer(const Func& func) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t payload = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        u64* const entry = Reserve(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        Commit(2 + payload);
    }
    void DeferFunction(std::function<void()>&& func);

    /// EDEN_NX2_SPLIT_CHECK=1 (debug): reports, with its stack, a thread other than the back
    /// using back-owned state (the Vulkan scheduler) while the back runs entries.
    static inline bool check_owner{};
    static void CheckOwner(const char* where);

    /// Makes what was sent visible to the back.
    void Publish();
    /// Waits until the back executed everything sent so far.
    void Drain(SplitDrainReason reason = SplitDrainReason::Other);
    /// Wait for possible command-memory writers; leave inert sink appends queued.
    void DrainCommandReads();
    /// Waits for every queued entry that may write guest memory or use the back's caches,
    /// rasterizer, query cache or Vulkan scheduler (the command-write frontier). Afterwards the
    /// back may still run proven register-only entries, so the caller may read guest memory and
    /// query/flush the buffer and query caches, but must not touch engine registers, method
    /// sinks or anything a deferred callback could use. Use Drain() for those.
    void DrainMemoryWriters(SplitDrainReason reason = SplitDrainReason::Other);
    /// Whether DrainMemoryWriters would currently have to wait (front thread only).
    [[nodiscard]] bool MemoryWritersPending() const {
        return active && command_writes.Target() > consumed.load(std::memory_order_acquire);
    }
    /// Profile the decision made after command-fetch synchronization, without changing it.
    void NoteCommandFetch(u64 bytes, bool historical_hit, bool flushed);
    /// Front-thread evidence counters for the GP-entry SYNC_WAIT contract (reporting only).
    CommandFetchSyncAudit& FetchSyncAudit();
    /// Writer hints made across split sessions (front thread only, reporting).
    [[nodiscard]] u64 PublicationHints() const {
        return publication_hints;
    }

private:
    enum class Kind : u64 {
        Wrap,   ///< The rest of the ring is unused: continue at its start
        Engine, ///< The engine the next methods go to: word 1
        Method, ///< Bits 8-23 method, 32-63 argument; flags IsLast, HasSegment (word 1)
        Multi,  ///< Bits 8-23 method, 32-63 amount; word 1 methods pending, [segment], words
        Call,   ///< Bits 8-31 payload size in words; word 1 thunk, then the payload
    };
    static constexpr u64 IsLast = 1 << 4;
    static constexpr u64 HasSegment = 1 << 5;
    static constexpr u64 Direct = 1 << 6; ///< Method: call it even if not executable (macros)

    static constexpr size_t RingWords = size_t{1} << 20; // 8 MiB
    static constexpr size_t RingMask = RingWords - 1;
    static constexpr u32 MaxMultiWords = 8192;
    static constexpr size_t PublishEvery = SplitPublication::MaximumInterval;

    template <typename Func>
    static void CallThunk(const u64* payload) {
        (*reinterpret_cast<const Func*>(payload))();
    }

    /// Room for an entry of `words` at the write position (never across the ring's end).
    u64* Reserve(size_t words) {
        const size_t index = write_position & RingMask;
        if (index + words <= RingWords && write_position + words - consumed_cache <= RingWords)
            [[likely]] {
            return &ring[index];
        }
        return ReserveSlow(words);
    }
    u64* ReserveSlow(size_t words);
    void Commit(size_t words, bool may_write = true) {
        write_position += words;
        command_writes.Commit(write_position, may_write);
        // A possible command-memory writer is what the next command fetch waits for: let a
        // spinning back start it now (split_publication.h). Order and contents are unchanged.
        switch (SplitPublication::AfterCommit(write_position, published_local, publish_every,
                                              may_write)) {
        case SplitPublication::Action::Full:
            Publish();
            break;
        case SplitPublication::Action::Hint:
            HintPublished();
            break;
        case SplitPublication::Action::None:
            break;
        }
    }
    /// Release-only publication of a committed writer; see split_publication.h for why a missed
    /// hint cannot lose work. Never updates published_local: the next full Publish still runs
    /// the wake handshake for this position.
    void HintPublished() {
        hinted_local = write_position;
        ++publication_hints;
        published.store(write_position, std::memory_order_release);
        if (back_waiting.load(std::memory_order_relaxed)) [[unlikely]] {
            Publish(); // Asleep (or about to be): a hint cannot wake it.
        }
    }
    void WaitForSpace(size_t words);
    void DrainTo(u64 target, SplitDrainReason reason);
    void SelectEngine(Tegra::Engines::EngineInterface* engine) {
        if (engine != front_engine) [[unlikely]] {
            SwitchEngine(engine);
        }
    }
    void SwitchEngine(Tegra::Engines::EngineInterface* engine);

    void BackLoop(std::stop_token stop_token);
    size_t Execute(const u64* entry);

    Core::System* system{};
    bool active{};
    bool front_macros{};
    std::unique_ptr<u64[]> ring;

    // Front side.
    u64 write_position{};
    u64 published_local{}; ///< Last full (seq_cst, wake-checked) publication
    u64 hinted_local{};    ///< Last release-only writer hint
    u64 publication_hints{};
    u64 consumed_cache{};
    CommandWriteFrontier command_writes;
    /// Words between publishes: small right after a drain, while the back has nothing to do.
    size_t publish_every{PublishEvery};
    Tegra::Engines::EngineInterface* front_engine{};

    // Back side.
    Tegra::Engines::EngineInterface* back_engine{};

    static GpuSplit instance;
    static inline thread_local bool back_thread_context{};

    alignas(64) std::atomic<u64> published{};
    alignas(64) std::atomic<u64> consumed{};
    SplitCompletion completion;
    alignas(64) std::atomic<bool> back_waiting{};
    std::atomic<u32> wake{};
    std::jthread back_thread;
};

} // namespace VideoCommon
