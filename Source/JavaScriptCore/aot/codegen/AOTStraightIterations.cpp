/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfIterations()
{
    return { Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::IteratorClose };
}

static StraightCallOptions optionsOfIterationInStraightForm(const BodyValue& value)
{
    Origin origin = value.origin();
    origin.isTailCall = false;
    return { .origin = origin };
}

static void generateIteratorOpen(StraightEmitter& emitter, const BodyValue& value)
{
    if (value.iteratorKind() != IteratorKind::Sync) {
        emitter.fail(value);
        return;
    }
    emitter.callOperation(value, value.isDelegating() ? RuntimeOperation::OpenDelegation : RuntimeOperation::OpenIteration, { value.iterableOperand() }, optionsOfIterationInStraightForm(value));
    emitter.define(value);
}

static bool isDelegationState(const BodyIndex& index, ValueID state)
{
    return !index.isParameter(state) && index.opcodeOf(state) == Opcode::IteratorOpen && index.value(state).as<Opcode::IteratorOpen>().isDelegating();
}

static void generateIteratorResume(StraightEmitter& emitter, const BodyValue& value)
{
    if (value.iteratorKind() == IteratorKind::Sync && isDelegationState(emitter.index(), value.iteratorOperand())) {
        StraightArgument sent = value.sentOperand() ? StraightArgument(value.sentOperand()) : StraightArgument::boxed(jsUndefined());
        emitter.callOperation(value, RuntimeOperation::StepDelegation, { value.iteratorOperand(), sent, StraightArgument::word(static_cast<uint32_t>(value.resumeMode())) }, optionsOfIterationInStraightForm(value));
        emitter.defineResult(value, static_cast<uint32_t>(IteratorResumeResult::Done), emitter.resultOfCall());
        emitter.defineResult(value, static_cast<uint32_t>(IteratorResumeResult::Value), GPRInfo::returnValueGPR2);
        return;
    }
    if (value.iteratorKind() != IteratorKind::Sync || value.resumeMode() != ResumeMode::Next || value.sentOperand() || value.readsValueWhenDone() || !value.heldOperands().empty()) {
        emitter.fail(value);
        return;
    }
    emitter.callOperation(value, RuntimeOperation::StepIteration, { value.iteratorOperand() }, optionsOfIterationInStraightForm(value));
    emitter.defineResult(value, static_cast<uint32_t>(IteratorResumeResult::Done), emitter.resultOfCall());
    emitter.defineResult(value, static_cast<uint32_t>(IteratorResumeResult::Value), GPRInfo::returnValueGPR2);
}

static void generateIteratorClose(StraightEmitter& emitter, const BodyValue& value)
{
    if (value.iteratorKind() != IteratorKind::Sync || !value.heldOperands().empty()) {
        emitter.fail(value);
        return;
    }
    RuntimeOperation operation = value.completion() == IteratorCompletion::Throw ? RuntimeOperation::CloseIterationAfterThrow : RuntimeOperation::CloseIteration;
    emitter.callOperation(value, operation, { value.iteratorOperand() }, optionsOfIterationInStraightForm(value));
}

void StraightEmitter::generateIteration(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::IteratorOpen:
        generateIteratorOpen(*this, value);
        return;
    case Opcode::IteratorResume:
        generateIteratorResume(*this, value);
        return;
    case Opcode::IteratorClose:
        generateIteratorClose(*this, value);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
