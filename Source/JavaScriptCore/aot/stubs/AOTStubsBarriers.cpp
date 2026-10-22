/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsBarriers.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTMutatorMarkStackLayout.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "Butterfly.h"
#include "CellState.h"
#include "JSObject.h"
#include "StoredCellLog.h"

namespace JSC { namespace AOT {

Jump branchIfBarrierIsNeeded(StubAssembler& jit, GPRReg owner, GPRReg scratch)
{
    jit.load8(Address(owner, JSCell::cellStateOffset()), scratch);
    return jit.branch32(CCallHelpers::BelowOrEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(barrierThreshold)));
}

void returnIfFencedAndNotBlack(StubAssembler& jit, GPRReg owner)
{
    Jump isNotFenced = jit.branchTest8(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced)));
    jit.memoryFence();
    Jump isBlack = jit.barrierBranchWithoutFence(owner, true);
    jit.returnToCaller();

    isNotFenced.link(&jit);
    isBlack.link(&jit);
}

// What Heap::addToRememberedSet() does while no collector runs beside the program. A full segment is left to it.
static void returnUnlessOperationRemembers(StubAssembler& jit, GPRReg owner, GPRReg scratch)
{
    Jump isNotFenced = jit.branchTest8(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced)));
    jit.memoryFence();
    Jump isBlack = jit.barrierBranchWithoutFence(owner, true);
    jit.returnToCaller();

    isNotFenced.link(&jit);
#if CPU(X86_64) && !ASSERT_ENABLED
    constexpr GPRReg top = CCallHelpers::s_scratchRegister;
    RELEASE_ASSERT(noOverlap(owner, scratch, top));
    JumpList leftToOperation;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(heap.mutatorMarkStack)), scratch);
    // The tests of stubs run them on an instance that has no heap.
    leftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    jit.loadPtr(Address(scratch, MutatorMarkStackLayout::offsetOfTop()), top);
    leftToOperation.append(jit.branch64(CCallHelpers::AboveOrEqual, top, TrustedImm32(MutatorMarkStackLayout::segmentCapacity)));
    jit.addPtr(TrustedImm32(1), Address(scratch, MutatorMarkStackLayout::offsetOfTop()));
    jit.loadPtr(Address(scratch, MutatorMarkStackLayout::offsetOfHeadSegment()), scratch);
    jit.storePtr(owner, BaseIndex(scratch, top, CCallHelpers::TimesEight, MutatorMarkStackLayout::offsetOfFirstCellInSegment));
    jit.store8(TrustedImm32(static_cast<int32_t>(CellState::PossiblyGrey)), Address(owner, JSCell::cellStateOffset()));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(heap.barriersExecuted)), scratch);
    jit.addPtr(TrustedImm32(1), Address(scratch));
    jit.returnToCaller();
    leftToOperation.link(&jit);
#else
    UNUSED_PARAM(scratch);
#endif
    isBlack.link(&jit);
}

static void rememberOwnerAndReturn(StubAssembler& jit, GPRReg owner)
{
    jit.callOperationAndReturn(Entry::operationAOTBarrierSlow, {
        .returns = OperationCall::Returns::Void,
        .firstArgument = OperationCall::FirstArgument::Instance,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = owner,
    });
}

StubSignature signatureOfWriteBarrier(StubKey key)
{
    StubSignature signature;
    signature.addOperand(key.operandRegister().value_or(gprOf(firstStubOperandRegister)));
    return signature;
}

void generateWriteBarrier(StubAssembler& jit)
{
    const GPRReg owner = jit.operand(0);
    const GPRReg state = jit.allocateScratch(StubRegister::T9);

    Jump isWithinThreshold = branchIfBarrierIsNeeded(jit, owner, state);
    jit.returnToCaller();

    isWithinThreshold.link(&jit);
    // The test is made where the owner lies. The rest is written once.
    if (jit.key().operandRegister()) {
        if (owner != gprOf(firstStubOperandRegister))
            jit.move(owner, jit.allocateScratch(firstStubOperandRegister));
        jit.jumpToStub(writeBarrier());
        return;
    }
    returnUnlessOperationRemembers(jit, owner, state);
    rememberOwnerAndReturn(jit, owner);
}

StubSignature signatureOfRememberElement(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::T9));
    signature.addOperand(gprOf(StubRegister::T11));
    return signature;
}

void generateRememberElement(StubAssembler& jit)
{
    static_assert(!static_cast<unsigned>(CellState::PossiblyBlack));
    const GPRReg array = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    const GPRReg log = jit.allocateScratch(StubRegister::T13);
    JumpList slowPath;
    JumpList done;

    Jump isFenced = jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced)));
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), scratch);
    slowPath.append(jit.branch32(CCallHelpers::Below, Address(scratch, Butterfly::offsetOfVectorLength()), TrustedImm32(StoredCellLog::minVectorLengthOfOwner)));
    done.append(jit.branchIfNotCell(value));
    done.append(jit.branchTest8(CCallHelpers::Zero, Address(value, JSCell::cellStateOffset())));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(heap.storedCellLog)), log);
    jit.loadPtr(Address(log, StoredCellLog::offsetOfCursor()), scratch);
    slowPath.append(jit.branchPtr(CCallHelpers::Equal, scratch, Address(log, StoredCellLog::offsetOfEnd())));
    jit.storePtr(value, Address(scratch));
    jit.addPtr(TrustedImm32(sizeof(JSCell*)), scratch);
    jit.storePtr(scratch, Address(log, StoredCellLog::offsetOfCursor()));
    done.link(&jit);
    jit.returnToCaller();

    isFenced.link(&jit);
    jit.memoryFence();
    slowPath.append(jit.barrierBranchWithoutFence(array, true));
    jit.returnToCaller();

    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTRememberElement, {
        .returns = OperationCall::Returns::Void,
        .firstArgument = OperationCall::FirstArgument::Instance,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = array,
        .thirdArgumentIn = value,
    });
}

Label returnAfterStoring(StubAssembler& jit, GPRReg owner, GPRReg value)
{
    const GPRReg state = jit.reuseScratch(StubRegister::T11);
    RELEASE_ASSERT(noOverlap(owner, value, state));

    Jump isNotCell = jit.branchIfNotCell(value);
    Label storedCell = jit.label();
    Jump isWithinThreshold = branchIfBarrierIsNeeded(jit, owner, state);
    isNotCell.link(&jit);
    jit.returnToCaller();

    isWithinThreshold.link(&jit);
    returnUnlessOperationRemembers(jit, owner, state);
    if (descriptorOf(jit.key().family()).clobbers == StubClobbers::ClobbersLikeCall) {
        jit.move(owner, operationArgumentGPR(1));
        jit.tailCallOperation(Entry::operationAOTBarrierSlow, OperationCall::FirstArgument::Instance);
    } else
        rememberOwnerAndReturn(jit, owner);
    return storedCell;
}

PreludeExit emitBarrierPrelude(StubAssembler& jit)
{
    // A plain call saves no registers, so there is nothing to skip.
    if (jit.key().family() == StubFamily::CallOperationPreservingRegisters)
        returnIfFencedAndNotBlack(jit, operationArgumentGPR(1));
    return PreludeExit::CallsOperation;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
