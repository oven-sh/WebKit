/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveBuiltinsInternal.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTTypeTestSite.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <optional>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static constexpr PristineAspectSet aspectsOfIndexedRead { PristineAspects::Storage, PristineAspects::Names };
static constexpr OpcodeSet listCallOpcodes { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };

static_assert(OperandIndex::CallList::list == OperandIndex::ConstructList::list);
static_assert(OperandIndex::CallList::list == OperandIndex::SuperConstructList::list);

static Vector<ValueID, 2> collectCastChain(const Function& function, ValueID listOperand)
{
    Vector<ValueID, 2> chain;
    for (ValueID id : CastChain(function, listOperand))
        chain.append(id);
    return chain;
}

static bool canMoveToCall(FunctionAnalyses& analyses, const Function& function, const ValueSite& call, ValueID list, ValueID spread)
{
    const UseLists& useLists = analyses.useLists();
    auto precedesCallInBlock = [&](ValueID id) {
        ValueLocation location = useLists.locationOf(id);
        return location.placement == ValuePlacement::InBlock && location.block == call.block && location.index < call.index;
    };
    if (!precedesCallInBlock(list) || !precedesCallInBlock(spread))
        return false;

    auto hasNoEffects = [&](ValueID id) {
        return analyses.hasNoEffects(id);
    };
    Vector<ValueID, 2> chain = collectCastChain(function, function.at(call.value).listOperand());
    RELEASE_ASSERT(chain.last() == list);
    if (!std::ranges::all_of(chain.span().first(chain.size() - 1), hasNoEffects))
        return false;

    // The list is empty and nothing sees it before the spread, so only the spread has to commute with what follows it.
    uint32_t firstIndexAfterSpread = useLists.locationOf(spread).index + 1;
    return std::ranges::all_of(function.at(call.block).values.subspan(firstIndexAfterSpread, call.index - firstIndexAfterSpread), hasNoEffects);
}

static bool canPlaceTests(FunctionPassContext& context, std::span<const UseRequirement> requirements, BlockID block)
{
    FunctionAnalyses& analyses = context.analyses;
    if (analyses.blockFrequencies().classOf(block) == BlockFrequency::Rare)
        return false;

    const BuiltinTable& table = context.program.builtins();
    bool hasImpossibleTest = std::ranges::any_of(requirements, [&](const UseRequirement& requirement) {
        return requirement.pristine && pristineTestOf(table, requirement.pristine.builtinClass) == PristineTest::NeverPasses;
    });
    if (hasImpossibleTest)
        return false;

    Vector<Value, 4> tests = createTestsOf(requirements);
    return std::ranges::none_of(tests, [&](const Value& test) {
        return analyses.findEarlierTest(test, block) == EarlierTest::Failed;
    });
}

// A CallBuiltin is no tail call. A list call that reads another array is the same call, so it stays the tail call it was.
static BuiltinUsePlan planBuiltinCallee(const BuiltinUseInputs& inputs, const ValueSite& site, ValueID array, ProgramPoint point)
{
    if (inputs.function.at(site.value).opcode != Opcode::CallList)
        return { };
    if (tailCallRequirementOf(inputs.function, site.block, site.value) == TailCallRequirement::Required)
        return { };
    BuiltinUsePlan plan = planListCallOfBuiltin(inputs, site.value, array, point);
    RELEASE_ASSERT(!plan || (plan.form == BuiltinCallForm::List && plan.operands.arguments.size() == 1 && plan.operands.arguments[0] == array));
    return plan;
}

static std::optional<ListCallSite> planCallOfSpreadArray(FunctionPassContext& context, const BuiltinUseInputs& inputs, const ValueSite& site)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    ValueID spread = findOnlySpreadIntoListOf(inputs, site.value);
    if (!spread)
        return std::nullopt;

    ValueID iterable = function.at(spread).iterableOperand();
    ValueID list = analyses.sameObjectThroughCasts(function.at(site.value).listOperand());
    if (!canMoveToCall(analyses, function, site, list, spread))
        return std::nullopt;

    ValueLocation spreadLocation = analyses.useLists().locationOf(spread);
    ProgramPoint pointBeforeRemovedSpread { spreadLocation.block, spreadLocation.index };
    ListCallSite listCall { site, list, spread, iterable, { }, planBuiltinCallee(inputs, site, iterable, pointBeforeRemovedSpread) };
    if (listCall.plan) {
        listCall.requirements = listCall.plan.requirements;
        return listCall;
    }

    std::optional<UseRequirement> requirement = requirementOfArgumentArray(inputs, iterable, pointBeforeRemovedSpread);
    if (!requirement)
        return std::nullopt;
    if (requirement->requiredType || requirement->pristine)
        listCall.requirements.append(*requirement);
    return listCall;
}

static std::optional<ListCallSite> planCallOfWholeList(FunctionPassContext& context, const BuiltinUseInputs& inputs, const ValueSite& site)
{
    ValueID list = context.function.at(site.value).listOperand();
    BuiltinUsePlan plan = planBuiltinCallee(inputs, site, list, site.point());
    if (!plan)
        return std::nullopt;
    Vector<UseRequirement, 2> requirements = plan.requirements;
    return ListCallSite { site, { }, { }, list, WTF::move(requirements), WTF::move(plan) };
}

