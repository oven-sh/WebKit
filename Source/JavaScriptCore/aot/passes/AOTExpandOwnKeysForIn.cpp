/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandOwnKeysInternal.h"

#if ENABLE(AOT)

#include "AOTCodeSizeEstimate.h"
#include "AOTBlockFrequencies.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTNaturalLoops.h"
#include "AOTRegionCloner.h"
#include <algorithm>
#include <array>
#include <memory>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ForInLoop {
    ValueID open;
    ValueID step;
    BlockID entry;
    BlockID header;
    Vector<BlockID> region;
    OwnKeyWalkPlan plan;
};

} // anonymous namespace

static bool hasNoEffect(FunctionPassContext& context, ValueID id)
{
    return context.function.at(id).opcode == Opcode::LoopHint || context.analyses.effectsOf(id).onAnyEdge().isRemovableWhenUnused();
}

// A turn that finds the object changed starts over at the header of the loop as written, so nothing in front of the step may have happened twice.
static ValueID findStepAtHeadOf(FunctionPassContext& context, BlockID header)
{
    for (ValueID id : context.function.at(header).values) {
        if (context.function.at(id).opcode == Opcode::ForInStep)
            return id;
        if (!hasNoEffect(context, id))
            return { };
    }
    return { };
}

static BlockID findOnlyEntryOf(FunctionPassContext& context, const NaturalLoop& loop)
{
    BlockID entry;
    for (BlockID predecessor : context.analyses.cfg().predecessors(loop.header())) {
        if (context.analyses.naturalLoops().belongsTo(predecessor, loop))
            continue;
        if (entry)
            return { };
        entry = predecessor;
    }
    if (!entry || context.function.at(entry).terminator.kind != TerminatorKind::Jump)
        return { };
    return entry;
}

static Vector<PropertyKeyID, 8> namesListedFor(const Program& program, const OwnKeyWalkArm& arm)
{
    Vector<PropertyKeyID, 8> names;
    for (uint32_t index : arm.order) {
        const Field& field = program.at(arm.layout).fields[index];
        if (isEnumerableStringField(program, field))
            names.append(field.key);
    }
    return names;
}

static std::optional<ForInLoop> planForInLoop(FunctionPassContext& context, const NaturalLoop& naturalLoop)
{
    const Function& function = context.function;
    ForInLoop loop;
    loop.header = naturalLoop.header();
    if (function.at(loop.header).frequency == BlockFrequency::Rare || isCold(heatClassOf(context, loop.header)))
        return std::nullopt;

    loop.step = findStepAtHeadOf(context, loop.header);
    if (!loop.step || !function.at(loop.step).positionOperand())
        return std::nullopt;
    loop.open = function.at(loop.step).enumeratorOperand();
    const Value& open = function.at(loop.open);
    if (open.opcode != Opcode::ForInOpen || open.objectOperand() != function.at(loop.step).objectOperand())
        return std::nullopt;

    loop.entry = findOnlyEntryOf(context, naturalLoop);
    if (!loop.entry || !function.at(loop.entry).values.contains(loop.open))
        return std::nullopt;
    // Between the open and the entry of the loop nothing may change the object: the tests of an arm say what the enumerator lists.
    const BlockValues& valuesOfEntry = function.at(loop.entry).values;
    for (size_t index = valuesOfEntry.find(loop.open) + 1; index < valuesOfEntry.size(); ++index) {
        if (!hasNoEffect(context, valuesOfEntry[index]))
            return std::nullopt;
    }

    for (unsigned index = 0; index < naturalLoop.size(); ++index)
        loop.region.append(naturalLoop.at(index));
    if (RegionCloner::checkCanDuplicate(context.analyses, loop.region.span(), singleElementSpan(loop.header)) != CloneCheck::CanClone)
        return std::nullopt;

    std::optional<OwnKeyWalkPlan> plan = planWalkOfEnumeratedObject(context, loop.open);
    if (!plan)
        return std::nullopt;
    loop.plan = WTF::move(*plan);

    // An object without names is done at the first step of the loop as written.
    loop.plan.arms.removeAllMatching([&](const OwnKeyWalkArm& arm) {
        return namesListedFor(context.program, arm).isEmpty();
    });

    uint64_t bytesOfRegion = 0;
    for (BlockID block : loop.region)
        bytesOfRegion += estimatedCodeBytes(context.program, function, function.at(block));
    uint64_t numberOfCopies = 0;
    for (const OwnKeyWalkArm& arm : loop.plan.arms)
        numberOfCopies += namesListedFor(context.program, arm).size();
    // The arms that fit are expanded, and the objects of the others take the loop as written.
    if (context.exceeds(LimitID::MaxVersionedIterationBytes, bytesOfRegion * numberOfCopies, loop.open)) {
        uint64_t maxCopies = context.limit(LimitID::MaxVersionedIterationBytes) / std::max<uint64_t>(bytesOfRegion, 1);
        size_t numberOfArms = 0;
        for (uint64_t copies = 0; numberOfArms < loop.plan.arms.size(); ++numberOfArms) {
            copies += namesListedFor(context.program, loop.plan.arms[numberOfArms]).size();
            if (copies > maxCopies)
                break;
        }
        loop.plan.arms.shrink(numberOfArms);
    }
    if (loop.plan.arms.isEmpty())
        return std::nullopt;
    return loop;
}

namespace {

class ForInLoopExpander final : public Emission {
public:
    ForInLoopExpander(IRBuilder& builder, const EmissionInputs& inputs, FunctionPassContext& pass, const ForInLoop& loop)
        : Emission(builder, inputs)
        , m_pass(pass)
        , m_loop(loop)
    {
    }

