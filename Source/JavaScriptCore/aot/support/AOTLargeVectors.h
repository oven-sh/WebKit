/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

// vector.reserveCapacity(capacity). What the vector holds moves to pages that are there.
template<typename T, size_t inlineCapacity>
void reserveCapacityInParallel(const ParallelLoops& loops, Vector<T, inlineCapacity>& vector, size_t capacity)
{
    if (capacity <= vector.capacity())
        return;
    auto larger = createVectorWithCapacityInParallel<T, inlineCapacity>(loops, capacity);
    larger.appendVector(WTF::move(vector));
    vector = WTF::move(larger);
}

// vector.grow(size).
template<typename T, size_t inlineCapacity>
void growInParallel(const ParallelLoops& loops, Vector<T, inlineCapacity>& vector, size_t size)
{
    reserveCapacityInParallel(loops, vector, size);
    vector.grow(size);
}

// Room for `size` elements on pages that are there, and for a quarter more on pages that nobody has touched.
template<typename T, size_t inlineCapacity>
void reserveRoomToSpareInParallel(const ParallelLoops& loops, Vector<T, inlineCapacity>& vector, size_t size)
{
    if (size <= vector.capacity())
        return;
    Vector<T, inlineCapacity> larger;
    larger.reserveInitialCapacity(size + size / 4);
#if !ASAN_ENABLED
    takePageFaultsInParallel(loops, unsafeMakeSpan(reinterpret_cast<uint8_t*>(larger.mutableSpan().data()), size * sizeof(T)));
#else
    UNUSED_PARAM(loops);
#endif
    larger.appendVector(WTF::move(vector));
    vector = WTF::move(larger);
}

// vector.grow(size), for a list with an entry for each function that stays from one step of the build to the next. A program gets more functions at every loop and at many passes,
// and a list that grows moves all that it holds, on one thread: so it takes a quarter more room than it needs.
template<typename T, size_t inlineCapacity>
void growWithRoomToSpareInParallel(const ParallelLoops& loops, Vector<T, inlineCapacity>& vector, size_t size)
{
    reserveRoomToSpareInParallel(loops, vector, size);
    vector.grow(size);
}

// Vector<T>(size).
template<typename T>
Vector<T> createVectorInParallel(const ParallelLoops& loops, size_t size)
{
    Vector<T> vector;
    growInParallel(loops, vector, size);
    return vector;
}

// Vector<T>(FillWith { }, size, value).
template<typename T>
Vector<T> createVectorInParallel(const ParallelLoops& loops, size_t size, const T& value)
{
    Vector<T> vector;
    reserveCapacityInParallel(loops, vector, size);
    vector.fill(value, size);
    return vector;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
