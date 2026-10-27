/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowRuleContext.h"

namespace JSC { namespace AOT {

struct BuiltinPropertyAction;

namespace Typed {

// A lookup under this name answers what readOwnPropertiesNobodyWrote() answers under any key.
inline constexpr ValueFlowName everyOwnPropertyNobodyWroteName { ValueFlowNameKind::EveryProperty, 4 };

void runHeapOperation(const ValueFlowStatement&, ValueFlowRuleContext&);

void runLookup(const ValueFlowStatement&, ValueFlowRuleContext&);

ClimbingSet objectsHeldFromCreation(ValueFlowRuleContext&, ClimbingSet objects);
void readOwnPropertiesNobodyWrote(ValueFlowRuleContext&, ClimbingSet holders, std::optional<PropertyKeyID>, ValueFlowNodeID result);
ClimbingSet objectsRightAbove(ValueFlowRuleContext&, ClimbingSet knownObjects);
ValueFlowNodeID exportsOfHoldersListedBesideUnknownObject(ValueFlowRuleContext&, const ValueFlowOperand&);
ValueFlowNodeID exportsOfHoldersIn(ValueFlowRuleContext&, const ValueFlowOperand&);

struct ValueFlowPropertyActionOperands {
    ValueFlowOperand object;
    ValueFlowOperand key;
    ValueFlowOperand value;
    ValueFlowNodeID target;
    ValueFlowNodeID thrown;
    std::optional<std::span<const PropertyKeyID>> namesInMap { };
    bool mapHasComputedNames { false };
};
void applyPropertyAction(ValueFlowRuleContext&, const BuiltinPropertyAction&, const ValueFlowPropertyActionOperands&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
