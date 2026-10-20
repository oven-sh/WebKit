/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTGenerator.h"
#include "AOTStubHelpers.h"
#include "JSGenerator.h"
#include "MacroAssemblerCodeRef.h"

namespace JSC { namespace AOT {

enum class AwaitedOperand : uint8_t { Value, ResultOfCallWithoutPromise };
enum class GeneratorSuspension : uint8_t { Yield, AsyncAwait, AsyncYield, AsyncYieldAfterAwait };

constexpr StubKey newPromise() { return StubKey(StubFamily::NewPromise); }
constexpr StubKey returnAsyncResult() { return StubKey(StubFamily::ReturnAsyncResult); }
constexpr StubKey awaitBySavingFrame(AwaitedOperand operand = AwaitedOperand::Value) { return StubKey(StubFamily::AwaitBySavingFrame).withForm(static_cast<uint8_t>(operand)); }
constexpr StubKey awaitCallBySavingFrame() { return awaitBySavingFrame(AwaitedOperand::ResultOfCallWithoutPromise); }
constexpr StubKey suspendGenerator(GeneratorSuspension suspension) { return StubKey(StubFamily::SuspendGenerator).withForm(static_cast<uint8_t>(suspension)); }
constexpr StubKey stepGenerator(GeneratorStepCheck check) { return StubKey(StubFamily::StepGenerator).withForm(static_cast<uint8_t>(check)); }
constexpr StubKey enterSavedFrame() { return StubKey(StubFamily::EnterSavedFrame); }
constexpr StubKey restoreSavedFrameToStack() { return StubKey(StubFamily::RestoreSavedFrameToStack); }

static_assert(static_cast<unsigned>(AwaitedOperand::ResultOfCallWithoutPromise) + 1 == descriptorOf(StubFamily::AwaitBySavingFrame).numberOfForms);
static_assert(static_cast<unsigned>(GeneratorSuspension::AsyncYieldAfterAwait) + 1 == descriptorOf(StubFamily::SuspendGenerator).numberOfForms);
static_assert(numberOfEnumerators<GeneratorStepCheck> == descriptorOf(StubFamily::StepGenerator).numberOfForms);
static_assert(!static_cast<unsigned>(GeneratorStepCheck::IsUnmodifiedGenerator) && static_cast<unsigned>(GeneratorStepCheck::IsGenerator) == 1 && static_cast<unsigned>(GeneratorStepCheck::IsUnmodified) == 2 && static_cast<unsigned>(GeneratorStepCheck::None) == 3);
static_assert(exists(newPromise()) && exists(returnAsyncResult()) && exists(awaitCallBySavingFrame()) && exists(suspendGenerator(GeneratorSuspension::AsyncYieldAfterAwait)));
static_assert(exists(stepGenerator(GeneratorStepCheck::None)) && exists(enterSavedFrame()) && exists(restoreSavedFrameToStack()));

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

// Appends its failing branches to otherwise and falls through on success.
void emitUnmodifiedGeneratorCheck(StubAssembler&, GPRReg generator, GPRReg scratch, JumpList& otherwise);

#if CPU(ARM64)
enum class PairsSavedInFrame : uint8_t { AreRestored, AreStored };
// The pairs of suspensionClobberedGPRs that the function saves, at -16, -32, -48 from the frame pointer. The number of pairs is the field of SavedFrame::savedRegisterBits bits at bit shift of word.
void emitMovePairsSavedInFrame(StubAssembler&, GPRReg word, unsigned shift, PairsSavedInFrame);
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
