/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTChooseCallingConventionsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTDerivedFunctions.h"
#include "AOTFlowApply.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTFunctionKinds.h"
#include "AOTIRBuilder.h"
#include "AOTResolveCallsCallees.h"
#include "AOTSplitUnions.h"
#include "AOTThreads.h"
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

using ConvertedGroups = Vector<CallGroupID, 2>;

namespace {

struct NullishCalleeSite {
    ValueSite call;
    ValueID callee;
};

} // anonymous namespace

Vector<GroupEntrypoints> findGroupEntrypoints(const CallGroups& groups, const DerivedFunctions& derivedFunctions)
{
    Vector<GroupEntrypoints> result(groups.numberOfGroups());
    for (unsigned index = 0; index < groups.numberOfGroups(); ++index) {
        for (FunctionID member : groups.at(CallGroupID(index)).members) {
            if (FunctionID entry = derivedFunctions.find(derivedFunctionKey(FunctionRole::GroupEntrypoint, member)))
                result[index].all.append(entry);
        }
    }
    return result;
}

static CallGroupID findGroupToCall(const ConventionInputs& context, const Function& function, const Value& value)
{
    if (value.opcode != Opcode::Call)
        return { };
    CallGroupID group = context.groups.groupOf(value.accessSite());
    if (!group || context.groupEntrypoints[group.index()].all.isEmpty())
        return { };
    // A call of undefined or null throws and enters no member.
    if (context.program().typeOf(function, value.calleeOperand()).isUndefinedOrNull())
        return { };
    return group;
}

static bool hasCallToConvert(const ConventionInputs& context, const Function& function)
{
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (findGroupToCall(context, function, function.at(id)))
                return true;
        }
    }
    return false;
}

static void separateNullishCallee(SiteExpansion& expansion, const NullishCalleeSite& site)
{
    FunctionPassContext& context = expansion.context();
    expansion.begin(site.call);

    Type nullish = Type::fromPrimitives(context.program.typeOf(context.function, site.callee).primitives & Primitive::UndefinedOrNull);
    ValueSplit split { .value = site.callee, .arms = { }, .hasRest = true, .restFrequency = expansion.frequency() };
    split.arms.append(SplitArm { context.types.intern(nullish), BlockFrequency::Rare });

    emitSplit(expansion, split,
        [&](uint32_t, ValueID narrowed) {
            expansion.appendOriginal({ OperandIndex::Call::callee, narrowed, { }, { } });
            expansion.finishArmAsUnreachable();
        },
        [&](ValueID narrowed) {
            expansion.finishArm(expansion.appendOriginal({ OperandIndex::Call::callee, narrowed, { }, { } }));
        });
    expansion.end();
}

static void separateNullishCallees(const ConventionInputs& context, FunctionID id, Function& function, TaskArenas& arenas)
{
    const Program& program = context.program();
    const FlowResults& flow = context.inputs.flow;
    FunctionAnalyses analyses(AnalysisContext { context.build, program, &flow, &context.inputs.effects, id }, function);

    Vector<NullishCalleeSite, 2> sites;
    forEachValueSite(analyses, function, [&](const ValueSite& site) {
        const Value& call = function.at(site.value);
        if (!findGroupToCall(context, function, call))
            return;
        CalleeCandidates candidates = calleeCandidates(program, flow, function, call.calleeOperand(), call.callees(), call.accessSite());
        if (candidates.mayBeNullish)
            sites.append(NullishCalleeSite { site, candidates.value });
    });
    if (sites.isEmpty())
        return;

    FunctionPassContext passContext { program, context.inputs, id, function, analyses, arenas.types, arenas.strings, context.build };
    IRBuilder builder(analyses.context(), function, arenas.types, &analyses);
    SiteExpansion expansion(passContext, builder);
    for (const NullishCalleeSite& site : sites | std::views::reverse)
        separateNullishCallee(expansion, site);
}

static ValueID fitOperand(IRBuilder& builder, ProgramPoint point, ValueID operand, const DeclaredType& declared)
{
    const Program& program = builder.context().program;
    if (program.typeOf(builder.function(), operand).isSubsetOf(program.at(declared.type), program))
        return operand;
    if (builder.context().build.validates())
        builder.insertBefore(point, makeValidateType(operand, declared.type));
    return builder.insertBefore(point, makeAssumeType(operand, declared.type));
}

