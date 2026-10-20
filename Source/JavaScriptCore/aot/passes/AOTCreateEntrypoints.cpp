/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCreateEntrypointsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDerivedFunctions.h"
#include "AOTFlowResults.h"
#include "AOTInterningPhases.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

Function createDerivedHeader(const Function& target, FunctionID targetID, FunctionRole role, uint32_t keyParameter)
{
    Function header;
    header.name = target.name;
    header.module = target.module;
    header.position = target.position;
    header.text = target.text;
    header.kind = target.kind;
    header.role = role;
    header.flags = target.flags & FunctionFlags::IsChecked;
    header.length = target.length;
    header.enclosingRuntimeFunction = target.enclosingRuntimeFunction;
    header.thisType = DeclaredType { .type = TypeTable::anything };
    header.capabilities = capabilitiesOf(role);
    header.derivation = Derivation { .source = targetID, .parameter = keyParameter };
    return header;
}

static bool canReturnWithTailCall(const Function& function, BlockID blockID, ValueID result)
{
    const Block& block = function.at(blockID);
    return !block.handler && !block.values.isEmpty() && block.values.last() == result && function.at(result).opcode == Opcode::CallDirect;
}

static void emitRequiredTailCall(IRBuilder& builder, ValueID call)
{
    builder.emitTerminator(IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { call }));
}

// The frame this one returns to may already wait to test the result of a chain of tail calls, against the type written on the function it called.
// Then the chain goes on. Else this frame becomes the one that waits.
static void emitTailCallUnlessResultNeedsTest(IRBuilder& builder, ResultCheck check, FunctionID callee, ValueID result)
{
    BlockID block = builder.currentBlock();
    Value call = builder.function().at(result);
    BitVector erasedIndexes;
    erasedIndexes.set(builder.function().at(block).values.size() - 1);
    builder.eraseValues(block, erasedIndexes);

    IRBuilder::BranchBlocks arms = builder.emitBranch(builder.append(Opcode::ReturnsToResultTest, { }));
    builder.moveToEndOf(arms.taken);
    emitRequiredTailCall(builder, builder.append(Value(call)));

    builder.moveToEndOf(arms.notTaken);
    builder.emitReturn(appendResultCheck(builder, check, callee, builder.append(WTF::move(call))));
}

void emitReturnOfEntrypointCall(IRBuilder& builder, const FlowResults& flow, FunctionID callee, ValueID result, EntrypointResultTest resultTest)
{
    const Function& function = builder.function();
    if (builder.context().program.typeOf(function, result).isNever()) {
        builder.emitUnreachable();
        return;
    }

    ResultCheck check = resultCheckOf(flow, callee);
    if (resultTest == EntrypointResultTest::MadeUnlessCallerTests && needsResultCheck(builder, check, callee, result) && canReturnWithTailCall(function, builder.currentBlock(), result)) {
        emitTailCallUnlessResultNeedsTest(builder, check, callee, result);
        return;
    }

    bool isLeftToReceiver = resultTest == EntrypointResultTest::LeftToReceiver;
    ValueID checked = isLeftToReceiver ? result : appendResultCheck(builder, check, callee, result);
    if (checked != result || !canReturnWithTailCall(function, builder.currentBlock(), result)) {
        builder.emitReturn(checked);
        return;
    }

    Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { result });
    terminator.setRequirement(isLeftToReceiver ? TailCallRequirement::Required : TailCallRequirement::Allowed);
    builder.emitTerminator(WTF::move(terminator));
}

static BitVector findFunctionsEnteredWithoutReference(const Program& program, const FlowResults& flow)
{
    BitVector calledByHost = functionsCalledByHost(program);
    BitVector readFromVariables = findFunctionsReadFromVariables(program, flow);

    BitVector entered(program.functions.size());
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        // A function that is read and never called loses its body. The read still creates the closure, whose identity is the entrypoint.
        bool isEntered = readFromVariables.quickGet(index) || (calledByHost.quickGet(index) && function.hasBody());
        if (isEntered && canBeCalledByValueOf(function.role))
            entered.quickSet(index);
    }
    return entered;
}

