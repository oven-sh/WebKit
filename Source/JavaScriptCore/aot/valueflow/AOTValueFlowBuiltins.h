/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTFlowPrototypes.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowRuleContext.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

// `m.call(o)` with one of these is `o.m()`: tslib's __values and __read call them so.
inline constexpr std::array makersOfIteratorsCalledInPlace {
    Builtins::ArrayPrototypeValues, Builtins::ArrayPrototypeKeys, Builtins::ArrayPrototypeEntries,
    Builtins::MapPrototypeKeys, Builtins::MapPrototypeValues, Builtins::MapPrototypeEntries,
    Builtins::SetPrototypeValues, Builtins::SetPrototypeEntries,
};

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
    bool isFoundOnReceiver { false };
    bool saysOnlyWhatIsCalled { false };
    bool readsOnlySpecies { false };
};

inline ValueFlowCellName cellOfCallsThatRunOtherCode(const BuiltinTable& builtins)
{
    return ValueFlowCellName { ValueFlowNameKind::Slot, builtins.slotCount() + builtins.nameCount() };
}

inline ValueFlowCellName cellOfValueFromLibrary(const BuiltinTable& builtins)
{
    return ValueFlowCellName { ValueFlowNameKind::Slot, builtins.slotCount() + builtins.nameCount() + 1 };
}

inline ValueFlowCellName cellOfDroppedResults(const BuiltinTable& builtins)
{
    return ValueFlowCellName { ValueFlowNameKind::Slot, builtins.slotCount() + builtins.nameCount() + 2 };
}

ValueFlowNodeID nodeReadFromNativeCell(ValueFlowRuleContext&, BuiltinName cell);
// Whoever takes the member without naming it holds what the member answers out of a native cell as a value nobody knows.
void openProxiesThatMemberAnswers(ValueFlowRuleContext&, BuiltinID member);
// True where flow can list the own names of every object among the values: `held` gets them.
bool listKeysAsObjectKeysDoes(ValueFlowRuleContext&, ClimbingSet values, ValueFlowNodeID held);

void applyHostUses(BuiltinID onBehalfOf, const ValueFlowOperand& value, const BuiltinHostUses&, ValueFlowRuleContext&);
void callFromHost(BuiltinID onBehalfOf, const ValueFlowOperand& callees, const BuiltinHostCall&, ValueFlowRuleContext&);

bool doRecordsDescribe(const BuiltinTable&, const BuiltinOverload&);
// True where every way to call the built-in does nothing to the operand but call it.
bool onlyCallsOperand(const BuiltinTable&, BuiltinID, BuiltinOperand);
bool onlyListsOperand(const BuiltinTable&, BuiltinID, BuiltinOperand);
bool onlyPassesOperandOn(const BuiltinTable&, BuiltinID, BuiltinOperand);
bool onlyAsksWhatStandsAbove(const BuiltinTable&, BuiltinID, BuiltinOperand, BuiltinCallMode);
bool onlyLists(const ValueFlowBehavior&);
bool standsBehindDescribedBuiltins(const Program&, FunctionID);
bool isEnteredByCallsAlone(const Program&, RuntimeFunctionID);
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
ValueFlowNodeID findNodeOfListedBun(ValueFlowRuleContext&, const ValueFlowOperand&);
ValueSetID promisesAllocatedByBuiltins(ValueFlowRuleContext&, ClimbingSet values);
void connectAwaitedValue(ValueFlowRuleContext&, const ValueFlowOperand&, ValueFlowNodeID target);
void applyBuiltin(BuiltinID, ValueFlowCallKind, const ValueFlowStatement& call, ValueFlowRuleContext&, ValueSetID receivers = ValueSets::anything);
void applyBuiltinFoundThroughUnknownObject(BuiltinID, const ValueFlowStatement& call, ValueFlowRuleContext&);
bool listsObjectThatLetsInUnseenCode(ValueFlowRuleContext&, const ValueFlowOperand&);
ClimbingSet builtinFunctionsListedBesideUnknownObject(ValueFlowRuleContext&, ValueFlowNodeID callee);
void readSpeciesOfUnknownObject(BuiltinID, ValueFlowRuleContext&);
void applyBuiltin(BuiltinID, ValueFlowCallKind, const ValueFlowCallGroup&, ValueFlowRuleContext&);
void applyEntryDescribing(ValueFlowObjectID closure, ValueFlowCallKind, const ValueFlowStatement& call, ValueFlowRuleContext&);
void applyEntryDescribing(ValueFlowObjectID closure, ValueFlowCallKind, const ValueFlowCallGroup&, ValueFlowRuleContext&);
void enterCodeThatEntryDescribes(ValueFlowObjectID closure, ValueFlowRuleContext&);
void noteCallOfBuiltin(BuiltinID, ValueFlowCallKind, ValueFlowRuleContext&);
void noteInstancesIn(ValueFlowRuleContext&, BuiltinTypeID);
void connectEnumerableMembersOf(ClimbingSet hostObjects, ValueFlowNodeID to, ValueFlowRuleContext&);
// The keys of objects that only the statements of the program have laid a name on: those that they were made with. What was added since is under AddedNames.
enum class OwnKeys : uint8_t { EnumerableStrings, Strings, All };
ClimbingSet objectsWithOwnKeysAmong(ValueFlowRuleContext&, ClimbingSet values);
std::optional<ValueSetID> keysThatObjectsWereMadeWith(ValueFlowRuleContext&, ClimbingSet objects, OwnKeys);
void carryOut(ValueFlowRuleContext&, ValueFlowNodeID values);
void carryOut(ValueFlowRuleContext&, const ValueFlowOperand& values);
bool mayBeFoundOnObjectNobodyKnows(ValueFlowRuleContext&, BuiltinName);
bool mayProgramHaveStoredMethodThatHostLooksFor(ValueFlowRuleContext&);
bool isCodeBehindDeclaredEntries(const Program&, FunctionID);
void callOtherCallables(ClimbingSet callees, ValueFlowCallKind, const ValueFlowStatement& call, ValueFlowRuleContext&);
void callOtherCallables(ClimbingSet callees, ValueFlowCallKind, const ValueFlowCallGroup&, ValueFlowRuleContext&);

void runInstanceOf(const ValueFlowStatement&, ValueFlowRuleContext&);
void describeObjectsRefusedAsFunctions(ValueFlowRuleContext&, ClimbingSet functions);
void letNativeCodeTestAgainst(ValueFlowRuleContext&, const ValueFlowOperand& constructors);

void runBehavior(const ValueFlowStatement&, ValueFlowRuleContext&);

inline BuiltinClassID classAbovePrototypeProperty(const Function& function)
{
    if (!function.has(FunctionFlags::IsGenerator))
        return BuiltinClasses::Object;
    return function.has(FunctionFlags::IsAsync) ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator;
}

inline bool mayReturnUnknownObject(const BuiltinType& type)
{
    if (type.isAnything() || type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction }))
        return true;
    return type.freshness == BuiltinFreshness::No && std::ranges::contains(type.parts, BuiltinTypePartKind::Instance, &BuiltinTypePart::kind);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
