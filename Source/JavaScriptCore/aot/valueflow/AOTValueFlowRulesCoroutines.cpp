/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesProtocolsInternal.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static void linkGenerator(ValueFlowRuleContext& context, FunctionID function, ClimbingSet generator)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowProgramNodes& nodes = context.nodes();
    ValueFlowNodeID yielded = nodes.yielded(function);
    ValueFlowNodeID returned = nodes.result(function, 0);
    ValueFlowNodeID sent = nodes.sent(function);

    // An async generator awaits what it yields and what it returns.
    bool isAsync = context.program().at(function).has(FunctionFlags::IsAsync);
    context.connect(isAsync ? context.lookup(context.valueOf(yielded), awaitedValueName) : yielded, context.written(generator, steppedValuesName));
    context.connect(isAsync ? context.lookup(context.valueOf(returned), awaitedValueName) : returned, context.written(generator, returnedValueName));

    // A step of a loop hands in undefined, and native code whatever it likes.
    context.add(sent, universe.undefinedValue());
    connectContentsOfSlot(context, generator, sentValuesName, sent);
    if (context.meet(generator, context.valueOf(ValueFlowProgramNodes::escaped())))
        context.add(sent, unknownValue(context));

    // Whoever resumes the body throws the unknown value.
    context.connect(nodes.thrown(function), ValueFlowProgramNodes::escaped());

    ValueFlowNodeID prototype = context.lookup(only(context, universe.closureOf(function)), nameOf(context, BuiltinNames::prototype));
    context.connect(context.masked(prototype, universe.objects()), context.written(generator, prototypeLinkName));
}

// One object stands for the promises or the generators that all calls of the function answer, so what goes between the body and that object is the same for every way in which the function is entered.
void runCoroutine(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowProgramNodes& nodes = context.nodes();
    FunctionID function = statement.function;
    ClimbingSet coroutine = only(context, context.universe().coroutineResultOf(function));
    if (context.program().at(function).has(FunctionFlags::IsGenerator)) {
        linkGenerator(context, function, coroutine);
        return;
    }
    resolvePromises(context, coroutine, context.valueOf(nodes.result(function, 0)));
    rejectPromises(context, coroutine, { nodes.thrown(function), ValueSets::nothing, ValueSets::anything });
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

// The operands of both operations: the generator, then what is handed in. StepGenerator answers the value itself, ResumeGenerator the iterator result that the method of the prototype answers.
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
        context.add(statement.thrown, unknownValue(context));
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
