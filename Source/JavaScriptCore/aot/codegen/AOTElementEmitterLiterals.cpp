/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAllocationEmitter.h"
#include "AOTArrayStorageForms.h"

#if ENABLE(AOT)

#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTGuardEmitter.h"
#include "JSCellButterfly.h"

namespace JSC { namespace AOT { namespace Typed {

void AllocationEmitter::generateNewArray(const ValueContext& value)
{
    if (isListedByArgumentsObjectAlone(value.id())) {
        define(value, CellPointer(loadImmutablePointer(AOT_INSTANCE_OFFSET(realm.emptyArgumentList)).value()));
        return;
    }
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
    if (generator().isHeaderTested(value.id())) {
        defineTest(value, boolean(true));
        return;
    }
    if (kind == ArrayStorage::Other && value->isWritable()) {
        defineTest(value, emitter<ElementAccessEmitter>().takesNumbersByKindOfStorage(in<CellPointer>(value->arrayOperand()), value->admitsInt32Storage()));
        return;
    }
    if (!accessToElementsHasCode(Opcode::HasArrayStorage, kind, value->admitsInt32Storage(), value->isWritable())) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        defineTest(value, boolean(false));
        return;
    }
    const auto* written = value->data() ? m_program.at(value->data()).root.tryAs<DataNode::Array>() : nullptr;
    if (written && !isStorageOfArray(*written)) {
        Pointer storage(m_out.load64(instanceWord(m_heaps.AOTInstance_constantCells, m_mentions.add(NumberSpace::DataStorage, value->data()))));
        defineTest(value, equal(emitter<ElementAccessEmitter>().storageOfList(in<CellPointer>(value->arrayOperand())), offsetBy(storage, JSCellButterfly::offsetOfData())));
        return;
    }
    if (value->data()) {
        Pointer written(m_out.add(loadImmutablePointer(AOT_INSTANCE_OFFSET(image.data)).value(), m_out.zeroExtPtr(numberOf(m_mentions.add(NumberSpace::DataElements, value->data())).value())));
        defineTest(value, equal(emitter<ElementAccessEmitter>().storageOfList(in<CellPointer>(value->arrayOperand())), written));
        return;
    }
    if (value->admitsInt32Storage()) {
        CellPointer list = in<CellPointer>(value->arrayOperand());
        if (value->isWritable()) {
            defineTest(value, kind == ArrayStorage::Contiguous ? emitter<ElementAccessEmitter>().holdsWritableBoxedElements(list) : emitter<ElementAccessEmitter>().hasWritableNumericStorage(list));
            return;
        }
        defineTest(value, kind == ArrayStorage::Contiguous ? emitter<ElementAccessEmitter>().holdsBoxedElements(list) : emitter<ElementAccessEmitter>().hasDoubleOrInt32Storage(list));
        return;
    }
    defineTest(value, emitter<ElementAccessEmitter>().hasArrayStorage(in<CellPointer>(value->arrayOperand()), kind, value->isWritable()));
}

void GuardEmitter::generateIsElementPresent(const ValueContext& value)
{
    if (!accessToElementsHasCode(Opcode::IsElementPresent, value->arrayStorage(), value->admitsInt32Storage(), false)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        defineTest(value, boolean(false));
        return;
    }
    const ArrayStorageForm* form = findArrayStorageForm(value->arrayStorage());
    emitter<ElementAccessEmitter>().holdKindOfList(value);
    defineTestByBranching(value, [&](BranchTargets targets) {
        if (form && form->kind == ArrayStorage::Double && value->admitsInt32Storage())
            emitter<ElementAccessEmitter>().branchOnPresentElementInDoubleOrInt32Storage(value, targets);
        else if (form)
            emitter<ElementAccessEmitter>().branchOnPresentElement(value, *form, targets);
        else
            emitter<ElementAccessEmitter>().branchOnPresentElementInAnyStorage(value, targets);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
