/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCallingConventions.h"

namespace JSC { namespace AOT { namespace Typed {

class Program;
struct LinkTables;

// `for (const n of nodes) n.area()`: the method table in the forms of the methods.
struct FormsOfSelector {
    // The one form in which the Body of every method of the family returns.
    Representation result { Representation::JSValue };
    // No method of the family returns a word.
    bool returnsNothing { false };
    // No row stands on a guard of a class: nothing puts the lookup into the table, and no object with a row can come to stand below another prototype.
    bool everyRowIsProven { false };
};
// For planMethodTable(), of a family whose rows and methods are listed. The default: as today.
FormsOfSelector formsOfSelector(const Program&, const LinkTables& linked, const CallingConventions&, const MethodFamily&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
