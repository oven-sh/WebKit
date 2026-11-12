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
#include <array>

namespace JSC { namespace AOT {

enum class PromiseHandler : uint8_t { OnFulfilled, OnRejected };
enum class ReactionChainReceiver : uint8_t { Promise, FulfilledValue };
enum class AwaitedOperand : uint8_t { Value, ResultOfCallWithoutPromise, ValueInModuleBody, Resolution };
enum class GeneratorSuspension : uint8_t { Yield, AsyncAwait, AsyncYield, AsyncYieldAfterAwait };
enum class StepConsumer : uint8_t { AsyncFunction, AsyncGenerator };
enum class GeneratorFunctionOrigin : uint8_t { Program, Library };
enum class RegistersSavedInAsyncFrame : uint8_t { None, RBX, R15, Both };
enum class AsyncResult : uint8_t { Any, Undefined };
inline constexpr unsigned numberOfSetsSavedInAsyncFrame = static_cast<unsigned>(RegistersSavedInAsyncFrame::Both) + 1;

constexpr StubKey newPromise() { return StubKey(StubFamily::NewPromise); }
constexpr StubKey newPromiseBeforeBody(unsigned argument) { return StubKey(StubFamily::NewPromiseBeforeBody).withForm(static_cast<uint8_t>(argument)); }
constexpr StubKey returnFulfilledPromise() { return StubKey(StubFamily::ReturnFulfilledPromise); }
constexpr StubKey promiseThen(PromiseHandler handler) { return StubKey(StubFamily::PromiseThen).withForm(static_cast<uint8_t>(handler)); }
constexpr StubKey newReactionChain(unsigned numberOfWords) { return StubKey(StubFamily::NewReactionChain).withCount(numberOfWords); }
constexpr StubKey startReactionChain(ReactionChainReceiver receiver) { return StubKey(StubFamily::StartReactionChain).withForm(static_cast<uint8_t>(receiver)); }
constexpr StubKey resolveReturnedPromise() { return StubKey(StubFamily::ResolveReturnedPromise); }
constexpr StubKey returnAsyncResult() { return StubKey(StubFamily::ReturnAsyncResult); }
constexpr StubKey returnAsyncResultInTailPosition(RegistersSavedInAsyncFrame saved, AsyncResult result = AsyncResult::Any) { return StubKey(StubFamily::ReturnAsyncResultInTailPosition).withForm(static_cast<uint8_t>(static_cast<unsigned>(result) * numberOfSetsSavedInAsyncFrame + static_cast<unsigned>(saved))); }
constexpr StubKey awaitBySavingFrame(AwaitedOperand operand = AwaitedOperand::Value) { return StubKey(StubFamily::AwaitBySavingFrame).withForm(static_cast<uint8_t>(operand)); }
constexpr StubKey awaitCallBySavingFrame() { return awaitBySavingFrame(AwaitedOperand::ResultOfCallWithoutPromise); }
constexpr StubKey awaitWithSavedFrame() { return StubKey(StubFamily::AwaitWithSavedFrame); }
constexpr StubKey suspendGenerator(GeneratorSuspension suspension) { return StubKey(StubFamily::SuspendGenerator).withForm(static_cast<uint8_t>(suspension)); }
constexpr StubKey stepGenerator(GeneratorStepCheck check) { return StubKey(StubFamily::StepGenerator).withForm(static_cast<uint8_t>(check)); }
constexpr StubKey resumeSteppedGenerator() { return StubKey(StubFamily::ResumeSteppedGenerator); }
inline constexpr StubRegister frameRegisterOfLeafStep = StubRegister::T10;
inline constexpr StubRegister generatorRegisterOfLeafStep = StubRegister::T12;
constexpr StubKey enterSavedFrame() { return StubKey(StubFamily::EnterSavedFrame); }
constexpr StubKey resumeNextJob() { return StubKey(StubFamily::ResumeNextJob); }
#if CPU(X86_64)
constexpr StubKey handlerOfJobReturn() { return StubKey(StubFamily::HandlerOfJobReturn); }
#endif
constexpr StubKey enterHandlerOfJob() { return StubKey(StubFamily::EnterHandlerOfJob); }
constexpr StubKey enterGeneratorFrame() { return StubKey(StubFamily::EnterGeneratorFrame); }
constexpr StubKey restoreSavedFrameToStack() { return StubKey(StubFamily::RestoreSavedFrameToStack); }
constexpr StubKey stepAsyncGenerator(StepConsumer consumer) { return StubKey(StubFamily::StepAsyncGenerator).withForm(static_cast<uint8_t>(consumer)); }
constexpr StubKey runAsyncGeneratorStep() { return StubKey(StubFamily::RunAsyncGeneratorStep); }
constexpr StubKey stepAsyncIteratorInEngine() { return StubKey(StubFamily::StepAsyncIteratorInEngine); }
constexpr StubKey newHeapFrame() { return StubKey(StubFamily::NewHeapFrame); }
constexpr StubKey isNotThenable() { return StubKey(StubFamily::IsNotThenable); }
constexpr StubKey newAsyncGenerator(GeneratorFunctionOrigin origin) { return StubKey(StubFamily::NewAsyncGenerator).withForm(static_cast<uint8_t>(origin)); }

static_assert(static_cast<unsigned>(PromiseHandler::OnRejected) + 1 == descriptorOf(StubFamily::PromiseThen).numberOfForms && exists(promiseThen(PromiseHandler::OnRejected)));
static_assert(static_cast<unsigned>(AwaitedOperand::Resolution) + 1 == descriptorOf(StubFamily::AwaitBySavingFrame).numberOfForms);
static_assert(static_cast<unsigned>(GeneratorSuspension::AsyncYieldAfterAwait) + 1 == descriptorOf(StubFamily::SuspendGenerator).numberOfForms);
static_assert(numberOfEnumerators<GeneratorStepCheck> == descriptorOf(StubFamily::StepGenerator).numberOfForms);
static_assert(!static_cast<unsigned>(GeneratorStepCheck::IsUnmodifiedGenerator) && static_cast<unsigned>(GeneratorStepCheck::IsGenerator) == 1 && static_cast<unsigned>(GeneratorStepCheck::IsUnmodified) == 2 && static_cast<unsigned>(GeneratorStepCheck::None) == 3);
static_assert(static_cast<unsigned>(StepConsumer::AsyncGenerator) + 1 == descriptorOf(StubFamily::StepAsyncGenerator).numberOfForms);
static_assert(2 * numberOfSetsSavedInAsyncFrame == descriptorOf(StubFamily::ReturnAsyncResultInTailPosition).numberOfForms);
inline constexpr unsigned registersOfNewPromiseBeforeBody = descriptorOf(StubFamily::NewPromiseBeforeBody).numberOfForms;
static_assert(exists(newPromise()) && exists(returnAsyncResult()) && exists(awaitCallBySavingFrame()) && exists(suspendGenerator(GeneratorSuspension::AsyncYieldAfterAwait)));
static_assert(exists(awaitBySavingFrame(AwaitedOperand::ValueInModuleBody)) && exists(awaitBySavingFrame(AwaitedOperand::Resolution)));
static_assert(exists(stepGenerator(GeneratorStepCheck::None)) && exists(enterSavedFrame()) && exists(restoreSavedFrameToStack()) && exists(enterGeneratorFrame()));
static_assert(exists(stepAsyncGenerator(StepConsumer::AsyncFunction)) && exists(stepAsyncGenerator(StepConsumer::AsyncGenerator)) && exists(runAsyncGeneratorStep()) && exists(stepAsyncIteratorInEngine()));
static_assert(bodyOf(stepAsyncIteratorInEngine()) == runAsyncGeneratorStep());
static_assert(exists(enterHandlerOfJob()) && bodyOf(enterHandlerOfJob()) == enterSavedFrame());
static_assert(exists(resumeNextJob()) && bodyOf(resumeNextJob()) == enterSavedFrame());
static_assert(exists(awaitWithSavedFrame()) && bodyOf(awaitWithSavedFrame()) == awaitBySavingFrame(AwaitedOperand::Value) && !static_cast<unsigned>(AwaitedOperand::Value));
static_assert(static_cast<unsigned>(GeneratorFunctionOrigin::Library) + 1 == descriptorOf(StubFamily::NewAsyncGenerator).numberOfForms);
static_assert(exists(newAsyncGenerator(GeneratorFunctionOrigin::Program)) && exists(newAsyncGenerator(GeneratorFunctionOrigin::Library)) && exists(newHeapFrame()));

inline constexpr StubRegister resumeModeRegister = StubRegister::T9;

inline constexpr int32_t resumesFunctionOfJobBit = 4;
static_assert(!(static_cast<int32_t>(ResumeMode::Next) & resumesFunctionOfJobBit) && !(static_cast<int32_t>(ResumeMode::Throw) & resumesFunctionOfJobBit) && !(static_cast<int32_t>(ResumeMode::Return) & resumesFunctionOfJobBit));

#if CPU(X86_64)
inline constexpr std::array<uint8_t, 2> moveIntoSiteWordOfAwaitedCall { 0x43, 0xba };
static_assert(gprOf(resumeModeRegister) == X86Registers::r10);

inline constexpr std::array<uint8_t, 4> leaveFunctionIfCarryIsSet { 0x73, 0x02, 0xc9, 0xc3 };
#endif

#if CPU(X86_64)
inline constexpr std::array<uint8_t, 4> headOfDelegationMark { 0x0f, 0x1f, 0x84, 0x00 };
inline constexpr int32_t delegationMark = 0x64656c67;
inline constexpr ptrdiff_t offsetOfDelegationMark = sizeof(headOfDelegationMark);
#else
inline constexpr int32_t delegationMark = static_cast<int32_t>(0xd2800000u | 0x6c67u << 5 | 31u);
inline constexpr ptrdiff_t offsetOfDelegationMark = 0;
#endif

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

inline StubSignature signatureWithResumeMode(std::initializer_list<Reg> operands)
{
    StubSignature signature = StubSignature::withGPRResult(operands);
    signature.result = StubResult::TwoGPRs;
    signature.secondResult = gprOf(resumeModeRegister);
    return signature;
}

struct AwaitRegisters { GPRReg savedFrame, awaited, word, other; };
struct NewSavedFrameRegisters { GPRReg saved, count, allocator, scratch; };
struct JobRegisters { GPRReg queue, slot, next, scratch; };
struct HeadJobRegisters { GPRReg queue, slot, word; };
struct PromiseAllJobRegisters { GPRReg context, values, value, index; };
struct ReactionChainRegisters { GPRReg chain, promise, target, handler, argument; };
struct ResolveThenableRegisters { GPRReg promise, promiseToResolve, other; };
struct YieldDeliveryRegisters { GPRReg yielded, producer, driver, consumerFrame, word, other; };
struct GeneratorJobRegisters { GPRReg queue, slot, word, driver, saved, mode, value; };

enum class RegistersSavedInFrame : uint8_t { AreRestored, AreStored };
void emitMoveRegistersSavedInFrame(StubAssembler&, GPRReg word, unsigned shift, RegistersSavedInFrame);
void emitSiteContinuationNote(StubAssembler&);

void emitBranchUnlessHolds(StubAssembler&, const AwaitCondition&, const AwaitRegisters&, JumpList& doesNotHold);
void emitBranchUnlessFrameMayContinue(StubAssembler&, const AwaitRegisters&, JumpList& mustSuspend);
#if CPU(X86_64)
struct AwaitedResultRegisters { GPRReg returnAddress, awaitStub, scratch; };
void emitBranchUnlessResultIsAwaited(StubAssembler&, Address returnAddress, const AwaitedResultRegisters&, JumpList& otherwise);
void emitReturnFulfillmentIfTakenInsteadOfPromise(StubAssembler&, GPRReg value, GPRReg result, const AwaitedResultRegisters&);
#endif
[[nodiscard]] Jump emitBranchIfTracksNoAsyncContext(StubAssembler&, GPRReg vm);
void emitLoadCurrentAsyncContext(StubAssembler&, GPRReg context);
[[nodiscard]] AssemblyHelpers::FreeListRefill emitAllocateSavedFrame(StubAssembler&, CoroutineKind, GPRReg siteWord, const NewSavedFrameRegisters&, JumpList& needsOperation);
void emitRunGeneratorJobWhenSettled(StubAssembler&, InternalMicrotask task, const AwaitRegisters&, const JobRegisters&, std::optional<GPRReg> siteWord, JumpList& isLeftToEngine);

void emitBranchIfMayBeThenable(StubAssembler&, GPRReg value, GPRReg structure, GPRReg scratch, JumpList& mayBeThenable);
void emitFulfillPromise(StubAssembler&, GPRReg promise, GPRReg value, GPRReg other, const JobRegisters&, JumpList& mayBeThenable, JumpList& needsEngine);
void emitQueueResolutionWithFulfillment(StubAssembler&, GPRReg promise, GPRReg value, GPRReg other, const JobRegisters&, JumpList& isNotQueued);
void emitResolvePromise(StubAssembler&, GPRReg promise, GPRReg value, GPRReg other, const JobRegisters&, JumpList& needsEngine);
void emitRunResolveThenableJob(StubAssembler&, GPRReg head, const ResolveThenableRegisters&, const JobRegisters&, JumpList& isLeftToJobLoop);
[[nodiscard]] Label emitRunResolveWithoutHandlerJob(StubAssembler&, GPRReg head, GPRReg promise, GPRReg value, const JobRegisters&, JumpList& isNoPromise, JumpList& isLeftToJobLoop);
void emitQueueResolutionWithPromise(StubAssembler&, GPRReg promise, GPRReg resolution, GPRReg other, const JobRegisters&, JumpList& needsEngine);
void emitTakeReactionChainStepJob(StubAssembler&, const HeadJobRegisters&, const ReactionChainRegisters&, JumpList& isForOtherStatus, JumpList& isLeftToJobLoop);
void emitBranchIfReactionChainGoesOn(StubAssembler&, GPRReg chain, GPRReg index, GPRReg scratch, JumpList& goesOn);
void emitAdvanceReactionChain(StubAssembler&, GPRReg chain);
void emitResolveReactionChain(StubAssembler&, GPRReg chain, GPRReg value, GPRReg other, const JobRegisters&, JumpList& needsEngine);
void emitRunSettleReactionChainJob(StubAssembler&, GPRReg head, GPRReg chain, GPRReg value, GPRReg structureID, const JobRegisters&, JumpList& isLeftToJobLoop);
void emitBranchIfRaceIsSettled(StubAssembler&, GPRReg head, GPRReg promise, GPRReg scratch, JumpList& isSettled, JumpList& isLeftToJobLoop);
void emitNewSettledResult(StubAssembler&, GPRReg head, GPRReg word);
void emitRunPromiseAllResolveJob(StubAssembler&, const HeadJobRegisters&, const PromiseAllJobRegisters&, GPRReg scratch, JumpList& isLeftToJobLoop, Address element);

void emitDeliverYieldToSuspendedConsumer(StubAssembler&, const YieldDeliveryRegisters&, JumpList& slowPath);
void emitAwaitYieldedValue(StubAssembler&, const AwaitRegisters&, const JobRegisters&, JumpList& isLeftToEngine);
void emitTakeBodyCallJob(StubAssembler&, const GeneratorJobRegisters&, JumpList& isLeftToJobLoop);
void emitTakeDriverResumeJob(StubAssembler&, const GeneratorJobRegisters&, JumpList& isLeftToJobLoop);
void emitTakeYieldAwaitedJob(StubAssembler&, const GeneratorJobRegisters&, JumpList& isLeftToJobLoop, JumpList& hasQueuedDriver);

enum class JobAsyncContext : uint8_t { Current, IsStored };
struct JobOperands {
    InternalMicrotask task;
    JSPromise::Status status;
    GPRReg resolution;
    GPRReg context;
    JobAsyncContext asyncContext;
    EncodedJSValue first { JSValue::ValueUndefined };
};
void emitReserveJob(StubAssembler&, const JobRegisters&, JumpList& needsOperation);
void emitFillJob(StubAssembler&, const JobOperands&, const JobRegisters&);
void emitLoadHeadJob(StubAssembler&, const HeadJobRegisters&, JumpList& noJob);
Address argumentOfJob(GPRReg slot, unsigned index);
void emitPutAsyncContextOfJobInPlace(StubAssembler&, std::optional<Address> contextOfJob, GPRReg scratch, GPRReg other, JumpList& isLeftToJobLoop);
void emitDequeueHeadJob(StubAssembler&, GPRReg queue, GPRReg scratch);
enum class PrefetchedJobs : uint8_t { Reactions, ThenableResolutions, Resolutions, FunctionResumes };
void emitDequeueHeadJobAndPrefetch(StubAssembler&, GPRReg queue, GPRReg scratch, PrefetchedJobs);
void emitPublishJob(StubAssembler&, InternalMicrotask, JSPromise::Status, const JobRegisters&);
enum class ReactionJob : uint8_t { HasNoAsyncContext, HasAsyncContext };
void emitTakeReactionJob(StubAssembler&, ReactionJob, const HeadJobRegisters&, GPRReg promise, GPRReg handler, GPRReg argument, JumpList& isLeftToJobLoop);
void emitTakePerformMicrotaskJob(StubAssembler&, const HeadJobRegisters&, GPRReg function, JumpList& isLeftToJobLoop);
void emitTakeFunctionResumeJob(StubAssembler&, const HeadJobRegisters&, GPRReg saved, GPRReg value, JumpList& isLeftToJobLoop);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
