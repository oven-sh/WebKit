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
// What Reflect.get answers: an accessor that is found from the object runs for the receiver.
void readForReceiver(ValueFlowRuleContext&, const ValueFlowOperand& object, const ValueFlowOperand& property, const ValueFlowOperand& receiver, ValueFlowNodeID result, ValueFlowNodeID thrown);
void listProxiesByTheirTraps(ValueFlowRuleContext&, ClimbingSet proxies);
ClimbingSet targetsListedInPlaceOf(ValueFlowRuleContext&, ClimbingSet proxies);
void askProxiesForListedValues(ValueFlowRuleContext&, ClimbingSet proxies, ValueFlowNodeID result);
// getOwnPropertyDescriptor or defineProperty. The second gets a descriptor that the engine has made.
void askProxiesAboutOwnProperty(ValueFlowRuleContext&, ClimbingSet proxies, const ValueFlowOperand& property, BuiltinName trap, ValueFlowNodeID descriptor, ValueFlowNodeID result);

ClimbingSet objectsHeldFromCreation(ValueFlowRuleContext&, ClimbingSet objects);
void readOwnPropertiesNobodyWrote(ValueFlowRuleContext&, ClimbingSet holders, std::optional<PropertyKeyID>, ValueFlowNodeID result);
enum class MetBy : bool { Program, NativeCode };
ValueFlowNodeID enumerablePropertiesNobodyWrote(ValueFlowRuleContext&, ClimbingSet holders, MetBy);
// Whoever goes over the properties of an object nobody knows reads them as a key nobody knows does.
inline constexpr ValueFlowName everyGetterRunByWalkName { ValueFlowNameKind::EveryMemberRead, 1 };
inline constexpr ValueFlowName everyGetterRunByPrinterName { ValueFlowNameKind::EveryMemberRead, 2 };
void runGettersOnObjectsNobodyKnows(ValueFlowRuleContext&);
void runGettersOnPrintedObjectsNobodyKnows(ValueFlowRuleContext&);
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
    const ValueFlowStatement* operation { nullptr };
};
void applyPropertyAction(ValueFlowRuleContext&, const BuiltinPropertyAction&, const ValueFlowPropertyActionOperands&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
