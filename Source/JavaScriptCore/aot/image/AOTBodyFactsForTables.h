/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTImageForward.h"
#include "AOTVector.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;
class LinkedFunctions;

namespace Typed {
class Plan;
class Program;
}

// A call in tail position in a plain function that may be entered by value. Both functions are as they are written, and there is no callee where the call is by value.
// Or no call: where such a function is listed while `this` is made for it.
struct WrittenTailCall {
    FunctionID function;
    FunctionID callee;
    uint32_t position { 0 };
    bool isWhereThisIsMade { false };
};

// What the tables say of the bodies as they are written. All by FunctionID of the program.
struct BodyFactsForTables {
    BitVector functionsThatOnlyReturn;
    // Those that have code, and those that were inlined into it.
    BitVector functionsWithFrames;
    // Ascending by function. Where `this` is made comes first, then the calls by position, no two at one place.
    Vector<WrittenTailCall> placesOfTailCalls;
};

// One look at each body that has a frame, in parallel by function. The driver calls it behind Compile, and gives the bodies back to their owner behind it.
BodyFactsForTables collectBodyFactsForTables(const Typed::Program&, const Typed::Plan&, const LinkedFunctions&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
