/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFuseGeneratorLoops.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct DeferredStart {
    ValueID generator;
    ValueID start;
    // The edge on which the generator has failed the test for an unmodified one. Behind it anything may look at the generator, so it is started there as at its creation.
    std::optional<Edge> edgeToProtocol;
    Vector<ValueID, 2> firstSteps;
};

} // anonymous namespace

// The call that starts a generator at its creation runs the head of the body, which evaluates the parameters, up to the initial yield. Nothing tells when a head without an effect has run.
static bool suspendsBeforeAnyEffect(FunctionPassContext& pass, const GeneratorBodyDescription& description)
{
    const Function& body = pass.program.at(description.body);
    if (!body.hasBody() || !description.generatorParameter)
        return false;
    FunctionAnalyses bodyAnalyses(AnalysisContext { pass.build, pass.program, &pass.inputs.flow, &pass.inputs.effects, description.body }, body);
    ValueID driver = body.at(entryBlock).parameters[*description.generatorParameter];
    for (ValueID id : body.at(entryBlock).values) {
        const Value& value = body.at(id);
        if (value.opcode == Opcode::IsGeneratorRunning)
            return value.generatorOperand() == driver;
        if (!bodyAnalyses.isUnobservable(id))
            return false;
    }
    return false;
}

static bool isTestOfPrototype(const Function& function, const Use& use)
{
    return use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::IsUnmodifiedGenerator;
}

static std::optional<Edge> findEdgeToProtocol(FunctionPassContext& pass, ValueID generator, BlockID block)
{
    const Function& function = pass.function;
    const Terminator& terminator = function.at(block).terminator;
    if (terminator.kind != TerminatorKind::Branch)
        return std::nullopt;
    const Value& test = function.at(terminator.conditionOperand());
    if (test.opcode != Opcode::IsUnmodifiedGenerator || pass.analyses.sameObjectThroughCasts(test.valueOperand()) != generator)
        return std::nullopt;
    constexpr uint32_t notTakenTargetIndex = 1;
    return Edge { block, notTakenTargetIndex };
}

static std::optional<DeferredStart> planDeferredStart(FunctionPassContext& pass, const EscapeAnalysis& escape, ValueID generator)
{
    const Function& function = pass.function;
    FunctionAnalyses& analyses = pass.analyses;
    std::optional<ObjectID> object = escape.objectDefinedBy(generator);
    if (!object || escape.liveInstancesOf(*object) != LiveInstances::AtMostOne)
        return std::nullopt;
    std::optional<GeneratorBodyDescription> description = describeGeneratorBody(pass.program, function, generator);
    if (!description || !suspendsBeforeAnyEffect(pass, *description))
        return std::nullopt;

    const UseLists& useLists = analyses.useLists();
    DeferredStart plan { generator, description->startAtCreation, std::nullopt, { } };
    if (useLists.locationOf(plan.start).placement != ValuePlacement::InBlock || useLists.useCount(plan.start))
        return std::nullopt;
    plan.edgeToProtocol = findEdgeToProtocol(pass, generator, useLists.blockOf(plan.start));
    auto isBehindFailedTest = [&](BlockID block) {
        return plan.edgeToProtocol && analyses.dominates(*plan.edgeToProtocol, block);
    };

    for (const ObjectUse& objectUse : escape.usesOf(*object)) {
        const Use& use = objectUse.use;
        if (use.kind != Use::Kind::Operand || isBehindFailedTest(use.block))
            continue;
        const Value& user = function.at(use.user);
        if (user.opcode != Opcode::StepGenerator || user.resumeMode() != ResumeMode::Next || use.operandIndex != OperandIndex::StepGenerator::generator)
            continue;
        // What the body throws in its first run leaves the generator as running. Nobody sees that where the exception leaves the function.
        if (user.check() != GeneratorStepCheck::None || escape.singleObject(user.generatorOperand()) != *object || function.at(use.block).handler)
            return std::nullopt;
        if (!analyses.dominates(plan.start, useLists.pointOf(use.user)))
            return std::nullopt;
        plan.firstSteps.append(use.user);
    }
    if (plan.firstSteps.isEmpty())
        return std::nullopt;

    // Every other reader finds a generator that has taken a step.
    for (const ObjectUse& objectUse : escape.usesOf(*object)) {
        const Use& use = objectUse.use;
        if (isBehindFailedTest(use.block) || isTestOfPrototype(function, use))
            continue;
        if (use.kind == Use::Kind::Operand && (use.user == plan.start || plan.firstSteps.contains(use.user)))
            continue;
        bool isBehindStep = plan.firstSteps.containsIf([&](ValueID step) {
            return analyses.dominates(step, use);
        });
        if (!isBehindStep)
            return std::nullopt;
    }
    return plan;
}

