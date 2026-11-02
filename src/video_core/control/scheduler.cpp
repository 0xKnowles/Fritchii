// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <memory>
#include <vector>

#include "common/assert.h"
#include "core/core.h"
#include "video_core/control/channel_state.h"
#include "video_core/control/scheduler.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"

namespace Tegra::Control {

void Scheduler::BindChannel(GPU& gpu, s32 bind_id) {
    // With the GPU thread split the rasterizer's channel switches with the methods around it, on
    // the back thread (deferred outside the lock: the back takes it to bind).
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer([this, gpu_ptr = &gpu, bind_id] {
            std::scoped_lock lk{scheduling_guard};
            gpu_ptr->BindChannel(bind_id);
        });
        return;
    }
    std::scoped_lock lk{scheduling_guard};
    gpu.BindChannel(bind_id);
}

void Scheduler::Push(GPU& gpu, s32 channel, CommandList&& entries) {
    std::shared_ptr<ChannelState> channel_state;
    {
        std::unique_lock lk(scheduling_guard);
        auto it = channels.find(channel);
        ASSERT(it != channels.end());
        channel_state = it->second;
    }
    BindChannel(gpu, channel_state->bind_id);
    // Process commands outside the lock to reduce contention.
    // Multiple channels can prepare their commands in parallel.
    channel_state->payload->dma_pusher.Push(std::move(entries));
    channel_state->payload->dma_pusher.DispatchCalls();
    if (channel_state->payload->dma_pusher.HasPendingAcquire() &&
        std::find(pending_channels.begin(), pending_channels.end(), channel_state) ==
            pending_channels.end()) {
        pending_channels.push_back(channel_state);
    }
    if (!pending_channels.empty()) {
        ResumePending(gpu);
    }
}

bool Scheduler::ResumePending(GPU& gpu) {
    bool dispatched = false;
    // Give each pending channel one turn, then return to the GPU thread's command queue.
    // A ready acquire does not guarantee that DispatchCalls can advance (e.g. IB disabled).
    // Retrying until "no progress" using readiness alone can spin forever and starve the CPU
    // readback/tick/stop commands needed to make actual progress. The GPU thread polls this
    // list again, including channels released by a later channel in this pass.
    for (const auto& state : pending_channels) {
        auto& dma_pusher = state->payload->dma_pusher;
        if (!dma_pusher.system.IsPoweredOn()) {
            return dispatched;
        }
        if (!dma_pusher.HasPendingAcquire() || !dma_pusher.CanDispatch() ||
            !dma_pusher.CanResumeAcquire()) {
            continue;
        }
        gpu.InvalidateReadbackCache();
        BindChannel(gpu, state->bind_id);
        dma_pusher.DispatchCalls();
        dispatched = true;
    }
    std::erase_if(pending_channels, [](const auto& state) {
        return !state->payload->dma_pusher.HasPendingAcquire();
    });
    return dispatched;
}

void Scheduler::DeclareChannel(std::shared_ptr<ChannelState> new_channel) {
    s32 channel = new_channel->bind_id;
    std::unique_lock lk(scheduling_guard);
    channels.emplace(channel, new_channel);
}

} // namespace Tegra::Control
