/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDecidedBranches.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTFlowApply.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTPropertyAccessCache.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <limits>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(size_t, maxComparedBranches, 8);
static constexpr uint32_t noGroup = std::numeric_limits<uint32_t>::max();

static KnownBoolean outcomeOnEdge(Edge edge)
{
    if (edge.isHandlerEdge() || edge.targetIndex > 1)
        return KnownBoolean::Unknown;
    return knownBoolean(!edge.targetIndex);
}

static Vector<KnownBoolean> outcomesBehindOutermostEdges(FunctionAnalyses& analyses, uint32_t numberOfGroups, NOESCAPE const Invocable<void(BlockID, const ScopedLambda<void(uint32_t)>&)> auto& forEachGroupBranchedOn, NOESCAPE const Invocable<uint32_t(BlockID)> auto& groupAskedFor)
{
    const CFG& cfg = analyses.cfg();
    Vector<KnownBoolean> outcomes(FillWith { }, cfg.numNodes(), KnownBoolean::Unknown);
    Vector<KnownBoolean> outcomeInScope(FillWith { }, numberOfGroups, KnownBoolean::Unknown);
    Vector<std::pair<BlockID, uint32_t>, 16> decided;
    analyses.dominators().walkTree([&](BlockID block) {
        std::optional<Edge> edge = cfg.singleIncomingEdge(block);
        KnownBoolean outcome = edge ? outcomeOnEdge(*edge) : KnownBoolean::Unknown;
        if (outcome != KnownBoolean::Unknown) {
            auto decide = [&](uint32_t group) {
                if (outcomeInScope[group] != KnownBoolean::Unknown)
                    return;
                outcomeInScope[group] = outcome;
                decided.append({ block, group });
            };
            forEachGroupBranchedOn(edge->from, ScopedLambda<void(uint32_t)>(decide));
        }
        if (uint32_t group = groupAskedFor(block); group != noGroup)
            outcomes[block.index()] = outcomeInScope[group];
    }, [&](BlockID block) {
        while (!decided.isEmpty() && decided.last().first == block)
            outcomeInScope[decided.takeLast().second] = KnownBoolean::Unknown;
    });
    return outcomes;
}

// The elements are sorted by the key.
static void forEachRunOfEqualKeys(auto elements, const auto& keyOf, NOESCAPE const auto& functor)
{
    for (size_t first = 0; first < elements.size();) {
        size_t end = first + 1;
        while (end < elements.size() && std::invoke(keyOf, elements[end]) == std::invoke(keyOf, elements[first]))
            ++end;
        functor(elements.subspan(first, end - first));
        first = end;
    }
}

static void keepRepeated(auto& sorted, const auto& keyOf)
{
    size_t end = 0;
    forEachRunOfEqualKeys(sorted.span(), keyOf, [&](auto run) {
        if (run.size() < 2)
            return;
        for (const auto& element : run)
            sorted[end++] = element;
    });
    sorted.shrink(end);
}

// Each of the branches has a block, and behind an edge of one of them the test of all of them has the outcome of that edge.
static KnownBoolean outcomeBehindEdges(FunctionAnalyses& analyses, const auto& branches, BlockID block, Vector<KnownBoolean>& outcomesOfFrequentBranches, NOESCAPE const auto& computeOutcomes)
{
    auto compareWithEachBranch = [&] {
        for (const auto& branch : branches) {
            if (branch.block == block)
                continue;
            if (analyses.dominates(Edge { branch.block, 0 }, block))
                return KnownBoolean::True;
            if (analyses.dominates(Edge { branch.block, 1 }, block))
                return KnownBoolean::False;
        }
        return KnownBoolean::Unknown;
    };
    if (branches.size() <= maxComparedBranches)
        return compareWithEachBranch();

    if (outcomesOfFrequentBranches.isEmpty())
        outcomesOfFrequentBranches = computeOutcomes();
    KnownBoolean outcome = outcomesOfFrequentBranches[block.index()];
    if (analyses.context().build.options().verifiesAfterEveryPass)
        RELEASE_ASSERT(outcome == compareWithEachBranch());
    return outcome;
}

static Vector<KnownBoolean> outcomesOfFrequentConditions(FunctionAnalyses& analyses, std::span<const BranchOnValue> branchesByOperand)
{
    const Function& function = analyses.function();
    Vector<uint32_t> groupOfValue(FillWith { }, function.values.size(), noGroup);
    uint32_t numberOfGroups = 0;
    forEachRunOfEqualKeys(branchesByOperand, &BranchOnValue::operand, [&](std::span<const BranchOnValue> branches) {
        if (branches.size() > maxComparedBranches)
            groupOfValue[branches[0].operand.index()] = numberOfGroups++;
    });
    return outcomesBehindOutermostEdges(analyses, numberOfGroups, [&](BlockID block, const ScopedLambda<void(uint32_t)>& functor) {
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch())
            return;
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            if (uint32_t group = groupOfValue[operand.index()]; group != noGroup)
                functor(group);
        });
    }, [&](BlockID block) {
        const Terminator& terminator = function.at(block).terminator;
        return terminator.isConditionalBranch() ? groupOfValue[terminator.conditionOperand().index()] : noGroup;
    });
}

