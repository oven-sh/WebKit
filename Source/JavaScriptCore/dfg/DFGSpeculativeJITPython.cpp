/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "DFGSpeculativeJIT.h"

#if ENABLE(DFG_JIT)

#include "BaselineJITRegisters.h"
#include "DFGAbstractInterpreterInlines.h"
#include "DFGOperations.h"
#include "DFGSlowPathGenerator.h"
#include "InlineCacheCompiler.h"
#include "PyObjects.h"
#include "PyTuple.h"

namespace JSC { namespace DFG {

// Python's nodes. See "The DFG" in python/README.md.

void SpeculativeJIT::compilePyBinaryOp(Node* node)
{
    JSValueOperand left(this, node->child1());
    JSValueOperand right(this, node->child2());
    GPRReg leftGPR = left.gpr();
    GPRReg rightGPR = right.gpr();

    flushRegisters();
    GPRFlushedCallResult result(this);
    callOperation(node->op() == PyBinaryOp ? operationPyBinaryOp : operationPyCompareOp, result.gpr(), LinkableConstant::globalObject(*this, node), leftGPR, rightGPR, TrustedImm32(node->pythonOperator()));
    jsValueResult(result.gpr(), node);
}

void SpeculativeJIT::compilePyUnaryOp(Node* node)
{
    JSValueOperand operand(this, node->child1());
    GPRReg operandGPR = operand.gpr();

    flushRegisters();
    GPRFlushedCallResult result(this);
    callOperation(operationPyUnaryOp, result.gpr(), LinkableConstant::globalObject(*this, node), operandGPR, TrustedImm32(node->pythonOperator()));
    jsValueResult(result.gpr(), node);
}

void SpeculativeJIT::compilePyToBool(Node* node)
{
    JSValueOperand operand(this, node->child1());
    GPRReg operandGPR = operand.gpr();

    flushRegisters();
    GPRFlushedCallResult result(this);
    callOperation(operationPyToBool, result.gpr(), LinkableConstant::globalObject(*this, node), operandGPR);
    unblessedBooleanResult(result.gpr(), node);
}

void SpeculativeJIT::compilePySetAttr(Node* node)
{
    SpeculateCellOperand base(this, node->child1());
    JSValueOperand value(this, node->child2());
    GPRReg baseGPR = base.gpr();
    GPRReg valueGPR = value.gpr();

    flushRegisters();
    cachedPutById(node, node->origin.semantic, baseGPR, valueGPR, node->cacheableIdentifier(), AccessType::PySetAttr);
    noResult(node);
}

void SpeculativeJIT::compilePyDelAttr(Node* node)
{
    JSValueOperand base(this, node->child1());
    GPRReg baseGPR = base.gpr();

    flushRegisters();
    callOperation(operationPyDelAttr, LinkableConstant::globalObject(*this, node), baseGPR, TrustedImmPtr(node->cacheableIdentifier().rawBits()));
    noResult(node);
}

void SpeculativeJIT::compilePyLoadMethod(Node* node)
{
    using BaselineJITRegisters::PyLoadMethod::baseGPR;
    using BaselineJITRegisters::PyLoadMethod::resultGPR;
    using BaselineJITRegisters::PyLoadMethod::selfGPR;
    using BaselineJITRegisters::PyLoadMethod::propertyCacheGPR;

    JSValueOperand base(this, node->child1());
    GPRReg givenGPR = base.gpr();

    flushRegisters();
    GPRTemporary function(this, resultGPR);
    GPRTemporary self(this, selfGPR);

    CodeOrigin codeOrigin = node->origin.semantic;
    CallSiteIndex callSite = recordCallSiteAndGenerateExceptionHandlingOSRExitIfNeeded(codeOrigin, m_stream.size());
    auto [ propertyCache, propertyCacheConstant ] = addPropertyInlineCache();
    move(givenGPR, baseGPR);
    JITGetByIdGenerator gen(
        codeBlock(), propertyCache, JITType::DFGJIT, codeOrigin, callSite, usedRegisters(), node->cacheableIdentifier(),
        baseGPR, resultGPR, propertyCacheGPR, AccessType::PyLoadMethod, CacheType::GetByIdPrototype);

    loadPropertyInlineCache(propertyCacheConstant, propertyCacheGPR);
    Jump isNotCell = branchIfNotCell(baseGPR);
    move(baseGPR, selfGPR);
    gen.generateDataICFastPath(*this);
    Label done = label();

    addSlowPathGeneratorLambda([=, this]() mutable {
        gen.generateDataICSlowPath(*this);
        // What throws from here is found by where the frame says that it is, as with what a handler calls.
        isNotCell.link(this);
        nearCallThunk(CodeLocationLabel { InlineCacheCompiler::generateSlowPathCode(vm(), AccessType::PyLoadMethod).retaggedCode<NoPtrTag>() });
        jump().linkTo(done, this);
    });

    useChildren(node);
    jsValueTupleResultWithoutUsingChildren(resultGPR, node, 0);
    jsValueTupleResultWithoutUsingChildren(selfGPR, node, 1);
}

void SpeculativeJIT::compilePyLoadGlobal(Node* node)
{
    SpeculateCellOperand globals(this, node->child1());
    SpeculateCellOperand builtins(this, node->child2());
    GPRReg globalsGPR = globals.gpr();
    GPRReg builtinsGPR = builtins.gpr();

    flushRegisters();
    GPRFlushedCallResult result(this);
    callOperation(operationPyLoadGlobal, result.gpr(), LinkableConstant::globalObject(*this, node), globalsGPR, builtinsGPR, TrustedImmPtr(node->cacheableIdentifier().rawBits()));
    jsValueResult(result.gpr(), node);
}

// What is at a place in a list. The place is not less than 0.
void SpeculativeJIT::loadPythonListItem(GPRReg listGPR, GPRReg indexGPR, GPRReg resultGPR, GPRReg scratchGPR, JumpList& slow)
{
    load8(Address(listGPR, JSCell::indexingTypeAndMiscOffset()), scratchGPR);
    and32(TrustedImm32(IndexingShapeMask), scratchGPR);
    Jump isContiguous = branch32(Equal, scratchGPR, TrustedImm32(ContiguousShape));
    slow.append(branch32(NotEqual, scratchGPR, TrustedImm32(Int32Shape)));
    isContiguous.link(this);
    loadPtr(Address(listGPR, JSObject::butterflyOffset()), scratchGPR);
    slow.append(branch32(AboveOrEqual, indexGPR, Address(scratchGPR, Butterfly::offsetOfPublicLength())));
    load64(BaseIndex(scratchGPR, indexGPR, TimesEight), resultGPR);
    slow.append(branchIfEmpty(resultGPR));
}

// What fixup could not tell the kind of. It finds out, as the baseline JIT does.
void SpeculativeJIT::compilePyGetItem(Node* node)
{
    JSValueOperand base(this, m_graph.varArgChild(node, 0));
    JSValueOperand key(this, m_graph.varArgChild(node, 1));
    GPRTemporary result(this);
    GPRTemporary index(this);
    GPRTemporary scratch(this);
    GPRReg baseGPR = base.gpr();
    GPRReg keyGPR = key.gpr();
    GPRReg resultGPR = result.gpr();
    GPRReg indexGPR = index.gpr();
    GPRReg scratchGPR = scratch.gpr();

    JumpList slow;
    slow.append(branchIfNotCell(baseGPR));
    slow.append(branchIfNotInt32(keyGPR));
    slow.append(branchIfInt32IsWholeFloat(keyGPR));
    zeroExtend32ToWord(keyGPR, indexGPR);
    Jump isNotList = branchIfNotType(baseGPR, ArrayType);
    {
        // From the end, if it is negative.
        Jump isFromTheStart = branch32(GreaterThanOrEqual, indexGPR, TrustedImm32(0));
        loadPtr(Address(baseGPR, JSObject::butterflyOffset()), scratchGPR);
        // An array with nothing in it may have nowhere to keep it, and then nothing is within it.
        slow.append(branchTestPtr(Zero, scratchGPR));
        add32(Address(scratchGPR, Butterfly::offsetOfPublicLength()), indexGPR);
        isFromTheStart.link(this);
        loadPythonListItem(baseGPR, indexGPR, resultGPR, scratchGPR, slow);
    }
    Jump done = jump();

    isNotList.link(this);
    slow.append(branchIfNotType(baseGPR, PyTupleType));
    {
        Jump isFromTheStart = branch32(GreaterThanOrEqual, indexGPR, TrustedImm32(0));
        add32(Address(baseGPR, PyTuple::offsetOfLength()), indexGPR);
        isFromTheStart.link(this);
        slow.append(branch32(AboveOrEqual, indexGPR, Address(baseGPR, PyTuple::offsetOfLength())));
        load64(BaseIndex(baseGPR, indexGPR, TimesEight, PyTuple::offsetOfValues()), resultGPR);
    }
    done.link(this);

    addSlowPathGenerator(slowPathCall(slow, this, operationPyGetItem, resultGPR, LinkableConstant::globalObject(*this, node), baseGPR, keyGPR));
    jsValueResult(resultGPR, node);
}

void SpeculativeJIT::compilePySetItem(Node* node)
{
    SpeculateCellOperand base(this, m_graph.varArgChild(node, 0));
    JSValueOperand key(this, m_graph.varArgChild(node, 1));
    JSValueOperand value(this, m_graph.varArgChild(node, 2));
    GPRTemporary index(this);
    GPRTemporary scratch(this);
    GPRReg baseGPR = base.gpr();
    GPRReg keyGPR = key.gpr();
    GPRReg valueGPR = value.gpr();
    GPRReg indexGPR = index.gpr();
    GPRReg scratchGPR = scratch.gpr();

    JumpList slow;
    slow.append(branchIfNotInt32(keyGPR));
    slow.append(branchIfInt32IsWholeFloat(keyGPR));
    slow.append(branchIfNotType(baseGPR, ArrayType));
    // Not one that shares what is in it with the literal that it was made from. One that has had nothing but ints in it goes on that way only if this is one.
    load8(Address(baseGPR, JSCell::indexingTypeAndMiscOffset()), scratchGPR);
    and32(TrustedImm32(IndexingShapeAndWritabilityMask), scratchGPR);
    Jump isContiguous = branch32(Equal, scratchGPR, TrustedImm32(ContiguousShape));
    slow.append(branch32(NotEqual, scratchGPR, TrustedImm32(Int32Shape)));
    slow.append(branchIfNotInt32(valueGPR));
    slow.append(branchIfInt32IsWholeFloat(valueGPR));
    isContiguous.link(this);

    loadPtr(Address(baseGPR, JSObject::butterflyOffset()), scratchGPR);
    zeroExtend32ToWord(keyGPR, indexGPR);
    Jump isFromTheStart = branch32(GreaterThanOrEqual, indexGPR, TrustedImm32(0));
    add32(Address(scratchGPR, Butterfly::offsetOfPublicLength()), indexGPR);
    isFromTheStart.link(this);
    slow.append(branch32(AboveOrEqual, indexGPR, Address(scratchGPR, Butterfly::offsetOfPublicLength())));
    store64(valueGPR, BaseIndex(scratchGPR, indexGPR, TimesEight));

    addSlowPathGenerator(slowPathCall(slow, this, operationPySetItem, NoResult, LinkableConstant::globalObject(*this, node), baseGPR, keyGPR, valueGPR));
    noResult(node);
}

void SpeculativeJIT::compilePyDelItem(Node* node)
{
    JSValueOperand base(this, node->child1());
    JSValueOperand key(this, node->child2());
    GPRReg baseGPR = base.gpr();
    GPRReg keyGPR = key.gpr();

    flushRegisters();
    callOperation(operationPyDelItem, LinkableConstant::globalObject(*this, node), baseGPR, keyGPR);
    noResult(node);
}

void SpeculativeJIT::compilePyGetIter(Node* node)
{
    JSValueOperand iterable(this, node->child1());
    GPRReg iterableGPR = iterable.gpr();

    flushRegisters();
    GPRFlushedCallResult result(this);
    callOperation(operationPyGetIter, result.gpr(), LinkableConstant::globalObject(*this, node), iterableGPR);
    jsValueResult(result.gpr(), node);
}

void SpeculativeJIT::compilePyIterNext(Node* node)
{
    using Kind = PyIterator::Kind;
    JSValueOperand iterator(this, node->child1());
    GPRTemporary result(this);
    GPRTemporary index(this);
    GPRTemporary scratch(this);
    GPRTemporary sequence(this);
    GPRReg iteratorGPR = iterator.gpr();
    GPRReg resultGPR = result.gpr();
    GPRReg indexGPR = index.gpr();
    GPRReg scratchGPR = scratch.gpr();
    GPRReg sequenceGPR = sequence.gpr();

    JumpList done;
    JumpList slow;
    slow.append(branchIfNotCell(iteratorGPR));
    slow.append(branchIfNotType(iteratorGPR, PyIteratorType));
    load8(Address(iteratorGPR, PyIterator::offsetOfKind()), scratchGPR);
    load64(Address(iteratorGPR, PyIterator::offsetOfIndex()), indexGPR);

    Jump isNotRange = branch32(NotEqual, scratchGPR, TrustedImm32(static_cast<int32_t>(Kind::Range)));
    {
        // `stop` is how many are left.
        load64(Address(iteratorGPR, PyIterator::offsetOfStop()), scratchGPR);
        Jump hasMore = branch64(GreaterThan, scratchGPR, TrustedImm32(0));
        move(TrustedImm64(JSValue::encode(JSValue())), resultGPR);
        done.append(jump());
        hasMore.link(this);
        signExtend32ToPtr(indexGPR, resultGPR);
        slow.append(branch64(NotEqual, resultGPR, indexGPR));
        sub64(TrustedImm32(1), scratchGPR);
        store64(scratchGPR, Address(iteratorGPR, PyIterator::offsetOfStop()));
        add64(Address(iteratorGPR, PyIterator::offsetOfStep()), indexGPR);
        store64(indexGPR, Address(iteratorGPR, PyIterator::offsetOfIndex()));
        zeroExtend32ToWord(resultGPR, resultGPR);
        boxInt32(resultGPR, resultGPR);
        done.append(jump());
    }

    // When one of these runs out it lets go of what it was going through, which is for the slow path. It has done that already if there is nothing here.
    isNotRange.link(this);
    load64(Address(iteratorGPR, PyIterator::offsetOfA()), sequenceGPR);
    slow.append(branchIfEmpty(sequenceGPR));
    slow.append(branch64(Above, indexGPR, TrustedImm32(std::numeric_limits<int32_t>::max())));
    Jump isNotList = branch32(NotEqual, scratchGPR, TrustedImm32(static_cast<int32_t>(Kind::List)));
    loadPythonListItem(sequenceGPR, indexGPR, resultGPR, scratchGPR, slow);
    Jump gotItem = jump();

    isNotList.link(this);
    slow.append(branch32(NotEqual, scratchGPR, TrustedImm32(static_cast<int32_t>(Kind::Tuple))));
    slow.append(branch32(AboveOrEqual, indexGPR, Address(sequenceGPR, PyTuple::offsetOfLength())));
    load64(BaseIndex(sequenceGPR, indexGPR, TimesEight, PyTuple::offsetOfValues()), resultGPR);

    gotItem.link(this);
    add64(TrustedImm32(1), indexGPR);
    store64(indexGPR, Address(iteratorGPR, PyIterator::offsetOfIndex()));
    done.link(this);

    addSlowPathGenerator(slowPathCall(slow, this, operationPyIterNext, resultGPR, LinkableConstant::globalObject(*this, node), iteratorGPR));
    jsValueResult(resultGPR, node);
}

void SpeculativeJIT::compilePyUnpackSequence(Node* node)
{
    JSValueOperand iterable(this, node->child1());
    GPRReg iterableGPR = iterable.gpr();

    flushRegisters();
    GPRFlushedCallResult result(this);
    callOperation(operationPyUnpackSequence, result.gpr(), LinkableConstant::globalObject(*this, node), iterableGPR, TrustedImm32(node->unpackedCount()), TrustedImm32(node->unpackedStarIndex()));
    cellResult(result.gpr(), node);
}

void SpeculativeJIT::compilePyGetTupleItem(Node* node)
{
    SpeculateCellOperand tuple(this, node->child1());
    GPRTemporary result(this);

    load64(Address(tuple.gpr(), PyTuple::offsetOfValues() + node->tupleItemIndex() * sizeof(EncodedJSValue)), result.gpr());
    jsValueResult(result.gpr(), node);
}

void SpeculativeJIT::compilePyNewTuple(Node* node)
{
    size_t scratchSize = sizeof(EncodedJSValue) * node->numChildren();
    ScratchBuffer* scratchBuffer = vm().scratchBufferForSize(scratchSize);
    EncodedJSValue* buffer = scratchBuffer ? static_cast<EncodedJSValue*>(scratchBuffer->dataBuffer()) : nullptr;
    for (unsigned i = 0; i < node->numChildren(); ++i) {
        JSValueOperand operand(this, m_graph.m_varArgChildren[node->firstChild() + i]);
        storeValue(operand.gpr(), buffer + i);
        operand.use();
    }

    flushRegisters();
    GPRFlushedCallResult result(this);
    callOperation(operationPyNewTuple, result.gpr(), LinkableConstant::globalObject(*this, node), TrustedImmPtr(buffer), size_t(node->numChildren()));
    cellResult(result.gpr(), node, UseChildrenCalledExplicitly);
}

void SpeculativeJIT::compilePyEnter(Node* node)
{
    flushRegisters();
    GPRFlushedCallResult isTooDeep(this);
    GPRTemporary scratch(this);
    GPRReg isTooDeepGPR = isTooDeep.gpr();
    GPRReg scratchGPR = scratch.gpr();

    load32(vm().addressOfPythonDepth(), scratchGPR);
    add32(TrustedImm32(1), scratchGPR);
    store32(scratchGPR, vm().addressOfPythonDepth());
    // It is 0 if there is something that has been put off until now.
    Jump isOrdinary = branch32(BelowOrEqual, scratchGPR, AbsoluteAddress(vm().addressOfPythonLimitUnlessWatched()));
    callOperation(operationPyEnterSlow, isTooDeepGPR, LinkableConstant::globalObject(*this, node));
    Jump goesOn = branchTest32(Zero, isTooDeepGPR);
    // A frame that is one too deep does not begin. How that is seen to is for what has the frame as it is looked for. It counts the frame for itself.
    sub32(TrustedImm32(1), AbsoluteAddress(vm().addressOfPythonDepth()));
    speculationCheck(ExoticObjectMode, JSValueSource(), nullptr, jump());
    goesOn.link(this);
    isOrdinary.link(this);
    noResult(node);
}

void SpeculativeJIT::compilePyLeave(Node* node)
{
    sub32(TrustedImm32(1), AbsoluteAddress(vm().addressOfPythonDepth()));
    noResult(node);
}

void SpeculativeJIT::compilePyCheckNoFrameObject(Node* node)
{
    speculationCheck(PythonFrameObjectExists, JSValueSource(), nullptr, branch64(NotEqual, addressFor(node->stackAccessData()->machineLocal), TrustedImm64(JSValue::encode(jsUndefined()))));
    noResult(node);
}

void SpeculativeJIT::compilePyCheckPendingWork(Node* node)
{
    Jump hasWork = branchTest32(Zero, AbsoluteAddress(vm().addressOfPythonLimitUnlessWatched()));
    addSlowPathGenerator(slowPathCall(hasWork, this, operationPyDoPendingWork, NoResult, LinkableConstant::globalObject(*this, node)));
    noResult(node);
}

void SpeculativeJIT::compilePyValueOrNothing(Node* node)
{
    JSValueOperand value(this, node->child1());
    GPRTemporary result(this, Reuse, value);
    move(value.gpr(), result.gpr());
    jsValueResult(result.gpr(), node);
}

void SpeculativeJIT::compilePyCheckInitializerResult(Node* node)
{
    JSValueOperand value(this, node->child1());
    GPRReg valueGPR = value.gpr();
    Jump isNotNone = branchIfNotUndefined(valueGPR);
    addSlowPathGenerator(slowPathCall(isNotNone, this, operationPyRaiseInitializerResult, NoResult, LinkableConstant::globalObject(*this, node), valueGPR));
    noResult(node);
}

void SpeculativeJIT::compilePyFloorDivOrMod(Node* node)
{
#if CPU(X86_64)
    // The machine has its own idea of where these are to be.
    SpeculateStrictInt32Operand left(this, node->child1());
    SpeculateStrictInt32Operand right(this, node->child2());
    GPRTemporary eax(this, X86Registers::eax);
    GPRTemporary edx(this, X86Registers::edx);
    GPRTemporary divisor(this);
    GPRTemporary scratch(this);
    GPRReg quotientGPR = eax.gpr();
    GPRReg remainderGPR = edx.gpr();
    GPRReg rightGPR = divisor.gpr();
    GPRReg scratchGPR = scratch.gpr();
    move(right.gpr(), rightGPR);
    move(left.gpr(), quotientGPR);
#else
    SpeculateStrictInt32Operand left(this, node->child1());
    SpeculateStrictInt32Operand right(this, node->child2());
    GPRTemporary quotient(this);
    GPRTemporary remainder(this);
    GPRTemporary scratch(this);
    GPRReg leftGPR = left.gpr();
    GPRReg rightGPR = right.gpr();
    GPRReg quotientGPR = quotient.gpr();
    GPRReg remainderGPR = remainder.gpr();
    GPRReg scratchGPR = scratch.gpr();
#endif

    // By 0 is an error. By -1 is what can be too big.
    add32(TrustedImm32(1), rightGPR, scratchGPR);
    speculationCheck(ExitKind::Overflow, JSValueSource(), nullptr, branch32(BelowOrEqual, scratchGPR, TrustedImm32(1)));
#if CPU(X86_64)
    x86ConvertToDoubleWord32();
    x86Div32(rightGPR);
#elif CPU(ARM64)
    div32(leftGPR, rightGPR, quotientGPR);
    multiplySub32(quotientGPR, rightGPR, leftGPR, remainderGPR);
#else
#error "Fixup is not to make these where there is nothing to divide with."
#endif
    // The machine rounds toward 0, and Python down.
    Jump isExact = branchTest32(Zero, remainderGPR);
    xor32(remainderGPR, rightGPR, scratchGPR);
    Jump hasSameSign = branch32(GreaterThanOrEqual, scratchGPR, TrustedImm32(0));
    sub32(TrustedImm32(1), quotientGPR);
    add32(rightGPR, remainderGPR);
    hasSameSign.link(this);
    isExact.link(this);
    strictInt32Result(node->op() == PyMod ? remainderGPR : quotientGPR, node);
}

void SpeculativeJIT::compilePyCheckDivisor(Node* node)
{
    SpeculateDoubleOperand divisor(this, node->child1());
    FPRTemporary zero(this);
    moveZeroToDouble(zero.fpr());
    speculationCheck(ExitKind::Overflow, JSValueSource(), nullptr, branchDouble(DoubleEqualAndOrdered, divisor.fpr(), zero.fpr()));
    noResult(node);
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
