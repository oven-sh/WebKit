/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class BasicBlock;
class Procedure;

// Sets didMergeBackEdges when several latches of a loop now share one bottom test. fixSSA() adds Phis there, so run eliminateRedundantPhiCycles() after it.
JS_EXPORT_PRIVATE bool rotateLoops(Procedure&, const Vector<BasicBlock*>* allowedHeaders = nullptr, bool* didMergeBackEdges = nullptr);

// A test at the top of a loop that leaves the loop, and that reads nothing that a turn of the loop writes, is asked in front of the loop alone. No block of the loop is copied but the ones with such tests.
// For the loops that are too large to get a fast version. Run fixSSA() after it.
JS_EXPORT_PRIVATE bool moveTestsThatAreSameInEveryTurnOutOfLoops(Procedure&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
