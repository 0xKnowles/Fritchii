// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include "common/common_types.h"

namespace Core::Memory {

// Every write must invalidate overlapping pending fence copies before it is
// collected. A previous write to the same page protects only its own bytes;
// even repeating those bytes needs protection if another fence was queued.
template <typename Protect, typename Collect>
void RecordRasterizerWrite(DAddr address, size_t size, Protect&& protect, Collect&& collect) {
    if (address != 0 && size != 0 && protect(address, size)) {
        collect(address, size);
    }
}

} // namespace Core::Memory
