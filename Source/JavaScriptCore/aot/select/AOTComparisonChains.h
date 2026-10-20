/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ChainKind : uint8_t { Integers, Numbers, Strings };

struct ChainCase {
    int64_t number { 0 };
    StringID string;
    Edge edge;
};

struct ComparisonChain {
    BlockID head;
    ValueID subject;
    Vector<BlockID, 4> links;
    Vector<ValueID, 8> comparisons;
    Vector<ChainCase, 8> cases;
    Edge fallThrough;
};

struct ChainLimits {
    uint64_t minCases { 0 };
    uint64_t maxCases { 0 };
    LimitID maxCasesLimit { LimitID::None };
};

using ChainLinkFilter = ScopedLambda<bool(ValueID comparison)>;

Vector<ComparisonChain> findComparisonChains(FunctionAnalyses&, ChainKind, const ChainLimits&, NOESCAPE const ChainLinkFilter& mayJoinChain);
void convertChainToSwitch(IRBuilder&, FunctionAnalyses&, ChainKind, const ComparisonChain&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