static ValueID appendResultOfStep(IRBuilder& builder, ValueID step, StepGeneratorResult result)
{
    Value read = IRBuilder::make(Opcode::ResultAt, { step });
    read.setIndex(static_cast<uint32_t>(result));
    return builder.append(WTF::move(read));
}

static void applyDeferredStart(FunctionPassContext& pass, IRBuilder& builder, const DeferredStart& plan)
{
    Function& function = pass.function;
    FunctionAnalyses& analyses = pass.analyses;
    Value start(function.at(plan.start));

    // One step outside of every loop is the first step whenever it runs.
    bool isKnownToBeFirst = plan.firstSteps.size() == 1 && !analyses.isInCycle(analyses.useLists().blockOf(plan.firstSteps[0]));

    ValueLocation location = analyses.useLists().locationOf(plan.start);
    BitVector erased;
    erased.set(location.index);
    builder.eraseValues(location.block, erased);
    if (plan.edgeToProtocol) {
        BlockID added = builder.splitEdge(*plan.edgeToProtocol);
        builder.insertBefore(ProgramPoint::atHeadOf(added), Value(start));
        builder.applyInsertions();
    }

    for (ValueID site : plan.firstSteps) {
        Value step(function.at(site));
        ValueID generator = step.generatorOperand();
        builder.setOrigin(step.origin());
        IRBuilder::Expansion expansion = builder.beginExpansion(analyses.useLists().pointOf(site));
        std::optional<BlockID> resumes;
        if (!isKnownToBeFirst) {
            IRBuilder::BranchBlocks blocks = builder.emitBranch(builder.append(Opcode::IsGeneratorUnstarted, { generator }));
            resumes = blocks.notTaken;
        }
        builder.append(Opcode::MarkGeneratorRunning, { generator });
        ValueID value = builder.append(Value(start));
        ValueID status = builder.append(Opcode::TakeGeneratorStepStatus, { generator });
        builder.endExpansion(expansion, { { value, status } });
        if (!resumes)
            continue;
        builder.moveToEndOf(*resumes);
        ValueID resumed = builder.append(WTF::move(step));
        builder.endExpansion(expansion, { { appendResultOfStep(builder, resumed, StepGeneratorResult::Value), appendResultOfStep(builder, resumed, StepGeneratorResult::Status) } });
    }
}

// A generator that no loop could be fused with is still seldom looked at between its creation and its first step.
PassOutcome startGeneratorsAtFirstStep(FunctionPassContext& pass)
{
    bool hasGenerator = std::ranges::any_of(pass.function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return pass.function.at(id).opcode == Opcode::NewGenerator;
        });
    });
    if (!hasGenerator)
        return PassOutcome::Unchanged;

    IRBuilder builder(pass.analyses.context(), pass.function, pass.types, &pass.analyses);
    PassOutcome outcome = PassOutcome::Unchanged;
    BitVector isTried;
    for (;;) {
        const EscapeAnalysis& escape = pass.analyses.escapeWithLiveInstances();
        if (escape.gaveUp())
            return outcome;
        std::optional<DeferredStart> plan;
        for (BlockID block : pass.analyses.cfg().reversePostOrder()) {
            for (ValueID id : pass.function.at(block).values) {
                if (plan || pass.function.at(id).opcode != Opcode::NewGenerator || isTried.set(id.index()))
                    continue;
                plan = planDeferredStart(pass, escape, id);
            }
        }
        if (!plan)
            return outcome;
        applyDeferredStart(pass, builder, *plan);
        pass.build.statistics().count(StatisticID::GeneratorsStartedAtFirstStep);
        outcome = PassOutcome::Changed;
        // The builder makes every edit and tells the analyses, and the pass ends with a search behind the last one.
        pass.keptAnalyses = allAnalyses;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
