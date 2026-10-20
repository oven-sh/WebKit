/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveBuiltinsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTTypeTestSite.h"
#include <algorithm>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static bool isMatchedCall(const BuiltinUsePlan& plan)
{
    if (!plan.isProven() || plan.form != BuiltinCallForm::Whole)
        return false;
    switch (plan.kind) {
    case BuiltinUseKind::Call:
    case BuiltinUseKind::LibraryCall:
        return true;
    case BuiltinUseKind::None:
    case BuiltinUseKind::Constant:
    case BuiltinUseKind::UnmatchedCall:
    case BuiltinUseKind::NewArray:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool passesOperandsUnchanged(const BuiltinCallPlan& plan)
{
    return std::ranges::all_of(plan.steps, [](const OperandStep& step) {
        switch (step.kind) {
        case OperandStepKind::Pass:
        case OperandStepKind::PassUndefined:
            return true;
        case OperandStepKind::Convert:
        case OperandStepKind::RequireObjectCoercible:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static std::optional<uint32_t> indexOfCalledOperand(const Program& program, const Value& call, BuiltinOverloadID id)
{
    std::optional<BuiltinOperand> called = operandCalledBy(program.builtins().at(id));
    if (!called || !parameterTypeOfOverload(program, id, *called).isSubsetOf(program.at(TypeTable::function), program))
        return std::nullopt;
    if (called->isReceiver())
        return OperandIndex::Call::thisValue;
    if (called->argumentIndex() >= call.argumentsOperands().size())
        return std::nullopt;
    return call.numberOfOperands() - call.argumentsOperands().size() + called->argumentIndex();
}

// `call` and Reflect.apply throw an error of their own for a callee that is no function, so they pass the call on for a proven function only.
// The planner of tests leaves a tail call alone, and a call that has to stay one is never demoted: the test stands here, and both of its arms end in a tail call.
Vector<CalledOperandTest> planTestsOfCalledOperands(FunctionPassContext& context)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const Type& anyFunction = program.at(TypeTable::function);

    Vector<CalledOperandTest> tests;
    forEachValueSite(context.analyses, function, [&](const ValueSite& site) {
        const Value& call = function.at(site.value);
        if (call.opcode != Opcode::Call || tailCallRequirementOf(function, site.block, site.value) != TailCallRequirement::Required)
            return;
        BuiltinID callee = program.typeOf(function, call.calleeOperand()).singleBuiltin();
        if (!callee)
            return;

        for (BuiltinOverloadID overload : program.builtins().overloadIDsOf(callee)) {
            std::optional<uint32_t> operandIndex = indexOfCalledOperand(program, call, overload);
            ValueID operand = operandIndex ? call.operand(*operandIndex) : ValueID();
            if (!operand)
                continue;
            const Type& type = program.typeOf(function, operand);
            if (type.isSubsetOf(anyFunction, program) || !type.overlaps(anyFunction, program))
                return;
            if (context.analyses.findEarlierTest(makeTypeTest(operand, TypeTable::function), site.block) != EarlierTest::Failed)
                tests.append(CalledOperandTest { site, *operandIndex });
            return;
        }
    });
    return tests;
}

void emitTestsOfCalledOperands(FunctionPassContext& context, std::span<const CalledOperandTest> tests)
{
    if (tests.empty())
        return;

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    SiteExpansion expansion(context, builder);
    for (const CalledOperandTest& test : tests | std::views::reverse) {
        expansion.begin(test.site);
        ValueID isFunction = builder.append(makeTypeTest(expansion.original().operand(test.operandIndex), TypeTable::function));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isFunction, testedOperandIndex, expansion.frequency(), BlockFrequency::Unlikely);
        builder.moveToEndOf(branch.taken);
        expansion.finishArm(expansion.appendOriginal({ test.operandIndex, branch.provenOnTaken, { }, { }, true }));
        builder.moveToEndOf(branch.notTaken);
        expansion.finishArm(expansion.appendOriginal({ test.operandIndex, branch.provenOnNotTaken, { }, { }, true }));
        expansion.end();
    }
}

// A CallBuiltin is no tail call, so a call that has to stay one would go on calling the function object of the built-in.
// The body of Function.prototype.apply is such a call of apply itself, and the call that apply and call pass on is a tail call by the language.
Vector<ForwardedTailCall> planForwardedTailCalls(FunctionPassContext& context)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    BuiltinUseInputs inputs { context.program, function, context.types, context.inputs.flow, analyses.effectContext(), &analyses, { } };

    Vector<ForwardedTailCall> calls;
    forEachValueSite(analyses, function, [&](const ValueSite& site) {
        if (function.at(site.value).opcode != Opcode::Call)
            return;
        if (tailCallRequirementOf(function, site.block, site.value) != TailCallRequirement::Required)
            return;

        BuiltinUsePlan use = planCallOfBuiltin(inputs, site.value, site.point());
        if (!isMatchedCall(use) || !passesOperandsUnchanged(use.call))
            return;
        BuiltinCall call { use.call.overload, BuiltinCallForm::Whole, 0, use.operands.receiver, use.operands.arguments.span() };
        if (std::optional<ForwardedCallPlan> plan = planForwardedCall(context.program, function, call))
            calls.append(ForwardedTailCall { site, *plan });
    });
    return calls;
}

void rewriteForwardedTailCalls(FunctionPassContext& context, std::span<const ForwardedTailCall> calls)
{
    if (calls.empty())
        return;

    Function& function = context.function;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (const ForwardedTailCall& call : calls) {
        ForwardedCallPlan plan = call.plan;
        if (plan.list && !isAlwaysArray(context.program.typeOf(function, plan.list))) {
            builder.setOrigin(function.at(call.site.value).origin());
            plan.list = builder.insertBefore(call.site.point(), IRBuilder::make(Opcode::NewArgumentList, { plan.callee, plan.list }));
        }
        becomeForwardedCall(function.at(call.site.value), plan);
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    context.build.statistics().count(StatisticID::BuiltinCallsResolved, calls.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