// Nobody edits the function while the branches are asked about, so the table says what the use lists would say of the terminators.
KnownBoolean TypeTestBranches::outcomeFromDominatingBranches(ValueID condition, BlockID block) const
{
    if (m_branchesByOperand.isEmpty())
        return KnownBoolean::Unknown;
    auto branches = std::ranges::equal_range(m_branchesByOperand, condition, { }, &BranchOnValue::operand);
    if (branches.empty())
        return KnownBoolean::Unknown;
    const Terminator& terminator = m_analyses.function().at(block).terminator;
    RELEASE_ASSERT(branches.size() <= maxComparedBranches || (terminator.isConditionalBranch() && terminator.conditionOperand() == condition));
    return outcomeBehindEdges(m_analyses, branches, block, m_outcomesOfFrequentConditions, [&] {
        return outcomesOfFrequentConditions(m_analyses, m_branchesByOperand.span());
    });
}

const Value* typeTestBranchedOn(const Function& function, const Block& block)
{
    if (block.terminator.kind != TerminatorKind::Branch)
        return nullptr;
    const Value& test = function.at(block.terminator.conditionOperand());
    return test.opcode == Opcode::TypeTest ? &test : nullptr;
}

void keepRepeatedTests(FunctionAnalyses& analyses, Vector<BranchOnTypeTest>& branches)
{
    // The identities of values are asked for only where two tests are for one type.
    AOT::stableSort(branches, { }, &BranchOnTypeTest::testedType);
    keepRepeated(branches, &BranchOnTypeTest::testedType);
    for (BranchOnTypeTest& branch : branches)
        branch.subject = analyses.identityOf(branch.subject);
    AOT::stableSort(branches, { }, &BranchOnTypeTest::test);
    // A test that no other branch repeats has nothing to learn from the table.
    keepRepeated(branches, &BranchOnTypeTest::test);
}

TypeTestBranches::TypeTestBranches(FunctionAnalyses& analyses)
    : m_analyses(analyses)
{
    const Function& function = analyses.function();
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch())
            continue;
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            m_branchesByOperand.append(BranchOnValue { operand, block });
        });
        if (const Value* test = typeTestBranchedOn(function, function.at(block))) {
            m_branches.append({ test->valueOperand(), test->testedType(), block });
            m_blocksThatBranchOnTypeTest.append(block);
        }
    }
    AOT::stableSort(m_branchesByOperand, { }, &BranchOnValue::operand);
    keepRepeated(m_branchesByOperand, &BranchOnValue::operand);
    keepRepeatedTests(analyses, m_branches);
    if (!m_branches.isEmpty())
        m_indexOfBranch.fill(0, analyses.function().blocks.size());
    for (uint32_t index = 0; index < m_branches.size(); ++index) {
        m_repeatsTest.set(m_branches[index].block.index());
        m_indexOfBranch[m_branches[index].block.index()] = index;
    }
}

static Vector<KnownBoolean> outcomesOfFrequentTests(FunctionAnalyses& analyses, std::span<const BranchOnTypeTest> branches)
{
    Vector<uint32_t> groupOfBlock(FillWith { }, analyses.cfg().numNodes(), noGroup);
    uint32_t numberOfGroups = 0;
    forEachRunOfEqualKeys(branches, &BranchOnTypeTest::test, [&](std::span<const BranchOnTypeTest> branchesOnTest) {
        if (branchesOnTest.size() <= maxComparedBranches)
            return;
        for (const BranchOnTypeTest& branch : branchesOnTest)
            groupOfBlock[branch.block.index()] = numberOfGroups;
        ++numberOfGroups;
    });
    return outcomesBehindOutermostEdges(analyses, numberOfGroups, [&](BlockID block, const ScopedLambda<void(uint32_t)>& functor) {
        if (uint32_t group = groupOfBlock[block.index()]; group != noGroup)
            functor(group);
    }, [&](BlockID block) {
        return groupOfBlock[block.index()];
    });
}

// What a value is tested for never changes, so behind an edge of one test every other test of the value for the same type has the same outcome.
KnownBoolean TypeTestBranches::outcomeFromDominatingTest(BlockID block) const
{
    const Value* test = m_repeatsTest.get(block.index()) ? typeTestBranchedOn(m_analyses.function(), m_analyses.function().at(block)) : nullptr;
    if (!test)
        return KnownBoolean::Unknown;
    std::tuple tested { m_analyses.identityOf(test->valueOperand()).index(), test->testedType().index() };
    return outcomeBehindEdges(m_analyses, std::ranges::equal_range(m_branches, tested, { }, &BranchOnTypeTest::test), block, m_outcomesOfFrequentTests, [&] {
        return outcomesOfFrequentTests(m_analyses, m_branches.span());
    });
}

bool holdsOnlyKnownFunctions(const Type& type)
{
    return !type.functions.isEmpty() && type.hasOnlyKnownCallees() && !type.primitives && !type.layouts && type.builtins.isEmpty();
}

