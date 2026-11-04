/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPrivateAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTClassToken.h"
#include "AOTLayout.h"
#include "AOTPlacement.h"
#include "AOTShapeTestEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "JSCJSValueInlines.h"

namespace JSC { namespace AOT { namespace Typed {

uint16_t PrivateAccessEmitter::indexInLayoutRecord(const ValueContext& value)
{
    std::optional<uint16_t> index = m_placement.slot(value->layout(), value->field()).indexInLayoutRecord;
    if (!index)
        fail(value, InternalErrorKind::OpcodeWithoutCode);
    return index.value_or(0);
}

bool PrivateAccessEmitter::isEvaluatedOnce(const ValueContext& value)
{
    const Layout& layout = m_program.at(value->layout());
    return Typed::isEvaluatedOnce(m_program, layout.isDerived() ? m_program.at(declaredLayoutOf(m_program, layout.flow.base)) : layout);
}

// An instance of a class with a built-in parent keeps the name as a property. The stub handles every other object.
bool PrivateAccessEmitter::findsLocationInLine(const ValueContext& value)
{
    return value.shape() == CodeShape::Fast && emitter<ShapeTestEmitter>().hasOnlyCellsWithBrands(typeOf(value->objectOperand()));
}

// Reads the first record of the field. A record is empty until it is filled and keeps what it holds.
TypedPointer PrivateAccessEmitter::locationInKnownStructure(const ValueContext& value, Boxed boxedObject, CellPointer token, LBasicBlock isUnknown)
{
    if (!typeOf(value->objectOperand()).isObject())
        branchUnless(isCell(boxedObject), isUnknown);
    CellPointer object = unboxCell(boxedObject);
    auto wordOfRecord = [&](Pointer records, ClassToken::KnownStructureSlot slot) {
        ptrdiff_t offset = JSCellButterfly::offsetOfData() + (ClassToken::slotOfRecord(indexInLayoutRecord(value), 0) + slot) * sizeof(EncodedJSValue);
        return Word32(m_out.load32(m_out.address(m_heaps.AOTClassToken_records, records.value(), offset)));
    };
    Pointer records = loadField<Pointer>(token, m_heaps.AOTClassToken_knownStructures);
    branchUnless(Condition(m_out.notNull(records.value())), isUnknown);
    branchUnless(equal(loadStructureID(object), wordOfRecord(records, ClassToken::StructureNumber)), isUnknown);
    if (!isEvaluatedOnce(value))
        branchUnless(isZero(wordOfRecord(records, ClassToken::LocationOfBrand)), isUnknown);
    Integer location = signExtended(wordOfRecord(records, ClassToken::LocationOfValue));
    Integer storage = select(lessThan(location, integer(0)), Integer(loadField<Pointer>(object, m_heaps.JSObject_butterfly).value()), Integer(object.value()));
    return TypedPointer(m_heaps.properties.atAnyNumber(), add(storage, location).value());
}

Boxed PrivateAccessEmitter::callPrivateGet(const ValueContext& value, Boxed object, CellPointer token, bool isOnRarePath)
{
    StubKey stub = privateGet();
    uint32_t immediate = indexInLayoutRecord(value);
    // The stub compares the layout of the object with the declaring one, whose slots are known here.
    const PlacedSlot& slot = m_placement.slot(value->layout(), value->field());
    const Field& declaration = fieldAt(m_program, declarationOf(m_program, FieldReference { value->layout(), value->field() }));
    bool isPlainSlotInCell = slot.storage == SlotStorage::Inline && !slot.mayExistWithoutStorage && !declaration.form.contains(FieldForm::IsAccessor);
    if (isPlainSlotInCell && !m_program.at(value->layout()).isDerived() && isEvaluatedOnce(value) && slot.offset < 1u << (32 - bitsOfFieldInPrivateGet)) {
        stub = privateGetFromSlot(slot.representation);
        immediate |= slot.offset << bitsOfFieldInPrivateGet;
    }
    return Boxed(callStub(value, stub, { object.value(), token.value() }, { .immediate = immediate, .isOnRarePath = isOnRarePath }).first);
}

void PrivateAccessEmitter::generatePrivateGet(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    if (!findsLocationInLine(value)) {
        define(value, callPrivateGet(value, object, token, false));
        return;
    }

    LBasicBlock isUnknown = newBlock();
    Join<Boxed> join(m_out);
    join.add(Boxed(m_out.load64(locationInKnownStructure(value, object, token, isUnknown))));
    m_out.appendTo(isUnknown);
    join.add(callPrivateGet(value, object, token, true));
    define(value, join.finish());
}

void PrivateAccessEmitter::storePrivateField(const ValueContext& value, RuntimeOperation operation)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    callVoidOperation(value, operation, { object, token, word32(indexInLayoutRecord(value)), stored });
}

void PrivateAccessEmitter::generatePrivateSet(const ValueContext& value)
{
    if (!findsLocationInLine(value)) {
        storePrivateField(value, RuntimeOperation::PrivateSet);
        return;
    }

    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    LBasicBlock isUnknown = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.store64(stored.value(), locationInKnownStructure(value, object, token, isUnknown));
    emitter<BarrierEmitter>().barrierAfterStore(value, unboxCell(object), stored, typeOf(value->valueOperand()));
    m_out.jump(continuation);

    m_out.appendTo(isUnknown);
    callVoidOperation(value, RuntimeOperation::PrivateSet, { object, token, word32(indexInLayoutRecord(value)), stored }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void PrivateAccessEmitter::generatePrivateDefine(const ValueContext& value)
{
    storePrivateField(value, RuntimeOperation::PrivateDefine);
}

void PrivateAccessEmitter::generatePrivateHas(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    Boxed result = callOperation<Boxed>(value, RuntimeOperation::PrivateHas, { object, token, word32(indexInLayoutRecord(value)) });
    defineTest(value, equal(result, boxedConstant(jsBoolean(true))));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
