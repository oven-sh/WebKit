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
    explicit GuardEmitter(CodeGenerator&);

    void generateShapeTest(const ValueContext&);
    void generateIsSlotFilled(const ValueContext&);
    void generateHasNoExtraProperties(const ValueContext&);
    void generateIsNotFrozen(const ValueContext&);
    void generateHasLayoutPrototype(const ValueContext&);
    void generateBelongsToInstance(const ValueContext&);
    void generateIsPristine(const ValueContext&);
    void generateHasArrayStorage(const ValueContext&);
    void generateIsInBounds(const ValueContext&);
    void generateIsNotThenable(const ValueContext&);
    void generateIsMemberUnchanged(const ValueContext&);
    void generateIsPrototypeLinkUnchanged(const ValueContext&);
    void generateIsClassEvaluatedOnce(const ValueContext&);
    void generateIsRequireEdgeReady(const ValueContext&);
    void generateHasKeyOrder(const ValueContext&);
    void generateIsShapeEpochCurrent(const ValueContext&);
    void generateIsClosureOf(const ValueContext&);
    void generateIsEnumeratorCurrent(const ValueContext&);

    Condition isGuardBitSet(GuardBitID);
    Condition belongsToInstance(CellPointer);
    Condition isNotThenable(const OperationContext&, Boxed, const Type&);

private:
    Condition isPresentCell(Boxed, const Type&);
    CellPointer untestedCell(const OperationContext&, ValueID);
    CellPointer cellOrElse(const OperationContext&, ValueID, B3::FrequentedBlock isNotCell);

    Condition hasInstanceRealm(Pointer structure);
    void branchOnIsNotThenable(Boxed, const Type&, BranchTargets);
    void branchOnIsWatched(TypedPointer inlineWatchpointSet, BranchTargets);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