// No single test dominates the block, and every way to it leaves some test of the value for the type by an edge with the same outcome.
KnownBoolean TypeTestBranches::outcomeOnEveryWay(BlockID block) const
{
    static constexpr unsigned maxBlocksOnTheWays = 32;

    const Value* test = m_repeatsTest.get(block.index()) ? typeTestBranchedOn(m_analyses.function(), m_analyses.function().at(block)) : nullptr;
    if (!test || !holdsOnlyKnownFunctions(m_analyses.program().at(test->testedType())))
        return KnownBoolean::Unknown;
    std::tuple tested { m_analyses.identityOf(test->valueOperand()).index(), test->testedType().index() };
    auto branchesOnTest = std::ranges::equal_range(m_branches, tested, { }, &BranchOnTypeTest::test);
    if (branchesOnTest.size() < 3)
        return KnownBoolean::Unknown;

    const size_t firstBranchOnTest = branchesOnTest.begin() - m_branches.begin();
    auto branchesOnSameTest = [&](BlockID from) {
        return m_repeatsTest.get(from.index()) && m_indexOfBranch[from.index()] - firstBranchOnTest < branchesOnTest.size();
    };

    const CFG& cfg = m_analyses.cfg();
    KnownBoolean outcome = KnownBoolean::Unknown;
    Vector<BlockID, maxBlocksOnTheWays> seen { block };
    for (size_t next = 0; next < seen.size(); ++next) {
        std::span<const Edge> incomingEdges = cfg.incomingEdges(seen[next]);
        if (incomingEdges.empty())
            return KnownBoolean::Unknown;
        for (Edge edge : incomingEdges) {
            if (branchesOnSameTest(edge.from)) {
                KnownBoolean onEdge = outcomeOnEdge(edge);
                if (onEdge == KnownBoolean::Unknown || (outcome != KnownBoolean::Unknown && outcome != onEdge))
                    return KnownBoolean::Unknown;
                outcome = onEdge;
                continue;
            }
            if (edge.isHandlerEdge())
                return KnownBoolean::Unknown;
            if (seen.contains(edge.from))
                continue;
            if (seen.size() == maxBlocksOnTheWays) {
                m_gaveUpOnLongWays = true;
                return KnownBoolean::Unknown;
            }
            seen.append(edge.from);
        }
    }
    return outcome;
}

static constexpr OpcodeSet slotReads { Opcode::GetSlot, Opcode::GetSlotOrUndefined };

static const Value* layoutTestBranchedOn(FunctionAnalyses& analyses, BlockID block)
{
    const Value* test = typeTestBranchedOn(analyses.function(), analyses.function().at(block));
    if (!test)
        return nullptr;
    const Program& program = analyses.program();
    const Type& subject = program.typeOf(analyses.function(), test->valueOperand());
    return subject.hasOnlyLayouts() && membersOf(program, subject).size() > 1 ? test : nullptr;
}

SlotValueTests::SlotValueTests(FunctionAnalyses& analyses, TypeArena& types, std::span<const BlockID> blocksThatBranchOnTypeTest)
    : m_analyses(analyses)
    , m_types(types)
{
    Vector<ValueID, 8> testedObjects;
    for (BlockID block : blocksThatBranchOnTypeTest) {
        if (const Value* test = layoutTestBranchedOn(analyses, block))
            testedObjects.append(analyses.identityOf(test->valueOperand()));
    }
    if (testedObjects.isEmpty())
        return;
    AOT::sort(testedObjects);
    for (BlockID block : analyses.cfg().reversePostOrder())
        collectTestedReads(testedObjects.span(), block);
    AOT::stableSort(m_reads, { }, &TestedRead::object);
}

static bool declareSameType(const Program& program, std::span<const LayoutID> layouts, PropertyKeyID key)
{
    TypeID common;
    for (LayoutID layout : layouts) {
        std::optional<FieldReference> field = findField(program, layout, key, BaseChain::Proven);
        if (!field)
            continue;
        TypeID type = fieldAt(program, *field).declared.written();
        if (common && common != type)
            return false;
        common = type;
    }
    return true;
}

// No value of such a slot tells the layouts apart.
static bool hasSameTypeInEveryLayout(const Program& program, const Value& read)
{
    std::span<const LayoutID> layouts = program.at(read.receivers()).completeMembers();
    PropertyAccessCache* cache = program.propertyAccessCache();
    if (!cache || layouts.size() < PropertyAccessCache::fewestLayouts || TypeTable::isProvisional(read.key()))
        return declareSameType(program, layouts, read.key());
    PropertyAccessCache::DeclaredTypesKey key { cache->lists.idOf(program.types, read.receivers()), read.key() };
    return findOrCompute(cache->declaredTypes, key, computeHash(key.layouts, key.key), [&] {
        return PropertyAccessCache::DeclaredTypes { key, declareSameType(program, layouts, read.key()) };
    }).areTheSame;
}

void SlotValueTests::addTestedRead(std::span<const ValueID> testedObjects, BlockID block, ValueID tested, std::optional<uint32_t> operandIndex)
{
    const Function& function = m_analyses.function();
    // Behind its casts a value is what it is, unless that is a parameter.
    Opcode opcodeBehindCasts = function.at(sameObjectThroughCasts(function, tested)).opcode;
    if (opcodeBehindCasts != Opcode::Parameter && !slotReads.contains(opcodeBehindCasts))
        return;
    ValueID read = m_analyses.identityOf(tested);
    const Value& definition = function.at(read);
    if (!slotReads.contains(definition.opcode) || !definition.receivers())
        return;
    ValueID object = m_analyses.identityOf(definition.objectOperand());
    if (!std::ranges::binary_search(testedObjects, object) || hasSameTypeInEveryLayout(m_analyses.program(), definition))
        return;

    // Several cases of a switch may lead to one block.
    const Terminator& terminator = function.at(block).terminator;
    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
        BlockID target = terminator.targets[targetIndex].block;
        std::span<const Edge> incomingEdges = m_analyses.cfg().incomingEdges(target);
        bool isEnteredFromTestOnly = std::ranges::all_of(incomingEdges, [&](Edge edge) {
            return edge.from == block && !edge.isHandlerEdge();
        });
        if (isEnteredFromTestOnly && incomingEdges.front().targetIndex == targetIndex)
            m_reads.append({ object, read, block, operandIndex, target, std::nullopt });
    }
}

