/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTFlowApply.h"
#include "AOTRegionCloner.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct DuplicationPlan {
    VirtualObjectID object;
    BlockID merge;
    Vector<Edge, 2> cellEdges;
};

} // anonymous namespace

static bool allocatesOnEveryPath(const Candidate& candidate)
{
    return candidate.isAbandoned || (candidate.numberOfBuilds && !candidate.diesVirtual);
}

static void resetAnalysis(AllocationSinking& sinking)
{
    for (uint32_t index = 0; index < sinking.numberOfCandidates(); ++index) {
        Candidate& candidate = sinking.at(VirtualObjectID(index));
        if (candidate.abandonedBy == LimitID::None && !candidate.isLeftOut)
            candidate.isAbandoned = false;
        candidate.isBuiltBeforeBranches = false;
    }
    sinking.decisions() = SinkingDecisions { };
}

static Vector<VirtualObjectID, 4> collectReachableObjects(const SinkingState& state, VirtualObjectID root)
{
    Vector<VirtualObjectID, 4> objects;
    if (!state.find(root))
        return objects;

    objects.append(root);
    for (size_t next = 0; next < objects.size(); ++next) {
        for (const HeldObject& entry : state.find(objects[next])->heldObjects) {
            if (state.find(entry.object))
                objects.appendIfNotContains(entry.object);
        }
    }
    return objects;
}

static Vector<BlockID, 4> collectBlocksEnteredFromOutside(const Function& function, const CFG& cfg, std::span<const BlockID> region)
{
    BitVector isInRegion(function.blocks.size());
    for (BlockID block : region)
        isInRegion.quickSet(block.index());
    Vector<BlockID, 4> entries;
    for (BlockID block : region) {
        bool isEnteredFromOutside = std::ranges::any_of(cfg.predecessors(block), [&](BlockID predecessor) {
            return !isInRegion.quickGet(predecessor.index());
        });
        if (isEnteredFromOutside)
            entries.append(block);
    }
    return entries;
}

Vector<Edge, 2> AllocationSinking::collectCellEdges(VirtualObjectID object, BlockID merge)
{
    std::span<const Edge> incomingEdges = m_analyses.cfg().incomingEdges(merge);
    if (std::ranges::any_of(incomingEdges, &Edge::isHandlerEdge))
        return { };

    Vector<Edge, 2> cellEdges;
    bool hasOtherShape = false;
    for (Edge edge : incomingEdges) {
        const SinkingState* head = headOf(edge.from);
        if (!head)
            continue;

        const Block& source = m_function.at(edge.from);
        prepare(edge.from);
        SinkingState state = *head;
        ObjectInterpreter interpreter(*this, SinkingMode::Analyze, state);
        for (ValueID id : source.values)
            interpreter.execute(id);
        interpreter.executeTerminator(source.terminator);
        OutgoingEdge outgoing = outgoingEdgeOf(m_function, edge);
        interpreter.enterEdge(outgoing, &source.terminator, nullptr);

        const ObjectState* entering = state.isReachable ? state.find(object) : nullptr;
        if (!entering)
            continue;
        // What may be a cell on a way that seldom runs is built there.
        BlockFrequency frequency = m_function.at(edge.from).frequency;
        bool isRarer = frequency != m_function.at(merge).frequency && rarerOf(frequency, m_function.at(merge).frequency) == frequency;
        if (entering->isCell() || (isRarer && entering->hasCell()))
            cellEdges.append(edge);
        else
            hasOtherShape = true;
    }
    if (!hasOtherShape)
        return { };
    return cellEdges;
}

Vector<BlockID> AllocationSinking::collectLiveRange(VirtualObjectID object, BlockID merge)
{
    const SinkingState* headOfMerge = headOf(merge);
    if (!headOfMerge)
        return { };
    Vector<VirtualObjectID, 4> objects = collectReachableObjects(*headOfMerge, object);
    if (objects.isEmpty())
        return { };

    const CFG& cfg = m_analyses.cfg();
    Vector<BlockID> region { merge };
    BitVector isInRegion(m_function.blocks.size());
    isInRegion.quickSet(merge.index());
    uint64_t numberOfValues = 0;
    for (size_t next = 0; next < region.size(); ++next) {
        BlockID block = region[next];
        numberOfValues += m_function.at(block).values.size() + 1;
        if (exceeds(LimitID::MaxDuplicatedTailValues, numberOfValues, at(object).definition))
            return { };

        for (BlockID successor : cfg.successors(block)) {
            if (successor == merge)
                return { };
            const SinkingState* head = headOf(successor);
            bool isLive = head && std::ranges::any_of(objects, [&](VirtualObjectID member) {
                return !!head->find(member);
            });
            if (isLive && !isInRegion.quickSet(successor.index()))
                region.append(successor);
        }
    }

    AOT::sort(region, { }, [&](BlockID block) {
        return cfg.reversePostOrderIndex(block);
    });
    return region;
}

