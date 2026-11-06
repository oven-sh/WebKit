/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTGenerator.h"
#include "AOTStubHelpers.h"
#include "JSGenerator.h"
#include "MacroAssemblerCodeRef.h"

namespace JSC { namespace AOT {

enum class PromiseHandler : uint8_t { OnFulfilled, OnRejected };
enum class AwaitedOperand : uint8_t { Value, ResultOfCallWithoutPromise, ValueAfterTurn, ValueInModuleBody };
enum class GeneratorSuspension : uint8_t { Yield, AsyncAwait, AsyncYield, AsyncYieldAfterAwait };
// A step of a loop or of `yield*`, or an awaited call of `next`: that goes on under the async context of the await, a step under the one that is active where the step is finished.
enum class StepConsumer : uint8_t { AsyncFunction, AsyncGenerator, AsyncFunctionAwaitingCall, AsyncGeneratorAwaitingCall };
enum class NewGeneratorKind : uint8_t { Sync, Async };

constexpr StubKey newPromise() { return StubKey(StubFamily::NewPromise); }
// PerformPromiseThen with one handler, which is callable, and a promise of the realm as the capability.
constexpr StubKey promiseThen(PromiseHandler handler) { return StubKey(StubFamily::PromiseThen).withForm(static_cast<uint8_t>(handler)); }
constexpr StubKey returnAsyncResult() { return StubKey(StubFamily::ReturnAsyncResult); }
constexpr StubKey awaitBySavingFrame(AwaitedOperand operand = AwaitedOperand::Value) { return StubKey(StubFamily::AwaitBySavingFrame).withForm(static_cast<uint8_t>(operand)); }
constexpr StubKey awaitCallBySavingFrame() { return awaitBySavingFrame(AwaitedOperand::ResultOfCallWithoutPromise); }
// For an async function that has not been suspended yet and whose SavedFrame somebody has made ahead of the suspension.
constexpr StubKey awaitWithSavedFrame() { return StubKey(StubFamily::AwaitWithSavedFrame); }
constexpr StubKey suspendGenerator(GeneratorSuspension suspension) { return StubKey(StubFamily::SuspendGenerator).withForm(static_cast<uint8_t>(suspension)); }
constexpr StubKey stepGenerator(GeneratorStepCheck check) { return StubKey(StubFamily::StepGenerator).withForm(static_cast<uint8_t>(check)); }
constexpr StubKey enterSavedFrame() { return StubKey(StubFamily::EnterSavedFrame); }
// The same adapter for the job loop where the job at hand calls the handler of a promise reaction or a function that queueMicrotask was given. The callee says which realm is entered.
constexpr StubKey enterHandlerOfJob() { return StubKey(StubFamily::EnterHandlerOfJob); }
// A step goes on in the function of a heap frame, or by copying the frame to the stack. Either way the frame that goes on returns to the label behind this call.
constexpr StubKey enterGeneratorFrame() { return StubKey(StubFamily::EnterGeneratorFrame); }
constexpr StubKey restoreSavedFrameToStack() { return StubKey(StubFamily::RestoreSavedFrameToStack); }
constexpr StubKey stepAsyncGenerator(StepConsumer consumer) { return StubKey(StubFamily::StepAsyncGenerator).withForm(static_cast<uint8_t>(consumer)); }
// What all forms of StepAsyncGenerator end in. One body, so that the frame of a generator's body has one return address whoever consumes it.
constexpr StubKey runAsyncGeneratorStep() { return StubKey(StubFamily::RunAsyncGeneratorStep); }
constexpr StubKey stepAsyncIteratorInEngine() { return StubKey(StubFamily::StepAsyncIteratorInEngine); }
constexpr StubKey newHeapFrame() { return StubKey(StubFamily::NewHeapFrame); }
constexpr StubKey newGenerator(NewGeneratorKind kind) { return StubKey(StubFamily::NewGenerator).withForm(static_cast<uint8_t>(kind)); }

static_assert(static_cast<unsigned>(PromiseHandler::OnRejected) + 1 == descriptorOf(StubFamily::PromiseThen).numberOfForms && exists(promiseThen(PromiseHandler::OnRejected)));
static_assert(static_cast<unsigned>(AwaitedOperand::ValueInModuleBody) + 1 == descriptorOf(StubFamily::AwaitBySavingFrame).numberOfForms);
static_assert(static_cast<unsigned>(GeneratorSuspension::AsyncYieldAfterAwait) + 1 == descriptorOf(StubFamily::SuspendGenerator).numberOfForms);
static_assert(numberOfEnumerators<GeneratorStepCheck> == descriptorOf(StubFamily::StepGenerator).numberOfForms);
static_assert(!static_cast<unsigned>(GeneratorStepCheck::IsUnmodifiedGenerator) && static_cast<unsigned>(GeneratorStepCheck::IsGenerator) == 1 && static_cast<unsigned>(GeneratorStepCheck::IsUnmodified) == 2 && static_cast<unsigned>(GeneratorStepCheck::None) == 3);
static_assert(static_cast<unsigned>(StepConsumer::AsyncGeneratorAwaitingCall) + 1 == descriptorOf(StubFamily::StepAsyncGenerator).numberOfForms);
static_assert(static_cast<unsigned>(NewGeneratorKind::Async) + 1 == descriptorOf(StubFamily::NewGenerator).numberOfForms);
static_assert(exists(newPromise()) && exists(returnAsyncResult()) && exists(awaitCallBySavingFrame()) && exists(suspendGenerator(GeneratorSuspension::AsyncYieldAfterAwait)));
static_assert(exists(awaitBySavingFrame(AwaitedOperand::ValueAfterTurn)) && exists(awaitBySavingFrame(AwaitedOperand::ValueInModuleBody)));
static_assert(exists(stepGenerator(GeneratorStepCheck::None)) && exists(enterSavedFrame()) && exists(restoreSavedFrameToStack()) && exists(enterGeneratorFrame()));
static_assert(exists(stepAsyncGenerator(StepConsumer::AsyncFunctionAwaitingCall)) && exists(stepAsyncGenerator(StepConsumer::AsyncGeneratorAwaitingCall)));
static_assert(exists(stepAsyncGenerator(StepConsumer::AsyncFunction)) && exists(stepAsyncGenerator(StepConsumer::AsyncGenerator)) && exists(runAsyncGeneratorStep()) && exists(stepAsyncIteratorInEngine()));
static_assert(bodyOf(stepAsyncIteratorInEngine()) == runAsyncGeneratorStep());
static_assert(exists(enterHandlerOfJob()) && bodyOf(enterHandlerOfJob()) == enterSavedFrame());
static_assert(exists(awaitWithSavedFrame()) && bodyOf(awaitWithSavedFrame()) == awaitBySavingFrame(AwaitedOperand::Value) && !static_cast<unsigned>(AwaitedOperand::Value));
static_assert(exists(newGenerator(NewGeneratorKind::Sync)) && exists(newGenerator(NewGeneratorKind::Async)));

// At the site of a yield the resume mode. At the site of a StepAsyncGenerator the status of the step.
inline constexpr StubRegister resumeModeRegister = StubRegister::T9;

constexpr JSGenerator::ResumeMode engineResumeModeOf(ResumeMode mode)
{
    switch (mode) {
    case ResumeMode::Next:
        return JSGenerator::ResumeMode::NormalMode;
    case ResumeMode::Throw:
        return JSGenerator::ResumeMode::ThrowMode;
    case ResumeMode::Return:
        return JSGenerator::ResumeMode::ReturnMode;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr uint32_t resumeModeBitsOfEngineMode(uint32_t engineMode) { return (engineMode << 1 | engineMode >> 1) & 3; }

static_assert(resumeModeBitsOfEngineMode(static_cast<uint32_t>(engineResumeModeOf(ResumeMode::Next))) == static_cast<uint32_t>(ResumeMode::Next));
static_assert(resumeModeBitsOfEngineMode(static_cast<uint32_t>(engineResumeModeOf(ResumeMode::Throw))) == static_cast<uint32_t>(ResumeMode::Throw));
static_assert(resumeModeBitsOfEngineMode(static_cast<uint32_t>(engineResumeModeOf(ResumeMode::Return))) == static_cast<uint32_t>(ResumeMode::Return));

CodePtr<JSEntryPtrTag> savedFrameEntrypoint();
CodePtr<JSEntryPtrTag> handlerOfJobEntrypoint();

// For a stub whose site reads resumeModeRegister beside the result.
inline StubSignature signatureWithResumeMode(std::initializer_list<Reg> operands)
{
    StubSignature signature = StubSignature::withGPRResult(operands);
    signature.result = StubResult::TwoGPRs;
    signature.secondResult = gprOf(resumeModeRegister);
    return signature;
}

// A condition names the registers that it reads by its words.
struct AwaitRegisters { GPRReg savedFrame, awaited, word, other; };
struct NewSavedFrameRegisters { GPRReg saved, count, allocator, scratch; };
struct JobRegisters { GPRReg queue, slot, next, scratch; };
struct HeadJobRegisters { GPRReg queue, slot, word; };
struct PromiseAllJobRegisters { GPRReg context, values, value, index; };
struct YieldDeliveryRegisters { GPRReg yielded, producer, driver, consumerFrame, word, other; };
struct GeneratorJobRegisters { GPRReg queue, slot, word, driver, saved, mode, value; };

// AOTStubsCoroutineResumption.cpp
enum class RegistersSavedInFrame : uint8_t { AreRestored, AreStored };
// The callee saves below the frame pointer that the SavedFrame::savedRegisterBits bits at bit shift of word name. On ARM64: a number of pairs of suspensionClobberedGPRs, at -16, -32, -48.
void emitMoveRegistersSavedInFrame(StubAssembler&, GPRReg word, unsigned shift, RegistersSavedInFrame);

// AOTStubsCoroutineSuspension.cpp
// Leaves the tested word in registers.word after EqualsOther and HasNoBit, and after HasAnyBit on a word wider than a byte.
void emitBranchUnlessHolds(StubAssembler&, const AwaitCondition&, const AwaitRegisters&, JumpList& doesNotHold);
// The frame is that of the frame pointer.
void emitBranchUnlessFrameMayContinue(StubAssembler&, const AwaitRegisters&, JumpList& mustSuspend);
// Taken while the VM tracks no async context: every context that could be captured or found is undefined. Leaves the VM in the register.
[[nodiscard]] Jump emitBranchIfTracksNoAsyncContext(StubAssembler&, GPRReg vm);
// AsyncContextSwapScope::current under a VM that tracks async contexts: what a job that is scheduled now captures, and what a job that goes on now runs under.
void emitLoadCurrentAsyncContext(StubAssembler&, GPRReg context);
// What SavedFrame::create does for the frame of the frame pointer, and for an async function what saveAsyncFunction does with its promise.
[[nodiscard]] AssemblyHelpers::FreeListRefill emitAllocateSavedFrame(StubAssembler&, CoroutineKind, GPRReg siteWord, const NewSavedFrameRegisters&, JumpList& needsOperation);
// JSPromise::resolveWithInternalMicrotaskForAsyncAwait(awaited, task, generator) for a pending promise of the realm and for a value that is settled as it is: registers.savedFrame is the generator.
void emitRunGeneratorJobWhenSettled(StubAssembler&, InternalMicrotask task, const AwaitRegisters&, const JobRegisters&, std::optional<GPRReg> siteWord, JumpList& isLeftToEngine);

// AOTStubsCoroutinePromises.cpp
void emitBranchIfMayBeThenable(StubAssembler&, GPRReg value, GPRReg structure, GPRReg scratch, JumpList& mayBeThenable);
// JSPromise::resolvePromise for a pending promise of the realm and a value that is no thenable, where the promise has no reaction, or one in its own two words whose job is queued.
void emitFulfillPromise(StubAssembler&, GPRReg promise, GPRReg value, GPRReg other, const JobRegisters&, JumpList& needsEngine);
// promiseAllResolveJob for the fulfilled job that emitLoadHeadJob has found, whole. Where it leaves the job to the job loop, nothing is stored.
void emitRunPromiseAllResolveJob(StubAssembler&, const HeadJobRegisters&, const PromiseAllJobRegisters&, GPRReg scratch, JumpList& isLeftToJobLoop);

// AOTStubsCoroutineSteps.cpp
// For a producer that the job loop entered and that has just suspended at a yield. Stores nothing before the last branch to slowPath.
void emitDeliverYieldToSuspendedConsumer(StubAssembler&, const YieldDeliveryRegisters&, JumpList& slowPath);
// What asyncGeneratorDispatchSuspend does with the value of a yield that nobody could take at once: it is awaited, and the job behind the await finishes the step. registers.savedFrame is the generator.
void emitAwaitYieldedValue(StubAssembler&, const AwaitRegisters&, const JobRegisters&, JumpList& isLeftToEngine);
// Each one is for the job at the head of the queue, as emitLoadHeadJob has left it, once its first word has named it.
void emitTakeBodyCallJob(StubAssembler&, const GeneratorJobRegisters&, JumpList& isLeftToJobLoop);
void emitTakeDriverResumeJob(StubAssembler&, const GeneratorJobRegisters&, JumpList& isLeftToJobLoop);
// Behind other jobs the driver gets its job at the end of the queue, and nothing is left to enter.
void emitTakeYieldAwaitedJob(StubAssembler&, const GeneratorJobRegisters&, JumpList& isLeftToJobLoop, JumpList& hasQueuedDriver);

// AOTStubsCoroutineJobs.cpp
// IsStored: the job runs under a context captured earlier, which the caller stores as the last argument between emitReserveJob and emitFillJob.
enum class JobAsyncContext : uint8_t { Current, IsStored };
struct JobOperands {
    InternalMicrotask task;
    JSPromise::Status status;
    GPRReg resolution;
    GPRReg context;
    JobAsyncContext asyncContext;
    EncodedJSValue first { JSValue::ValueUndefined };
};
// Stores nothing. Between the two, scratch is free and nothing may be queued.
void emitReserveJob(StubAssembler&, const JobRegisters&, JumpList& needsOperation);
// The job that JSGlobalObject::queueMicrotask(vm, task, status, first, resolution, context, asyncContext) queues.
void emitFillJob(StubAssembler&, const JobOperands&, const JobRegisters&);
// For whoever runs the job at the head of the queue in the place of the job loop. Leaves the queue, the address of the job and its first word, which names the task, the status and the realm. Takes nothing.
void emitLoadHeadJob(StubAssembler&, const HeadJobRegisters&, JumpList& noJob);
Address argumentOfJob(GPRReg slot, unsigned index);
// A job runs under the async context that it has captured: what AsyncContextSwapScope does at its start.
void emitPutAsyncContextOfJobInPlace(StubAssembler&, std::optional<Address> contextOfJob, GPRReg scratch, GPRReg other, JumpList& isLeftToJobLoop);
void emitDequeueHeadJob(StubAssembler&, GPRReg queue, GPRReg scratch);
// For a job whose four arguments the caller has stored in the reserved place.
void emitPublishJob(StubAssembler&, InternalMicrotask, JSPromise::Status, const JobRegisters&);
// For a PromiseReactionJob that emitLoadHeadJob has found. Takes it if its promise is a plain promise of the realm and its last argument is the async context or nothing, not a context for the handler.
enum class ReactionJob : uint8_t { HasNoAsyncContext, HasAsyncContext };
void emitTakeReactionJob(StubAssembler&, ReactionJob, const HeadJobRegisters&, GPRReg promise, GPRReg handler, GPRReg argument, JumpList& isLeftToJobLoop);
// For a BunPerformMicrotaskJob that emitLoadHeadJob has found. Takes it if it calls a function without arguments and no async context is in place or to be put in place.
void emitTakePerformMicrotaskJob(StubAssembler&, const HeadJobRegisters&, GPRReg function, JumpList& isLeftToJobLoop);
// For a CompiledAsyncFunctionResume job that emitLoadHeadJob has found. Takes it if the function is of this instance. Where it leaves the job to the job loop, the queue is as it was.
void emitTakeFunctionResumeJob(StubAssembler&, const HeadJobRegisters&, GPRReg saved, GPRReg value, JumpList& isLeftToJobLoop);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
