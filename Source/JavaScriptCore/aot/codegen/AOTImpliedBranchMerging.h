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

// Two tests of one value in a row, each leaving for a block that is seldom entered: where the one value that fails the first also fails the second, the likely path keeps the second alone.
// Behind its failing edge the first tells the two apart.
// A Select whose condition a branch above has decided on every path to it becomes the chosen side: loads that the last round of optimizations made one show such Selects only then.
JS_EXPORT_PRIVATE bool mergeImpliedBranches(B3::Procedure&);
// In front of that, a test of a compressed reference that was decoded into a boxed value (is it a cell? is it the one marker its type allows?) becomes the test of the word that the decoding
// itself makes, so that test decides the decoding on both sides. A marker is asked of the word. A read that stands for a null check goes through the pointer, whatever the word is: what a marker
// decodes to lies in the first pages of the cage of cells, which are never mapped. Behind that read the decoding is decided.
B3::ClientPhase mergeImpliedBranchesAfterOptimization(const Typed::Program&, const Typed::Function&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
