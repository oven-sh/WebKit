/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTRuntimeValidation.h"
#include "AOTThreshold.h"
#include "JSType.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static constexpr const ArrayStorageForm& int32s = arrayStorageFormOf(ArrayStorage::Int32);
static constexpr const ArrayStorageForm& doubles = arrayStorageFormOf(ArrayStorage::Double);
static constexpr const ArrayStorageForm& contiguous = arrayStorageFormOf(ArrayStorage::Contiguous);

static_assert(int32s.hole == contiguous.hole && int32s.element != Representation::Double && contiguous.element == Representation::JSValue);
static_assert(doubles.element == Representation::Double);

ArrayIntrinsicEmitter::ArrayIntrinsicEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

Condition ArrayIntrinsicEmitter::isPristineArrayValue(ValueID id)
{
    static_assert(DerivedArrayType == ArrayType + 1);
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    LBasicBlock isNotObject = newBlock();
    Join<Condition> join(m_out);
    CellPointer object = objectOrElse(id, isNotObject);
    Condition isArray = hasCellTypeIn(object, { ArrayType, DerivedArrayType });
    join.add(logicalAnd(isArray, elements.isPristineArray(object, { PristineAspects::Storage, PristineAspects::Names })));

    m_out.appendTo(isNotObject);
    join.add(boolean(false));
    return join.finish();
}

void ArrayIntrinsicEmitter::generateIsPristineArray(const ValueContext& value)
{
    defineTest(value, isPristineArrayValue(value->valueOperand()));
}

void ArrayIntrinsicEmitter::generateArrayStorageKind(const ValueContext& value)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    Word32 shape = elements.indexingMode(in<CellPointer>(value->arrayOperand()), arrayStorageTestMask);
    Word32 kind = word32(static_cast<int32_t>(ArrayStorage::Other));
    for (const ArrayStorageForm& form : arrayStorageForms)
        kind = select(elements.hasShape(shape, form), word32(static_cast<int32_t>(form.kind)), kind);
    define(value, kind);
}

void ArrayIntrinsicEmitter::generateGetDoubleIndex(const ValueContext& value)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    LBasicBlock otherwise = newBlock();
    Join<Number> join(m_out);
    branchUnless(elements.hasShape(elements.indexingMode(array, arrayStorageTestMask), doubles), otherwise);
    ArrayElements storage = elements.elementsOf(array);
    branchUnless(below(index, elements.publicLength(storage)), otherwise);
    join.add(elements.loadRawElement(doubles, storage, index).as<Number>());

    m_out.appendTo(otherwise);
    join.add(elements.rawHole(doubles).as<Number>());
    define(value, join.finish());
}

Boxed ArrayIntrinsicEmitter::contiguousElementOrEmpty(CellPointer array, Integer index)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    LBasicBlock otherwise = newBlock();
    Join<Boxed> join(m_out);
    Word32 shape = elements.indexingMode(array, arrayStorageTestMask);
    branchUnless(logicalOr(elements.hasShape(shape, contiguous), elements.hasShape(shape, int32s)), otherwise);
    ArrayElements storage = elements.elementsOf(array);
    branchUnless(below(index, elements.publicLength(storage)), otherwise);
    Boxed element(m_out.load64(elements.elementOfUnknownForm(storage, index)));
    ensureStillAliveHere(array);
    join.add(element);

    m_out.appendTo(otherwise);
    join.add(elements.rawHole(contiguous).as<Boxed>());
    return join.finish();
}

void ArrayIntrinsicEmitter::generateHasContiguousIndex(const ValueContext& value)
{
    Boxed element = contiguousElementOrEmpty(in<CellPointer>(value->arrayOperand()), integerOperand(value->positionOperand()));
    define(value, isNotEmpty(element));
}

void ArrayIntrinsicEmitter::generateGetContiguousIndex(const ValueContext& value)
{
    Boxed element = contiguousElementOrEmpty(in<CellPointer>(value->arrayOperand()), integerOperand(value->positionOperand()));
    defineAs(value, select(isNotEmpty(element), element, undefined()), typeOf(value.id()));
}

void ArrayIntrinsicEmitter::putInBounds(const ValueContext& value, const ArrayStorageForm& form)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    ValueID stored = value->valueOperand();
    Operand rawElement = get(stored);
    LBasicBlock isInvalid = newBlock();
    LBasicBlock isValid = newBlock();
    branchUnless(elements.hasWritableShape(elements.indexingMode(array, IndexingShapeAndWritabilityMask), form), isInvalid);
    ArrayElements storage = elements.elementsOf(array);
    branch(below(index, elements.publicLength(storage)), { normalEdge(isValid), rareEdge(isInvalid) });

    m_out.appendTo(isInvalid);
    callVoidOperation(value, RuntimeOperation::ReportValidationFailure, { boxCell(array), word32(static_cast<int32_t>(ValidationFailureKind::ValueType)), word32(m_id.index()), word32(value.id().index()) }, { .isOnRarePath = true });
    m_out.unreachable();

    m_out.appendTo(isValid);
    elements.storeRawElement(form, storage, index, rawElement);
    if (form.holdsCells)
        barrierAfterStore(value, array, rawElement, typeOf(stored));
}

void ArrayIntrinsicEmitter::generatePutContiguousIndex(const ValueContext& value)
{
    putInBounds(value, contiguous);
}

void ArrayIntrinsicEmitter::generatePutDoubleIndex(const ValueContext& value)
{
    putInBounds(value, doubles);
}

