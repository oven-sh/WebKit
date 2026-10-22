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
enum class AwaitedOperand : uint8_t { Value, ResultOfCallWithoutPromise, ValueAfterTurn };
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
constexpr StubKey restoreSavedFrameToStack() { return StubKey(StubFamily::RestoreSavedFrameToStack); }
constexpr StubKey stepAsyncGenerator(StepConsumer consumer) { return StubKey(StubFamily::StepAsyncGenerator).withForm(static_cast<uint8_t>(consumer)); }
// What all forms of StepAsyncGenerator end in. One body, so that the frame of a generator's body has one return address whoever consumes it.
constexpr StubKey runAsyncGeneratorStep() { return StubKey(StubFamily::RunAsyncGeneratorStep); }
constexpr StubKey stepAsyncIteratorInEngine() { return StubKey(StubFamily::StepAsyncIteratorInEngine); }
constexpr StubKey newGenerator(NewGeneratorKind kind) { return StubKey(StubFamily::NewGenerator).withForm(static_cast<uint8_t>(kind)); }

static_assert(static_cast<unsigned>(PromiseHandler::OnRejected) + 1 == descriptorOf(StubFamily::PromiseThen).numberOfForms && exists(promiseThen(PromiseHandler::OnRejected)));
static_assert(static_cast<unsigned>(AwaitedOperand::ValueAfterTurn) + 1 == descriptorOf(StubFamily::AwaitBySavingFrame).numberOfForms);
static_assert(static_cast<unsigned>(GeneratorSuspension::AsyncYieldAfterAwait) + 1 == descriptorOf(StubFamily::SuspendGenerator).numberOfForms);
static_assert(numberOfEnumerators<GeneratorStepCheck> == descriptorOf(StubFamily::StepGenerator).numberOfForms);
static_assert(!static_cast<unsigned>(GeneratorStepCheck::IsUnmodifiedGenerator) && static_cast<unsigned>(GeneratorStepCheck::IsGenerator) == 1 && static_cast<unsigned>(GeneratorStepCheck::IsUnmodified) == 2 && static_cast<unsigned>(GeneratorStepCheck::None) == 3);
static_assert(static_cast<unsigned>(StepConsumer::AsyncGeneratorAwaitingCall) + 1 == descriptorOf(StubFamily::StepAsyncGenerator).numberOfForms);
static_assert(static_cast<unsigned>(NewGeneratorKind::Async) + 1 == descriptorOf(StubFamily::NewGenerator).numberOfForms);
static_assert(exists(newPromise()) && exists(returnAsyncResult()) && exists(awaitCallBySavingFrame()) && exists(suspendGenerator(GeneratorSuspension::AsyncYieldAfterAwait)));
static_assert(exists(awaitBySavingFrame(AwaitedOperand::ValueAfterTurn)));
static_assert(exists(stepGenerator(GeneratorStepCheck::None)) && exists(enterSavedFrame()) && exists(restoreSavedFrameToStack()));
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

// Appends its failing branches to otherwise and falls through on success.
void emitUnmodifiedGeneratorCheck(StubAssembler&, GPRReg generator, GPRReg scratch, JumpList& otherwise);

#if CPU(ARM64)
enum class PairsSavedInFrame : uint8_t { AreRestored, AreStored };
// The pairs of suspensionClobberedGPRs that the function saves, at -16, -32, -48 from the frame pointer. The number of pairs is the field of SavedFrame::savedRegisterBits bits at bit shift of word.
void emitMovePairsSavedInFrame(StubAssembler&, GPRReg word, unsigned shift, PairsSavedInFrame);
#endif

// AOTStubsCoroutineSuspension.cpp
// A condition names the registers that it reads by its words. The others may be InvalidGPRReg.
struct AwaitRegisters {
    GPRReg savedFrame;
    GPRReg awaited;
    GPRReg word;
    GPRReg other;
};
// Leaves the tested word in registers.word after EqualsOther and HasNoBit, and after HasAnyBit on a word wider than a byte.
void emitBranchUnlessHolds(StubAssembler&, const AwaitCondition&, const AwaitRegisters&, JumpList& doesNotHold);
// The frame is that of the frame pointer.
void emitBranchUnlessFrameMayContinue(StubAssembler&, const AwaitRegisters&, JumpList& mustSuspend);
// Taken while the VM tracks no async context: every context that could be captured or found is undefined. Leaves the VM in the register.
[[nodiscard]] Jump emitBranchIfTracksNoAsyncContext(StubAssembler&, GPRReg vm);
// AsyncContextSwapScope::current under a VM that tracks async contexts: what a job that is scheduled now captures, and what a job that goes on now runs under.
void emitLoadCurrentAsyncContext(StubAssembler&, GPRReg context);
struct NewSavedFrameRegisters {
    GPRReg saved;
    GPRReg count;
    GPRReg allocator;
    GPRReg scratch;
};
// What SavedFrame::create does for the frame of the frame pointer, and for an async function what saveAsyncFunction does with its promise. The stack is as at the entry of the stub.
// The words behind the header stay unwritten: a SavedFrame lists none of them until a frame is saved in it.
// The caller emits the refill behind an exit of its fast path. Nothing is allocated before the last exit.
[[nodiscard]] AssemblyHelpers::FreeListRefill emitAllocateSavedFrame(StubAssembler&, CoroutineKind, GPRReg siteWord, const NewSavedFrameRegisters&, JumpList& needsOperation);
struct JobRegisters;
// JSPromise::resolveWithInternalMicrotaskForAsyncAwait(awaited, task, generator) for a pending promise of the realm and for a value that is settled as it is: registers.savedFrame is the generator.
// Leaves the sentinel in the place of the awaited value: what a body returns that has seen to its own resumption. Stores nothing before the last branch to isLeftToEngine.
// The barrier on a promise overwrites the register of a site word, which is kept for whoever names it. For the others such a promise is left to the engine.
void emitRunGeneratorJobWhenSettled(StubAssembler&, InternalMicrotask task, const AwaitRegisters&, const JobRegisters&, std::optional<GPRReg> siteWord, JumpList& isLeftToEngine);

