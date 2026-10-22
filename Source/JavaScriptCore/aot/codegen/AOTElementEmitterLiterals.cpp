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
    defineTest(value, emitter<ElementAccessEmitter>().hasArrayStorage(in<CellPointer>(value->arrayOperand()), kind, value->isWritable()));
}

void GuardEmitter::generateIsInBounds(const ValueContext& value)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    // The test stands behind the test or the proof of a form, as the access that it is for does.
    defineTest(value, elements.testLength(elements.indexOrElse(value->positionOperand(), true, nullptr), elements.elementsOf(in<CellPointer>(value->arrayOperand()))).isInside);
}

void GuardEmitter::generateIsElementPresent(const ValueContext& value)
{
    const ArrayStorageForm* form = findArrayStorageForm(value->arrayStorage());
    if (!form) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        defineTest(value, boolean(false));
        return;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        emitter<ElementAccessEmitter>().branchOnPresentElement(value, *form, targets);
    });
}

// A view of a detached buffer has the length 0.
void GuardEmitter::generateIsInsideView(const ValueContext& value)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock fails = targets.notTaken.block();
        Integer index = elements.indexOrElse(value->positionOperand(), value->keyIsIntegral(), fails);
        TypedArrayElements view = elements.typedArrayElementsOf(in<CellPointer>(value->viewOperand()), fails);
        branch(below(index, view.length), targets);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
