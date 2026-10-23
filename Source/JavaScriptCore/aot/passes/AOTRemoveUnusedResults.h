/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"
#include "AOTVector.h"
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class PassRunner;

void removeUnusedResults(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

struct ResultCallSite {
    ValueID call;
    FunctionID callee;
    BitVector readResults;
};

struct ResultCallSiteList {
    FunctionID caller;
    Vector<ResultCallSite> sites;
};

Vector<ResultCallSite> collectResultCallSites(const Program&, const Function& caller);
Vector<ResultCallSiteList> collectAllResultCallSites(const Program&, BuildContext&);

bool returnsOnlyResults(const Function&);

enum class ResultEditing : uint8_t { Unknown, Allowed };

ResultEditing canEditResults(const Program&, const CallGraph&, FunctionID);

void retainResults(Function&, const BitVector& retained);

struct RetargetedCallSite {
    ValueID call;
    FunctionID callee;
    const BitVector* retained { nullptr };
    TypeID singleResultType;
};

void retargetResultCallSites(Function& caller, std::span<const RetargetedCallSite>);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