static FunctionID addGenericEntrypoints(const Program& program, GenericEntrypointRequest request, NOESCAPE const Invocable<FunctionID(const GenericEntrypointRequest&)> auto& add)
{
    switch (entrypointVariantsOf(program.at(request.target))) {
    case EntrypointVariants::WithoutNewTarget:
        request.takesNewTarget = false;
        return add(request);
    case EntrypointVariants::WithNewTarget:
        request.takesNewTarget = true;
        return add(request);
    case EntrypointVariants::Both: {
        GenericEntrypointRequest constructRequest = request;
        constructRequest.takesNewTarget = true;
        constructRequest.groupEntrypoint = FunctionID();
        request.takesNewTarget = false;
        request.constructEntrypoint = add(constructRequest);
        return add(request);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The entrypoints get the ids, and their types the ids, that they would get one after the other.
static void addEntrypointsForHost(ProgramEditor& editor, const AnalysisContext& context, std::span<const FunctionID> targets)
{
    if (targets.empty())
        return;

    const Program& program = editor.program();
    const ParallelLoops& loops = context.build.parallelLoops();
    unsigned count = static_cast<unsigned>(targets.size());
    Vector<uint32_t> firstAdded;
    firstAdded.reserveInitialCapacity(count);
    unsigned numberAdded = 0;
    for (FunctionID target : targets) {
        firstAdded.append(numberAdded);
        numberAdded += entrypointVariantsOf(program.at(target)) == EntrypointVariants::Both ? 2 : 1;
    }
    uint32_t firstID = editor.reserveFunctions(numberAdded).index();

    InterningPhases phases(editor, count);
    forEachIndexInParallel(loops, count, [&](unsigned position) {
        TaskArenas arenas(phases, position);
        uint32_t nextID = firstID + firstAdded[position];
        addGenericEntrypoints(program, { .target = targets[position], .isEnteredByHost = true }, [&](const GenericEntrypointRequest& variant) {
            FunctionID id(nextID++);
            editor.replaceFunctionLike(id, variant.target, createGenericEntrypoint(context, arenas.types, arenas.strings, variant));
            return id;
        });
    });

    BodyRemaps remaps = phases.finish();
    if (remaps.isIdentity())
        return;
    forEachIndexInParallel(loops, numberAdded, [&](unsigned index) {
        applyRemaps(remaps, editor.at(FunctionID(firstID + index)));
    });
}

void createEntrypoints(ProgramEditor& editor, const OptimizationInputs& inputs, const CallGroups& groups, BuildContext& build)
{
    const Program& program = editor.program();
    AnalysisContext context { build, program, &inputs.flow, nullptr };
    BitVector entered = findFunctionsEnteredWithoutReference(program, inputs.flow);
    BitVector done(program.functions.size());
    size_t numberOfFunctionsBefore = program.functions.size();
    uint64_t numberOfGroupEntrypoints = 0;

    {
        TypeArena types(editor.types());
        StringArena strings(editor.strings());
        auto add = [&](const GenericEntrypointRequest& variant) {
            return editor.addFunctionLike(variant.target, createGenericEntrypoint(context, types, strings, variant));
        };
        for (unsigned index = 0; index < groups.numberOfGroups(); ++index) {
            const CallGroup& group = groups.at(CallGroupID(index));
            GroupSignature signature = createGroupSignature(program, inputs.flow, types, groups, group);
            for (FunctionID member : group.members) {
                RELEASE_ASSERT(!inputs.derivedFunctions->find(derivedFunctionKey(FunctionRole::GenericEntrypoint, member)));
                EntrypointResultTest resultTest = groups.leavesResultTestToReceiver(member) ? EntrypointResultTest::LeftToReceiver : EntrypointResultTest::MadeByEntrypoint;
                FunctionID groupEntrypoint = editor.addFunctionLike(member, createGroupEntrypoint(context, types, member, signature, resultTest));
                addGenericEntrypoints(program, {
                    .target = member,
                    .groupEntrypoint = groupEntrypoint,
                    .callers = groups.genericCallersOf(member),
                    .isEnteredByHost = entered.get(member.index()),
                }, add);
                done.set(member.index());
                ++numberOfGroupEntrypoints;
            }
        }
    }

    Vector<FunctionID> enteredByHostOnly;
    for (size_t index : entered) {
        if (!done.get(index))
            enteredByHostOnly.append(FunctionID(index));
    }
    addEntrypointsForHost(editor, context, enteredByHostOnly.span());

    uint64_t numberOfAddedFunctions = program.functions.size() - numberOfFunctionsBefore;
    build.statistics().count(StatisticID::GroupEntrypoints, numberOfGroupEntrypoints);
    build.statistics().count(StatisticID::GenericEntrypoints, numberOfAddedFunctions - numberOfGroupEntrypoints);
    build.statistics().countChanges(PassID::CreateEntrypoints, numberOfAddedFunctions);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
