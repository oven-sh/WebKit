/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleGraph.h"
#include "JSExportMacros.h"
#include <wtf/BitVector.h>
#include <wtf/FastBitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;

// Code generation releases function bodies while the program tables are encoded.
struct BodyFactsForTables {
    FastBitVector usedTypeSites;
    BitVector functionsThatOnlyReturn;
    Vector<ModuleImportSite> importSites;
};

JS_EXPORT_PRIVATE BodyFactsForTables collectBodyFactsForTables(const Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
