/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

struct UseOfUnspecifiedValue {
    BlockID block;
    // Null for the terminator of the block.
    ValueID user;
    ValueID operand;
    // From the first block of the function to the block of the user.
    Vector<BlockID> path;
};

// An Unspecified value travels beside an integer that says whether it is there. This follows the value from every edge that passes it, takes at a branch or a switch
// on known integers only the edges that they take, and lists the operations that look at such a value.
struct UsesOfUnspecifiedValues {
    Vector<UseOfUnspecifiedValue> uses;
    bool wasEveryPathFollowed { true };
};

// The integer may be tested by an outlined body. Where that body may change under the reader, a function that calls one is not judged.
enum class OtherFunctions : bool { AreAtRest, MayChange };

UsesOfUnspecifiedValues findUsesOfUnspecifiedValues(FunctionAnalyses&, size_t maxUses, OtherFunctions, size_t maxBlockVisits = 1 << 22);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
