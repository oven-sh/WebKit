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

// Where several ways back of one loop were led to one test at the bottom, what they hand on is joined there: the caller folds the joins of one value once the procedure is in SSA form again.
JS_EXPORT_PRIVATE bool rotateLoops(Procedure&, const Vector<BasicBlock*>* allowedHeaders = nullptr, bool* didJoinWaysBack = nullptr);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
