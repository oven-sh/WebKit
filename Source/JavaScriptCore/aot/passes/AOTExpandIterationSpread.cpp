/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsCheck.h"
#include "AOTFunctionAnalyses.h"
#include "AOTTypeTestSite.h"
#include <algorithm>
#include <functional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SpreadSite {
    ProgramPoint point;
    ValueID value;
    IterationPlan plan;
};

struct SpreadExpansion {
    FunctionPassContext& pass;
    IRBuilder& builder;
    IRBuilder::Expansion cut;
    ValueID array;
    BlockID loopEntry;
};

} // anonymous namespace

static Vector<SpreadSite> collectSpreads(FunctionPassContext& pass)
{
    Vector<SpreadSite> sites;
    for (BlockID block : pass.analyses.cfg().reversePostOrder()) {
        const BlockValues& values = pass.function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (pass.function.at(values[index]).opcode == Opcode::AppendSpread)
                sites.append(SpreadSite { ProgramPoint { block, index }, values[index], planSpread(pass, values[index]) });
        }
    }
    return sites;
}

static ReplacedEffects replacedEffectsOf(FunctionPassContext& pass, const Value& appendSpread)
{
    ReplacedEffects replaced;
    replaced.add(pass.analyses.effectContext(), pass.function, appendSpread);
    return replaced;
}

static void checkAppendElements(FunctionPassContext& pass, BlockID block, ValueID appendElements, const ReplacedEffects& replaced)
{
    checkReplacement(pass.analyses.effectContext(), pass.function, { pass.id, block, appendElements }, pass.function.at(appendElements), replaced, pass.build.diagnostics());
}

static ValueID appendResultAt(IRBuilder& builder, ValueID step, IteratorResumeResult result)
{
    Value read = IRBuilder::make(Opcode::ResultAt, { step });
    read.setIndex(static_cast<uint32_t>(result));
    return builder.append(WTF::move(read));
}

static void replaceWithAppendElements(FunctionPassContext& pass, const SpreadSite& site)
{
    Value& value = pass.function.at(site.value);
    ReplacedEffects replaced = replacedEffectsOf(pass, value);
    ValueID array = value.arrayOperand();
    ValueID source = value.iterableOperand();
    value.become(Opcode::AppendElements);
    value.setArrayOperand(array);
    value.setSourceOperand(source);
    pass.analyses.didChangeValues();
    checkAppendElements(pass, site.point.block, site.value, replaced);
}

static ValueID addLoopEntry(SpreadExpansion& expansion, BlockFrequency frequency, ValueID iterable)
{
    Function& function = expansion.pass.function;
    TypeID type = function.at(iterable).type;
    TypeID predicted = function.at(iterable).predictionOrType();
    expansion.loopEntry = expansion.builder.addBlock(frequency);
    ValueID source = expansion.builder.addParameter(expansion.loopEntry, type);
    if (predicted != type) {
        function.at(source).setPrediction(predicted);
        expansion.pass.analyses.didChangeValues();
    }
    return source;
}

static void emitFailedClassTest(SpreadExpansion& expansion, ValueID rest)
{
    IRBuilder& builder = expansion.builder;
    if (expansion.loopEntry) {
        builder.emitJump(expansion.loopEntry, { rest });
        return;
    }

    Value check = IRBuilder::make(Opcode::RequireObjectCoercible, { rest });
    check.setNullishAccess(NullishAccess::Iterate);
    builder.append(WTF::move(check));
    builder.emitUnreachable();
}

