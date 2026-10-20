/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsInternal.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class PlannedStretchRanges {
    WTF_MAKE_NONCOPYABLE(PlannedStretchRanges);
public:
    PlannedStretchRanges(const Function& function, std::span<const StretchRange> ranges, const auto& cuts, const auto& splitEdges)
        : m_function(function)
        , m_ranges(ranges)
        , m_rangesOfBlock(FillWith { }, function.blocks.size(), RangesOfBlock { })
    {
        for (uint32_t index = 0; index < ranges.size(); ++index) {
            RangesOfBlock& rangesOfBlock = m_rangesOfBlock[ranges[index].block.index()];
            if (!rangesOfBlock.count)
                rangesOfBlock.first = index;
            ++rangesOfBlock.count;
        }

        uint32_t numberOfBlocks = function.blocks.size();
        Vector<uint32_t> plannedSizes;
        plannedSizes.reserveInitialCapacity(numberOfBlocks);
        m_plannedStarts.reserveInitialCapacity(numberOfBlocks);
        for (uint32_t index = 0; index < numberOfBlocks; ++index) {
            BlockID block(index);
            plannedSizes.append(static_cast<uint32_t>(function.at(block).values.size()));
            m_plannedStarts.append(ProgramPoint::atHeadOf(block));
        }
        for (const auto& cut : cuts) {
            plannedSizes[cut.point.block.index()] = cut.point.index + static_cast<uint32_t>(function.at(cut.blockAfter).values.size());
            m_plannedStarts[cut.blockAfter.index()] = cut.point;
        }
        for (const auto& split : splitEdges)
            m_plannedStarts[split.block.index()] = ProgramPoint { split.edge.from, plannedSizes[split.edge.from.index()] };
    }

    bool isInStretchBefore(ProgramPoint plannedPoint) const
    {
        RangesOfBlock rangesOfBlock = m_rangesOfBlock[plannedPoint.block.index()];
        std::span<const StretchRange> ranges = m_ranges.subspan(rangesOfBlock.first, rangesOfBlock.count);
        auto range = std::ranges::lower_bound(ranges, plannedPoint.index, { }, &StretchRange::end);
        return range != ranges.end() && (range->begin < plannedPoint.index || range->end == plannedPoint.index);
    }

    bool endsInStretch(BlockID block) const
    {
        return isInStretchBefore(plannedPointOf(pointBeforeTerminatorOf(m_function, block)));
    }

    bool isDefinedInStretch(const CFG& cfg, const ValueLocation& definition) const
    {
        switch (definition.placement) {
        case ValuePlacement::Parameter:
            return std::ranges::any_of(cfg.incomingEdges(definition.block), [&](Edge edge) {
                return !edge.isHandlerEdge() && endsInStretch(edge.from);
            });
        case ValuePlacement::InBlock:
            return isInStretchBefore(plannedPointOf({ definition.block, definition.index + 1 }));
        case ValuePlacement::Detached:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

private:
    struct RangesOfBlock {
        uint32_t first { 0 };
        uint32_t count { 0 };
    };

    ProgramPoint plannedPointOf(ProgramPoint point) const
    {
        ProgramPoint start = m_plannedStarts[point.block.index()];
        return { start.block, start.index + point.index };
    }

    const Function& m_function;
    std::span<const StretchRange> m_ranges;
    Vector<RangesOfBlock> m_rangesOfBlock;
    Vector<ProgramPoint> m_plannedStarts;
};

struct AdoptedProofPoints {
    Vector<ProgramPoint> pointsAfter;
    BitVector startsStretch;
};

} // anonymous namespace

static AdoptedProofPoints findAdoptedProofPoints(FunctionAnalyses& analyses, std::span<const ValueID> proofs, const PlannedStretchRanges* ranges)
{
    AdoptedProofPoints result;
    if (proofs.empty())
        return result;

    const CFG& cfg = analyses.cfg();
    const UseLists& useLists = analyses.useLists();
    result.pointsAfter.reserveInitialCapacity(proofs.size());
    for (ValueID proof : proofs) {
        if (ranges && !ranges->isDefinedInStretch(cfg, useLists.locationOf(proof)))
            result.startsStretch.set(result.pointsAfter.size());
        result.pointsAfter.append(useLists.pointAfter(proof));
    }
    return result;
}

static bool haveSamePosition(const PlannedTest& a, const PlannedTest& b)
{
    if (a.position != b.position)
        return false;
    switch (a.position) {
    case PlannedTestPosition::BeforeUse:
        return a.point == b.point;
    case PlannedTestPosition::OnEdge:
        return a.edge == b.edge;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueID useOf(const TestPlacementPlan& plan, const PlannedTest& planned)
{
    return plan.sites.sites[plan.requirements[planned.requirement].site].use;
}

void TestPlacer::adoptExistingProofs()
{
    m_adoptedProofs.reserveInitialCapacity(m_plan.proofs.size());
    for (const ExistingProof& proof : m_plan.proofs) {
        switch (proof.source) {
        case ProofSource::TakenEdge:
        case ProofSource::SwitchCase:
            m_adoptedProofs.append(adoptEdgeProof(proof));
            break;
        case ProofSource::GuardValue:
        case ProofSource::Allocation:
            m_adoptedProofs.append(proof.value);
            break;
        }
    }
}

ValueID TestPlacer::adoptEdgeProof(const ExistingProof& proof)
{
    BlockID targetBefore = targetOf(m_function, proof.edge).block;
    ValueID adopted = ensureEdgeResult(m_builder, m_context.analyses, proof.edge, testedOperandIndex);
    BlockID target = targetOf(m_function, proof.edge).block;
    if (target != targetBefore)
        m_splitEdges.append(SplitEdge { proof.edge, target });
    return adopted;
}

void TestPlacer::defineAdoptedProofs()
{
    bool savesShapeEpoch = m_plan.hasRenewal();
    if (m_adoptedProofs.isEmpty() && !savesShapeEpoch)
        return;

    std::optional<PlannedStretchRanges> ranges;
    if (savesShapeEpoch)
        ranges.emplace(m_function, m_plan.ranges.span(), m_cuts, m_splitEdges);
    AdoptedProofPoints points = findAdoptedProofPoints(m_context.analyses, m_adoptedProofs.span(), ranges ? &*ranges : nullptr);

    // A read inside a stretch could follow the write that made an earlier proof stale: the epoch is read where none can be stale.
    auto saveShapeEpoch = [&](Origin origin, ProgramPoint point) {
        m_builder.setOrigin(origin);
        m_epochReads.append(m_builder.insertBefore(point, IRBuilder::make(Opcode::GetShapeEpoch)));
    };

    // A value of a covered block does not reach the handler of that block, so each handler on the way out of the entry block has a read of its own.
    BitVector hasBaseEpochRead;
    BlockID baseBlock = savesShapeEpoch ? entryBlock : BlockID();
    while (baseBlock && hasBaseEpochRead.add(baseBlock.index())) {
        const Block& block = m_function.at(baseBlock);
        saveShapeEpoch(block.terminator.origin(), ProgramPoint { baseBlock, indexAfterCaughtValues(m_function, block) });
        baseBlock = block.handler ? block.handler->block : BlockID();
    }

    BitVector isFollowedByEpochRead;
    for (uint32_t index = 0; index < m_adoptedProofs.size(); ++index) {
        TestConditionID condition = m_plan.proofs[index].condition;
        ValueID adopted = m_adoptedProofs[index];
        if (m_definitions[condition.index()].contains(adopted))
            continue;

        Origin origin = m_function.at(adopted).origin();
        ValueID definition = adopted;
        if (m_isDefinition.get(adopted.index())) {
            m_builder.setOrigin(origin);
            definition = m_builder.insertBefore(points.pointsAfter[index], IRBuilder::make(Opcode::AllGuards, { adopted }));
        }
        addDefinition(condition, definition);
        if (points.startsStretch.get(index) && isFollowedByEpochRead.add(adopted.index()))
            saveShapeEpoch(origin, points.pointsAfter[index]);
    }

    for (uint32_t index = 0; ranges && index < m_plan.tests.size(); ++index) {
        const PlannedTest& planned = m_plan.tests[index];
        if (!isPlaced(planned.stretch) || (index && haveSamePosition(m_plan.tests[index - 1], planned)))
            continue;

        BlockID block;
        bool continuesStretch = false;
        switch (planned.position) {
        case PlannedTestPosition::BeforeUse: {
            const Cut* cut = findCut(planned.point);
            RELEASE_ASSERT(cut);
            block = cut->blockBefore;
            continuesStretch = ranges->isInStretchBefore(planned.point);
            break;
        }
        case PlannedTestPosition::OnEdge:
            block = ensureSplit(planned.edge);
            continuesStretch = ranges->endsInStretch(block);
            break;
        }
        if (!continuesStretch)
            saveShapeEpoch(m_function.at(useOf(m_plan, planned)).origin(), pointBeforeTerminatorOf(m_function, block));
    }

    m_builder.applyInsertions();
}

void TestPlacer::emitTests()
{
    defineAdoptedProofs();

    Vector<uint32_t, 4> run;
    for (uint32_t index = 0; index < m_plan.tests.size();) {
        const PlannedTest& first = m_plan.tests[index];
        run.shrink(0);
        for (; index < m_plan.tests.size() && haveSamePosition(first, m_plan.tests[index]); ++index) {
            if (isPlaced(m_plan.tests[index].stretch))
                run.append(index);
        }
        if (run.isEmpty())
            continue;

        RELEASE_ASSERT(m_cloner);
        TestChain chain;
        switch (first.position) {
        case PlannedTestPosition::BeforeUse: {
            const Cut* cut = findCut(first.point);
            RELEASE_ASSERT(cut);
            chain = TestChain { cut->blockBefore, Target { cut->blockAfter, { }, { } }, Target { m_cloner->cloneOf(cut->blockAfter), { }, { } }, m_function.at(cut->blockAfter).frequency };
            break;
        }
        case PlannedTestPosition::OnEdge: {
            BlockID block = ensureSplit(first.edge);
            Target next = m_function.at(block).terminator.next();
            chain = TestChain { block, next, Target { m_cloner->cloneOf(next.block), next.arguments, { } }, m_function.at(next.block).frequency };
            break;
        }
        }
        RELEASE_ASSERT(chain.failedTarget.block);
        emitChain(WTF::move(chain), run.span());
    }
}

void TestPlacer::emitChain(TestChain&& chain, std::span<const uint32_t> plannedTests)
{
    Terminator& jump = m_function.at(chain.block).terminator;
    RELEASE_ASSERT(jump.kind == TerminatorKind::Jump && jump.next().block == chain.passedTarget.block);
    jump = Terminator { };

    m_builder.setHandler(std::nullopt);
    m_builder.moveToEndOf(chain.block);
    for (uint32_t plannedTest : plannedTests) {
        switch (m_plan.tests[plannedTest].kind) {
        case PlannedTestKind::Test:
            emitTest(chain, plannedTest);
            break;
        case PlannedTestKind::Renewal:
            emitRenewal(chain, plannedTest);
            break;
        }
    }
    m_builder.emitJump(chain.passedTarget.block, WTF::move(chain.passedTarget.arguments));
}

void TestPlacer::emitTest(const TestChain& chain, uint32_t plannedTest)
{
    const PlannedTest& planned = m_plan.tests[plannedTest];
    m_builder.setOrigin(m_function.at(useOf(m_plan, planned)).origin());
    ValueID test = m_builder.append(createTest(planned));
    addDefinition(planned.condition, branchOn(chain, plannedTest, test).provenWhenPassed);
}

void TestPlacer::emitRenewal(const TestChain& chain, uint32_t plannedTest)
{
    const PlannedTest& planned = m_plan.tests[plannedTest];
    m_builder.setOrigin(m_function.at(useOf(m_plan, planned)).origin());

    Vector<ValueID, 4> staleGuards;
    for (TestConditionID condition : planned.renewed)
        staleGuards.append(representativeOf(condition));
    ValueID allStaleGuards = m_builder.appendAllGuards(staleGuards.span());

    RELEASE_ASSERT(!m_epochReads.isEmpty());
    ValueID test = m_builder.append(Opcode::IsShapeEpochCurrent, { m_epochReads.first(), allStaleGuards });
    ValueID renewedGuard = branchOn(chain, plannedTest, test).provenWhenPassed;
    ++m_numberOfRenewals;

    for (TestConditionID condition : planned.renewed)
        addDefinition(condition, renewedGuard);
    readShapeEpoch();
}

auto TestPlacer::branchOn(const TestChain& chain, uint32_t plannedTest, ValueID test) -> EmittedTest&
{
    BlockID block = m_builder.currentBlock();
    const Value& value = m_function.at(test);
    ValueID subject = value.numberOfOperands() ? value.operand(testedOperandIndex) : ValueID();

    IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(test, testedOperandIndex, chain.frequency, TestPlacementModel::frequencyOfGenericCopy(chain.frequency));
    m_builder.moveToEndOf(branch.notTaken);
    m_builder.emitJump(chain.failedTarget.block, Arguments(chain.failedTarget.arguments));
    m_builder.moveToEndOf(branch.taken);

    m_emittedTests.append(EmittedTest { plannedTest, block, subject, branch.taken, branch.notTaken, branch.provenOnTaken, branch.provenOnNotTaken });
    return m_emittedTests.last();
}

Value TestPlacer::createTest(const PlannedTest& planned)
{
    RELEASE_ASSERT(planned.condition == requirementAt(planned.requirement).condition);
    Value test = m_plan.sites.tests[requirementAt(planned.requirement).test];
    std::optional<unsigned> guardIndex = OperandPosition::guard(test.opcode);

    if (planned.position == PlannedTestPosition::OnEdge) {
        uint32_t numberOfHoistedOperands = 0;
        for (uint32_t operandIndex = 0; operandIndex < test.numberOfOperands(); ++operandIndex) {
            if (guardIndex != operandIndex)
                test.setOperand(operandIndex, planned.operands[numberOfHoistedOperands++]);
        }
    }

    Vector<ValueID, 4> guards;
    forEachTestPrerequisite(m_plan.requirements.span(), *m_plan.conditions, planned.requirement, [&](const TestPrerequisite& prerequisite) {
        ValueID proof = representativeOf(prerequisite.condition);
        if (prerequisite.narrowedOperand)
            test.setOperand(*prerequisite.narrowedOperand, proof);
        else
            guards.appendIfNotContains(proof);
    });
    if (!guards.isEmpty())
        test.setOperand(*guardIndex, m_builder.appendAllGuards(guards.span()));
    return test;
}

void TestPlacer::readShapeEpoch()
{
    m_epochReads.append(m_builder.append(Opcode::GetShapeEpoch, { }));
}

void TestPlacer::addDefinition(TestConditionID condition, ValueID proof)
{
    Vector<ValueID, 2>& definitions = m_definitions[condition.index()];
    if (definitions.contains(proof))
        return;
    if (m_isDefinition.get(proof.index()))
        proof = wrapGuard(proof);
    m_isDefinition.set(proof.index());
    definitions.append(proof);
}

ValueID TestPlacer::representativeOf(TestConditionID condition) const
{
    const Vector<ValueID, 2>& definitions = m_definitions[condition.index()];
    RELEASE_ASSERT(!definitions.isEmpty());
    return definitions.first();
}

ValueID TestPlacer::wrapGuard(ValueID guard)
{
    return m_builder.append(Opcode::AllGuards, { guard });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
