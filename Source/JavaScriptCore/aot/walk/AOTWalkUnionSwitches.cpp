/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTOperatorForms.h"
#include "AOTPrimitives.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTTypeNarrowing.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t leastCasesOfUnionSwitches = 3;

static constexpr OpcodeSet equalitiesThatUnionSwitchesStandFor { Opcode::StrictEq, Opcode::StringEqualsLiteral, Opcode::ValueEqualsLiteral };

namespace {

struct ConstantOfUnionSwitches {
    bool isText { false };
    int64_t number { 0 };
    StringID string;
};

struct CaseOfUnionSwitches {
    int64_t number { 0 };
    StringID string;
    Walker::EdgeToTargetOfBody target;
};

struct ReadOfSubjectInUnionSwitches {
    ValueID object;
    PropertyKeyID key;
};

} // anonymous namespace

static ValueID valueBehindNamesInUnionSwitches(const Code& code, ValueID value)
{
    return code.valueBehindNames<valuePreservingOpcodes, Code::SourcesOfNames::StandEarlier>(value);
}

static bool isOperationOfBlockInUnionSwitches(const BodyIndex& index, BodyBlock block, ValueID value)
{
    return value && block.holds(value) && !index.isParameter(value);
}

static bool isConstantBehindCastsInUnionSwitches(const BodyIndex& index, ValueID value)
{
    ValueID root = index.stripCasts(value);
    return root && !index.isParameter(root) && primitiveConstants.contains(index.opcodeOf(root));
}

static std::optional<ConstantOfUnionSwitches> caseBehindCastsInUnionSwitches(const BodyIndex& index, ValueID value)
{
    ValueID root = index.stripCasts(value);
    if (!root || index.isParameter(root))
        return std::nullopt;
    BodyValue operation = index.value(root);
    if (operation.opcode() == Opcode::String)
        return ConstantOfUnionSwitches { true, 0, operation.as<Opcode::String>().string() };
    if (operation.opcode() != Opcode::Integer)
        return std::nullopt;
    int64_t number = operation.as<Opcode::Integer>().integer();
    if (!Range::full().contains(number))
        return std::nullopt;
    return ConstantOfUnionSwitches { false, number, { } };
}

static bool standsUnderHandlerOfHeadInUnionSwitches(BodyBlock head, BodyBlock passed)
{
    if (head.hasHandler() != passed.hasHandler())
        return false;
    if (!head.hasHandler())
        return true;
    return head.handler().block() == passed.handler().block() && std::ranges::equal(head.handler().arguments(), passed.handler().arguments());
}

static bool mayBePassedByInUnionSwitches(const BodyIndex& index, BodyBlock head, BodyBlock passed, uint32_t numberOfIncomingEdges)
{
    BlockID id = passed.id();
    if (index.numberOfIncomingEdges(id) != numberOfIncomingEdges || index.isLoopHead(id) || index.isEnteredByHandlerEdge(id) || passed.isHandlerBlock() || passed.subject())
        return false;
    return id != head.id() && index.loopOf(id) == index.loopOf(head.id()) && standsUnderHandlerOfHeadInUnionSwitches(head, passed);
}

static bool comparesSubjectItselfInUnionSwitches(const Code& code, Reference comparison, Reference subject)
{
    if (!comparison || !comparison.isValue())
        return false;
    ValueID built = valueBehindNamesInUnionSwitches(code, comparison.id());
    if (!built || !equalitiesThatUnionSwitchesStandFor.contains(code.at(built).opcode))
        return false;
    ValueID producer = valueBehindNamesInUnionSwitches(code, subject.id());
    return producer && std::ranges::any_of(code.at(built).operands(), [&](ValueID operand) {
        return operand && !code.isConstant(operand) && valueBehindNamesInUnionSwitches(code, operand) == producer;
    });
}