static void emitArrayArm(SpreadExpansion& expansion, const IterationArm& arm, ValueID iterable, const ReplacedEffects& replaced)
{
    RELEASE_ASSERT(arm.form == IterationForm::Array);
    IRBuilder& builder = expansion.builder;
    ValueID subject = iterable;
    ValueID guard;

    if (!arm.classIsProven) {
        ValueID test = builder.appendTypeTest(iterable, arm.testedType);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, BlockFrequency::Normal, BlockFrequency::Unlikely);
        subject = branch.provenOnTaken;
        builder.moveToEndOf(branch.notTaken);
        emitFailedClassTest(expansion, branch.provenOnNotTaken);
        builder.moveToEndOf(branch.taken);
    }

    if (!arm.namesAreProven) {
        RELEASE_ASSERT(expansion.loopEntry);
        ValueID test = builder.append(makePristineTest(subject, BuiltinClasses::Array, { PristineAspects::Storage, PristineAspects::Names }));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
        guard = branch.provenOnTaken;
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(expansion.loopEntry, { subject });
        builder.moveToEndOf(branch.taken);
    }

    Value appendElements = IRBuilder::make(Opcode::AppendElements, { expansion.array, subject });
    if (guard)
        appendElements.setGuardOperand(guard);
    BlockID block = builder.currentBlock();
    checkAppendElements(expansion.pass, block, builder.append(WTF::move(appendElements)), replaced);
    builder.endExpansion(expansion.cut, std::span<const ValueID> { });
}

static ValueID emitLoop(SpreadExpansion& expansion, ValueID source)
{
    IRBuilder& builder = expansion.builder;
    BlockFrequency frequency = expansion.pass.function.at(expansion.loopEntry).frequency;

    builder.moveToEndOf(expansion.loopEntry);
    Value iteratorOpen = IRBuilder::make(Opcode::IteratorOpen, { source });
    iteratorOpen.setIteratorKind(IteratorKind::Sync);
    ValueID open = builder.append(WTF::move(iteratorOpen));
    BlockID header = builder.addBlock(frequency);
    builder.emitJump(header);

    builder.moveToEndOf(header);
    builder.append(Opcode::LoopHint, { });
    Value iteratorResume = IRBuilder::make(Opcode::IteratorResume, { open });
    iteratorResume.setIteratorKind(IteratorKind::Sync);
    iteratorResume.setResumeMode(ResumeMode::Next);
    ValueID step = builder.append(WTF::move(iteratorResume));
    ValueID done = appendResultAt(builder, step, IteratorResumeResult::Done);
    ValueID value = appendResultAt(builder, step, IteratorResumeResult::Value);
    IRBuilder::BranchBlocks blocks = builder.emitBranch(done, frequency, frequency);
    builder.endExpansion(expansion.cut, std::span<const ValueID> { });

    builder.moveToEndOf(blocks.notTaken);
    builder.append(Opcode::AppendElement, { expansion.array, value });
    builder.emitJump(header);
    return open;
}

static void expandSpread(FunctionPassContext& pass, IRBuilder& builder, const SpreadSite& site, Vector<ValueID>& opensOfSpreads)
{
    const IterationPlan& plan = site.plan;
    RELEASE_ASSERT(plan.hasClassArm() || plan.hasProtocolArm());
    if (plan.hasClassArm() && plan.arms.first().needsNoTest()) {
        replaceWithAppendElements(pass, site);
        return;
    }

    Value original = pass.function.at(site.value);
    ValueID iterable = original.iterableOperand();
    ReplacedEffects replaced = replacedEffectsOf(pass, original);
    BlockFrequency loopFrequency = rarerOf(plan.hasClassArm() ? BlockFrequency::Unlikely : BlockFrequency::Normal, pass.function.at(site.point.block).frequency);

    builder.setOrigin(original.origin());
    SpreadExpansion expansion { pass, builder, builder.beginExpansion(site.point), original.arrayOperand(), { } };
    ValueID source;
    if (plan.hasProtocolArm())
        source = addLoopEntry(expansion, loopFrequency, iterable);

    if (plan.hasClassArm())
        emitArrayArm(expansion, plan.arms.first(), iterable, replaced);
    else
        builder.emitJump(expansion.loopEntry, { iterable });

    if (expansion.loopEntry)
        opensOfSpreads.append(emitLoop(expansion, source));
}

PassOutcome expandSpreads(FunctionPassContext& pass, IRBuilder& builder, Vector<ValueID>& opensOfSpreads)
{
    Vector<SpreadSite> sites = collectSpreads(pass);
    if (sites.isEmpty())
        return PassOutcome::Unchanged;

    std::ranges::sort(sites, std::ranges::greater { }, [](const SpreadSite& site) {
        return std::pair { site.point.block, site.point.index };
    });
    for (const SpreadSite& site : sites)
        expandSpread(pass, builder, site, opensOfSpreads);
    builder.setHandler(std::nullopt);
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
