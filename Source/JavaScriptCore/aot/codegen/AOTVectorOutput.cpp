/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVectorOutput.h"

#if ENABLE(AOT)

#include "AllowMacroScratchRegisterUsage.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr SIMDInfo numberLanes { SIMDLane::f64x2, SIMDSignMode::None };

#if CPU(X86_64)
static constexpr uint8_t lastByteOfCVTPS2PD = 0x5a;
static constexpr uint8_t lastByteOfCVTPD2PS = 0x5a;
static constexpr uint8_t lastByteOfPMOVZXBD = 0x31;
static constexpr uint8_t lastByteOfPMOVSXBD = 0x21;
static constexpr uint8_t lastByteOfPMOVZXWD = 0x33;
static constexpr uint8_t lastByteOfPMOVSXWD = 0x23;
static constexpr uint8_t lastByteOfPMOVSXDQ = 0x25;
static constexpr uint8_t lastByteOfUNPCKLPD = 0x14;
static constexpr uint8_t lastByteOfSQRTPD = 0x51;
static constexpr uint8_t lastByteOfPMULLD = 0x40;
static constexpr uint8_t swapHalves = 0x4e;
#endif

template<typename Functor>
LValue VectorOutput::unary(LType type, LValue operand, const Functor& emit)
{
    Machine::PatchOperation* result = m_out.patchpoint(type);
    result->appendSomeRegister(operand);
    result->effects = Machine::Effects::none();
    result->setRunsWithoutFrame(true);
    result->setGenerator([emit](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        emit(jit, params);
    });
    return result;
}

LValue VectorOutput::splat(LValue number)
{
    return unary(Machine::Vector128, number, [](CCallHelpers& jit, const Machine::PatchParameters& params) {
        jit.vectorSplat(SIMDLane::f64x2, params[1].fpr(), params[0].fpr());
    });
}

LValue VectorOutput::insert(LValue first, LValue second)
{
    Machine::PatchOperation* result = m_out.patchpoint(Machine::Vector128);
    result->appendSomeRegister(first);
    result->appendSomeRegister(second);
    result->effects = Machine::Effects::none();
    result->setRunsWithoutFrame(true);
    result->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        FPRReg destination = params[0].fpr();
        FPRReg first = params[1].fpr();
        FPRReg second = params[2].fpr();
        if (destination == second && destination != first) {
            jit.moveVector(second, MacroAssembler::fpTempRegister);
            second = MacroAssembler::fpTempRegister;
        }
        if (destination != first)
            jit.moveVector(first, destination);
#if CPU(X86_64)
        jit.m_assembler.vectorOp_rr(true, false, lastByteOfUNPCKLPD, second, destination);
#else
        jit.vectorReplaceLane(SIMDLane::f64x2, CCallHelpers::TrustedImm32(1), second, destination);
#endif
    });
    return result;
}

LValue VectorOutput::lane(LValue numbers, unsigned index)
{
    RELEASE_ASSERT(index < 2);
    return unary(Machine::Double, numbers, [index](CCallHelpers& jit, const Machine::PatchParameters& params) {
        FPRReg destination = params[0].fpr();
        FPRReg source = params[1].fpr();
        if (!index) {
            jit.moveVector(source, destination);
            return;
        }
#if CPU(X86_64)
        jit.m_assembler.pshufd_i8rr(swapHalves, source, destination);
#else
        jit.vectorExtractLane(SIMDLane::f64x2, CCallHelpers::TrustedImm32(1), source, destination);
#endif
    });
}