static ReadOfSubjectInUnionSwitches findReadThatMayBeRepeatedInUnionSwitches(const Walker& walker, ValueID rootOfSubject, Reference subject)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (index.isParameter(rootOfSubject) || index.opcodeOf(rootOfSubject) != Opcode::PropGet)
        return { };
    BodyOperation<Opcode::PropGet> written = index.value(rootOfSubject).as<Opcode::PropGet>();
    if (written.isFreeName())
        return { };

    const Code& code = walker.code();
    ValueID slotRead = valueBehindNamesInUnionSwitches(code, subject.id());
    if (!slotRead || code.at(slotRead).opcode != Opcode::GetSlot || testsItsOwnGuess(code.at(slotRead)) || code.at(slotRead).key() != written.key())
        return { };
    if (code.blockOf(slotRead) != walker.currentBlock() || !code.at(slotRead).traits().isRemovableWhenUnused())
        return { };
    for (ValueID built = code.lastOperationOf(walker.currentBlock()); built != slotRead; built = code.at(built).previous) {
        if (!built || !code.at(built).traits().isRemovableWhenUnused())
            return { };
    }
    return { index.stripCasts(written.objectOperand()), written.key() };
}

static void removeRepeatedCasesInUnionSwitches(Vector<CaseOfUnionSwitches, 16>& cases)
{
    Vector<uint32_t, 16> order;
    for (uint32_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex)
        order.append(caseIndex);
    AOT::stableSort(order, { }, [&](uint32_t caseIndex) {
        return std::pair { cases[caseIndex].number, cases[caseIndex].string };
    });
    Vector<bool, 16> isRepeated;
    isRepeated.fill(false, cases.size());
    for (size_t place = 1; place < order.size(); ++place) {
        const CaseOfUnionSwitches& earlier = cases[order[place - 1]];
        const CaseOfUnionSwitches& later = cases[order[place]];
        isRepeated[order[place]] = earlier.number == later.number && earlier.string == later.string;
    }
    uint32_t caseIndex = 0;
    cases.removeAllMatching([&](const CaseOfUnionSwitches&) {
        return isRepeated[caseIndex++];
    });
}

static void removeCasesThatTypeRulesOutInUnionSwitches(Walker& walker, TypeID typeOfSubject, bool comparesTexts, Vector<CaseOfUnionSwitches, 16>& cases)
{
    TypingContext typing = walker.typing();
    cases.removeAllMatching([&](const CaseOfUnionSwitches& chainCase) {
        return isNeverType(comparesTexts ? narrowByStringCase(typing, typeOfSubject, chainCase.string) : narrowByIntegerCase(typing, typeOfSubject, chainCase.number));
    });
}

static TerminatorDraft draftOfSwitchInUnionSwitches(Reference subject, bool comparesTexts, std::span<const CaseOfUnionSwitches> cases, Origin origin)
{
    TerminatorDraft terminator(comparesTexts ? TerminatorKind::SwitchString : TerminatorKind::Switch, origin);
    terminator.setSubjectOperand(subject);
    if (comparesTexts) {
        Vector<StringID, 16> strings;
        for (const CaseOfUnionSwitches& chainCase : cases)
            strings.append(chainCase.string);
        terminator.setStrings(strings.span());
        return terminator;
    }
    Vector<int64_t, 16> numbers;
    for (const CaseOfUnionSwitches& chainCase : cases)
        numbers.append(chainCase.number);
    terminator.setCases(numbers.span());
    return terminator;
}

namespace {

class RowOfAlternativesInUnionSwitches {
public:
    RowOfAlternativesInUnionSwitches(Walker& walker, BodyBlock head)
        : m_walker(walker)
        , m_index(walker.currentBodyIndex())
        , m_head(head)
    {
    }

