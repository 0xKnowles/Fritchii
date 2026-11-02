// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <vector>

#include "common/common_types.h"
#include "video_core/engines/engine_interface.h"

namespace Tegra::Engines {

// GPU thread split with macros on its front half (gpu_split.h, EDEN_NX2_SPLIT=2). An engine whose
// macros the front runs (Maxwell3D and KeplerCompute FrontCallMethod) owns, on the front, the
// macro code and bindings, its partial macro call and a mirror of the registers its macros read.
// While it does, its own (back) method path skips macro loads and binds. The split can stop and
// start again during a game (GpuSplit::MaybeStart follows the accuracy settings): when it stops,
// every such engine has to take that state back, or its unsplit path keeps skipping macro
// uploads and binds and its next split start reuses a stale register mirror.
//
// Engines register here when the front takes them over. GpuSplit::Stop returns them all
// (ReturnAll) after its back thread has been joined, so nothing else executes their methods.
// Engines unregister when they hand back or are destroyed (channel teardown).
//
// Threading: Register and ReturnAll run on the GPU (front) thread, or on the thread that joined
// it; engine destruction happens after the GPU thread is joined. The mutex only protects the
// list itself; returning an engine that is being destroyed concurrently is not supported.
class SplitFrontOwnership {
public:
    static void Register(EngineInterface* engine) {
        auto& registry = Get();
        std::scoped_lock<std::mutex> lock{registry.mutex};
        if (std::find(registry.owners.begin(), registry.owners.end(), engine) ==
            registry.owners.end()) {
            registry.owners.push_back(engine);
        }
    }

    static void Unregister(EngineInterface* engine) {
        auto& registry = Get();
        std::scoped_lock<std::mutex> lock{registry.mutex};
        std::erase(registry.owners, engine);
    }

    /// Hands every registered engine's front state back to it (EngineInterface::
    /// ReturnSplitFrontOwnership). Precondition: no thread executes their methods. For the
    /// queued work to be reflected in their registers first, drain the split before stopping.
    /// Returns how many engines were handed back.
    static size_t ReturnAll() {
        size_t returned = 0;
        while (EngineInterface* const engine = PopOne()) {
            engine->ReturnSplitFrontOwnership();
            ++returned;
        }
        return returned;
    }

    [[nodiscard]] static size_t Count() {
        auto& registry = Get();
        std::scoped_lock<std::mutex> lock{registry.mutex};
        return registry.owners.size();
    }

private:
    struct Registry {
        std::mutex mutex;
        std::vector<EngineInterface*> owners;
    };

    static Registry& Get() {
        // Never destroyed: an engine destroyed during process exit may still unregister.
        static Registry* const registry = new Registry;
        return *registry;
    }

    static EngineInterface* PopOne() {
        auto& registry = Get();
        std::scoped_lock<std::mutex> lock{registry.mutex};
        if (registry.owners.empty()) {
            return nullptr;
        }
        EngineInterface* const engine = registry.owners.back();
        registry.owners.pop_back();
        return engine;
    }
};

} // namespace Tegra::Engines
