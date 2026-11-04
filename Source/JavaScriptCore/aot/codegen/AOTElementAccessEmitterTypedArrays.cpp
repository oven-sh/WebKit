/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTElementAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTBufferBuiltinEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTGuessedAccessEmitter.h"
#include "AOTResultRules.h"
#include "AOTSlotEmitter.h"
#include "AOTTypeArena.h"
#include "AOTTypedArrayKinds.h"
#include "JSArrayBufferView.h"
#include <limits>

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
    return either<Integer>(hasNoBits(loadByte(view, m_heaps.JSArrayBufferView_mode), resizabilityAndAutoLengthMask), [&] {
        return loadField<Integer>(view, m_heaps.JSArrayBufferView_length);
    }, [&] {
        return callLeaf<Integer>(RuntimeOperation::TypedArrayLength, { view });
    });
}

} // anonymous namespace

// The heap of the vector holds inner pointers, so the view outlives every use of a vector that was loaded under it. Nothing else keeps the view alive.
TypedArrayElements ElementAccessEmitter::typedArrayElementsOf(CellPointer view, LBasicBlock isNotFixedLength)
{
    branchUnless(hasNoBits(loadByte(view, m_heaps.JSArrayBufferView_mode), resizabilityAndAutoLengthMask), isNotFixedLength);
    return { view, loadField<Integer>(view, m_heaps.JSArrayBufferView_length), loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector) };
}

TypedArrayElements ElementAccessEmitter::typedArrayElementsOf(ValueID id, LBasicBlock isNotFixedLength)
{
    CellPointer view = in<CellPointer>(id);
    const Operation& definition = operationAt(definitionOf(id));
    if (definition.opcode != Opcode::CallInline || definition.inlineBuiltin() != InlineBuiltin::TypedArrayWithLength)
        return typedArrayElementsOf(view, isNotFixedLength);
    // A view that is created with a length gets a buffer of its own, and such a buffer never changes its size.
    return { view, loadField<Integer>(view, m_heaps.JSArrayBufferView_length), loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector) };
}

// A test has found the view to have a fixed length and the position to be inside it. Nothing has written the storage of views since the vector that the access names was read.
TypedArrayElements ElementAccessEmitter::typedArrayElementsInsideView(const ValueContext& value, CellPointer view)
{
    std::optional<Pointer> vector = inOptional<Pointer>(value->vectorOperand());
    TypedArrayElements elements { view, loadField<Integer>(view, m_heaps.JSArrayBufferView_length), vector ? *vector : loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector) };
    return elements;
}

Operand ElementAccessEmitter::loadBytesOfTypedArrayElement(const TypedArrayElements& elements, Integer index, TypedArrayType type)
{
    RELEASE_ASSERT(hasInlineElementAccess(type));
    TypedPointer element(m_heaps.TypedArrayProperties, offsetBy(elements.vector, index, logElementSize(type)).value());
    return emitter<SlotEmitter>().load(element, elementStore(type));
}