    bool find()
    {
        ValueID tested = valueTestedByBranchOf(m_head);
        Alternative first;
        if (!tested || !readAlternative(m_head, tested, first))
            return false;
        m_isDisjunction = !first.isNegated;
        m_comparesTexts = first.constant.isText;
        m_rootOfSubject = m_index.stripCasts(first.subject);
        m_subject = m_walker.referenceOf(first.subject);
        if (!m_rootOfSubject || !m_subject || !m_subject.isValue())
            return false;
        if (!comparesSubjectItselfInUnionSwitches(m_walker.code(), m_walker.referenceOf(first.comparison), m_subject))
            return false;
        m_readOfSubject = findReadThatMayBeRepeatedInUnionSwitches(m_walker, m_rootOfSubject, m_subject);
        m_cases.append(CaseOfUnionSwitches { first.constant.number, first.constant.string, { } });

        for (BodyBlock block = m_head; tested && readOperator(block, tested);) {
            block = m_index.block(m_join);
            tested = parameterThatJoinOnlyTests(block);
        }
        if (!m_join)
            return false;
        removeRepeatedCasesInUnionSwitches(m_cases);
        return true;
    }

    bool comparesTexts() const { return m_comparesTexts; }
    bool isDisjunction() const { return m_isDisjunction; }
    Reference subject() const { return m_subject; }
    Vector<CaseOfUnionSwitches, 16>& cases() { return m_cases; }
    BlockID blockThatAnswersAtOnce() const { return m_blockThatAnswersAtOnce; }
    ValueID answerHandedOnAtOnce() const { return m_answerHandedOnAtOnce; }
    BlockID blockOfLastAlternative() const { return m_blockOfLastAlternative; }
    ValueID lastAlternative() const { return m_lastAlternative; }
    std::span<const BlockID> blocksPassedBy() const LIFETIME_BOUND { return m_blocksPassedBy.span(); }

private:
    struct Alternative {
        ValueID answer;
        ValueID comparison;
        ValueID subject;
        ConstantOfUnionSwitches constant;
        bool isNegated { false };
    };

    ValueID valueTestedByBranchOf(BodyBlock block) const
    {
        BodyTerminator terminator = block.terminator();
        if (terminator.kind() != TerminatorKind::Branch || terminator.numberOfTargets() != 2)
            return { };
        ValueID condition = terminator.as<TerminatorKind::Branch>().conditionOperand();
        if (!isOperationOfBlockInUnionSwitches(m_index, block, condition) || m_index.opcodeOf(condition) != Opcode::ToBoolean || !m_index.hasOneUse(condition))
            return { };
        return m_index.value(condition).operand(OperandIndex::ToBoolean::value);
    }

    bool readAlternative(BodyBlock block, ValueID answer, Alternative& alternative) const
    {
        if (!isOperationOfBlockInUnionSwitches(m_index, block, answer))
            return false;
        alternative.answer = answer;
        alternative.comparison = answer;
        if (m_index.opcodeOf(answer) == Opcode::LogicalNot) {
            alternative.isNegated = true;
            alternative.comparison = m_index.value(answer).operand(OperandIndex::LogicalNot::value);
            if (!isOperationOfBlockInUnionSwitches(m_index, block, alternative.comparison))
                return false;
        }
        if (m_index.opcodeOf(alternative.comparison) != Opcode::StrictEq)
            return false;
        BodyValue equality = m_index.value(alternative.comparison);
        for (uint32_t constantIndex = 0; constantIndex < 2; ++constantIndex) {
            std::optional<ConstantOfUnionSwitches> constant = caseBehindCastsInUnionSwitches(m_index, equality.operand(constantIndex));
            if (!constant)
                continue;
            alternative.subject = equality.operand(1 - constantIndex);
            alternative.constant = *constant;
            return !isConstantBehindCastsInUnionSwitches(m_index, alternative.subject);
        }
        return false;
    }

    bool handsOnTestedValueAlone(BodyTerminator terminator, uint32_t targetIndex, BodyTarget target, ValueID tested) const
    {
        if (!target.arguments().empty() || target.proofs().size() != 1)
            return false;
        std::optional<BodyEdgeResult> result = m_index.edgeResult(terminator, targetIndex, target, 0);
        return result && result->subject == tested;
    }