void SlotValueTests::collectTestedReads(std::span<const ValueID> testedObjects, BlockID block)
{
    const Function& function = m_analyses.function();
    const Terminator& terminator = function.at(block).terminator;
    switch (terminator.kind) {
    case TerminatorKind::Branch: {
        addTestedRead(testedObjects, block, terminator.conditionOperand(), std::nullopt);
        const Value& test = function.at(terminator.conditionOperand());
        if (!provesForever(provesOf(test.opcode)))
            return;
        for (uint32_t index = 0; index < test.numberOfOperands(); ++index) {
            if (test.operand(index) && test.operandKind(index) != OperandKind::Guard)
                addTestedRead(testedObjects, block, test.operand(index), index);
        }
        return;
    }
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
        addTestedRead(testedObjects, block, terminator.subjectOperand(), 0);
        return;
    case TerminatorKind::Jump:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Return:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::TryCallInline:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const Vector<Type>& SlotValueTests::slotTypesByReceiverOf(ValueID read)
{
    for (const auto& [knownRead, slotTypes] : m_slotTypesByReceiver) {
        if (knownRead == read)
            return *slotTypes;
    }
    const Function& function = m_analyses.function();
    TypingContext context { m_analyses.program(), function, m_types };
    std::span<const LayoutID> receivers = m_types.at(function.at(read).receivers()).completeMembers();
    auto slotTypes = makeUniqueWithoutFastMallocCheck<Vector<Type>>(receivers.size(), [&](size_t index) {
        return typeOfSlotRead(context, function.at(read), singleElementSpan(receivers[index]));
    });
    m_slotTypesByReceiver.append({ read, WTF::move(slotTypes) });
    return *m_slotTypesByReceiver.last().second;
}

// What holds in a block is what holds on each edge into it.
const Vector<LayoutID>& SlotValueTests::layoutsRuledOutBy(TestedRead& tested)
{
    if (tested.layoutsRuledOut)
        return *tested.layoutsRuledOut;

    const Function& function = m_analyses.function();
    const Terminator& terminator = function.at(tested.block).terminator;
    auto isRuledOut = [&](uint32_t targetIndex, const Type& slotType) {
        TypingContext context { m_analyses.program(), function, m_types };
        if (terminator.hasCases()) {
            if (targetIndex + 1 == terminator.targets.size())
                return narrowTypeByOtherwise(slotType, terminator).isNever();
            return narrowTypeByCase(context, slotType, terminator, targetIndex).isNever();
        }
        EdgeSide side = targetIndex ? EdgeSide::NotTaken : EdgeSide::Taken;
        if (tested.operandIndex)
            return narrowTypeByTest(context, slotType, function.at(terminator.conditionOperand()), *tested.operandIndex, side).isNever();
        return (side == EdgeSide::Taken ? truthyPartOf(slotType, m_types) : falsyPartOf(slotType, m_types)).isNever();
    };

    std::span<const Edge> incomingEdges = m_analyses.cfg().incomingEdges(tested.target);
    Vector<LayoutID> layouts;
    std::span<const LayoutID> receivers = m_types.at(function.at(tested.read).receivers()).completeMembers();
    const Vector<Type>& slotTypes = slotTypesByReceiverOf(tested.read);
    for (size_t index = 0; index < receivers.size(); ++index) {
        bool isRuledOutOnEveryEdge = std::ranges::all_of(incomingEdges, [&](Edge edge) {
            return isRuledOut(edge.targetIndex, slotTypes[index]);
        });
        if (isRuledOutOnEveryEdge)
            layouts.append(receivers[index]);
    }
    tested.layoutsRuledOut = WTF::move(layouts);
    return *tested.layoutsRuledOut;
}

KnownBoolean SlotValueTests::outcomeFromDominatingTests(BlockID block)
{
    const Value* test = m_reads.isEmpty() ? nullptr : layoutTestBranchedOn(m_analyses, block);
    if (!test)
        return KnownBoolean::Unknown;

    const Program& program = m_analyses.program();
    const Type& subject = program.typeOf(m_analyses.function(), test->valueOperand());
    // A structural type lists every layout with its names. Skip layouts that are never allocated: no object has one, and no slot test could rule them out.
    Vector<LayoutID, 8> layouts;
    for (LayoutID layout : membersOf(program, subject)) {
        if (program.at(layout).flow.allocation != AllocationState::NeverAllocated)
            layouts.append(layout);
    }
    size_t numberOfAllocatedMembers = layouts.size();
    for (TestedRead& tested : std::ranges::equal_range(m_reads, m_analyses.identityOf(test->valueOperand()), { }, &TestedRead::object)) {
        if (!m_analyses.dominators().dominates(tested.target, block))
            continue;
        const Vector<LayoutID>& ruledOut = layoutsRuledOutBy(tested);
        layouts.removeAllMatching([&](LayoutID layout) {
            return std::ranges::binary_search(ruledOut, layout);
        });
    }
    if (layouts.isEmpty() || layouts.size() == numberOfAllocatedMembers)
        return KnownBoolean::Unknown;

    Type narrowed = subject;
    narrowed.layouts = m_types.completeLayoutSet(layouts.span());
    return evaluateTypeTest(program, narrowed, program.at(test->testedType()));
}

TestsOfSameValues::TestsOfSameValues(FunctionAnalyses& analyses, TypeArena& types)
    : m_analyses(analyses)
    , m_types(types)
{
    const Function& function = analyses.function();
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        if (function.at(block).terminator.kind != TerminatorKind::Branch)
            continue;
        ValueID id = analyses.definitionThroughUpcasts(function.at(block).terminator.conditionOperand());
        const Value& test = function.at(id);
        Proof proof = provesOf(test.opcode);
        // Which of two integers is the greater is B3's to follow.
        if (!provesForever(proof) || proof == Proof::IntegerBound)
            continue;
        for (uint32_t index = 0; index < test.numberOfOperands(); ++index) {
            ValueID operand = test.operand(index);
            if (operand && test.operandKind(index) != OperandKind::Guard && resultRuleOf(function.at(operand).opcode) != ResultRule::Constant)
                m_operands.append({ operand, block, id, index });
        }
    }
    if (m_operands.isEmpty())
        return;
    bool hasExpectations = analyses.opcodesOfValues().contains(Opcode::Expect);
    if (m_operands.size() < 2 && !hasExpectations) {
        m_operands.clear();
        return;
    }
    const ValueIdentities& identities = analyses.identities();
    BitVector isTested(function.values.size());
    for (TestedOperand& operand : m_operands) {
        operand.subject = identities.identityOf(operand.subject);
        isTested.quickSet(operand.subject.index());
    }
    if (hasExpectations) {
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            for (ValueID id : function.at(block).values) {
                if (function.at(id).opcode != Opcode::Expect)
                    continue;
                ValueID subject = identities.identityOf(function.at(id).valueOperand());
                if (isTested.quickGet(subject.index()))
                    m_operands.append({ subject, block, id, 0, true });
            }
        }
    }
    AOT::stableSort(m_operands, { }, &TestedOperand::subject);
    size_t end = 0;
    forEachRunOfEqualKeys(m_operands.span(), &TestedOperand::subject, [&](std::span<const TestedOperand> operands) {
        if (operands.size() < 2 || std::ranges::all_of(operands, &TestedOperand::isExpectation))
            return;
        for (const TestedOperand& operand : operands) {
            m_hasExpectations |= operand.isExpectation;
            if (!operand.isExpectation)
                m_blocksThatTest.set(operand.block.index());
            m_operands[end++] = operand;
        }
    });
    m_operands.shrink(end);
}

