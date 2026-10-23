/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleGraph.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <wtf/BitVector.h>
#include <wtf/FastBitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;

// Code generation releases function bodies while the program tables are encoded.
struct BodyFactsForTables {
    FastBitVector usedTypeSites;
    BitVector functionsThatOnlyReturn;
    // Those that have code, and those that were inlined into it.
    BitVector functionsWithFrames;
    Vector<ModuleImportSite> importSites;
};

JS_EXPORT_PRIVATE BodyFactsForTables collectBodyFactsForTables(const Program&, const BitVector& bodiesWithoutCode, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
