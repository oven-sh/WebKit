/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTType.h"
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;

inline constexpr Effects effectsOfAnything { };
inline constexpr Effects effectsOfUnknownCode = Effects::unknownCode();

class DistinctEffects {
    WTF_MAKE_NONCOPYABLE(DistinctEffects);
public:
    DistinctEffects() = default;
    DistinctEffects(DistinctEffects&&) = default;
    DistinctEffects& operator=(DistinctEffects&&) = default;

    const Effects* keep(const Effects&);
    Vector<Vector<Effects>> takeBlocks();

private:
    static constexpr size_t effectsInBlock = 64;
    static constexpr size_t fewestPlaces = 64;

    const Effects*& placeOf(const Effects&);
    void makeRoomInPlaces();

    Vector<Vector<Effects>> m_blocks;
    Vector<const Effects*> m_places;
    size_t m_numberKept { 0 };
};

inline constexpr uint64_t objectsOfAnyLayout = UINT64_MAX;
uint64_t bitOfLayoutInKnownEffects(const Program&, LayoutID);
Vector<uint64_t> bitsOfLayoutsInKnownEffects(const Program&);
inline constexpr AbstractHeapSet heapsOfObjectsWithLayouts {
    AbstractHeap::ObjectFields, AbstractHeap::NamedProperties, AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::PrototypeLinks
};
inline constexpr AbstractHeapSet heapsOfPartsOfObjects = perObjectHeaps - unobservableWriteHeaps - AbstractHeapSet { AbstractHeap::EnvironmentFields };

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
    Vector<Vector<Effects>> blocksOfDistinctEffects;

    uint64_t layoutsWrittenBy(FunctionID) const;
    const LinkedEffectsGivenArguments* findEffectsGivenArguments(FunctionID) const;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
