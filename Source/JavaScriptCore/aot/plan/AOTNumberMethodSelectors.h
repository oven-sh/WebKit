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

struct FormsOfSelector {
    Representation result { Representation::JSValue };
    bool returnsNothing { false };
    bool everyRowIsProven { false };
};
FormsOfSelector formsOfSelector(const Program&, const LinkTables& linked, const CallingConventions&, const MethodFamily&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
