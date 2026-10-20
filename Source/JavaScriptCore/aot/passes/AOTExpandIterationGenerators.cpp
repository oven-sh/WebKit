/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFuseGeneratorLoops.h"

namespace JSC { namespace AOT { namespace Typed {

static ArmState protocolStateOf(IterationExpansion& expansion, const ArmState& state)
{
    BuiltinID next = state.arm.form == IterationForm::AsyncGenerator ? Builtins::AsyncGeneratorPrototypeNext : Builtins::GeneratorPrototypeNext;
    return ArmState { .arm = IterationArm { }, .subject = state.subject, .nextMethod = expansion.appendBuiltin(next) };
}

static GeneratorStepCheck checkForLookup(const ArmState& state)
{
    return state.arm.namesAreProven ? GeneratorStepCheck::None : GeneratorStepCheck::IsUnmodified;
}

static GeneratorStepCheck checkForMode(const ArmState& state, ResumeMode mode)
{
    switch (mode) {
    case ResumeMode::Next:
        return GeneratorStepCheck::None;
    case ResumeMode::Throw:
    case ResumeMode::Return:
        return checkForLookup(state);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueID appendStepGenerator(IterationExpansion& expansion, ValueID generator, ResumeMode mode, ValueID sent, GeneratorStepCheck check)
{
    Value step = IRBuilder::make(Opcode::StepGenerator, { generator, sent });
    step.setResumeMode(mode);
    step.setCheck(check);
    return expansion.builder().append(WTF::move(step));
}

static ValueID appendStepResult(IterationExpansion& expansion, ValueID step, StepGeneratorResult result)
{
    return expansion.appendResultAt(step, static_cast<uint32_t>(result));
}

static ValueID appendStatusTest(IterationExpansion& expansion, ValueID status, GeneratorStepStatus expected)
{
    return expansion.appendIsEqualToInteger(status, static_cast<int64_t>(expected));
}

static IRBuilder::BranchBlocks branchOnNotStepped(IterationExpansion& expansion, ValueID status)
{
    ValueID wasNotStepped = appendStatusTest(expansion, status, GeneratorStepStatus::NotStepped);
    return expansion.builder().emitBranch(wasNotStepped, BlockFrequency::Rare, BlockFrequency::Normal);
}

static void finishGeneratorStep(IterationExpansion& expansion, ValueID done, ValueID value, bool readsValueWhenDone)
{
    IRBuilder& builder = expansion.builder();
    if (readsValueWhenDone) {
        expansion.finishStep(done, value);
        return;
    }

    IRBuilder::BranchBlocks completed = builder.emitBranch(done, BlockFrequency::Normal, BlockFrequency::Normal);
    expansion.finishStepAsDone();
    builder.moveToEndOf(completed.notTaken);
    expansion.finishStep(builder.appendBoolean(false), value);
}

static void emitReturnStep(IterationExpansion& expansion, const ArmState& state, IteratorCompletion completion)
{
    IRBuilder& builder = expansion.builder();
    ValueID step = appendStepGenerator(expansion, state.subject, ResumeMode::Return, builder.appendUndefined(), checkForLookup(state));
    ValueID status = appendStepResult(expansion, step, StepGeneratorResult::Status);

    IRBuilder::BranchBlocks blocks = branchOnNotStepped(expansion, status);
    ArmState protocol = protocolStateOf(expansion, state);
    emitProtocolClose(expansion, protocol, IteratorKind::Sync, completion);

    builder.moveToEndOf(blocks.notTaken);
    expansion.finishClose();
}

void emitGeneratorStep(IterationExpansion& expansion, ArmState& state, const StepRequest& request)
{
    IRBuilder& builder = expansion.builder();
    ValueID sent = request.sent ? request.sent : builder.appendUndefined();
    ValueID step = appendStepGenerator(expansion, state.subject, request.mode, sent, checkForMode(state, request.mode));
    ValueID value = appendStepResult(expansion, step, StepGeneratorResult::Value);
    ValueID status = appendStepResult(expansion, step, StepGeneratorResult::Status);

    IRBuilder::BranchBlocks blocks = branchOnNotStepped(expansion, status);
    ArmState protocol = protocolStateOf(expansion, state);
    emitProtocolStep(expansion, protocol, request);

    builder.moveToEndOf(blocks.notTaken);
    finishGeneratorStep(expansion, appendStatusTest(expansion, status, GeneratorStepStatus::Completed), value, request.readsValueWhenDone);
}

void emitAsyncGeneratorStep(IterationExpansion& expansion, ArmState& state, const StepRequest& request)
{
    IRBuilder& builder = expansion.builder();
    if (request.mode != ResumeMode::Next) {
        ArmState protocol = protocolStateOf(expansion, state);
        emitProtocolStep(expansion, protocol, request);
        return;
    }

    ValueID sent = request.sent ? request.sent : builder.appendUndefined();
    ValueID step = builder.append(Opcode::StepAsyncGenerator, { state.subject, sent });
    ValueID value = appendStepResult(expansion, step, StepGeneratorResult::Value);
    ValueID status = appendStepResult(expansion, step, StepGeneratorResult::Status);

    ValueID isAwaited = expansion.appendIsEqualToInteger(status, static_cast<int64_t>(AsyncGeneratorStepStatus::Awaited));
    IRBuilder::BranchBlocks blocks = builder.emitBranch(isAwaited, BlockFrequency::Normal, BlockFrequency::Normal);
    emitIteratorResultReads(expansion, appendAwait(builder, value), request.readsValueWhenDone);

    builder.moveToEndOf(blocks.notTaken);
    ValueID done = expansion.appendIsEqualToInteger(status, static_cast<int64_t>(AsyncGeneratorStepStatus::Completed));
    finishGeneratorStep(expansion, done, value, request.readsValueWhenDone);
}

void emitGeneratorClose(IterationExpansion& expansion, ArmState& state, IteratorCompletion completion)
{
    switch (completion) {
    case IteratorCompletion::Normal:
        emitReturnStep(expansion, state, completion);
        return;
    case IteratorCompletion::Throw:
        break;
    }

    IRBuilder& builder = expansion.builder();
    std::optional<Target> outer = builder.handler();
    BlockID dropped = builder.addHandlerBlock(BlockRole::CatchHandler, ValueID());
    builder.setHandler(Target { dropped, { }, { } });
    BlockID attempt = builder.addBlock(BlockFrequency::Rare);
    builder.setHandler(outer);
    builder.emitJump(attempt);

    builder.moveToEndOf(attempt);
    emitReturnStep(expansion, state, completion);

    builder.moveToEndOf(dropped);
    builder.append(Opcode::CatchValue, { });
    expansion.finishClose();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
