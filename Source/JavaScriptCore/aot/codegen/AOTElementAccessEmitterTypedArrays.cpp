/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTElementAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTBufferBuiltinEmitter.h"
#include "AOTBuiltinFamilyEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTResultRules.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTTypedArrayKinds.h"
#include "JSArrayBufferView.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

class TypedArraySupport final : public BuiltinFamilyEmitter {
public:
    explicit TypedArraySupport(CodeGenerator& generator)
        : BuiltinFamilyEmitter(generator)
    {
    }

    using BuiltinFamilyEmitter::cellOperand;
    using BuiltinFamilyEmitter::inRepresentation;
    using BuiltinFamilyEmitter::numberOperand;

    Integer lengthOf(CellPointer view);
};

Integer TypedArraySupport::lengthOf(CellPointer view)
{
    LBasicBlock isNotFixedLength = newBlock();
    Join<Integer> join(m_out);
    join.add(emitter<ElementAccessEmitter>().typedArrayElementsOf(view, isNotFixedLength).length);

    m_out.appendTo(isNotFixedLength);
    join.add(callLeaf<Integer>(RuntimeOperation::TypedArrayLength, { view }));
    return join.finish();
}

} // anonymous namespace

TypedArrayElements ElementAccessEmitter::typedArrayElementsOf(CellPointer view, LBasicBlock isNotFixedLength)
{
    branchUnless(hasNoBits(loadByte(view, m_heaps.JSArrayBufferView_mode), resizabilityAndAutoLengthMask), isNotFixedLength);
    return { view, loadField<Integer>(view, m_heaps.JSArrayBufferView_length), loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector) };
}

// The guard has found the view to have a fixed length and the position to be inside it.
TypedArrayElements ElementAccessEmitter::typedArrayElementsInsideView(const ValueContext& value, CellPointer view, Integer index)
{
    TypedArrayElements elements { view, loadField<Integer>(view, m_heaps.JSArrayBufferView_length), loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector) };
    if (generator().validatesOrChecksProofs()) {
        RuntimeValidationEmitter& validation = emitter<RuntimeValidationEmitter>();
        validation.checkPrecondition(value, hasNoBits(loadByte(view, m_heaps.JSArrayBufferView_mode), resizabilityAndAutoLengthMask));
        validation.checkPrecondition(value, below(index, elements.length));
    }
    return elements;
}

Operand ElementAccessEmitter::loadTypedArrayElement(const TypedArrayElements& elements, Integer index, TypedArrayType type)
{
    RELEASE_ASSERT(hasInlineElementAccess(type));
    TypedPointer element(m_heaps.TypedArrayProperties, offsetBy(elements.vector, index, logElementSize(type)).value());
    Operand loaded = emitter<SlotEmitter>().load(element, elementStore(type));
    ensureStillAliveHere(elements.view);
    if (isFloat(type))
        return ImpureNumber(loaded.value);
    return loaded;
}

Word32 ElementAccessEmitter::clampedToUint8(Integer value)
{
    Integer nonNegative = select(lessThan(value, 0), 0, value);
    return low32(select(greaterThan(nonNegative, UINT8_MAX), UINT8_MAX, nonNegative));
}

Word32 ElementAccessEmitter::clampedToUint8(Word32 value)
{
    Word32 nonNegative = select(lessThan(value, 0), 0, value);
    return select(greaterThan(nonNegative, UINT8_MAX), UINT8_MAX, nonNegative);
}

void ElementAccessEmitter::storeTypedArrayElement(const TypedArrayElements& elements, Integer index, TypedArrayType type, Operand stored)
{
    SlotRepresentation slot = elementStore(type);
    if (hasIntegerElements(type)) {
        Word32 bits;
        if (stored.is<Number>())
            bits = emitter<ConversionEmitter>().toInt32(stored.as<Number>());
        else
            bits = stored.is<Integer>() ? low32(stored.as<Integer>()) : stored.as<Word32>();
        stored = slot == SlotRepresentation::Uint32 ? Operand(zeroExtended(bits)) : Operand(bits);
    } else if (isClamped(type))
        stored = stored.is<Word32>() ? clampedToUint8(stored.as<Word32>()) : clampedToUint8(stored.as<Integer>());

    TypedPointer element(m_heaps.TypedArrayProperties, offsetBy(elements.vector, index, logElementSize(type)).value());
    emitter<SlotEmitter>().store(element, slot, stored);
    ensureStillAliveHere(elements.view);
}

void ElementAccessEmitter::generateGetTypedArrayLength(const ValueContext& value)
{
    define(value, TypedArraySupport(generator()).lengthOf(in<CellPointer>(value->viewOperand())));
}

