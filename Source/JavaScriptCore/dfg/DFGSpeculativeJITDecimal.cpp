/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "DFGSpeculativeJIT.h"

#if ENABLE(DFG_JIT)

#include "DFGAbstractInterpreterInlines.h"
#include "DFGSlowPathGenerator.h"
#include "DecimalObject.h"
#include "DecimalOperations.h"
#include "JSCInlines.h"

namespace JSC { namespace DFG {

void SpeculativeJIT::decimalResult(GPRReg low, GPRReg upper, Node* node)
{
    useChildren(node);
    std::array words { low, upper };
    for (unsigned i = 0; i < words.size(); ++i) {
        auto& data = m_graph.m_tupleData.at(node->tupleOffset() + i);
        if (!data.refCount)
            continue;
        ASSERT(data.refCount == 1);
        m_gprs.retain(words[i], data.virtualRegister, SpillOrderStorage);
        generationInfoFromVirtualRegister(data.virtualRegister).initStorage(node, data.refCount, words[i]);
    }
}

template<typename OperationType, typename... Arguments>
void SpeculativeJIT::callDecimalOperation(Node* node, OperationType operation, Arguments... arguments)
{
    flushRegisters();
    GPRFlushedCallResult low(this);
    GPRFlushedCallResult2 upper(this);
    setupArguments<OperationType>(arguments...);
    appendCallSetResult(operation, low.gpr(), upper.gpr());
    if (node->op() != DecimalArith && node->argumentOfDecimalNode().useKind() == UntypedUse) {
        if (node->op() == ToDecimal)
            exceptionCheck();
        speculationCheck(BadDecimalArgument, JSValueSource(), nullptr, branch64(Equal, upper.gpr(), TrustedImm32(upperOfNoDecimal)));
    }
    decimalResult(low.gpr(), upper.gpr(), node);
}

void SpeculativeJIT::compileDecimalWordConstant(Node* node)
{
    GPRTemporary result(this);
    move(Imm64(node->decimalWord()), result.gpr());
    storageResult(result.gpr(), node);
}

void SpeculativeJIT::compileGetDecimalWord(Node* node)
{
    SpeculateCellOperand base(this, node->child1());
    GPRTemporary result(this, Reuse, base);
    load64(Address(base.gpr(), node->decimalWordIndex() ? DecimalObject::offsetOfUpper() : DecimalObject::offsetOfLow()), result.gpr());
    storageResult(result.gpr(), node);
}

void SpeculativeJIT::compileToDecimal(Node* node)
{
    switch (node->child1().useKind()) {
    case Int32Use: {
        SpeculateInt32Operand value(this, node->child1());
        callDecimalOperation(node, operationInt32ToDecimal, value.gpr());
        return;
    }
    case DoubleRepUse: {
        SpeculateDoubleOperand value(this, node->child1());
        callDecimalOperation(node, operationDoubleToDecimal, value.fpr());
        return;
    }
    case UntypedUse: {
        JSValueOperand value(this, node->child1());
        callDecimalOperation(node, operationToDecimal, LinkableConstant::globalObject(*this, node), value.gpr());
        return;
    }
    default:
        DFG_CRASH(m_graph, node, "Bad use kind"_s);
    }
}

void SpeculativeJIT::compileDecimalArith(Node* node)
{
    StorageOperand low(this, m_graph.varArgChild(node, 0));
    StorageOperand upper(this, m_graph.varArgChild(node, 1));
    GPRReg lowGPR = low.gpr();
    GPRReg upperGPR = upper.gpr();

    switch (node->decimalOperation()) {
    case DecimalOperation::Abs:
    case DecimalOperation::Negate: {
        GPRTemporary resultLow(this);
        GPRTemporary resultUpper(this);
        if (node->decimalOperation() == DecimalOperation::Abs) {
            lshift64(upperGPR, TrustedImm32(1), resultUpper.gpr());
            urshift64(TrustedImm32(1), resultUpper.gpr());
        } else {
            // NaN has no sign.
            move(TrustedImm64(Decimal128::upperOfNaN), resultLow.gpr());
            compare64(NotEqual, upperGPR, resultLow.gpr(), resultUpper.gpr());
            lshift64(TrustedImm32(63), resultUpper.gpr());
            xor64(upperGPR, resultUpper.gpr());
        }
        move(lowGPR, resultLow.gpr());
        decimalResult(resultLow.gpr(), resultUpper.gpr(), node);
        return;
    }
    case DecimalOperation::Mantissa:
        callDecimalOperation(node, operationDecimalMantissa, lowGPR, upperGPR);
        return;
    default: {
        StorageOperand otherLow(this, m_graph.varArgChild(node, 2));
        StorageOperand otherUpper(this, m_graph.varArgChild(node, 3));
        GPRReg otherLowGPR = otherLow.gpr();
        GPRReg otherUpperGPR = otherUpper.gpr();
        callDecimalOperation(node, binaryDecimalOperation(node->decimalOperation()), lowGPR, upperGPR, otherLowGPR, otherUpperGPR);
        return;
    }
    }
}

void SpeculativeJIT::compileDecimalScale(Node* node)
{
    StorageOperand low(this, node->child1());
    StorageOperand upper(this, node->child2());
    GPRReg lowGPR = low.gpr();
    GPRReg upperGPR = upper.gpr();
    bool isRound = node->decimalOperation() == DecimalOperation::Round;
    TrustedImm32 mode(node->decimalRoundingMode());

    if (node->child3().useKind() == Int32Use) {
        SpeculateInt32Operand count(this, node->child3());
        callDecimalOperation(node, isRound ? operationDecimalRound : operationDecimalScale10, lowGPR, upperGPR, count.gpr(), mode);
        return;
    }
    JSValueOperand count(this, node->child3());
    callDecimalOperation(node, isRound ? operationDecimalRoundByValue : operationDecimalScale10ByValue, lowGPR, upperGPR, count.gpr(), mode);
}

// Flushes the registers. The result is that of a call.
void SpeculativeJIT::emitDecimalCompare(Node* node)
{
    StorageOperand low(this, m_graph.varArgChild(node, 0));
    StorageOperand upper(this, m_graph.varArgChild(node, 1));
    StorageOperand otherLow(this, m_graph.varArgChild(node, 2));
    StorageOperand otherUpper(this, m_graph.varArgChild(node, 3));
    GPRReg lowGPR = low.gpr();
    GPRReg upperGPR = upper.gpr();
    GPRReg otherLowGPR = otherLow.gpr();
    GPRReg otherUpperGPR = otherUpper.gpr();
    flushRegisters();
    setupArguments<decltype(operationDecimalCompare)>(lowGPR, upperGPR, otherLowGPR, otherUpperGPR);
    appendCall(operationDecimalCompare);
}

void SpeculativeJIT::compileDecimalTest(Node* node)
{
    auto operation = node->decimalOperation();
    if (operation == DecimalOperation::IsNaN || operation == DecimalOperation::IsFinite) {
        StorageOperand upper(this, m_graph.varArgChild(node, 1));
        GPRTemporary result(this);
        GPRReg resultGPR = result.gpr();
        if (operation == DecimalOperation::IsNaN) {
            urshift64(upper.gpr(), TrustedImm32(Decimal128::shiftOfExponent), resultGPR);
            compare32(Equal, resultGPR, TrustedImm32(Decimal128::exponentOfNaN), resultGPR);
        } else {
            // The exponents of NaN and infinity differ in the last bit only.
            static_assert((Decimal128::exponentOfNaN ^ Decimal128::exponentOfInfinity) == 1);
            lshift64(upper.gpr(), TrustedImm32(1), resultGPR);
            urshift64(TrustedImm32(Decimal128::shiftOfExponent + 2), resultGPR);
            compare32(NotEqual, resultGPR, TrustedImm32(Decimal128::exponentOfNaN >> 1), resultGPR);
        }
        unblessedBooleanResult(resultGPR, node);
        return;
    }

    emitDecimalCompare(node);
    GPRFlushedCallResult result(this);
    GPRReg resultGPR = result.gpr();
    static_assert(unorderedDecimals == 2);
    switch (operation) {
    case DecimalOperation::Equals:
        compare32(Equal, resultGPR, TrustedImm32(0), resultGPR);
        break;
    case DecimalOperation::NotEquals:
        and32(TrustedImm32(1), resultGPR);
        break;
    case DecimalOperation::LessThan:
        compare32(LessThan, resultGPR, TrustedImm32(0), resultGPR);
        break;
    case DecimalOperation::LessThanOrEqual:
        compare32(LessThanOrEqual, resultGPR, TrustedImm32(0), resultGPR);
        break;
    case DecimalOperation::GreaterThan:
        compare32(Equal, resultGPR, TrustedImm32(1), resultGPR);
        break;
    case DecimalOperation::GreaterThanOrEqual:
        compare32(BelowOrEqual, resultGPR, TrustedImm32(1), resultGPR);
        break;
    default:
        DFG_CRASH(m_graph, node, "Bad operation"_s);
    }
    unblessedBooleanResult(resultGPR, node);
}

void SpeculativeJIT::compileDecimalToNumber(Node* node)
{
    if (node->decimalOperation() == DecimalOperation::Exponent) {
        StorageOperand low(this, m_graph.varArgChild(node, 0));
        StorageOperand upper(this, m_graph.varArgChild(node, 1));
        GPRReg lowGPR = low.gpr();
        GPRReg upperGPR = upper.gpr();
        GPRTemporary result(this);
        GPRTemporary scratch(this);
        GPRReg resultGPR = result.gpr();
        GPRReg scratchGPR = scratch.gpr();

        // A coefficient of more than 64 bits, and the values that have none.
        JumpList slowCases;
        lshift64(upperGPR, TrustedImm32(64 - Decimal128::shiftOfExponent), scratchGPR);
        slowCases.append(branchTest64(NonZero, scratchGPR));
        slowCases.append(branchTest64(Zero, lowGPR));

        // As numberOfDigits().
        countLeadingZeros64(lowGPR, scratchGPR);
        move(TrustedImm32(64), resultGPR);
        sub32(scratchGPR, resultGPR);
        mul32(TrustedImm32(1233), resultGPR, resultGPR);
        urshift32(TrustedImm32(12), resultGPR);
        move(TrustedImmPtr(Decimal128::powersOf10()), scratchGPR);
        load64(BaseIndex(scratchGPR, resultGPR, TimesEight), scratchGPR);
        compare64(Below, lowGPR, scratchGPR, scratchGPR);
        sub32(scratchGPR, resultGPR);

        lshift64(upperGPR, TrustedImm32(1), scratchGPR);
        rshift64(TrustedImm32(64 - Decimal128::bitsOfExponent), scratchGPR);
        add32(scratchGPR, resultGPR);
        boxInt32(resultGPR, resultGPR);
        addSlowPathGenerator(slowPathCall(slowCases, this, operationDecimalExponent, NeedToSpill, ExceptionCheckRequirement::CheckNotNeeded, resultGPR, lowGPR, upperGPR));
        jsValueResult(resultGPR, node);
        return;
    }

    ASSERT(node->decimalOperation() == DecimalOperation::Compare);
    emitDecimalCompare(node);
    GPRFlushedCallResult result(this);
    GPRReg resultGPR = result.gpr();
    Jump isUnordered = branch32(Equal, resultGPR, TrustedImm32(unorderedDecimals));
    boxInt32(resultGPR, resultGPR);
    Jump done = jump();
    isUnordered.link(this);
    moveTrustedValue(jsNaN(), resultGPR);
    done.link(this);
    jsValueResult(resultGPR, node);
}

void SpeculativeJIT::compileDecimalToString(Node* node)
{
    StorageOperand low(this, node->child1());
    StorageOperand upper(this, node->child2());
    GPRReg lowGPR = low.gpr();
    GPRReg upperGPR = upper.gpr();
    flushRegisters();
    GPRFlushedCallResult result(this);
    callOperation(node->decimalOperation() == DecimalOperation::ToString ? operationDecimalToString : operationDecimalToExponential, result.gpr(), TrustedImmPtr(&vm()), lowGPR, upperGPR);
    cellResult(result.gpr(), node);
}

void SpeculativeJIT::compileDecimalToFixed(Node* node)
{
    StorageOperand low(this, m_graph.varArgChild(node, 0));
    StorageOperand upper(this, m_graph.varArgChild(node, 1));
    JSValueOperand digits(this, m_graph.varArgChild(node, 2));
    JSValueOperand roundingMode(this, m_graph.varArgChild(node, 3));
    GPRReg lowGPR = low.gpr();
    GPRReg upperGPR = upper.gpr();
    GPRReg digitsGPR = digits.gpr();
    GPRReg roundingModeGPR = roundingMode.gpr();
    flushRegisters();
    GPRFlushedCallResult result(this);
    callOperation(operationDecimalToFixed, result.gpr(), LinkableConstant::globalObject(*this, node), lowGPR, upperGPR, digitsGPR, roundingModeGPR);
    speculationCheck(BadDecimalArgument, JSValueSource(), nullptr, branchTestPtr(Zero, result.gpr()));
    cellResult(result.gpr(), node);
}

void SpeculativeJIT::compileNewDecimal(Node* node)
{
    StorageOperand low(this, node->child1());
    StorageOperand upper(this, node->child2());
    GPRTemporary result(this);
    GPRTemporary scratch1(this);
    GPRTemporary scratch2(this);
    GPRReg lowGPR = low.gpr();
    GPRReg upperGPR = upper.gpr();
    GPRReg resultGPR = result.gpr();

    JumpList slowCases;
    FrozenValue* structure = m_graph.freezeStrong(node->structure().get());
    emitAllocateJSObjectWithKnownSize<DecimalObject>(resultGPR, TrustedImmPtr(structure), TrustedImmPtr(nullptr), scratch1.gpr(), scratch2.gpr(), slowCases, sizeof(DecimalObject), SlowAllocationResult::UndefinedBehavior);
    store64(lowGPR, Address(resultGPR, DecimalObject::offsetOfLow()));
    store64(upperGPR, Address(resultGPR, DecimalObject::offsetOfUpper()));
    mutatorFence(vm());

    addSlowPathGenerator(slowPathCall(slowCases, this, operationNewDecimal, resultGPR, TrustedImmPtr(&vm()), TrustedImmPtr(structure), lowGPR, upperGPR));
    cellResult(resultGPR, node);
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
