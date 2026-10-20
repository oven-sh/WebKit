/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIntegerRelations.h"
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// The string whose length the value is, or nothing.
ValueID stringMeasuredBy(const Program&, const Function&, const Value&);

class StringPositions {
    WTF_MAKE_NONCOPYABLE(StringPositions);
public:
    explicit StringPositions(FunctionAnalyses&);

    // A string never changes its length, so a comparison with any read of it holds for every other.
    bool isInside(ValueID string, ValueID position, BlockID at);

private:
    using Lengths = Vector<ValueID, 2>;

    void collectLengths();

    FunctionAnalyses& m_analyses;
    IntegerRelations m_relations;
    HashMap<ValueID, Lengths> m_lengthsOfString;
    bool m_didCollectLengths { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