bool TestsOfSameValues::isLive(const TestedOperand& operand) const
{
    return m_analyses.cfg().isReachable(operand.block) && (operand.isExpectation || m_analyses.function().at(operand.block).terminator.kind == TerminatorKind::Branch);
}

bool TestsOfSameValues::didChangeControlFlow()
{
    m_hasOutcomes = false;
    if (m_operands.isEmpty())
        return false;
    bool mayDecideMore = false;
    forEachRunOfEqualKeys(m_operands.span(), &TestedOperand::subject, [&](std::span<const TestedOperand> operands) {
        size_t numberOfLiveOperands = std::ranges::count_if(operands, [&](const TestedOperand& operand) { return isLive(operand); });
        mayDecideMore |= numberOfLiveOperands >= 2 && std::ranges::any_of(operands, [&](const TestedOperand& operand) { return !operand.isExpectation && isLive(operand); });
    });
    return mayDecideMore;
}

// An edge on which nothing is left of an operand is never taken. Only the blocks that say or ask something of a value are looked at, in the order in which a walk of the dominator tree would meet them.
void TestsOfSameValues::computeOutcomes()
{
    const Program& program = m_analyses.program();
    const Function& function = m_analyses.function();
    const CFG& cfg = m_analyses.cfg();
    const Dominators& dominators = m_analyses.dominators();
    TypingContext context { program, function, m_types };
    m_hasOutcomes = true;
    m_outcomes.fill(KnownBoolean::Unknown, cfg.numNodes());
    m_provenExpectations.clear();

    struct Step {
        uint32_t number;
        // None where the block asks.
        std::optional<EdgeSide> edgeEntered;
        uint32_t lastNumberBelow;
        const TestedOperand* operand;

        auto order() const { return std::tuple { number, !edgeEntered }; }
    };
    // What the edge has left of the value is looked for when a block behind the edge asks. Where the edge hands the operand on and nothing above it says more than the type of the operand, that is the type of what it hands on.
    enum class ScopeState : uint8_t { NotLookedAt, IsWhatEdgeHandsOn, WorkedOut };
    struct Scope {
        uint32_t lastNumberBelow;
        const TestedOperand* test;
        EdgeSide side;
        ValueID handedOn;
        ScopeState state { ScopeState::NotLookedAt };
        std::optional<Type> left { };
        // What is left was taken from this type, so it lies inside.
        TypeID narrowedType { };
    };
    Vector<Step, 16> steps;
    Vector<Scope, 8> scopes;
    // Null where the first scopes say no more of the value than the type of the operand does. What was taken from that very type may be all of it: whoever has to know asks.
    auto leftOf = [&](size_t numberOfScopes, ValueID operand, bool mustSayMore) -> const Type* {
        if (!numberOfScopes)
            return nullptr;
        const Scope& innermost = scopes[numberOfScopes - 1];
        TypeID type = function.at(operand).type;
        if (innermost.state == ScopeState::IsWhatEdgeHandsOn) {
            if (type == function.at(innermost.handedOn).type || sameObjectThroughCasts(function, operand) == innermost.handedOn)
                return nullptr;
        } else if (!mustSayMore && type == innermost.narrowedType)
            return &*innermost.left;
        const Type& left = innermost.state == ScopeState::IsWhatEdgeHandsOn ? context.typeOf(innermost.handedOn) : *innermost.left;
        const Type& typeOfOperand = m_types.at(type);
        return typeOfOperand == left || typeOfOperand.isSubsetOf(left, program) ? nullptr : &left;
    };
    auto workOutScopes = [&] {
        size_t first = scopes.size();
        while (first && scopes[first - 1].state == ScopeState::NotLookedAt)
            --first;
        for (size_t index = first; index < scopes.size(); ++index) {
            Scope& scope = scopes[index];
            const Value& test = function.at(scope.test->user);
            ValueID value = test.operand(scope.test->operandIndex);
            TypeID type = function.at(value).type;
            const Type* left = leftOf(index, value, false);
            if (!left && scope.handedOn) {
                scope.state = ScopeState::IsWhatEdgeHandsOn;
                continue;
            }
            if (!left || (scopes[index - 1].state == ScopeState::WorkedOut && type == scopes[index - 1].narrowedType))
                scope.narrowedType = type;
            scope.left = narrowTypeByTest(context, left ? *left : m_types.at(type), test, scope.test->operandIndex, scope.side);
            scope.state = ScopeState::WorkedOut;
        }
    };
    auto valueHandedOn = [&](const TestedOperand& operand, uint32_t targetIndex) -> ValueID {
        const Terminator& terminator = function.at(operand.block).terminator;
        const Target& target = terminator.targets[targetIndex];
        size_t position = terminator.conditionOperand() == operand.user ? target.proofs.find(operand.operandIndex) : notFound;
        return position == notFound ? ValueID() : function.at(target.block).parameters[target.arguments.size() + position];
    };
    forEachRunOfEqualKeys(m_operands.span(), &TestedOperand::subject, [&](std::span<const TestedOperand> operands) {
        steps.shrink(0);
        scopes.shrink(0);
        bool entersEdge = false;
        for (const TestedOperand& operand : operands) {
            if (!isLive(operand))
                continue;
            steps.append({ dominators.numberOf(operand.block), std::nullopt, 0, &operand });
            if (operand.isExpectation)
                continue;
            for (uint32_t targetIndex = 0; targetIndex < 2; ++targetIndex) {
                BlockID target = function.at(operand.block).terminator.targets[targetIndex].block;
                if (cfg.singleIncomingEdge(target) != Edge { operand.block, targetIndex })
                    continue;
                steps.append({ dominators.numberOf(target), targetIndex ? EdgeSide::NotTaken : EdgeSide::Taken, dominators.lastNumberBelow(target), &operand });
                entersEdge = true;
            }
        }
        if (!entersEdge)
            return;
        AOT::stableSort(steps, { }, &Step::order);
        for (const Step& step : steps) {
            while (!scopes.isEmpty() && scopes.last().lastNumberBelow < step.number)
                scopes.removeLast();
            const TestedOperand& operand = *step.operand;
            if (step.edgeEntered) {
                scopes.append({ step.lastNumberBelow, &operand, *step.edgeEntered, valueHandedOn(operand, *step.edgeEntered == EdgeSide::Taken ? 0 : 1) });
                continue;
            }
            if (scopes.isEmpty())
                continue;
            workOutScopes();
            const Value& user = function.at(operand.user);
            // What the type of the operand says has been asked.
            const Type* left = leftOf(scopes.size(), user.operand(operand.operandIndex), true);
            if (!left || left->isNever())
                continue;
            if (operand.isExpectation) {
                if (left->isSubsetOf(m_types.at(user.expectedType().predicted()), program))
                    m_provenExpectations.append({ operand.block, operand.user, m_types.intern(*left) });
                continue;
            }
            KnownBoolean& outcome = m_outcomes[operand.block.index()];
            if (outcome != KnownBoolean::Unknown)
                continue;
            if (narrowTypeByTest(context, *left, user, operand.operandIndex, EdgeSide::NotTaken).isNever())
                outcome = KnownBoolean::True;
            else if (narrowTypeByTest(context, *left, user, operand.operandIndex, EdgeSide::Taken).isNever())
                outcome = KnownBoolean::False;
        }
    });
}

