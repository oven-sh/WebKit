/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCoroutines.h"

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTStubsBarriers.h"
#include "JSGlobalObject.h"
#include "JSPromise.h"

namespace JSC { namespace AOT {

StubSignature signatureOfNewPromise(StubKey)
{
    return StubSignature::withGPRResult({ });
}

void generateNewPromise(StubAssembler& jit)
{
    const GPRReg promise = jit.result();
    const GPRReg allocator = jit.allocateScratch(StubRegister::T9);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    static_assert(JSPromise::offsetOfSlot() == JSPromise::offsetOfPacked() + static_cast<ptrdiff_t>(sizeof(uint64_t)));
    static_assert(JSObject::butterflyOffset() == sizeof(uint64_t) && !JSValue::ValueEmpty);
    JumpList slowCases;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Promise)].allocator)), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    auto refill = allocateCellWithLateRefill(jit, promise, allocator, scratch, slowCases);
    jit.load64(Address(instanceGPR, offsetOfPromiseHeader), scratch);
#if CPU(ARM64)
    jit.storePair64(scratch, ARM64Registers::zr, promise, TrustedImm32(0));
    jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, promise, TrustedImm32(JSPromise::offsetOfPacked()));
#else
    jit.store64(scratch, Address(promise));
    jit.store64(TrustedImm32(0), Address(promise, JSObject::butterflyOffset()));
    jit.store64(TrustedImm32(0), Address(promise, JSPromise::offsetOfPacked()));
    jit.store64(TrustedImm32(0), Address(promise, JSPromise::offsetOfSlot()));
#endif
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowCases);
    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNewPromise, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = false });
}

static void emitNonThenableCheck(StubAssembler& jit, GPRReg object, GPRReg structure, GPRReg scratch, JumpList& otherwise)
{
    loadStructure(jit, object, structure);
    jit.load32(Address(structure, Structure::bitFieldOffset()), scratch);
    jit.and32(TrustedImm32(Structure::s_definitelyNonThenableStateBits), scratch);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(static_cast<int32_t>(Structure::DefinitelyNonThenableState::NonThenable) << Structure::s_definitelyNonThenableStateShift)));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
    otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(structure, Structure::realmOffset())));
    otherwise.append(branchIfWatchpointSetIsNotWatched(jit, Address(scratch, OBJECT_OFFSETOF(JSGlobalObject, m_promiseThenWatchpointSet)), scratch));
    // The Structure of a compiled object does not show its extra properties, and "then" may be one.
    Jump isNotCompiled = jit.branch8(CCallHelpers::NotEqual, Address(object, JSCell::typeInfoTypeOffset()), TrustedImm32(CompiledObjectType));
    otherwise.append(jit.branchTest32(CCallHelpers::NonZero, Address(object, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)));
    isNotCompiled.link(&jit);
}

static void emitBranchIfMayBeThenable(StubAssembler& jit, GPRReg value, GPRReg structure, GPRReg scratch, JumpList& mayBeThenable)
{
    Jump isNotCell = jit.branchIfNotCell(value);
    Jump isNotObject = jit.branchIfNotObject(value);
    emitNonThenableCheck(jit, value, structure, scratch, mayBeThenable);
    isNotObject.link(&jit);
    isNotCell.link(&jit);
}

StubSignature signatureOfReturnAsyncResult(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

void generateReturnAsyncResult(StubAssembler& jit)
{
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
    const GPRReg promise = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg result = jit.result();
    const GPRReg structure = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = result;
    RELEASE_ASSERT(promise == operationArgumentGPR(1) && value == operationArgumentGPR(2));
    RELEASE_ASSERT(noOverlap(promise, value, result, structure, instanceGPR));
    RELEASE_ASSERT(result == gprOf(firstStubOperandRegister));

    JumpList slowCases;
    Jump hasPromise = jit.branchTestPtr(CCallHelpers::NonZero, promise);
    emitBranchIfMayBeThenable(jit, value, structure, scratch, slowCases);
    jit.loadPtr(Address(fp, sizeof(void*)), scratch);
    slowCases.append(jit.branchPtr(CCallHelpers::Equal, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs) + StubWords::offsetOfReturnAddress(StubFrameKind::SavedFrameAdapter))));
    jit.move(value, result);
    jit.returnToCaller();

    hasPromise.link(&jit);
    slowCases.append(jit.branchTest64(CCallHelpers::NonZero, Address(promise, JSPromise::offsetOfPacked())));
    emitBranchIfMayBeThenable(jit, value, structure, scratch, slowCases);
    jit.store64(value, Address(promise, JSPromise::offsetOfSlot()));
    jit.move(TrustedImm64(static_cast<int64_t>(fulfilledPromiseBits)), scratch);
    jit.store64(scratch, Address(promise, JSPromise::offsetOfPacked()));
    jit.move(promise, result);
    Jump valueIsNotCell = jit.branchIfNotCell(value);
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, promise, structure);
    valueIsNotCell.link(&jit);
    jit.returnToCaller();

    // The barrier takes its owner in the result register and preserves it.
    needsBarrier.link(&jit);
    jit.emitFunctionPrologue();
    jit.callStub(writeBarrier());
    jit.emitFunctionEpilogue();
    jit.returnToCaller();

    // Not a tail call: the operation finds the async function's frame as the caller of the stub's frame.
    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTResolvePromiseOrReturnResult, { });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
