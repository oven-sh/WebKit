/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// The substrings that split returns for a separator that is never empty, found one after the other in the string itself.
struct SplitString {
    ValueID string;
    ValueID separator;
    BuiltinOverloadID separatorSearch;
};

std::optional<SplitString> splitStringOf(FunctionPassContext&, const Value& call);

// A position is where the next substring begins. After the last substring it is past the end of the string.
Range positionRangeOfSplitString();

struct SplitStringStep {
    BlockID done;
    ValueID start;
    ValueID end;
    ValueID next;
};

// Leaves the builder where the substring from start to end was found. The block for a position after the last substring is left open.
SplitStringStep appendSplitStringStep(IRBuilder&, const SplitString&, ValueID position);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
