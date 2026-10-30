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

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
