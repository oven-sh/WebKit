/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class GuardEmitter final : public OperationEmitter {
public:
    explicit GuardEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateShapeTest(const ValueContext&);
    void generateIsSlotFilled(const ValueContext&);
    void generateHasNoExtraProperties(const ValueContext&);
    void generateHasNoFieldOutsideSlot(const ValueContext&);
    void generateHasBirthFieldsFilled(const ValueContext&);
    void generateHasKeyOrder(const ValueContext&);
    void generateIsNotFrozen(const ValueContext&);
    void generateHasLayoutPrototype(const ValueContext&);
    void generateBelongsToInstance(const ValueContext& value) { defineTest(value, belongsToInstance(in<CellPointer>(value->objectOperand()))); }
    void generateIsPristine(const ValueContext&);
    void generateHasArrayStorage(const ValueContext&);
    void generateIsInBounds(const ValueContext&);
    void generateIsElementPresent(const ValueContext&);
    void generateIsInsideView(const ValueContext&);
    void generateIsNotThenable(const ValueContext&);
    void generateAreMembersUnchanged(const ValueContext&);
    // FindsDeclaredMember with receivers: askGuardsAboutDeclaredMember() of walk/AOTWalkGuesses.cpp.
    void defineWhetherDeclaredMemberIsFound(const ValueContext&);
    // PropHasOwn with receivers: receiversWhoseCellsAnswerForOwnProperty() of walk/AOTWalkGuesses.cpp. It leaves for `targets` where the cell answers, and for `asksRunTime` where it does not.
    void branchOnOwnPropertyByCell(const ValueContext&, BranchTargets, LBasicBlock asksRunTime);
    void generateIsClosureOf(const ValueContext&);
    void generateIsBoundClosureOf(const ValueContext&);
    void generateIsEnumeratorCurrent(const ValueContext&);

    // AOTClassGuards.h. A null name: the guard of the side, which says nothing of any member.
    Condition holds(LayoutID classLayout, ClassSide, PropertyKeyID name, bool isAskedByStoresAlone = false);
    Condition belongsToInstance(CellPointer cell) { return hasInstanceRealm(loadStructure(cell)); }
    // What AreMembersUnchanged asks. The list is not empty.
    Condition areMembersUnchanged(PayloadListView<MemberConditionRecord>);
    // Whether the cell is a compiled closure that inherits from the realm's Function.prototype and has only the properties it was created with.
    Condition isClosureAsCreated(CellPointer, BuiltinClassID, OptionSet<PristineAspects>);
    Condition isNotThenable(const OperationContext&, Boxed, const Type&);
    // As arraySpeciesWatchpointIsValid() asks: in the realm of the instance neither `constructor` of Array.prototype nor @@species of Array has changed. No before the run time has asked for the first time.
    Condition realmWatchesSpeciesOfLists();

private:
    void failAndDefineFalse(const ValueContext&, InternalErrorKind);
    Condition guardInByteHolds(LayoutID classLayout, ClassSide, TypedPointer byteOfGuard);
    Condition isPresentCell(Boxed, const Type&);
    // These tests read words of compiled objects. A cell of a built-in class fails them.
    void defineTestOfCompiledObject(const ValueContext&, NOESCAPE const Invocable<Condition()> auto& test);
    Condition orderWordHasNoBits(const ValueContext&, uint32_t bits);
    CellPointer untestedCell(const OperationContext&, ValueID);
    CellPointer cellOrElse(const OperationContext&, ValueID, BlockEdge isNotCell);
    bool failingSideBeginsWithAccessThrough(const ValueContext& test, ValueID object) const;

    Condition hasInstanceRealm(Pointer structure);
    void branchOnIsNotThenable(Boxed, const Type&, BranchTargets);
    void branchOnIsWatched(TypedPointer inlineWatchpointSet, BranchTargets);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
