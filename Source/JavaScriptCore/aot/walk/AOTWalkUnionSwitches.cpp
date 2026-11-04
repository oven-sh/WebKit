/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations: a chain of `===` with constants ends its first block in one Switch or SwitchString, a row of `||` that answers in such a switch with two ways, and a switch of which one case is left is a branch.
// At a suspension: nothing. No block that is passed by holds one.
// At a throw: every block that is passed by stands under the handler of the first with the same arguments, and the switch has that handler edge.
// Withheld: each comparison is walked where it is written.

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

// From this many cases on, a test of bits, a jump table or a hash of the string costs less than the comparisons one behind the other.
static constexpr uint32_t leastCasesOfUnionSwitches = 3;

// What is left of a comparison of the subject itself. Where the walk has found a cheaper question (a type, a constant), the switch would ask the dearer one.
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
    // Behind casts. Null: no block that is passed by may read the subject anew.
    ValueID object;
    PropertyKeyID key;
};

} // anonymous namespace

static ValueID valueBehindNamesInUnionSwitches(const Code& code, ValueID value)
{
    while (value && opcodesThatGiveSameValue.contains(code.at(value).opcode)) {
        ValueID source = code.at(value).valueOperand();
        if (source.index() >= value.index())
            return { };
        value = source;
    }
    return value;
}

static bool isOperationOfBlockInUnionSwitches(const BodyIndex& index, BodyBlock block, ValueID value)
{
    return value && block.holds(value) && !index.isParameter(value);
}

static bool isConstantBehindCastsInUnionSwitches(const BodyIndex& index, ValueID value)
{
    ValueID root = index.valueBehindCasts(value);
    return root && !index.isParameter(root) && primitiveConstants.contains(index.opcodeOf(root));
}

// An integer or a text. -0 and NaN are written as doubles, and the code of a Switch finds no whole number past the integers that types track in a boxed subject: none of those is a case.
static std::optional<ConstantOfUnionSwitches> caseBehindCastsInUnionSwitches(const BodyIndex& index, ValueID value)
{
    ValueID root = index.valueBehindCasts(value);
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

// Each block hands its handler the locals as they stand at its own head, so the same handler block is not enough.
static bool standsUnderHandlerOfHeadInUnionSwitches(BodyBlock head, BodyBlock passed)
{
    if (head.hasHandler() != passed.hasHandler())
        return false;
    if (!head.hasHandler())
        return true;
    return head.handler().block() == passed.handler().block() && std::ranges::equal(head.handler().arguments(), passed.handler().arguments());
}

// Whoever leads the edges behind such a block at the head runs what the body runs: no other edge comes there, and it lies in the same turns and under the same handler.
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

// `if (this.kind === 1) .. else if (this.kind === 2) ..`, `return node.kind === 1 || node.kind === 4 || node.kind === 9`: each comparison reads the field anew. The first block has read it out of its slot, without running code,
// and nothing that it has built behind the read writes anything that is seen. A block that is passed by does nothing that anybody sees, so the same read off the same object finds the same value there.
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
    return { index.valueBehindCasts(written.objectOperand()), written.key() };
}

// The first comparison with a constant takes whatever equals it.
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

// What the proven type of the subject rules out folds as well where each comparison is walked by itself.
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

