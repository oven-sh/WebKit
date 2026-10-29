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
    void generateIsNotFrozen(const ValueContext&);
    void generateHasLayoutPrototype(const ValueContext&);
    void generateBelongsToInstance(const ValueContext& value) { defineTest(value, belongsToInstance(in<CellPointer>(value->objectOperand()))); }
    void generateIsPristine(const ValueContext&);
    void generateHasArrayStorage(const ValueContext&);
    void generateIsInBounds(const ValueContext&);
    void generateIsElementPresent(const ValueContext&);
    void generateIsInsideView(const ValueContext&);
    void generateIsNotThenable(const ValueContext&);
    void generateAreMembersUnchanged(const ValueContext& value) { defineTest(value, holds(value->guardBit())); }
    void generateHasKeyOrder(const ValueContext&);
    void generateIsClosureOf(const ValueContext&);
    void generateIsEnumeratorCurrent(const ValueContext&);

    Condition holds(GuardBitID);
    Condition belongsToInstance(CellPointer cell) { return hasInstanceRealm(loadStructure(cell)); }
    // Whether the cell is a compiled closure that inherits from the realm's Function.prototype and has only the properties it was created with.
    Condition isClosureAsCreated(CellPointer, BuiltinClassID, OptionSet<PristineAspects>);
    Condition isNotThenable(const OperationContext&, Boxed, const Type&);

private:
    Condition isPresentCell(Boxed, const Type&);
    bool standsInFrontOfReadsAlone(const ValueContext& test);
    uint8_t flagsTestedBy(const Value& test);
    void planTestsOfFlags();
    Condition testOfFlags(const ValueContext&);
    // These tests read words of compiled objects. A cell of a built-in class fails them.
    void defineTestOfCompiledObject(const ValueContext&, NOESCAPE const Invocable<Condition()> auto& test);
    Condition orderWordHasNoBits(const ValueContext&, uint32_t bits);
    CellPointer untestedCell(const OperationContext&, ValueID);
    CellPointer cellOrElse(const OperationContext&, ValueID, B3::FrequentedBlock isNotCell);

    Condition hasInstanceRealm(Pointer structure);
    void branchOnIsNotThenable(Boxed, const Type&, BranchTargets);
    void branchOnIsWatched(TypedPointer inlineWatchpointSet, BranchTargets);

    bool m_didPlanTestsOfFlags { false };
    HashMap<ValueID, uint8_t> m_flagsToTest;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
