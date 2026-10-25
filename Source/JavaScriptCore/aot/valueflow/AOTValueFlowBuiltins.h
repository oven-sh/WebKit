/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowRuleContext.h"

namespace JSC { namespace AOT { namespace Typed {

struct ValueFlowBuiltinCall {
    ValueFlowCallKind kind { ValueFlowCallKind::Call };
    ValueFlowOperand thisValue { { }, ValueSets::nothing, ValueSets::anything };
    std::span<const ValueFlowOperand> arguments;
    bool argumentsMayBeAbsent { false };
    ValueFlowOperand otherArguments { { }, ValueSets::nothing, ValueSets::anything };
    ValueFlowOperand laterArguments { { }, ValueSets::nothing, ValueSets::anything };
    ValueFlowNodeID result;
    ValueFlowNodeID thrown;
    ValueFlowObjectID resultObject;
    ValueFlowObjectID contentsObject;
    const ValueFlowStatement* operation { nullptr };
    bool saysOnlyWhatIsCalled { false };
    bool readsOnlySpecies { false };
};

inline ValueFlowName cellOfCallsThatRunOtherCode(const BuiltinTable& builtins)
{
    return { ValueFlowNameKind::Slot, builtins.slotCount() + builtins.nameCount() };
}

inline ValueFlowName cellOfValueFromLibrary(const BuiltinTable& builtins)
{
    return { ValueFlowNameKind::Slot, builtins.slotCount() + builtins.nameCount() + 1 };
}

inline ValueFlowName cellOfDroppedResults(const BuiltinTable& builtins)
{
    return { ValueFlowNameKind::Slot, builtins.slotCount() + builtins.nameCount() + 2 };
}

ValueFlowNodeID nodeReadFromNativeCell(ValueFlowRuleContext&, BuiltinName cell);

void applyHostUses(BuiltinID onBehalfOf, const ValueFlowOperand& value, const BuiltinHostUses&, ValueFlowRuleContext&);
void callFromHost(BuiltinID onBehalfOf, const ValueFlowOperand& callees, const BuiltinHostCall&, ValueFlowRuleContext&);

bool doRecordsDescribe(const BuiltinTable&, const BuiltinOverload&);
bool standsBehindDescribedBuiltins(const Program&, FunctionID);
ValueFlowObjectID calleeOfDescribedCall(const ValueFlowUniverse&);
bool isCodeThatRecordsDescribe(const ValueFlowUniverse&, FunctionID);

void applyBuiltin(BuiltinID, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
bool bindsNoArgument(const Program&, const ValueFlowUniverse&, ValueFlowObjectID boundFunction);
bool raisesEventWhenCalled(const BuiltinTable&, BuiltinID);

BuiltinOverloadID overloadForAnotherNewTarget(const BuiltinTable&, BuiltinID, ValueFlowCallKind);
bool applyStepOfBuiltinIterator(BuiltinID, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
void holdContentsIteratedByResult(BuiltinID, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
bool applyConstructionOfPromise(BuiltinID, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
void settlePromisesThrough(ClimbingSet functions, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
void adoptRejectionOf(ValueFlowRuleContext&, ClimbingSet values, ValueFlowNodeID reason);
void applyBuiltin(BuiltinID, ValueFlowCallKind, const ValueFlowStatement& call, ValueFlowRuleContext&, ValueSetID receivers = ValueSets::anything);
void applyBuiltinFoundThroughUnknownObject(BuiltinID, const ValueFlowStatement& call, ValueFlowRuleContext&);
void readSpeciesOfUnknownObject(BuiltinID, ValueFlowRuleContext&);
void applyBuiltin(BuiltinID, ValueFlowCallKind, const ValueFlowCallGroup&, ValueFlowRuleContext&);
void applyEntryDescribing(ValueFlowObjectID closure, ValueFlowCallKind, const ValueFlowStatement& call, ValueFlowRuleContext&);
void applyEntryDescribing(ValueFlowObjectID closure, ValueFlowCallKind, const ValueFlowCallGroup&, ValueFlowRuleContext&);
void enterCodeThatEntryDescribes(ValueFlowObjectID closure, ValueFlowRuleContext&);
void noteCallOfBuiltin(BuiltinID, ValueFlowCallKind, ValueFlowRuleContext&);
bool mayBeFoundOnObjectNobodyKnows(ValueFlowRuleContext&, BuiltinName);
bool isCodeBehindDeclaredEntries(const Program&, FunctionID);
void callOtherCallables(ClimbingSet callees, ValueFlowCallKind, const ValueFlowStatement& call, ValueFlowRuleContext&);
void callOtherCallables(ClimbingSet callees, ValueFlowCallKind, const ValueFlowCallGroup&, ValueFlowRuleContext&);

void runInstanceOf(const ValueFlowStatement&, ValueFlowRuleContext&);
void letNativeCodeTestAgainst(ValueFlowRuleContext&, const ValueFlowOperand& constructors);

void runBehavior(const ValueFlowStatement&, ValueFlowRuleContext&);

inline BuiltinClassID classAbovePrototypeProperty(const Function& function)
{
    if (!function.has(FunctionFlags::IsGenerator))
        return BuiltinClasses::Object;
    return function.has(FunctionFlags::IsAsync) ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator;
}

inline BuiltinClassID classOfFunctionObject(const Function& function)
{
    bool isAsync = function.has(FunctionFlags::IsAsync);
    if (function.has(FunctionFlags::IsGenerator))
        return isAsync ? BuiltinClasses::AsyncGeneratorFunction : BuiltinClasses::GeneratorFunction;
    return isAsync ? BuiltinClasses::AsyncFunction : BuiltinClasses::Function;
}

inline bool mayReturnUnknownObject(const BuiltinType& type)
{
    if (type.isAnything() || type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction }))
        return true;
    return type.freshness == BuiltinFreshness::No && std::ranges::contains(type.parts, BuiltinTypePartKind::Instance, &BuiltinTypePart::kind);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
