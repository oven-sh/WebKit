/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

class PassRunner;
class ProgramEditor;
struct OptimizationInputs;

// A variable that is stored once, by code that runs once, with a value that nobody can tell when it was made, stays empty until its first read.
// What makes the value moves into a function of its own: it allocates, fills what it has allocated, and reads only what never changes.
// Each read finds the variable empty or filled, and the one that finds it empty calls that function.
void deferInitializers(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
