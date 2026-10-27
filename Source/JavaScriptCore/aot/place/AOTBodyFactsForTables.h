/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTModuleGraph.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <wtf/BitVector.h>
#include <wtf/FastBitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;

// A call in tail position in a plain function that may be entered by value. Both functions are as they are written, and there is no callee where the call is by value.
struct WrittenTailCall {
    FunctionID function;
    FunctionID callee;
    uint32_t position { 0 };
};

// Code generation releases function bodies while the program tables are encoded.
struct BodyFactsForTables {
    FastBitVector usedTypeSites;
    BitVector functionsThatOnlyReturn;
    // Those that have code, and those that were inlined into it.
    BitVector functionsWithFrames;
    Vector<ModuleImportSite> importSites;
    // Ascending by function, then by position. No two are at one place.
    Vector<WrittenTailCall> placesOfTailCalls;
};

JS_EXPORT_PRIVATE BodyFactsForTables collectBodyFactsForTables(const Program&, const BitVector& bodiesWithoutCode, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