// AOTStubsCoroutinePromises.cpp
void emitBranchIfMayBeThenable(StubAssembler&, GPRReg value, GPRReg structure, GPRReg scratch, JumpList& mayBeThenable);
struct JobRegisters;
// JSPromise::resolvePromise for a pending promise of the realm and a value that is no thenable, where the promise has no reaction, or one in its own two words whose job is queued:
// a handler for its fulfillment, or a coroutine of compiled code that waits for it. Stores nothing before the last branch to needsEngine.
void emitFulfillPromise(StubAssembler&, GPRReg promise, GPRReg value, GPRReg other, const JobRegisters&, JumpList& needsEngine);
struct HeadJobRegisters;
struct PromiseAllJobRegisters {
    GPRReg context;
    GPRReg values;
    GPRReg value;
    GPRReg index;
};
// promiseAllResolveJob for the fulfilled job that emitLoadHeadJob has found, whole. Where it leaves the job to the job loop, nothing is stored.
void emitRunPromiseAllResolveJob(StubAssembler&, const HeadJobRegisters&, const PromiseAllJobRegisters&, GPRReg scratch, JumpList& isLeftToJobLoop);

// AOTStubsCoroutineSteps.cpp
struct YieldDeliveryRegisters {
    GPRReg yielded;
    GPRReg producer;
    GPRReg driver;
    GPRReg consumerFrame;
    GPRReg word;
    GPRReg other;
};
// For a producer that the job loop entered and that has just suspended at a yield. Stores nothing before the last branch to slowPath.
void emitDeliverYieldToSuspendedConsumer(StubAssembler&, const YieldDeliveryRegisters&, JumpList& slowPath);
// What asyncGeneratorDispatchSuspend does with the value of a yield that nobody could take at once: it is awaited, and the job behind the await finishes the step.
// registers.savedFrame is the generator. Leaves the sentinel in the place of the value. Stores nothing before the last branch to isLeftToEngine.
void emitAwaitYieldedValue(StubAssembler&, const AwaitRegisters&, const JobRegisters&, JumpList& isLeftToEngine);
struct GeneratorJobRegisters {
    GPRReg queue;
    GPRReg slot;
    GPRReg word;
    GPRReg driver;
    GPRReg saved;
    GPRReg mode;
    GPRReg value;
};
// Each one is for the job at the head of the queue, as emitLoadHeadJob has left it, once its first word has named it. It takes the job and leaves what is entered in its place:
// the generator or the SavedFrame of the async function as the driver, the SavedFrame to restore, and the value. Where it leaves the job to the job loop, nothing is stored.
// The caller has the mode of a body call. The other two resume with ResumeMode::Next.
void emitTakeBodyCallJob(StubAssembler&, const GeneratorJobRegisters&, JumpList& isLeftToJobLoop);
void emitTakeDriverResumeJob(StubAssembler&, const GeneratorJobRegisters&, JumpList& isLeftToJobLoop);
// Behind other jobs the driver gets its job at the end of the queue, and nothing is left to enter.
void emitTakeYieldAwaitedJob(StubAssembler&, const GeneratorJobRegisters&, JumpList& isLeftToJobLoop, JumpList& hasQueuedDriver);

// AOTStubsCoroutineJobs.cpp
struct JobRegisters {
    GPRReg queue;
    GPRReg slot;
    GPRReg next;
    GPRReg scratch;
};
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
// For a job that runs under a context captured earlier: between the two.
void emitStoreAsyncContextOfJob(StubAssembler&, const JobRegisters&, GPRReg asyncContext);
// The job that JSGlobalObject::queueMicrotask(vm, task, status, first, resolution, context, asyncContext) queues.
void emitFillJob(StubAssembler&, const JobOperands&, const JobRegisters&);
struct HeadJobRegisters {
    GPRReg queue;
    GPRReg slot;
    GPRReg word;
};
// For whoever runs the job at the head of the queue in the place of the job loop. Leaves the queue, the address of the job and its first word, which names the task, the status and the realm. Takes nothing.
void emitLoadHeadJob(StubAssembler&, const HeadJobRegisters&, JumpList& noJob);
Address argumentOfJob(GPRReg slot, unsigned index);
// A job runs under the async context that it has captured: what AsyncContextSwapScope does at its start. Whoever entered the adapter has such a scope of its own, which puts back at its end what was there
// in front of its job, whatever the jobs behind it have left. Without an address the job has captured nothing. The last thing in front of the taking of a job: stores nothing before its last branch.
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