bool AllocationSinking::paysWithoutEdges(VirtualObjectID object, std::span<const Edge> edges)
{
    BitVector allocatedOnEveryPath(m_candidates.size());
    for (size_t index = 0; index < m_candidates.size(); ++index) {
        if (allocatesOnEveryPath(m_candidates[index]))
            allocatedOnEveryPath.quickSet(index);
    }

    resetAnalysis(*this);
    m_ignoredEdges.clear();
    m_ignoredEdges.append(edges);
    analyze();
    m_ignoredEdges.clear();

    const SinkingState* head = headOf(targetOf(m_function, edges.front()).block);
    if (!head)
        return false;
    Vector<VirtualObjectID, 4> objects = collectReachableObjects(*head, object);
    return std::ranges::any_of(objects, [&](VirtualObjectID member) {
        return allocatedOnEveryPath.quickGet(member.index()) && !allocatesOnEveryPath(at(member));
    });
}

void AllocationSinking::duplicate(IRBuilder& builder, std::span<const BlockID> region, std::span<const Edge> cellEdges)
{
    RegionCloner cloner(builder, m_function);
    cloner.cloneBlocks(region, [](ValueID, Value&) { });
    for (Edge edge : cellEdges) {
        const Target& target = targetOf(m_function, edge);
        BlockID clone = cloner.cloneOf(target.block);
        builder.redirectEdge(edge, clone, Arguments(target.arguments));
    }
    cloner.repairSSA(m_analyses);
}

// A callee that hands over the parts of its result hands over the object itself on some of its ways. The caller puts the parts together only to have one value where the two meet.
// A way that seldom runs and makes the object counts the same, where it is a read or a test behind the meeting that wants the cell.
bool AllocationSinking::meetsObjectMadeOnAnotherWay(const Candidate& join) const
{
    if (join.isLeftOut || m_function.at(join.block).frequency != BlockFrequency::Normal)
        return false;
    bool isMadeOnEveryPath = std::ranges::any_of(m_candidates, [&](const Candidate& candidate) {
        return candidate.family == join.family && allocatesOnEveryPath(candidate);
    });
    if (!isMadeOnEveryPath)
        return false;
    std::optional<Opcode> cause = join.firstCause ? join.firstCause->opcode : std::nullopt;
    bool isBuiltForRead = cause && (interpretedOf(*cause) == Interpretation::Read || interpretedOf(*cause) == Interpretation::Test);
    size_t parameterIndex = m_function.at(join.block).parameters.find(join.definition);
    return std::ranges::any_of(m_analyses.cfg().incomingEdges(join.block), [&](Edge edge) {
        ValueID incoming = valuePassedOn(m_function, edge, parameterIndex);
        if (!incoming)
            return false;
        return (isBuiltForRead && m_function.at(edge.from).frequency != BlockFrequency::Normal) || m_function.at(sameObjectThroughCasts(m_function, incoming)).opcode == Opcode::ResultAt;
    });
}

// Whether the list fits the storage that its readers expect is asked where it is made, of each element that may be no such number. Behind that there are two lists, and of each the answer is known:
// the one that is made of numbers, and one for every value, on a way that seldom runs.
static void testElementsOfList(IRBuilder& builder, const Program& program, Function& function, ProgramPoint point)
{
    Value list = function.at(function.at(point.block).values[point.index]);
    TypeID typeOfList = list.type;
    Type numbers = typeOfNumbersIn(arrayStorageAtAllocation(program, program.at(typeOfList)));
    IRBuilder::Expansion expansion = builder.beginExpansion(point);
    BlockID holdsOtherValue;

    Vector<ValueID, 4> elements;
    for (ValueID element : list.elementsOperands()) {
        if (program.typeOf(function, element).isSubsetOf(numbers, program)) {
            elements.append(element);
            continue;
        }
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(element, numbers), OperandIndex::TypeTest::value, BlockFrequency::Normal, BlockFrequency::Rare);
        elements.append(branch.provenOnTaken);
        if (!holdsOtherValue)
            holdsOtherValue = builder.addBlock(BlockFrequency::Rare);
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(holdsOtherValue);
        builder.moveToEndOf(branch.taken);
    }
    Value listOfNumbers = list;
    listOfNumbers.setContents(builder.types().intern(program.at(list.contents()).intersectWith(numbers, builder.types())));
    listOfNumbers.setElementsOperands(elements.span());
    builder.endExpansion(expansion, builder.append(makeAssumeType(builder.append(WTF::move(listOfNumbers)), typeOfList)));

    if (!holdsOtherValue)
        return;
    builder.moveToEndOf(holdsOtherValue);
    list.setContents(TypeTable::anything);
    builder.emitJump(expansion.continuation, { builder.append(makeAssumeType(builder.append(WTF::move(list)), typeOfList)) });
}

