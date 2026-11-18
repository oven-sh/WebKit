/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSlotPlan.h"
#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet opcodesThatListStoresOfCopiedConstructorTakes { Opcode::Construct };
AOT_LINK_OF_WALK(listStoresOfCopiedConstructor, opcodesThatListStoresOfCopiedConstructorTakes)

bool storeLaysWhatSlotOfFreshObjectHolds(Walker&, Reference object, Reference held, Reference stored);

bool lookMayFinishBirthOf(Walker&, Reference object);
void finishBirthAtReturnOfClassConstructor(Walker&, Reference returned, Origin);
void finishBirthAtReturnOfInitializer(Walker&, Origin);
void showWhatIsHeldFilledWhereConstructionEnds(Walker&, FunctionID constructor, Reference object, Origin);
void showWhatIsHeldFilledAtReturnOfClassConstructor(Walker&, Reference returned, Origin);
Reference allocateFilledObject(Walker&, Reference object, Origin);
Reference allocateFilledObjectAtReturn(Walker&, Reference returned, Origin);

struct MergeableObject {
    LayoutID layout;
    TypeID shape;
    uint32_t orderWord { 0 };
    Vector<FieldIndex, 8> fields;
    Vector<Reference, 8> values;
    Reference length;
};
Reference knownElementOfArray(Walker&, const WalkState&, Reference array, Reference position);
TypeID typeOfMergeableObject(Walker&, const MergeableObject&);
bool holdsBrandOfClass(const Walker&, FieldReference);
std::optional<MergeableObject> findMergeableObject(Walker&, const WalkState&, ValueID cell);
Reference allocateMergeableObject(Walker&, const MergeableObject&, Origin);
Reference allocateMergedObject(Walker&, ValueID parameter, Origin);
bool arrivesAsMergedObject(Walker&, ValueID cell, const MergeableObject& expected);
void noteMergedObject(Walker&, Reference parameter, const MergeableObject&);

struct StoresListedAtBirth {
    Vector<FieldIndex, 16> fields;
    bool asksForLayoutPrototype { false };
    Vector<MemberConditionRecord, 8> conditions;
    Vector<MemberConditionRecord, 8> conditionsRemembered;
    bool isBornUnfinished { false };
};
StoresListedAtBirth findStoresThatAllocationMayList(Walker&, LayoutID, std::span<const PropertyKeyID> keys, std::span<const TypeID> stored, std::span<const PropertyKeyID> keysStoredFurtherOn);
bool looksAtCellsAskForLayoutPrototype(Walker&, LayoutID);
void noteGuardsShownByTest(Walker&, std::span<const MemberConditionRecord>, Reference test, Reference receiver = { });
void checkGuardsThatAreRemembered(Walker&, std::span<const MemberConditionRecord>, Origin);
void askWholeSideWhereGuardsMayNotHaveBegun(Walker&, LayoutID classLayout, Vector<MemberConditionRecord, 8>& conditions);
std::optional<MemberConditionRecord> findGuardOfCallOffConstructorFunction(Walker&, FunctionID);
std::optional<MemberConditionRecord> findStaticMethodGuard(Walker&, FunctionID, PropertyKeyID);

struct InitializerMethod {
    FunctionID function;
    std::optional<MemberConditionRecord> guard;
};
InitializerMethod findInitializerMethod(Walker&, LayoutID, PropertyKeyID);

struct GuardOfPrototypeAtConstruction {
    MemberConditionRecord condition;
    bool isKnownToHold { false };
};
bool isPrototypeAtConstructionProven(const Walker&, FunctionID constructor);
void notePrototypeOfConstruction(Walker&, Reference object, LayoutID);
bool hasPrototypeOfConstruction(const Walker&, Reference object, LayoutID);
bool hasGuardsOfOneEvaluation(const Walker&, LayoutID classLayout);
std::optional<GuardOfPrototypeAtConstruction> findGuardOfPrototypeAtConstruction(Walker&, ValueID construction, FunctionID constructor, bool newCellIsLookedAt, bool guardsAreAskedAtBirth, Origin);
void checkPrototypeOfCellMadeBehindGuard(Walker&, Reference object, Reference constructor, LayoutID constructed, Origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
