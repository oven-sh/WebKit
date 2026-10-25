/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class OutliningSiteFinder;
class ProgramEditor;
struct OptimizationInputs;

// For whoever wants to read each function once outlining is through with it, while the function is in the cache where that can be.
struct ReadersOfOutlinedFunctions {
    // By one thread, while nobody reads: the program has got the heads of the shared bodies.
    const ScopedLambda<void()>& didAddFunctions;
    // Each shared body, and each function in which the finder has found a site, whether or not a site of it was replaced.
    const ScopedLambda<void(FunctionID)>& read;
};

// With a finder: it has looked at every function with a body, as the functions are now.
void outlineColdOperations(ProgramEditor&, const OptimizationInputs&, BuildContext&, OutliningSiteFinder* = nullptr, const ReadersOfOutlinedFunctions* = nullptr);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
