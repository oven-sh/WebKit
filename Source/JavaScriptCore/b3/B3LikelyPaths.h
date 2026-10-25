/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3BasicBlock.h"
#include "B3NaturalLoops.h"
#include <optional>
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class Procedure;

class LikelyPathAnalysis {
    WTF_MAKE_NONCOPYABLE(LikelyPathAnalysis);
public:
    JS_EXPORT_PRIVATE LikelyPathAnalysis(Procedure&, const NaturalLoops&);

    JS_EXPORT_PRIVATE void findBlocksOnEveryLikelyPath(const NaturalLoop&, IndexSet<BasicBlock*>& result);

private:
    bool isLikelyInnerEdge(const NaturalLoop&, FrequencyClass likeliestClassOfSource, const FrequentedBlock& successor) const;
    bool isIterationEnd(const NaturalLoop&, BasicBlock*) const;
    void walkLikelyEdges(const NaturalLoop&);
    void computeImmediateDominators(const NaturalLoop&);
    BasicBlock* nearestCommonDominator(BasicBlock*, BasicBlock*) const;

    const NaturalLoops& m_loops;
    IndexMap<BasicBlock*, std::optional<unsigned>> m_postOrderNumber;
    IndexMap<BasicBlock*, BasicBlock*> m_immediateDominator;
    Vector<BasicBlock*, 32> m_postOrder;
    Vector<BasicBlock*, 8> m_iterationEnds;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
