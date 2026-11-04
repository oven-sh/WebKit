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
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTResultRules.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsElements.h"
#include "AOTTypeArena.h"
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

Condition ArrayBufferViewBuiltinEmitter::haveFixedLength(CellPointer first, CellPointer second)
{
    Word32 modeOfFirst = loadByte(first, m_heaps.JSArrayBufferView_mode);
    Word32 modeOfSecond = loadByte(second, m_heaps.JSArrayBufferView_mode);
    return hasNoBits(bitOr(modeOfFirst, modeOfSecond), isResizableOrGrowableSharedMode);
}

void ArrayBufferViewBuiltinEmitter::moveBytes(const OperationContext& context, CellPointer target, Integer targetOffset, CellPointer source, Integer sourceOffset, Integer count)
{
    Pointer to = offsetBy(vectorOf(target), targetOffset, 0);
    Pointer from = offsetBy(vectorOf(source), sourceOffset, 0);
    Condition areFew = belowOrEqual(count, maxBytesMovedByStub);
    LBasicBlock few = newBlock();
    LBasicBlock many = newBlock();
    LBasicBlock moved = newBlock();
    ScopeFork fork = m_out.forkScope();
    m_out.branch(areFew.value(), normalEdge(few), normalEdge(many));

    // The stub preserves all but two registers, so nothing is saved around the call.
    m_out.appendToArm(few, fork);
    callStub(context, moveFewBytes(), { to.value(), from.value(), count.value() });
    m_out.jump(moved);

    m_out.appendToArm(many, fork);
    callLeaf(RuntimeOperation::MoveBytes, std::initializer_list<Operand> { to, from, count }, Representation::Void);
    m_out.jump(moved);

    m_out.appendToJoin(moved, fork);
    ensureStillAliveHere(target);
    ensureStillAliveHere(source);
}

void ArrayBufferViewBuiltinEmitter::generateTypedArrayLength(const InlineCall& call)
{
    CellPointer view = cellOperand(call.receiver());
    missUnless(call, hasFixedLength(view));
    finish(call, lengthOf(view), bufferSizeRange);
}

void ArrayBufferViewBuiltinEmitter::generateTypedArrayByteLength(const InlineCall& call)
{
    TypedArrayType type = typedArrayTypeOf(BuiltinTypeContext { m_program, types() }, typeIDOf(call.receiver()));
    RELEASE_ASSERT(isTypedView(type));
    CellPointer view = cellOperand(call.receiver());
    missUnless(call, hasFixedLength(view));
    finish(call, Integer(m_out.shl(lengthOf(view).value(), logElementSize(type))), bufferSizeRange);
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
    TypedArrayType type = m_builtins.typedArrayTypeOf(constructedClass);
    RELEASE_ASSERT(isTypedView(type) && m_builtins.typedArrayClassOf(constructedClass) == constructedClass);
    StubCallResult view = callStub(call.context(), newTypedArray(), { word32(type).value(), numberOperand(call.argument(0)).value() }, { .calledBuiltin = functionOf(call.overload()) });
    finish(call, CellPointer(view.first));
}