const ArrayStorageForm& ArrayIntrinsicEmitter::formToConstruct(const ValueContext& value) const
{
    const ArrayStorageForm* form = findArrayStorageForm(value->arrayStorage());
    return form ? *form : contiguous;
}

CellPointer ArrayIntrinsicEmitter::sourceOrNull(ValueID id)
{
    CellPointer null(m_out.int64Zero);
    if (!id || typeOf(id).isUndefinedOrNull())
        return null;
    Operand operand = get(id);
    switch (operand.representation) {
    case Representation::Cell:
        return operand.as<CellPointer>();
    case Representation::Cell32: {
        NarrowReference reference = operand.as<NarrowReference>();
        return select(isCellReference(reference), decode(reference), null);
    }
    case Representation::JSValue: {
        Boxed boxed = operand.as<Boxed>();
        return select(isCell(boxed), unboxCell(boxed), null);
    }
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::LiteralString:
        return null;
    case Representation::Unchosen:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ArrayIntrinsicEmitter::newArrayOfHoles(const ValueContext& value, Integer length, ValueID source, const ArrayStorageForm& form, uint32_t flags)
{
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    join.add(ArrayAllocationEmitter(generator()).allocateArrayOfHoles(value, length, form, slowPath));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::NewArrayWithHoles, { sourceOrNull(source), length, word32(form.indexingType), word32(flags) }, { .isOnRarePath = true }));
    define(value, join.finish());
}

void ArrayIntrinsicEmitter::generateNewDoubleArray(const ValueContext& value)
{
    newArrayOfHoles(value, integerOperand(value->lengthOperand()), ValueID(), doubles, NewArrayWithHolesFlag::mustHaveIndexingType);
}

void ArrayIntrinsicEmitter::generateNewContiguousArray(const ValueContext& value)
{
    newArrayOfHoles(value, integerOperand(value->lengthOperand()), ValueID(), contiguous, NewArrayWithHolesFlag::mustHaveIndexingType);
}

void ArrayIntrinsicEmitter::generateNewArrayLike(const ValueContext& value)
{
    const Type& length = typeOf(value->lengthOperand());
    bool isEmpty = length.isInteger() && length.integers == Range::one(0);
    newArrayOfHoles(value, isEmpty ? integer(0) : integerOperand(value->lengthOperand()), value->sourceOperand(), formToConstruct(value), 0);
}

void ArrayIntrinsicEmitter::generateNewArrayWithCapacity(const ValueContext& value)
{
    Integer capacity = integerOperand(value->lengthOperand());
    Integer maxCapacity = integer(static_cast<int64_t>(build().limit(LimitID::MaxFusedArrayCapacity)));
    Integer length = select(below(capacity, maxCapacity), capacity, maxCapacity);
    newArrayOfHoles(value, length, value->sourceOperand(), formToConstruct(value), NewArrayWithHolesFlag::lengthIsCapacity);
}

Condition ArrayIntrinsicEmitter::tryStore(const ValueContext& value, const ArrayStorageForm& form, CellPointer array, Integer index, ValueID stored)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    LBasicBlock otherwise = newBlock();
    std::optional<Operand> rawElement = elements.rawElementToStore(form, stored, otherwise);
    if (!rawElement) {
        m_out.jump(otherwise);
        m_out.appendTo(otherwise);
        return boolean(false);
    }

    Join<Condition> join(m_out);
    branchUnless(elements.hasWritableShape(elements.indexingMode(array, IndexingShapeAndWritabilityMask), form), otherwise);
    ArrayElements storage = elements.elementsOf(array);
    branchUnless(below(index, elements.publicLength(storage)), otherwise);
    elements.storeRawElement(form, storage, index, *rawElement);
    if (form.holdsCells)
        barrierAfterStore(value, array, *rawElement, typeOf(stored));
    join.add(boolean(true));

    m_out.appendTo(otherwise);
    join.add(boolean(false));
    return join.finish();
}

void ArrayIntrinsicEmitter::generateTryPutByValDirect(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    define(value, tryStore(value, formToConstruct(value), array, index, value->valueOperand()));
}

void ArrayIntrinsicEmitter::generateTryPutIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    define(value, emitter<ElementAccessEmitter>().tryStoreElement(value, array, index, value->valueOperand()));
}

void ArrayIntrinsicEmitter::generateGrowNewArray(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::GrowNewArray, { in<CellPointer>(value->arrayOperand()), integerOperand(value->lengthOperand()) });
}

void ArrayIntrinsicEmitter::generateTruncateNewArray(const ValueContext& value)
{
    AOT_THRESHOLD(int64_t, smallUnusedCapacity, 8);
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer length = integerOperand(value->lengthOperand());
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    Word32 mode = elements.indexingMode(array, IndexingShapeAndWritabilityMask);
    Condition hasForm;
    for (const ArrayStorageForm& form : arrayStorageForms)
        hasForm = logicalOr(hasForm, elements.hasWritableShape(mode, form));
    branchUnless(hasForm, slowPath);
    branchUnless(isNotZero(ArrayAllocationEmitter(generator()).headerOfNewArrayWithShape(mode)), slowPath);
    ArrayElements storage = elements.elementsOf(array);
    Integer capacity = elements.publicLength(storage);
    branchUnless(belowOrEqual(length, capacity), slowPath);
    Integer unusedCapacity = sub(capacity, length);
    branchUnless(logicalOr(belowOrEqual(unusedCapacity, integer(smallUnusedCapacity)), belowOrEqual(unusedCapacity, length)), slowPath);
    elements.setPublicLength(storage, length);
    join.add(array);

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::TruncateNewArray, { array, low32(length) }, { .isOnRarePath = true }));
    define(value, join.finish());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
