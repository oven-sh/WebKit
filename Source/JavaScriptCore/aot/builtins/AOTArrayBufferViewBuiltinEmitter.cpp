/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayBufferViewBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBarrierEmitter.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTypes.h"
#include "AOTByteLiterals.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTResultRules.h"
#include "AOTStringEmitter.h"
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

Integer ArrayBufferViewBuiltinEmitter::lengthOfViewInArgument(const InlineCall& call)
{
    ValueID argument = call.argument(0);
    Boxed value = in<Boxed>(argument);
    if (typeOf(argument).primitives)
        missUnless(call, isCell(value));
    CellPointer view = unboxCell(value);
    missUnless(call, hasCellTypeIn(view, { Int8ArrayType, BigUint64ArrayType }));
    missUnless(call, hasFixedLength(view));
    Integer length = lengthOf(view);
    missUnless(call, isNotZero(length));
    return length;
}

void ArrayBufferViewBuiltinEmitter::generateViewLength(const InlineCall& call)
{
    finish(call, lengthOfViewInArgument(call), bufferSizeRange);
}

void ArrayBufferViewBuiltinEmitter::generateViewIsOutOfBounds(const InlineCall& call)
{
    lengthOfViewInArgument(call);
    finish(call, boolean(false));
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

void ArrayBufferViewBuiltinEmitter::generateByteLiteral(const InlineCall& call)
{
    auto stringOf = [&](ValueID argument) {
        const Operation& definition = operationAt(definitionOf(argument));
        RELEASE_ASSERT(definition.opcode == Opcode::String);
        return definition.string();
    };
    auto kind = call.parameter<ByteLiteralKind>(0);
    ValueID encoding = call.argumentOrNull(1);
    StringID string = stringOf(call.argument(0));
    std::optional<Vector<uint8_t>> bytes = encodeByteLiteral(types(), build().stringEncoders(), kind, string, encoding ? stringOf(encoding) : StringID());
    RELEASE_ASSERT(bytes);
    if (StringEmitter& strings = emitter<StringEmitter>(); m_records.inlinedStrings.areRecorded() && bytes->size() >= strings.textOf(string).length()) [[unlikely]]
        strings.recordInlined(string);
    Pointer address(bytes->isEmpty() ? m_out.constInt64(0) : m_out.relocatable(SymbolSpace::Constant, m_out.addConstantData(bytes->span(), ConstantKind::ByteLiteral)));
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::CopyByteLiteral, { address, word32(static_cast<uint32_t>(bytes->size())), word32(kind == ByteLiteralKind::BufferFrom) }, { .calledBuiltin = functionOf(call.overload()) }));
}
CellPointer ArrayBufferViewBuiltinEmitter::askedAnewWherePositionsAreTested(CellPointer view)
{
    ElementAccessEmitter& accesses = emitter<ElementAccessEmitter>();
    return accesses.checksBounds() ? accesses.askedAnew(view) : view;
}

void ArrayBufferViewBuiltinEmitter::generateTypedArraySet(const InlineCall& call)
{
    BuiltinTypeContext typeContext { m_program, types() };
    TypedArrayType type = typedArrayTypeOf(typeContext, typeIDOf(call.receiver()));
    RELEASE_ASSERT(isTypedView(type) && typedArrayTypeOf(typeContext, typeIDOf(call.argument(0))) == type);
    unsigned shift = logElementSize(type);
    CellPointer target = askedAnewWherePositionsAreTested(cellOperand(call.receiver()));
    CellPointer source = askedAnewWherePositionsAreTested(cellOperand(call.argument(0)));
    ValueID offset = call.argumentOrNull(1);
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

    m_out.appendTo(nothingToMove);
    Condition targetHasMemory = isNotZero(Integer(vectorOf(target).value()));
    Condition sourceHasMemory = isNotZero(Integer(vectorOf(source).value()));
    missUnless(call, logicalAnd(targetHasMemory, sourceHasMemory));
    m_out.jump(done);

    m_out.appendTo(done);
    finishWithUndefined(call);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
