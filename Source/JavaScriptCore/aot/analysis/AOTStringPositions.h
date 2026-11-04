/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIntegerRelations.h"
#include "AOTVector.h"
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

ValueID stringMeasuredBy(const Program&, const Function&, const Value&);

class StringPositions {
    WTF_MAKE_NONCOPYABLE(StringPositions);
public:
    explicit StringPositions(FunctionAnalyses&);

    bool isInside(ValueID string, ValueID position, BlockID at);

private:
    FunctionAnalyses& m_analyses;
    IntegerRelations m_relations;
    HashMap<ValueID, Vector<ValueID, 2>> m_lengthsOfString;
    bool m_didCollectLengths { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
