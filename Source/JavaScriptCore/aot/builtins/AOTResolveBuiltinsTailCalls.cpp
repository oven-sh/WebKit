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
#include "AOTVector.h"
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
    case BuiltinUseKind::Undefined:
    case BuiltinUseKind::HostValue:
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

Vector<CalledOperandTest> planTestsOfCalledOperands(FunctionPassContext& context)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const Type& anyFunction = program.at(TypeTable::function);

    Vector<CalledOperandTest> tests;
    context.forEachSiteIn(SiteList::Calls, [&](const ValueSite& site) {
        const Value& call = function.at(site.value);
        if (tailCallRequirementOf(function, site.block, site.value) != TailCallRequirement::Required)
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

Vector<ForwardedTailCall> planForwardedTailCalls(FunctionPassContext& context)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    BuiltinUseInputs inputs { context.program, function, context.types, context.inputs.flow, analyses.effectContext(), &analyses, { } };

    Vector<ForwardedTailCall> calls;
    context.forEachSiteIn(SiteList::Calls, [&](const ValueSite& site) {
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
        becomeForwardedCall(function.edit(call.site.value), plan);
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    context.build.statistics().count(StatisticID::BuiltinCallsResolved, calls.size());
}

static constexpr size_t maxArgumentsPassedOnByCount = 4;

static bool isListOfApply(const Program& program, const Function& function, const Use& use)
{
    const Value& call = function.at(use.user);
    if (call.opcode != Opcode::Call || program.typeOf(function, call.calleeOperand()).singleBuiltin() != Builtins::FunctionPrototypeApply)
        return false;
    constexpr uint32_t indexOfListAmongArguments = 1;
    uint32_t numberOfArguments = call.argumentsOperands().size();
    return numberOfArguments > indexOfListAmongArguments && use.operandIndex == call.numberOfOperands() - numberOfArguments + indexOfListAmongArguments;
}

static BitVector blocksThatLeadTo(const CFG& cfg, BlockID block)
{
    BitVector leading(cfg.numNodes());
    Vector<BlockID, 16> worklist { block };
    while (!worklist.isEmpty()) {
        for (BlockID predecessor : cfg.predecessors(worklist.takeLast())) {
            if (!leading.set(predecessor.index()))
                worklist.append(predecessor);
        }
    }
    return leading;
}

static bool holdsWhatWasPassed(FunctionPassContext& context, ValueID arguments, const ValueSite& call)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    BitVector leadsToCall = blocksThatLeadTo(context.analyses.cfg(), call.block);
    return std::ranges::all_of(useLists.usesOf(arguments), [&](const Use& use) {
        if (use.isEdgeUse()) {
            BlockID target = targetOf(function, use.edge()).block;
            return target != call.block && !leadsToCall.get(target.index());
        }
        bool comesBeforeCall = leadsToCall.get(use.block.index()) || (use.block == call.block && use.kind == Use::Kind::Operand && useLists.pointOf(use).index <= call.index);
        if (!comesBeforeCall)
            return true;
        if (use.kind != Use::Kind::Operand)
            return false;
        Opcode user = function.at(use.user).opcode;
        if (user == Opcode::PropGet || user == Opcode::ElemGet)
            return !use.operandIndex;
        return isListOfApply(context.program, function, use);
    });
}

Vector<CallOfOwnArguments> planCallsOfOwnArguments(FunctionPassContext& context)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    BuiltinUseInputs inputs { context.program, function, context.types, context.inputs.flow, analyses.effectContext(), &analyses, { } };

    Vector<CallOfOwnArguments> calls;
    context.forEachSiteIn(SiteList::Calls, [&](const ValueSite& site) {
        const Value& value = function.at(site.value);
        if (function.at(site.block).frequency.predicted() != BlockFrequency::Normal)
            return;
        if (context.program.typeOf(function, value.calleeOperand()).singleBuiltin() != Builtins::FunctionPrototypeApply)
            return;

        BuiltinUsePlan use = planCallOfBuiltin(inputs, site.value, site.point());
        if (!isMatchedCall(use) || !passesOperandsUnchanged(use.call))
            return;
        BuiltinCall call { use.call.overload, BuiltinCallForm::Whole, 0, use.operands.receiver, use.operands.arguments.span() };
        std::optional<ForwardedCallPlan> plan = planForwardedCall(context.program, function, call);
        if (!plan || !plan->list || function.at(plan->list).opcode != Opcode::CreateUnmappedArguments)
            return;
        size_t numberOfParameters = function.at(plan->list).parametersOperands().size();
        if (!numberOfParameters || numberOfParameters > maxArgumentsPassedOnByCount)
            return;
        if (plan->callee == plan->list || plan->thisValue == plan->list || !holdsWhatWasPassed(context, plan->list, site))
            return;
        calls.append(CallOfOwnArguments { site, *plan });
    });
    return calls;
}

static bool isTakenByApplyAlone(FunctionPassContext& context, ValueID arguments)
{
    if (context.analyses.useLists().useCount(context.function.at(arguments).restOperand()) != 1)
        return false;
    return std::ranges::all_of(context.analyses.useLists().usesOf(arguments), [&](const Use& use) {
        return use.kind == Use::Kind::Operand && isListOfApply(context.program, context.function, use);
    });
}

static void passOnOwnArgumentsByCount(SiteExpansion& expansion, const CallOfOwnArguments& call, bool isTakenByApplyAlone)
{
    IRBuilder& builder = expansion.builder();
    Function& function = expansion.function();
    ValueID count = function.at(call.plan.list).countOperand();
    ValueID rest = function.at(call.plan.list).restOperand();
    Vector<ValueID, maxArgumentsPassedOnByCount> parameters(function.at(call.plan.list).parametersOperands());

    expansion.begin(call.site);
    Vector<BlockID, maxArgumentsPassedOnByCount + 1> arms;
    Vector<std::pair<int64_t, Target>> cases;
    for (size_t numberPassed = 0; numberPassed <= parameters.size(); ++numberPassed) {
        arms.append(builder.addBlock(expansion.frequency()));
        cases.append({ static_cast<int64_t>(numberPassed), Target { arms.last(), { }, { } } });
    }
    BlockID hasMoreArguments = builder.addBlock(BlockFrequency::Unlikely);
    builder.emitSwitch(count, WTF::move(cases), Target { hasMoreArguments, { }, { } });

    ForwardedCallPlan planWithoutList = call.plan;
    planWithoutList.list = { };
    for (size_t numberPassed = 0; numberPassed <= parameters.size(); ++numberPassed) {
        builder.moveToEndOf(arms[numberPassed]);
        ValueID passedOn = expansion.appendOriginal();
        becomeForwardedCall(function.edit(passedOn), planWithoutList);
        function.edit(passedOn, ValueEdit::Definition).setArgumentsOperands(parameters.span().first(numberPassed));
        expansion.finishArm(passedOn);
    }

    builder.moveToEndOf(hasMoreArguments);
    if (isTakenByApplyAlone) {
        ValueID test = builder.append(makePristineTest(rest, BuiltinClasses::Array, { PristineAspects::Storage, PristineAspects::Names }));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, 0, BlockFrequency::Unlikely, BlockFrequency::Rare);
        builder.moveToEndOf(branch.taken);
        Value listOfParameters = IRBuilder::make(Opcode::NewArray);
        listOfParameters.setContents(TypeTable::anything);
        listOfParameters.setElementsOperands(parameters.span());
        ValueID list = builder.append(WTF::move(listOfParameters));
        Value appendElements = IRBuilder::make(Opcode::AppendElements, { list, rest });
        appendElements.setGuardOperand(branch.provenOnTaken);
        builder.append(WTF::move(appendElements));
        ValueID passedOn = expansion.appendOriginal();
        Value& apply = function.edit(passedOn);
        for (uint32_t index = 0; index < apply.numberOfOperands(); ++index) {
            if (apply.operand(index) == call.plan.list)
                apply.setOperand(index, list);
        }
        expansion.finishArm(passedOn);
        builder.moveToEndOf(branch.notTaken);
    }
    expansion.finishArm(expansion.appendOriginal());
    expansion.end();
}

void passOnOwnArgumentsByCount(FunctionPassContext& context, std::span<const CallOfOwnArguments> calls)
{
    if (calls.empty())
        return;

    Vector<bool, 4> areTakenByApplyAlone(calls.size(), [&](size_t index) {
        return isTakenByApplyAlone(context, calls[index].plan.list);
    });
    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    SiteExpansion expansion(context, builder);
    for (size_t index = calls.size(); index--;) {
        passOnOwnArgumentsByCount(expansion, calls[index], areTakenByApplyAlone[index]);
        context.analyses.didChangeValues();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
