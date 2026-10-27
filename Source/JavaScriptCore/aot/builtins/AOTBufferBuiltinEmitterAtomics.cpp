/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBufferBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayBufferViewBuiltinEmitter.h"
#include "AOTBuiltinTypes.h"
#include "AOTConversionEmitter.h"
#include "AOTResultRules.h"
#include "AOTTypedArrayKinds.h"

namespace JSC { namespace AOT { namespace Typed {

static unsigned numberOfAtomicsOperands(AtomicsOperation operation)
{
    switch (operation) {
    case AtomicsOperation::Load:
        return 0;
    case AtomicsOperation::Add:
    case AtomicsOperation::And:
    case AtomicsOperation::Exchange:
    case AtomicsOperation::Or:
    case AtomicsOperation::Store:
    case AtomicsOperation::Sub:
    case AtomicsOperation::Xor:
        return 1;
    case AtomicsOperation::CompareExchange:
        return 2;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BufferBuiltinEmitter::generateAtomics(const InlineCall& call)
{
    AtomicsOperation operation = call.parameter<AtomicsOperation>(0);
    RELEASE_ASSERT(call.numberOfArguments() == 2 + numberOfAtomicsOperands(operation));
    TypedArrayType elements = typedArrayTypeOf(m_builtins, typeOf(call.argument(0)));
    RELEASE_ASSERT(hasIntegerElements(elements));
    Width width = widthForBytes(elementSize(elements));
    LBasicBlock miss = missBlockOf(call);

    CellPointer view = cellOperand(call.argument(0));
    Integer index = offsetOrElse(call.argument(1), miss);
    std::array<LValue, 2> operands { };
    for (unsigned i = 2; i < call.numberOfArguments(); ++i) {
        ValueID operand = call.argument(i);
        Word32 bits = typeOf(operand).isInteger() ? low32(integerOperand(operand)) : emitter<ConversionEmitter>().toInt32(numberOperand(operand));
        operands[i - 2] = bits.value();
    }
    Pointer vector = emitter<ArrayBufferViewBuiltinEmitter>().findTypedArrayElement(view, index, miss, miss);
    LValue address = offsetBy(vector, index, logElementSize(elements)).value();

    LValue access = emitAtomicAccess(operation, operands, address, width);
    m_heaps.decorateFencedAccess(&m_heaps.TypedArrayProperties, access);
    ensureStillAliveHere(view);
    if (operation == AtomicsOperation::Store) {
        ValueID stored = call.argument(2);
        if (typeOf(stored).isInteger()) {
            finish(call, get(stored), typeOf(stored));
            return;
        }
        Number value = numberOperand(stored);
        Condition isNotNaN = equal(value, value);
        finish(call, select(isNotNaN, add(trunc(value), 0), 0));
        return;
    }
    LValue bits = access;
    if (elements == TypeUint8)
        bits = m_out.bitAnd(bits, m_out.constInt32(0xff));
    else if (elements == TypeUint16)
        bits = m_out.bitAnd(bits, m_out.constInt32(0xffff));
    Operand element = elements == TypeUint32 ? Operand(zeroExtended(Word32(bits))) : Operand(Word32(bits));
    finish(call, element, elementTypeOf(elements));
}

LValue BufferBuiltinEmitter::emitAtomicAccess(AtomicsOperation operation, std::array<LValue, 2> operands, LValue address, Width width)
{
    TypedPointer pointer(m_heaps.TypedArrayProperties, address);
    FTL::TypedPointer atomicPointer(m_heaps.TypedArrayProperties, address);
    switch (operation) {
    case AtomicsOperation::Add:
        return m_out.atomicXchgAdd(operands[0], atomicPointer, width);
    case AtomicsOperation::And:
        return m_out.atomicXchgAnd(operands[0], atomicPointer, width);
    case AtomicsOperation::CompareExchange:
        return m_out.atomicStrongCAS(operands[0], operands[1], atomicPointer, width);
    case AtomicsOperation::Exchange:
        return m_out.atomicXchg(operands[0], atomicPointer, width);
    case AtomicsOperation::Load:
        return width == Width8 ? m_out.load8SignExt32(pointer) : width == Width16 ? m_out.load16SignExt32(pointer) : m_out.load32(pointer);
    case AtomicsOperation::Or:
        return m_out.atomicXchgOr(operands[0], atomicPointer, width);
    case AtomicsOperation::Store:
        return width == Width8 ? m_out.store32As8(operands[0], pointer) : width == Width16 ? m_out.store32As16(operands[0], pointer) : m_out.store32(operands[0], pointer);
    case AtomicsOperation::Sub:
        return m_out.atomicXchgSub(operands[0], atomicPointer, width);
    case AtomicsOperation::Xor:
        return m_out.atomicXchgXor(operands[0], atomicPointer, width);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
