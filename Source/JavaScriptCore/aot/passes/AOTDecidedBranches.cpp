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
#include <algorithm>

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

static KnownBoolean evaluateFromDominatingBranches(FunctionAnalyses& analyses, ValueID condition, BlockID block)
{
    const Function& function = analyses.function();
    for (const Use& use : analyses.useLists().usesOf(condition)) {
        if (use.kind != Use::Kind::TerminatorOperand || use.block == block || !function.at(use.block).terminator.isConditionalBranch())
            continue;
        if (analyses.dominates(Edge { use.block, 0 }, block))
            return KnownBoolean::True;
        if (analyses.dominates(Edge { use.block, 1 }, block))
            return KnownBoolean::False;
    }
    return KnownBoolean::Unknown;
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
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        if (const Value* test = typeTestBranchedOn(analyses, block))
            m_branches.append(TestedBranch { test->valueOperand(), test->testedType(), block });
    }
    // The identities of values are asked for only where two tests are for one type.
    std::ranges::stable_sort(m_branches, { }, [](const TestedBranch& branch) {
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
    std::ranges::stable_sort(m_branches, { }, &TestedBranch::test);
}

// What a value is tested for never changes, so behind an edge of one test every other test of the value for the same type has the same outcome.
KnownBoolean TypeTestBranches::outcomeFromDominatingTest(BlockID block) const
{
    const Value* test = m_branches.isEmpty() ? nullptr : typeTestBranchedOn(m_analyses, block);
    if (!test)
        return KnownBoolean::Unknown;
    std::tuple tested { m_analyses.identityOf(test->valueOperand()).index(), test->testedType().index() };
    for (const TestedBranch& other : std::ranges::equal_range(m_branches, tested, { }, &TestedBranch::test)) {
        if (other.block == block)
            continue;
        if (m_analyses.dominates(Edge { other.block, 0 }, block))
            return KnownBoolean::True;
        if (m_analyses.dominates(Edge { other.block, 1 }, block))
            return KnownBoolean::False;
    }
    return KnownBoolean::Unknown;
}

static constexpr OpcodeSet slotReads { Opcode::GetSlot, Opcode::GetSlotOrUndefined };

static const Value* layoutTestBranchedOn(FunctionAnalyses& analyses, BlockID block)
{
    const Value* test = typeTestBranchedOn(analyses, block);
    if (!test)
        return nullptr;
    const Program& program = analyses.program();
    const Type& subject = program.typeOf(analyses.function(), test->valueOperand());
    return subject.hasOnlyLayouts() && membersOf(program, subject).size() > 1 ? test : nullptr;
}

SlotValueTests::SlotValueTests(FunctionAnalyses& analyses, TypeArena& types)
    : m_analyses(analyses)
    , m_types(types)
{
    std::span<const BlockID> blocks = analyses.cfg().reversePostOrder();
    Vector<ValueID, 8> testedObjects;
    for (BlockID block : blocks) {
        if (const Value* test = layoutTestBranchedOn(analyses, block))
            testedObjects.append(analyses.identityOf(test->valueOperand()));
    }
    if (testedObjects.isEmpty())
        return;
    std::ranges::sort(testedObjects);
    for (BlockID block : blocks)
        collectTestedReads(testedObjects.span(), block);
    std::ranges::stable_sort(m_reads, { }, &TestedRead::object);
}

// No value of such a slot tells the layouts apart.
static bool hasSameTypeInEveryLayout(const Program& program, const Value& read)
{
    TypeID common;
    for (LayoutID layout : program.at(read.receivers()).members) {
        std::optional<FieldReference> field = findField(program, layout, read.key(), BaseChain::Proven);
        if (!field)
            continue;
        TypeID type = fieldAt(program, *field).declared.type;
        if (common && common != type)
            return false;
        common = type;
    }
    return true;
}

void SlotValueTests::addTestedRead(std::span<const ValueID> testedObjects, BlockID block, ValueID tested, std::optional<uint32_t> operandIndex)
{
    const Function& function = m_analyses.function();
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

bool SlotValueTests::isRuledOut(const TestedRead& tested, uint32_t targetIndex, LayoutID layout) const
{
    const Function& function = m_analyses.function();
    TypingContext context { m_analyses.program(), function, m_types };
    Type slotType = typeOfSlotRead(context, function.at(tested.read), singleElementSpan(layout));

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
    for (LayoutID layout : m_types.at(m_analyses.function().at(tested.read).receivers()).members) {
        bool isRuledOutOnEveryEdge = std::ranges::all_of(incomingEdges, [&](Edge edge) {
            return isRuledOut(tested, edge.targetIndex, layout);
        });
        if (isRuledOutOnEveryEdge)
            layouts.append(layout);
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
    Terminator& terminator = function.at(block).terminator;
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

KnownBoolean evaluateCondition(FunctionAnalyses& analyses, const TypeTestBranches& typeTests, SlotValueTests& slotValueTests, BlockID block)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    const Terminator& terminator = function.at(block).terminator;
    ASSERT(terminator.isConditionalBranch());
    ValueID condition = terminator.conditionOperand();

    KnownBoolean outcome = booleanOf(knownValueOf(program, function, condition));
    if (outcome == KnownBoolean::Unknown)
        outcome = evaluateFromOperands(program, function, function.at(analyses.definitionThroughUpcasts(condition)));
    if (outcome == KnownBoolean::Unknown)
        outcome = evaluateFromDominatingBranches(analyses, condition, block);
    if (outcome == KnownBoolean::Unknown)
        outcome = typeTests.outcomeFromDominatingTest(block);
    if (outcome == KnownBoolean::Unknown)
        outcome = slotValueTests.outcomeFromDominatingTests(block);
    return outcome;
}

void decideTerminator(IRBuilder& builder, BlockID block, uint32_t targetIndex)
{
    Function& function = builder.function();
    Edge edge { block, targetIndex };
    RELEASE_ASSERT(canFollowWithoutTest(function, edge));

    Terminator& terminator = function.at(block).terminator;
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
