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
class Value;
struct CountedLoop;

struct LoopTestCopy {
    BasicBlock* test { nullptr };
    BasicBlock* latch { nullptr };
    Value* testedValue { nullptr };
    BasicBlock* failed { nullptr };
};

struct ClampTargets {
    Value* limit { nullptr };
    BasicBlock* beyondLimit { nullptr };
    Vector<LoopTestCopy, 1> otherTests;
    // Copies of the tested value on ways that come back to the loop's own test.
    Vector<Value*, 1> otherTestedValues;
};

JS_EXPORT_PRIVATE void clampLoopBound(Procedure&, const CountedLoop&, const ClampTargets&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
