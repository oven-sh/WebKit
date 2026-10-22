/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSavedFrameEmitter.h"

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTBuildContext.h"
#include "AOTCallEmitter.h"
#include "AOTCoroutines.h"
#include "AOTFrameMapEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubCatalog.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static_assert(awaitConditionsOnState.size() == 2);
static_assert(awaitConditionsOnState[0].word == AwaitConditionWord { AwaitConditionBase::Instance, AOT_INSTANCE_OFFSET(coroutines.frameThatMayContinue), sizeof(void*), false });
static_assert(awaitConditionsOnState[0].test == AwaitConditionTest::EqualsFramePointer);
static_assert(awaitConditionsOnState[1].word == AwaitConditionWord { AwaitConditionBase::MicrotaskQueue, MicrotaskQueue::offsetOfStart(), sizeof(size_t), false });
static_assert(awaitConditionsOnState[1].other == AwaitConditionWord { AwaitConditionBase::MicrotaskQueue, MicrotaskQueue::offsetOfEnd(), sizeof(size_t), false });
static_assert(awaitConditionsOnState[1].test == AwaitConditionTest::EqualsOther);

CoroutineKind SavedFrameEmitter::kindOfFunction() const
{
    return coroutineOfBody(m_function).kind;
}

LValue SavedFrameEmitter::cellOrZero(ValueID driver)
{
    if (representationOf(driver) == Representation::Cell)
        return in<CellPointer>(driver).value();
    Boxed boxed = in<Boxed>(driver);
    return m_out.select(isCell(boxed).value(), boxed.value(), m_out.int64Zero);
}

// The site word names the location of this word, and a constant has none, so nothing here is folded by type. Who reads the word tells a promise from undefined.
LValue SavedFrameEmitter::driverAsPassed(ValueID driver)
{
    if (representationOf(driver) == Representation::Cell)
        return in<CellPointer>(driver).value();
    return in<Boxed>(driver).value();
}

StubCallResult SavedFrameEmitter::suspend(const ValueContext& value, const Suspension& suspension)
{
    const StubCatalog& catalog = inputs().stubCatalog;
    const StubSummary& summary = catalog.summaryOf(suspension.key);
    const StubSignature& signature = summary.signature;
    std::array<LValue, 3> operands { suspension.firstOperand, suspension.secondOperand, suspension.thirdOperand };
    unsigned numberOfOperands = suspension.thirdOperand ? 3 : suspension.secondOperand ? 2 : 1;
    RELEASE_ASSERT(suspension.firstOperand && (suspension.secondOperand || !suspension.thirdOperand));
    RELEASE_ASSERT(signature.numberOfOperands == numberOfOperands + 1);
    Reg siteWordRegister = signature.operands[numberOfOperands];
    bool hasMode = signature.result == StubResult::TwoGPRs;
    RELEASE_ASSERT(hasMode || signature.result == StubResult::GPR);
    LType type = hasMode ? m_proc.addTuple({ B3::Int64, B3::Int64 }) : B3::Int64;

    RegisterSet lateClobbered = summary.clobbered;
    lateClobbered.merge(RegisterSet::macroClobberedGPRs());
    lateClobbered.add(siteWordRegister, IgnoreVectors);
    CallPatchpointRequest request {
        .resultType = type,
        .earlyClobbered = RegisterSet::macroClobberedGPRs(),
        .lateClobbered = lateClobbered,
        .effects = catalog.effectsOf(suspension.key),
        .argumentBufferAccess = ArgumentBufferAccess::ReadsAndWrites,
        .holdsUniqueRecord = true,
    };

    LValue driver = driverAsPassed(suspension.driver);
    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    B3::PatchpointValue* patchpoint = calls.createCallPatchpoint(value, request);
    patchpoint->setNeedsFrameRecord(true);
    for (unsigned index = 0; index < numberOfOperands; ++index)
        patchpoint->append(B3::ConstrainedValue(operands[index], B3::ValueRep::reg(signature.operands[index])));
    unsigned driverChild = m_proc.resultCount(type) + patchpoint->numChildren();
    patchpoint->append(B3::ConstrainedValue(driver, B3::ValueRep::LateColdAny));
    FrameMapEmitter::Roots roots = emitter<FrameMapEmitter>().appendRoots(patchpoint, value);
    patchpoint->resultConstraints = { B3::ValueRep::reg(signature.firstResult) };
    if (hasMode)
        patchpoint->resultConstraints.append(B3::ValueRep::reg(signature.secondResult));

    uint32_t site = calls.addSite(value);
    patchpoint->setGenerator([records = &m_records, build = &build(), function = m_id, valueID = value.id(), key = suspension.key, site, roots, driverChild, siteWordGPR = siteWordRegister.gpr()](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        std::optional<uint64_t> word = FrameMapEmitter::siteWordFor(records->frameMaps, params, roots, driverChild);
        if (!word) {
            build->diagnostics().add(InternalError { .kind = InternalErrorKind::FrameMapNotEncodable, .function = function, .value = valueID });
            jit.breakpoint();
            return;
        }
        if (*word >> 32)
            jit.move(CCallHelpers::TrustedImm64(*word), siteWordGPR);
        else
            jit.move(CCallHelpers::TrustedImm32(static_cast<uint32_t>(*word)), siteWordGPR);
        records->stubCalls.call(jit, key, OptionalIndex { site });
    });
    calls.finishCallPatchpoint(value, patchpoint, site, request);

    if (!hasMode)
        return { patchpoint, nullptr };
    return { m_out.extract(patchpoint, 0), m_out.extract(patchpoint, 1) };
}

