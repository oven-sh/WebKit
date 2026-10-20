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
    Integer length = elements.publicLength(elements.elementsOf(in<CellPointer>(value->arrayOperand())));
    defineTest(value, below(elements.indexOrElse(value->positionOperand(), true, nullptr), length));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
