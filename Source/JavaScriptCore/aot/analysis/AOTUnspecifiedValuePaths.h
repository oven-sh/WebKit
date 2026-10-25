/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

struct UseOfUnspecifiedValue {
    BlockID block;
    ValueID user;
    ValueID operand;
    Vector<BlockID> path;
};

struct UsesOfUnspecifiedValues {
    Vector<UseOfUnspecifiedValue> uses;
    bool wasEveryPathFollowed { true };
};

enum class OtherFunctions : bool { AreAtRest, MayChange };

// The operations in `countedAsLooking` only keep their operands. A pass that wants to test those operands there asks as if the operations looked at them.
UsesOfUnspecifiedValues findUsesOfUnspecifiedValues(FunctionAnalyses&, size_t maxUses, OtherFunctions, size_t maxBlockVisits = 1 << 22, const BitVector* countedAsLooking = nullptr);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
