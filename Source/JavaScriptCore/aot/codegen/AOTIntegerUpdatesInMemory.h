/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "B3Policy.h"
#include "JSExportMacros.h"

namespace JSC { namespace AOT {

namespace Typed {
class Program;
struct Function;
}

JS_EXPORT_PRIVATE bool updateIntegersInMemory(B3::Procedure&);
B3::ClientPhase updateIntegersInMemoryAfterOptimization(const Typed::Program&, const Typed::Function&);
// A loop that pushes to one list keeps the length of the list in a register. Memory gets it where the loop is left, and in front of whatever else in the loop may read it.
B3::ClientPhase keepListLengthsInRegistersAfterOptimization();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
