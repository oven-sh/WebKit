/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowEdges.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static FlowSlotAccess createPromiseSlotStore(ValueID origin, FlowLocalID promise, BuiltinSlotID slot, FlowLocalID stored)
{
    return { .kind = FlowAccessKind::Store, .origin = origin, .receiver = promise, .slot = slot, .value = stored };
}

static void addPromiseRejection(FlowSummaryAssembler& assembler, ValueID origin, FlowLocalID promise, FlowLocalID reason)
{
    assembler.add(createPromiseSlotStore(origin, promise, BuiltinSlots::PromiseReason, reason));
    // The host reports a rejection that no handler takes.
    assembler.copy(reason, assembler.external(FlowExternalNode::nativeCode()));
}

void FlowSummaryBuilder::addIteratorOpen(ValueID id, const Value& value)
{
    m_assembler.add(FlowIteration {
        .step = FlowIterationStep::Open,
        .iteratorKind = value.iteratorKind(),
        .origin = id,
        .iterable = localOf(value.iterableOperand()),
        .state = localOf(id),
        .thrown = thrownTarget(),
    });
}

void FlowSummaryBuilder::addIteratorResume(ValueID id, const Value& value)
{
    FlowLocalID steppedValue = resultLocalsOf(id, numberOfEnumerators<IteratorResumeResult>)[static_cast<uint32_t>(IteratorResumeResult::Value)];

    m_assembler.add(FlowIteration {
        .step = FlowIterationStep::Resume,
        .iteratorKind = value.iteratorKind(),
        .resumeMode = value.resumeMode(),
        .origin = id,
        .state = localOf(value.iteratorOperand()),
        .sent = localOf(value.sentOperand()),
        .value = steppedValue,
        .thrown = thrownTarget(),
        .stepIndex = value.stepIndex(),
    });
}

void FlowSummaryBuilder::addIteratorClose(ValueID id, const Value& value)
{
    m_assembler.add(FlowIteration {
        .step = FlowIterationStep::Close,
        .iteratorKind = value.iteratorKind(),
        .resumeMode = ResumeMode::Return,
        .origin = id,
        .state = localOf(value.iteratorOperand()),
        .thrown = thrownTarget(),
    });
}

void FlowSummaryBuilder::addAwait(ValueID id, const Value& value)
{
    m_assembler.add(FlowAwait { id, localOf(value.valueOperand()), localOf(id), thrownTarget() });
}

void FlowSummaryBuilder::addYield(ValueID id, const Value& value)
{
    const FlowCoroutineSummary& coroutine = m_assembler.coroutine();
    RELEASE_ASSERT(coroutine.yielded && coroutine.sent);

    FlowLocalID sentValue = resultLocalsOf(id, numberOfEnumerators<YieldResult>)[static_cast<uint32_t>(YieldResult::Sent)];
    m_assembler.copy(localOf(value.valueOperand()), coroutine.yielded);
    m_assembler.copy(coroutine.sent, sentValue);
}

void FlowSummaryBuilder::addGeneratorResumption(ValueID id, const Value& value)
{
    FlowLocalID resumedValue;
    switch (value.opcode) {
    case Opcode::ResumeGenerator:
        resumedValue = localOf(id);
        break;
    case Opcode::StepGenerator:
        resumedValue = resultLocalsOf(id, numberOfEnumerators<StepGeneratorResult>)[static_cast<uint32_t>(StepGeneratorResult::Value)];
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    m_assembler.add(FlowIteration {
        .step = FlowIterationStep::Resume,
        .iteratorKind = IteratorKind::Sync,
        .resumeMode = value.resumeMode(),
        .origin = id,
        .state = localOf(value.generatorOperand()),
        .sent = localOf(value.valueOperand()),
        .value = resumedValue,
        .thrown = thrownTarget(),
    });
}

void FlowSummaryBuilder::addArrayIteratorStep(ValueID id, const Value& value)
{
    m_assembler.add(FlowIteration {
        .step = FlowIterationStep::Resume,
        .iteratorKind = IteratorKind::Sync,
        .resumeMode = ResumeMode::Next,
        .origin = id,
        .state = localOf(value.iteratorOperand()),
        .value = resultLocalsOf(id, numberOfEnumerators<StepArrayIteratorResult>)[static_cast<uint32_t>(StepArrayIteratorResult::Value)],
        .thrown = thrownTarget(),
    });
}

void FlowSummaryBuilder::addResolvePromise(ValueID id, const Value& value)
{
    FlowLocalID promise = localOf(value.promiseOperand());
    FlowLocalID resolvedValue = m_assembler.addTemporary();
    FlowLocalID rejectionReason = m_assembler.addTemporary();

    m_assembler.add(FlowAwait { id, localOf(value.valueOperand()), resolvedValue, rejectionReason });
    // A promise that is resolved with itself is rejected with an error of the run time.
    m_assembler.add(FlowOperandUse { .origin = id, .thrown = rejectionReason });
    m_assembler.add(createPromiseSlotStore(id, promise, BuiltinSlots::PromiseValue, resolvedValue));
    addPromiseRejection(m_assembler, id, promise, rejectionReason);
}

void FlowSummaryBuilder::addRejectPromise(ValueID id, const Value& value)
{
    addPromiseRejection(m_assembler, id, localOf(value.promiseOperand()), localOf(value.valueOperand()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
