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
#include "AOTVector.h"
#include <algorithm>
#include <limits>
#include <wtf/HashMap.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

static KnownBoolean booleanOf(KnownValue value)
{
    if (value.kind() != KnownValue::Kind::Boolean)
        return KnownBoolean::Unknown;
    return knownBoolean(value.asBoolean());
}

static KnownBoolean evaluateFromOperands(const Program& program, const Function& function, const Value& test)
{
    KnownBoolean outcome = evaluateTestByTypes(program, function, test);
    if (outcome != KnownBoolean::Unknown)
        return outcome;
    return booleanOf(evaluateWithKnownOperands(program, function, test, [&](ValueID operand) {
        return knownValueOf(program, function, operand);
    }));
}

static constexpr uint32_t noGroup = std::numeric_limits<uint32_t>::max();

static KnownBoolean outcomeOnEdge(Edge edge)
{
    if (edge.isHandlerEdge() || edge.targetIndex > 1)
        return KnownBoolean::Unknown;
    return edge.targetIndex ? KnownBoolean::False : KnownBoolean::True;
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

// Nobody edits the function while the branches are asked about, so this says what the use lists would say of the terminators. It holds the values that several branches name, and no other.
auto TypeTestBranches::branchesOn(ValueID value) const -> std::span<const BranchOnValue>
{
    if (m_branchesByOperand.isEmpty())
        return { };
    auto found = std::ranges::equal_range(m_branchesByOperand, value.index(), { }, [](const BranchOnValue& branch) {
        return branch.operand.index();
    });
    return { found.begin(), found.end() };
}

// Those on one value stay in reverse post order.
void TypeTestBranches::keepValuesThatSeveralBranchesName()
{
    Vector<BranchOnValue, 32>& branches = m_branchesByOperand;
    AOT::stableSort(branches, { }, [](const BranchOnValue& branch) {
        return branch.operand.index();
    });
    size_t end = 0;
    for (size_t first = 0; first < branches.size();) {
        size_t endOfValue = first + 1;
        while (endOfValue < branches.size() && branches[endOfValue].operand == branches[first].operand)
            ++endOfValue;
        if (endOfValue - first > 1) {
            for (size_t index = first; index < endOfValue; ++index)
                branches[end++] = branches[index];
        }
        first = endOfValue;
    }
    branches.shrink(end);
}

void TypeTestBranches::computeOutcomesOfFrequentConditions() const
{
    const Function& function = m_analyses.function();
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> groupOfCondition;
    auto groupOf = [&](ValueID condition) {
        auto found = groupOfCondition.find(condition.index());
        return found == groupOfCondition.end() ? noGroup : found->value;
    };
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch())
            continue;
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            if (branchesOn(operand).size() > maxComparedBranches)
                groupOfCondition.add(operand.index(), groupOfCondition.size());
        });
    }
    m_outcomesOfFrequentConditions = outcomesBehindOutermostEdges(m_analyses, groupOfCondition.size(), [&](BlockID block, const ScopedLambda<void(uint32_t)>& functor) {
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch())
            return;
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            if (uint32_t group = groupOf(operand); group != noGroup)
                functor(group);
        });
    }, [&](BlockID block) {
        const Terminator& terminator = function.at(block).terminator;
        return terminator.isConditionalBranch() ? groupOf(terminator.conditionOperand()) : noGroup;
    });
}

KnownBoolean TypeTestBranches::outcomeFromDominatingBranches(ValueID condition, BlockID block) const
{
    std::span<const BranchOnValue> branches = branchesOn(condition);
    if (branches.empty())
        return KnownBoolean::Unknown;
    const Function& function = m_analyses.function();
    auto compareWithEachBranch = [&] {
        for (const BranchOnValue& branch : branches) {
            if (branch.block == block)
                continue;
            if (m_analyses.dominates(Edge { branch.block, 0 }, block))
                return KnownBoolean::True;
            if (m_analyses.dominates(Edge { branch.block, 1 }, block))
                return KnownBoolean::False;
        }
        return KnownBoolean::Unknown;
    };
    if (branches.size() <= maxComparedBranches)
        return compareWithEachBranch();

    RELEASE_ASSERT(function.at(block).terminator.isConditionalBranch() && function.at(block).terminator.conditionOperand() == condition);
    if (m_outcomesOfFrequentConditions.isEmpty())
        computeOutcomesOfFrequentConditions();
    KnownBoolean outcome = m_outcomesOfFrequentConditions[block.index()];
    if (m_analyses.context().build.options().verifiesAfterEveryPass)
        RELEASE_ASSERT(outcome == compareWithEachBranch());
    return outcome;
}

