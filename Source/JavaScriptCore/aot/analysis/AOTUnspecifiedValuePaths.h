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
    ValueID user;
    ValueID operand;
    Vector<BlockID> path;
};

struct UsesOfUnspecifiedValues {
    Vector<UseOfUnspecifiedValue> uses;
    bool wasEveryPathFollowed { true };
};

enum class OtherFunctions : bool { AreAtRest, MayChange };

UsesOfUnspecifiedValues findUsesOfUnspecifiedValues(FunctionAnalyses&, size_t maxUses, OtherFunctions, size_t maxBlockVisits = 1 << 22);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
