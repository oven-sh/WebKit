/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreads.h"
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename T>
void reserveLargeListInLinkFlow(const ParallelLoops& loops, Vector<T>& list, size_t size, size_t room)
{
    RELEASE_ASSERT(list.isEmpty() && room >= size);
    list.reserveInitialCapacity(room);
#if !ASAN_ENABLED
    constexpr size_t bytesPerTask = 256 * KB;
    std::span<uint8_t> bytes = unsafeMakeSpan(reinterpret_cast<uint8_t*>(list.mutableSpan().data()), size * sizeof(T));
    forEachIndexInParallel(loops, static_cast<unsigned>((bytes.size() + bytesPerTask - 1) / bytesPerTask), [&](unsigned task) {
        zeroSpan(bytes.subspan(task * bytesPerTask, std::min(bytesPerTask, bytes.size() - task * bytesPerTask)));
    });
#else
    UNUSED_PARAM(loops);
#endif
}

template<typename T>
void fillLargeListInLinkFlow(const ParallelLoops& loops, Vector<T>& list, const T& value, size_t size, size_t room)
{
    reserveLargeListInLinkFlow(loops, list, size, room);
    list.fill(value, size);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
