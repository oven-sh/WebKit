/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsInternal.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

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
    if (m_adoptedProofs.isEmpty())
        return;

    // An insertion ends the life of the use lists.
    const UseLists& useLists = m_context.analyses.useLists();
    Vector<ProgramPoint> pointsAfter;
    pointsAfter.reserveInitialCapacity(m_adoptedProofs.size());
    for (ValueID proof : m_adoptedProofs)
        pointsAfter.append(useLists.pointAfter(proof));

    for (uint32_t index = 0; index < m_adoptedProofs.size(); ++index) {
        TestConditionID condition = m_plan.proofs[index].condition;
        ValueID adopted = m_adoptedProofs[index];
        if (m_definitions[condition.index()].contains(adopted))
            continue;

        ValueID definition = adopted;
        if (m_isDefinition.get(adopted.index())) {
            m_builder.setOrigin(m_function.at(adopted).origin());
            definition = m_builder.insertBefore(pointsAfter[index], IRBuilder::make(Opcode::AllGuards, { adopted }));
        }
        addDefinition(condition, definition);
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

        RELEASE_ASSERT(m_copy);
        TestChain chain;
        switch (first.position) {
        case PlannedTestPosition::BeforeUse: {
            const Cut* cut = findCut(first.point);
            RELEASE_ASSERT(cut);
            chain = TestChain { cut->blockBefore, Target { cut->blockAfter, { }, { } }, Target { m_copy->copyOf(cut->blockAfter), { }, { } }, m_function.at(cut->blockAfter).frequency, true };
            break;
        }
        case PlannedTestPosition::OnEdge: {
            BlockID block = ensureSplit(first.edge);
            Target next = m_function.at(block).terminator.next();
            chain = TestChain { block, next, Target { m_copy->copyOf(next.block), next.arguments, { } }, m_function.at(next.block).frequency, false };
            break;
        }
        }
        RELEASE_ASSERT(chain.failedTarget.block);
        emitChain(WTF::move(chain), run.span());
    }
}

void TestPlacer::emitChain(TestChain&& chain, std::span<const uint32_t> plannedTests)
{
    Terminator& jump = m_function.edit(chain.block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
    RELEASE_ASSERT(jump.kind == TerminatorKind::Jump && jump.next().block == chain.passedTarget.block);
    jump = Terminator { };

    m_builder.setHandler(std::nullopt);
    m_builder.moveToEndOf(chain.block);
    m_builder.emitFailureOfPredictions(Target { chain.failedTarget.block, Arguments(chain.failedTarget.arguments), { } });
    for (uint32_t plannedTest : plannedTests.first(plannedTests.size() - 1))
        emitTest(chain, plannedTest, IsLastOfChain::No);
    emitTest(chain, plannedTests.back(), IsLastOfChain::Yes);
    if (!chain.isOnlyWayToPassedTarget)
        m_builder.emitJump(chain.passedTarget.block, WTF::move(chain.passedTarget.arguments));
}

void TestPlacer::emitTest(const TestChain& chain, uint32_t plannedTest, IsLastOfChain isLast)
{
    const PlannedTest& planned = m_plan.tests[plannedTest];
    m_builder.setOrigin(m_function.at(useOf(m_plan, planned)).origin());
    ValueID test = m_builder.append(createTest(planned));
    addDefinition(planned.condition, branchOn(chain, plannedTest, test, isLast).provenWhenPassed);
}

// No block is made that only jumps: SimplifyControlFlow would merge it away in the same round, and every pass in between would walk it.
auto TestPlacer::branchOn(const TestChain& chain, uint32_t plannedTest, ValueID test, IsLastOfChain isLast) -> EmittedTest&
{
    BlockID block = m_builder.currentBlock();

    bool provesNothingWhenFailed = provesForStretch(provesOf(m_function.at(test).opcode));
    bool entersPassedTarget = isLast == IsLastOfChain::Yes && chain.isOnlyWayToPassedTarget;
    IRBuilder::ProvenBranchTargets targets;
    if (entersPassedTarget)
        targets.enteredOnlyWhenTaken = chain.passedTarget.block;
    if (provesNothingWhenFailed)
        targets.whenNotTaken = Target { chain.failedTarget.block, Arguments(chain.failedTarget.arguments), { } };
    IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(test, testedOperandIndex, chain.frequency, TestPlacementModel::frequencyOfGenericCopy(chain.frequency), WTF::move(targets));
    if (!provesNothingWhenFailed) {
        m_builder.moveToEndOf(branch.notTaken);
        m_builder.emitJump(chain.failedTarget.block, Arguments(chain.failedTarget.arguments));
    }
    if (!entersPassedTarget)
        m_builder.moveToEndOf(branch.taken);

    m_emittedTests.append(EmittedTest { plannedTest, block, branch.taken, branch.notTaken, branch.provenOnTaken, branch.provenOnNotTaken });
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
        if (!prerequisite.narrowedOperand) {
            guards.appendIfNotContains(proof);
            return;
        }
        if (std::optional<TypeID> castType = findTypeOfOperand(test, *prerequisite.narrowedOperand, proof))
            proof = m_builder.append(makeAssumeType(proof, *castType));
        test.setOperand(*prerequisite.narrowedOperand, proof);
    });
    if (!guards.isEmpty())
        test.setOperand(*guardIndex, m_builder.appendAllGuards(guards.span()));
    keepOnlyWhatOperandsProve(test);
    return test;
}

// An operand that has to be an integer was one at the site: by its own type and the test that it passed. The value that passed the test may be the one in front of a cast.
std::optional<TypeID> TestPlacer::findTypeOfOperand(const Value& test, uint32_t operandIndex, ValueID passed) const
{
    TypeArena& types = m_context.types;
    const Type& passedType = types.at(m_function.at(passed).type);
    const Type& operandType = types.at(m_function.at(test.operand(operandIndex)).type);
    bool hasToBeInteger = test.operandKind(operandIndex) == OperandKind::Integer;
    if (!hasToBeInteger && operandIndex != testedOperandIndex)
        return std::nullopt;
    if (hasToBeInteger ? passedType.hasOnlyPrimitives(Primitive::Integer) : passedType.isSubsetOf(operandType, m_context.program))
        return std::nullopt;
    Type both = passedType.intersectWith(operandType, types);
    if (both.isNever() || (hasToBeInteger && !both.hasOnlyPrimitives(Primitive::Integer)))
        return std::nullopt;
    return types.intern(both);
}

// The site was planned with the types of its own operands. The value that stands for an operand here may be the one in front of a cast, of which less is known.
void TestPlacer::keepOnlyWhatOperandsProve(Value& test) const
{
    if (!Value::opcodesWithKeyIsIntegral.contains(test.opcode) || !test.keyIsIntegral())
        return;
    ValueID key = OperandPosition::property(test.opcode) ? test.propertyOperand() : test.positionOperand();
    if (!m_context.program.typeOf(m_function, key).hasOnlyPrimitives(Primitive::NumberWithoutFraction))
        test.setKeyIsIntegral(false);
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
