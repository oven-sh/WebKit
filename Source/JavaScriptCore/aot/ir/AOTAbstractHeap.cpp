/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAbstractHeap.h"

#if ENABLE(AOT)

#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr bool hasDenseMaskIndexes()
{
    unsigned nextMaskIndex = 0;
    for (unsigned index = 0; index < numberOfHeaps; ++index) {
        auto heap = static_cast<AbstractHeap>(index);
        if (!indexedHeaps.contains(heap))
            continue;
        if (maskIndexOf(heap) != nextMaskIndex)
            return false;
        ++nextMaskIndex;
    }
    return nextMaskIndex == numberOfIndexedHeaps;
}

static_assert(sizeof(AbstractHeapSet) == 8);
static_assert(allHeaps.size() == numberOfHeaps);

static_assert(indexedHeaps == AbstractHeapSet {
    AbstractHeap::ModuleVariables,
    AbstractHeap::EnvironmentFields,
    AbstractHeap::ObjectFields,
    AbstractHeap::ShapeWords,
    AbstractHeap::MemberGuards,
    AbstractHeap::NamedProperties,
    AbstractHeap::Elements,
});
static_assert(numberOfIndexesOf(AbstractHeap::EnvironmentFields) == 2);
static_assert(heapsMatching([](AbstractHeap heap) { return numberOfIndexesOf(heap) == 1; }) == indexedHeaps - AbstractHeapSet { AbstractHeap::EnvironmentFields });
static_assert(hasDenseMaskIndexes());

static_assert(perObjectHeaps.containsAll({
    AbstractHeap::EnvironmentFields,
    AbstractHeap::ObjectFields,
    AbstractHeap::ShapeWords,
    AbstractHeap::OrderWords,
    AbstractHeap::NamedProperties,
    AbstractHeap::Elements,
    AbstractHeap::ArrayStorage,
    AbstractHeap::CollectionStorage,
    AbstractHeap::CollectionEntries,
    AbstractHeap::PromiseState,
    AbstractHeap::InternalFields,
    AbstractHeap::PrototypeLinks,
}));
static_assert(!perObjectHeaps.containsAny({
    AbstractHeap::ModuleVariables,
    AbstractHeap::MemberGuards,
    AbstractHeap::AllocatorState,
    AbstractHeap::RuntimeState,
    AbstractHeap::ExternalState,
}));
static_assert(!perObjectHeaps.containsAny({ AbstractHeap::TypedArrayStorage, AbstractHeap::TypedArrayElements }), "views of one ArrayBuffer are different objects that share its bytes and its length");

static_assert(!orderedHeaps.contains(AbstractHeap::AllocatorState));
static_assert(orderedHeaps.contains(AbstractHeap::RuntimeState));

static_assert(heapsHoldingValues.containsAll(compiledCodeHeaps));
static_assert(!heapsHoldingValues.containsAny({
    AbstractHeap::ShapeWords,
    AbstractHeap::OrderWords,
    AbstractHeap::ArrayStorage,
    AbstractHeap::TypedArrayStorage,
    AbstractHeap::TypedArrayElements,
    AbstractHeap::CollectionStorage,
}));

} // anonymous namespace

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
