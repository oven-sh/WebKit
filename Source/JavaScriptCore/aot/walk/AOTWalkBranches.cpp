/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkBranches.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperations.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTOpcodeSets.h"
#include "AOTRepresentationRules.h"
#include "AOTWalkCountersOfLoops.h"
#include "AOTWalkTextBuilders.h"
#include <algorithm>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr TerminatorKindSet kindsThatLeadToBlocksInWalkBranches { TerminatorKind::Jump, TerminatorKind::Branch, TerminatorKind::Switch, TerminatorKind::SwitchString, TerminatorKind::SwitchOnShape };

namespace {

class TargetsInWalkBranches {
public:
    explicit TargetsInWalkBranches(const BodyIndex& index)
        : m_index(index)
    {
    }

    BodyTarget at(BlockID block, uint32_t targetIndex)
    {
        if (m_block != block || targetIndex < m_indexOfTarget) {
            m_block = block;
            m_terminator = m_index.terminator(block);
            m_target = m_terminator.targets().begin();
            m_indexOfTarget = 0;
        }
        RELEASE_ASSERT(targetIndex < m_terminator.numberOfTargets());
        for (; m_indexOfTarget < targetIndex; ++m_indexOfTarget)
            ++m_target;
        return *m_target;
    }
    const BodyTerminator& terminator() const LIFETIME_BOUND { return m_terminator; }

private:
    const BodyIndex& m_index;
    BlockID m_block;
    BodyTerminator m_terminator;
    BodyTargets::Iterator m_target;
    uint32_t m_indexOfTarget { 0 };
};

} // anonymous namespace

static uint32_t numberOfEdgesOfDraftInWalkBranches(const TerminatorDraft& draft)
{
    switch (draft.kind) {
    case TerminatorKind::Switch:
        return static_cast<uint32_t>(draft.cases().size()) + 1;
    case TerminatorKind::SwitchString:
        return static_cast<uint32_t>(draft.strings().size()) + 1;
    case TerminatorKind::SwitchOnShape:
        return static_cast<uint32_t>(draft.layoutSets().size()) + 1;
    default:
        return *numberOfFixedTargetsOf(draft.kind);
    }
}

