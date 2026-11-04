/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameAllocationPlanner.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRBuilder.h"
#include "AOTSSARepair.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool leavesFunction(const Block& block)
{
    switch (block.terminator.kind) {
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
        return true;
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
        return !block.handler;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool capturesEnvironment(FunctionAnalyses& analyses, ValueID closure, ValueID environment)
{
    return std::ranges::any_of(analyses.function().at(closure).operands(), [&](ValueID captured) {
        return captured && analyses.sameObjectThroughCasts(captured) == environment;
    });
}

static Vector<ProgramPoint, 4> findMaterializationPoints(FunctionAnalyses& analyses, ValueID environment, const BitVector& frameClosures)
{
    const Function& function = analyses.function();
    Vector<ProgramPoint, 4> points;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (function.at(values[index]).opcode == Opcode::NewClosure && !frameClosures.get(values[index].index()) && capturesEnvironment(analyses, values[index], environment))
                points.append({ block, index });
        }
    }
    return points;
}

// Some run leaves the function, or makes the environment anew, without a copy.
static bool paysToMaterializeLate(FunctionAnalyses& analyses, ValueID environment, std::span<const ProgramPoint> points)
{
    const Function& function = analyses.function();
    BitVector blocksWithPoint(function.blocks.size());
    for (const ProgramPoint& point : points)
        blocksWithPoint.quickSet(point.block.index());

    const CFG& cfg = analyses.cfg();
    BlockID allocationBlock = analyses.useLists().blockOf(environment);
    if (blocksWithPoint.quickGet(allocationBlock.index()))
        return false;

    BitVector blocksBeforePoints = cfg.blocksReachableFrom(allocationBlock, blocksWithPoint);
    bool reachesAllocationAgain = std::ranges::any_of(cfg.predecessors(allocationBlock), [&](BlockID predecessor) {
        return blocksBeforePoints.quickGet(predecessor.index());
    });
    if (reachesAllocationAgain)
        return true;
    for (size_t index : blocksBeforePoints) {
        if (leavesFunction(function.at(BlockID(index))))
            return true;
    }
    return false;
}

static std::optional<BitVector> findFrameHolders(FunctionAnalyses& analyses, ValueID environmentID, const BitVector& frameClosures)
{
    const EscapeAnalysis& escape = analyses.escape();
    std::optional<ObjectID> environment = escape.objectDefinedBy(environmentID);
    if (!environment || escape.aliasesOf(*environment).size() != 1)
        return std::nullopt;

    BitVector holders(analyses.function().values.size());
    BitVector visited(escape.numberOfObjects());
    Vector<ObjectID, 8> worklist;
    for (size_t index : frameClosures) {
        if (!capturesEnvironment(analyses, ValueID(index), environmentID))
            continue;
        std::optional<ObjectID> object = escape.objectDefinedBy(ValueID(index));
        if (!object)
            return std::nullopt;
        if (!visited.quickSet(object->index()))
            worklist.append(*object);
    }

    while (!worklist.isEmpty()) {
        ObjectID object = worklist.takeLast();
        for (ValueID alias : escape.aliasesOf(object))
            holders.quickSet(alias.index());
        for (ObjectID container : escape.containersOf(object)) {
            if (!visited.quickSet(container.index()))
                worklist.append(container);
        }
    }
    return holders;
}

bool canMaterializeEnvironmentLate(FunctionAnalyses& analyses, ValueID environment, const BitVector& frameClosures, std::span<const ClosureMadeAgain> closuresMadeAgain)
{
    // A closure of the heap takes the environment of the heap, so the environment is copied in front of it. Where a closure is made again, that is the last use of the closure of the frame.
    Vector<ProgramPoint, 4> points = findMaterializationPoints(analyses, environment, frameClosures);
    Vector<ValueID, 4> closuresAtTheirLastUse(FillWith { }, points.size(), ValueID());
    for (const ClosureMadeAgain& madeAgain : closuresMadeAgain) {
        if (!capturesEnvironment(analyses, madeAgain.closure, environment))
            continue;
        points.append(analyses.useLists().pointOf(madeAgain.use));
        closuresAtTheirLastUse.append(madeAgain.closure);
    }
    if (!paysToMaterializeLate(analyses, environment, points.span()))
        return false;
    std::optional<BitVector> holders = findFrameHolders(analyses, environment, frameClosures);
    if (!holders || holders->isEmpty())
        return holders.has_value();

    const Function& function = analyses.function();
    Liveness liveness(function, analyses.cfg(), analyses.useLists(), { .trackedValues = &*holders });
    std::optional<Liveness::LocalCalc> calc;
    BlockID currentBlock;
    uint32_t nextIndex = 0;
    for (size_t position = points.size(); position--;) {
        const ProgramPoint& point = points[position];
        if (!calc || currentBlock != point.block || nextIndex < point.index) {
            calc.emplace(liveness, point.block);
            currentBlock = point.block;
            nextIndex = function.at(point.block).values.size();
        }
        while (nextIndex > point.index)
            calc->execute(--nextIndex);

        bool isHolderLive = false;
        calc->forEachLive([&](ValueID holder) {
            isHolderLive |= holder != closuresAtTheirLastUse[position];
        });
        if (isHolderLive)
            return false;
    }
    return true;
}

void materializeEnvironmentLate(IRBuilder& builder, FunctionAnalyses& analyses, ValueID environment, const BitVector& frameClosures)
{
    const Function& function = analyses.function();
    RELEASE_ASSERT(&builder.function() == &function);
    RELEASE_ASSERT(function.at(environment).storage() == AllocationStorage::FrameUntilMaterialized);
    Vector<ProgramPoint, 4> points = findMaterializationPoints(analyses, environment, frameClosures);
    if (points.isEmpty())
        return;

    LayoutID layout = function.at(environment).layout();
    Vector<ValueID, 4> copies;
    Vector<ValueID, 4> closures;
    copies.reserveInitialCapacity(points.size());
    for (const ProgramPoint& point : points) {
        closures.append(function.at(point.block).values[point.index]);
        builder.setOrigin(function.at(closures.last()).origin());
        Value copy = IRBuilder::make(Opcode::MaterializeEnvironment, { environment });
        copy.setLayout(layout);
        copies.append(builder.insertBefore(point, WTF::move(copy)));
    }
    builder.applyInsertions();

    if (analyses.context().build.isSabotaged(Sabotage::MakersKeepEnvironmentsThatWereCopied)) {
        for (size_t index = 0; index < copies.size(); ++index) {
            Value& closure = builder.function().edit(closures[index], ValueEdit::Definition);
            for (uint32_t operandIndex = 0; operandIndex < closure.numberOfOperands(); ++operandIndex) {
                if (ValueID captured = closure.operand(operandIndex); captured && sameObjectThroughCasts(function, captured) == environment)
                    closure.setOperand(operandIndex, copies[index]);
            }
        }
        return;
    }

    SSARepair repair(builder, analyses);
    auto variable = repair.addVariable(environment);
    for (ValueID copy : copies)
        repair.addDefinition(variable, copy);
    repair.run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
