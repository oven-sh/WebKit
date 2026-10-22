/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMemoryLocation.h"
#include "AOTNaturalLoops.h"
#include "AOTPass.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// The blocks that stay in a loop when `leaves` has left it and that may write a variable which every turn reads, other than by a store of the loop's own.
struct BlocksWritingLoopVariables {
    BitVector blocks;
    // There is such a variable, and the loop still turns.
    bool canCarryVariablesWithoutThem { false };
};

BlocksWritingLoopVariables findBlocksWritingLoopVariables(FunctionPassContext&, const NaturalLoop&, const BitVector& leaves);

struct VariableOfLoop {
    BlockID header;
    ValueID parameter;
    MemoryLocation location;
    ValueID base;
};

// For a loop that those blocks have left and that is entered from its copy, behind a whole turn: so what every turn reads has been read when it is read ahead of the loop.
bool carryVariablesAroundSeparatedLoop(FunctionPassContext&, BlockID header, Vector<VariableOfLoop>& variablesWithStores);

// For loops that have their final form, with the types they have in it.
bool delayStoresOfLoopVariables(FunctionPassContext&, std::span<const VariableOfLoop>);

// In every loop that no path has to leave for it: the variables that the first turn reads before the loop can end.
PassOutcome carryVariablesAroundLoops(FunctionPassContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
