/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowBodies.h"
#include "AOTValueFlowBuiltins.h"

namespace JSC { namespace AOT { namespace Typed {

struct ValueFlowTypedResult {
    TypeID type;
    Vector<LayoutID, 4> layouts;
    Vector<TypeID, 4> typesWithArrays;
    Vector<LayoutID, 4> layoutsAmongPlainData;

    explicit operator bool() const { return !!type; }

    static constexpr uint32_t firstOrdinal = 2;
    uint32_t ordinalOfLayout(size_t index) const { return firstOrdinal + static_cast<uint32_t>(index); }
    uint32_t ordinalOfArray(size_t index) const { return ordinalOfLayout(layouts.size() + index); }
    uint32_t ordinalOfPlainArray() const { return ordinalOfArray(typesWithArrays.size()); }
    uint32_t numberOfObjects() const { return ordinalOfPlainArray() + 1 - firstOrdinal; }
};

void appendSitesOfTypedResults(const Program&, const ValueFlowBuiltinTables&, const Function&, const CFG&, Vector<ValueFlowSite>&);
void allocateTypedResult(const BuiltinOverload&, const ValueFlowStatement& call, const ValueFlowBuiltinCall&, ValueFlowObjectID promise, ValueSetID plainData, ValueFlowRuleContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
