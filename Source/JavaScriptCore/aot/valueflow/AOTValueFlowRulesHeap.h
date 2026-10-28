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

// A lookup under this name answers what readOwnPropertiesNobodyWrote() answers under the key of an enumerable property, and under any key for a receiver with a property that was defined again.
inline constexpr ValueFlowName everyOwnPropertyNobodyWroteName { ValueFlowNameKind::EveryProperty, 4 };
// The same for native code, to which what a built-in class lays on its instances is nothing new.
inline constexpr ValueFlowName everyOwnPropertyNewToNativeCodeName { ValueFlowNameKind::EveryProperty, 7 };
// A lookup under this name answers every own property that the receivers hold, and nothing for a name that they lack.
inline constexpr ValueFlowName everyOwnPropertyHeldName { ValueFlowNameKind::EveryProperty, 5 };
// The same without the properties that are not enumerable on an instance of a built-in class since it was made.
inline constexpr ValueFlowName everyEnumerableOwnPropertyHeldName { ValueFlowNameKind::EveryProperty, 6 };

void runHeapOperation(const ValueFlowStatement&, ValueFlowRuleContext&);

void runLookup(const ValueFlowStatement&, ValueFlowRuleContext&);

ClimbingSet objectsHeldFromCreation(ValueFlowRuleContext&, ClimbingSet objects);
void readOwnPropertiesNobodyWrote(ValueFlowRuleContext&, ClimbingSet holders, std::optional<PropertyKeyID>, ValueFlowNodeID result);
enum class MetBy : bool { Program, NativeCode };
ValueFlowNodeID enumerablePropertiesNobodyWrote(ValueFlowRuleContext&, ClimbingSet holders, MetBy);
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
    std::optional<std::span<const PropertyKeyID>> namesOfSource { };
};
void applyPropertyAction(ValueFlowRuleContext&, const BuiltinPropertyAction&, const ValueFlowPropertyActionOperands&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
