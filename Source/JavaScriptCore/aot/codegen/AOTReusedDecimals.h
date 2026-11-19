/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDecimalRuns.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

// Results of runs, and the parameters that they are passed to, of which no two are alive at once: a total that goes around a loop. They
// take turns in one object, which is named by a word of the frame and keeps its trailing zeros. Whatever is no run gets the object for
// good: see DecimalEmitter::letGo().
class ReusedDecimals {
public:
    ReusedDecimals() = default;
    ReusedDecimals(const Code&, const DecimalRuns&, std::span<const BlockID> order, const ScopedLambda<bool(BlockID home, BlockID user)>& dominates);

    unsigned numberOfObjects() const { return m_numberOfObjects; }
    bool contains(ValueID value) const { return m_values.get(value.index()); }
    unsigned objectOf(ValueID value) const { return std::ranges::find(m_objects, value, &std::pair<ValueID, unsigned>::first)->second; }
    bool shareObject(ValueID value, ValueID other) const { return contains(value) && contains(other) && objectOf(value) == objectOf(other); }

private:
    BitVector m_values;
    Vector<std::pair<ValueID, unsigned>> m_objects;
    unsigned m_numberOfObjects { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
