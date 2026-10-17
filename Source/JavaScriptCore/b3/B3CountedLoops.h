/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3NaturalLoops.h"
#include "B3Opcode.h"
#include <wtf/HashMap.h>
#include <wtf/IndexMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class UpsilonValue;

struct CountedLoop {
    BasicBlock* preHeader { nullptr };
    BasicBlock* header { nullptr };
    BasicBlock* test { nullptr };
    unsigned latchSuccessorIndex { 0 };
    BasicBlock* latch { nullptr };
    Opcode relation { Oops };
    Value* inductionVariable { nullptr };
    Value* start { nullptr };
    Value* next { nullptr };
    int64_t step { 0 };
    bool wrapsAroundInt32 { false };
    std::optional<int64_t> saturation;
    Value* bound { nullptr };

    bool increases() const { return relation == LessThan || relation == LessEqual || relation == Below || relation == BelowEqual; }
    bool isUnsigned() const { return relation == Below || relation == BelowEqual || relation == Above || relation == AboveEqual; }
    bool includesEqual() const { return relation == LessEqual || relation == GreaterEqual || relation == BelowEqual || relation == AboveEqual; }
};

static constexpr int64_t maxIterationsPerStrip = 1 << 16;

class UpsilonsByPhi {
    WTF_MAKE_NONCOPYABLE(UpsilonsByPhi);
public:
    explicit UpsilonsByPhi(Procedure& proc)
        : m_proc(proc)
    {
    }

    const Vector<UpsilonValue*, 2>* find(Value* phi) const;

private:
    Procedure& m_proc;
    mutable IndexMap<Value*, Vector<UpsilonValue*, 2>> m_map;
};

std::optional<CountedLoop> analyzeCountedLoop(NaturalLoops&, const NaturalLoop&, const UpsilonsByPhi&, const ScopedLambda<bool(Value*)>& isInvariant);
std::optional<CountedLoop> analyzeTopTestedCountedLoop(NaturalLoops&, const NaturalLoop&, BasicBlock* preHeader, BasicBlock* test, Value* condition, const UpsilonsByPhi&, const ScopedLambda<bool(Value*)>& isInvariant, const ScopedLambda<bool(BasicBlock*)>& isLeftOut);

Value* appendStripEnd(Procedure&, BasicBlock*, const CountedLoop&, Value* from, Value* bound, bool relationMayNotHold);

bool relationHoldsOnEntry(const CountedLoop&);
std::optional<bool> evaluateComparisonWithBound(const CountedLoop&, Opcode, Value* left, Value* right, Value* bound);
bool foldComparisonsWithBounds(Procedure&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