    void run()
    {
        Function& function = builder().function();
        builder().setOrigin(function.at(m_loop.open).origin());
        Vector<BlockID, 4> firstTurnOfArm;
        for (const OwnKeyWalkArm& arm : m_loop.plan.arms)
            firstTurnOfArm.append(cloneTurnsOf(arm));
        m_pass.analyses.didChangeControlFlow();

        Target toLoop = function.at(m_loop.entry).terminator.targets[0];
        function.at(m_loop.entry).terminator = Terminator();
        m_pass.analyses.didChangeControlFlow();
        builder().setHandler(function.at(m_loop.entry).handler);
        builder().moveToEndOf(m_loop.entry);

        BlockFrequency frequency = function.at(m_loop.entry).frequency;
        ValueID source = function.at(m_loop.open).objectOperand();
        for (size_t index = 0; index < m_loop.plan.arms.size(); ++index) {
            const OwnKeyWalkArm& arm = m_loop.plan.arms[index];
            BlockID nextArm = builder().addBlock(frequency);
            TestedObject object = emitShapeTest(source, singleElementSpan(arm.layout), nextArm);
            emitKeyOrderTest(object, arm.layout, arm.order.span(), nextArm);
            builder().emitJump(firstTurnOfArm[index], Arguments(toLoop.arguments));
            builder().moveToEndOf(nextArm);
        }
        builder().emitJump(toLoop.block, Arguments(toLoop.arguments));

        RegionCloner::repairSSA(m_pass.analyses, m_cloners.span());
        for (const Turn& turn : m_turns)
            expandStepOf(turn);
    }

private:
    struct Turn {
        BlockID header;
        ValueID step;
        PropertyKeyID name;
        size_t position { 0 };
    };

    // The turns of an arm stand one behind the other, each with its name as a constant. The one behind the last goes on in the loop as written, which finds what the prototypes list, or the end.
    BlockID cloneTurnsOf(const OwnKeyWalkArm& arm)
    {
        Vector<PropertyKeyID, 8> names = namesListedFor(program(), arm);
        BlockID next = m_loop.header;
        for (size_t position = names.size(); position--;)
            next = cloneTurn(names[position], position, next);
        return next;
    }

    BlockID cloneTurn(PropertyKeyID name, size_t position, BlockID nextTurn)
    {
        Function& function = builder().function();
        auto cloner = makeUniqueWithoutFastMallocCheck<RegionCloner>(builder(), function);
        cloner->cloneBlocks(m_loop.region.span(), [](ValueID, Value&) { });
        BlockID header = cloner->cloneOf(m_loop.header);
        for (BlockID block : cloner->clonedBlocks()) {
            for (Target& target : function.at(block).terminator.targets) {
                if (target.block == header)
                    target.block = nextTurn;
            }
        }
        m_turns.append(Turn { header, cloner->cloneOf(m_loop.step), name, position });
        m_cloners.append(WTF::move(cloner));
        return header;
    }

    // While the object is as the enumerator found it, the step answers with the name at its position. Otherwise the turn is taken by the loop as written, from the same position on.
    void expandStepOf(const Turn& turn)
    {
        Function& function = builder().function();
        Arguments carriedValues;
        for (ValueID parameter : function.at(turn.header).parameters)
            carriedValues.append(parameter);

        // A turn is on no cycle of its own.
        BitVector loopHints;
        const BlockValues& values = function.at(turn.header).values;
        for (uint32_t index = 0; values[index] != turn.step; ++index) {
            if (function.at(values[index]).opcode == Opcode::LoopHint)
                loopHints.set(index);
        }
        builder().eraseValues(turn.header, loopHints);

        ValueID enumerator = function.at(turn.step).enumeratorOperand();
        ValueID object = function.at(turn.step).objectOperand();
        BlockFrequency frequency = function.at(turn.header).frequency;
        builder().setOrigin(function.at(turn.step).origin());
        IRBuilder::Expansion expansion = builder().beginExpansion({ turn.header, static_cast<uint32_t>(function.at(turn.header).values.find(turn.step)) });
        ValueID position = builder().appendInteger(turn.position);
        ValueID isCurrent = builder().append(Opcode::IsEnumeratorCurrent, { enumerator, object, position });
        IRBuilder::ProvenBranch branch = builder().emitBranchWithProof(isCurrent, 0, frequency, rarerOf(frequency, BlockFrequency::Unlikely));
        builder().moveToEndOf(branch.notTaken);
        builder().emitJump(m_loop.header, WTF::move(carriedValues));
        builder().moveToEndOf(branch.taken);
        std::array<ValueID, 3> results { builder().appendBoolean(false), appendString(program().at(turn.name).text), position };
        builder().endExpansion(expansion, results);
    }

    FunctionPassContext& m_pass;
    const ForInLoop& m_loop;
    Vector<std::unique_ptr<RegionCloner>, 8> m_cloners;
    Vector<Turn, 8> m_turns;
};

} // anonymous namespace

bool expandForInLoops(FunctionPassContext& context, IRBuilder& builder, const EmissionInputs& inputs)
{
    bool hasStep = context.function.values.containsIf([](const Value& value) {
        return value.opcode == Opcode::ForInStep;
    });
    if (!hasStep)
        return false;

    // One loop at a time: an expansion changes the blocks that the loops are made of.
    bool changed = false;
    for (;;) {
        std::optional<ForInLoop> found;
        NaturalLoops& loops = context.analyses.naturalLoops();
        for (unsigned index = 0; index < loops.numLoops() && !found; ++index)
            found = planForInLoop(context, loops.loop(index));
        if (!found)
            return changed;
        ForInLoopExpander(builder, inputs, context, *found).run();
        context.build.statistics().count(StatisticID::OwnKeyWalksExpanded, found->plan.arms.size());
        changed = true;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
