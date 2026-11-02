// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <type_traits>
#include <utility>

#include "common/common_types.h"

namespace VideoCommon {

// Bytes already selected for eviction, but still owned until GPU-safe destruction.
// This counter must outlive the retired resources referring to it.
class RetirementMemory {
public:
    RetirementMemory() = default;
    RetirementMemory(const RetirementMemory&) = delete;
    RetirementMemory& operator=(const RetirementMemory&) = delete;
    u64 PendingBytes() const noexcept { return pending; }
    u64 RemainingUsage(u64 measured_usage) const noexcept {
        return measured_usage - (std::min)(measured_usage, pending);
    }

private:
    template <class T> friend class RetiredResource;
    u64 pending{};
};

template <class T>
class RetiredResource {
public:
    RetiredResource(RetirementMemory& memory, u64 bytes, T&& resource_)
        : credit{memory, bytes}, resource{std::move(resource_)} {}
    RetiredResource(const RetiredResource&) = delete;
    RetiredResource& operator=(const RetiredResource&) = delete;
    RetiredResource(RetiredResource&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : resource{std::move(other.resource)} {
        // Transfer the credit only after a potentially throwing resource move succeeds.
        // A failed vector relocation must leave the original owner's credit intact.
        credit.memory = std::exchange(other.credit.memory, nullptr);
        credit.bytes = other.credit.bytes;
    }
    RetiredResource& operator=(RetiredResource&&) = delete;

private:
    struct Credit {
        Credit() = default;
        Credit(RetirementMemory& memory_, u64 bytes_) : memory{&memory_}, bytes{bytes_} {
            memory->pending += bytes;
        }
        Credit(Credit&& other) noexcept
            : memory{std::exchange(other.memory, nullptr)}, bytes{other.bytes} {}
        Credit(const Credit&) = delete;
        Credit& operator=(const Credit&) = delete;
        ~Credit() {
            if (memory) memory->pending -= bytes;
        }
        RetirementMemory* memory{};
        u64 bytes{};
    };

    // Reverse destruction order: the GPU resource is freed before its credit disappears.
    Credit credit;
    T resource;
};

} // namespace VideoCommon