    bool readsSubjectAnew(BodyBlock block, ValueID value) const
    {
        ValueID root = m_index.stripCasts(value);
        if (!m_readOfSubject.object || !isOperationOfBlockInUnionSwitches(m_index, block, root) || m_index.opcodeOf(root) != Opcode::PropGet)
            return false;
        BodyOperation<Opcode::PropGet> read = m_index.value(root).as<Opcode::PropGet>();
        return !read.isFreeName() && read.key() == m_readOfSubject.key && m_index.stripCasts(read.objectOperand()) == m_readOfSubject.object;
    }

    bool holdsNothingBesideAlternative(BodyBlock block, const Alternative& alternative) const
    {
        for (BodyValue operation : block.values()) {
            if (operation.id() == alternative.answer)
                continue;
            bool isPartOfAlternative = operation.id() == alternative.comparison || primitiveConstants.contains(operation.opcode()) || operation.opcode() == Opcode::Upcast || readsSubjectAnew(block, operation.id());
            if (!isPartOfAlternative || m_index.useCount(operation.id()) > 1)
                return false;
        }
        return true;
    }

    bool readOperator(BodyBlock block, ValueID tested)
    {
        BodyTerminator terminator = block.terminator();
        uint32_t indexOfAnswer = m_isDisjunction ? 0 : 1;
        BodyTarget toAnswer = m_isDisjunction ? terminator.taken() : terminator.notTaken();
        BodyTarget toRightSide = m_isDisjunction ? terminator.notTaken() : terminator.taken();
        if (!handsOnTestedValueAlone(terminator, indexOfAnswer, toAnswer, tested) || !handsOnTestedValueAlone(terminator, 1 - indexOfAnswer, toRightSide, tested))
            return false;

        BodyBlock answersAtOnce = m_index.block(toAnswer.block());
        if (!mayBePassedByInUnionSwitches(m_index, m_head, answersAtOnce, 1) || answersAtOnce.numberOfParameters() != 1 || answersAtOnce.numberOfValues() || answersAtOnce.terminator().kind() != TerminatorKind::Jump)
            return false;
        ValueID handedOn = answersAtOnce.parameter(0);
        BodyTarget toJoin = answersAtOnce.terminator().next();
        if (!toJoin.proofs().empty() || toJoin.arguments().size() != 1 || toJoin.arguments()[0] != handedOn || !m_index.hasOneUse(handedOn))
            return false;

        BodyBlock rightSide = m_index.block(toRightSide.block());
        if (!mayBePassedByInUnionSwitches(m_index, m_head, rightSide, 1) || rightSide.numberOfParameters() != 1 || !m_index.isUnused(rightSide.parameter(0)))
            return false;
        if (!rightSide.numberOfValues()) {
            if (rightSide.terminator().kind() != TerminatorKind::Jump || rightSide.terminator().next().numberOfIncomingValues())
                return false;
            rightSide = m_index.block(rightSide.terminator().next().block());
            if (!mayBePassedByInUnionSwitches(m_index, m_head, rightSide, 1) || rightSide.numberOfParameters())
                return false;
        }
        if (rightSide.terminator().kind() != TerminatorKind::Jump)
            return false;
        BodyTarget fromRightSide = rightSide.terminator().next();
        if (fromRightSide.block() != toJoin.block() || !fromRightSide.proofs().empty() || fromRightSide.arguments().size() != 1)
            return false;

        Alternative alternative;
        if (!m_index.hasOneUse(fromRightSide.arguments()[0]) || !readAlternative(rightSide, fromRightSide.arguments()[0], alternative))
            return false;
        if (alternative.isNegated == m_isDisjunction || alternative.constant.isText != m_comparesTexts)
            return false;
        if (m_index.stripCasts(alternative.subject) != m_rootOfSubject && !readsSubjectAnew(rightSide, alternative.subject))
            return false;
        if (!holdsNothingBesideAlternative(rightSide, alternative) || m_index.numberOfIncomingEdges(toJoin.block()) != 2 || toJoin.block() == m_head.id())
            return false;

        m_cases.append(CaseOfUnionSwitches { alternative.constant.number, alternative.constant.string, { } });
        if (block.id() != m_head.id())
            m_blocksPassedBy.append(block.id());
        m_blocksPassedBy.append(answersAtOnce.id());
        m_blocksPassedBy.append(toRightSide.block());
        if (rightSide.id() != toRightSide.block())
            m_blocksPassedBy.append(rightSide.id());
        m_blockThatAnswersAtOnce = answersAtOnce.id();
        m_answerHandedOnAtOnce = handedOn;
        m_blockOfLastAlternative = rightSide.id();
        m_lastAlternative = alternative.answer;
        m_join = toJoin.block();
        return true;
    }

