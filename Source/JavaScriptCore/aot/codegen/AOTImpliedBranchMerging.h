/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "B3Policy.h"
#include "JSExportMacros.h"

namespace JSC { namespace AOT {

// Two tests of one value in a row, each leaving for a block that is seldom entered: where the one value that fails the first also fails the second, the likely path keeps the second alone.
// Behind its failing edge the first tells the two apart.
JS_EXPORT_PRIVATE bool mergeImpliedBranches(B3::Procedure&);
B3::ClientPhase mergeImpliedBranchesAfterOptimization();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