bool AllocationSinking::testElementsOfLists(IRBuilder& builder)
{
    Vector<ProgramPoint, 2> points;
    for (ValueID literal : m_testedListsThatMayNotFit) {
        const Value& list = m_function.at(literal);
        ValueLocation location = m_analyses.useLists().locationOf(literal);
        if (list.opcode != Opcode::NewArray || location.placement != ValuePlacement::InBlock)
            continue;
        if (list.elementsOperands().empty() || !list.holes().empty() || list.capacityOperand())
            continue;
        // Only an allocation is a guard of what it makes.
        bool isGuard = std::ranges::any_of(m_analyses.useLists().usesOf(literal), [&](const Use& use) {
            return use.kind == Use::Kind::Operand && m_function.at(use.user).operandKind(use.operandIndex) == OperandKind::Guard;
        });
        if (isGuard)
            continue;
        points.append(ProgramPoint { location.block, location.index });
    }
    AOT::sort(points, [](const ProgramPoint& a, const ProgramPoint& b) {
        return a.block != b.block ? a.block > b.block : a.index > b.index;
    });
    for (ProgramPoint point : points)
        testElementsOfList(builder, m_program, builder.function(), point);
    return !points.isEmpty();
}

// A block that only hands its parameter on to the next block where values meet hides from that one which way brings the parts and which the whole.
bool AllocationSinking::handOnToNextJoin(IRBuilder& builder, const Candidate& join)
{
    const Block& block = m_function.at(join.block);
    if (!block.values.isEmpty() || block.parameters.size() != 1 || block.terminator.kind != TerminatorKind::Jump || block.terminator.next().block == join.block)
        return false;
    bool isOnlyHandedOn = std::ranges::all_of(m_analyses.useLists().usesOf(join.definition), [&](const Use& use) {
        return use.kind == Use::Kind::EdgeArgument && use.block == join.block;
    });
    Vector<Edge, 4> edges(m_analyses.cfg().incomingEdges(join.block));
    bool takesArguments = std::ranges::all_of(edges, [&](Edge edge) {
        return !edge.isHandlerEdge() && numberOfExplicitArguments(m_function, edge) == 1;
    });
    if (!isOnlyHandedOn || !takesArguments)
        return false;

    Target next = block.terminator.next();
    for (Edge edge : edges) {
        ValueID incoming = valuePassedOn(m_function, edge, 0);
        Vector<ValueID, 4> arguments(next.arguments.size(), [&](size_t index) {
            return next.arguments[index] == join.definition ? incoming : next.arguments[index];
        });
        builder.redirectEdge(edge, next.block, Arguments(arguments.span()));
    }
    builder.emptyUnreachableBlocks();
    return true;
}

bool AllocationSinking::duplicateLiveRange(IRBuilder& builder)
{
    Vector<DuplicationPlan, 2> plans;
    for (uint32_t index = 0; index < m_candidates.size(); ++index) {
        const Candidate& candidate = m_candidates[index];
        if (candidate.abandonedBy != LimitID::None)
            continue;
        if (candidate.isJoin ? meetsObjectMadeOnAnotherWay(candidate) : !!candidate.mergeBlock)
            plans.append(DuplicationPlan { VirtualObjectID(index), candidate.isJoin ? candidate.block : candidate.mergeBlock, { } });
    }
    if (plans.isEmpty() || hasSpentItsWork())
        return false;
    for (const DuplicationPlan& plan : plans) {
        if (at(plan.object).isJoin && handOnToNextJoin(builder, at(plan.object)))
            return true;
    }

    Vector<Candidate> savedCandidates = m_candidates;
    SinkingDecisions savedDecisions = m_decisions;
    auto restoreAnalysis = [&] {
        m_candidates = savedCandidates;
        m_decisions = savedDecisions;
    };
    auto collectClonableRange = [&](const DuplicationPlan& plan) {
        Vector<BlockID> region = collectLiveRange(plan.object, plan.merge);
        if (region.isEmpty())
            return region;
        // A way that seldom runs leaves the range where it builds the object, and may come back into the range further down.
        Vector<BlockID, 4> entries = collectBlocksEnteredFromOutside(m_function, m_analyses.cfg(), region.span());
        if (RegionCloner::checkCanDuplicate(m_analyses, region.span(), entries.span()) != CloneCheck::CanClone)
            region.clear();
        return region;
    };

    resetAnalysis(*this);
    startWorkOver();
    solve();
    for (DuplicationPlan& plan : plans) {
        plan.cellEdges = collectCellEdges(plan.object, plan.merge);
        if (!plan.cellEdges.isEmpty() && collectClonableRange(plan).isEmpty())
            plan.cellEdges.clear();
    }

    uint64_t numberOfTrials = 0;
    for (const DuplicationPlan& plan : plans) {
        if (plan.cellEdges.isEmpty())
            continue;
        if (exceeds(LimitID::MaxSinkingRounds, ++numberOfTrials, at(plan.object).definition))
            continue;
        restoreAnalysis();
        if (!paysWithoutEdges(plan.object, plan.cellEdges.span()))
            continue;
        Vector<BlockID> region = collectClonableRange(plan);
        if (region.isEmpty())
            continue;

        duplicate(builder, region.span(), plan.cellEdges.span());
        context().build.statistics().count(StatisticID::TailsDuplicatedForSinking);
        return true;
    }

    restoreAnalysis();
    startWorkOver();
    m_decisions.takeDidChange();
    analyzeUntilNothingChanges();
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
