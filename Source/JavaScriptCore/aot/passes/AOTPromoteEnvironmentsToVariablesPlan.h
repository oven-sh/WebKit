/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;

enum class CaptureChange : uint8_t { None, Remove, ReadVariable };

struct PlannedCapture {
    CaptureChange change { CaptureChange::None };
    VariableID variable;
};

struct StoredValue {
    FunctionID function;
    ValueID value;
    VariableID variable;
};

struct PromotionPlan {
    bool isEmpty() const { return editedFunctions.isEmpty(); }

    bool isPromoted(LayoutID layout) const { return promotedLayouts.get(layout.index()); }
    VariableID variableOf(LayoutID layout, uint32_t field) const { return VariableID(firstVariables[layout.index()].index() + field); }

    FunctionID closureFunctionOf(FunctionID function) const { return closureFunctions[function.index()]; }
    std::span<const PlannedCapture> capturesOf(FunctionID function) const LIFETIME_BOUND { return captures[closureFunctionOf(function).index()].span(); }
    BitVector removedCapturesOf(FunctionID) const;
    std::span<const StoredValue> storedValuesIn(FunctionID) const LIFETIME_BOUND;

    BitVector promotedLayouts;
    Vector<VariableID> firstVariables;
    Vector<FunctionID> closureFunctions;
    Vector<Vector<PlannedCapture>> captures;
    Vector<StoredValue> storedValues;
    Vector<Variable> variables;
    Vector<FunctionID> editedFunctions;
    uint64_t numberOfPromotedCaptures { 0 };
};

PromotionPlan planEnvironmentPromotion(const Program&, const FlowResults&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