void ArrayBufferViewBuiltinEmitter::generateTypedArraySet(const InlineCall& call)
{
    BuiltinTypeContext typeContext { m_program, types() };
    TypedArrayType type = typedArrayTypeOf(typeContext, typeIDOf(call.receiver()));
    RELEASE_ASSERT(isTypedView(type) && typedArrayTypeOf(typeContext, typeIDOf(call.argument(0))) == type);
    unsigned shift = logElementSize(type);
    CellPointer target = cellOperand(call.receiver());
    CellPointer source = cellOperand(call.argument(0));
    ValueID offset = call.argumentOrNull(1);
    // A fractional part is truncated, and a number that is not a valid index into the view falls back to the function.
    Integer position = !offset ? integer(0) : typeOf(offset).isInteger() ? integerOperand(offset) : emitter<ConversionEmitter>().toInteger(numberOperand(offset));
    missUnless(call, haveFixedLength(target, source));
    Integer count = lengthOf(source);
    Integer room = lengthOf(target);
    if (offset)
        missUnless(call, belowOrEqual(position, room));
    missUnless(call, belowOrEqual(count, sub(room, position)));
    LBasicBlock move = newBlock();
    LBasicBlock nothingToMove = newBlock();
    LBasicBlock done = newBlock();
    m_out.branch(isNotZero(count).value(), normalEdge(move), unlikelyEdge(nothingToMove));

    m_out.appendTo(move);
    Integer targetOffset(m_out.shl(position.value(), shift));
    Integer numberOfBytes(m_out.shl(count.value(), shift));
    moveBytes(call.context(), target, targetOffset, source, integer(0), numberOfBytes);
    m_out.jump(done);

    // A view of a detached buffer has no elements; the function throws for it.
    m_out.appendTo(nothingToMove);
    Condition targetHasMemory = isNotZero(Integer(vectorOf(target).value()));
    Condition sourceHasMemory = isNotZero(Integer(vectorOf(source).value()));
    missUnless(call, logicalAnd(targetHasMemory, sourceHasMemory));
    m_out.jump(done);

    m_out.appendTo(done);
    finishWithUndefined(call);
}

// `Array.from(bytes)`: the list gets the storage that holds the elements of the view, which are numbers.
// A view whose length follows its buffer misses. So does a view without elements: where its buffer is detached the function throws.
void ArrayBufferViewBuiltinEmitter::generateArrayFrom(const InlineCall& call)
{
    // More elements than these are copied by the operation.
    static constexpr int64_t maxElementsOfViewCopiedInLine = 512;
    ValueID items = call.argument(0);
    TypedArrayType arrayType = typedArrayTypeOf(BuiltinTypeContext { m_program, types() }, typeIDOf(items));
    RELEASE_ASSERT(hasInlineElementAccess(arrayType));
    Type elementType = types().at(elementTypeOf(arrayType));
    ArrayAllocationEmitter allocation(generator());
    ElementAccessEmitter& elementAccess = emitter<ElementAccessEmitter>();
    const ArrayStorageForm& form = arrayStorageFormOf(arrayStorageAtAllocation(types(), elementTypeOf(arrayType)));
    RELEASE_ASSERT(!form.holdsCells);
    // Storage of int32 holds boxed values.
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
    m_out.branch(belowOrEqual(count, integer(maxElementsOfViewCopiedInLine)).value(), normalEdge(isShort), unlikelyEdge(outOfLine));

    m_out.appendTo(isShort);
    CellPointer array = allocation.allocateArrayOfHoles(call.context(), count, form, outOfLine);
    {
        CodeGenerator::InitializationScope scope(generator());
        ArrayElements elements = elementAccess.elementsOf(array);
        TypedArrayElements viewed { source, count, vectorOf(source) };
        ValueFromBlock firstIndex = m_out.anchor(integer(0).value());
        m_out.jump(copyLoop);

        m_out.appendTo(copyLoop);
        Integer index(m_out.phi(Machine::Int64, firstIndex));
        m_out.branch(below(index, count).value(), normalEdge(copy), normalEdge(copied));

        m_out.appendTo(copy);
        Operand element = inRepresentation(call.context(), rawRepresentation, elementAccess.loadTypedArrayElement(viewed, index, arrayType), elementType);
        // NaN marks a hole in storage of doubles. The operation makes the list that holds one, and nobody has seen this one.
        if (element.is<Number>() && !elementType.isInteger())
            branchUnless(equal(element.as<Number>(), element.as<Number>()), outOfLine);
        elementAccess.storeRawElement(form, elements, index, element);
        m_out.addIncomingToPhi(index.value(), m_out.anchor(add(index, integer(1)).value()));
        m_out.jump(copyLoop);

        m_out.appendTo(copied);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    ensureStillAliveHere(source);
    join.add(array);

    m_out.appendTo(outOfLine);
    join.add(callOperation<CellPointer>(call.context(), RuntimeOperation::ArrayFromTypedArray, { source, word32(form.indexingType) }));
    finish(call, join.finish());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