KnownBoolean TestsOfSameValues::outcomeBehindDominatingTests(BlockID block)
{
    if (!m_blocksThatTest.get(block.index()))
        return KnownBoolean::Unknown;
    if (!m_hasOutcomes)
        computeOutcomes();
    return m_outcomes[block.index()];
}

Vector<TestsOfSameValues::ProvenExpectation, 4> TestsOfSameValues::takeProvenExpectations()
{
    if (!m_hasExpectations)
        return { };
    if (!m_hasOutcomes)
        computeOutcomes();
    return std::exchange(m_provenExpectations, { });
}

void proveExpectation(IRBuilder& builder, const TestsOfSameValues::ProvenExpectation& proven)
{
    Function& function = builder.function();
    size_t index = function.at(proven.block).values.find(proven.expectation);
    RELEASE_ASSERT(index != notFound);
    ProgramPoint point { proven.block, static_cast<uint32_t>(index) };
    builder.setOrigin(function.at(proven.expectation).origin());
    // It holds from the tests on, not wherever the value is at hand.
    ValueID expectationStoodHere = builder.insertBefore(point, IRBuilder::make(Opcode::Specified));
    ValueID cast = builder.insertBefore(point, makeAssumeType(function.at(proven.expectation).valueOperand(), expectationStoodHere, Proven::behindFact(proven.type)));
    function.edit(proven.expectation).setValueOperand(cast);
}

