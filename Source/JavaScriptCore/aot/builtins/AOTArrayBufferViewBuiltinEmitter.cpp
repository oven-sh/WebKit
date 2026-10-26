/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayBufferViewBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBarrierEmitter.h"
#include "AOTBuiltinTypes.h"
#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTStubsAllocation.h"
#include "AOTTypedArrayKinds.h"
#include "JSArrayBufferView.h"
#include "PageCount.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr Range bufferSizeRange { 0, static_cast<int64_t>(MAX_ARRAY_BUFFER_SIZE) };

ArrayBufferViewBuiltinEmitter::ArrayBufferViewBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

Condition ArrayBufferViewBuiltinEmitter::hasFixedLength(CellPointer view)
{
    return hasNoBits(loadByte(view, m_heaps.JSArrayBufferView_mode), isResizableOrGrowableSharedMode);
}

Integer ArrayBufferViewBuiltinEmitter::lengthOf(CellPointer view)
{
    return loadField<Integer>(view, m_heaps.JSArrayBufferView_length);
}

Integer ArrayBufferViewBuiltinEmitter::byteOffsetOf(CellPointer view)
{
    return loadField<Integer>(view, m_heaps.JSArrayBufferView_byteOffset);
}

Pointer ArrayBufferViewBuiltinEmitter::vectorOf(CellPointer view)
{
    return loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector);
}

Pointer ArrayBufferViewBuiltinEmitter::findTypedArrayElement(CellPointer view, Integer index, LBasicBlock otherwise, LBasicBlock outside)
{
    Condition isFixed = hasFixedLength(view);
    Condition isInside = below(index, select(isFixed, lengthOf(view), integer(0)));
    if (otherwise == outside) {
        branchUnless(isInside, otherwise);
        return vectorOf(view);
    }
    LBasicBlock inside = newBlock();
    LBasicBlock notInside = newBlock();
    m_out.branch(isInside.value(), normalEdge(inside), rareEdge(notInside));
    m_out.appendTo(notInside);
    m_out.branch(isFixed.value(), normalEdge(outside), rareEdge(otherwise));
    m_out.appendTo(inside);
    return vectorOf(view);
}

Operand ArrayBufferViewBuiltinEmitter::loadElement(CellPointer view, Pointer vector, Integer index, TypedArrayType type)
{
    return emitter<ElementAccessEmitter>().loadTypedArrayElement(TypedArrayElements { view, { }, vector }, index, type);
}

void ArrayBufferViewBuiltinEmitter::generateTypedArrayLength(const InlineCall& call)
{
    CellPointer view = cellOperand(call.receiver());
    missUnless(call, hasFixedLength(view));
    finish(call, lengthOf(view), bufferSizeRange);
}

void ArrayBufferViewBuiltinEmitter::generateTypedArrayByteLength(const InlineCall& call)
{
    TypedArrayType type = typedArrayTypeOf(m_builtins, typeOf(call.receiver()));
    RELEASE_ASSERT(isTypedView(type));
    CellPointer view = cellOperand(call.receiver());
    missUnless(call, hasFixedLength(view));
    finish(call, Integer(m_out.shl(lengthOf(view).value(), m_out.constInt32(logElementSize(type)))), bufferSizeRange);
}

void ArrayBufferViewBuiltinEmitter::generateTypedArrayByteOffset(const InlineCall& call)
{
    CellPointer view = cellOperand(call.receiver());
    missUnless(call, hasFixedLength(view));
    finish(call, byteOffsetOf(view), bufferSizeRange);
}

void ArrayBufferViewBuiltinEmitter::generateTypedArrayWithLength(const InlineCall& call)
{
    BuiltinClassID constructedClass = m_builtins.at(m_builtins.at(call.overload()).builtin).instanceClass;
    RELEASE_ASSERT(constructedClass);
    TypedArrayType type = m_builtins.at(constructedClass).typedArrayType;
    RELEASE_ASSERT(isTypedView(type));
    StubCallResult view = callStub(call.context(), newTypedArray(), { word32(type).value(), numberOperand(call.argument(0)).value() }, { .calledBuiltin = functionOf(call.overload()) });
    finish(call, CellPointer(view.first));
}

void ArrayBufferViewBuiltinEmitter::generateArrayFrom(const InlineCall& call)
{
    ValueID items = call.argument(0);
    TypedArrayType arrayType = typedArrayTypeOf(m_builtins, typeOf(items));
    RELEASE_ASSERT(hasInlineElementAccess(arrayType));
    Type elementType = elementTypeOf(arrayType);
    ArrayAllocationEmitter allocation(generator());
    ElementAccessEmitter& elementAccess = emitter<ElementAccessEmitter>();
    const ArrayStorageForm& resultForm = allocation.formAtAllocation(resultTypeOf(call));
    const ArrayStorageForm& form = resultForm.kind == ArrayStorage::Int32 && !elementType.isInt32() ? arrayStorageFormOf(ArrayStorage::Double) : resultForm;
    Representation rawRepresentation = form.element == Representation::Double ? Representation::Double : Representation::JSValue;
    LBasicBlock isShort = newBlock();
    LBasicBlock outOfLine = newBlock();
    LBasicBlock copyLoop = newBlock();
    LBasicBlock copy = newBlock();
    LBasicBlock copied = newBlock();
    Join<CellPointer> join(m_out);

    CellPointer source = cellOperand(items);
    missUnless(call, hasFixedLength(source));
    Integer count = lengthOf(source);
    missUnless(call, isNotZero(count));
    Integer maxInlineCount = integer(static_cast<int64_t>(build().limit(LimitID::MaxInlineTypedArrayCopyLength)));
    m_out.branch(belowOrEqual(count, maxInlineCount).value(), normalEdge(isShort), unlikelyEdge(outOfLine));

    m_out.appendTo(isShort);
    CellPointer array = allocation.allocateArrayOfHoles(call.context(), count, form, outOfLine);
    {
        CodeGenerator::InitializationScope scope(generator());
        ArrayElements elements = elementAccess.elementsOf(array);
        Pointer vector = vectorOf(source);
        ValueFromBlock firstIndex = m_out.anchor(integer(0).value());
        m_out.jump(copyLoop);

        m_out.appendTo(copyLoop);
        Integer index(m_out.phi(B3::Int64, firstIndex));
        m_out.branch(below(index, count).value(), normalEdge(copy), normalEdge(copied));

        m_out.appendTo(copy);
        Operand loaded = loadElement(source, vector, index, arrayType);
        // A NaN leaves for the operation.
        if (loaded.is<ImpureNumber>() && rawRepresentation == Representation::Double)
            loaded = Number(loaded.value);
        Operand element = inRepresentation(call.context(), rawRepresentation, loaded, elementType);
        if (element.is<Number>() && !elementType.isInteger())
            branchUnless(equal(element.as<Number>(), element.as<Number>()), outOfLine);
        elementAccess.storeRawElement(form, elements, index, element);
        m_out.addIncomingToPhi(index.value(), m_out.anchor(add(index, integer(1)).value()));
        m_out.jump(copyLoop);

        m_out.appendTo(copied);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    join.add(array);

    m_out.appendTo(outOfLine);
    join.add(callOperation<CellPointer>(call.context(), RuntimeOperation::ArrayFromTypedArray, { source, word32(form.indexingType) }));
    finish(call, join.finish());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
