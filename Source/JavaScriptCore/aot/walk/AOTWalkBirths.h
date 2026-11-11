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
void showWhatIsHeldFilledWhereConstructionEnds(Walker&, FunctionID constructor, Reference object, Origin);
void showWhatIsHeldFilledAtReturnOfClassConstructor(Walker&, Reference returned, Origin);
Reference allocateFilledObject(Walker&, Reference object, Origin);
Reference allocateFilledObjectAtReturn(Walker&, Reference returned, Origin);

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
std::optional<GuardOfPrototypeAtConstruction> findGuardOfPrototypeAtConstruction(Walker&, ValueID construction, FunctionID constructor, bool newCellIsLookedAt, bool guardsAreAskedAtBirth, Origin);
void checkPrototypeOfCellMadeBehindGuard(Walker&, Reference object, Reference constructor, LayoutID constructed, Origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