void Walker::endBlockLeadingTo(TerminatorDraft&& draft, std::span<const EdgeToTargetOfBody> edges)
{
    if (isUnreachable())
        return;
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    uint32_t numberOfEdges = static_cast<uint32_t>(edges.size());
    RELEASE_ASSERT(kindsThatLeadToBlocksInWalkBranches.contains(draft.kind));
    RELEASE_ASSERT(numberOfEdges == numberOfEdgesOfDraftInWalkBranches(draft));

    Vector<uint64_t, 16> targetsLedTo;
    for (const EdgeToTargetOfBody& edge : edges)
        targetsLedTo.append(static_cast<uint64_t>(edge.inputBlock.index()) << 32 | edge.targetIndex);
    std::ranges::sort(targetsLedTo);
    RELEASE_ASSERT(std::ranges::adjacent_find(targetsLedTo) == targetsLedTo.end());

    for (const EdgeToTargetOfBody& edge : edges) {
        if (edge.inputBlock == m_frames[frameIndex].block)
            continue;
        if (!index.strictlyDominates(m_frames[frameIndex].block, edge.inputBlock)) {
            fail("a terminator passes by a block that the block which it ends does not dominate"_s);
            return;
        }
        uint32_t& first = inputBlock(frameIndex, edge.inputBlock).firstNarrowerName;
        if (first == narrowerNamesOfDominator)
            first = m_frames[frameIndex].firstNarrowerName;
    }

    TargetsInWalkBranches targets(index);

    if (draft.kind == TerminatorKind::Jump) {
        BodyTarget target = targets.at(edges[0].inputBlock, edges[0].targetIndex);
        if (BlockID head = inputBlock(frameIndex, target.block()).head; head && target.proofs().empty()) {
            Vector<Reference, 8> arguments;
            for (ValueID argument : target.arguments())
                arguments.append(referenceOf(argument));
            if (hasStringsInBuilders() && edgeExitsBuilderLoop(*this, target.block())) {
                StringsFinishedOnEdge textsTakenOut;
                finishStringsInBuildersOnEdge(*this, target.block(), target, arguments, textsTakenOut);
                if (isUnreachable())
                    return;
            }
            closeBackEdge({ }, head, arguments.span());
            return;
        }
    }

    Origin originOfTests = draft.origin();
    originOfTests.isTailCall = false;
    bool areAsTaken = operandsAreAsTaken(draft);
    for (uint32_t operandIndex = 0; operandIndex < draft.numberOfOperands() && !areAsTaken; ++operandIndex) {
        Reference operand = draft.operand(operandIndex);
        if (!operand || draft.operandKind(operandIndex) != OperandKind::Boolean)
            continue;
        TypeID type = typeOf(operand);
        if (!type || isNeverType(type) || isBooleanType(type) || !m_types.at(type).isJavaScriptValue())
            continue;

        Draft test(Opcode::ToBoolean, originOfTests);
        test.setValueOperand(operand);
        chooseConservativeFields(test);
        Reference answer = build(WTF::move(test));
        if (!answer)
            break;
        draft.setOperand(operandIndex, answer);
    }
    if (isUnreachable() || (!areAsTaken && !checkOperandKinds(draft)))
        return;

    WantedRepresentations formsWanted;
    if (!areAsTaken)
        formsWanted = wantedRepresentationsOf(*this, draft);
    for (uint32_t operandIndex = 0; operandIndex < draft.numberOfOperands() && !areAsTaken; ++operandIndex) {
        if (!hasRegister(formsWanted[operandIndex]))
            continue;
        Reference converted = convert(draft.operand(operandIndex), formsWanted[operandIndex], draft.origin());
        if (!converted)
            break;
        draft.setOperand(operandIndex, converted);
    }
    if (isUnreachable())
        return;

    if (!isDeclaredPure(draft.kind)) {
        Effects effects = effectsOfTerminator(typing(), draft, typesOfOperands(draft));
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
            noteStateAtThrowForHandler(draft.operands());
    }

    BlockID from = m_block;
    m_code.end(from, draft, numberOfEdges);
    m_block = { };

    uint32_t firstEdge = m_code.at(from).terminator.firstEdge.index();
    WalkState atEnd = WTF::move(m_state);
    for (uint32_t number = 0; number < numberOfEdges; ++number) {
        if (number + 1 == numberOfEdges)
            m_state = WTF::move(atEnd);
        else
            m_state = atEnd;
        BodyTarget target = targets.at(edges[number].inputBlock, edges[number].targetIndex);
        SetForScope edgeBeingLed(m_edgeOfCodeBeingLed, number);
        leadEdge(EdgeID(firstEdge + number), from, targets.terminator(), edges[number].targetIndex, target);
        if (m_hasFailed)
            return;
    }
    m_block = { };
    m_state.clear();
}

