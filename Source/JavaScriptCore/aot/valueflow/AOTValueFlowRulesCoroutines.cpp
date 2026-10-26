/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesProtocolsInternal.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

void runCoroutine(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowProgramNodes& nodes = context.nodes();
    FunctionID function = statement.function;
    ClimbingSet coroutine = only(context, universe.coroutineResultOf(function));
    ValueFlowNodeID returned = nodes.result(function, 0);
    if (!context.program().at(function).has(FunctionFlags::IsGenerator)) {
        resolvePromises(context, coroutine, { returned, ValueSets::nothing, ValueSets::anything });
        rejectPromises(context, coroutine, { nodes.thrown(function), ValueSets::nothing, ValueSets::anything });
        return;
    }
    ValueFlowNodeID yielded = nodes.yielded(function);
    ValueFlowNodeID sent = nodes.sent(function);

    context.connect(yielded, context.written(coroutine, steppedValuesName));
    context.connect(returned, context.written(coroutine, returnedValueName));
    if (context.program().at(function).has(FunctionFlags::IsAsync)) {
        context.connect(context.lookup(context.valueOf(yielded), awaitedValueName), context.written(coroutine, steppedValuesName));
        context.connect(context.lookup(context.valueOf(returned), awaitedValueName), context.written(coroutine, returnedValueName));
    }

    context.add(sent, universe.undefinedValue());
    connectContentsOfSlot(context, coroutine, sentValuesName, sent);
    if (context.holdsAnyOf(coroutine, ValueFlowProgramNodes::escaped()))
        context.add(sent, unknownValue(context, ValueFlowUnknownSource::ParameterOfEscapedFunction));

    context.connect(nodes.thrown(function), ValueFlowProgramNodes::escaped());

    ValueFlowNodeID prototype = context.lookup(only(context, universe.closureOf(function)), nameOf(context, BuiltinNames::prototype));
    context.connect(context.masked(prototype, universe.objects()), context.written(coroutine, prototypeLinkName));
}

static BuiltinID methodOfGenerators(ResumeMode mode)
{
    switch (mode) {
    case ResumeMode::Next:
        return Builtins::GeneratorPrototypeNext;
    case ResumeMode::Throw:
        return Builtins::GeneratorPrototypeThrow;
    case ResumeMode::Return:
        return Builtins::GeneratorPrototypeReturn;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void runGeneratorOperation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    ResumeMode mode = context.program().at(statement.function).at(statement.site).resumeMode();
    ClimbingSet generators = context.restrict(context.valueOf(operands[0]), universe.objects());
    std::span<const ValueFlowOperand> sent = operands.subspan(1, 1);

    forEachNodeOfSlotWrittenThrough(context, generators, sentValuesName, [&](ValueFlowNodeID held) {
        context.connect(sent[0], held);
    });
    if (statement.thrown) {
        context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
        if (mode == ResumeMode::Throw)
            context.connect(sent[0], statement.thrown);
    }
    if (!statement.result)
        return;
    if (statement.opcode == Opcode::StepGenerator)
        connectOutcomeOfStep(context, generators, mode, sent, statement.result);
    else
        context.add(statement.result, makeIteratorResult(context, universe.resultOf(methodOfGenerators(mode), 0), generators, mode, sent));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
