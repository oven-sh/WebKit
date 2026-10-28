/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsBarriers.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTHeapHooks.h"
#include "AOTInstance.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "Butterfly.h"
#include "CellState.h"
#include "JSObject.h"
#include "StoredCellLog.h"

namespace JSC { namespace AOT {

using Returns = OperationCall::Returns;
using Saves = OperationCall::Saves;

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

// Does what Heap::addToRememberedSet() does for a black owner while no collector runs concurrently. Falls through when the segment is full, which the operation handles.
static void returnIfMarkStackTakes(StubAssembler& jit, GPRReg owner, GPRReg scratch)
{
#if CPU(X86_64)
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
#if ASSERT_ENABLED
    jit.addPtr(TrustedImm32(1), Address(scratch, MutatorMarkStackLayout::offsetOfTopInSegment));
#endif
    jit.storePtr(owner, BaseIndex(scratch, top, CCallHelpers::TimesEight, MutatorMarkStackLayout::offsetOfFirstCellInSegment));
    jit.store8(TrustedImm32(static_cast<int32_t>(CellState::PossiblyGrey)), Address(owner, JSCell::cellStateOffset()));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(heap.barriersExecuted)), scratch);
    jit.addPtr(TrustedImm32(1), Address(scratch));
    jit.returnToCaller();
    leftToOperation.link(&jit);
#elif CPU(ARM64) && !ASSERT_ENABLED
    // An addition to a word in memory and the store of a constant take the macro assembler's scratch register, so neither comes while top is in it.
    static_assert(MutatorMarkStackLayout::segmentCapacity < 1 << 12, "The capacity is the immediate of one comparison.");
    const GPRReg top = jit.scratchRegister();
    RELEASE_ASSERT(noOverlap(owner, scratch, top));
    JumpList leftToOperation;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(heap.mutatorMarkStack)), scratch);
    // The tests of stubs run them on an instance that has no heap.
    leftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    jit.loadPtr(Address(scratch, MutatorMarkStackLayout::offsetOfTop()), top);
    leftToOperation.append(jit.branch64(CCallHelpers::AboveOrEqual, top, TrustedImm32(MutatorMarkStackLayout::segmentCapacity)));
    jit.add64(TrustedImm32(1), top);
    jit.storePtr(top, Address(scratch, MutatorMarkStackLayout::offsetOfTop()));
    jit.loadPtr(Address(scratch, MutatorMarkStackLayout::offsetOfHeadSegment()), scratch);
    jit.getEffectiveAddress(BaseIndex(scratch, top, CCallHelpers::TimesEight), scratch);
    jit.storePtr(owner, Address(scratch, MutatorMarkStackLayout::offsetOfFirstCellInSegment - static_cast<ptrdiff_t>(sizeof(JSCell*))));
    jit.store8(TrustedImm32(static_cast<int32_t>(CellState::PossiblyGrey)), Address(owner, JSCell::cellStateOffset()));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(heap.barriersExecuted)), scratch);
    jit.addPtr(TrustedImm32(1), Address(scratch));
    jit.returnToCaller();
    leftToOperation.link(&jit);
#else
    UNUSED_PARAM(jit);
    UNUSED_PARAM(owner);
    UNUSED_PARAM(scratch);
#endif
}

static void returnUnlessOperationRemembers(StubAssembler& jit, GPRReg owner, GPRReg scratch)
{
    Jump isNotFenced = jit.branchTest8(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced)));
    jit.memoryFence();
    Jump isBlack = jit.barrierBranchWithoutFence(owner, true);
    jit.returnToCaller();

    isNotFenced.link(&jit);
    returnIfMarkStackTakes(jit, owner, scratch);
    isBlack.link(&jit);
}

static void rememberOwnerAndReturn(StubAssembler& jit, GPRReg owner)
{
    jit.callOperationAndReturn(Entry::operationAOTBarrierSlow, { .returns = Returns::Void, .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = owner });
}

StubSignature signatureOfWriteBarrier(StubKey key)
{
    return StubSignature::withoutResult({ key.operandRegister().value_or(gprOf(firstStubOperandRegister)) });
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
    return StubSignature::withoutResult({ gprOf(StubRegister::T9), gprOf(StubRegister::T11) });
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
    Jump isTooShortForLog = jit.branch32(CCallHelpers::Below, Address(scratch, Butterfly::offsetOfVectorLength()), TrustedImm32(StoredCellLog::minVectorLengthOfOwner));
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

    // StoredCellLog::tryRemember() declines short arrays, so remember the array like any other owner.
    isTooShortForLog.link(&jit);
    returnIfMarkStackTakes(jit, array, scratch);

    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTRememberElement,
        { .returns = Returns::Void, .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = array, .thirdArgumentIn = value });
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
