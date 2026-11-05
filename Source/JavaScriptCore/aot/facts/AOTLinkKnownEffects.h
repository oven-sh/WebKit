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

// PROOFS in Facts::linked(). In Facts::held() and in conservativeLinkTables() every list is empty, which claims nothing.

inline constexpr uint64_t objectsOfAnyLayout = UINT64_MAX;
// One bit: Program::locationBitOf() of the layout at the top of the row of written bases (writtenBaseOf()), so a class and all below it share it. objectsOfAnyLayout: the program has no bit for it.
uint64_t bitOfLayoutInKnownEffects(const Program&, LayoutID);
// The same of every layout, each from the layout above it: for whoever asks for many, since the one above goes up the whole row.
Vector<uint64_t> bitsOfLayoutsInKnownEffects(const Program&);
// Where the properties of an object of a layout lie, its shape and its place below a prototype. Whoever writes there and cannot say in whose object writes in objectsOfAnyLayout.
inline constexpr AbstractHeapSet heapsOfObjectsWithLayouts {
    AbstractHeap::ObjectFields, AbstractHeap::NamedProperties, AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::PrototypeLinks
};
// Every part of an object that somebody can see, but for the fields of an environment. `class Stack extends Array`: an object with a layout has elements too.
inline constexpr AbstractHeapSet heapsOfPartsOfObjects = perObjectHeaps - heapsWhoseWritesNobodySees - AbstractHeapSet { AbstractHeap::EnvironmentFields };

// What a call of the function does where more is known of what it is handed than Link knows of all its callers together. A bit for each of the first 32 entry parameters.
struct LinkedEffectsGivenArguments {
    FunctionID function;
    // ASSUMED: what arrives there lies within LinkedParameter::expected of linked(). Whoever uses the record has proven that of his arguments.
    uint32_t parametersTakenAsExpected { 0 };
    // LEFT OUT: the calls of what arrives there. Whoever uses the record hands over closures of ONE function and adds what a call of that function does.
    uint32_t parametersCalled { 0 };
    uint64_t layoutsWritten { objectsOfAnyLayout };
    Effects onReturn;
    Effects onThrow;
};

struct LinkKnownEffects {
    // By function. Empty: objectsOfAnyLayout for each.
    Vector<uint64_t> layoutsWrittenByFunction;
    // Ascending by function, one at most for each: only where it says less than LinkedFunction does.
    Vector<LinkedEffectsGivenArguments> givenArguments;

    // Whatever the function and all that it may call write in a heap of perObjectHeaps, by whichever exit, they write in an object of a layout whose bit is set, in an object that the call itself has made,
    // or in an object of one of the engine's classes. It says something only beside effects that run no other code.
    uint64_t layoutsWrittenBy(FunctionID) const;
    // Null: LinkedFunction says all there is.
    const LinkedEffectsGivenArguments* findEffectsGivenArguments(FunctionID) const;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
