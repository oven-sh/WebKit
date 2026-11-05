/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTType.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;

inline constexpr uint64_t objectsOfAnyLayout = UINT64_MAX;
uint64_t bitOfLayoutInKnownEffects(const Program&, LayoutID);
Vector<uint64_t> bitsOfLayoutsInKnownEffects(const Program&);
inline constexpr AbstractHeapSet heapsOfObjectsWithLayouts {
    AbstractHeap::ObjectFields, AbstractHeap::NamedProperties, AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::PrototypeLinks
};
inline constexpr AbstractHeapSet heapsOfPartsOfObjects = perObjectHeaps - heapsWhoseWritesNobodySees - AbstractHeapSet { AbstractHeap::EnvironmentFields };

struct LinkedEffectsGivenArguments {
    FunctionID function;
    uint32_t parametersTakenAsExpected { 0 };
    uint32_t parametersCalled { 0 };
    uint64_t layoutsWritten { objectsOfAnyLayout };
    Effects onReturn;
    Effects onThrow;
};

struct LinkKnownEffects {
    Vector<uint64_t> layoutsWrittenByFunction;
    Vector<LinkedEffectsGivenArguments> givenArguments;

    uint64_t layoutsWrittenBy(FunctionID) const;
    const LinkedEffectsGivenArguments* findEffectsGivenArguments(FunctionID) const;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