static std::optional<Opcode> oppositeOfRelation(const Operation& test)
{
    if (!allRelations.contains(test.opcode) || test.form() != OperatorForm::Integers)
        return std::nullopt;
    switch (test.opcode) {
    case Opcode::Less:
        return Opcode::GreaterEq;
    case Opcode::LessEq:
        return Opcode::Greater;
    case Opcode::Greater:
        return Opcode::LessEq;
    default:
        return Opcode::Less;
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Walker::leaveBlockAnotherWay(const BodyTerminator& terminator)
{
    switch (terminator.kind()) {
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow: {
        bool hasPassedByWhatFeedsThrow = std::exchange(m_frames.last().hasPassedByWhatFeedsThrow, false);
        if (has(Mechanism::Handlers) && jumpToHandlerInPlaceOfThrow(terminator))
            return true;
        if (!hasPassedByWhatFeedsThrow)
            return false;
        fail("operations in front of a throw were passed by, and the throw has not become a jump"_s);
        return true;
    }
    case TerminatorKind::Branch: {
        if ((!m_versionedLoops.isEmpty() && handTurnOverWhereLesserBoundIsReached(terminator)) || (m_normalizesLoopTests && (branchOnCounterRelation(terminator) || branchOnEndTest(terminator))))
            return true;
        if (!has(Mechanism::Folding))
            return false;
        Reference condition = referenceOf(terminator.as<TerminatorKind::Branch>().conditionOperand());
        if (!condition || !condition.isValue())
            return false;
        if (std::optional<uint32_t> onlyTarget = onlyTargetWhereAllComeToOnePlace(terminator)) {
            EdgeToTargetOfBody edge { terminator.block(), *onlyTarget };
            endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, m_origin), singleElementSpan(edge));
            return true;
        }
        const Operation& made = m_code.at(condition.id());
        bool isLookedThrough = made.opcode == Opcode::LogicalNot || (made.opcode == Opcode::ToBoolean ? isBooleanType(m_code.at(made.valueOperand()).type) : conversionsOfForm.contains(made.opcode));
        return (isLookedThrough || oppositeOfRelation(made)) && branchOnTestBelowNegations(terminator);
    }
    case TerminatorKind::Switch:
        return has(Mechanism::Folding) && endBlockWithSwitchOverShapes(terminator);
    case TerminatorKind::SwitchString:
        return has(Mechanism::Folding) && (endBlockWithSwitchOverShapes(terminator) || branchOnHoistedComparisons(terminator));
    default:
        return false;
    }
}

bool Walker::branchOnHoistedComparisons(const BodyTerminator& terminator)
{
    auto overStrings = terminator.as<TerminatorKind::SwitchString>();
    if (!m_opensGuesses || !has(Mechanism::Guesses) || m_versionedLoops.isEmpty() || overStrings.strings().size() > mostTextsToldApart)
        return false;
    Reference subject = referenceOf(overStrings.subjectOperand());
    if (!subject || !subject.isValue() || !isInvariantInSpecializedVersion(subject))
        return false;
    const BodyIndex& index = currentBodyIndex();
    bool someProofIsRead = false;
    index.forEachEdgeResult(terminator.block(), [&](const BodyEdgeResult& result) {
        someProofIsRead |= !index.isUnused(result.parameter);
    });
    if (someProofIsRead || !tryHoistEntryTest())
        return false;

    count(StatisticID::InvariantStringSwitchesHoisted);
    uint32_t caseIndex = 0;
    for (StringID text : overStrings.strings()) {
        Draft comparison(Opcode::ValueEqualsLiteral, m_origin);
        chooseConservativeFields(comparison);
        comparison.setOperand(0, subject);
        comparison.setLiteral(text);
        Reference isEqual;
        {
            SpecializedPreheaderScope preheaderScope(*this, true);
            isEqual = build(WTF::move(comparison));
        }
        if (!isEqual || m_hasFailed)
            return true;
        BlockID equal = addBlock();
        BlockID next = addBlock();
        branch(isEqual, equal, next);
        continueIn(equal);
        EdgeToTargetOfBody toCase { terminator.block(), caseIndex++ };
        endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, m_origin), singleElementSpan(toCase));
        continueIn(next);
    }
    EdgeToTargetOfBody otherwise { terminator.block(), caseIndex };
    endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, m_origin), singleElementSpan(otherwise));
    return true;
}