static const Value* typeTestBranchedOn(FunctionAnalyses& analyses, BlockID block)
{
    const Function& function = analyses.function();
    const Terminator& terminator = function.at(block).terminator;
    if (terminator.kind != TerminatorKind::Branch)
        return nullptr;
    const Value& test = function.at(terminator.conditionOperand());
    return test.opcode == Opcode::TypeTest ? &test : nullptr;
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
        if (const Value* test = typeTestBranchedOn(analyses, block)) {
            m_branches.append(TestedBranch { test->valueOperand(), test->testedType(), block });
            m_blocksThatBranchOnTypeTest.append(block);
        }
    }
    keepValuesThatSeveralBranchesName();
    // The identities of values are asked for only where two tests are for one type.
    AOT::stableSort(m_branches, { }, [](const TestedBranch& branch) {
        return branch.testedType.index();
    });
    auto isOnlyTestForItsType = [&](const TestedBranch& branch) {
        size_t index = &branch - m_branches.span().data();
        bool equalsPrevious = index && m_branches[index - 1].testedType == branch.testedType;
        bool equalsNext = index + 1 < m_branches.size() && m_branches[index + 1].testedType == branch.testedType;
        return !equalsPrevious && !equalsNext;
    };
    Vector<TestedBranch> repeated;
    for (const TestedBranch& branch : m_branches) {
        if (!isOnlyTestForItsType(branch))
            repeated.append(branch);
    }
    m_branches = WTF::move(repeated);
    for (TestedBranch& branch : m_branches)
        branch.subject = analyses.identityOf(branch.subject);
    AOT::stableSort(m_branches, { }, &TestedBranch::test);

    // A test that no other branch repeats has nothing to learn from the table.
    size_t end = 0;
    for (size_t first = 0; first < m_branches.size();) {
        size_t endOfTest = first + 1;
        while (endOfTest < m_branches.size() && m_branches[endOfTest].test() == m_branches[first].test())
            ++endOfTest;
        if (endOfTest - first > 1) {
            for (size_t index = first; index < endOfTest; ++index) {
                m_repeatsTest.set(m_branches[index].block.index());
                m_branches[end++] = m_branches[index];
            }
        }
        first = endOfTest;
    }
    m_branches.shrink(end);
}

void TypeTestBranches::computeOutcomesOfFrequentTests() const
{
    Vector<uint32_t> groupOfBlock(FillWith { }, m_analyses.cfg().numNodes(), noGroup);
    uint32_t numberOfGroups = 0;
    for (size_t first = 0; first < m_branches.size();) {
        size_t end = first + 1;
        while (end < m_branches.size() && m_branches[end].test() == m_branches[first].test())
            ++end;
        if (end - first > maxComparedBranches) {
            for (size_t index = first; index < end; ++index)
                groupOfBlock[m_branches[index].block.index()] = numberOfGroups;
            ++numberOfGroups;
        }
        first = end;
    }
    m_outcomesOfFrequentTests = outcomesBehindOutermostEdges(m_analyses, numberOfGroups, [&](BlockID block, const ScopedLambda<void(uint32_t)>& functor) {
        if (uint32_t group = groupOfBlock[block.index()]; group != noGroup)
            functor(group);
    }, [&](BlockID block) {
        return groupOfBlock[block.index()];
    });
}

