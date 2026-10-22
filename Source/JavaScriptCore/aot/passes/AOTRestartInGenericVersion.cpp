/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRestartInGenericVersion.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

// The typed body that stands behind the tests at the entrance of a function has a generic version of the whole function beside it.
static FunctionID findGenericVersionBeside(const Program& program, FunctionID id, const Function& function)
{
    if (function.role != FunctionRole::Source || !function.derivation.source || function.results.size() != 1 || !function.hasOrdinaryFrame())
        return { };
    const auto* record = program.at(function.derivation.source).creator.tryAs<CreatorRecord::EntryChecks>();
    if (!record || record->body != id || !program.at(record->fallback).hasBody())
        return { };
    return record->fallback;
}

// Nobody can tell that it ran, so nobody can tell that it ran twice.
static bool canRunTwice(const Effects& effects)
{
    return effects.writes.containsOnly(heapsWhoseWritesNobodySees) && !effects.mayRunOtherCode();
}

static BitVector findBlocksThatEndBeforeAnyEffect(FunctionPassContext& pass)
{
    const Function& function = pass.function;
    const CFG& cfg = pass.analyses.cfg();
    BitVector endsBeforeAnyEffect(function.blocks.size());
    for (BlockID block : cfg.reversePostOrder()) {
        auto canRunTwiceOnAnyEdge = [](const EffectsOnEdges& effects) {
            return canRunTwice(effects.normal) && canRunTwice(effects.handler);
        };
        bool everythingCanRunTwice = canRunTwiceOnAnyEdge(pass.analyses.effectsOfTerminator(block)) && std::ranges::all_of(function.at(block).values, [&](ValueID id) {
            return canRunTwiceOnAnyEdge(pass.analyses.effectsOf(id));
        });
        if (everythingCanRunTwice)
            endsBeforeAnyEffect.quickSet(block.index());
    }
    for (bool changed = true; std::exchange(changed, false);) {
        for (BlockID block : cfg.reversePostOrder()) {
            if (!endsBeforeAnyEffect.quickGet(block.index()))
                continue;
            bool followsEffect = std::ranges::any_of(cfg.predecessors(block), [&](BlockID predecessor) {
                return !endsBeforeAnyEffect.quickGet(predecessor.index());
            });
            if (followsEffect) {
                endsBeforeAnyEffect.quickClear(block.index());
                changed = true;
            }
        }
    }
    return endsBeforeAnyEffect;
}

// A test of a type may be a guess, or tell the members of a union apart: the side on which it fails is then an ordinary way through the function, which is worth its own code.
// A test of the state of an object fails where somebody has changed the object behind the back of its type.
static bool testsStateOfObject(const Function& function, const Terminator& branch)
{
    Proof proof = provesOf(function.at(branch.conditionOperand()).opcode);
    return provesForStretch(proof) && proof != Proof::ExactFunction;
}

// What fails such a test is undefined or null where the program goes on as if it had an object: it is about to throw.
static bool failsOnlyForUndefinedAndNull(FunctionPassContext& pass, const Terminator& branch)
{
    const Value& test = pass.function.at(branch.conditionOperand());
    if (test.opcode != Opcode::TypeTest)
        return false;
    return pass.program.typeOf(pass.function, test.valueOperand()).without(pass.program.at(test.testedType()), pass.types).isUndefinedOrNull();
}

static BlockID emitCallOfGenericVersion(IRBuilder& builder, FunctionID genericVersion)
{
    const Function& function = builder.function();
    const Function& generic = builder.context().program.at(genericVersion);
    BlockID block = builder.addBlock(BlockFrequency::Rare);
    builder.moveToEndOf(block);
    Value call = IRBuilder::make(Opcode::CallDirect);
    call.setFunction(genericVersion);
    call.setArgumentsOperands(function.at(entryBlock).parameters.span());
    call.setIsNeverInlined(true);
    ValueID result = builder.append(WTF::move(call));
    Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { result });
    terminator.setRequirement(tailCallRequirementOf(generic));
    builder.emitTerminator(WTF::move(terminator));
    return block;
}

static Vector<Edge, 16> findFailingEdgesBeforeAnyEffect(FunctionPassContext& pass, BlockID restart)
{
    const Function& function = pass.function;
    BitVector endsBeforeAnyEffect = findBlocksThatEndBeforeAnyEffect(pass);
    Vector<Edge, 16> failingEdges;
    for (BlockID id : pass.analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        if (!endsBeforeAnyEffect.quickGet(id.index()) || block.frequency != BlockFrequency::Normal || block.handler)
            continue;
        if (!block.terminator.isConditionalBranch() || !testsThatMayRestartInGenericVersion.contains(function.at(block.terminator.conditionOperand()).opcode))
            continue;
        bool testsState = testsStateOfObject(function, block.terminator);
        if (!testsState && !failsOnlyForUndefinedAndNull(pass, block.terminator))
            continue;
        for (uint32_t index = 0; index < block.terminator.targets.size(); ++index) {
            BlockID target = block.terminator.targets[index].block;
            BlockFrequency frequency = function.at(target).frequency;
            if (target != restart && (testsState ? frequency != BlockFrequency::Normal : frequency == BlockFrequency::Rare))
                failingEdges.append(Edge { id, index });
        }
    }
    return failingEdges;
}

// A test of the state of an object that fails leads into generic code for what follows it, which the function holds for itself.
// Until the body has done something that somebody can see, the call can begin again instead: the generic version is given the arguments of the call, and does all of it.
// So every such test of a function shares one block, nothing is handed over, and the generic code behind them goes.
PassOutcome restartInGenericVersion(FunctionPassContext& pass)
{
    const Program& program = pass.program;
    Function& function = pass.function;
    FunctionID genericVersion = findGenericVersionBeside(program, pass.id, function);
    if (!genericVersion)
        return PassOutcome::Unchanged;
    const Function& generic = program.at(genericVersion);
    if (function.at(entryBlock).parameters.size() != generic.at(entryBlock).parameters.size())
        return PassOutcome::Unchanged;
    TypingContext typing { program, function, pass.types };
    if (!typing.typeOf(generic.result()).isSubsetOf(typing.typeOf(function.result()), program))
        return PassOutcome::Unchanged;
    for (unsigned index = 0; index < generic.numberOfEntryParameters(); ++index) {
        if (!program.typeOf(function, function.at(entryBlock).parameters[index]).isSubsetOf(program.at(declaredEntryTypeOf(typing, generic, index)), program))
            return PassOutcome::Unchanged;
    }

    IRBuilder builder(pass.analyses.context(), function, pass.types, &pass.analyses);
    builder.setOrigin({ function.position, 0 });
    BlockID restart;
    size_t numberOfRestarts = 0;
    // Generic code that has gone no longer joins the code behind it, which may then stand in front of every effect too.
    for (;;) {
        Vector<Edge, 16> failingEdges = findFailingEdgesBeforeAnyEffect(pass, restart);
        if (failingEdges.isEmpty())
            break;
        if (!restart)
            restart = emitCallOfGenericVersion(builder, genericVersion);
        for (Edge edge : failingEdges)
            builder.redirectEdge(edge, Target { restart, { }, { } });
        builder.emptyUnreachableBlocks();
        numberOfRestarts += failingEdges.size();
    }
    if (!numberOfRestarts)
        return PassOutcome::Unchanged;
    pass.build.statistics().count(StatisticID::RestartsInGenericVersion, numberOfRestarts);
    // The graph is that of the last search, which has edited nothing.
    pass.keptAnalyses = controlFlowAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