bool Walker::branchOnCounterRelation(const BodyTerminator& terminator)
{
    BlockID inputHead = terminator.block();
    size_t frameIndex = m_frames.size() - 1;
    if (m_frames[frameIndex].blocks.isEmpty())
        return false;
    const InputBlock opened = m_frames[frameIndex].blocks[inputHead.index()];
    if (!opened.head || !opened.hasEqualityTest || isUnreachable() || hasStringsInBuilders() || !has(Mechanism::Folding) || !has(Mechanism::UnboxedNumbers))
        return false;
    bool isProven = opened.counterStaysWithinBound;
    std::optional<EqualityLoopTest> test = findEqualityLoopTest(*this, inputHead);
    if (!test)
        return false;
    const BodyIndex& index = currentBodyIndex();
    bool someProofIsRead = false;
    index.forEachEdgeResult(inputHead, [&](const BodyEdgeResult& result) {
        someProofIsRead |= !index.isUnused(result.parameter);
    });
    Reference condition = referenceOf(terminator.as<TerminatorKind::Branch>().conditionOperand());
    Reference counter = referenceOf(index.block(inputHead).parameter(test->position));
    Draft zero(Opcode::Integer, m_origin);
    Reference bound = test->bound ? referenceOf(test->bound) : constant(WTF::move(zero));
    auto isInteger = [&](Reference value) {
        return value && value.isRecord() && m_types.at(typeOf(value)).isInteger();
    };
    if (someProofIsRead || !condition || !condition.isValue() || !isInteger(counter) || counter != Reference::value(m_code.at(opened.head).parameters()[test->position]) || !isInteger(bound))
        return false;

    if (!isProven && (!m_opensGuesses || !canRestartIteration()))
        return false;

    Reference relation = buildIntegerOperator(*this, test->countsUp ? Opcode::Less : Opcode::Greater, counter, bound, m_origin);
    if (!relation || !relation.isValue() || isUnreachable() || m_code.isConstant(relation.id()) || m_code.at(relation.id()).form() != OperatorForm::Integers)
        return false;
    count(StatisticID::EqualityLoopTestsNormalized);
    EdgeToTargetOfBody exit { inputHead, test->exitTarget };
    EdgeToTargetOfBody stay { inputHead, 1 - test->exitTarget };
    BlockID continuing = addBlock();
    BlockID leaving = addBlock();
    BlockID asked = m_block;
    branch(relation, continuing, leaving, true);
    continueIn(continuing);
    Draft narrowing(Opcode::Narrowed, m_origin);
    narrowing.setValueOperand(counter);
    narrowing.setTestOperand(relation);
    narrowing.setOperandIndex(OperandIndex::Less::left);
    narrowing.setIsOnTakenEdge(true);
    Reference narrowed = build(WTF::move(narrowing));
    BlockID next = terminator.targets()[stay.targetIndex].block();
    bool entersNextAlone = !inputBlock(frameIndex, next).head && index.numberOfIncomingEdges(next) == 1 && !index.isLoopHead(next);
    if (narrowed && narrowed.isValue() && narrowed != counter && entersNextAlone)
        noteNarrowerName(next, counter, narrowed);
    if (entersNextAlone && !isUnreachable() && !m_hasFailed) {
        EdgeID noEdge;
        Vector<Reference, 8> handedOn;
        narrowSecondCounters(noEdge, asked, next, 0, handedOn);
    }
    endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, m_origin), singleElementSpan(stay));
    if (m_hasFailed)
        return true;
    continueIn(leaving);
    if (isProven) {
        if (checksBounds() && !isUnreachable())
            checkThatValuesAreTheSame(condition, booleanOfUnit(!test->exitTarget), m_origin);
    } else {
        Reference isReached = buildIntegerOperator(*this, Opcode::StrictEq, counter, bound, m_origin);
        if (!isReached || m_hasFailed || isUnreachable())
            return true;
        BlockID reached = addBlock();
        BlockID passed = addBlock(BlockFrequency::Rare);
        branch(isReached, reached, passed);
        continueIn(passed);
        if (!isUnreachable())
            restartIteration();
        continueIn(reached);
        recordSpeculation();
    }
    endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, m_origin), singleElementSpan(exit));
    return true;
}