// What a value is tested for never changes, so behind an edge of one test every other test of the value for the same type has the same outcome.
KnownBoolean TypeTestBranches::outcomeFromDominatingTest(BlockID block) const
{
    const Value* test = m_repeatsTest.get(block.index()) ? typeTestBranchedOn(m_analyses, block) : nullptr;
    if (!test)
        return KnownBoolean::Unknown;
    std::tuple tested { m_analyses.identityOf(test->valueOperand()).index(), test->testedType().index() };
    auto branchesOnTest = std::ranges::equal_range(m_branches, tested, { }, &TestedBranch::test);
    auto compareWithEachBranch = [&] {
        for (const TestedBranch& other : branchesOnTest) {
            if (other.block == block)
                continue;
            if (m_analyses.dominates(Edge { other.block, 0 }, block))
                return KnownBoolean::True;
            if (m_analyses.dominates(Edge { other.block, 1 }, block))
                return KnownBoolean::False;
        }
        return KnownBoolean::Unknown;
    };
    if (branchesOnTest.size() <= maxComparedBranches)
        return compareWithEachBranch();

    if (m_outcomesOfFrequentTests.isEmpty())
        computeOutcomesOfFrequentTests();
    KnownBoolean outcome = m_outcomesOfFrequentTests[block.index()];
    if (m_analyses.context().build.options().verifiesAfterEveryPass)
        RELEASE_ASSERT(outcome == compareWithEachBranch());
    return outcome;
}

static bool listsOnlyFunctions(const Type& type)
{
    return !type.functions.isEmpty() && type.hasOnlyKnownCallees() && !type.primitives && !type.layouts && type.builtins.isEmpty();
}

