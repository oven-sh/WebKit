/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameAllocationMaterialization.h"

#if ENABLE(AOT)

#include "AOTSSARepair.h"
#include <algorithm>
#include <ranges>

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

EnvironmentMaterialization::EnvironmentMaterialization(FunctionAnalyses& analyses, ValueID environment, const BitVector& frameClosures, std::span<const ClosureMadeAgain> closuresMadeAgain)
    : m_analyses(analyses)
    , m_function(analyses.function())
    , m_environment(environment)
    , m_frameClosures(frameClosures)
    , m_closuresMadeAgain(closuresMadeAgain)
{
}

bool EnvironmentMaterialization::canRun()
{
    Vector<ProgramPoint, 4> materializationPoints = findMaterializationPoints();
    Vector<ProgramPoint, 4> allPoints = materializationPoints;
    for (const ClosureMadeAgain& madeAgain : m_closuresMadeAgain) {
        if (capturesEnvironment(m_function.at(madeAgain.closure)))
            allPoints.append(m_analyses.useLists().pointOf(madeAgain.use));
    }
    if (!isProfitable(allPoints.span()))
        return false;
    std::optional<BitVector> holders = findFrameHolders();
    return holders && !isAnyHolderLive(materializationPoints.span(), *holders) && !isAnyOtherHolderLiveWhereClosureIsMadeAgain(*holders);
}

void EnvironmentMaterialization::run(IRBuilder& builder)
{
    RELEASE_ASSERT(&builder.function() == &m_function);
    RELEASE_ASSERT(m_function.at(m_environment).storage() == AllocationStorage::FrameUntilMaterialized);
    Vector<ProgramPoint, 4> materializationPoints = findMaterializationPoints();
    if (materializationPoints.isEmpty())
        return;

    LayoutID layout = m_function.at(m_environment).layout();
    Vector<ValueID, 4> copies;
    copies.reserveInitialCapacity(materializationPoints.size());
    for (const ProgramPoint& point : materializationPoints) {
        builder.setOrigin(m_function.at(m_function.at(point.block).values[point.index]).origin());
        Value copy = IRBuilder::make(Opcode::MaterializeEnvironment, { m_environment });
        copy.setLayout(layout);
        copies.append(builder.insertBefore(point, WTF::move(copy)));
    }
    builder.applyInsertions();

    SSARepair repair(builder, m_analyses);
    auto variable = repair.addVariable(m_environment);
    for (ValueID copy : copies)
        repair.addDefinition(variable, copy);
    repair.run();
}

bool EnvironmentMaterialization::requiresHeapEnvironment(ValueID id) const
{
    const Value& value = m_function.at(id);
    return value.opcode == Opcode::NewClosure && !m_frameClosures.get(id.index()) && capturesEnvironment(value);
}

bool EnvironmentMaterialization::capturesEnvironment(const Value& closure) const
{
    for (uint32_t index = 0; index < closure.numberOfOperands(); ++index) {
        ValueID captured = closure.operand(index);
        if (captured && m_analyses.sameObjectThroughCasts(captured) == m_environment)
            return true;
    }
    return false;
}

bool EnvironmentMaterialization::isProfitable(std::span<const ProgramPoint> materializationPoints)
{
    BitVector blocksWithPoint(m_function.blocks.size());
    for (const ProgramPoint& point : materializationPoints)
        blocksWithPoint.quickSet(point.block.index());

    const CFG& cfg = m_analyses.cfg();
    BlockID allocationBlock = m_analyses.useLists().blockOf(m_environment);
    if (blocksWithPoint.quickGet(allocationBlock.index()))
        return false;

    BitVector blocksBeforePoints = cfg.blocksReachableFrom(allocationBlock, blocksWithPoint);
    bool reachesAllocationAgain = std::ranges::any_of(cfg.predecessors(allocationBlock), [&](BlockID predecessor) {
        return blocksBeforePoints.quickGet(predecessor.index());
    });
    if (reachesAllocationAgain)
        return true;
    for (size_t index : blocksBeforePoints) {
        if (leavesFunction(m_function.at(BlockID(index))))
            return true;
    }
    return false;
}

bool EnvironmentMaterialization::isAnyHolderLive(std::span<const ProgramPoint> materializationPoints, const BitVector& holders)
{
    if (holders.isEmpty())
        return false;

    Liveness liveness(m_function, m_analyses.cfg(), m_analyses.useLists(), { .trackedValues = &holders });
    std::optional<Liveness::LocalCalc> calc;
    BlockID currentBlock;
    uint32_t nextIndex = 0;
    for (const ProgramPoint& point : materializationPoints | std::views::reverse) {
        if (!calc || currentBlock != point.block) {
            calc.emplace(liveness, point.block);
            currentBlock = point.block;
            nextIndex = m_function.at(point.block).values.size();
        }
        while (nextIndex > point.index)
            calc->execute(--nextIndex);

        bool isLive = false;
        calc->forEachLive([&](ValueID) {
            isLive = true;
        });
        if (isLive)
            return true;
    }
    return false;
}

// The closure of the heap takes the environment of the heap, so the environment is copied in front of it. The use is the last one of the closure of the frame.
bool EnvironmentMaterialization::isAnyOtherHolderLiveWhereClosureIsMadeAgain(const BitVector& holders)
{
    if (holders.isEmpty() || m_closuresMadeAgain.empty())
        return false;

    Liveness liveness(m_function, m_analyses.cfg(), m_analyses.useLists(), { .trackedValues = &holders });
    for (const ClosureMadeAgain& madeAgain : m_closuresMadeAgain) {
        if (!capturesEnvironment(m_function.at(madeAgain.closure)))
            continue;
        ProgramPoint point = m_analyses.useLists().pointOf(madeAgain.use);
        Liveness::LocalCalc calc(liveness, point.block);
        for (uint32_t index = m_function.at(point.block).values.size(); index > point.index;)
            calc.execute(--index);

        bool isOtherLive = false;
        calc.forEachLive([&](ValueID holder) {
            isOtherLive |= holder != madeAgain.closure;
        });
        if (isOtherLive)
            return true;
    }
    return false;
}

Vector<ProgramPoint, 4> EnvironmentMaterialization::findMaterializationPoints() const
{
    Vector<ProgramPoint, 4> points;
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const auto& values = m_function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (requiresHeapEnvironment(values[index]))
                points.append({ block, index });
        }
    }
    return points;
}

std::optional<BitVector> EnvironmentMaterialization::findFrameHolders()
{
    const EscapeAnalysis& escape = m_analyses.escape();
    std::optional<ObjectID> environment = escape.objectDefinedBy(m_environment);
    if (!environment || escape.aliasesOf(*environment).size() != 1)
        return std::nullopt;

    BitVector holders(m_function.values.size());
    BitVector visited(escape.numberOfObjects());
    Vector<ObjectID, 8> worklist;
    for (size_t index : m_frameClosures) {
        ValueID closure(index);
        if (!capturesEnvironment(m_function.at(closure)))
            continue;
        std::optional<ObjectID> object = escape.objectDefinedBy(closure);
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