bool hasEdgeResults(const Function& function, Edge edge)
{
    return !edge.isHandlerEdge() && numberOfEdgeResults(function, edge.from, edge.targetIndex);
}

static bool isBranchWithProofs(const Terminator& terminator)
{
    return terminator.isConditionalBranch() && (!terminator.taken().proofs.isEmpty() || !terminator.notTaken().proofs.isEmpty());
}

bool isConditionOfBranchWithProofs(const Function& function, const UseLists& useLists, ValueID id)
{
    return std::ranges::any_of(useLists.usesOf(id), [&](const Use& use) {
        return use.kind == Use::Kind::TerminatorOperand && isBranchWithProofs(function.at(use.block).terminator);
    });
}

BitVector conditionsOfBranchesWithProofs(const Function& function, const CFG& cfg)
{
    BitVector conditions;
    for (BlockID block : cfg.reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (!isBranchWithProofs(terminator))
            continue;
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            conditions.set(operand.index());
        });
    }
    return conditions;
}

bool canFollowWithoutTest(const Function& function, Edge edge)
{
    if (edge.isHandlerEdge())
        return true;

    uint32_t numberOfResults = numberOfEdgeResults(function, edge.from, edge.targetIndex);
    for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
        std::optional<EdgeResult> result = tryGetEdgeResult(function, { edge.from, edge.targetIndex, resultIndex });
        if (!result || result->kind != EdgeResultKind::NarrowedOperand)
            return false;
    }
    return true;
}

Arguments argumentsWithoutTest(const Function& function, Edge edge)
{
    Arguments arguments = targetOf(function, edge).arguments;
    if (edge.isHandlerEdge())
        return arguments;

    uint32_t numberOfResults = numberOfEdgeResults(function, edge.from, edge.targetIndex);
    for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
        EdgeResult result = edgeResultAt(function, { edge.from, edge.targetIndex, resultIndex });
        RELEASE_ASSERT(result.kind == EdgeResultKind::NarrowedOperand);
        arguments.append(result.subject);
    }
    return arguments;
}

// The edge of a test narrows its subject, and a jump in its place passes the subject with the type it has.
bool argumentsFitParameters(const Program& program, const Function& function, const Target& target)
{
    const CompactList<ValueID>& parameters = function.at(target.block).parameters;
    RELEASE_ASSERT(target.proofs.isEmpty() && target.arguments.size() == parameters.size());
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (!program.typeOf(function, target.arguments[index]).isSubsetOf(program.typeOf(function, parameters[index]), program))
            return false;
    }
    return true;
}

// The outcome of a test may be known where the type of its subject does not say it. The jump then passes the subject as the edge narrowed it.
bool argumentsFitParametersAfterCasts(const Program& program, const Function& function, TypeArena& types, const Target& target)
{
    const CompactList<ValueID>& parameters = function.at(target.block).parameters;
    RELEASE_ASSERT(target.proofs.isEmpty() && target.arguments.size() == parameters.size());
    for (size_t index = 0; index < parameters.size(); ++index) {
        const Type& passed = program.typeOf(function, target.arguments[index]);
        const Type& expected = program.typeOf(function, parameters[index]);
        if (passed.isSubsetOf(expected, program))
            continue;
        Type cast = passed.castTo(expected, types);
        if (cast.isNever() || !cast.isSubsetOf(expected, program))
            return false;
    }
    return true;
}