LValue VectorOutput::compute(Arithmetic operation, LValue left, LValue right)
{
    Machine::PatchOperation* result = m_out.patchpoint(Machine::Vector128);
    result->appendSomeRegister(left);
    result->appendSomeRegister(right);
    result->effects = Machine::Effects::none();
    result->setRunsWithoutFrame(true);
    result->setGenerator([operation](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        FPRReg destination = params[0].fpr();
        FPRReg left = params[1].fpr();
        FPRReg right = params[2].fpr();
        switch (operation) {
        case Arithmetic::Add:
            jit.vectorAdd(numberLanes, left, right, destination);
            return;
        case Arithmetic::Sub:
            jit.vectorSub(numberLanes, left, right, destination);
            return;
        case Arithmetic::Mul:
            jit.vectorMul(numberLanes, left, right, destination);
            return;
        case Arithmetic::Div:
            jit.vectorDiv(numberLanes, left, right, destination);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    return result;
}

LValue VectorOutput::squareRoots(LValue numbers)
{
    return unary(Machine::Vector128, numbers, [](CCallHelpers& jit, const Machine::PatchParameters& params) {
#if CPU(X86_64)
        jit.m_assembler.vectorOp_rr(true, false, lastByteOfSQRTPD, params[1].fpr(), params[0].fpr());
#else
        jit.vectorSqrt(numberLanes, params[1].fpr(), params[0].fpr());
#endif
    });
}

LValue VectorOutput::spreadBytes(LValue word)
{
    return unary(Machine::Vector128, word, [](CCallHelpers& jit, const Machine::PatchParameters& params) {
        FPRReg destination = params[0].fpr();
        jit.move32ToFloat(params[1].gpr(), destination);
#if CPU(X86_64)
        jit.m_assembler.vectorOp_rr(true, true, lastByteOfPMOVZXBD, destination, destination);
#else
        jit.vectorExtendLow({ SIMDLane::i16x8, SIMDSignMode::Unsigned }, destination, destination);
        jit.vectorExtendLow({ SIMDLane::i32x4, SIMDSignMode::Unsigned }, destination, destination);
#endif
    });
}

LValue VectorOutput::toNumbers(LValue words, unsigned half)
{
    RELEASE_ASSERT(half < 2);
    return unary(Machine::Vector128, words, [half](CCallHelpers& jit, const Machine::PatchParameters& params) {
        FPRReg destination = params[0].fpr();
        FPRReg source = params[1].fpr();
#if CPU(X86_64)
        if (half) {
            jit.m_assembler.pshufd_i8rr(swapHalves, source, destination);
            source = destination;
        }
        jit.m_assembler.cvtdq2pd_rr(source, destination);
#else
        if (half)
            jit.vectorExtendHigh({ SIMDLane::i64x2, SIMDSignMode::Signed }, source, destination);
        else
            jit.vectorExtendLow({ SIMDLane::i64x2, SIMDSignMode::Signed }, source, destination);
        jit.m_assembler.vectorScvtf(destination, destination, SIMDLane::f64x2);
#endif
    });
}

LValue VectorOutput::widenToWords(LValue integers, TypedArrayType type)
{
    return unary(Machine::Vector128, integers, [type](CCallHelpers& jit, const Machine::PatchParameters& params) {
        FPRReg destination = params[0].fpr();
        if (elementSize(type) == 4) {
            jit.moveDouble(params[1].fpr(), destination);
            return;
        }
        FPRReg source = destination;
        if (params[1].isFPR())
            source = params[1].fpr();
        else
            jit.move32ToFloat(params[1].gpr(), destination);
        bool isByte = elementSize(type) == 1;
        bool isSigned = type == TypeInt8 || type == TypeInt16;
#if CPU(X86_64)
        uint8_t lastByte = isByte ? (isSigned ? lastByteOfPMOVSXBD : lastByteOfPMOVZXBD) : isSigned ? lastByteOfPMOVSXWD : lastByteOfPMOVZXWD;
        jit.m_assembler.vectorOp_rr(true, true, lastByte, source, destination);
#else
        SIMDSignMode sign = isSigned ? SIMDSignMode::Signed : SIMDSignMode::Unsigned;
        if (isByte) {
            jit.vectorExtendLow({ SIMDLane::i16x8, sign }, source, destination);
            source = destination;
        }
        jit.vectorExtendLow({ SIMDLane::i32x4, sign }, source, destination);
#endif
    });
}

LValue VectorOutput::singlesToNumbers(LValue singles)
{
    return unary(Machine::Vector128, singles, [](CCallHelpers& jit, const Machine::PatchParameters& params) {
#if CPU(X86_64)
        jit.m_assembler.vectorOp_rr(false, false, lastByteOfCVTPS2PD, params[1].fpr(), params[0].fpr());
#else
        jit.vectorPromote({ SIMDLane::f32x4, SIMDSignMode::None }, params[1].fpr(), params[0].fpr());
#endif
    });
}

LValue VectorOutput::numbersToSingles(LValue numbers)
{
    return unary(Machine::Double, numbers, [](CCallHelpers& jit, const Machine::PatchParameters& params) {
#if CPU(X86_64)
        jit.m_assembler.vectorOp_rr(true, false, lastByteOfCVTPD2PS, params[1].fpr(), params[0].fpr());
#else
        jit.vectorDemote({ SIMDLane::f64x2, SIMDSignMode::None }, params[1].fpr(), params[0].fpr());
#endif
    });
}

static SIMDLane laneOfIntegers(unsigned lanes)
{
    switch (lanes) {
    case 16:
        return SIMDLane::i8x16;
    case 8:
        return SIMDLane::i16x8;
    case 4:
        return SIMDLane::i32x4;
    case 2:
        return SIMDLane::i64x2;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LValue VectorOutput::splatInteger(LValue word, unsigned lanes)
{
    Machine::PatchOperation* result = m_out.patchpoint(Machine::Vector128);
    result->appendSomeRegister(word);
    result->effects = Machine::Effects::none();
    result->setRunsWithoutFrame(true);
    result->setGenerator([lane = laneOfIntegers(lanes)](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
#if CPU(X86_64)
        FPRReg destination = params[0].fpr();
        jit.move32ToFloat(params[1].gpr(), destination);
        if (lane == SIMDLane::i8x16)
            jit.m_assembler.punpcklbw_rr(destination, destination);
        if (lane != SIMDLane::i32x4)
            jit.m_assembler.punpcklwd_rr(destination, destination);
        jit.m_assembler.pshufd_i8rr(0, destination, destination);
#else
        jit.vectorSplat(lane, params[1].gpr(), params[0].fpr());
#endif
    });
    return result;
}

LValue VectorOutput::joinWords(LValue firstPair, LValue secondPair)
{
    Machine::PatchOperation* result = m_out.patchpoint(Machine::Vector128);
    result->appendSomeRegister(firstPair);
    result->appendSomeRegister(secondPair);
    result->effects = Machine::Effects::none();
    result->setRunsWithoutFrame(true);
    result->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        FPRReg destination = params[0].fpr();
        jit.move64ToDouble(params[1].gpr(), destination);
        jit.move64ToDouble(params[2].gpr(), MacroAssembler::fpTempRegister);
#if CPU(X86_64)
        jit.m_assembler.vectorOp_rr(true, false, lastByteOfUNPCKLPD, MacroAssembler::fpTempRegister, destination);
#else
        jit.vectorReplaceLane(SIMDLane::f64x2, CCallHelpers::TrustedImm32(1), MacroAssembler::fpTempRegister, destination);
#endif
    });
    return result;
}

static void emitIntegers(CCallHelpers& jit, Opcode opcode, SIMDLane lane, FPRReg left, FPRReg right, FPRReg destination)
{
    SIMDInfo integers { lane, SIMDSignMode::None };
    SIMDInfo bits { SIMDLane::v128, SIMDSignMode::None };
    switch (opcode) {
    case Opcode::Add:
        jit.vectorAdd(integers, left, right, destination);
        return;
    case Opcode::Sub:
        jit.vectorSub(integers, left, right, destination);
        return;
    case Opcode::Mul:
#if CPU(X86_64)
        RELEASE_ASSERT(lane == SIMDLane::i32x4);
        if (destination == right)
            std::swap(left, right);
        if (destination != left)
            jit.moveVector(left, destination);
        jit.m_assembler.vectorOp_rr(true, true, lastByteOfPMULLD, right, destination);
#else
        jit.vectorMul(integers, left, right, destination);
#endif
        return;
    case Opcode::BitAnd:
        jit.vectorAnd(bits, left, right, destination);
        return;
    case Opcode::BitOr:
        jit.vectorOr(bits, left, right, destination);
        return;
    case Opcode::BitXor:
        jit.vectorXor(bits, left, right, destination);
        return;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LValue VectorOutput::computeIntegers(Opcode opcode, unsigned lanes, LValue left, LValue right)
{
    Machine::PatchOperation* result = m_out.patchpoint(Machine::Vector128);
    result->appendSomeRegister(left);
    result->appendSomeRegister(right);
    result->effects = Machine::Effects::none();
    result->setRunsWithoutFrame(true);
    result->setGenerator([opcode, lane = laneOfIntegers(lanes)](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        emitIntegers(jit, opcode, lane, params[1].fpr(), params[2].fpr(), params[0].fpr());
    });
    return result;
}

LValue VectorOutput::shiftWords([[maybe_unused]] bool isLeft, LValue words, [[maybe_unused]] unsigned amount)
{
    return unary(Machine::Vector128, words, [=]([[maybe_unused]] CCallHelpers& jit, [[maybe_unused]] const Machine::PatchParameters& params) {
#if CPU(X86_64)
        FPRReg destination = params[0].fpr();
        if (params[1].fpr() != destination)
            jit.moveVector(params[1].fpr(), destination);
        if (isLeft)
            jit.m_assembler.pslld_i8r(amount, destination);
        else
            jit.m_assembler.psrld_i8r(amount, destination);
#else
        RELEASE_ASSERT_NOT_REACHED();
#endif
    });
}

LValue VectorOutput::reduceWords(Opcode opcode, LValue words)
{
    Machine::PatchOperation* result = m_out.patchpoint(Machine::Int32);
    result->appendSomeRegister(words);
    result->effects = Machine::Effects::none();
    result->setRunsWithoutFrame(true);
    result->numFPScratchRegisters = 1;
    result->setGenerator([opcode](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        FPRReg sum = params.fpScratch(0);
        FPRReg other = MacroAssembler::fpTempRegister;
        FPRReg words = params[1].fpr();
#if CPU(X86_64)
        static constexpr uint8_t swapNeighbors = 0xb1;
        jit.m_assembler.pshufd_i8rr(swapHalves, words, sum);
        emitIntegers(jit, opcode, SIMDLane::i32x4, sum, words, sum);
        jit.m_assembler.pshufd_i8rr(swapNeighbors, sum, other);
        emitIntegers(jit, opcode, SIMDLane::i32x4, sum, other, sum);
        jit.moveFloatTo32(sum, params[0].gpr());
#else
        jit.vectorExtractLane(SIMDLane::f64x2, CCallHelpers::TrustedImm32(1), words, sum);
        emitIntegers(jit, opcode, SIMDLane::i32x4, sum, words, sum);
        jit.vectorExtractLane(SIMDLane::f32x4, CCallHelpers::TrustedImm32(1), sum, other);
        emitIntegers(jit, opcode, SIMDLane::i32x4, sum, other, sum);
        jit.moveFloatTo32(sum, params[0].gpr());
#endif
    });
    return result;
}

LValue VectorOutput::sumsOfPairs(LValue words)
{
    return unary(Machine::Vector128, words, [](CCallHelpers& jit, const Machine::PatchParameters& params) {
        FPRReg destination = params[0].fpr();
        FPRReg words = params[1].fpr();
#if CPU(X86_64)
        FPRReg other = MacroAssembler::fpTempRegister;
        jit.m_assembler.pshufd_i8rr(swapHalves, words, other);
        jit.m_assembler.vectorOp_rr(true, true, lastByteOfPMOVSXDQ, other, other);
        jit.m_assembler.vectorOp_rr(true, true, lastByteOfPMOVSXDQ, words, destination);
        emitIntegers(jit, Opcode::Add, SIMDLane::i64x2, destination, other, destination);
#else
        jit.m_assembler.vectorSaddlp(destination, words, SIMDLane::i32x4);
#endif
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