// B3 gives all FramePointer values of a function one value, and a frame that was saved goes on at another address. A suspension writes every heap, so nothing moves this read across one.
Pointer SavedFrameEmitter::framePointerAtThisPoint()
{
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::pointerType());
    patchpoint->effects = B3::Effects::none();
    patchpoint->effects.reads = B3::HeapRange::top();
    patchpoint->setNeedsFrameRecord(true);
    patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        jit.move(GPRInfo::callFrameRegister, params[0].gpr());
    });
    return Pointer(patchpoint);
}

// The conditions on which the stub AwaitBySavingFrame goes on without saving the frame. Whatever queues a job calls the run time, so the words of the queue are run-time state.
void SavedFrameEmitter::generateAwaitMayContinue(const ValueContext& value)
{
    const B3::AbstractHeap& queueWords = m_heaps.entire(AbstractHeap::RuntimeState);
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock isFrameOfLastJob = newBlock();
        Pointer frameThatMayContinue(m_out.loadPtr(mutableInstanceField(AOT_INSTANCE_OFFSET(coroutines.frameThatMayContinue))));
        branch(equal(frameThatMayContinue, framePointerAtThisPoint()), { B3::FrequentedBlock(isFrameOfLastJob, targets.taken.frequency()), targets.notTaken });

        m_out.appendTo(isFrameOfLastJob);
        LValue queue = m_out.loadPtr(mutableInstanceField(offsetOfMicrotaskQueue));
        Integer start(m_out.load64(m_out.address(queueWords, queue, MicrotaskQueue::offsetOfStart())));
        Integer end(m_out.load64(m_out.address(queueWords, queue, MicrotaskQueue::offsetOfEnd())));
        branch(equal(start, end), targets);
    });
}

void SavedFrameEmitter::generateAwaitBySavingFrame(const ValueContext& value)
{
    ValueID awaited = value->awaitedOperand();
    ValueID driver = value->promiseOperand();
    switch (kindOfFunction()) {
    case CoroutineKind::AsyncFunction: {
        AwaitedOperand form = value->takesExtraTurn() ? AwaitedOperand::ValueAfterTurn : AwaitedOperand::Value;
        define(value, Boxed(suspend(value, { awaitBySavingFrame(form), in<Boxed>(awaited).value(), nullptr, nullptr, driver }).first));
        return;
    }
    case CoroutineKind::AsyncGenerator:
        // The engine resumes an async generator, and none of its jobs waits a turn more.
        if (value->takesExtraTurn()) {
            fail(value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
        define(value, Boxed(suspend(value, { AOT::suspendGenerator(GeneratorSuspension::AsyncAwait), cellOrZero(driver), in<Boxed>(awaited).value(), nullptr, driver }).first));
        return;
    case CoroutineKind::Generator:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SavedFrameEmitter::generateAwaitCallBySavingFrame(const ValueContext& value)
{
    if (kindOfFunction() != CoroutineKind::AsyncFunction) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    Vector<Operand, 8> arguments;
    for (ValueID argument : value->argumentsOperands())
        arguments.append(get(argument));
    arguments.append(undefined());
    Boxed returned = emitter<CallEmitter>().callDirect(value, value->function(), arguments.span(), { .isTailCall = value->isDemotedTailCall() }).as<Boxed>();
    define(value, Boxed(suspend(value, { awaitCallBySavingFrame(), returned.value(), nullptr, nullptr, value->promiseOperand() }).first));
}

void SavedFrameEmitter::suspendGenerator(const ValueContext& value, GeneratorSuspension form, Boxed yielded)
{
    ValueID generator = value->generatorOperand();
    defineYieldResults(value, suspend(value, { AOT::suspendGenerator(form), in<CellPointer>(generator).value(), yielded.value(), nullptr, generator }));
}

void SavedFrameEmitter::defineYieldResults(const ValueContext& value, StubCallResult result)
{
    std::array<Operand, numberOfEnumerators<YieldResult>> results;
    results[static_cast<size_t>(YieldResult::Sent)] = Boxed(result.first);
    results[static_cast<size_t>(YieldResult::Mode)] = low32(Integer(result.second));
    defineResults(value, results);
}

void SavedFrameEmitter::generateYieldBySavingFrame(const ValueContext& value)
{
    switch (kindOfFunction()) {
    case CoroutineKind::Generator:
        suspendGenerator(value, GeneratorSuspension::Yield, in<Boxed>(value->valueOperand()));
        return;
    case CoroutineKind::AsyncGenerator:
        suspendGenerator(value, GeneratorSuspension::AsyncYield, in<Boxed>(value->valueOperand()));
        return;
    case CoroutineKind::AsyncFunction:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SavedFrameEmitter::generateAsyncGeneratorYield(const ValueContext& value)
{
    switch (kindOfFunction()) {
    case CoroutineKind::AsyncGenerator:
        suspendGenerator(value, GeneratorSuspension::AsyncYieldAfterAwait, in<Boxed>(value->valueOperand()));
        return;
    case CoroutineKind::Generator:
    case CoroutineKind::AsyncFunction:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