    ValueID parameterThatJoinOnlyTests(BodyBlock join) const
    {
        if (!mayBePassedByInUnionSwitches(m_index, m_head, join, 2) || join.numberOfParameters() != 1 || join.numberOfValues() != 1)
            return { };
        ValueID parameter = join.parameter(0);
        return valueTestedByBranchOf(join) == parameter && m_index.useCount(parameter) == 3 ? parameter : ValueID();
    }

    Walker& m_walker;
    const BodyIndex& m_index;
    BodyBlock m_head;
    bool m_isDisjunction { true };
    bool m_comparesTexts { false };
    ValueID m_rootOfSubject;
    Reference m_subject;
    ReadOfSubjectInUnionSwitches m_readOfSubject;
    Vector<CaseOfUnionSwitches, 16> m_cases;
    BlockID m_blockThatAnswersAtOnce;
    ValueID m_answerHandedOnAtOnce;
    BlockID m_blockOfLastAlternative;
    ValueID m_lastAlternative;
    Vector<BlockID, 16> m_blocksPassedBy;
    BlockID m_join;
};

} // anonymous namespace

void Walker::endWithSwitchOverOwnBlocks(TerminatorDraft& terminator, std::span<const BlockID> targets, BlockID otherwise)
{
    if (!checkOperandKinds(terminator))
        return;
    WantedRepresentations formsWanted = wantedRepresentationsOf(*this, terminator);
    for (uint32_t operandIndex = 0; operandIndex < terminator.numberOfOperands(); ++operandIndex) {
        if (!hasRegister(formsWanted[operandIndex]))
            continue;
        Reference converted = convert(terminator.operand(operandIndex), formsWanted[operandIndex], terminator.origin());
        if (!converted)
            break;
        terminator.setOperand(operandIndex, converted);
    }
    if (isUnreachable())
        return;
    Effects effects = Effects::none();
    if (!declaredBoundOf(terminator.kind).isPure())
        effects = effectsOfTerminator(typing(), terminator, typesOfOperands(terminator));
    if (effects.mayCollect && isUnderHandler())
        effects.mayThrow |= ThrowEffect::onlyForLackOfMemory();
    if (effects.mayThrow) {
        leadHandlerEdge();
        if (isUnreachable())
            return;
    }
    recordTrialEffects(effects);
    m_state.invalidate(effects);
    if (effects.mayThrow && has(Mechanism::Handlers))
        noteStateAtThrowForHandler(terminator.operands());

    BlockID from = m_block;
    uint32_t numberOfCases = static_cast<uint32_t>(targets.size());
    m_code.end(from, terminator, numberOfCases + 1);
    uint32_t firstEdge = m_code.at(from).terminator.firstEdge.index();
    for (uint32_t caseIndex = 0; caseIndex < numberOfCases; ++caseIndex) {
        settleParametersOf(targets[caseIndex]);
        m_code.closeEdge(EdgeID(firstEdge + caseIndex), targets[caseIndex], { });
    }
    settleParametersOf(otherwise);
    m_code.closeEdge(EdgeID(firstEdge + numberOfCases), otherwise, { });

    Vector<BlockID, 16> blocksLedTo(targets);
    blocksLedTo.append(otherwise);
    AOT::sort(blocksLedTo);
    blocksLedTo.shrink(blocksLedTo.size() - std::ranges::unique(blocksLedTo).size());
    for (size_t index = 0; index + 1 < blocksLedTo.size(); ++index)
        arriveAt(blocksLedTo[index], WalkState(m_state));
    arriveAt(blocksLedTo.last(), WTF::move(m_state));
    m_block = { };
    m_state.clear();
}