// `return k === 1 || k === 4 || k === 9`, `const is = k !== 1 && k !== 4 && k !== 9`, as the front end writes a logical operator whose value is taken (lower/expression/logical.rs). For each operator, from the innermost out:
//     .. e = StrictEq k, c   t = ToBoolean e   Branch t -> taken once(| 0), notTaken on(| 0)
//     once(p): Jump -> join(p)      on(q): Jump -> next()      next(): f = StrictEq k, d   Jump -> join(f)      join(x): t = ToBoolean x   Branch t -> ..
// The join of one operator is where the next one asks. Behind `&&` the two edges have changed places, and each comparison stands below a LogicalNot.
// It looks at blocks that are not walked yet, and builds nothing.
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
        m_comparisonOfHead = first.comparison;
        m_rootOfSubject = m_index.valueBehindCasts(first.subject);
        m_subject = m_walker.referenceOf(first.subject);
        if (!m_rootOfSubject || !m_subject || !m_subject.isValue())
            return false;
        // In front of the look at the other alternatives: where the first is refused each join asks in its turn, and looks ahead itself.
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
    // `||`: whatever equals a constant answers true. `&&`: false.
    bool isDisjunction() const { return m_isDisjunction; }
    Reference subject() const { return m_subject; }
    ValueID comparisonOfHead() const { return m_comparisonOfHead; }
    Vector<CaseOfUnionSwitches, 16>& cases() { return m_cases; }
    // Of the outermost operator: the block that hands the answer of the left side on to the join, with the parameter that it hands on, and the block of the last comparison, with what that gives.
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

    // Null: the block does not end in a Branch on a ToBoolean of its own that the Branch alone reads.
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

    // The value, behind casts, is a read in the block that finds what the subject was read as.
    bool readsSubjectAnew(BodyBlock block, ValueID value) const
    {
        ValueID root = m_index.valueBehindCasts(value);
        if (!m_readOfSubject.object || !isOperationOfBlockInUnionSwitches(m_index, block, root) || m_index.opcodeOf(root) != Opcode::PropGet)
            return false;
        BodyOperation<Opcode::PropGet> read = m_index.value(root).as<Opcode::PropGet>();
        return !read.isFreeName() && read.key() == m_readOfSubject.key && m_index.valueBehindCasts(read.objectOperand()) == m_readOfSubject.object;
    }

    // Nobody outside the block reads what it makes but for the answer: it dominates no other block.
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

    // `block` ends in a Branch on the truth of `tested`. True: it is one more operator of the row, and the members below speak of it. False: nothing has changed.
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
        // The edge has a block of its own, which takes the proof.
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
        if (m_index.valueBehindCasts(alternative.subject) != m_rootOfSubject && !readsSubjectAnew(rightSide, alternative.subject))
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

    // Its one parameter, where the join may be passed by: the ToBoolean and the two proofs behind it are all that mentions the parameter. Null: the row ends at the join.
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
    ValueID m_comparisonOfHead;
    ValueID m_rootOfSubject;
    Reference m_subject;
    ReadOfSubjectInUnionSwitches m_readOfSubject;
    Vector<CaseOfUnionSwitches, 16> m_cases;
    BlockID m_blockThatAnswersAtOnce;
    ValueID m_answerHandedOnAtOnce;
    BlockID m_blockOfLastAlternative;
    ValueID m_lastAlternative;
    Vector<BlockID, 16> m_blocksPassedBy;
    // Of the last operator that was read. Null: none was.
    BlockID m_join;
};

} // anonymous namespace

// ---- A switch over blocks of the walk's own

// The terminator has its subject and its cases. What walkTerminator() does behind its draft, with edges that are closed at once, as those of branch() are.
void Walker::endWithSwitchOverOwnBlocks(TerminatorDraft& terminator, std::span<const BlockID> targets, BlockID otherwise)
{
    if (!checkOperandKinds(terminator))
        return;
    // convertOperands() is a template of AOTWalk.cpp. This is what it does to a terminator.
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

    // Every edge brings the one state, so a block that several lead to takes it once.
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

void Walker::switchOnTexts(Reference subject, std::span<const StringID> cases, std::span<const BlockID> targets, BlockID otherwise)
{
    RELEASE_ASSERT(cases.size() == targets.size());
    if (isUnreachable())
        return;
    TerminatorDraft terminator(TerminatorKind::SwitchString, m_origin);
    terminator.setSubjectOperand(subject);
    terminator.setStrings(cases);
    endWithSwitchOverOwnBlocks(terminator, targets, otherwise);
}

// ---- In the place of terminators of the body

// Nobody enters a block that is passed by, so what it dominates would find no narrower name: standIn() asks the dominator. What holds in the block being walked holds in all that it dominates.
// A look at a loop has counted on those names.
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
    if (index.hasIrreducibleFlow() || isWalkingBlockForEachWay(m_frames.size() - 1))
        return false;

    RowOfAlternativesInUnionSwitches row(*this, index.block(terminator.block()));
    if (!row.find() || row.cases().size() < leastCasesOfUnionSwitches)
        return false;
    Reference subject = row.subject();
    TypeID typeOfSubject = typeOf(subject);
    if (!typeOfSubject || isNeverType(typeOfSubject))
        return false;
    removeCasesThatTypeRulesOutInUnionSwitches(*this, typeOfSubject, row.comparesTexts(), row.cases());
    if (row.cases().size() < leastCasesOfUnionSwitches)
        return false;

    takeNarrowerNamesPast(row.blocksPassedBy());
    // Two blocks of the walk's own, since every listed case brings the join the same answer: the code of the switch takes cases that lead to one block for one way.
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
