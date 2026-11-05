/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayout.h"
#include "AOTID.h"
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;
class ThinSummaries;

// `function make() { start(); const limit = read(); return run; function start() { .. } function run(n) { return n > limit; } }`: FROM WHERE EACH CLOSURE OVER A SCOPE MAY BE ENTERED. A PROOF: the walk leaves out the check of a slot's dead zone by it.
// The scan says by itself that nobody sees a slot empty whose first store stands above every point at which THE SCOPE leaves (AOTScanEnvironmentsInlines.h). Here `start` takes the scope out in front of the store, and `run` reads the slot.
// Nobody reads a slot of a scope but the body that made it and the functions that capture it. Such a function runs once somebody calls it by binding or has a closure of it, and a closure comes into other hands than the body's
// only where the body uses it for more than to be captured or laid into a slot of the scope, or where a function runs that has it: that captures it, reads the slot in which it lies, or makes it.
// So a function over the scope runs behind one of the points at which the body lets go of a thing that leads to it (SummaryOrderOfScope::entries, holds, ThinSummary::functionsNamed, and the slots that its summary reads).
// A store that stands above all those points has run. The scope of each turn of a loop, and of each run of the body, is another object: all this is said of one of them.
struct OrderOfScopes {
    // Whenever the function runs, the slot of the scope that it has captured, or made, has been stored into wherever it reads the slot.
    bool isFilledWhenever(FunctionID runs, FieldReference) const;

    // Blocks by their numbers in the order of dominators of the body that makes the scope. lastBlockBelow == SummaryOrderOfScope::aboveAll: nobody sees the slot empty, the body itself included.
    struct Store {
        uint32_t block { 0 };
        uint32_t lastBlockBelow { 0 };
        uint32_t position { 0 };
    };
    // Of all the points that lead to the function: the one in front in that order, and the number of the last block.
    struct Entries {
        uint32_t firstBlock { 0 };
        uint32_t positionInFirstBlock { 0 };
        uint32_t lastBlock { 0 };
    };
    using Traits = WTF::UnsignedWithZeroKeyHashTraits<uint64_t>;
    // By layout and field.
    HashMap<uint64_t, Store, DefaultHash<uint64_t>, Traits> stores;
    // By function and layout. A function that is not listed: nothing is said of when it runs.
    HashMap<uint64_t, Entries, DefaultHash<uint64_t>, Traits> entries;
};

OrderOfScopes findOrderOfScopes(const Program&, const ThinSummaries&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
