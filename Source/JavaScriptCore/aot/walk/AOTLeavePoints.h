/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

// AT WHICH POINTS THE SLOW VERSION OF A FUNCTION IS ENTERED. A unit that builds a way out of a fast version notes the point, and the slow version has an entry for each point that was noted.
// A point is a number within its function, which follows from the body alone. This is a set and never an order: whichever thread notes what first, of() answers the same.
//
// WHO MAY NOTE. A unit of G notes a point of F only where LinkTables::leadsTo(G, F). Then UnitQueue makes the SlowBody of F behind that unit: driver/AOTCompileQueue.h, SLOW VERSIONS LAST.
// The queue closes the points of a function when nobody is left who may note one. One kind of unit comes later still: a unit of the cycle of F that a slow version of that cycle was the first to mention.
// It finds the points closed and leaves only at those that are there. Which units those are follows from the mentions alone, so no byte hangs on the threads.
class LeavePoints {
    WTF_MAKE_NONCOPYABLE(LeavePoints);
public:
    LeavePoints() = default;

    // By one thread, while nobody else asks. Behind it nothing is noted and nothing is closed.
    void prepare(size_t numberOfFunctions)
    {
        m_isClosed = Vector<std::atomic<uint32_t>>((numberOfFunctions + bitsInWord - 1) / bitsInWord);
        for (std::atomic<uint32_t>& word : m_isClosed)
            word.store(0, std::memory_order_relaxed);
        for (Part& part : m_parts) {
            Locker locker { part.lock };
            part.pointsOfFunctions.clear();
        }
    }

    // All of these by any thread.
    bool isClosed(FunctionID function) const { return m_isClosed[function.index() / bitsInWord].load(std::memory_order_acquire) >> (function.index() % bitsInWord) & 1; }
    // Asked before a way out is built. True: the slow version has the point once it is noted.
    bool mayHave(FunctionID function, uint32_t number) const { return !isClosed(function) || std::ranges::binary_search(of(function), number); }
    // mayHave() holds.
    void note(FunctionID function, uint32_t number)
    {
        if (isClosed(function)) {
            RELEASE_ASSERT(std::ranges::binary_search(of(function), number));
            return;
        }
        Part& part = partOf(function);
        Locker locker { part.lock };
        part.pointsOfFunctions.add(function.index(), Vector<uint32_t>()).iterator->value.append(number);
    }
    void close(FunctionID function)
    {
        {
            Part& part = partOf(function);
            Locker locker { part.lock };
            if (auto found = part.pointsOfFunctions.find(function.index()); found != part.pointsOfFunctions.end()) {
                Vector<uint32_t>& points = found->value;
                std::ranges::sort(points);
                points.shrink(std::ranges::unique(points).begin() - points.begin());
            }
        }
        m_isClosed[function.index() / bitsInWord].fetch_or(1u << (function.index() % bitsInWord), std::memory_order_release);
    }
    // Behind close(). Ascending, each once.
    std::span<const uint32_t> of(FunctionID function) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(isClosed(function));
        const Part& part = partOf(function);
        Locker locker { part.lock };
        auto found = part.pointsOfFunctions.find(function.index());
        // The table may grow beside this and move the list, never what the list holds.
        return found == part.pointsOfFunctions.end() ? std::span<const uint32_t>() : found->value.span();
    }

private:
    static constexpr uint32_t bitsInWord = 32;
    static constexpr size_t numberOfParts = 64;
    struct Part {
        mutable Lock lock;
        HashMap<uint32_t, Vector<uint32_t>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> pointsOfFunctions WTF_GUARDED_BY_LOCK(lock);
    };
    Part& partOf(FunctionID function) LIFETIME_BOUND { return m_parts[function.index() % numberOfParts]; }
    const Part& partOf(FunctionID function) const LIFETIME_BOUND { return m_parts[function.index() % numberOfParts]; }

    std::array<Part, numberOfParts> m_parts;
    // [function / 32]
    Vector<std::atomic<uint32_t>> m_isClosed;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
