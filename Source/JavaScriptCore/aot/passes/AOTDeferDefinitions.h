/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class ProgramEditor;

// A variable of a module that is stored once, with a closure that captures nothing, and that no read can see before the store becomes a variable that is born as that function:
// the module body neither makes the closure nor stores it, and the first read that needs an object makes it.
void deferDefinitions(ProgramEditor&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