void Walker::switchOn(Reference subject, std::span<const int64_t> cases, std::span<const BlockID> targets, BlockID otherwise)
{
    RELEASE_ASSERT(cases.size() == targets.size());
    if (isUnreachable())
        return;
    TerminatorDraft terminator(TerminatorKind::Switch, m_origin);
    terminator.setSubjectOperand(subject);
    terminator.setCases(cases);
    endWithSwitchOverOwnBlocks(terminator, targets, otherwise);
}

void Walker::switchOnString(Reference subject, std::span<const StringID> strings, std::span<const BlockID> targets, BlockID otherwise, Origin origin)
{
    RELEASE_ASSERT(strings.size() == targets.size());
    if (isUnreachable())
        return;
    TerminatorDraft terminator(TerminatorKind::SwitchString, origin);
    terminator.setSubjectOperand(subject);
    terminator.setStrings(strings);
    endWithSwitchOverOwnBlocks(terminator, targets, otherwise);
}

void Walker::takeNarrowerNamesPast(std::span<const BlockID> blocksPassedBy)
{
    size_t frameIndex = m_frames.size() - 1;
    for (BlockID passedBy : blocksPassedBy) {
        uint32_t& first = inputBlock(frameIndex, passedBy).firstNarrowerName;
        if (first == narrowerNamesOfDominator)
            first = m_frames[frameIndex].firstNarrowerName;
    }
}

bool Walker::endBlockWithSwitchOverAlternatives(const BodyTerminator& terminator)
{
    if (!has(Mechanism::TypedOperations) || terminator.kind() != TerminatorKind::Branch || isUnreachable())
        return false;
    const BodyIndex& index = currentBodyIndex();
    if (index.hasIrreducibleFlow() || isTailDuplicatingBlock(m_frames.size() - 1))
        return false;

    RowOfAlternativesInUnionSwitches row(*this, index.block(terminator.block()));
    if (!row.find() || row.cases().size() < leastCasesOfUnionSwitches)
        return false;
    if (m_restArrayAllocator && m_frames.last().isOfUnit && std::ranges::contains(row.blocksPassedBy(), m_restArrayAllocationBlock))
        return false;
    Reference subject = row.subject();
    TypeID typeOfSubject = typeOf(subject);
    if (!typeOfSubject || isNeverType(typeOfSubject))
        return false;
    removeCasesThatTypeRulesOutInUnionSwitches(*this, typeOfSubject, row.comparesTexts(), row.cases());
    if (row.cases().size() < leastCasesOfUnionSwitches)
        return false;

    takeNarrowerNamesPast(row.blocksPassedBy());
    BlockID equalsSomeConstant = addBlock();
    BlockID equalsNoConstant = addBlock();
    Vector<BlockID, 16> targets;
    targets.fill(equalsSomeConstant, row.cases().size());
    TerminatorDraft switchOverConstants = draftOfSwitchInUnionSwitches(subject, row.comparesTexts(), row.cases().span(), m_origin);
    endWithSwitchOverOwnBlocks(switchOverConstants, targets.span(), equalsNoConstant);

    auto leadToJoinWith = [&](BlockID block, BlockID passedBy, ValueID handedOn, bool answer) {
        continueIn(block);
        if (isUnreachable() || m_hasFailed)
            return;
        Draft constantAnswer(Opcode::Boolean, m_origin);
        constantAnswer.setBoolean(answer);
        bind(handedOn, constant(WTF::move(constantAnswer)));
        EdgeToTargetOfBody jumpOfBody { passedBy, 0 };
        endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, m_origin), singleElementSpan(jumpOfBody));
    };
    leadToJoinWith(equalsSomeConstant, row.blockThatAnswersAtOnce(), row.answerHandedOnAtOnce(), row.isDisjunction());
    leadToJoinWith(equalsNoConstant, row.blockOfLastAlternative(), row.lastAlternative(), !row.isDisjunction());
    m_block = { };
    m_state.clear();
    return true;
}