static void convertCall(IRBuilder& builder, ProgramPoint point, FunctionID declaring)
{
    Function& function = builder.function();
    const Function& entry = builder.context().program.at(declaring);
    ValueID id = function.at(point.block).values[point.index];

    // insertBefore() reallocates function.values, so the call's operands and payload are copied first.
    const Value& call = function.at(id);
    ValueID callee = call.calleeOperand();
    ValueID thisValue = call.thisValueOperand();
    Vector<ValueID, 8> passedArguments(call.argumentsOperands());
    CalleeSetID callees = call.callees();
    AccessSiteID site = call.accessSite();
    bool isDemotedTailCall = call.isDemotedTailCall();
    builder.setOrigin(call.origin());

    ValueID undefined;
    auto passedOrUndefined = [&](ValueID passed) {
        if (passed)
            return passed;
        if (!undefined)
            undefined = builder.insertBefore(point, IRBuilder::make(Opcode::Undefined));
        return undefined;
    };

    ValueID closure = fitOperand(builder, point, callee, entry.captures[0]);
    Vector<ValueID, 8> arguments;
    if (entry.has(FunctionFlags::HasThis))
        arguments.append(fitOperand(builder, point, passedOrUndefined(thisValue), entry.thisType));
    for (unsigned index = 0; index < entry.parameters.size(); ++index) {
        ValueID passed = index < passedArguments.size() ? passedArguments[index] : ValueID();
        arguments.append(fitOperand(builder, point, passedOrUndefined(passed), entry.parameters[index]));
    }
    RELEASE_ASSERT(entry.captures.size() == 1 && entry.numberOfEntryParameters() == arguments.size() + 1);

    Value& groupCall = function.at(id);
    groupCall.become(Opcode::CallGroup);
    groupCall.setFunction(declaring);
    groupCall.setClosureOperand(closure);
    groupCall.setArgumentsOperands(arguments.span());
    groupCall.setCallees(callees);
    groupCall.setAccessSite(site);
    groupCall.setIsDemotedTailCall(isDemotedTailCall);
    groupCall.type = entry.result().type;
    groupCall.setPrediction({ });
}

static bool convertCallsIn(const ConventionInputs& context, FunctionID id, Function& function, TaskArenas& arenas, ConvertedGroups& convertedGroups)
{
    separateNullishCallees(context, id, function, arenas);

    IRBuilder builder(AnalysisContext { context.build, context.program(), &context.inputs.flow, &context.inputs.effects, id }, function, arenas.types);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (uint32_t index = 0; index < function.at(block).values.size(); ++index) {
            CallGroupID group = findGroupToCall(context, function, function.at(function.at(block).values[index]));
            if (!group)
                continue;
            convertCall(builder, ProgramPoint { block, index }, context.groupEntrypoints[group.index()].declaring());
            convertedGroups.append(group);
        }
    }
    builder.applyInsertions();
    return !convertedGroups.isEmpty();
}

static FunctionID memberOf(const Program& program, FunctionID groupEntrypoint)
{
    return program.at(groupEntrypoint).creator.as<CreatorRecord::Entrypoint>().target;
}

static FunctionID findGenericEntrypoint(const ConventionInputs& context, FunctionID member)
{
    return context.inputs.derivedFunctions->find(derivedFunctionKey(FunctionRole::GenericEntrypoint, member));
}

static void removeEntrypoints(const ConventionInputs& context, GroupEntrypoints& entrypoints)
{
    for (FunctionID entry : entrypoints.all) {
        FunctionID member = memberOf(context.program(), entry);
        context.editor.removeBody(entry);
        if (FunctionID generic = findGenericEntrypoint(context, member))
            context.editor.at(generic).creator.as<CreatorRecord::Entrypoint>().groupEntrypoint = FunctionID();
    }
    entrypoints.all.clear();
}

static ClosureCells closureCellsOfMember(const ConventionInputs& context, FunctionID member)
{
    switch (closureFormOf(context.program(), context.inputs.flow, member)) {
    case ClosureForm::Full:
        return ClosureCells::AllFull;
    case ClosureForm::Slim:
        return ClosureCells::AllSlim;
    case ClosureForm::Deferred:
    case ClosureForm::Singleton:
        return ClosureCells::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void recordEntrypoints(const ConventionInputs& context, const GroupEntrypoints& entrypoints, CallingConventionTables& tables)
{
    const Program& program = context.program();
    ClosureCells cells = closureCellsOfMember(context, memberOf(program, entrypoints.declaring()));
    for (FunctionID entry : entrypoints.all) {
        FunctionID member = memberOf(program, entry);
        if (FunctionID generic = findGenericEntrypoint(context, member))
            tables.valueEntries[generic.index()] = entry;
        if (closureCellsOfMember(context, member) != cells)
            cells = ClosureCells::Unknown;
    }
    for (FunctionID entry : entrypoints.all)
        tables.closureCells[entry.index()] = cells;
}

void convertCallsToGroupCalls(const ConventionInputs& context, CallingConventionTables& tables, BitVector& changedFunctions)
{
    if (!context.groups.numberOfGroups())
        return;

    const Program& program = context.program();
    Vector<FunctionID> candidates = collectInParallel<FunctionID>(context.build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
        if (hasCallToConvert(context, program.functions[index]))
            found.append(FunctionID(index));
    });

    Vector<ConvertedGroups> convertedGroups(candidates.size());
    editFunctionsInParallel(context.editor, context.build, candidates.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        if (convertCallsIn(context, id, function, arenas, convertedGroups[index]))
            simplifyEditedFunction(context.runner, program, context.inputs, id, function, arenas);
    });

    uint64_t numberOfGroupCalls = 0;
    for (unsigned index = 0; index < candidates.size(); ++index) {
        if (convertedGroups[index].isEmpty())
            continue;
        changedFunctions.set(candidates[index].index());
        numberOfGroupCalls += convertedGroups[index].size();
        for (CallGroupID group : convertedGroups[index])
            ++context.groupEntrypoints[group.index()].numberOfSites;
    }

    for (GroupEntrypoints& entrypoints : context.groupEntrypoints) {
        if (entrypoints.numberOfSites)
            recordEntrypoints(context, entrypoints, tables);
        else
            removeEntrypoints(context, entrypoints);
    }
    context.build.statistics().count(StatisticID::GroupCalls, numberOfGroupCalls);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