void castArgumentsToParameters(IRBuilder& builder, BlockID block, bool validates)
{
    const Program& program = builder.context().program;
    Function& function = builder.function();
    Terminator& terminator = function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
    RELEASE_ASSERT(terminator.kind == TerminatorKind::Jump);

    builder.setOrigin(terminator.origin());
    Target& target = terminator.targets[0];
    // What decided the test holds where the test stood, not wherever its subject is at hand.
    ValueID testStoodHere;
    for (size_t index = 0; index < target.arguments.size(); ++index) {
        ValueID argument = target.arguments[index];
        ValueID parameter = function.at(target.block).parameters[index];
        if (program.typeOf(function, argument).isSubsetOf(program.typeOf(function, parameter), program))
            continue;
        TypeID narrowed = function.at(parameter).type;
        if (validates)
            builder.insertBefore(pointBeforeTerminatorOf(function, block), makeValidateType(argument, narrowed));
        if (!testStoodHere)
            testStoodHere = builder.insertBefore(pointBeforeTerminatorOf(function, block), IRBuilder::make(Opcode::Specified));
        target.arguments[index] = builder.insertBefore(pointBeforeTerminatorOf(function, block), makeAssumeType(argument, testStoodHere, Proven::behindFact(narrowed)));
    }
}

static uint32_t findCaseTarget(const auto& cases, auto taken, uint32_t otherwise)
{
    for (uint32_t index = 0; index < cases.size(); ++index) {
        if (cases[index] == taken)
            return index;
    }
    return otherwise;
}

std::optional<uint32_t> decidedTargetOf(const Terminator& terminator, KnownValue tested)
{
    if (!tested.isKnown())
        return std::nullopt;

    uint32_t otherwise = static_cast<uint32_t>(terminator.targets.size()) - 1;
    switch (terminator.kind) {
    case TerminatorKind::Branch:
        if (tested.kind() != KnownValue::Kind::Boolean)
            return std::nullopt;
        return tested.asBoolean() ? 0u : 1u;
    case TerminatorKind::Switch:
        if (std::optional<int64_t> taken = switchCaseOf(tested))
            return findCaseTarget(terminator.cases(), *taken, otherwise);
        return otherwise;
    case TerminatorKind::SwitchString:
        if (tested.kind() == KnownValue::Kind::String)
            return findCaseTarget(terminator.strings(), tested.asString(), otherwise);
        return otherwise;
    case TerminatorKind::Jump:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Return:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::TryCallInline:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static KnownBoolean booleanOf(KnownValue value)
{
    return value.kind() == KnownValue::Kind::Boolean ? knownBoolean(value.asBoolean()) : KnownBoolean::Unknown;
}

KnownBoolean evaluateConditionByDefinitions(FunctionAnalyses& analyses, BlockID block)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    const Terminator& terminator = function.at(block).terminator;
    ASSERT(terminator.isConditionalBranch());

    KnownBoolean outcome = booleanOf(knownValueOf(program, function, terminator.conditionOperand()));
    if (outcome != KnownBoolean::Unknown)
        return outcome;
    const Value& test = function.at(analyses.definitionThroughUpcasts(terminator.conditionOperand()));
    outcome = evaluateTestByTypes(program, function, test);
    if (outcome != KnownBoolean::Unknown)
        return outcome;
    return booleanOf(evaluateWithKnownOperands(program, function, test, [&](ValueID operand) {
        return knownValueOf(program, function, operand);
    }));
}

KnownBoolean evaluateCondition(FunctionAnalyses& analyses, const TypeTestBranches& typeTests, SlotValueTests& slotValueTests, TestsOfSameValues& testsOfSameValues, BlockID block)
{
    KnownBoolean outcome = evaluateConditionByDefinitions(analyses, block);
    if (outcome == KnownBoolean::Unknown)
        outcome = typeTests.outcomeFromDominatingBranches(analyses.function().at(block).terminator.conditionOperand(), block);
    if (outcome == KnownBoolean::Unknown)
        outcome = typeTests.outcomeFromDominatingTest(block);
    if (outcome == KnownBoolean::Unknown)
        outcome = typeTests.outcomeOnEveryWay(block);
    if (outcome == KnownBoolean::Unknown)
        outcome = slotValueTests.outcomeFromDominatingTests(block);
    if (outcome == KnownBoolean::Unknown)
        outcome = testsOfSameValues.outcomeBehindDominatingTests(block);
    return outcome;
}

void raiseFrequenciesBehind(FunctionAnalyses& analyses, Function& function, Edge decided)
{
    Expected<BlockFrequency> expected = function.at(targetOf(function, decided).block).frequency;
    Expected<BlockFrequency> actual = function.at(decided.from).frequency;
    if (!isRarerThan(expected, actual).predicted())
        return;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        if (function.at(block).frequency.isSamePredictionAs(expected) && analyses.dominates(decided, block))
            function.edit(block, BlockEdit::Rest).frequency = actual;
    }
}

void decideTerminator(IRBuilder& builder, BlockID block, uint32_t targetIndex)
{
    Function& function = builder.function();
    Edge edge { block, targetIndex };
    RELEASE_ASSERT(canFollowWithoutTest(function, edge));

    Terminator& terminator = function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
    if (terminator.isConditionalBranch()) {
        builder.convertBranchToJump(block, targetIndex);
        return;
    }

    RELEASE_ASSERT(terminator.hasCases());
    Arguments arguments = argumentsWithoutTest(function, edge);
    BlockID target = terminator.targets[targetIndex].block;
    Origin previousOrigin = builder.origin();
    BlockID previousBlock = builder.currentBlock();

    builder.setOrigin(terminator.origin());
    terminator = Terminator { };
    builder.moveToEndOf(block);
    builder.emitJump(target, WTF::move(arguments));

    builder.setOrigin(previousOrigin);
    if (previousBlock)
        builder.moveToEndOf(previousBlock);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