bool Walker::endBlockWithNumberOfCondition(const BodyTerminator& terminator)
{
    if (!has(Mechanism::TypedOperations) || isUnreachable() || !isInLoop() || terminator.numberOfTargets() != 2)
        return false;
    const BodyIndex& index = currentBodyIndex();
    auto joinBehind = [&](BlockID side) {
        BodyTerminator end = index.terminator(side);
        return end.kind() == TerminatorKind::Jump ? end.next().block() : BlockID();
    };
    BlockID joinOfSides = joinBehind(terminator.taken().block());
    if (!joinOfSides || index.block(joinOfSides).numberOfParameters() != 1 || joinBehind(terminator.notTaken().block()) != joinOfSides)
        return false;
    if (index.hasIrreducibleFlow() || isTailDuplicatingBlock(m_frames.size() - 1))
        return false;
    Reference condition = referenceOf(terminator.as<TerminatorKind::Branch>().conditionOperand());
    if (!condition || !condition.isValue() || m_code.isConstant(condition.id()) || typeOf(condition) != TypeID(TypeTable::boolean))
        return false;

    BodyBlock head = index.block(terminator.block());
    std::array<BlockID, 2> sides { terminator.taken().block(), terminator.notTaken().block() };
    std::array<ValueID, 2> arguments;
    std::array<int64_t, 2> numbers;
    BlockID join;
    for (size_t side = 0; side < sides.size(); ++side) {
        BodyBlock block = index.block(sides[side]);
        if (!mayBePassedByInUnionSwitches(index, head, block, 1) || block.terminator().kind() != TerminatorKind::Jump)
            return false;
        for (uint32_t parameter = 0; parameter < block.numberOfParameters(); ++parameter) {
            if (!index.isUnused(block.parameter(parameter)))
                return false;
        }
        BodyTarget next = block.terminator().next();
        if (!next.proofs().empty() || next.arguments().size() != 1 || next.block() == head.id() || (join && next.block() != join) || index.numberOfIncomingEdges(next.block()) != 2)
            return false;
        join = next.block();
        arguments[side] = next.arguments()[0];
        std::optional<ConstantOfUnionSwitches> constant = caseBehindCastsInUnionSwitches(index, arguments[side]);
        if (!constant || constant->isText || (constant->number != 0 && constant->number != 1))
            return false;
        numbers[side] = constant->number;
        for (BodyValue operation : block.values()) {
            if (!primitiveConstants.contains(operation.opcode()) && operation.opcode() != Opcode::Convert)
                return false;
        }
    }
    if (sides[0] == sides[1] || numbers[0] == numbers[1])
        return false;
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::BitOr, Opcode::BitXor, Opcode::LShift };
    ValueID joined = index.block(join).numberOfParameters() == 1 ? index.block(join).parameter(0) : ValueID();
    ValueID user = joined ? index.onlyUsingValue(joined) : ValueID();
    while (user && index.opcodeOf(user) == Opcode::Convert)
        user = index.onlyUsingValue(user);
    if (!user || !arithmetic.contains(index.opcodeOf(user)))
        return false;

    takeNarrowerNamesPast(std::span<const BlockID>(sides));
    count(StatisticID::ConditionalsConvertedToNumbers);
    if (!numbers[0]) {
        Draft negated(Opcode::LogicalNot, m_origin);
        negated.setValueOperand(condition);
        condition = build(WTF::move(negated));
    }
    Reference number;
    if (condition) {
        Draft conversion(Opcode::ToNumber, m_origin);
        chooseConservativeFields(conversion);
        conversion.setValueOperand(condition);
        conversion.setForm(OperatorForm::Booleans);
        number = build(WTF::move(conversion));
    }
    if (!number || isUnreachable() || m_hasFailed)
        return true;
    Reference constant = referenceBoundTo(arguments[0]);
    bind(arguments[0], number);
    EdgeToTargetOfBody jumpOfBody { sides[0], 0 };
    endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, m_origin), singleElementSpan(jumpOfBody));
    bind(arguments[0], constant);
    m_block = { };
    m_state.clear();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
