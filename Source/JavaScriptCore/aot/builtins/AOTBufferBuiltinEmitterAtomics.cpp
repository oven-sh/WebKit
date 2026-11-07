/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBufferBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTConversionEmitter.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include "AOTTypedArrayKinds.h"
#include "AOTVector.h"
#include "CCallHelpers.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct AtomicAccessGenerator {
    AtomicsOperation operation;
    Width width;

    void operator()(CCallHelpers&, const Machine::PatchParameters&) const;
};

#if CPU(ARM64)

bool hasAtomicReadModifyWriteInstructions(const Machine::Procedure& procedure)
{
    return procedure.traits().features.contains(CPUFeature::LSE);
}

void AtomicAccessGenerator::operator()(CCallHelpers& jit, const Machine::PatchParameters& params) const
{
    unsigned firstOperand = operation == AtomicsOperation::Store ? 0 : 1;
    CCallHelpers::Address memory(params[firstOperand].gpr());
    auto operandAt = [&](unsigned index) {
        return params[firstOperand + 1 + index].gpr();
    };
    auto loadLinked = [&](GPRReg destination) {
        switch (width) {
        case Width8:
            jit.loadLinkAcq8(memory, destination);
            return;
        case Width16:
            jit.loadLinkAcq16(memory, destination);
            return;
        case Width32:
            jit.loadLinkAcq32(memory, destination);
            return;
        default:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto storeConditionally = [&](GPRReg source, GPRReg status) {
        switch (width) {
        case Width8:
            jit.storeCondRel8(source, memory, status);
            return;
        case Width16:
            jit.storeCondRel16(source, memory, status);
            return;
        case Width32:
            jit.storeCondRel32(source, memory, status);
            return;
        default:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    if (operation == AtomicsOperation::Load) {
        GPRReg result = params[0].gpr();
        switch (width) {
        case Width8:
            jit.loadAcq8(memory, result);
            return;
        case Width16:
            jit.loadAcq16(memory, result);
            return;
        case Width32:
            jit.loadAcq32(memory, result);
            return;
        default:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    if (operation == AtomicsOperation::Store) {
        switch (width) {
        case Width8:
            jit.storeRel8(operandAt(0), memory);
            return;
        case Width16:
            jit.storeRel16(operandAt(0), memory);
            return;
        case Width32:
            jit.storeRel32(operandAt(0), memory);
            return;
        default:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    GPRReg result = params[0].gpr();
    if (hasAtomicReadModifyWriteInstructions(params.proc())) {
        auto readModifyWrite = [&](auto on8Bits, auto on16Bits, auto on32Bits, GPRReg operand) {
            RELEASE_ASSERT(width == Width8 || width == Width16 || width == Width32);
            (jit.*(width == Width8 ? on8Bits : width == Width16 ? on16Bits : on32Bits))(operand, memory, result);
        };
        switch (operation) {
        case AtomicsOperation::Add:
            readModifyWrite(&CCallHelpers::atomicXchgAdd8, &CCallHelpers::atomicXchgAdd16, &CCallHelpers::atomicXchgAdd32, operandAt(0));
            return;
        case AtomicsOperation::Sub:
            jit.neg32(operandAt(0), result);
            readModifyWrite(&CCallHelpers::atomicXchgAdd8, &CCallHelpers::atomicXchgAdd16, &CCallHelpers::atomicXchgAdd32, result);
            return;
        case AtomicsOperation::And:
            jit.not32(operandAt(0), result);
            readModifyWrite(&CCallHelpers::atomicXchgClear8, &CCallHelpers::atomicXchgClear16, &CCallHelpers::atomicXchgClear32, result);
            return;
        case AtomicsOperation::Or:
            readModifyWrite(&CCallHelpers::atomicXchgOr8, &CCallHelpers::atomicXchgOr16, &CCallHelpers::atomicXchgOr32, operandAt(0));
            return;
        case AtomicsOperation::Xor:
            readModifyWrite(&CCallHelpers::atomicXchgXor8, &CCallHelpers::atomicXchgXor16, &CCallHelpers::atomicXchgXor32, operandAt(0));
            return;
        case AtomicsOperation::Exchange:
            readModifyWrite(&CCallHelpers::atomicXchg8, &CCallHelpers::atomicXchg16, &CCallHelpers::atomicXchg32, operandAt(0));
            return;
        case AtomicsOperation::CompareExchange:
            jit.move(operandAt(0), result);
            switch (width) {
            case Width8:
                jit.atomicStrongCAS8(result, operandAt(1), memory);
                return;
            case Width16:
                jit.atomicStrongCAS16(result, operandAt(1), memory);
                return;
            case Width32:
                jit.atomicStrongCAS32(result, operandAt(1), memory);
                return;
            default:
                break;
            }
            RELEASE_ASSERT_NOT_REACHED();
        case AtomicsOperation::Load:
        case AtomicsOperation::Store:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    GPRReg updated = params.gpScratch(0);
    GPRReg status = params.gpScratch(1);
    CCallHelpers::Label retry = jit.label();
    loadLinked(result);
    if (operation == AtomicsOperation::CompareExchange) {
        CCallHelpers::Jump differs = jit.branch32(CCallHelpers::NotEqual, result, operandAt(0));
        storeConditionally(operandAt(1), status);
        jit.branchTest32(CCallHelpers::NonZero, status).linkTo(retry, &jit);
        CCallHelpers::Jump done = jit.jump();
        differs.link(&jit);
        storeConditionally(result, status);
        jit.branchTest32(CCallHelpers::NonZero, status).linkTo(retry, &jit);
        done.link(&jit);
        return;
    }

    GPRReg stored = updated;
    switch (operation) {
    case AtomicsOperation::Add:
        jit.add32(result, operandAt(0), updated);
        break;
    case AtomicsOperation::Sub:
        jit.sub32(result, operandAt(0), updated);
        break;
    case AtomicsOperation::And:
        jit.and32(result, operandAt(0), updated);
        break;
    case AtomicsOperation::Or:
        jit.or32(result, operandAt(0), updated);
        break;
    case AtomicsOperation::Xor:
        jit.xor32(result, operandAt(0), updated);
        break;
    case AtomicsOperation::Exchange:
        stored = operandAt(0);
        break;
    case AtomicsOperation::Load:
    case AtomicsOperation::Store:
    case AtomicsOperation::CompareExchange:
        RELEASE_ASSERT_NOT_REACHED();
    }
    storeConditionally(stored, status);
    jit.branchTest32(CCallHelpers::NonZero, status).linkTo(retry, &jit);
}

#elif CPU(X86_64)

void AtomicAccessGenerator::operator()(CCallHelpers& jit, const Machine::PatchParameters& params) const
{
    unsigned firstOperand = operation == AtomicsOperation::Store ? 0 : 1;
    CCallHelpers::Address memory(params[firstOperand].gpr());
    auto operandAt = [&](unsigned index) {
        return params[firstOperand + 1 + index].gpr();
    };
    auto load = [&](GPRReg destination) {
        switch (width) {
        case Width8:
            jit.load8(memory, destination);
            return;
        case Width16:
            jit.load16(memory, destination);
            return;
        case Width32:
            jit.load32(memory, destination);
            return;
        default:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto exchange = [&](GPRReg valueAndResult) {
        switch (width) {
        case Width8:
            jit.atomicXchg8(valueAndResult, memory);
            return;
        case Width16:
            jit.atomicXchg16(valueAndResult, memory);
            return;
        case Width32:
            jit.atomicXchg32(valueAndResult, memory);
            return;
        default:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto exchangeAndAdd = [&](GPRReg valueAndResult) {
        switch (width) {
        case Width8:
            jit.atomicXchgAdd8(valueAndResult, memory);
            return;
        case Width16:
            jit.atomicXchgAdd16(valueAndResult, memory);
            return;
        case Width32:
            jit.atomicXchgAdd32(valueAndResult, memory);
            return;
        default:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    switch (operation) {
    case AtomicsOperation::Load:
        load(params[0].gpr());
        return;
    case AtomicsOperation::Store:
        jit.move(operandAt(0), params.gpScratch(0));
        exchange(params.gpScratch(0));
        return;
    case AtomicsOperation::Exchange:
        jit.move(operandAt(0), params[0].gpr());
        exchange(params[0].gpr());
        return;
    case AtomicsOperation::Add:
        jit.move(operandAt(0), params[0].gpr());
        exchangeAndAdd(params[0].gpr());
        return;
    case AtomicsOperation::Sub:
        jit.move(operandAt(0), params[0].gpr());
        jit.neg32(params[0].gpr());
        exchangeAndAdd(params[0].gpr());
        return;
    case AtomicsOperation::CompareExchange: {
        RELEASE_ASSERT(params[0].gpr() == X86Registers::eax && operandAt(0) == X86Registers::eax && operandAt(1) != X86Registers::eax && memory.base != X86Registers::eax);
        switch (width) {
        case Width8:
            jit.atomicStrongCAS8(X86Registers::eax, operandAt(1), memory);
            return;
        case Width16:
            jit.atomicStrongCAS16(X86Registers::eax, operandAt(1), memory);
            return;
        case Width32:
            jit.atomicStrongCAS32(X86Registers::eax, operandAt(1), memory);
            return;
        default:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    case AtomicsOperation::And:
    case AtomicsOperation::Or:
    case AtomicsOperation::Xor:
        break;
    }

    GPRReg updated = params.gpScratch(0);
    RELEASE_ASSERT(params[0].gpr() == X86Registers::eax && operandAt(0) != X86Registers::eax && memory.base != X86Registers::eax);
    load(X86Registers::eax);
    CCallHelpers::Label retry = jit.label();
    jit.move(X86Registers::eax, updated);
    if (operation == AtomicsOperation::And)
        jit.and32(operandAt(0), updated);
    else if (operation == AtomicsOperation::Or)
        jit.or32(operandAt(0), updated);
    else
        jit.xor32(operandAt(0), updated);
    CCallHelpers::Jump failed;
    switch (width) {
    case Width8:
        failed = jit.branchAtomicStrongCAS8(CCallHelpers::Failure, X86Registers::eax, updated, memory);
        break;
    case Width16:
        failed = jit.branchAtomicStrongCAS16(CCallHelpers::Failure, X86Registers::eax, updated, memory);
        break;
    case Width32:
        failed = jit.branchAtomicStrongCAS32(CCallHelpers::Failure, X86Registers::eax, updated, memory);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    failed.linkTo(retry, &jit);
}

#else
#error "Atomics in line need code for this target."
#endif

} // anonymous namespace

LValue BufferBuiltinEmitter::emitAtomicAccess(AtomicsOperation operation, std::span<const LValue> operands, LValue address, Width width)
{
    RELEASE_ASSERT(operands.size() == numberOfValuesTakenBy(operation));
    bool isStore = operation == AtomicsOperation::Store;
    bool isLoad = operation == AtomicsOperation::Load;
    bool comparesWithAccumulator = isX86_64() && (operation == AtomicsOperation::CompareExchange || operation == AtomicsOperation::And || operation == AtomicsOperation::Or || operation == AtomicsOperation::Xor);

    const Machine::AbstractHeap* elements = &m_heaps.entire(AbstractHeap::TypedArrayElements);
    Machine::PatchOperation* access = m_out.patchpoint(isStore ? Machine::Void : Machine::Int32);
    access->effects = Machine::Effects::none();
    m_heaps.decoratePatchpointRead(elements, access);
    m_heaps.decoratePatchpointWrite(elements, access);
    access->setRunsWithoutFrame(true);

#if CPU(X86_64)
    if (comparesWithAccumulator) {
        access->resultConstraints = { Machine::ValueRep::reg(X86Registers::eax) };
        access->append(address, Machine::ValueRep::SomeLateRegister);
        if (operation == AtomicsOperation::CompareExchange) {
            access->append(operands[0], Machine::ValueRep::reg(X86Registers::eax));
            access->append(operands[1], Machine::ValueRep::SomeLateRegister);
        } else {
            access->append(operands[0], Machine::ValueRep::SomeLateRegister);
            access->clobberEarly(Machine::maskOf(Reg(X86Registers::eax)));
            access->numGPScratchRegisters = 1;
        }
        access->setGenerator(AtomicAccessGenerator { operation, width });
        return access;
    }
#else
    UNUSED_VARIABLE(comparesWithAccumulator);
#endif

    if (!isStore && !isLoad)
        access->resultConstraints = { Machine::ValueRep::SomeEarlyRegister };
    access->appendSomeRegister(address);
    for (LValue operand : operands)
        access->appendSomeRegister(operand);
#if CPU(ARM64)
    access->numGPScratchRegisters = isStore || isLoad || hasAtomicReadModifyWriteInstructions(m_proc) ? 0 : 2;
#else
    access->numGPScratchRegisters = isStore;
#endif
    access->setGenerator(AtomicAccessGenerator { operation, width });
    return isStore ? nullptr : access;
}

void BufferBuiltinEmitter::generateAtomics(const InlineCall& call)
{
    AtomicsOperation operation = call.parameter<AtomicsOperation>(0);
    RELEASE_ASSERT(call.numberOfArguments() == 2 + numberOfValuesTakenBy(operation));
    TypedArrayType elements = typedArrayTypeOf(BuiltinTypeContext { m_program, types() }, typeIDOf(call.argument(0)));
    RELEASE_ASSERT(hasIntegerElements(elements));
    Width width = widthForBytes(elementSize(elements));
    LBasicBlock miss = missBlockOf(call);

    CellPointer view = cellOperand(call.argument(0));
    Integer index = offsetOrElse(call.argument(1), miss);
    Vector<LValue, 2> operands;
    for (unsigned i = 2; i < call.numberOfArguments(); ++i) {
        ValueID operand = call.argument(i);
        Word32 bits = typeOf(operand).isInteger() ? low32(integerOperand(operand)) : emitter<ConversionEmitter>().toInt32(numberOperand(operand));
        operands.append(bits.value());
    }
    if (operation == AtomicsOperation::CompareExchange && width != Width32)
        operands[0] = m_out.bitAnd(operands[0], m_out.constInt32(width == Width8 ? 0xff : 0xffff));
    Pointer vector = vectorHoldingElement(view, index, miss);
    LValue address = offsetBy(vector, index, logElementSize(elements)).value();

    LValue access = emitAtomicAccess(operation, operands.span(), address, width);
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

    Word32 bits(access);
    Operand element;
    switch (elements) {
    case TypeInt8:
        element = shiftRight(shiftLeft(bits, 24), 24);
        break;
    case TypeUint8:
        element = bitAnd(bits, 0xff);
        break;
    case TypeInt16:
        element = shiftRight(shiftLeft(bits, 16), 16);
        break;
    case TypeUint16:
        element = bitAnd(bits, 0xffff);
        break;
    case TypeInt32:
        element = bits;
        break;
    case TypeUint32:
        element = zeroExtended(bits);
        break;
    case NotTypedArray:
    case TypeUint8Clamped:
    case TypeFloat16:
    case TypeFloat32:
    case TypeFloat64:
    case TypeBigInt64:
    case TypeBigUint64:
    case TypeDataView:
        RELEASE_ASSERT_NOT_REACHED();
    }
    finish(call, element, types().at(elementTypeOf(elements)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