Vector<ListCallSite> planListCalls(FunctionPassContext& context)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    BuiltinUseInputs inputs { context.program, function, context.types, context.inputs.flow, analyses.effectContext(), &analyses, { } };

    Vector<ListCallSite> listCalls;
    static_assert(listCallOpcodes == callsWithList);
    for (const ValueSite& site : context.sitesIn(SiteList::CallsWithList)) {
        std::optional<ListCallSite> listCall = planCallOfSpreadArray(context, inputs, site);
        if (!listCall)
            listCall = planCallOfWholeList(context, inputs, site);
        if (!listCall)
            continue;
        if (!listCall->requirements.isEmpty() && !canPlaceTests(context, listCall->requirements.span(), site.block))
            continue;

        listCalls.append(WTF::move(*listCall));
    }
    return listCalls;
}

static void retireList(Function& function, const ListCallSite& listCall, ValueID listOperand)
{
    Vector<ValueID, 2> chain = collectCastChain(function, listOperand);
    RELEASE_ASSERT(chain.last() == listCall.list);
    becomeUndefined(function, listCall.spread);
    for (ValueID id : chain)
        becomeUndefined(function, id);
}

static void rewriteProvenListCall(FunctionPassContext& context, const ListCallSite& listCall)
{
    const Program& program = context.program;
    Function& function = context.function;
    const BuiltinUsePlan& plan = listCall.plan;
    ValueID id = listCall.site.value;

    ReplacedEffects replaced;
    replaced.add({ program, context.inputs.effects }, function, function.at(id));
    if (listCall.spread) {
        for (ValueID replacedValue : { listCall.list, listCall.spread })
            replaced.add({ program, context.inputs.effects }, function, function.at(replacedValue));
        retireList(function, listCall, function.at(id).listOperand());
    }

    Value& call = function.edit(id);
    if (!plan) {
        call.setListOperand(listCall.array);
        checkRewrite(context, listCall.site, replaced);
        return;
    }

    bool isPristineByType = pristineAspectsProvenBy(program.typeOf(function, listCall.array), BuiltinClasses::Array).containsAll(aspectsOfIndexedRead);
    bool isDemotedTailCall = call.isDemotedTailCall();
    becomeBuiltinCall(call, plan.call.overload, BuiltinCallForm::List, plan.operands, plan.stamps);
    call.setNeedsPristineReceiver(!isPristineByType);
    call.setIsDemotedTailCall(isDemotedTailCall);
    call.type = resultTypeOf(program, function, call, context.types);
    call.setExpected(expectedTypeOf(program, function, call, context.types));
    checkRewrite(context, listCall.site, replaced);
}

static void expandTestedListCall(SiteExpansion& expansion, const ListCallSite& listCall)
{
    IRBuilder& builder = expansion.builder();
    Function& function = expansion.function();
    const BuiltinUsePlan& plan = listCall.plan;
    std::optional<Value> listCopy;
    std::optional<Value> spreadCopy;
    if (listCall.spread) {
        listCopy = function.at(listCall.list);
        spreadCopy = function.at(listCall.spread);
    }

    expansion.begin(listCall.site);
    if (listCall.spread) {
        expansion.addReplaced(*listCopy);
        expansion.addReplaced(*spreadCopy);
        retireList(function, listCall, expansion.original().listOperand());
    }

    Origin originOfCall = builder.origin();
    auto appendGenericCall = [&] {
        if (!listCall.spread)
            return expansion.appendOriginal();

        builder.setOrigin(listCopy->origin());
        ValueID newList = builder.append(Value(*listCopy));
        Value newSpread = *spreadCopy;
        newSpread.setArrayOperand(newList);
        builder.setOrigin(spreadCopy->origin());
        builder.append(WTF::move(newSpread));
        builder.setOrigin(originOfCall);
        return expansion.appendOriginal({ OperandIndex::CallList::list, newList, { }, { } });
    };

    ValueID iterable = listCall.array;
    for (Value& test : createTestsOf(listCall.requirements.span())) {
        RELEASE_ASSERT(test.operand(testedOperandIndex) == listCall.array);
        test.setOperand(testedOperandIndex, iterable);
        bool narrowsIterable = provesForever(provesOf(test.opcode));

        ValueID condition = builder.append(WTF::move(test));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(condition, testedOperandIndex, expansion.frequency(), BlockFrequency::Unlikely);
        builder.moveToEndOf(branch.notTaken);
        expansion.finishArm(appendGenericCall());
        builder.moveToEndOf(branch.taken);
        if (narrowsIterable)
            iterable = branch.provenOnTaken;
    }

    if (plan) {
        Value call = makeBuiltinCall(plan.call.overload, BuiltinCallForm::List, { { }, { iterable } }, plan.stamps);
        call.setNeedsPristineReceiver(true);
        call.setIsDemotedTailCall(expansion.isTailCall());
        expansion.finishArm(expansion.append(WTF::move(call)));
    } else
        expansion.finishArm(expansion.appendOriginal({ OperandIndex::CallList::list, iterable, { }, { } }));
    expansion.end();
}

void emitListCalls(FunctionPassContext& context, std::span<const ListCallSite> listCalls)
{
    if (listCalls.empty())
        return;

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    SiteExpansion expansion(context, builder);
    for (const ListCallSite& listCall : listCalls | std::views::reverse) {
        if (listCall.plan && tailCallRequirementOf(context.function, listCall.site.block, listCall.site.value))
            demoteTailCall(context, builder, listCall.site.block);

        if (listCall.requirements.isEmpty())
            rewriteProvenListCall(context, listCall);
        else
            expandTestedListCall(expansion, listCall);
    }
    context.analyses.didChangeValues();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
