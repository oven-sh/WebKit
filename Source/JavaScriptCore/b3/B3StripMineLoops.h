/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class BasicBlock;
class PatchpointValue;
class Procedure;
class Value;
struct CountedLoop;

struct LoopTestCopy {
    BasicBlock* test { nullptr };
    BasicBlock* latch { nullptr };
    Value* testedValue { nullptr };
    BasicBlock* failed { nullptr };
};

struct StripTargets {
    Value* limit { nullptr };
    BasicBlock* beyondLimit { nullptr };
    bool relationHoldsOnEntry { false };
    Vector<LoopTestCopy, 1> otherTests;
};

JS_EXPORT_PRIVATE bool stripMineLoops(Procedure&);

JS_EXPORT_PRIVATE void stripMineLoop(Procedure&, const CountedLoop&, PatchpointValue* poll, const StripTargets&);

JS_EXPORT_PRIVATE void clampLoopBound(Procedure&, const CountedLoop&, const StripTargets&);

JS_EXPORT_PRIVATE Value* appendStripEnd(Procedure&, BasicBlock*, const CountedLoop&, Value* from, Value* limit, bool relationMayNotHold);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
