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
    void generateHasKeyOrder(const ValueContext&);
    void generateAreHeldFieldsFilled(const ValueContext&);
    void generateIsNotFrozen(const ValueContext&);
    void generateHasLayoutPrototype(const ValueContext&);
    void generateIsPristine(const ValueContext&);
    void generateHasArrayStorage(const ValueContext&);
    void generateIsElementPresent(const ValueContext&);
    void generateIsNotThenable(const ValueContext&);
    void generateAreMembersUnchanged(const ValueContext&);
    void generateIsMemberAsBuilt(const ValueContext&);
    void generateIsHolderWithoutAddedNames(const ValueContext&);
    void defineWhetherDeclaredMemberIsFound(const ValueContext&);
    void defineWhetherObjectIsPlainInstance(const ValueContext&);
    void defineWhetherNoMemberIsFound(const ValueContext&);
    void branchOnOwnPropertyByCell(const ValueContext&, BranchTargets, LBasicBlock asksRunTime);
    void generateIsClosureOf(const ValueContext&);
    void generateIsEnumeratorCurrent(const ValueContext&);

    Condition holds(LayoutID classLayout, ClassSide, PropertyKeyID name);
    Condition guardsWereBegun(LayoutID constructorFunction);
    Condition belongsToInstance(CellPointer cell) { return hasInstanceRealm(loadStructure(cell)); }
    Condition areMembersUnchanged(PayloadListView<MemberConditionRecord>);
    Condition areMembersUnchanged(std::span<const MemberConditionRecord>);
    Condition isClosureAsCreated(CellPointer, BuiltinClassID, OptionSet<PristineAspects>);
    Condition isNotThenable(const OperationContext&, Boxed, const Type&);
    Condition realmWatchesSpeciesOfLists();

private:
    void failAndDefineFalse(const ValueContext&, InternalErrorKind);
    Condition guardInByteHolds(LayoutID classLayout, ClassSide, TypedPointer byteOfGuard);
    Condition isPresentCell(Boxed, const Type&);
    void defineTestOfCompiledObject(const ValueContext&, NOESCAPE const Invocable<Condition()> auto& test);
    Condition orderWordHasNoBits(const ValueContext&, uint32_t bits);
    CellPointer untestedCell(const OperationContext&, ValueID);
    CellPointer cellOrElse(const OperationContext&, ValueID, BlockEdge isNotCell);
    CellPointer cellOrElse(const OperationContext&, ValueID, const Type& known, BlockEdge isNotCell);

    Condition hasInstanceRealm(Pointer structure);
    void branchOnIsNotThenable(Boxed, const Type&, BranchTargets);
    void branchOnIsWatched(TypedPointer inlineWatchpointSet, BranchTargets);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