Operand ElementAccessEmitter::loadTypedArrayElement(const TypedArrayElements& elements, Integer index, TypedArrayType type)
{
    Operand loaded = loadBytesOfTypedArrayElement(elements, index, type);
    // The bytes may spell any NaN. Whoever boxes a number, or keeps it beside markers, takes it to hold the one NaN.
    if (isFloat(type))
        return emitter<ConversionEmitter>().purified(loaded.as<Number>());
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

// Loaded under the heap of inner pointers, like every vector: the view outlives the last access that takes the result, however far from here it stands.
void ElementAccessEmitter::generateGetTypedArrayVector(const ValueContext& value)
{
    build().statistics().count(StatisticID::VectorsOfViewsRead);
    define(value, loadField<Pointer>(in<CellPointer>(value->viewOperand()), m_heaps.JSArrayBufferView_vector));
}

void ElementAccessEmitter::generateGetTypedArrayElement(const ValueContext& value)
{
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateElementRead(value);
        return;
    }

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

    Type elementType = types().at(elementTypeOf(type));
    if (value->isInsideView()) {
        build().statistics().count(StatisticID::AccessesInsideViews);
        Integer index = indexInsideLength(value->positionOperand());
        TypedArrayElements elements = typedArrayElementsInsideView(value, view);
        if (readsNumberOutOfBytes(value.value())) {
            define(value, generator().settleNaNOf(value, loadBytesOfTypedArrayElement(elements, index, type).as<Number>()));
            return;
        }
        Operand element = loadTypedArrayElement(elements, index, type);
        define(value, TypedArraySupport(generator()).inRepresentation(value, representation, element, elementType));
        return;
    }

    LBasicBlock outside = newBlock();
    LBasicBlock otherwise = newBlock();
    OperandJoin join(m_out, representation);
    ScopeFork fork = m_out.forkScope();

    Integer index = indexOrElse(value->positionOperand(), value->keyIsIntegral(), outside);
    TypedArrayElements elements = typedArrayElementsOf(value->viewOperand(), otherwise);
    branchUnless(below(index, elements.length), outside);
    Operand element = loadTypedArrayElement(elements, index, type);
    join.add(TypedArraySupport(generator()).inRepresentation(value, representation, element, elementType));

    m_out.appendToJoin(outside, fork);
    join.add(absentElement(value, representation));

    m_out.appendToJoin(otherwise, fork);
    join.add(readByValue());
    define(value, join.finishBehind(fork));
}

void ElementAccessEmitter::generatePutTypedArrayElement(const ValueContext& value)
{
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateElementStore(value);
        return;
    }

    TypedArrayType type = value->typedArrayType();
    ValueID storedValue = value->valueOperand();
    if (!typeOf(storedValue).hasOnlyPrimitives(isBigInt(type) ? Primitive::BigInt : Primitive::Number)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    TypedArraySupport support(generator());
    CellPointer view = in<CellPointer>(value->viewOperand());
    // A view of numbers takes the bits of a NaN as they are, and in a view of integers every NaN is 0.
    Operand stored = takesAnyNaN(value.value(), OperandIndex::PutTypedArrayElement::value) ? generator().getWithAnyNaN(storedValue) : get(storedValue);
    bool storesInline = hasInlineElementAccess(type) && !stored.is<Boxed>() && !(isClamped(type) && stored.is<Number>());
    auto elementToStore = [&] {
        return isFloat(type) && !stored.is<Number>() ? Operand(support.numberOperand(storedValue)) : stored;
    };
    // A store that takes the operation has no use for the test that has passed: the operation tests the position for itself.
    if (value->isInsideView() && storesInline) {
        build().statistics().count(StatisticID::AccessesInsideViews);
        Integer index = indexInsideLength(value->positionOperand());
        TypedArrayElements elements = typedArrayElementsInsideView(value, view);
        storeTypedArrayElement(elements, index, type, elementToStore());
        return;
    }

    LBasicBlock done = newBlock();
    ScopeFork fork = m_out.forkScope();
    Integer index = indexOrElse(value->positionOperand(), value->keyIsIntegral(), done);
    if (isBigInt(type)) {
        Integer length = support.lengthOf(view);
        branchUnless(below(index, length), done);
        TypedArrayElements elements { view, length, loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector) };
        storeTypedArrayElement(elements, index, type, emitter<BufferBuiltinEmitter>().low64Of(support.cellOperand(storedValue)));
        m_out.jump(done);

        m_out.appendToJoin(done, fork);
        return;
    }

    LBasicBlock otherwise = newBlock();
    if (storesInline) {
        TypedArrayElements elements = typedArrayElementsOf(value->viewOperand(), otherwise);
        branchUnless(below(index, elements.length), done);
        storeTypedArrayElement(elements, index, type, elementToStore());
        m_out.jump(done);
    } else
        m_out.jump(otherwise);

    m_out.appendToJoin(otherwise, fork);
    callVoidOperation(value, RuntimeOperation::TypedArrayPut, { view, index, support.numberOperand(storedValue) }, { .isOnRarePath = storesInline });
    m_out.jump(done);

    m_out.appendToJoin(done, fork);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