// No single test dominates the block, and every way to it leaves some test of the value for the type by an edge with the same outcome.
KnownBoolean TypeTestBranches::outcomeOnEveryWay(BlockID block) const
{
    static constexpr unsigned maxBlocksOnTheWays = 32;

    const Value* test = m_repeatsTest.get(block.index()) ? typeTestBranchedOn(m_analyses, block) : nullptr;
    if (!test || !listsOnlyFunctions(m_analyses.program().at(test->testedType())))
        return KnownBoolean::Unknown;
    std::tuple tested { m_analyses.identityOf(test->valueOperand()).index(), test->testedType().index() };
    auto branchesOnTest = std::ranges::equal_range(m_branches, tested, { }, &TestedBranch::test);
    if (branchesOnTest.size() < 3)
        return KnownBoolean::Unknown;

    const CFG& cfg = m_analyses.cfg();
    KnownBoolean outcome = KnownBoolean::Unknown;
    Vector<BlockID, maxBlocksOnTheWays> seen { block };
    for (size_t next = 0; next < seen.size(); ++next) {
        std::span<const Edge> incomingEdges = cfg.incomingEdges(seen[next]);
        if (incomingEdges.empty())
            return KnownBoolean::Unknown;
        for (Edge edge : incomingEdges) {
            bool leavesTest = std::ranges::contains(branchesOnTest, edge.from, &TestedBranch::block);
            if (leavesTest) {
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
    const Value* test = typeTestBranchedOn(analyses, block);
    if (!test)
        return nullptr;
    const Program& program = analyses.program();
    const Type& subject = program.typeOf(analyses.function(), test->valueOperand());
    return subject.hasOnlyLayouts(program) && membersOf(program, subject).size() > 1 ? test : nullptr;
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
        TypeID type = fieldAt(program, *field).declared.type;
        if (common && common != type)
            return false;
        common = type;
    }
    return true;
}

// No value of such a slot tells the layouts apart.
static bool hasSameTypeInEveryLayout(const Program& program, const Value& read)
{
    std::span<const LayoutID> layouts = program.at(read.receivers()).members.span();
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
    const Vector<LayoutID>& receivers = m_types.at(function.at(read).receivers()).members;
    auto slotTypes = makeUniqueWithoutFastMallocCheck<Vector<Type>>(receivers.size(), [&](size_t index) {
        return typeOfSlotRead(context, function.at(read), singleElementSpan(receivers[index]));
    });
    m_slotTypesByReceiver.append({ read, WTF::move(slotTypes) });
    return *m_slotTypesByReceiver.last().second;
}

bool SlotValueTests::isRuledOut(const TestedRead& tested, uint32_t targetIndex, const Type& slotType) const
{
    const Function& function = m_analyses.function();
    TypingContext context { m_analyses.program(), function, m_types };

    const Terminator& terminator = function.at(tested.block).terminator;
    if (terminator.hasCases()) {
        if (targetIndex + 1 == terminator.targets.size())
            return narrowTypeByOtherwise(slotType, terminator).isNever();
        return narrowTypeByCase(context, slotType, terminator, targetIndex).isNever();
    }
    EdgeSide side = targetIndex ? EdgeSide::NotTaken : EdgeSide::Taken;
    if (tested.operandIndex)
        return narrowTypeByTest(context, slotType, function.at(terminator.conditionOperand()), *tested.operandIndex, side).isNever();
    return (side == EdgeSide::Taken ? truthyPartOf(slotType, m_types) : falsyPartOf(slotType, m_types)).isNever();
}

// What holds in a block is what holds on each edge into it.
const Vector<LayoutID>& SlotValueTests::layoutsRuledOutBy(TestedRead& tested)
{
    if (tested.layoutsRuledOut)
        return *tested.layoutsRuledOut;

    std::span<const Edge> incomingEdges = m_analyses.cfg().incomingEdges(tested.target);
    Vector<LayoutID> layouts;
    const Vector<LayoutID>& receivers = m_types.at(m_analyses.function().at(tested.read).receivers()).members;
    const Vector<Type>& slotTypes = slotTypesByReceiverOf(tested.read);
    for (size_t index = 0; index < receivers.size(); ++index) {
        bool isRuledOutOnEveryEdge = std::ranges::all_of(incomingEdges, [&](Edge edge) {
            return isRuledOut(tested, edge.targetIndex, slotTypes[index]);
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
    std::span<const LayoutID> members = membersOf(program, subject);
    Vector<LayoutID, 8> layouts(members);
    for (TestedRead& tested : std::ranges::equal_range(m_reads, m_analyses.identityOf(test->valueOperand()), { }, &TestedRead::object)) {
        if (!m_analyses.dominators().dominates(tested.target, block))
            continue;
        const Vector<LayoutID>& ruledOut = layoutsRuledOutBy(tested);
        layouts.removeAllMatching([&](LayoutID layout) {
            return std::ranges::binary_search(ruledOut, layout);
        });
    }
    if (layouts.isEmpty() || layouts.size() == members.size())
        return KnownBoolean::Unknown;

    Type narrowed = subject;
    narrowed.layouts = m_types.completeLayoutSet(layouts.span());
    return evaluateTypeTest(program, narrowed, program.at(test->testedType()));
}

bool hasEdgeResults(const Function& function, Edge edge)
{
    return !edge.isHandlerEdge() && numberOfEdgeResults(function, edge.from, edge.targetIndex);
}

bool isConditionOfBranchWithProofs(const Function& function, const UseLists& useLists, ValueID id)
{
    return std::ranges::any_of(useLists.usesOf(id), [&](const Use& use) {
        if (use.kind != Use::Kind::TerminatorOperand)
            return false;
        const Terminator& terminator = function.at(use.block).terminator;
        return terminator.isConditionalBranch() && (!terminator.taken().proofs.isEmpty() || !terminator.notTaken().proofs.isEmpty());
    });
}

BitVector conditionsOfBranchesWithProofs(const Function& function, const CFG& cfg)
{
    BitVector conditions;
    for (BlockID block : cfg.reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch() || (terminator.taken().proofs.isEmpty() && terminator.notTaken().proofs.isEmpty()))
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
    for (size_t index = 0; index < target.arguments.size(); ++index) {
        ValueID argument = target.arguments[index];
        ValueID parameter = function.at(target.block).parameters[index];
        if (program.typeOf(function, argument).isSubsetOf(program.typeOf(function, parameter), program))
            continue;
        TypeID narrowed = function.at(parameter).type;
        if (validates)
            builder.insertBefore(pointBeforeTerminatorOf(function, block), makeValidateType(argument, narrowed));
        target.arguments[index] = builder.insertBefore(pointBeforeTerminatorOf(function, block), makeAssumeType(argument, narrowed));
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

KnownBoolean evaluateConditionByDefinitions(FunctionAnalyses& analyses, BlockID block)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    const Terminator& terminator = function.at(block).terminator;
    ASSERT(terminator.isConditionalBranch());
    ValueID condition = terminator.conditionOperand();

    KnownBoolean outcome = booleanOf(knownValueOf(program, function, condition));
    if (outcome == KnownBoolean::Unknown)
        outcome = evaluateFromOperands(program, function, function.at(analyses.definitionThroughUpcasts(condition)));
    return outcome;
}

KnownBoolean evaluateCondition(FunctionAnalyses& analyses, const TypeTestBranches& typeTests, SlotValueTests& slotValueTests, BlockID block)
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
    return outcome;
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
