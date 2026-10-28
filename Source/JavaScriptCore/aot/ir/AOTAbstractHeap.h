/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include <array>
#include <bit>
#include <utility>
#include <wtf/EnumSet.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

using AbstractHeapSet = EnumSet<AbstractHeap>;

inline constexpr unsigned numberOfHeaps = numberOfEnumerators<AbstractHeap>;

static_assert(numberOfHeaps > 0 && numberOfHeaps <= 64);

constexpr AbstractHeapSet heapsMatching(bool (*matches)(AbstractHeap))
{
    AbstractHeapSet result;
    for (unsigned index = 0; index < numberOfHeaps; ++index) {
        auto heap = static_cast<AbstractHeap>(index);
        if (matches(heap))
            result.add(heap);
    }
    return result;
}

inline constexpr AbstractHeapSet allHeaps = heapsMatching([](AbstractHeap) { return true; });
inline constexpr AbstractHeapSet observableHeaps = heapsMatching([](AbstractHeap heap) { return isObservable(heap); });
inline constexpr AbstractHeapSet orderedHeaps = heapsMatching([](AbstractHeap heap) { return isOrdered(heap); });
inline constexpr AbstractHeapSet heapsHoldingValues = heapsMatching([](AbstractHeap heap) { return holdsValues(heap); });
inline constexpr AbstractHeapSet perObjectHeaps = heapsMatching([](AbstractHeap heap) { return isPerObject(heap); });
inline constexpr AbstractHeapSet indexedHeaps = heapsMatching([](AbstractHeap heap) { return numberOfIndexesOf(heap) > 0; });

inline constexpr AbstractHeapSet bookkeepingHeaps { AbstractHeap::AllocatorState, AbstractHeap::RuntimeState };
// A guard ends because of what is done to an object. Where that is left out, the guard goes on telling the truth.
inline constexpr AbstractHeapSet heapsWhoseWritesNobodySees = bookkeepingHeaps | AbstractHeapSet { AbstractHeap::StringStorage, AbstractHeap::CollectionStorage, AbstractHeap::MemberGuards };
inline constexpr AbstractHeapSet compiledCodeHeaps { AbstractHeap::ModuleVariables, AbstractHeap::EnvironmentFields };

inline constexpr unsigned numberOfIndexedHeaps = static_cast<unsigned>(indexedHeaps.size());

static_assert(!bookkeepingHeaps.containsAny(observableHeaps));
static_assert(indexedHeaps.containsAll(compiledCodeHeaps));
static_assert(numberOfIndexedHeaps > 0);

constexpr unsigned maskIndexOf(AbstractHeap heap)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(indexedHeaps.contains(heap));
    uint64_t lowerHeaps = (uint64_t { 1 } << std::to_underlying(heap)) - 1;
    return static_cast<unsigned>(std::popcount(static_cast<uint64_t>(indexedHeaps.toRaw()) & lowerHeaps));
}

inline constexpr std::array<AbstractHeap, numberOfIndexedHeaps> indexedHeapsInOrder = [] {
    std::array<AbstractHeap, numberOfIndexedHeaps> heaps { };
    for (unsigned index = 0; index < numberOfHeaps; ++index) {
        auto heap = static_cast<AbstractHeap>(index);
        if (indexedHeaps.contains(heap))
            heaps[maskIndexOf(heap)] = heap;
    }
    return heaps;
}();

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
