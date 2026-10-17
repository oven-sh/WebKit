/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class BasicBlock;
class Procedure;

bool versionLoops(Procedure&, Vector<BasicBlock*>& topTestedVersionHeaders);
bool eliminateRedundantPhiCycles(Procedure&);
bool narrowExtendedWordPhis(Procedure&);
bool foldOutOfRangeComparisons(Procedure&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