bool Walker::branchOnTestBelowNegations(const BodyTerminator& terminator)
{
    Reference condition = referenceOf(terminator.operand(0));
    if (!condition || !condition.isValue())
        return false;

    ValueID test = condition.id();
    bool isNegated = false;
    bool hasPassedNegation = false;
    while (true) {
        const Operation& made = m_code.at(test);
        if (made.opcode == Opcode::LogicalNot) {
            isNegated = !isNegated;
            hasPassedNegation = true;
        } else if (made.opcode == Opcode::ToBoolean ? !isBooleanType(m_code.at(made.valueOperand()).type) : !conversionsOfForm.contains(made.opcode))
            break;
        test = made.valueOperand();
    }
    if (m_code.isConstant(test))
        return false;

    const BodyIndex& index = currentBodyIndex();
    ValueID written = terminator.operand(0);
    LoopID loop = index.loopOf(terminator.block());
    BodyTarget leftTo = isNegated ? terminator.notTaken() : terminator.taken();
    BodyTarget stayedIn = isNegated ? terminator.taken() : terminator.notTaken();
    bool isFromCopy = !index.isParameter(written) && !allRelations.contains(index.opcodeOf(written)) && m_code.blockOf(test) == m_block;
    std::optional<Opcode> relation = isFromCopy && loop && !index.isInLoop(leftTo.block(), loop) && index.isInLoop(stayedIn.block(), loop) ? oppositeOfRelation(m_code.at(test)) : std::nullopt;
    if (!hasPassedNegation && !relation)
        return false;
    bool someProofIsRead = false;
    index.forEachEdgeResult(terminator.block(), [&](const BodyEdgeResult& result) {
        someProofIsRead |= !index.isUnused(result.parameter);
    });
    if (someProofIsRead)
        return false;

    Reference opposite = relation ? buildIntegerOperator(*this, *relation, Reference::value(m_code.at(test).operand(0)), Reference::value(m_code.at(test).operand(1)), m_origin) : Reference();
    bool branchesOnOpposite = opposite && opposite.isValue() && !m_code.isConstant(opposite.id());
    SetForScope negatedTest(m_negatedTest, branchesOnOpposite ? Reference::value(test) : Reference());
    isNegated ^= branchesOnOpposite;
    TerminatorDraft branch(TerminatorKind::Branch, m_origin);
    branch.setConditionOperand(branchesOnOpposite ? opposite : Reference::value(test));
    EdgeToTargetOfBody edges[] { { terminator.block(), isNegated ? 1u : 0u }, { terminator.block(), isNegated ? 0u : 1u } };
    endBlockLeadingTo(WTF::move(branch), edges);
    return true;
}

bool isUnseenWhenUnused(const Walker& walker, const BodyValue& operation)
{
    Opcode opcode = operation.opcode();
    if (resultRuleOf(opcode) == ResultRule::None || boundToOf(opcode) != BoundTo::Nothing || opcodesThatStateTypes.contains(opcode))
        return false;
    if (opcode == Opcode::Builtin && firstReadOfNamedBuiltinIsSeen(walker, operation))
        return false;
    if (operandsMayRunCode(opcode))
        return false;
    return declaredEffectsOfOperation(walker.program(), opcode, operation).isRemovableWhenUnused();
}

static ALWAYS_INLINE bool isPassedUnseenInWalkBranches(const Walker& walker, const BodyIndex& index, BodyBlock block, BodyTarget arriving)
{
    if (index.numberOfIncomingEdges(block.id()) != 1 || block.numberOfParameters() != arriving.numberOfIncomingValues())
        return false;
    BodyTerminator terminator = block.terminator();
    if (terminator.kind() != TerminatorKind::Jump || terminator.numberOfTargets() != 1 || !terminator.next().proofs().empty())
        return false;
    for (BodyValue operation : block.values()) {
        if (operation.opcode() == Opcode::NarrowByField)
            continue;
        if (operation.opcode() == Opcode::NarrowByTest && index.isUnused(operation.id()))
            continue;
        if (!index.isUnused(operation.id()) || !isUnseenWhenUnused(walker, operation))
            return false;
    }
    return true;
}