// The length cannot change, the memory is there, and a position in it fits 31 bits.
void ElementAccessEmitter::generateHasFixedBytes(const ValueContext& value)
{
    CellPointer view = in<CellPointer>(value->viewOperand());
    Condition hasFixedLength = hasNoBits(loadByte(view, m_heaps.JSArrayBufferView_mode), resizabilityAndAutoLengthMask);
    Condition hasMemory = isNotZero(Integer(loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector).value()));
    Condition isShort = belowOrEqual(loadField<Integer>(view, m_heaps.JSArrayBufferView_length), std::numeric_limits<int32_t>::max());
    defineTest(value, logicalAnd(logicalAnd(hasFixedLength, hasMemory), isShort));
}

void ElementAccessEmitter::generateTypedArrayRange(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::TypedArrayRange, { in<CellPointer>(value->viewOperand()), integerOf(value->startOperand()), integerOf(value->endOperand()) }));
}

void ElementAccessEmitter::generateGetTypedArrayElement(const ValueContext& value)
{
    TypedArrayType type = value->typedArrayType();
    CellPointer view = in<CellPointer>(value->viewOperand());
    Representation representation = value.representation();
    auto readByValue = [&] {
        return emitter<ConversionEmitter>().unbox(readOutOfLine(value, view), typeOf(value.id()), representation);
    };
    if (!hasInlineElementAccess(type)) {
        define(value, readByValue());
        return;
    }
    if (value->isInsideView()) {
        Integer index = indexInsideLength(value->positionOperand());
        TypedArrayElements elements = typedArrayElementsInsideView(value, view, index);
        Operand element = loadTypedArrayElement(elements, index, type);
        define(value, TypedArraySupport(generator()).inRepresentation(value, representation, element, elementTypeOf(type)));
        return;
    }

    LBasicBlock outside = newBlock();
    LBasicBlock otherwise = newBlock();
    OperandJoin join(m_out, representation);

    Integer index = indexOrElse(value->positionOperand(), value->keyIsIntegral(), outside);
    TypedArrayElements elements = typedArrayElementsOf(view, otherwise);
    branchUnless(below(index, elements.length), outside);
    Operand element = loadTypedArrayElement(elements, index, type);
    join.add(TypedArraySupport(generator()).inRepresentation(value, representation, element, elementTypeOf(type)));

    m_out.appendTo(outside);
    join.add(absentElement(value, representation));

    m_out.appendTo(otherwise);
    join.add(readByValue());
    define(value, join.finish());
}

void ElementAccessEmitter::generatePutTypedArrayElement(const ValueContext& value)
{
    TypedArrayType type = value->typedArrayType();
    ValueID storedValue = value->valueOperand();
    if (!typeOf(storedValue).hasOnlyPrimitives(isBigInt(type) ? Primitive::BigInt : Primitive::Number)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    TypedArraySupport support(generator());
    CellPointer view = in<CellPointer>(value->viewOperand());
    Operand stored = get(storedValue);
    bool storesInline = hasInlineElementAccess(type) && !stored.is<Boxed>() && !(isClamped(type) && stored.is<Number>());
    auto elementToStore = [&] {
        return isFloat(type) ? Operand(support.numberOperand(storedValue)) : stored;
    };
    // A store that takes the operation has no use for the guard: the operation tests the position for itself.
    if (value->isInsideView() && storesInline) {
        Integer index = indexInsideLength(value->positionOperand());
        TypedArrayElements elements = typedArrayElementsInsideView(value, view, index);
        storeTypedArrayElement(elements, index, type, elementToStore());
        return;
    }

    LBasicBlock done = newBlock();
    Integer index = indexOrElse(value->positionOperand(), value->keyIsIntegral(), done);
    if (isBigInt(type)) {
        Integer length = support.lengthOf(view);
        branchUnless(below(index, length), done);
        TypedArrayElements elements { view, length, loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector) };
        storeTypedArrayElement(elements, index, type, emitter<BufferBuiltinEmitter>().low64Of(support.cellOperand(storedValue)));
        m_out.jump(done);

        m_out.appendTo(done);
        return;
    }

    LBasicBlock otherwise = newBlock();
    if (storesInline) {
        TypedArrayElements elements = typedArrayElementsOf(view, otherwise);
        branchUnless(below(index, elements.length), done);
        storeTypedArrayElement(elements, index, type, elementToStore());
        m_out.jump(done);
    } else
        m_out.jump(otherwise);

    m_out.appendTo(otherwise);
    callVoidOperation(value, RuntimeOperation::TypedArrayPut, { view, index, support.numberOperand(storedValue) }, { .isOnRarePath = storesInline });
    m_out.jump(done);

    m_out.appendTo(done);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
