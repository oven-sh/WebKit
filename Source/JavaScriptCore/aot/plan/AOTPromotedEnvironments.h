/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFactRecords.h"
#include "AOTID.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

class Facts;
class Program;
struct SlotPlanParts;

struct PromotedVariable {
    ModuleID module;
    StringID name;
    LocationFacts location;
    FunctionID maker;
    LayoutID layout;
    uint32_t field { 0 };
    FunctionID function;
    uint32_t capture { 0 };
};

struct PromotedLayout {
    LayoutID layout;
    VariableID firstVariable;
};

struct RemovedCapture {
    FunctionID function;
    uint32_t capture { 0 };
    VariableID variable;
};

struct StoreOfCapturedValue {
    FunctionID function;
    ValueID value;
    VariableID variable;
};

struct PromotedEnvironments {
    uint32_t numberOfCapturesOfClassesStoredForGood { 0 };
    uint32_t numberOfRemovedCapturesOf(FunctionID function) const { return function.index() < numbersOfRemovedCaptures.size() ? numbersOfRemovedCaptures[function.index()] : 0; }
    const PromotedVariable* find(VariableID id) const LIFETIME_BOUND
    {
        if (!id || !firstVariable || id.index() < firstVariable.index() || id.index() - firstVariable.index() >= variables.size())
            return nullptr;
        return &variables[id.index() - firstVariable.index()];
    }

    bool isPromoted(LayoutID layout) const { return !!variableOf(layout, 0); }
    VariableID variableOf(LayoutID layout, uint32_t field) const
    {
        if (layouts.isEmpty())
            return { };
        auto found = std::ranges::lower_bound(layouts, layout, { }, &PromotedLayout::layout);
        return found != layouts.end() && found->layout == layout ? VariableID(found->firstVariable.index() + field) : VariableID();
    }

    bool isMaker(FunctionID function) const { return makers.get(function.index()); }

    const RemovedCapture* findRemoved(FunctionID function, uint32_t capture) const LIFETIME_BOUND
    {
        if (!numberOfRemovedCapturesOf(function))
            return nullptr;
        auto found = std::ranges::lower_bound(removedCaptures, std::pair { function, capture }, { }, [](const RemovedCapture& removed) {
            return std::pair { removed.function, removed.capture };
        });
        return found != removedCaptures.end() && found->function == function && found->capture == capture ? &*found : nullptr;
    }
    bool isRemoved(FunctionID function, uint32_t capture) const { return !!findRemoved(function, capture); }
    std::span<const RemovedCapture> removedCapturesOf(FunctionID function) const LIFETIME_BOUND
    {
        if (!numberOfRemovedCapturesOf(function))
            return { };
        auto found = std::ranges::equal_range(removedCaptures, function, { }, &RemovedCapture::function);
        return { found.begin(), found.end() };
    }

    std::span<const StoreOfCapturedValue> storesIn(FunctionID function) const LIFETIME_BOUND
    {
        if (stores.isEmpty() || !isMaker(function))
            return { };
        auto found = std::ranges::equal_range(stores, function, { }, &StoreOfCapturedValue::function);
        return { found.begin(), found.end() };
    }

    VariableID firstVariable;
    Vector<PromotedVariable> variables;
    Vector<uint32_t> numbersOfRemovedCaptures;
    Vector<PromotedLayout> layouts;
    Vector<RemovedCapture> removedCaptures;
    Vector<StoreOfCapturedValue> stores;
    BitVector makers;
};

PromotedEnvironments planPromotedEnvironments(const Program&, const Facts&, const SlotPlanParts&, std::span<const LayoutID> scopesTakenByClosuresMadeForCallsAlone);
void appendPromotedVariables(const PromotedEnvironments&, SlotPlanParts&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
