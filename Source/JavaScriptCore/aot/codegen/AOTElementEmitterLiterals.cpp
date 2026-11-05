/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAllocationEmitter.h"

#if ENABLE(AOT)

#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTGuardEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

void AllocationEmitter::generateNewArray(const ValueContext& value)
{
    ArrayAllocationEmitter allocation(generator());
    define(value, allocation.newLiteral(value));
}

void AllocationEmitter::generateAppendElement(const ValueContext& value)
{
    emitter<ElementAccessEmitter>().appendElement(value, in<CellPointer>(value->arrayOperand()), value->valueOperand());
}

void AllocationEmitter::generateAppendHole(const ValueContext& value)
{
    callOperation<CellPointer>(value, RuntimeOperation::AppendHole, { in<CellPointer>(value->arrayOperand()) });
}

void GuardEmitter::generateHasArrayStorage(const ValueContext& value)
{
    ArrayStorage kind = value->arrayStorage();
    if (!hasArrayStorageForm(kind)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        defineTest(value, boolean(false));
        return;
    }
    if (value->admitsInt32Storage()) {
        defineTest(value, emitter<ElementAccessEmitter>().hasDoubleOrInt32Storage(in<CellPointer>(value->arrayOperand())));
        return;
    }
    defineTest(value, emitter<ElementAccessEmitter>().hasArrayStorage(in<CellPointer>(value->arrayOperand()), kind, value->isWritable()));
}

void GuardEmitter::generateIsInBounds(const ValueContext& value)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    Integer index = elements.indexOrElse(value->positionOperand(), true, nullptr);
    defineTest(value, elements.testLength(value, index, elements.elementsOf(value, in<CellPointer>(value->arrayOperand()))).isInside);
}

void GuardEmitter::generateIsElementPresent(const ValueContext& value)
{
    const ArrayStorageForm* form = findArrayStorageForm(value->arrayStorage());
    defineTestByBranching(value, [&](BranchTargets targets) {
        if (form && value->admitsInt32Storage())
            emitter<ElementAccessEmitter>().branchOnPresentElementInDoubleOrInt32Storage(value, targets);
        else if (form)
            emitter<ElementAccessEmitter>().branchOnPresentElement(value, *form, targets);
        else
            emitter<ElementAccessEmitter>().branchOnPresentElementInAnyStorage(value, targets);
    });
}

void GuardEmitter::generateIsInsideView(const ValueContext& value)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock fails = targets.notTaken.block();
        Integer index = elements.indexOrElse(value->positionOperand(), value->keyIsIntegral(), fails);
        TypedArrayElements view = elements.typedArrayElementsOf(value->viewOperand(), fails);
        branch(value->mayEqualLength() ? belowOrEqual(index, view.length) : below(index, view.length), targets);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