WayBehindTerminator wayBehindTarget(const Walker& walker, BlockID from, uint32_t targetIndex, BodyTarget target)
{
    const BodyIndex& index = walker.currentBodyIndex();
    WayBehindTerminator way { from, targetIndex, target, { }, 0, target };
    while (way.numberOfPassedBlocks < mostBlocksPassedBehindTerminator) {
        BodyBlock block = index.block(way.arrival.block());
        if (!isPassedUnseenInWalkBranches(walker, index, block, way.arrival))
            break;
        way.passed[way.numberOfPassedBlocks++] = block.id();
        way.arrival = block.terminator().next();
    }
    return way;
}

static ValueID valueBroughtInWalkBranches(const BodyIndex& index, const WayBehindTerminator& way, uint32_t position)
{
    auto broughtByEdge = [&](uint32_t positionInTarget) -> ValueID {
        std::span<const ValueID> arguments = way.target.arguments();
        if (positionInTarget < arguments.size())
            return arguments[positionInTarget];
        std::optional<BodyEdgeResult> result = index.edgeResult(way.from, way.targetIndex, way.target, positionInTarget - static_cast<uint32_t>(arguments.size()));
        return result ? result->subject : ValueID();
    };

    ValueID value = way.numberOfPassedBlocks ? way.arrival.arguments()[position] : broughtByEdge(position);
    for (uint32_t place = way.numberOfPassedBlocks; value && place--;) {
        std::optional<uint32_t> parameter = index.block(way.passed[place]).indexOfParameter(index.stripCasts(value));
        if (!parameter)
            continue;
        value = place ? index.terminator(way.passed[place - 1]).next().arguments()[*parameter] : broughtByEdge(*parameter);
    }
    return value ? index.stripCasts(value) : value;
}

bool comeToSamePlace(const BodyIndex& index, const WayBehindTerminator& left, const WayBehindTerminator& right, ValueID falseOnLeft, ValueID falseOnRight)
{
    uint32_t numberOfValues = left.arrival.numberOfIncomingValues();
    if (left.arrival.block() != right.arrival.block() || numberOfValues != right.arrival.numberOfIncomingValues())
        return false;
    for (uint32_t position = 0; position < numberOfValues; ++position) {
        ValueID value = valueBroughtInWalkBranches(index, left, position);
        ValueID other = valueBroughtInWalkBranches(index, right, position);
        if (!value || (value != other && (value != falseOnLeft || other != falseOnRight)))
            return false;
    }
    return true;
}

ALWAYS_INLINE_INTO_OTHER_FILES std::optional<uint32_t> Walker::onlyTargetWhereAllComeToOnePlace(const BodyTerminator& terminator) const
{
    if (terminator.kind() != TerminatorKind::Branch || terminator.numberOfTargets() != 2)
        return std::nullopt;
    const BodyIndex& index = currentBodyIndex();
    BodyTargets::Iterator target = terminator.targets().begin();
    BodyTarget taken = *target;
    BodyTarget notTaken = *++target;

    auto mayBePassed = [&](BodyTarget led) {
        return index.numberOfIncomingEdges(led.block()) == 1 && index.terminator(led.block()).kind() == TerminatorKind::Jump;
    };
    if (taken.block() != notTaken.block() && !mayBePassed(taken) && !mayBePassed(notTaken))
        return std::nullopt;
    auto endsWhereNoOtherWayComes = [&](BodyTarget led) {
        return index.numberOfIncomingEdges(led.block()) == 1 && !isPassedUnseenInWalkBranches(*this, index, index.block(led.block()), led);
    };
    if (taken.block() != notTaken.block() && (endsWhereNoOtherWayComes(taken) || endsWhereNoOtherWayComes(notTaken)))
        return std::nullopt;

    if (!comeToSamePlace(index, wayBehindTarget(*this, terminator.block(), 0, taken), wayBehindTarget(*this, terminator.block(), 1, notTaken)))
        return std::nullopt;
    return 0;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
