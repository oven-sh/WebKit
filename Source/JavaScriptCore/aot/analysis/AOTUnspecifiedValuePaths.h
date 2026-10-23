/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <optional>
#include <wtf/Vector.h>

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

// An Unspecified value travels beside an integer that says whether it is there. This follows every path through the function, takes at a branch or a switch
// on a known integer only the edge that the integer takes, and lists the operations that look at such a value.
struct UsesOfUnspecifiedValues {
    Vector<UseOfUnspecifiedValue> uses;
    bool wasEveryPathFollowed { true };
};

UsesOfUnspecifiedValues findUsesOfUnspecifiedValues(FunctionAnalyses&, size_t maxUses);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
