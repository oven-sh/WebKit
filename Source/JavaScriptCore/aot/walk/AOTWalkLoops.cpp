/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTCallingConventions.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLinkTables.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTOperatorForms.h"
#include "AOTPlan.h"
#include "AOTResultRules.h"
#include "AOTTypeNarrowing.h"
#include "AOTWalkCoroutineCallsInternal.h"
#include "AOTWalkCountersOfLoops.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkStrings.h"
#include "AOTWalkTextBuilders.h"
#include "AOTWalkNumberForms.h"
#include "AOTTypeText.h"
#include <algorithm>
#include <array>
#include <optional>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsNotHeldAgainstRowsOfLoops = bookkeepingHeaps | AbstractHeapSet { AbstractHeap::StringStorage };
static_assert(!heapsNotHeldAgainstRowsOfLoops.containsAny(AbstractHeapSet { AbstractHeap::ArrayStorage, AbstractHeap::ArrayLengths }));

static ValueID valueHandedOnInLoops(const BodyIndex& index, ValueID value)
{
    for (;;) {
        value = index.stripCasts(value);
        ValueID tested = index.valueNarrowedInto(value);
        if (!tested)
            return value;
        value = tested;
    }
}

static Vector<bool, 8> parametersHandedRoundUnchangedInLoops(const BodyIndex& index, LoopID loop)
{
    const BodyLoop& record = index.loop(loop);
    BodyBlock head = index.block(record.head);
    Vector<bool, 8> isUnchanged;
    isUnchanged.fill(!index.isEnteredByHandlerEdge(record.head), head.numberOfParameters());
    if (isUnchanged.isEmpty())
        return isUnchanged;

    for (BlockID block : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(block).targets()) {
            if (target.block() != record.head)
                continue;
            std::span<const ValueID> arguments = target.arguments();
            for (uint32_t parameter = 0; parameter < isUnchanged.size(); ++parameter)
                isUnchanged[parameter] = isUnchanged[parameter] && parameter < arguments.size() && valueHandedOnInLoops(index, arguments[parameter]) == head.parameter(parameter);
        }
    }
    return isUnchanged;
}

static void findParametersHandedRoundUnchangedInLoopsWithin(const BodyIndex& index, LoopID outermost, Vector<Vector<bool, 8>>& byLoop)
{
    if (byLoop.isEmpty())
        byLoop.grow(index.numberOfLoops());
    uint32_t numberOfLoopsLeft = index.loop(outermost).numberOfLoopsWithin;
    for (uint32_t loop = outermost.index(); numberOfLoopsLeft && loop < index.numberOfLoops(); ++loop) {
        BlockID head = index.loop(LoopID(loop)).head;
        if (!index.isInLoop(head, outermost))
            continue;
        --numberOfLoopsLeft;
        byLoop[loop].fill(!index.isEnteredByHandlerEdge(head), index.block(head).numberOfParameters());
    }
    for (BlockID block : index.membersOf(outermost)) {
        for (BodyTarget target : index.terminator(block).targets()) {
            if (!index.isLoopHead(target.block()) || !index.isInLoop(block, index.loopOf(target.block())))
                continue;
            BodyBlock head = index.block(target.block());
            Vector<bool, 8>& isUnchanged = byLoop[index.loopOf(target.block()).index()];
            std::span<const ValueID> arguments = target.arguments();
            for (uint32_t parameter = 0; parameter < isUnchanged.size(); ++parameter)
                isUnchanged[parameter] = isUnchanged[parameter] && parameter < arguments.size() && valueHandedOnInLoops(index, arguments[parameter]) == head.parameter(parameter);
        }
    }
}

static bool isComparedByTestOfHeadInLoops(const BodyIndex& index, BlockID inputHead, uint32_t position)
{
    BodyBlock head = index.block(inputHead);
    BodyTerminator end = head.terminator();
    ValueID asked = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!asked || index.isParameter(asked) || !head.holds(asked) || !allRelations.contains(index.opcodeOf(asked)))
        return false;
    BodyValue relation = index.value(asked);
    ValueID parameter = head.parameter(position);
    auto isParameterOrSum = [&](ValueID operand) {
        ValueID value = index.stripCasts(operand);
        if (value == parameter || index.isParameter(value) || index.opcodeOf(value) != Opcode::Add)
            return value == parameter;
        BodyValue sum = index.value(value);
        ValueID left = index.stripCasts(sum.operand(0));
        ValueID right = index.stripCasts(sum.operand(1));
        ValueID addend = left == parameter ? right : right == parameter ? left : ValueID();
        return addend && !index.isParameter(addend) && index.opcodeOf(addend) == Opcode::Integer;
    };
    return isParameterOrSum(relation.operand(0)) || isParameterOrSum(relation.operand(1));
}

static Effects effectsOfMembersAsWrittenInLoops(const TypingContext& context, const BodyIndex& index, BlockID inputHead)
{
    Effects effects = Effects::none();
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        if (!index.isReachable(member))
            continue;
        for (BodyValue operation : index.block(member).values()) {
            effects.merge(effectsOf(context, operation, OperandTypes { }, TypeID()));
            if (effects.mayRunOtherCode())
                return effects;
        }
        effects.merge(effectsOfTerminator(context, index.terminator(member), OperandTypes { }));
        if (effects.mayRunOtherCode())
            return effects;
    }
    return effects;
}

static TypeID widenedInLoops(TypeArena& types, TypeID assumed, TypeID united)
{
    Type record = types.at(united);
    if (!record.isJavaScriptValue())
        return united;
    united = types.withoutBoundsOfFractions(types.withLiteralStringsWidened(united));
    if (!(record.primitives & Primitive::Integer))
        return united;
    if (record.primitives & Primitive::WholeNumber || !record.integers.isSubsetOf(Range::exact()))
        return types.withPrimitives(types.withIntegers(united, Range::full()), Primitive::WholeNumber);

    if (record.integers == Range::int32() || record.integers == Range::uint32())
        return united;

    Range before = types.at(assumed).integers;
    Range widened = record.integers;
    if (before.isEmpty() || widened.minimum < before.minimum)
        widened.minimum = Range::exact().minimum;
    if (before.isEmpty() || widened.maximum > before.maximum)
        widened.maximum = Range::exact().maximum;
    return types.withIntegers(united, widened);
}

TypeID topsOfKindsInLoops(TypeArena& types, TypeID type)
{
    Type record = types.at(type);
    if (!record.isJavaScriptValue())
        return type;
    TypeID tops = types.withLiteralStringsWidened(types.withoutObjects(type));
    if (record.primitives & Primitive::Number)
        tops = types.unionOf(tops, TypeTable::number);
    if (record.primitives & Primitive::Boolean)
        tops = types.unionOf(tops, TypeTable::boolean);
    if (record.primitives & Primitive::AnySymbol)
        tops = types.unionOf(tops, TypeTable::symbol);
    return record.hasObjects() ? types.unionOf(tops, TypeTable::object) : tops;
}


std::optional<std::array<BodyEdgeResult, 2>> boundsProvenOnEdge(const BodyIndex& index, const BodyTerminator& terminator, uint32_t targetIndex)
{
    if (!terminator.isConditionalBranch() || index.isParameter(terminator.operand(0)))
        return std::nullopt;
    BodyValue relation = index.value(terminator.operand(0));
    if (provesOf(relation.opcode()) != Proof::IntegerBound)
        return std::nullopt;

    std::array<BodyEdgeResult, 2> bounds;
    for (uint32_t operandIndex = 0; operandIndex < bounds.size(); ++operandIndex) {
        bounds[operandIndex].proof = Proof::IntegerBound;
        bounds[operandIndex].test = relation.id();
        bounds[operandIndex].subject = relation.operand(operandIndex);
        bounds[operandIndex].operandIndex = operandIndex;
        bounds[operandIndex].isOnTakenEdge = !targetIndex;
    }
    return bounds;
}

bool relationBoundsNothing(const BodyIndex& index, const BodyValue& relation)
{
    static constexpr OpcodeSet numbers { Opcode::Integer, Opcode::Double };
    for (uint32_t place : { 0u, 1u }) {
        ValueID compared = relation.operand(place);
        while (compared && !index.isParameter(compared) && opcodesThatStateTypes.contains(index.opcodeOf(compared)) && index.hasOneUse(compared))
            compared = index.value(compared).operand(0);
        if (!compared || index.isParameter(compared))
            return false;
        Opcode opcode = index.opcodeOf(compared);
        if (!numbers.contains(opcode) && (opcode != Opcode::Call || !index.hasOneUse(compared)))
            return false;
    }
    return true;
}

ValueID relationThatNarrowsBehind(const Walker& walker, const BodyTerminator& terminator)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.reckonsOnWhatItHasBuilt() || !boundsProvenOnEdge(index, terminator, 0))
        return { };

    BodyValue relation = index.value(terminator.operand(0));
    std::array<TypeID, 2> types;
    for (uint32_t operandIndex = 0; operandIndex < types.size(); ++operandIndex) {
        Reference built = walker.referenceOf(relation.operand(operandIndex));
        types[operandIndex] = built && built.isRecord() ? walker.code().at(built.id()).type : TypeID(TypeTable::anything);
    }
    OperandTypes operands { types };
    for (uint32_t operandIndex = 0; operandIndex < types.size(); ++operandIndex) {
        for (EdgeSide side : { EdgeSide::Taken, EdgeSide::NotTaken }) {
            if (typeOfEdgeResult(walker.typing(), relation, operands, operandIndex, side) != types[operandIndex])
                return relation.id();
        }
    }
    return { };
}

LoopPreview makePessimisticPreview(Walker& walker, BlockID inputHead, std::span<const TypeID> entryTypes, std::span<const TypeID> settledAround)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    Vector<bool, 8> isUnchanged;
    if (index.isLoopHead(inputHead))
        isUnchanged = parametersHandedRoundUnchangedInLoops(index, index.loopOf(inputHead));

    LoopPreview preview;
    uint32_t numberOfParameters = index.block(inputHead).numberOfParameters();
    for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
        TypeID entering = parameter < entryTypes.size() ? entryTypes[parameter] : TypeID();
        bool staysAsItEnters = entering && ((parameter < isUnchanged.size() && isUnchanged[parameter]) || !types.at(entering).isJavaScriptValue());
        if (staysAsItEnters)
            preview.typesOfParameters.append(entering);
        else if (parameter < settledAround.size())
            preview.typesOfParameters.append(settledAround[parameter]);
        else {
            TypeID written = index.block(inputHead).parameterType(parameter);
            bool mayBeAbsent = (entering && types.at(entering).primitives & Primitive::Absent) || (written && types.at(written).primitives & Primitive::Absent);
            TypeID lost = types.anything(UnknownSeed::LostByWalk);
            preview.typesOfParameters.append(mayBeAbsent ? types.unionOf(lost, TypeTable::absent) : lost);
        }
    }
    return preview;
}

TypeID widestTypeOfOperation(Walker& walker, const BodyValue& operation)
{
    Opcode opcode = operation.opcode();
    TypeID anything = walker.types().anything(UnknownSeed::LostByWalk);
    Vector<TypeID, 16> types;
    types.fill(TypeID(), numberOfNamedOperandsOf(opcode) + operation.variadicOperands().size());
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        Reference built = walker.referenceOf(operand);
        TypeID type = built && built.isRecord() ? typeOfRecordForLooks(walker, walker.code().at(built.id()).type) : TypeID();
        types[operandIndex] = type ? type : anything;
    });
    OperandTypes operands { types.span() };
    if (opcode == Opcode::NarrowByTest)
        return operands[OperandIndex::NarrowByTest::left];
    if (opcodesThatStateTypes.contains(opcode))
        return operands[0];
    if (resultRuleOf(opcode) == ResultRule::None)
        return { };
    if (opcode == Opcode::ResultAt)
        return anything;

    Draft draft(opcode);
    draft.flags = static_cast<uint8_t>(operation.flags());
    copyFields(operation, draft, [](ValueID operand) {
        return Reference::value(operand);
    });
    TypingContext typing { walker.program(), walker.types(), walker.positions(), TypeLevel::Proven };
    if (opcodesThatMayHaveSeveralResults.contains(opcode) && numberOfResultsOf(typing.program, draft))
        return TypeTable::undefined;
    if (opcode != Opcode::ElemGet)
        return resultTypeOf(typing, draft, operands);
    TypeID before = walker.exchangeResultOfReadBeingTyped(walker.facts().linked().resultOfReadByKey(walker.currentFunction(), operation.id()));
    TypeID type = resultTypeOf(typing, draft, operands);
    walker.exchangeResultOfReadBeingTyped(before);
    return type;
}

bool Walker::wordCutsWalkAtThisHearing()
{
    if (m_hasFailed || m_numberOfTryWhoseWalkIsCutShort)
        return false;
    if (!m_trialVisits.containsIf([](const TrialVisit& trialVisit) { return trialVisit.knowsWhatIsAssumed && !trialVisit.assumed.mayRunOtherCode(); }))
        return false;
    return m_numberOfHearingsUnderQuietTries++ % m_inputs.options.loopVisitAbortPeriod == m_inputs.options.loopVisitAbortPhase;
}

void Walker::renameFrameInTries(size_t from, size_t to)
{
    for (TrialVisit& trialVisit : m_trialVisits) {
        if (trialVisit.frameIndex == from)
            trialVisit.frameIndex = to;
    }
    for (auto& bought : m_loopsInFinalBuild) {
        if (bought.first == from)
            bought.first = static_cast<uint32_t>(to);
    }
    for (ConstantTripLoop& writtenOut : m_constantTripLoops) {
        if (writtenOut.frameIndex == from)
            writtenOut.frameIndex = to;
    }
    if (m_inheritedSummaryHeader && m_inheritedSummaryFrame == from)
        m_inheritedSummaryFrame = to;
    if (from < m_operationsBeingWalkedInFrames.size()) {
        if (m_operationsBeingWalkedInFrames.size() <= to)
            m_operationsBeingWalkedInFrames.grow(to + 1);
        m_operationsBeingWalkedInFrames[to] = m_operationsBeingWalkedInFrames[from];
    }
    if (to > from) {
        m_frameLogSizes.append({ m_bindings.size(), m_inputBlocksInTries.size(), m_expectationsInTries.size(), m_statesOfHandlersInTries.size(), m_placesOfOpenLoopsInTries.size() });
        return;
    }
    FrameLogSizes since = m_frameLogSizes.takeLast();
    auto rename = [&](auto& log, size_t first) {
        for (size_t place = first; place < log.size(); ++place) {
            if (log[place].frame == from)
                log[place].frame = static_cast<uint32_t>(to);
        }
    };
    rename(m_bindings, since.bindings);
    rename(m_inputBlocksInTries, since.inputBlocks);
    rename(m_expectationsInTries, since.expectations);
    rename(m_statesOfHandlersInTries, since.statesOfHandlers);
    rename(m_placesOfOpenLoopsInTries, since.placesOfOpenLoops);
}

TypeID Walker::typeInRowOfValueCarriedRound(BlockID inputHead, size_t placeAmongParameters, TypeID entering)
{
    for (size_t place = m_trialVisits.size(); place--;) {
        const TrialVisit& trialVisit = m_trialVisits[place];
        if (trialVisit.frameIndex != m_frames.size() - 1 || trialVisit.inputHead != inputHead)
            continue;
        LoopSummary& summary = m_loopSummaries[trialVisit.summaryIndex];
        if (summary.isPessimistic)
            break;
        while (summary.types.size() <= placeAmongParameters) {
            summary.types.append(TypeID());
            summary.rungs.append(0);
        }
        TypeID& held = summary.types[placeAmongParameters];
        held = held ? m_types.unionOf(held, entering) : entering;
        return held;
    }
    return m_types.unionOf(m_types.anything(UnknownSeed::LostByWalk), entering);
}

bool Walker::hasOpenTrialVisit(size_t frameIndex, BlockID inputHead) const
{
    const Frame& frame = m_frames[frameIndex];
    return frame.index->isLoopHead(inputHead) && frame.loopsInTrialVisits.get(frame.index->loopOf(inputHead).index());
}

#define FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_TAKES_BACK(macro) \
    macro(block) macro(blockFollowsClosedArms) macro(endOfValues) macro(firstValueListedByBirthInFront) macro(endOfValuesListedByBirthInFront) macro(hasHandler) macro(handlerOnlyClosesIterations) macro(isInPreheader) macro(narrowedValues) macro(firstNarrowerName) macro(listOfNarrowerNamesIsLong) macro(lookedAt) \
    macro(wholeExpectations) macro(shapesOfValues) macro(handlerBindingsBlock) macro(hasPreheaderHandlerBindings) macro(handlerBindings)

#define FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_LEAVES(macro) \
    macro(returnDominator) macro(exitDominator) macro(walkedUpTo) macro(endOfStretch) macro(numberOfStretchesBeingWalked) macro(currentBlockEndCount) macro(placeTakenByEndOfBlockAskedLast) \
    macro(numberOfFramesUpToHandlerBelow) macro(firstOperationToWalk) macro(hasPassedByWhatFeedsThrow) macro(beginOfStretch) macro(loopsBeingVersioned) macro(fullyUnrolledHeader) \
    macro(turnsOfForIn) macro(valuesListedForTurnsWrittenOut) macro(stepWhosePositionEdgesBring) macro(placeOfForInWhosePositionEdgesBring) \
    macro(whereCopyLeavesForCall) macro(whereHeadEnds)

#define FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_LEAVES_AS_LONG_AS_IT_LEAVES_VALUES_BUILT_TWICE(macro) \
    macro(blocksBeingTailDuplicated)

#define FOR_EACH_FIELD_OF_FRAME_IN_TRY(macro) \
    FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_TAKES_BACK(macro) FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_LEAVES(macro) \
    FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_LEAVES_AS_LONG_AS_IT_LEAVES_VALUES_BUILT_TWICE(macro)

#define FOR_EACH_FIELD_OF_FRAME_THAT_LOGS_PUT_BACK(macro) \
    macro(references) macro(expectations) macro(blocks) macro(statesOfHandlers) macro(placesOfStatesOfHandlers) macro(placesOfOpenLoops) \
    macro(narrowerNames) macro(placesOfNarrowerNames) macro(findsNarrowerNamesByReference) macro(narrowerNamesInTables) macro(firstNarrowerNamesInTables) macro(listInTables) \
    macro(duplicatedValues) macro(booleansOfUnit) macro(returnsOfCopy)

#define FOR_EACH_FIELD_OF_FRAME_SET_WHERE_FRAME_IS_LAID(macro) \
    macro(function) macro(body) macro(index) macro(variablesStoredFirst) macro(loopDepthAtCall) macro(numberOfValuesBuiltBefore) macro(typeExpectedOfAnswer) macro(answerIsDeclared) macro(useOfAnswerAsKey) \
    macro(inlineFrame) macro(callsAreCountedByPlan) macro(callCopied) macro(isCopiedByLinkOfChain) macro(inlineeRestartPoint) macro(displacedRestartMark) macro(mayLeaveForCall) macro(isWalkedWhereItsGuardFailed) macro(inlinedHeadBlocks) macro(handlerOfCodeBelow)

#define FOR_EACH_FIELD_OF_FRAME_THAT_ITS_WRITER_PUTS_BACK(macro) \
    macro(returnsInPlaceOfUnit) macro(isOfUnit) macro(countAmongFramesOfFunction) macro(loopsInTrialVisits)

#define FOR_EACH_FIELD_OF_FRAME_THAT_HANGS_ON_NO_STATE(macro) \
    macro(isRecursive) macro(keepsFields) macro(summaryListsEscapes) \
    macro(blockOfLoopDepth) macro(loopDepth) macro(valueHandedBack) macro(hasLookedForValueHandedBack) macro(handlerBlocks) macro(numberOfHandlersThatRead) macro(parametersHandedRoundUnchangedByLoop) \
    macro(outermostLoopsLookedThrough) macro(accessesUnderProvenKeys) macro(usesInLoops) macro(loopsWithBlocksTouched) macro(arithmeticInLoops) macro(operatorsFoundBehindValuesForGuesses) macro(numbersOfLongListsOfNamesOfRoots) macro(endsOfUsesOfTexts)

#define FOR_EACH_FIELD_OF_FRAME_WRITTEN_OUTSIDE_LOOPS_AND_HANDLERS_ALONE(macro) \
    macro(environmentsMade)

#define FOR_EACH_FIELD_OF_FRAME_THAT_ONLY_NUMBERS_WHAT_OTHERS_HOLD(macro) \
    macro(numbersOfNarrowedReferences)

#define FOR_EACH_FIELD_OF_FRAME(macro) \
    FOR_EACH_FIELD_OF_FRAME_IN_TRY(macro) FOR_EACH_FIELD_OF_FRAME_THAT_LOGS_PUT_BACK(macro) FOR_EACH_FIELD_OF_FRAME_SET_WHERE_FRAME_IS_LAID(macro) \
    FOR_EACH_FIELD_OF_FRAME_THAT_ITS_WRITER_PUTS_BACK(macro) FOR_EACH_FIELD_OF_FRAME_THAT_HANGS_ON_NO_STATE(macro) \
    FOR_EACH_FIELD_OF_FRAME_WRITTEN_OUTSIDE_LOOPS_AND_HANDLERS_ALONE(macro) FOR_EACH_FIELD_OF_FRAME_THAT_ONLY_NUMBERS_WHAT_OTHERS_HOLD(macro)

#define COMMA_AND_NAME(name) , name
#define WITHOUT_WHAT_STANDS_IN_FRONT_OF_FIRST_COMMA(nothing, ...) __VA_ARGS__
#define NAMES_BETWEEN_COMMAS(...) WITHOUT_WHAT_STANDS_IN_FRONT_OF_FIRST_COMMA(__VA_ARGS__)

#define FOR_EACH_FIELD_OF_WALKER_IN_TRY(macro) \
    macro(m_state) macro(m_block) macro(m_origin) macro(m_opensGuesses) macro(m_closedArmKeepsSites) macro(m_assignedMemberChoice) macro(m_asksWhetherSlotsAreFilled) macro(m_firstFreeOpenEdge) macro(m_pendingBlocks) macro(m_numberOfClosedArms) \
    macro(m_numberOfIterationsInFrontOfTheirLoops) macro(m_isLeadingHandlerEdge) \
    macro(m_numberOfLoopsThatMayBeLeft) macro(m_numberOfScopesWhereProofsHoldEverywhere) \
    macro(m_loopsOverGenerators) macro(m_environmentCellsOfUnit) macro(m_operationsCopiedAtWrittenCall) macro(m_closuresThatAllocationAloneTakes) macro(m_valuesDefinedBehindBirth) macro(m_valuesWrittenBehindBirth) macro(m_layoutOfBirthThatListsWhatIsBehind) macro(m_coldCopyDepth) macro(m_walksCopyWhereHolderIsNoBuiltin) macro(m_walksCopyForWhatIsNoBuiltin) macro(m_holderThatIsNoBuiltin) macro(m_genericVersionsInsideSpecialized) \
    macro(m_isWalkingAccess) macro(m_pollInFrontOfLoopBeingEntered) macro(m_edgeOfCodeBeingLed) macro(m_negatedTest) macro(m_callCopiedByLinkOfChain) \
    macro(m_numberOfFramesAtCallCopiedByLinkOfChain) macro(m_callIsAccountedFor) macro(m_calleeAccountedFor) \
    macro(m_holdsReadsOfVariablesAsConstants) macro(m_countsOfUnit) macro(m_speculationCount) macro(m_hoistedArrayGuardCount) macro(m_hoistedElementCheckCount) macro(m_hoistedNumberReadCount) macro(m_mechanisms) \
    macro(m_versionedLoops) macro(m_loopsLeftAtCaughtThrows) macro(m_sharedGenericVersions) macro(m_listAskedForStorageOfNumbersInVain) macro(m_receiverOfLargerMethod) macro(m_walksCleanupBesideCopyOfLoop) macro(m_copyThatMakesNewList) macro(m_runOverBoxedValuesTakesNewListOfCopyFirst) macro(m_listThatRunOverBoxedValuesTakesFirst) macro(m_receiverOfCopyThatMakesNewList) macro(m_depthOfCopyThatMakesNewList) \
    macro(m_hasURLsThatAreNeverMade) \
    macro(m_requestedLiteralStorage) \
    macro(m_unspecifiedConstants) macro(m_valueBeingWalked) macro(m_skippedFreshCellCount) macro(m_iterationRestartCount) macro(m_undoneStoreCount) macro(m_versionedLoopCount) macro(m_versionedLoopNestCount) \
    macro(m_expandedObjectAssignCount) macro(m_duplicatedJoinCount) macro(m_threadedConditionCount) macro(m_fieldCalleeSpeculationCount) macro(m_genericInstanceCheckCount) macro(m_genericMemberLookupCount) macro(m_emptySlotCaseCount) macro(m_foldedSliceCount) \
    macro(m_numbersOfProofsChecked) macro(m_untestableProofCount) macro(m_impliedProofCount) macro(m_unsatisfiableProofCheckCount) macro(m_foldedProofCheckCount) \
    macro(m_generatorsTakenByConsumers) macro(m_generatorMethodsTested) macro(m_generatorsWalkedInConsumers) macro(m_hasTakenGeneratorsSteppedByHand) macro(m_endOfManualIteration) macro(m_resumptionsToLeave) macro(m_numberOfFramesOverHandlersOfCode) macro(m_believedAheadOfEdges) macro(m_leadsIntoPlainWalkThatIsKept) \
    macro(m_numberOfFramesAtRowOfShapeTests) macro(m_closedArmOfRowOfShapeTests) macro(m_stretchWithArmsAsWritten) macro(m_mergedFreshObjectCount) macro(m_numberOfRecordsBuiltToAskGuardsAgain) macro(m_carriesSlotsOfObjects)

#define FOR_EACH_FIELD_OF_WALKER_THAT_LOGS_PUT_BACK(macro) \
    macro(m_entriesOfCopiesOfLoops) macro(m_entriesOfCopiesOfLoopsInTries) macro(m_arrivalsHeld) macro(m_statesOfHandlersInTries) macro(m_placesOfOpenLoopsInTries) \
    macro(m_inputBlocksInTries) macro(m_expectationsInTries) macro(m_code) macro(m_frames) macro(m_numberOfValuesWalkedAndKept) macro(m_hasFailed) macro(m_typesShown) macro(m_typesAsked) \
    macro(m_failure) macro(m_kindOfFailure) macro(m_reports) macro(m_openEdges) macro(m_bindings) macro(m_sharedConstants) macro(m_numberOfSharedConstants) \
    macro(m_narrowedTypedLast) macro(m_copiesOfConstants) macro(m_numberOfCopiesOfConstants) macro(m_indexedIterations) macro(m_inputHeadsOfIterationsAddedInTries) \
    macro(m_typesChangedInTries) macro(m_preheaderDepthUndoLog) macro(m_sharedConstantsAddedInTries) macro(m_copiesOfConstantsAddedInTries) macro(m_fieldsOfScopesOfHeads) \
    macro(m_scopesThatClosuresHold) macro(m_scopesWhoseFieldsHaveComeRound) macro(m_blocksWithFieldsOfScopesInOrder) macro(m_inputHeadsOfIterations) \
    macro(m_openLoops) macro(m_preheaderDepths) macro(m_formAskedAhead) macro(m_coroutineState) macro(m_keptByStrings) macro(m_stringsInBuilders) \
    macro(m_stringInBuilderIndex)

#define FOR_EACH_FIELD_OF_WALKER_SET_WHERE_UNIT_BEGINS(macro) \
    macro(m_isWalkingLastAlone) macro(m_inputs) macro(m_arrivals) macro(m_mechanismsOfBuild) macro(m_stepsOfBuildByOpcode) macro(m_stepsOfWalkByOpcode) macro(m_workerHasBegunUnit) macro(m_newestWalkerOfWorker) \
    macro(m_hasBegunUnit) macro(m_explainsClosedAccessesTo) macro(m_positionsOfBuild) macro(m_positions) macro(m_positionsKnowingWhatFunctionsHandBack) macro(m_positionsWithoutProofs) \
    macro(m_storesOfCapturedValues) macro(m_way)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ITS_WRITER_PUTS_BACK(macro) \
    macro(m_valuesStoodInFor) macro(m_restArrayAllocationBlock) macro(m_restArrayAllocator) macro(m_accessWhoseKeyIsTested) macro(m_depthOfCopiesAroundThatAccess) macro(m_accessWhoseKeyArmHolds) macro(m_endOfKeyHeldByArm) macro(m_depthOfCopiesAroundKeyHeldByArm) macro(m_staticFieldSeenInVariableByArm) macro(m_isMakingObjectsOfConstant) \
    macro(m_trialVisitDepth) macro(m_nestingDepth) macro(m_serialOfNewestMark) macro(m_loopsInFinalBuild) macro(m_loopReentries) macro(m_constantTripLoops) macro(m_reversionableLoops) macro(m_frameLogSizes) macro(m_buysTwoBuilds) \
    macro(m_numberOfFormsHeardAsArms) macro(m_operationsThatRanOtherCodeOfRow) macro(m_guardsOfStoresAskedOfRow) macro(m_previewOutOfRow) macro(m_listsAreStruckInRow) macro(m_isInt32VersionRejected) macro(m_hasInt32Version) macro(m_otherCodeStaysInTypedBuildOfRow) macro(m_otherCodeIsKeptOutOfFirstCopyInVainOfRow) macro(m_otherCodeStayedInVainOfRow) macro(m_readsAskSitesThoughCallStaysOfRow) macro(m_sumsAreNotAskedInRow) macro(m_keysAreReadOffLookInRow) macro(m_keyWasTakenForCounterAskedInFront) macro(m_walksCopyThatAsksCountersAlone) macro(m_headOfTypedBuildOverListsTestedInFront) \
    macro(m_frameOfTypedBuildOverListsTestedInFront) macro(m_listsMadeHereOfTypedBuildOverListsTestedInFront) macro(m_inheritedSummaryHeader) macro(m_inheritedSummaryFrame) macro(m_inheritedEntryTypes) macro(m_inheritedParameterTypes) \
    macro(m_inheritedWideningRungs) macro(m_inheritedRarelyWidenedParameters) macro(m_inheritedOverreadWidenedParameters) macro(m_firstOtherCodeRanInCopyOfRow) macro(m_summaryHasUninlinedCall) macro(m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces) macro(m_turnsOfRowTakeApartListsFromInFront) macro(m_handlersRunOtherCode) macro(m_rowSaysTurnsAreQuietOutsideArms) macro(m_effectsOutsideArmsOfRow) \
    macro(m_testsSeenOfRow) macro(m_missingTestFinder) macro(m_isVersioningDryRun) macro(m_unswitchCandidates) macro(m_unswitchCollectors) macro(m_operatorsMadeInFrontOfLoops) macro(m_hoistedRows) macro(m_fieldArrayReads) macro(m_ordinalOfCopyOfLoop) macro(m_walksCopyWithoutSomeIterationGuesses) macro(m_walksCopyWithoutQuestionsOfEveryTurn) macro(m_walksCopyForAnotherKindOfStorage) macro(m_walksLoopOverBoxedNumbers) macro(m_walksNumericListCopy) macro(m_loopOfOtherKind) macro(m_frontsSharedWithTypedBuilds) macro(m_restartablePreviewHeader) \
    macro(m_numberOfStretchesBehindBytesOfMembers) macro(m_numbersOfFramesOfFunctions) macro(m_resultOfReadBeingTyped) macro(m_resultOfReadIsRemembered) macro(m_contentsBeingTyped) macro(m_handlerMadeOverCopies) \
    macro(m_numberOfFramesBelowHandlerMade) macro(m_isBuildingInPlace) macro(m_rowOfCopyFoundUnderKey) macro(m_hasHoistedFields) macro(m_readArm) macro(m_loopCheckedForStores) macro(m_numberOfLoopsAroundEdgeBeingLed) macro(m_readWhoseGuessIsOpen) macro(m_guardedReadArray) macro(m_frameOfFusedChain) macro(m_fusesNoChain) macro(m_currentMethodChain) macro(m_weighsWhatIsWritten) macro(m_makesLetterOfStep)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ITS_WRITER_SETS_AND_CLEARS(macro) \
    macro(m_dropsChecksOfProofs) macro(m_generatorReadTested) \
    macro(m_hoistedDiamond) macro(m_threadedCondition) macro(m_threadedConditionValue) macro(m_threadedConditionFrame) \
    macro(m_numberOfFieldsOfScopesOfHeadBeingOpened) macro(m_pendingCarriedResultCount) macro(m_buildsTestsBesidePlainWalk) macro(m_expectationsRememberedForLook) macro(m_typesOfInputsInLook) \
    macro(m_looksTakeFormsBehindLooksAsBuilt) macro(m_walksCopyWhereItsGuardFailed) macro(m_armOfNextFusedChain) macro(m_nextCopyIsOfMethodThatProtocolCalls) macro(m_expectedOfArgumentsOfCopies) macro(m_purchaseAsksWhetherTurnsCanBeQuiet) macro(m_resultOfCallBeingTyped) macro(m_valueBeforePendingStore) macro(m_pendingUndoableStore) macro(m_isUndoingStores)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_CHOOSES_WHAT_IS_TESTED(macro) \
    macro(m_whatMeetsInParameters) macro(m_keysStoredInFrontOfLastRow) macro(m_valuesThatEveryWayHandsToParameters) macro(m_loopsWhosePushFillsLiteral)

#define FOR_EACH_FIELD_OF_WALKER_THAT_HANGS_ON_NO_STATE(macro) \
    macro(m_typesHeldOfSlots) \
    macro(m_listsThatCallsAloneTake) \
    macro(methodsGivenUnderKeys) macro(m_storesIntoRowsOfBlocks) macro(m_usersWithinBlocks) macro(m_usersOfValuesOfLoops) macro(m_valuesTakenForKeys) macro(m_loopsThatReckonWithElements) macro(m_loopsToUnswitch) macro(m_holdersStoredByName) macro(m_elementReadsOfLoops) macro(m_computedComparisons) macro(m_numbersInListsOfWrittenObjects) macro(m_lastStoresByKeyOfLoops) macro(m_pushesOntoLiterals) macro(m_steppedCounters) macro(m_storesByKeyOfBlocks) macro(m_namesStoredInOutermostLoops) macro(m_whetherElementsOfLoopsMeetWhatGoesRound) macro(receiversMadeInBodies) macro(gettersExpectedInFunction) macro(m_placesSeenToStandInStretches) macro(m_keptByGeneratorsInLoops) \
    macro(m_listsOfMembersByTag) macro(m_membersByTagOfSets) macro(m_types) macro(m_roundsKept) macro(m_listsOfNames) macro(m_numbersOfListsOfNames) \
    macro(m_functionsInReachOfUnit) macro(m_surveysOfConstants) macro(m_constantsThatListFieldsHeldFilled) macro(m_positionsOfSlowBody) macro(m_noTextWasEvaluated) macro(m_positionsKnowingNothing) \
    macro(m_heldPositions) macro(m_valuesReadBehindLoopFoundLast) macro(m_accessesOfSlotsInLoopListedLast) macro(m_accessesOfScopesByFunctions) macro(m_isArrayIteratorProtocolIntact) macro(m_objectPrototypeMayGainName) macro(m_keptByTypedAccesses) \
    macro(m_keptByScalarReplacement) macro(m_keptByUnionTableCalls) macro(m_keptByCopiesOwnKeys) macro(m_keptByInstanceOf) macro(m_keptByPositionsInTexts) macro(m_keptForInt32Promises) macro(m_stepsOfHeadParameters) macro(m_liveness) macro(m_callsListedOfFunction) \
    macro(m_sidesOfChoicesThatArriveAtBlocks) macro(m_questionsAskedByCallsOfEntryBlocks) macro(m_namesGatheredForRoots)

#define FOR_EACH_FIELD_OF_WALKER_KEYED_BY_RECORDS_OR_BLOCKS_OF_CODE(macro) \
    macro(m_blocksBuiltWhereCountStoodForLength) macro(m_sourcesOfHeadParameters) macro(m_headParametersWithSources) macro(m_mergedFreshObjects) macro(m_fillersOfAllocations) \
    macro(m_headsOfClosedLoops) macro(m_genericVersionEntries) macro(m_iterationsOfHeads)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_COUNTS_UP(macro) \
    macro(m_numberOfMarksTaken) macro(m_restartPointCount) macro(m_normalizesLoopTests) macro(m_claimsSecondCounters) macro(m_refusesSecondCounters) macro(m_numberOfOperationsBuiltThatAreSeenIfRunAgain) macro(m_numberOfOperationsBuiltThatMayEnterByValue) macro(m_numberOfHearingsUnderQuietTries)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_NUMBERS_WHAT_STATES_HOLD(macro) \
    macro(m_numbersOfStates) macro(m_answersOfStates)

#define FOR_EACH_FIELD_OF_WALKER_THAT_NUMBERS_WHAT_STATES_OF_OPEN_TURNS_HOLD_AND_IS_EMPTIED_WHERE_NO_WAY_IS_OPEN(macro) \
    macro(m_undoableStores) macro(m_undoableStoreStates)

#define FOR_EACH_FIELD_OF_WALKER_THAT_TRIES_LEARN(macro) \
    macro(m_comparesElementsOfViews) macro(m_summariesRevisitedForCheck) macro(m_summariesAtLastBuildVisit) macro(m_loopSummaries) macro(m_loopSummaryIndices) macro(m_carriedResultRequests) macro(m_trialVisits) macro(m_numberOfTryWhoseWalkIsCutShort) \
    macro(m_typedBuildEndedListsTestedInFront) macro(m_effectsOfLoopKeptLast) macro(m_guardsOfStoresAskedInLoopKeptLast) macro(m_typedBuildAsksGuardsThatItWasBoughtFor) macro(m_inheritedEscapingUses) macro(m_inheritedScopeStores) macro(m_everyTurnOfTypedBuildLeaves) macro(m_readWasKeptFromItsSiteBecauseCallStays) \
    macro(m_turnOfTypedBuildLeavesWithoutTest) macro(m_innerLoopExitsOnLikelyPath) macro(m_typedBuildTestsNothing) macro(m_typedBuildIsLeftAtOtherCodeOnUsualWay) macro(m_sumOfTypedBuildComesRoundUnproven) macro(m_unguardedSumLoops) macro(m_rowsWhoseKeysAreReadOffLook) macro(m_effectsSeededForNextRow) macro(m_loopsWhoseTextsStayOutOfBuilders) macro(m_walkOfLoopBeganTexts) \
    macro(m_frontOfLoopLeftOpen) macro(m_leavesFrontOfLoopOpenBehindWalk) macro(m_genericVersionWouldSpeculate) macro(m_reportOfTypedBuildMadeLast) macro(m_walkOfLoopWasKept) \
    macro(m_headOfLoopKeptLast) macro(m_frameOfLoopKeptLast) macro(m_rejectedUnswitchLoops) macro(m_unswitchFallbackStart) macro(m_isUnswitchedVersionQuiet) macro(m_hasUnswitchFailed) macro(m_typedBuildAskedNoListThatItWasBoughtFor)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_REPORTS(macro) \
    macro(m_loopVisitCount) macro(m_undoneLoopVisitCount) macro(m_abortedLoopVisitCount) macro(m_predictedLoopEffectsCount) macro(m_refutedLoopEffectsCount) macro(m_failedLoopVersioningCount) macro(m_numberOfValuesWalked) macro(m_functionOfFailure) \
    macro(m_failureIsSaid) macro(m_snapshotsHeld) macro(m_whatJudgeOfOperandsRefused)

#define FOR_EACH_FIELD_OF_WALKER_WRITTEN_IN_FRONT_OF_EVERY_READ(macro) \
    macro(m_questionOfTruthLeftToItsOwnTests) macro(m_frameOfThatQuestionOfTruth) macro(m_blockBuiltWhereListsWereMade) macro(m_opcodeBeingWalked) macro(m_operationsBeingWalkedInFrames) macro(m_lookHasAskedAboutUnbuiltValue) macro(m_operandTypes) macro(m_loopThatItsTryHasWalked) macro(m_operationWritesWhatTurnHasMade)

#define FOR_EACH_FIELD_OF_WALKER_WRITTEN_IN_FRONT_OF_EVERY_LOOP(macro) \
    macro(m_argumentsObjectsPutOffBehindEntry)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_WITHHOLDS_WHEN_STALE(macro) \
    macro(m_unitHasMadeScopes) macro(m_numberOfStoresIntoUnmadeRecords) macro(m_parametersThatSomeScanFoundToReadTexts)

#define FOR_EACH_FIELD_OF_WALKER(macro) \
    FOR_EACH_FIELD_OF_WALKER_IN_TRY(macro) \
    FOR_EACH_FIELD_OF_WALKER_THAT_LOGS_PUT_BACK(macro) FOR_EACH_FIELD_OF_WALKER_SET_WHERE_UNIT_BEGINS(macro) FOR_EACH_FIELD_OF_WALKER_THAT_ITS_WRITER_PUTS_BACK(macro) \
    FOR_EACH_FIELD_OF_WALKER_THAT_ITS_WRITER_SETS_AND_CLEARS(macro) FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_CHOOSES_WHAT_IS_TESTED(macro) FOR_EACH_FIELD_OF_WALKER_THAT_HANGS_ON_NO_STATE(macro) \
    FOR_EACH_FIELD_OF_WALKER_KEYED_BY_RECORDS_OR_BLOCKS_OF_CODE(macro) FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_COUNTS_UP(macro) FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_NUMBERS_WHAT_STATES_HOLD(macro) \
    FOR_EACH_FIELD_OF_WALKER_THAT_NUMBERS_WHAT_STATES_OF_OPEN_TURNS_HOLD_AND_IS_EMPTIED_WHERE_NO_WAY_IS_OPEN(macro) FOR_EACH_FIELD_OF_WALKER_THAT_TRIES_LEARN(macro) FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_REPORTS(macro) FOR_EACH_FIELD_OF_WALKER_WRITTEN_IN_FRONT_OF_EVERY_READ(macro) \
    FOR_EACH_FIELD_OF_WALKER_WRITTEN_IN_FRONT_OF_EVERY_LOOP(macro) FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_WITHHOLDS_WHEN_STALE(macro)

#define FOR_EACH_FIELD_OF_WAY_THAT_CHOOSES_COST_OR_PLACE(macro) macro(genericVersionSpeculates) macro(frameIndex) macro(inputHead) macro(entry) macro(genericStart) macro(guessesCountedOn) macro(canUseIterationGuards) macro(hasCallInIteration) macro(hasUninlinedCall) macro(relationThatAsksLesserBound) macro(turnsThatRunOtherCodeComeRound) macro(lastBackEdgeDominator) macro(mayRestartUnderHandler) macro(restartsAtCaughtThrows) macro(startOfCopyWithoutSomeIterationGuesses) macro(arrayGuardedAtHeader) macro(storageGuardedAtHeader) macro(isArrayGuardedAtHeaderWritable) macro(unswitchCandidates)
#define FOR_EACH_FIELD_OF_WAY_THAT_WALK_OF_TURN_WRITES(macro) macro(frontAsksWhatNothingSpeaksFor) macro(valuesAtHeader) macro(elementsReadAhead) macro(isEntered) macro(restartPoint) macro(headAsksLesserBound) macro(headThatAsksLesserBound) macro(effectsCheckBeginning) macro(isLeftAtOtherCodeOnUsualWay) macro(moduleVariablesAtHeader) macro(undoBlocks) macro(guessOfSomeTurnsIsAskedInFront) macro(questionOfEveryTurnIsAskedInFront) macro(isLeftAtAccessOnUsualWay) macro(isUnswitched) macro(hoistedSiteReads) macro(secondCounterTest) macro(secondCounterTestResult)
#define FOR_EACH_FIELD_OF_WAY_ASKED_AGAIN_WHERE_IT_COMES_ROUND(macro) macro(backEdgeChecks) macro(variableBackEdgeChecks)
#define FOR_EACH_FIELD_OF_WAY_USED_WHILE_STATE_HOLDS_ITS_NOTE(macro) macro(lesserBound) macro(boundBesideLesserBound) macro(viewWhoseLengthIsBound) macro(variableThatIsBound) macro(readOfVariableThatIsBound) macro(counterBelowRoom) macro(secondCounterGuards) macro(steppedKeyLimits)
#define FOR_EACH_FIELD_OF_WAY(macro) FOR_EACH_FIELD_OF_WAY_THAT_CHOOSES_COST_OR_PLACE(macro) FOR_EACH_FIELD_OF_WAY_THAT_WALK_OF_TURN_WRITES(macro) FOR_EACH_FIELD_OF_WAY_ASKED_AGAIN_WHERE_IT_COMES_ROUND(macro) FOR_EACH_FIELD_OF_WAY_USED_WHILE_STATE_HOLDS_ITS_NOTE(macro)

struct Walker::SnapshotForTry {
    static void holdListsToFieldsOf(Walker::VersionedLoop& versionedLoop)
    {
        [[maybe_unused]] auto& [NAMES_BETWEEN_COMMAS(FOR_EACH_FIELD_OF_WAY(COMMA_AND_NAME))] = versionedLoop;
    }

    static void holdListsToFieldsOf(Walker::Frame& frame)
    {
#define HOLD_NAME_TO_FIELD(name) static_assert(std::is_member_object_pointer_v<decltype(&Walker::Frame::name)>);
        FOR_EACH_FIELD_OF_FRAME(HOLD_NAME_TO_FIELD)
#undef HOLD_NAME_TO_FIELD
        [[maybe_unused]] auto& [NAMES_BETWEEN_COMMAS(FOR_EACH_FIELD_OF_FRAME(COMMA_AND_NAME))] = frame;
    }

    static void holdListsToFieldsOf(Walker& walker)
    {
#define HOLD_NAME_TO_FIELD(name) static_assert(!std::is_void_v<decltype(walker.name)>);
        FOR_EACH_FIELD_OF_WALKER(HOLD_NAME_TO_FIELD)
#undef HOLD_NAME_TO_FIELD
        [[maybe_unused]] auto& [NAMES_BETWEEN_COMMAS(FOR_EACH_FIELD_OF_WALKER(COMMA_AND_NAME))] = walker;
    }

    struct ConsumerOfOpenGenerator {
        size_t frameIndex { 0 };
        std::unique_ptr<Walker::Frame::DuplicatedValues> duplicatedValues;
#define DECLARE_FIELD_OF_FRAME(name) decltype(Walker::Frame::name) name;
        FOR_EACH_FIELD_OF_FRAME_IN_TRY(DECLARE_FIELD_OF_FRAME)
#undef DECLARE_FIELD_OF_FRAME
        size_t numberOfNarrowerNames { 0 };
        size_t numberOfPlacesOfNarrowerNames { 0 };
        uint32_t listInTables { noNarrowerName };

        ConsumerOfOpenGenerator(const Walker::Frame& frame, size_t frameIndex)
            : frameIndex(frameIndex)
            , duplicatedValues(frame.duplicatedValues ? frame.duplicatedValues->copyForUndo() : nullptr)
#define COPY_FIELD_OF_FRAME(name) , name(frame.name)
            FOR_EACH_FIELD_OF_FRAME_IN_TRY(COPY_FIELD_OF_FRAME)
#undef COPY_FIELD_OF_FRAME
            , numberOfNarrowerNames(frame.narrowerNames.size())
            , numberOfPlacesOfNarrowerNames(frame.placesOfNarrowerNames.size())
            , listInTables(frame.findsNarrowerNamesByReference ? frame.listInTables : noNarrowerName)
        {
        }

        void putBack(Walker::Frame& frame) const
        {
            if (!duplicatedValues)
                frame.duplicatedValues = nullptr;
            else
                frame.duplicatedValues->putBack(*duplicatedValues);
#define PUT_BACK_FIELD_OF_FRAME(name) frame.name = name;
            FOR_EACH_FIELD_OF_FRAME_IN_TRY(PUT_BACK_FIELD_OF_FRAME)
#undef PUT_BACK_FIELD_OF_FRAME
            if (frame.findsNarrowerNamesByReference) {
                frame.bringTablesOfNarrowerNamesTo(listInTables);
                frame.narrowerNamesInTables.shrink(numberOfNarrowerNames);
            }
            frame.narrowerNames.shrink(numberOfNarrowerNames);
            frame.placesOfNarrowerNames.shrink(numberOfPlacesOfNarrowerNames);
        }
    };
    Vector<ConsumerOfOpenGenerator> consumersOfOpenGenerators;
    Vector<size_t, 8> numbersOfReturnsOfCopies;
    std::unique_ptr<Walker::Frame::DuplicatedValues> duplicatedValues;
    decltype(Walker::Frame::booleansOfUnit) booleansOfUnit;
#define DECLARE_FIELD_OF_FRAME(name) decltype(Walker::Frame::name) name;
    FOR_EACH_FIELD_OF_FRAME_IN_TRY(DECLARE_FIELD_OF_FRAME)
#undef DECLARE_FIELD_OF_FRAME
#define DECLARE_FIELD_OF_WALKER(name) std::conditional_t<std::is_same_v<decltype(Walker::name), BlockBeingBuilt>, BlockID, std::remove_reference_t<decltype(Walker::name)>> name;
    FOR_EACH_FIELD_OF_WALKER_IN_TRY(DECLARE_FIELD_OF_WALKER)
#undef DECLARE_FIELD_OF_WALKER
    Code::Mark code;
    struct Hold {
        WTF_MAKE_NONCOPYABLE(Hold);
    public:
        explicit Hold(Walker& walker)
            : m_walker(walker)
        {
            ++m_walker.m_snapshotsHeld;
        }
        ~Hold()
        {
            if (!--m_walker.m_snapshotsHeld && m_walker.m_hasFailed && !m_walker.m_failureIsSaid && m_walker.m_trialVisits.isEmpty())
                m_walker.sayFailure();
        }

    private:
        Walker& m_walker;
    };
    Hold hold;
    Vector<Code::OpenBlock, 8> openBlocks;
    size_t numberOfFrames { 0 };
    size_t numberOfScopesThatClosuresHold { 0 };
    size_t numberOfScopesWhoseFieldsHaveComeRound { 0 };
    size_t numberOfBlocksWithFieldsOfScopes { 0 };
    size_t numberOfTexts { 0 };
    size_t numberOfArrivalsHeld { 0 };
    size_t numberOfTextsLogged { 0 };
    size_t numberOfCodeUnitsLogged { 0 };
    size_t numberOfOpenEdges { 0 };
    size_t numberOfOpenEdgesLogged { 0 };
    size_t numberOfOpenLoops { 0 };
    size_t numberOfOpenLoopsLogged { 0 };
    size_t numberOfStatesOfHandlersLogged { 0 };
    size_t numberOfPlacesOfOpenLoopsLogged { 0 };
    size_t numberOfBindings { 0 };
    size_t numberOfReports { 0 };
    size_t numberOfInputBlocksLogged { 0 };
    size_t numberOfExpectationsLogged { 0 };
    size_t numberOfIterations { 0 };
    size_t numberOfIterationsLogged { 0 };
    size_t numberOfInputHeadsOfIterationsAdded { 0 };
    size_t numberOfTypesChanged { 0 };
    size_t numberOfEntriesOfCopiesOfLoops { 0 };
    size_t numberOfPlacesOfLoopsChanged { 0 };
    size_t numberOfSharedConstantsAdded { 0 };
    size_t numberOfCopiesOfConstantsAdded { 0 };
    uint32_t numberOfValuesWalkedAndKept { 0 };
    std::optional<KeptByCallsOfCoroutines::AwaitOfCallThatMayBeCopied> awaitOfCallThatMayBeCopied;
    Vector<ValueID, 2> awaitsTakenInCopies;
    Vector<NotesOfStrings::URLThatIsNeverMade, 1> urlsThatAreNeverMade;
    size_t numberOfNarrowerNames { 0 };
    size_t numberOfPlacesOfNarrowerNames { 0 };
    uint32_t listInTables { noNarrowerName };

    explicit SnapshotForTry(Walker& walker)
#define COPY_FIELD_OF_FRAME(name) name(walker.m_frames.last().name),
        : duplicatedValues(walker.m_frames.last().duplicatedValues ? walker.m_frames.last().duplicatedValues->copyForUndo() : nullptr),
        booleansOfUnit(walker.m_frames.first().booleansOfUnit),
        FOR_EACH_FIELD_OF_FRAME_IN_TRY(COPY_FIELD_OF_FRAME)
#undef COPY_FIELD_OF_FRAME
#define COPY_FIELD_OF_WALKER(name) name(walker.name),
        FOR_EACH_FIELD_OF_WALKER_IN_TRY(COPY_FIELD_OF_WALKER)
#undef COPY_FIELD_OF_WALKER
        code(walker.m_code.mark())
        , hold(walker)
        , numberOfFrames(walker.m_frames.size())
        , numberOfScopesThatClosuresHold(walker.m_scopesThatClosuresHold.size())
        , numberOfScopesWhoseFieldsHaveComeRound(walker.m_scopesWhoseFieldsHaveComeRound.size())
        , numberOfBlocksWithFieldsOfScopes(walker.m_blocksWithFieldsOfScopesInOrder.size())
        , numberOfTexts(walker.m_stringsInBuilders.size())
        , numberOfArrivalsHeld(walker.m_arrivalsHeld.size())
        , numberOfTextsLogged(walker.m_stringsInBuilders.sizeOfLog())
        , numberOfOpenEdges(walker.m_openEdges.size())
        , numberOfOpenEdgesLogged(walker.m_openEdges.sizeOfLog())
        , numberOfOpenLoops(walker.m_openLoops.size())
        , numberOfOpenLoopsLogged(walker.m_openLoops.sizeOfLog())
        , numberOfStatesOfHandlersLogged(walker.m_statesOfHandlersInTries.size())
        , numberOfPlacesOfOpenLoopsLogged(walker.m_placesOfOpenLoopsInTries.size())
        , numberOfBindings(walker.m_bindings.size())
        , numberOfReports(walker.m_reports.size())
        , numberOfInputBlocksLogged(walker.m_inputBlocksInTries.size())
        , numberOfExpectationsLogged(walker.m_expectationsInTries.size())
        , numberOfIterations(walker.m_indexedIterations.size())
        , numberOfIterationsLogged(walker.m_indexedIterations.sizeOfLog())
        , numberOfInputHeadsOfIterationsAdded(walker.m_inputHeadsOfIterationsAddedInTries.size())
        , numberOfTypesChanged(walker.m_typesChangedInTries.size())
        , numberOfEntriesOfCopiesOfLoops(walker.m_entriesOfCopiesOfLoopsInTries.size())
        , numberOfPlacesOfLoopsChanged(walker.m_preheaderDepthUndoLog.size())
        , numberOfSharedConstantsAdded(walker.m_sharedConstantsAddedInTries.size())
        , numberOfCopiesOfConstantsAdded(walker.m_copiesOfConstantsAddedInTries.size())
        , numberOfValuesWalkedAndKept(walker.m_numberOfValuesWalkedAndKept)
        , numberOfNarrowerNames(walker.m_frames.last().narrowerNames.size())
        , numberOfPlacesOfNarrowerNames(walker.m_frames.last().placesOfNarrowerNames.size())
        , listInTables(walker.m_frames.last().findsNarrowerNamesByReference ? walker.m_frames.last().listInTables : noNarrowerName)
    {
        RELEASE_ASSERT(!walker.m_hasFailed || walker.failureOutlivesTries());
        if (KeptByCallsOfCoroutines* kept = findKeptByCallsOfCoroutines(walker)) {
            awaitOfCallThatMayBeCopied = kept->awaitOfCallThatMayBeCopied;
            awaitsTakenInCopies = kept->awaitsTakenInCopies;
        }
        if (walker.keptByStrings()) {
            const NotesOfStrings& notes = notesOfStrings(walker);
            numberOfCodeUnitsLogged = notes.codeUnitsChanged.size();
            urlsThatAreNeverMade = notes.urlsThatAreNeverMade;
        }
        for (const GeneratorWalkedInConsumer& open : walker.m_generatorsWalkedInConsumers) {
            size_t consumer = open.frameOfTurn == UINT32_MAX ? open.frameOfConsumer : open.frameOfTurn;
            if (consumer + 1 < numberOfFrames && !consumersOfOpenGenerators.containsIf([&](const ConsumerOfOpenGenerator& taken) { return taken.frameIndex == consumer; }))
                consumersOfOpenGenerators.append(ConsumerOfOpenGenerator(walker.m_frames[consumer], consumer));
        }
        for (size_t frameIndex = 0; frameIndex < numberOfFrames; ++frameIndex)
            numbersOfReturnsOfCopies.append(walker.m_frames[frameIndex].returnsOfCopy ? walker.m_frames[frameIndex].returnsOfCopy->size() : 0);
        Vector<BlockID, 16> blocksThatAreOpen;
        auto addOpenBlock = [&](BlockID block) {
            if (block)
                blocksThatAreOpen.append(block);
        };
        addOpenBlock(walker.m_block);
        if (!walker.m_openLoops.isEmpty())
            addOpenBlock(walker.m_openLoops.last().preheaderBlock);
        for (const PendingBlock& pending : walker.m_pendingBlocks)
            addOpenBlock(pending.block);
        std::ranges::sort(blocksThatAreOpen, { }, [](BlockID block) { return block.index(); });
        blocksThatAreOpen.shrink(std::ranges::unique(blocksThatAreOpen).begin() - blocksThatAreOpen.begin());
        for (BlockID block : blocksThatAreOpen)
            openBlocks.append(walker.m_code.stateOf(block));
    }

    void putBack(Walker& walker) const
    {
        ++walker.m_undoneLoopVisitCount;
        walker.m_numberOfValuesWalkedAndKept = numberOfValuesWalkedAndKept;
        if (!walker.failureOutlivesTries()) {
            walker.m_hasFailed = false;
            walker.m_failure = { };
            walker.m_kindOfFailure = InternalErrorKind::Unknown;
        }
        walker.m_frames.shrink(numberOfFrames);
        walker.m_arrivalsHeld.shrink(numberOfArrivalsHeld);
        if (!duplicatedValues)
            walker.m_frames.last().duplicatedValues = nullptr;
        else
            walker.m_frames.last().duplicatedValues->putBack(*duplicatedValues);
#define PUT_BACK_FIELD_OF_FRAME(name) walker.m_frames.last().name = name;
        FOR_EACH_FIELD_OF_FRAME_IN_TRY(PUT_BACK_FIELD_OF_FRAME)
#undef PUT_BACK_FIELD_OF_FRAME
#define PUT_BACK_FIELD_OF_WALKER(name) walker.name = name;
        FOR_EACH_FIELD_OF_WALKER_IN_TRY(PUT_BACK_FIELD_OF_WALKER)
#undef PUT_BACK_FIELD_OF_WALKER
        for (const Walker::BelievedAheadOfEdges& believed : walker.m_believedAheadOfEdges)
            walker.m_code.noteBeliefAheadOfEdges(believed.block, BeliefAheadOfEdges::Listed);
        walker.m_openEdges.putBack(numberOfOpenEdges, numberOfOpenEdgesLogged);
        walker.m_openLoops.putBack(numberOfOpenLoops, numberOfOpenLoopsLogged);
        for (size_t place = 0; place < walker.m_openLoops.size(); ++place)
            RELEASE_ASSERT(!walker.m_headsOfClosedLoops.contains(walker.m_openLoops[place].head.index()));
        walker.truncateStringInBuilderIndex(numberOfTexts);
        walker.m_stringsInBuilders.putBack(numberOfTexts, numberOfTextsLogged);
        while (walker.m_statesOfHandlersInTries.size() > numberOfStatesOfHandlersLogged) {
            StateOfHandlerInTry logged = walker.m_statesOfHandlersInTries.takeLast();
            if (logged.frame >= numberOfFrames)
                continue;
            Frame& frame = walker.m_frames[logged.frame];
            WalkState dropped;
            frame.takeStateOfHandler(logged.handler, dropped);
            if (logged.before) {
                frame.addStateOfHandler(logged.handler, WalkState());
                frame.statesOfHandlers.last() = WTF::move(*logged.before);
            }
        }
        while (walker.m_placesOfOpenLoopsInTries.size() > numberOfPlacesOfOpenLoopsLogged) {
            PlaceOfOpenLoopInTry logged = walker.m_placesOfOpenLoopsInTries.takeLast();
            if (logged.frame < numberOfFrames)
                walker.m_frames[logged.frame].placesOfOpenLoops[logged.loop.index()] = logged.before;
        }
        walker.m_indexedIterations.putBack(numberOfIterations, numberOfIterationsLogged);
        walker.forgetSharedConstantsAddedBehind(numberOfSharedConstantsAdded);
        walker.forgetCopiesOfConstantsAddedBehind(numberOfCopiesOfConstantsAdded);
        while (walker.m_preheaderDepthUndoLog.size() > numberOfPlacesOfLoopsChanged) {
            auto changed = walker.m_preheaderDepthUndoLog.takeLast();
            walker.m_preheaderDepths[changed.first] = changed.second;
        }
        while (walker.m_inputHeadsOfIterationsAddedInTries.size() > numberOfInputHeadsOfIterationsAdded)
            walker.m_inputHeadsOfIterations.remove(walker.m_inputHeadsOfIterationsAddedInTries.takeLast());
        while (walker.m_entriesOfCopiesOfLoopsInTries.size() > numberOfEntriesOfCopiesOfLoops)
            --walker.m_entriesOfCopiesOfLoops.find(walker.m_entriesOfCopiesOfLoopsInTries.takeLast())->value;
        while (walker.m_typesChangedInTries.size() > numberOfTypesChanged) {
            Walker::TypeChangedInTry changed = walker.m_typesChangedInTries.takeLast();
            auto& types = changed.isAsked ? walker.m_typesAsked : walker.m_typesShown;
            if (changed.before)
                types.set(changed.record, changed.before);
            else
                types.remove(changed.record);
        }
        if (KeptByCallsOfCoroutines* kept = findKeptByCallsOfCoroutines(walker)) {
            kept->awaitOfCallThatMayBeCopied = awaitOfCallThatMayBeCopied;
            kept->awaitsTakenInCopies = awaitsTakenInCopies;
        }
        if (walker.keptByStrings()) {
            NotesOfStrings& notes = notesOfStrings(walker);
            while (notes.codeUnitsChanged.size() > numberOfCodeUnitsLogged) {
                NotesOfStrings::CodeUnitChanged changed = notes.codeUnitsChanged.takeLast();
                auto& codeUnits = changed.isBesideCharacter ? notes.codePointsBesideCharacters : notes.codeUnitsBesideCharCodes;
                if (changed.before)
                    codeUnits.set(changed.record, changed.before);
                else
                    codeUnits.remove(changed.record);
            }
            notes.urlsThatAreNeverMade = urlsThatAreNeverMade;
        }
        while (walker.m_bindings.size() > numberOfBindings) {
            Binding binding = walker.m_bindings.takeLast();
            if (binding.frame < walker.m_frames.size())
                walker.m_frames[binding.frame].references.placeOf<NamesOfValues::Writer::PutsBackWhatLogHolds>(binding.input.index()) = binding.previous;
        }
        walker.m_frames.first().booleansOfUnit = booleansOfUnit;
        while (walker.m_inputBlocksInTries.size() > numberOfInputBlocksLogged) {
            InputBlockInTry logged = walker.m_inputBlocksInTries.takeLast();
            if (logged.frame < walker.m_frames.size()) {
                walker.m_frames[logged.frame].blocks[logged.block.index()] = logged.before;
                noteThatBlockIsTouched(walker.m_frames[logged.frame], logged.block);
            }
        }
        while (walker.m_expectationsInTries.size() > numberOfExpectationsLogged) {
            ExpectationInTry logged = walker.m_expectationsInTries.takeLast();
            if (logged.frame < walker.m_frames.size() && !walker.m_frames[logged.frame].expectations.isEmpty())
                walker.m_frames[logged.frame].expectations[logged.value.index()] = logged.before;
        }
        Frame& onTop = walker.m_frames.last();
        if (onTop.findsNarrowerNamesByReference) {
            onTop.bringTablesOfNarrowerNamesTo(listInTables);
            onTop.narrowerNamesInTables.shrink(numberOfNarrowerNames);
        }
        onTop.narrowerNames.shrink(numberOfNarrowerNames);
        onTop.placesOfNarrowerNames.shrink(numberOfPlacesOfNarrowerNames);
        for (const ConsumerOfOpenGenerator& consumer : consumersOfOpenGenerators)
            consumer.putBack(walker.m_frames[consumer.frameIndex]);
        for (size_t frameIndex = 0; frameIndex < numbersOfReturnsOfCopies.size(); ++frameIndex) {
            if (walker.m_frames[frameIndex].returnsOfCopy)
                walker.m_frames[frameIndex].returnsOfCopy->shrink(numbersOfReturnsOfCopies[frameIndex]);
        }
        walker.m_reports.shrink(numberOfReports);
        walker.m_narrowedTypedLast = { };
        walker.m_formAskedAhead = { };
        walker.m_scopesThatClosuresHold.shrink(numberOfScopesThatClosuresHold);
        walker.m_scopesWhoseFieldsHaveComeRound.shrink(numberOfScopesWhoseFieldsHaveComeRound);
        while (walker.m_blocksWithFieldsOfScopesInOrder.size() > numberOfBlocksWithFieldsOfScopes)
            walker.m_fieldsOfScopesOfHeads.remove(walker.m_blocksWithFieldsOfScopesInOrder.takeLast());
        walker.m_code.abandonBackTo(code, openBlocks.span());
    }
};

void Walker::letMentionsOfLoopLeave(BlockID inputHead, std::span<const ValueID> mentionsThatLeave, Vector<FreshObjectKeptAtHead, 2>* kept, bool keepsScopes)
{
    if (!m_state.holdsFreshObjects())
        return;
    if (std::ranges::contains(mentionsThatLeave, everyFreshObject)) {
        Vector<Reference, 8> fresh;
        m_state.forEachFreshObject([&](Reference object) {
            fresh.append(object);
        });
        for (Reference object : fresh)
            letLeave(object);
        return;
    }
    const BodyIndex& index = currentBodyIndex();
    uint32_t firstValue = index.block(inputHead).firstParameter().index();
    keepsScopes &= kept && unitHasMadeScopes();
    Vector<ValueID, 4> onlyRead;
    Vector<ValueID, 4> mayOnlyBorrowScopes;
    auto mention = [&](ValueID value) {
        if (!value || value.index() >= firstValue || m_inputs.options.shouldIgnoreLoopEscapes)
            return;
        if (Reference reference = referenceOf(value); reference && reference.isRecord())
            letLeave(reference);
    };
    auto readsWithoutTakingObject = [&](const BodyValue& operation, uint32_t operandIndex) {
        if (operation.opcode() == Opcode::PropGet)
            return operandIndex == OperandIndex::PropGet::object && !operation.isFreeName() && operation.key() == program().keyOf(BuiltinNames::length);
        static_assert(OperandIndex::ElemGet::object == OperandIndex::ElemHas::object);
        return (operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemHas) && operandIndex == OperandIndex::ElemGet::object && operation.propertyOperand() != operation.objectOperand();
    };
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        BodyBlock block = index.block(member);
        for (BodyValue operation : block.values()) {
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (kept && operand && operand.index() < firstValue && readsWithoutTakingObject(operation, operandIndex) && !std::ranges::contains(mentionsThatLeave, operand)) {
                    if (!onlyRead.contains(operand))
                        onlyRead.append(operand);
                    return;
                }
                if (keepsScopes && operand && operand.index() < firstValue && mentionMayOnlyBorrowScopes(operation, operandIndex)) {
                    mayOnlyBorrowScopes.append(operand);
                    return;
                }
                mention(operand);
            });
        }
        BodyTerminator end = block.terminator();
        end.forEachOperand([&](ValueID operand, auto...) { mention(operand); });
        for (BodyTarget target : end.targets()) {
            for (ValueID argument : target.arguments())
                mention(argument);
        }
        if (block.hasHandler()) {
            for (ValueID argument : block.handler().arguments())
                mention(argument);
        }
    }
    if (!mayOnlyBorrowScopes.isEmpty())
        keepScopesAtMentions(mayOnlyBorrowScopes, mentionsThatLeave, *kept);
    for (ValueID value : onlyRead) {
        Reference reference = referenceOf(value);
        if (!reference || !reference.isValue())
            continue;
        Reference object = referenceTo(underlyingCellOf(m_code, reference.id()));
        if (!m_state.isFresh(object))
            continue;
        Opcode maker = m_code.at(object.id()).opcode;
        if (hasKind(m_types, typeOf(object), OperandKind::Array) || maker == Opcode::CreateUnmappedArguments || maker == Opcode::CreateMappedArguments)
            kept->append(FreshObjectKeptAtHead { value, object });
        else
            letLeave(reference);
    }
}

void Walker::standAheadOfEdges(BlockID block)
{
    m_block = block;
    m_code.noteBeliefAheadOfEdges(block, m_state.holdsFreshObjects() ? BeliefAheadOfEdges::Listed : BeliefAheadOfEdges::Nothing);
    if (m_state.holdsFreshObjects())
        m_believedAheadOfEdges.append(BelievedAheadOfEdges { block, m_state });
}

void Walker::forgetWhatWasBelievedAt(BlockID block)
{
    if (m_code.at(block).beliefAheadOfEdges != BeliefAheadOfEdges::Listed)
        return;
    m_code.noteBeliefAheadOfEdges(block, BeliefAheadOfEdges::GivenUp);
    m_believedAheadOfEdges.removeAllMatching([&](const BelievedAheadOfEdges& believed) {
        return believed.block == block;
    });
}

Walker::TrialVisit* Walker::findTryOfBlockAheadOfEdges(BlockID block)
{
    for (size_t place = m_trialVisits.size(); place--;) {
        TrialVisit& trialVisit = m_trialVisits[place];
        if (trialVisit.frameIndex >= m_frames.size())
            continue;
        if (m_frames[trialVisit.frameIndex].blocks[trialVisit.inputHead.index()].head == block)
            return &trialVisit;
    }
    return nullptr;
}

void Walker::holdWhatArrivesWhereEdgesAreAwaited(BlockID to)
{
    switch (m_code.at(to).beliefAheadOfEdges) {
    case BeliefAheadOfEdges::NoEdgeIsAwaited:
    case BeliefAheadOfEdges::Nothing:
        return;
    case BeliefAheadOfEdges::GivenUp:
        if (!m_leadsIntoPlainWalkThatIsKept)
            fail("an edge arrives at the head of a loop whose walk has ended"_s);
        return;
    case BeliefAheadOfEdges::Listed:
        break;
    }
    size_t believedHere = m_believedAheadOfEdges.reverseFindIf([&](const BelievedAheadOfEdges& believed) {
        return believed.block == to;
    });
    RELEASE_ASSERT(believedHere != notFound);
    TrialVisit* trialVisit = findTryOfBlockAheadOfEdges(to);
    size_t firstGenericVersionTrial = notFound;
    if (!trialVisit && !m_versionedLoops.isEmpty() && m_versionedLoops.last().entry == to) {
        firstGenericVersionTrial = m_trialVisits.reverseFindIf([&](const TrialVisit& ofLoop) {
            return ofLoop.frameIndex == m_versionedLoops.last().frameIndex && ofLoop.inputHead == m_versionedLoops.last().inputHead;
        });
    }
    auto canIgnoreFactLoss = [&](size_t firstTry, Reference holder, bool isFieldOfScope, Reference believed) {
        if (!believed || isFieldOfScope || !holder.isValue() || isEnvironmentAllocation(holder) || !m_state.isFresh(holder) || m_trialVisits[firstTry].frameIndex != m_frames.size() - 1)
            return false;
        if (believed != holder && believed.isValue() && m_state.isFresh(Reference::value(underlyingCellOf(m_code, believed.id()))))
            return false;
        ValueID root = underlyingCellOf(m_code, holder.id());
        if (m_code.isConstant(root) || !allocatesNewCell(m_code.at(root)))
            return false;
        for (size_t place = firstTry; place < m_trialVisits.size() && !m_inputs.options.shouldIgnoreLoopWrites; ++place) {
            if (m_trialVisits[place].objectsMadeInFrontThatAreWritten.contains(holder.id().index()))
                return false;
        }
        if (!m_inputs.options.shouldIgnoreLoopUses && classifyAgainstAllocations(allocationsUsedInLoop(m_trialVisits[firstTry].inputHead), holder) != IsListedAllocation::No)
            return false;
        count(believed == holder ? StatisticID::FreshObjectFactLossesIgnored : StatisticID::FreshObjectSlotLossesIgnored);
        return true;
    };
    m_believedAheadOfEdges[believedHere].state.forEachBeliefOfFreshHolderLackingIn(m_state, [&](Reference holder, std::optional<FieldReference> fieldOfScope, Reference believed) {
        bool isCarriedRound = believed && believed.isValue() && m_code.at(believed.id()).opcode == Opcode::Parameter && m_code.at(believed.id()).block() == to;
        if (!trialVisit) {
            if (isCarriedRound || (firstGenericVersionTrial != notFound && canIgnoreFactLoss(firstGenericVersionTrial, holder, !!fieldOfScope, believed)))
                return;
            bool entersLoopBeingVersioned = firstGenericVersionTrial != notFound && m_inheritedSummaryHeader == m_versionedLoops.last().inputHead && m_inheritedSummaryFrame == m_versionedLoops.last().frameIndex;
            if (entersLoopBeingVersioned && !believed && !fieldOfScope) {
                if (!m_inheritedEscapingUses.contains(everyFreshObject))
                    m_inheritedEscapingUses.append(everyFreshObject);
                return;
            }
            fail("what arrives at a block is not what was believed there"_s);
            return;
        }
        if (fieldOfScope && trialVisit->keepsScopesAtHead && isEnvironmentAllocation(holder)) {
            auto addInOrder = [&](Vector<FieldReference, 4>& fields) {
                auto place = std::ranges::lower_bound(fields, *fieldOfScope);
                if (place == fields.end() || *place != *fieldOfScope)
                    fields.insert(place - fields.begin(), *fieldOfScope);
            };
            addInOrder(trialVisit->fieldsOfScopesStored);
            if (!m_state.environmentField(holder, *fieldOfScope))
                addInOrder(trialVisit->fieldsOfScopesEnded);
            if (!isCarriedRound)
                trialVisit->misfits = true;
            return;
        }
        if (isCarriedRound || (!believed && isEnvironmentAllocation(holder) && m_state.isFresh(holder) && m_state.cellFacts(holder).containsAll(m_believedAheadOfEdges[believedHere].state.cellFacts(holder))))
            return;
        size_t kept = trialVisit->freshObjectsKept.findIf([&](const FreshObjectKeptAtHead& object) {
            return object.object == holder;
        });
        if (believed == holder && kept != notFound && m_state.isFresh(holder) && !trialVisit->objectsMadeInFrontThatAreWritten.contains(holder.id().index())) {
            count(StatisticID::RetainedObjectFactLossesIgnored);
            return;
        }
        if (kept == notFound && canIgnoreFactLoss(m_trialVisits.placeOf(*trialVisit), holder, !!fieldOfScope, believed))
            return;
        loseObjectKeptAtHead(*trialVisit, kept == notFound ? FreshObjectKeptAtHead { everyFreshObject, holder } : trialVisit->freshObjectsKept[kept]);
    });
}

void Walker::loseObjectKeptAtHead(TrialVisit& trialVisit, const FreshObjectKeptAtHead& kept)
{
    trialVisit.freshObjectHasLeft = true;
    Vector<ValueID, 2>& leave = m_loopSummaries[trialVisit.summaryIndex].mentionsThatLeave;
    if (!leave.contains(kept.mention))
        leave.append(kept.mention);
    bool hasInheritedSummary = m_inheritedSummaryHeader == trialVisit.inputHead && m_inheritedSummaryFrame == trialVisit.frameIndex;
    if (hasInheritedSummary && isEnvironmentAllocation(kept.object) && !m_inheritedEscapingUses.contains(kept.mention))
        m_inheritedEscapingUses.append(kept.mention);
}

TypeID Walker::climbLadderOfLoop(LoopSummary& summary, size_t parameter, TypeID assumed, TypeID found, bool isFirstInTry)
{
    TypeID united = m_types.unionOf(assumed, found);
    const TrialVisit& trialVisit = m_trialVisits.last();
    TypeID top = m_inheritedSummaryHeader == trialVisit.inputHead && m_inheritedSummaryFrame == trialVisit.frameIndex && &m_loopSummaries[trialVisit.summaryIndex] == &summary && parameter < m_inheritedParameterTypes.size() ? m_inheritedParameterTypes[parameter] : TypeID();
    uint8_t rung = isFirstInTry || !summary.rungs[parameter] ? summary.rungs[parameter] : summary.rungs[parameter] - 1;
    for (;; ++rung) {
        TypeID wider;
        switch (rung) {
        case 0:
            wider = widenedInLoops(m_types, assumed, united);
            break;
        case 1: {
            wider = topsOfKindsInLoops(m_types, united);
            Type record = m_types.at(united);
            if (record.isJavaScriptValue() && record.primitives & Primitive::Number && !(record.primitives & Primitive::FractionalNumber))
                wider = m_types.withoutPrimitives(wider, Primitive::FractionalNumber);
            break;
        }
        case 2:
            wider = topsOfKindsInLoops(m_types, united);
            break;
        default:
            summary.rungs[parameter] = 4;
            return m_types.unionOf(m_types.anything(UnknownSeed::LostByWalk), united);
        }
        wider = m_types.unionOf(wider, united);
        if (!m_types.isSubset(wider, assumed)) {
            summary.rungs[parameter] = std::max<uint8_t>(summary.rungs[parameter], rung + 1);
            if (top && !m_types.isSubset(wider, top) && m_types.isSubset(united, top) && !m_types.isSubset(top, assumed))
                return top;
            return wider;
        }
    }
}

LoopPreview Walker::makePreviewFromSummary(BlockID inputHead, std::span<const TypeID> entryTypes, const EqualityLoopTest* equality)
{
    LoopSummary& summary = m_loopSummaries[m_trialVisits.last().summaryIndex];
    if (summary.isPessimistic)
        return makePessimisticPreview(*this, inputHead, entryTypes, { });
    const BodyIndex& index = currentBodyIndex();
    Vector<bool, 8> isUnchanged;
    if (LoopID loop = index.loopOf(inputHead); summary.types.isEmpty() && !index.loop(loop).parent && index.loop(loop).numberOfLoopsWithin == 1)
        isUnchanged = parametersHandedRoundUnchangedInLoops(index, loop);
    else if (summary.types.isEmpty()) {
        LoopID outermost = index.loop(loop).outermost;
        Frame& frame = m_frames.last();
        if (!frame.outermostLoopsLookedThrough.get(outermost.index())) {
            frame.outermostLoopsLookedThrough.set(outermost.index());
            findParametersHandedRoundUnchangedInLoopsWithin(index, outermost, frame.parametersHandedRoundUnchangedByLoop);
        }
        isUnchanged = frame.parametersHandedRoundUnchangedByLoop[loop.index()];
    }
    LoopPreview preview;
    preview.effects = summary.knowsEffects ? summary.effects : Effects::unknownCode();
    if (!summary.knowsEffects && (index.holdsAnyOf(opcodesWithSuspendingBoundInTable) || index.holdsAnyOf(OpcodeSet { Opcode::IteratorResume, Opcode::IteratorClose }))) {
        for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
            for (BodyValue operation : index.block(member).values())
                preview.effects.maySuspend |= maySuspendAsWritten(operation);
        }
    }
    if (!summary.knowsEffects && m_trialVisits.size() >= 2) {
        const Effects& assumedAround = m_trialVisits[m_trialVisits.size() - 2].assumed;
        if (!assumedAround.mayRunOtherCode() && !preview.effects.maySuspend)
            preview.effects = assumedAround;
    }
    if (!summary.knowsEffects && preview.effects.mayRunOtherCode() && !preview.effects.maySuspend) {
        const LinkedFunction& linked = facts().linked().function(currentFunction());
        if (!linked.effectsOnReturn().mayRunOtherCode() && !linked.effectsOnThrow().mayRunOtherCode()) {
            preview.effects = linked.effectsOnReturn();
            preview.effects.merge(linked.effectsOnThrow());
            summary.effectsAreGuessed = true;
        }
    }
    if (!summary.knowsEffects && preview.effects.mayRunOtherCode() && !preview.effects.maySuspend) {
        Effects written = effectsOfMembersAsWrittenInLoops(typing(), index, inputHead);
        if (!written.mayRunOtherCode() && !written.maySuspend) {
            preview.effects = written;
            summary.effectsAreGuessed = true;
            summary.effectsAreGuessedByTextOfLoop = true;
            ++m_predictedLoopEffectsCount;
        }
    }
    m_trialVisits.last().entered.shrink(0);
    m_trialVisits.last().entered.append(entryTypes);
    bool hasInheritedSummary = m_inheritedSummaryHeader == inputHead && m_inheritedSummaryFrame == m_frames.size() - 1;
    bool hasInheritedEntryTypes = hasInheritedSummary && std::ranges::equal(entryTypes, m_inheritedEntryTypes);
    bool isTypedBuildOfPurchase = !m_versionedLoops.isEmpty() && m_versionedLoops.last().frameIndex == m_frames.size() - 1 && m_versionedLoops.last().inputHead == inputHead;
    Vector<TypeID, 8> enteredWhenItFitted = std::exchange(summary.enteredFromHeadsAroundWhenItFitted, { });
    bool isExitChecked = equality && (isTypedBuildOfPurchase || (m_opensGuesses && canRestartIteration()));
    for (uint32_t parameter = 0; parameter < entryTypes.size(); ++parameter) {
        TypeID entering = entryTypes[parameter];
        bool isNew = summary.types.size() <= parameter;
        bool isEqualityCounter = false;
        bool isMeetingBound = false;
        bool comesOutOfElementsOutsideViews = hasInheritedSummary && isTypedBuildOfPurchase && parameter < 64 && (m_inheritedOverreadWidenedParameters >> parameter & 1);
        bool integerIsKeptInTypedBuild = comesOutOfElementsOutsideViews;
        if (isNew) {
            summary.types.append(entering);
            summary.rungs.append(0);
            bool startIsStruck = parameter < summary.enteredWhereStartWasStruck.size() && summary.enteredWhereStartWasStruck[parameter];
            if (entering && m_types.at(entering).isJavaScriptValue() && !(parameter < isUnchanged.size() && isUnchanged[parameter]) && !startIsStruck) {
                Type record = m_types.at(entering);
                bool startsAtLengthBound = false;
                std::optional<Range> equalityRange = equality && equality->position == parameter && record.isInteger() && !record.integers.isEmpty() ? findCounterRange(*this, inputHead, *equality, record.integers, isExitChecked) : std::nullopt;
                isEqualityCounter = equalityRange.has_value();
                if (equalityRange)
                    summary.types[parameter] = m_types.unionOf(m_types.withIntegers(entering, *equalityRange), entering);
                else if (record.primitives == Primitive::Integer && !record.integers.isEmpty() && record.integers.isSubsetOf(Range::int32())) {
                    if (std::optional<NumberThatDivisionsBringRound> broughtRound = findNumberThatDivisionsBringRound(*this, inputHead, parameter, entryTypes, WhatEntersHead::IsInteger)) {
                        summary.types[parameter] = m_types.unionOf(m_types.integer(broughtRound->integers), entering);
                        integerIsKeptInTypedBuild = true;
                    } else if (comesOutOfElementsOutsideViews)
                        summary.types[parameter] = m_types.unionOf(m_types.integer(Range { 0, Range::aroundEveryFraction().maximum }), entering);
                    else if (std::optional<Range> meeting = findRangeOfMeetingBound(*this, inputHead, parameter, entryTypes)) {
                        summary.types[parameter] = m_types.unionOf(m_types.integer(*meeting), entering);
                        isMeetingBound = isTypedBuildOfPurchase;
                    } else if (operatorsOnBitsAloneBringRound(*this, inputHead, parameter, entryTypes))
                        summary.types[parameter] = m_types.integer(Range::int32());
                    else if (std::optional<Range> counted = record.integers.minimum == record.integers.maximum ? findCounterRange(*this, inputHead, parameter, record.integers.minimum) : std::nullopt)
                        summary.types[parameter] = m_types.unionOf(m_types.withIntegers(entering, *counted), entering);
                    else if (isMultipliedBelowLength(*this, inputHead, parameter)) {
                        summary.types[parameter] = m_types.withIntegers(entering, Range { record.integers.minimum, Range::uint32().maximum - 1 });
                        startsAtLengthBound = true;
                    } else if (record.integers.minimum == record.integers.maximum) {
                        bool falls = counterOnlyFalls(*this, inputHead, parameter);
                        Range stepped { record.integers.minimum - falls, record.integers.maximum + !falls };
                        summary.types[parameter] = m_types.unionOf(widenedInLoops(m_types, entering, m_types.withIntegers(entering, stepped)), entering);
                    }
                } else if (std::optional<Range> counted = record.isInteger() && record.integers.isSubsetOf(Range::exact()) ? findCounterRange(*this, inputHead, parameter, record.integers.minimum) : std::nullopt)
                    summary.types[parameter] = m_types.unionOf(m_types.withIntegers(entering, *counted), entering);
                else {
                    TypeID widened = m_types.withLiteralStringsWidened(entering);
                    if (record.primitives & Primitive::Boolean)
                        widened = m_types.unionOf(widened, TypeTable::boolean);
                    summary.types[parameter] = widened;
                }
                if (summary.types[parameter] != entering && !startsAtLengthBound)
                    summary.rungs[parameter] = 1;
                size_t parameterAround = 0;
                ValueID around = parameter < m_trialVisits.last().headParametersThatEnter.size() ? m_trialVisits.last().headParametersThatEnter[parameter] : ValueID();
                const TrialVisit* tryAround = around ? tryOfHeadParameter(around, parameterAround) : nullptr;
                if (summary.types[parameter] != entering || (tryAround && startIsGuessedInWalk(*tryAround, parameterAround))) {
                    while (summary.startIsWiderThanWhatEnters.size() <= parameter)
                        summary.startIsWiderThanWhatEnters.append(false);
                    summary.startIsWiderThanWhatEnters[parameter] = true;
                }
            }
            if (entering && m_types.at(entering).isJavaScriptValue() && !(parameter < isUnchanged.size() && isUnchanged[parameter])) {
                SetForScope standsInLoop(m_frames.last().isInPreheader, false);
                TypeID expected = findExpectation(*this, index.block(inputHead).parameter(parameter)).type;
                if (expected && m_types.at(expected).isJavaScriptValue() && !m_types.at(expected).isAnything() && !(m_types.at(expected).primitives & Primitive::Number) && !(m_types.at(entering).primitives & Primitive::Number) && m_types.at(entering).isUndefinedOrNull())
                    summary.types[parameter] = m_types.unionOf(entering, expected);
            }
        }
        integerIsKeptInTypedBuild |= isTypedBuildOfPurchase && hasInheritedEntryTypes && !m_refusesSecondCounters && entering && m_types.at(entering).isInteger() && stepsOfHeadParameter(inputHead, parameter).has_value();
        bool mayStayIntegerInThisBuild = (!hasInheritedEntryTypes || (isTypedBuildOfPurchase && (integerIsKeptInTypedBuild || isComparedByTestOfHeadInLoops(index, inputHead, parameter)))) && entering && m_types.at(entering).isInteger() && parameter < m_inheritedParameterTypes.size() && m_inheritedParameterTypes[parameter] && !m_types.at(m_inheritedParameterTypes[parameter]).isInteger();
        if (isNew && hasInheritedSummary && !mayStayIntegerInThisBuild && !isEqualityCounter && !isMeetingBound && parameter < m_inheritedParameterTypes.size() && parameter < m_inheritedEntryTypes.size() && m_inheritedEntryTypes[parameter] == entering) {
            TypeID ofPlainWalk = m_inheritedParameterTypes[parameter];
            TypeID ownStart = summary.types[parameter];
            bool rowSaysWhatSumOfUnknownsGives = ofPlainWalk && ownStart && m_types.at(ownStart).isNumber() && m_types.at(ofPlainWalk).hasOnlyPrimitives(Primitive::Number | Primitive::AnyString | Primitive::BigInt);
            bool startsBelowPlainWalk = isTypedBuildOfPurchase && parameter < 64 && ((m_inheritedRarelyWidenedParameters >> parameter & 1) || rowSaysWhatSumOfUnknownsGives) && ofPlainWalk && ownStart && ownStart != ofPlainWalk
                && m_types.at(ofPlainWalk).isJavaScriptValue() && m_types.at(ownStart).isJavaScriptValue() && m_types.isSubset(ownStart, ofPlainWalk) && !m_types.isSubset(ofPlainWalk, topsOfKindsInLoops(m_types, ownStart));
            if (startsBelowPlainWalk && !(m_inheritedRarelyWidenedParameters >> parameter & 1))
                summary.sumsThatStartAtNumberThatEnters |= 1ull << parameter;
            if (!startsBelowPlainWalk) {
                summary.types[parameter] = ofPlainWalk;
                summary.rungs[parameter] = m_inheritedWideningRungs[parameter];
            }
        }
        TypeID& held = summary.types[parameter];
        if (parameter < enteredWhenItFitted.size() && enteredWhenItFitted[parameter] && entering && held && entering != enteredWhenItFitted[parameter] && !summary.narrowerStartHasMisfitted && m_types.at(entering).isJavaScriptValue() && m_types.at(held).isJavaScriptValue()) {
            TypeID beside = parameter < summary.cameRoundBesideParametersWhenItFitted.size() ? summary.cameRoundBesideParametersWhenItFitted[parameter] : TypeID();
            TypeID start = beside ? m_types.unionOf(entering, beside) : entering;
            m_trialVisits.last().startIsNarrowerThanRowHeld |= !m_types.isSubset(held, start);
            held = start;
        }
        if (entering && held && !m_types.isSubset(entering, held))
            held = m_types.unionOf(held, entering);
        preview.typesOfParameters.append(held);
    }
    if (!summary.hasAskedAboutNarrowerForm)
        summary.steppedOnDoubles = findParametersSteppedOnDoubles(inputHead, preview);
    preview.parametersSteppedOnDoubles = summary.steppedOnDoubles;
    if (summary.trialVisitCount == 0 && summary.mentionsThatLeave.isEmpty() && m_inheritedSummaryHeader == inputHead && m_inheritedSummaryFrame == m_frames.size() - 1)
        summary.mentionsThatLeave = m_inheritedEscapingUses;
    if (!summary.storesIntoScopes.areKnown && hasInheritedSummary) {
        Vector<SlotOfLoop, 2> learned = std::exchange(summary.storesIntoScopes, m_inheritedScopeStores).slots;
        for (const SlotOfLoop& slot : learned) {
            Vector<SlotOfLoop, 2>& slots = summary.storesIntoScopes.slots;
            auto place = std::ranges::lower_bound(slots, slot.order(), { }, &SlotOfLoop::order);
            if (place == slots.end() || place->order() != slot.order())
                slots.insert(place - slots.begin(), slot);
            else
                place->hasLeftIntegers |= slot.hasLeftIntegers;
            if (place != slots.end() && place->order() == slot.order()) {
                place->hasUnknownSteps |= slot.hasUnknownSteps;
                place->stepsOfTurn = place->stepsOfTurn.unionWith(slot.stepsOfTurn);
            }
        }
    }
    if (hasInheritedSummary && unitHasMadeScopes())
        handBackScopesThatBuildLetsLeave(summary.mentionsThatLeave.span());
    m_trialVisits.last().freshObjectsKept.shrink(0);
    letMentionsOfLoopLeave(inputHead, summary.mentionsThatLeave.span(), &m_trialVisits.last().freshObjectsKept);
    handScopesKeptAtHeadToPreview(index, inputHead, summary.storesIntoScopes, m_trialVisits.last().freshObjectsKept.span(), preview);
    preview.hasLetMentionsLeave = true;
    if (!summary.typesOfValues.isEmpty()) {
        preview.typesOfValues = summary.typesOfValues;
        preview.firstValue = summary.firstValue;
    }
    if (m_inputs.options.replaysFinalLoopVisits && !m_isWalkingLastAlone)
        m_trialVisits.last().assumedSummary = std::make_shared<LoopSummary>(summary);
    return preview;
}

void Walker::noteTypeOfSumLeftInArm(ValueID sum)
{
    Reference bound = referenceBoundTo(sum);
    if (!bound)
        return;
    TypeID type = isStringInBuilder(*this, bound) ? TypeID(TypeTable::string) : typeOfRecordForLooks(*this, ownTypeOf(bound));
    if (type)
        m_trialVisits.last().typesOfSumsLeftInArms.append({ sum.index(), type });
}

void Walker::narrowSummaryToObservedTypes(LoopSummary& summary, const TrialVisit& trialVisit)
{
    for (size_t parameter = 0; parameter < summary.types.size() && parameter < trialVisit.cameRound.size() && parameter < trialVisit.entered.size(); ++parameter) {
        TypeID assumed = summary.types[parameter];
        if (!assumed || !trialVisit.cameRound[parameter] || !trialVisit.entered[parameter] || !m_types.at(assumed).isJavaScriptValue() || !m_types.at(trialVisit.cameRound[parameter]).isJavaScriptValue() || !m_types.at(trialVisit.entered[parameter]).isJavaScriptValue())
            continue;
        TypeID narrowed = m_types.unionOf(trialVisit.entered[parameter], trialVisit.cameRound[parameter]);
        if (m_types.isSubset(narrowed, assumed))
            summary.types[parameter] = narrowed;
    }
}

bool Walker::turnsCheckArithmeticOnIntegers(BlockID inputHead)
{
    Frame& frame = m_frames.last();
    const BodyIndex& index = *frame.index;
    constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Increment, Opcode::Decrement };
    if (!frame.arithmeticInLoops) {
        frame.arithmeticInLoops = makeUniqueWithoutFastMallocCheck<Vector<std::pair<ValueID, BlockID>>>();
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            if (!index.loopOf(BlockID(number)))
                continue;
            for (BodyValue operation : index.block(BlockID(number)).values()) {
                if (arithmetic.contains(operation.opcode()))
                    frame.arithmeticInLoops->append({ operation.id(), BlockID(number) });
            }
        }
    }
    LoopID loop = index.loopOf(inputHead);
    std::span<const BlockID> members = index.membersOf(loop);
    std::span<const std::pair<ValueID, BlockID>> listed = frame.arithmeticInLoops->span();
    size_t place = std::ranges::lower_bound(listed, members.front().index(), { }, [](const auto& each) { return each.second.index(); }) - listed.begin();
    for (; place < listed.size() && listed[place].second.index() <= members.back().index(); ++place) {
        if (!index.isInLoop(listed[place].second, loop))
            continue;
        Reference bound = frame.references[listed[place].first.index()];
        if (!bound || !bound.isValue())
            continue;
        ValueID id = bound.id();
        while (conversionsOfForm.contains(m_code.at(id).opcode))
            id = m_code.at(id).operand(0);
        const Operation& record = m_code.at(id);
        if (!arithmetic.contains(record.opcode) || record.form() == OperatorForm::Integers)
            continue;
        bool takesIntegers = true;
        for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(record.opcode); ++operandIndex)
            takesIntegers &= m_types.at(m_code.at(record.operand(operandIndex)).type).isInteger();
        if (takesIntegers)
            return true;
    }
    return false;
}

bool Walker::narrowerTypeOfHeadPaysWalk(TypeID assumed, TypeID narrowed, bool turnsCheckTheirArithmetic)
{
    if (representationOfNumber(*this, narrowed) != representationOfNumber(*this, assumed))
        return true;
    const Type& wide = m_types.at(assumed);
    const Type& narrow = m_types.at(narrowed);
    if (wide.primitives != narrow.primitives || wide.anyObject != narrow.anyObject || wide.anyFunction != narrow.anyFunction)
        return true;
    if (wide.objects != narrow.objects || wide.unnamedLayouts != narrow.unnamedLayouts)
        return true;
    if (wide.holdsFractionsWithoutBounds() != narrow.holdsFractionsWithoutBounds())
        return true;
    if (narrow.integers.isSubsetOf(Range::int32()) && !wide.integers.isSubsetOf(Range::int32()))
        return true;
    Range whatBitwiseOperatorsGive = Range::int32().unionWith(Range::uint32());
    return turnsCheckTheirArithmetic && narrow.integers.isSubsetOf(whatBitwiseOperatorsGive) && !wide.integers.isSubsetOf(whatBitwiseOperatorsGive);
}

Vector<bool, 8> Walker::findParametersSteppedOnDoubles(BlockID inputHead, const LoopSummary& summary)
{
    if (summary.typesOfValues.isEmpty())
        return { };
    uint32_t firstValue = summary.firstValue;
    uint32_t endOfValues = firstValue + static_cast<uint32_t>(summary.typesOfValues.size());
    return findParametersSteppedOnDoubles(inputHead, false, scopedLambda<TypeID(ValueID)>([&](ValueID value) -> TypeID {
        if (!value)
            return { };
        if (value.index() >= firstValue)
            return value.index() < endOfValues ? summary.typesOfValues[value.index() - firstValue] : TypeID();
        Reference reference = referenceOf(value);
        return reference && reference.isRecord() ? m_code.at(reference.id()).type : TypeID();
    }));
}

Vector<bool, 8> Walker::findParametersSteppedOnDoubles(BlockID inputHead, const LoopPreview& preview)
{
    uint32_t firstParameter = currentBodyIndex().block(inputHead).firstParameter().index();
    uint32_t endOfParameters = firstParameter + static_cast<uint32_t>(preview.typesOfParameters.size());
    return findParametersSteppedOnDoubles(inputHead, true, scopedLambda<TypeID(ValueID)>([&](ValueID value) -> TypeID {
        if (!value)
            return { };
        if (value.index() >= firstParameter)
            return value.index() < endOfParameters ? preview.typesOfParameters[value.index() - firstParameter] : TypeID();
        Reference reference = referenceOf(value);
        return reference && reference.isRecord() ? m_code.at(reference.id()).type : TypeID();
    }));
}

Vector<bool, 8> Walker::findParametersSteppedOnDoubles(BlockID inputHead, bool isBeforeVisit, const ScopedLambda<TypeID(ValueID)>& typeInRow)
{
    const BodyIndex& index = currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    BodyBlock head = index.block(inputHead);
    Vector<bool, 8> isStepped;
    auto reachesLastExactInteger = [&](TypeID type) {
        if (!type || !m_types.at(type).isInteger())
            return false;
        Range integers = m_types.at(type).integers;
        return integers.maximum >= maxExactInteger || integers.minimum <= -maxExactInteger;
    };
    bool someCounterReaches = false;
    for (uint32_t parameter = 0; parameter < head.numberOfParameters() && !someCounterReaches; ++parameter)
        someCounterReaches = reachesLastExactInteger(typeInRow(head.parameter(parameter)));
    if (!someCounterReaches || head.numberOfParameters() > 32)
        return isStepped;
    constexpr OpcodeSet steps { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    auto typeOfStep = [&](BodyValue step) -> TypeID {
        if (!isBeforeVisit)
            return typeInRow(step.id());
        if (step.opcode() != Opcode::Increment && step.opcode() != Opcode::Decrement)
            return { };
        TypeID stepped = typeInRow(index.stripCasts(step.operand(0)));
        return stepped && m_types.at(stepped).isInteger() ? stepped : TypeID();
    };
    LoopID nest = index.loop(loop).outermost;
    auto forEachBlockOfNest = [&](const auto& functor) {
        for (BlockID member : index.membersOf(nest))
            functor(index.block(member));
    };
    if (!m_frames.last().usesInLoops) {
        m_frames.last().usesInLoops = makeUniqueWithoutFastMallocCheck<UsesInLoops>();
        UsesInLoops& made = *m_frames.last().usesInLoops;
        made.newestStepOfLoop.fill(UINT32_MAX, index.numberOfLoops());
        made.nestsWithListedSteps.fill(false, index.numberOfLoops());
        made.nestsWithListedUses.fill(false, index.numberOfLoops());
    }
    UsesInLoops& listed = *m_frames.last().usesInLoops;
    if (!std::exchange(listed.nestsWithListedSteps[nest.index()], true)) {
        forEachBlockOfNest([&](BodyBlock block) {
            for (BodyValue step : block.values()) {
                if (!steps.contains(step.opcode()))
                    continue;
                for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(step.opcode()); ++operandIndex) {
                    ValueID stepped = index.stripCasts(step.operand(operandIndex));
                    BlockID holder = index.isParameter(stepped) ? index.blockOf(stepped) : BlockID();
                    if (!holder || !index.isLoopHead(holder) || !index.isInLoop(block.id(), index.loopOf(holder)))
                        continue;
                    uint32_t older = std::exchange(listed.newestStepOfLoop[index.loopOf(holder).index()], static_cast<uint32_t>(listed.steps.size()));
                    listed.steps.append({ step.id(), older });
                }
            }
        });
    }
    for (uint32_t place = listed.newestStepOfLoop[loop.index()]; place != UINT32_MAX; place = listed.steps[place].second) {
        BodyValue step = index.value(listed.steps[place].first);
        uint32_t numberOfOperands = numberOfNamedOperandsOf(step.opcode());
        TypeID result = typeOfStep(step);
        TypeID first = isBeforeVisit ? result : typeInRow(step.operand(0));
        TypeID second = numberOfOperands == 2 ? typeInRow(step.operand(1)) : TypeID();
        if (!result || !first || (numberOfOperands == 2 && !second))
            continue;
        if (!reachesLastExactInteger(result))
            continue;
        if (chooseArithmeticForm(program(), m_types, step.opcode(), OperatorTypes { first, second }, result) != OperatorForm::CheckedIntegers)
            continue;
        for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
            ValueID stepped = index.stripCasts(step.operand(operandIndex));
            if (!index.isParameter(stepped) || index.blockOf(stepped) != inputHead)
                continue;
            if (isStepped.isEmpty())
                isStepped.fill(false, head.numberOfParameters());
            isStepped[stepped.index() - head.firstParameter().index()] = true;
        }
    }
    if (isStepped.isEmpty())
        return isStepped;

    if (listed.newestUseOf.isEmpty()) {
        listed.newestUseOf.fill(UINT32_MAX, index.numberOfValues());
        listed.madeOf.fill(0, index.numberOfValues());
    }
    if (!std::exchange(listed.nestsWithListedUses[nest.index()], true)) {
        auto list = [&](ValueID used, UseInLoops::Kind kind, BlockID block, uint32_t user, uint32_t place) {
            if (used)
                listed.uses.append({ kind, block, user, place, std::exchange(listed.newestUseOf[used.index()], static_cast<uint32_t>(listed.uses.size())) });
        };
        forEachBlockOfNest([&](BodyBlock block) {
            for (BodyValue operation : block.values()) {
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    list(operand, UseInLoops::Kind::Operand, block.id(), operation.id().index(), 0);
                });
            }
            BodyTerminator end = block.terminator();
            end.forEachOperand([&](ValueID operand, uint32_t) {
                list(operand, UseInLoops::Kind::TakenAsItIs, block.id(), 0, 0);
            });
            for (BodyTarget target : end.targets()) {
                uint32_t place = 0;
                for (ValueID argument : target.arguments())
                    list(argument, UseInLoops::Kind::Argument, block.id(), target.block().index(), place++);
            }
            if (block.hasHandler()) {
                for (ValueID argument : block.handler().arguments())
                    list(argument, UseInLoops::Kind::TakenAsItIs, block.id(), 0, 0);
            }
        });
    }
    auto mayBeTakenAsInteger = [&](ValueID value) {
        TypeID type = typeInRow(value);
        return !type || m_types.at(type).isInteger();
    };
    Vector<uint32_t, 16> waiting;
    Vector<uint32_t, 16> given;
    auto give = [&](uint32_t value, uint32_t parts) {
        uint32_t& made = listed.madeOf[value];
        if ((made | parts) == made)
            return;
        if (!made)
            given.append(value);
        made |= parts;
        waiting.append(value);
    };
    for (uint32_t parameter = 0; parameter < head.numberOfParameters(); ++parameter) {
        if (isStepped[parameter])
            give(head.parameter(parameter).index(), 1u << parameter);
    }
    uint32_t takenAsIntegers = 0;
    while (!waiting.isEmpty()) {
        uint32_t value = waiting.takeLast();
        uint32_t parts = listed.madeOf[value];
        for (uint32_t place = listed.newestUseOf[value]; place != UINT32_MAX; place = listed.uses[place].older) {
            const UseInLoops& use = listed.uses[place];
            if (!index.isInLoop(use.block, loop))
                continue;
            if (use.kind == UseInLoops::Kind::TakenAsItIs) {
                takenAsIntegers |= parts;
                continue;
            }
            if (use.kind == UseInLoops::Kind::Argument) {
                BlockID target(use.user);
                bool isJoinBehind = target.index() > use.block.index() && index.isInLoop(target, loop) && !index.isLoopHead(target);
                if (isJoinBehind && use.place < index.block(target).numberOfParameters())
                    give(index.block(target).parameter(use.place).index(), parts);
                else if (target != inputHead && index.isInLoop(target, loop))
                    takenAsIntegers |= parts;
                continue;
            }
            BodyValue operation = index.value(ValueID(use.user));
            Opcode opcode = operation.opcode();
            TypeID result = isBeforeVisit ? TypeID() : typeInRow(operation.id());
            bool givesCounter = isBeforeVisit ? opcode == Opcode::Increment || opcode == Opcode::Decrement : steps.contains(opcode) && result && m_types.at(result).isNumber();
            if (opcodesThatStateTypes.contains(opcode) || givesCounter)
                give(use.user, parts);
            else if (!allRelations.contains(opcode) || (mayBeTakenAsInteger(operation.operand(0)) && mayBeTakenAsInteger(operation.operand(1))))
                takenAsIntegers |= parts;
        }
    }
    for (uint32_t value : given)
        listed.madeOf[value] = 0;
    for (uint32_t parameter = 0; parameter < head.numberOfParameters(); ++parameter) {
        if (takenAsIntegers & (1u << parameter))
            isStepped[parameter] = false;
    }
    return isStepped;
}

ALWAYS_INLINE_INTO_OTHER_FILES ValueID Walker::headParameterBehind(Reference value) const
{
    if (!value || !value.isValue())
        return { };
    ValueID id = value.id();
    while (conversionsOfForm.contains(m_code.at(id).opcode))
        id = m_code.at(id).operand(0);
    const Operation& record = m_code.at(id);
    return record.opcode == Opcode::Parameter && record.block() && m_code.at(record.block()).isLoopHead && !m_genericVersionEntries.contains(record.block().index()) ? id : ValueID();
}

struct ValuesReached {
    static constexpr size_t numberSearchedWithoutSet = 8;
    Vector<ValueID, numberSearchedWithoutSet> inOrder;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> all;

    void add(ValueID id)
    {
        if (inOrder.size() < numberSearchedWithoutSet) {
            if (!inOrder.contains(id))
                inOrder.append(id);
            return;
        }
        if (all.isEmpty()) {
            for (ValueID earlier : inOrder)
                all.add(earlier.index());
        }
        if (all.add(id.index()).isNewEntry)
            inOrder.append(id);
    }
};

template<typename Functor>
void Walker::forEachValueThatMeetsIn(ValueID value, BlockID openHead, uint32_t firstRecord, const Functor& functor) const
{
    const Code& code = m_code;
    ValuesReached reached;
    Vector<ValueID, ValuesReached::numberSearchedWithoutSet>& joins = reached.inOrder;
    auto take = [&](ValueID id, WhatMeetsInParameter* inJoin) {
        auto hand = [&](ValueID source, TypeID type) {
            if (!inJoin)
                return functor(source, type);
            if (source && !inJoin->headParameter && code.at(source).opcode == Opcode::Parameter && code.at(source).block() && code.at(code.at(source).block()).isLoopHead)
                inJoin->headParameter = source;
            else if (!source || source != inJoin->headParameter)
                inJoin->beside = inJoin->beside ? m_types.unionOf(inJoin->beside, type) : type;
        };
        for (;;) {
            while (conversionsOfForm.contains(code.at(id).opcode))
                id = code.at(id).operand(0);
            const Operation& record = code.at(id);
            if (record.opcode != Opcode::Parameter || id.index() < firstRecord || !record.block() || record.block() == openHead || code.at(record.block()).isEntry)
                return hand(id, record.type);
            auto noted = m_whatMeetsInParameters.find(id.index());
            if (noted == m_whatMeetsInParameters.end() || noted->value.firstRecord > firstRecord)
                return inJoin || code.at(record.block()).isLoopHead ? hand(id, record.type) : reached.add(id);
            if (noted->value.beside)
                hand(ValueID(), noted->value.beside);
            if (!noted->value.headParameter)
                return;
            id = noted->value.headParameter;
        }
    };
    auto takeWhatEntersJoin = [&](ValueID join, WhatMeetsInParameter* inJoin) {
        BlockID block = code.at(join).block();
        std::span<const ValueID> parameters = code.at(block).parameters();
        size_t position = std::ranges::lower_bound(parameters, join.index(), { }, [](ValueID each) { return each.index(); }) - parameters.begin();
        bool everyEdgeFillsIt = position < parameters.size() && parameters[position] == join && code.at(block).firstEdgeIn();
        code.forEachIncomingEdgeUnsealed(block, [&](const Edge& edge) {
            everyEdgeFillsIt = everyEdgeFillsIt && position < edge.arguments().size();
            if (everyEdgeFillsIt)
                take(edge.arguments()[position], inJoin);
        });
        return everyEdgeFillsIt;
    };
    take(value, nullptr);
    for (size_t place = 0; place < joins.size(); ++place) {
        if (!takeWhatEntersJoin(joins[place], nullptr))
            functor(joins[place], code.at(joins[place]).type);
    }
    std::ranges::sort(joins, { }, [](ValueID each) { return each.index(); });
    for (ValueID join : joins) {
        WhatMeetsInParameter inJoin { { }, { }, firstRecord };
        if (!takeWhatEntersJoin(join, &inJoin))
            inJoin = { { }, code.at(join).type, firstRecord };
        m_whatMeetsInParameters.set(join.index(), inJoin);
    }
}

TypeID Walker::typeThatComesRoundBesideParameterItself(ValueID headParameter, Reference value) const
{
    if (!value || !value.isValue())
        return value ? ownTypeOf(value) : TypeID();
    TypeID beside;
    forEachValueThatMeetsIn(value.id(), m_code.at(headParameter).block(), headParameter.index(), [&](ValueID source, TypeID type) {
        if (source != headParameter)
            beside = beside ? m_types.unionOf(beside, type) : type;
    });
    return beside;
}

void Walker::noteHeadParametersThatEnter(TrialVisit& trialVisit, std::span<const Reference> entering) const
{
    if (m_trialVisits.size() < 2)
        return;
    const TrialVisit* around = &m_trialVisits[m_trialVisits.size() - 2];
    for (Reference enters : entering) {
        size_t parameterOfDirect = 0;
        ValueID headParameter = headParameterBehind(enters);
        if (headParameter && !tryOfHeadParameter(headParameter, parameterOfDirect))
            headParameter = { };
        TypeID explained;
        TypeID beside;
        if (!headParameter && enters && enters.isValue()) {
            forEachValueThatMeetsIn(enters.id(), m_frames[around->frameIndex].blocks[around->inputHead.index()].head, around->firstRecord, [&](ValueID source, TypeID type) {
                size_t parameterAround = 0;
                const TrialVisit* taken = !headParameter && source && m_code.at(source).opcode == Opcode::Parameter && m_code.at(source).block() && m_code.at(m_code.at(source).block()).isLoopHead ? tryOfHeadParameter(source, parameterAround) : nullptr;
                if (taken) {
                    headParameter = source;
                    explained = parameterAround < taken->cameRoundBesideItself.size() && taken->cameRoundBesideItself[parameterAround] ? m_types.unionOf(type, taken->cameRoundBesideItself[parameterAround]) : type;
                } else if (!source || source != headParameter)
                    beside = beside ? m_types.unionOf(beside, type) : type;
            });
            if (!headParameter || !m_types.isSubset(ownTypeOf(enters), beside ? m_types.unionOf(explained, beside) : explained))
                headParameter = { };
        }
        trialVisit.headParametersThatEnter.append(headParameter);
        trialVisit.enteredBesideHeadParameters.append(headParameter ? beside : TypeID());
    }
}

void Walker::handUpWhatCameRoundBesideParameters(const TrialVisit& trialVisit)
{
    BlockID head = m_frames[trialVisit.frameIndex].blocks[trialVisit.inputHead.index()].head;
    for (size_t parameter = 0; parameter < trialVisit.headParametersThatEnter.size(); ++parameter) {
        if (!trialVisit.headParametersThatEnter[parameter])
            continue;
        if (head && parameter < m_code.at(head).parameters().size())
            m_whatMeetsInParameters.set(m_code.at(head).parameters()[parameter].index(), WhatMeetsInParameter { trialVisit.headParametersThatEnter[parameter], trialVisit.enteredBesideHeadParameters[parameter], 0 });
        TypeID beside = parameter < trialVisit.cameRoundBesideItself.size() ? trialVisit.cameRoundBesideItself[parameter] : TypeID();
        size_t parameterAround = 0;
        const TrialVisit* taken = beside ? tryOfHeadParameter(trialVisit.headParametersThatEnter[parameter], parameterAround) : nullptr;
        if (!taken)
            continue;
        Vector<TypeID, 8>& all = m_trialVisits[m_trialVisits.placeOf(*taken)].cameRoundBesideItself;
        if (all.size() <= parameterAround)
            all.grow(parameterAround + 1);
        all[parameterAround] = all[parameterAround] ? m_types.unionOf(all[parameterAround], beside) : beside;
    }
}

void Walker::noteSourcesOfHeadParameters(size_t frameIndex, BlockID inputHead, uint32_t summaryIndex, const TrialVisit& trialVisit, std::span<const Reference> entering)
{
    BlockID head = inputBlock(frameIndex, inputHead).head;
    if (!head || !m_code.at(head).isLoopHead)
        return;
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    for (size_t parameter = 0; parameter < parameters.size() && parameter < entering.size() && parameter < trialVisit.entered.size(); ++parameter) {
        SourcesOfHeadParameter sources;
        sources.summaryIndex = summaryIndex;
        sources.parameter = static_cast<uint32_t>(parameter);
        if (parameter < trialVisit.madeInTurns.size())
            sources.made = trialVisit.madeInTurns[parameter];
        if (parameter < trialVisit.headParametersThatCameRound.size())
            sources.headParametersHandedOn = trialVisit.headParametersThatCameRound[parameter];
        if (ValueID around = headParameterBehind(entering[parameter]))
            sources.headParametersHandedOn.append(around);
        else if (trialVisit.entered[parameter])
            sources.made = sources.made ? m_types.unionOf(sources.made, trialVisit.entered[parameter]) : trialVisit.entered[parameter];
        auto added = m_sourcesOfHeadParameters.add(parameters[parameter].index(), SourcesOfHeadParameter { });
        sources.place = added.isNewEntry ? m_headParametersWithSources.size() : added.iterator->value.place;
        added.iterator->value = WTF::move(sources);
        if (added.isNewEntry)
            m_headParametersWithSources.append(parameters[parameter].index());
    }
}

template<typename IsRoot, typename TypeAtStart, typename Take>
void Walker::carryTypesAlongRoad(TypeArena& types, std::span<const RecordThatMakes> steps, uint32_t numberOfRecords, const IsRoot& isRoot, const TypeAtStart& typeAtStart, const Take& take)
{
    Vector<uint32_t, 16> firstStepOf;
    firstStepOf.fill(0, numberOfRecords + 1);
    for (RecordThatMakes step : steps)
        ++firstStepOf[step.maker + 1];
    for (uint32_t record = 0; record < numberOfRecords; ++record)
        firstStepOf[record + 1] += firstStepOf[record];
    Vector<RecordThatMakes, 16> road;
    road.grow(steps.size());
    Vector<uint32_t, 16> nextStepOf(firstStepOf.span().first(numberOfRecords));
    for (RecordThatMakes step : steps)
        road[nextStepOf[step.maker]++] = step;

    struct RecordOnWalk {
        uint32_t ordinal { 0 };
        uint32_t lowestReached { 0 };
        uint32_t ring { std::numeric_limits<uint32_t>::max() };
    };
    Vector<RecordOnWalk, 16> walked;
    walked.grow(numberOfRecords);
    Vector<uint32_t, 16> notYetInRing;
    Vector<uint32_t, 16> membersOfRings;
    Vector<uint32_t, 16> endsOfRings;
    Vector<std::pair<uint32_t, uint32_t>, 16> path;
    uint32_t numberMet = 0;
    auto meet = [&](uint32_t record) {
        walked[record].ordinal = walked[record].lowestReached = ++numberMet;
        notYetInRing.append(record);
        path.append({ record, firstStepOf[record] });
    };
    for (uint32_t giver = 0; giver < numberOfRecords; ++giver) {
        if (walked[giver].ordinal || !isRoot(giver))
            continue;
        meet(giver);
        while (!path.isEmpty()) {
            auto [maker, step] = path.last();
            if (step < firstStepOf[maker + 1]) {
                ++path.last().second;
                uint32_t made = road[step].made;
                if (!walked[made].ordinal)
                    meet(made);
                else if (walked[made].ring == RecordOnWalk().ring)
                    walked[maker].lowestReached = std::min(walked[maker].lowestReached, walked[made].ordinal);
                continue;
            }
            path.removeLast();
            if (!path.isEmpty())
                walked[path.last().first].lowestReached = std::min(walked[path.last().first].lowestReached, walked[maker].lowestReached);
            if (walked[maker].lowestReached != walked[maker].ordinal)
                continue;
            for (uint32_t member = maker + 1; member != maker;) {
                member = notYetInRing.takeLast();
                walked[member].ring = endsOfRings.size();
                membersOfRings.append(member);
            }
            endsOfRings.append(membersOfRings.size());
        }
    }

    auto unite = [&](TypeID& into, TypeID more) {
        if (!into)
            into = more;
        else if (more && !types.isSubset(more, into))
            into = types.unionOf(into, more);
    };
    Vector<TypeID, 16> givenIntoRings;
    givenIntoRings.grow(endsOfRings.size());
    for (uint32_t ring = endsOfRings.size(); ring--;) {
        std::span<const uint32_t> members = membersOfRings.span().subspan(ring ? endsOfRings[ring - 1] : 0);
        members = members.first(endsOfRings[ring] - (ring ? endsOfRings[ring - 1] : 0));
        TypeID held = givenIntoRings[ring];
        for (uint32_t member : members)
            unite(held, typeAtStart(member));
        if (!held)
            continue;
        for (uint32_t member : members) {
            take(member, held);
            for (uint32_t step = firstStepOf[member]; step < firstStepOf[member + 1]; ++step) {
                if (uint32_t taker = walked[road[step].made].ring; taker != ring)
                    unite(givenIntoRings[taker], held);
            }
        }
    }
}

Vector<TypeID, 16> Walker::typesThatReachHeadParametersOfNest(uint32_t firstRecordOfNest, size_t firstHeadParameter, size_t endOfHeadParameters)
{
    uint32_t numberOfRecords = endOfHeadParameters - firstHeadParameter;
    Vector<TypeID, 16> typesThatReach;
    typesThatReach.grow(numberOfRecords);
    Vector<RecordThatMakes, 16> road;
    for (uint32_t taker = 0; taker < numberOfRecords; ++taker) {
        const SourcesOfHeadParameter& sources = m_sourcesOfHeadParameters.find(m_headParametersWithSources[firstHeadParameter + taker])->value;
        TypeID all = sources.made;
        for (ValueID handedOn : sources.headParametersHandedOn) {
            auto found = handedOn.index() >= firstRecordOfNest ? m_sourcesOfHeadParameters.find(handedOn.index()) : m_sourcesOfHeadParameters.end();
            if (found == m_sourcesOfHeadParameters.end()) {
                if (TypeID type = m_code.at(handedOn).type)
                    all = all ? m_types.unionOf(all, type) : type;
            } else if (uint32_t place = found->value.place; place >= firstHeadParameter && place < endOfHeadParameters && place - firstHeadParameter != taker)
                road.append({ static_cast<uint32_t>(place - firstHeadParameter), taker });
        }
        typesThatReach[taker] = all;
    }
    carryTypesAlongRoad(m_types, road.span(), numberOfRecords, [&](uint32_t) {
        return true;
    }, [&](uint32_t record) {
        return typesThatReach[record];
    }, [&](uint32_t record, TypeID held) {
        typesThatReach[record] = held;
    });
    return typesThatReach;
}

void Walker::narrowRowsInsideTo(const TrialVisit::NarrowingThatWaits& narrowing)
{
    std::span<const uint32_t> headParametersOfNest = m_headParametersWithSources.span().subspan(narrowing.firstHeadParameter, narrowing.endOfHeadParameters - narrowing.firstHeadParameter);
    Vector<TypeID, 16> typesThatReach = typesThatReachHeadParametersOfNest(narrowing.firstRecordOfNest, narrowing.firstHeadParameter, narrowing.endOfHeadParameters);
    for (uint32_t headParameter : headParametersOfNest) {
        const SourcesOfHeadParameter& sources = m_sourcesOfHeadParameters.find(headParameter)->value;
        if (sources.summaryIndex == narrowing.ownSummaryIndex || sources.summaryIndex >= m_loopSummaries.size())
            continue;
        LoopSummary& inside = m_loopSummaries[sources.summaryIndex];
        if (sources.parameter >= inside.types.size() || inside.isPessimistic)
            continue;
        TypeID reaches = typesThatReach[sources.place - narrowing.firstHeadParameter];
        TypeID held = inside.types[sources.parameter];
        if (reaches && held && m_types.at(held).isJavaScriptValue() && m_types.at(reaches).isJavaScriptValue() && m_types.isSubset(reaches, held))
            inside.types[sources.parameter] = reaches;
    }
}

void Walker::noteAllThatComesRoundInTry(BlockID head, size_t parameter, TypeID found, Reference value)
{
    for (size_t place = m_trialVisits.size(); place--;) {
        TrialVisit& trialVisit = m_trialVisits[place];
        if (trialVisit.frameIndex >= m_frames.size() || m_frames[trialVisit.frameIndex].blocks[trialVisit.inputHead.index()].head != head)
            continue;
        if (trialVisit.cameRound.size() <= parameter)
            trialVisit.cameRound.grow(parameter + 1);
        trialVisit.cameRound[parameter] = trialVisit.cameRound[parameter] ? m_types.unionOf(trialVisit.cameRound[parameter], found) : found;
        if (trialVisit.valuesThatCameRound.size() <= parameter)
            trialVisit.valuesThatCameRound.grow(parameter + 1);
        trialVisit.valuesThatCameRound[parameter].append(value.isValue() ? value.id() : ValueID());
        if (((parameter < trialVisit.headParametersThatEnter.size() && trialVisit.headParametersThatEnter[parameter]) || startIsGuessedInWalk(trialVisit, parameter)) && parameter < m_code.at(head).parameters().size()) {
            if (trialVisit.cameRoundBesideItself.size() <= parameter)
                trialVisit.cameRoundBesideItself.grow(parameter + 1);
            TypeID& all = trialVisit.cameRoundBesideItself[parameter];
            if (TypeID beside = typeThatComesRoundBesideParameterItself(m_code.at(head).parameters()[parameter], value))
                all = all ? m_types.unionOf(all, beside) : beside;
            TypeID ofParameter = m_code.at(m_code.at(head).parameters()[parameter]).type;
            if (!m_types.isSubset(found, ofParameter) && !m_types.isSubset(found, all ? m_types.unionOf(all, ofParameter) : ofParameter))
                all = all ? m_types.unionOf(all, found) : found;
        }
        if (trialVisit.madeInTurns.size() <= parameter) {
            trialVisit.madeInTurns.grow(parameter + 1);
            trialVisit.headParametersThatCameRound.grow(parameter + 1);
        }
        if (ValueID handedOn = headParameterBehind(value)) {
            if (!trialVisit.headParametersThatCameRound[parameter].contains(handedOn))
                trialVisit.headParametersThatCameRound[parameter].append(handedOn);
            return;
        }
        trialVisit.madeInTurns[parameter] = trialVisit.madeInTurns[parameter] ? m_types.unionOf(trialVisit.madeInTurns[parameter], found) : found;
        if (value.isValue())
            noteOwnParametersThatMeetInValue(trialVisit, parameter, value.id(), head);
        return;
    }
}

uint64_t Walker::findOverreadWidenedParameters(const TrialVisit& trialVisit, const LoopSummary& summary, BlockID head) const
{
    struct ValuesReached {
        void add(ValueID id)
        {
            if (!isIn.set(id.index()))
                inOrder.append(id);
        }
        BitVector isIn;
        Vector<ValueID, 16> inOrder;
    };
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    if (!head)
        return 0;
    std::span<const ValueID> own = m_code.at(head).parameters();
    uint64_t widened = 0;
    for (size_t parameter = 0; parameter < trialVisit.valuesThatCameRound.size() && parameter < summary.types.size() && parameter < trialVisit.entered.size() && parameter < 64; ++parameter) {
        TypeID held = summary.types[parameter];
        TypeID entered = trialVisit.entered[parameter];
        if (!held || !entered || !m_types.at(entered).isInteger() || !m_types.at(held).isNumber() || m_types.at(held).isInteger())
            continue;
        ValuesReached carried;
        ValuesReached summed;
        bool isFollowed = true;
        bool readsOutside = false;
        auto behindNames = [&](ValueID id) {
            while (id && (conversionsOfForm.contains(m_code.at(id).opcode) || m_code.at(id).opcode == Opcode::Narrowed || m_code.at(id).opcode == Opcode::ToNumber))
                id = m_code.at(id).operand(0);
            return id;
        };
        auto add = [&](ValuesReached& into, ValueID id) {
            id = behindNames(id);
            if (id)
                into.add(id);
            else
                isFollowed = false;
        };
        auto follow = [&](ValueID id, ValuesReached& into, bool isSummed) {
            const Operation& record = m_code.at(id);
            if (record.type && m_types.at(record.type).isInteger())
                return;
            if (record.opcode == Opcode::GetTypedArrayElement) {
                readsOutside = true;
                isFollowed &= !record.isInsideView() && m_types.at(elementTypeOf(record.typedArrayType())).isInteger();
            } else if (record.opcode == Opcode::Parameter && record.block() == head) {
                size_t place = std::ranges::find(own, id) - own.begin();
                isFollowed &= !isSummed && place < trialVisit.valuesThatCameRound.size() && place < trialVisit.entered.size() && trialVisit.entered[place] && m_types.at(trialVisit.entered[place]).isInteger();
                for (size_t value = 0; isFollowed && value < trialVisit.valuesThatCameRound[place].size(); ++value)
                    add(into, trialVisit.valuesThatCameRound[place][value]);
            } else if (record.opcode == Opcode::Parameter && id.index() >= trialVisit.firstRecord && !m_code.at(record.block()).isLoopHead && !m_code.at(record.block()).isHandlerBlock()) {
                std::span<const ValueID> parameters = m_code.at(record.block()).parameters();
                size_t position = std::ranges::find(parameters, id) - parameters.begin();
                for (EdgeID edge = m_code.at(record.block()).firstEdgeIn(); edge; edge = m_code.at(edge).nextInto())
                    add(into, position < m_code.at(edge).arguments().size() ? m_code.at(edge).arguments()[position] : ValueID());
            } else if (sums.contains(record.opcode)) {
                for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(record.opcode); ++operandIndex)
                    add(summed, record.operand(operandIndex));
            } else if (record.opcode == Opcode::CallInline && (record.inlineBuiltin() == InlineBuiltin::MathMin || record.inlineBuiltin() == InlineBuiltin::MathMax)) {
                OpcodeSet boundedSums = record.inlineBuiltin() == InlineBuiltin::MathMin ? OpcodeSet { Opcode::Add, Opcode::Increment } : OpcodeSet { Opcode::Sub, Opcode::Decrement };
                for (ValueID argument : record.argumentsOperands()) {
                    ValueID made = behindNames(argument);
                    if (isSummed || !made || !boundedSums.contains(m_code.at(made).opcode)) {
                        add(into, argument);
                        continue;
                    }
                    for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(m_code.at(made).opcode); ++operandIndex)
                        add(carried, m_code.at(made).operand(operandIndex));
                }
            } else
                isFollowed = false;
        };
        for (ValueID value : trialVisit.valuesThatCameRound[parameter])
            add(carried, value);
        for (size_t next = 0; isFollowed && next < carried.inOrder.size(); ++next)
            follow(carried.inOrder[next], carried, false);
        for (size_t next = 0; isFollowed && next < summed.inOrder.size(); ++next)
            follow(summed.inOrder[next], summed, true);
        if (isFollowed && readsOutside)
            widened |= 1ull << parameter;
    }
    return widened;
}

uint64_t Walker::findRarelyWidenedParameters(const TrialVisit& trialVisit, const LoopSummary& summary, BlockID head)
{
    constexpr size_t mostValuesFollowed = 32;
    uint64_t widened = 0;
    std::span<const ValueID> own = head ? m_code.at(head).parameters() : std::span<const ValueID>();
    for (size_t parameter = 0; parameter < trialVisit.valuesThatCameRound.size() && parameter < summary.types.size() && parameter < trialVisit.entered.size() && parameter < 64; ++parameter) {
        TypeID held = summary.types[parameter];
        TypeID onLikelyWays = trialVisit.entered[parameter];
        if (!held || !onLikelyWays || !m_types.at(held).isJavaScriptValue() || !m_types.at(onLikelyWays).isJavaScriptValue())
            continue;
        Vector<ValueID, mostValuesFollowed> reached;
        bool isFollowed = true;
        bool leavesOutAbsentElement = false;
        auto add = [&](ValueID id) {
            if (!id || reached.size() > mostValuesFollowed)
                isFollowed = false;
            else if (!reached.contains(id))
                reached.append(id);
        };
        for (ValueID value : trialVisit.valuesThatCameRound[parameter])
            add(value);
        for (size_t next = 0; isFollowed && next < reached.size(); ++next) {
            if (reached.size() > mostValuesFollowed) {
                isFollowed = false;
                break;
            }
            ValueID id = reached[next];
            while (conversionsOfForm.contains(m_code.at(id).opcode))
                id = m_code.at(id).operand(0);
            const Operation& record = m_code.at(id);
            BlockID block = m_code.blockOf(id);
            if (block && m_code.at(block).frequency == BlockFrequency::Rare)
                continue;
            if (record.opcode == Opcode::Parameter && id.index() >= trialVisit.firstRecord && !m_code.at(block).isLoopHead && !m_code.at(block).isHandlerBlock()) {
                std::span<const ValueID> parameters = m_code.at(block).parameters();
                size_t position = std::ranges::find(parameters, id) - parameters.begin();
                for (EdgeID edge = m_code.at(block).firstEdgeIn(); edge; edge = m_code.at(edge).nextInto()) {
                    if (m_code.at(m_code.at(edge).from).frequency == BlockFrequency::Rare)
                        continue;
                    add(position < m_code.at(edge).arguments().size() ? m_code.at(edge).arguments()[position] : ValueID());
                }
                continue;
            }
            if (record.opcode == Opcode::Parameter && block == head) {
                size_t place = std::ranges::find(own, id) - own.begin();
                if (place >= trialVisit.valuesThatCameRound.size() || place >= trialVisit.entered.size() || !trialVisit.entered[place] || !m_types.at(trialVisit.entered[place]).isJavaScriptValue()) {
                    isFollowed = false;
                    break;
                }
                onLikelyWays = m_types.unionOf(onLikelyWays, trialVisit.entered[place]);
                for (ValueID value : trialVisit.valuesThatCameRound[place])
                    add(value);
                continue;
            }
            if (!record.type || !m_types.at(record.type).isJavaScriptValue()) {
                isFollowed = false;
                break;
            }
            TypeID brought = record.type;
            if (record.opcode == Opcode::GetArrayElement && record.keyIsIntegral() && (m_types.at(brought).primitives & Primitive::Undefined)) {
                brought = m_types.withoutPrimitives(brought, Primitive::Undefined);
                leavesOutAbsentElement = true;
                if (isNeverType(brought))
                    continue;
            }
            onLikelyWays = m_types.unionOf(onLikelyWays, brought);
        }
        if (isFollowed && onLikelyWays != held && m_types.isSubset(onLikelyWays, held)) {
            widened |= 1ull << parameter;
            if (leavesOutAbsentElement)
                count(StatisticID::AbsentArrayElementWideningsAvoided);
        }
    }
    return widened;
}

bool Walker::noteWhatCameRoundInTry(BlockID head, size_t parameter, TypeID assumed, TypeID found, size_t numberOfTriesAround, TypeID* enteredUnderGuessedStart)
{
    if (!m_types.at(assumed).isJavaScriptValue() || !m_types.at(found).isJavaScriptValue())
        return false;
    for (size_t place = std::min(numberOfTriesAround, m_trialVisits.size()); place--;) {
        TrialVisit& trialVisit = m_trialVisits[place];
        if (trialVisit.frameIndex >= m_frames.size() || m_frames[trialVisit.frameIndex].blocks[trialVisit.inputHead.index()].head != head)
            continue;
        LoopSummary& summary = m_loopSummaries[trialVisit.summaryIndex];
        if (parameter >= summary.types.size() || summary.isPessimistic)
            return false;
        uint64_t bit = parameter < 64 ? 1ull << parameter : 0;
        trialVisit.misfits = true;
        if (trialVisit.cameRoundBesideItself.size() <= parameter)
            trialVisit.cameRoundBesideItself.grow(parameter + 1);
        TypeID& besideItself = trialVisit.cameRoundBesideItself[parameter];
        if (enteredUnderGuessedStart || !besideItself)
            besideItself = besideItself ? m_types.unionOf(besideItself, found) : found;
        TypeID enteredAround = climbHeadAroundWith(trialVisit, place, parameter, besideItself);
        bool isStruckNow = !(parameter < trialVisit.startsStruck.size() && trialVisit.startsStruck[parameter]);
        TypeID entered;
        if (!isStruckNow)
            entered = summary.enteredWhereStartWasStruck[parameter];
        else if (startIsGuessedInWalk(trialVisit, parameter)) {
            entered = enteredAround ? enteredAround : parameter < trialVisit.entered.size() ? trialVisit.entered[parameter] : TypeID();
            if (enteredAround && trialVisit.enteredBesideHeadParameters[parameter])
                entered = m_types.unionOf(entered, trialVisit.enteredBesideHeadParameters[parameter]);
        }
        if (entered && m_types.at(entered).isJavaScriptValue()) {
            if (enteredUnderGuessedStart)
                *enteredUnderGuessedStart = entered;
            if (isStruckNow) {
                summary.startIsWiderThanWhatEnters[parameter] = false;
                if (summary.enteredWhereStartWasStruck.size() <= parameter)
                    summary.enteredWhereStartWasStruck.grow(parameter + 1);
                summary.enteredWhereStartWasStruck[parameter] = entered;
                while (trialVisit.startsStruck.size() <= parameter)
                    trialVisit.startsStruck.append(false);
                trialVisit.startsStruck[parameter] = true;
                summary.rungs[parameter] = 0;
            }
            summary.types[parameter] = climbLadderOfLoop(summary, parameter, entered, besideItself, isStruckNow);
            trialVisit.parametersThatClimbed |= bit;
            return true;
        }
        TypeID climbed = climbLadderOfLoop(summary, parameter, assumed, found, !(trialVisit.parametersThatClimbed & bit));
        trialVisit.parametersThatClimbed |= bit;
        summary.types[parameter] = m_types.unionOf(summary.types[parameter], climbed);
        return true;
    }
    return false;
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::noteOwnParametersThatMeetInValue(TrialVisit& trialVisit, size_t parameter, ValueID value, BlockID head)
{
    constexpr OpcodeSet arithmeticWhoseKindFollowsItsOperands { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Increment, Opcode::Decrement };
    auto behindConversions = [&](ValueID id) {
        while (conversionsOfForm.contains(m_code.at(id).opcode))
            id = m_code.at(id).operand(0);
        return id;
    };
    auto forEachValueThatMakes = [&](ValueID id, const auto& functor) {
        const Operation& record = m_code.at(id);
        if (id.index() < trialVisit.firstRecord)
            return;
        if (arithmeticWhoseKindFollowsItsOperands.contains(record.opcode)) {
            for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(record.opcode); ++operandIndex) {
                if (ValueID operand = record.operand(operandIndex); operand && m_types.at(m_code.at(operand).type).hasOnlyPrimitives(Primitive::Number | Primitive::BigInt))
                    functor(behindConversions(operand));
            }
            return;
        }
        BlockID block = record.opcode == Opcode::Parameter ? record.block() : BlockID();
        if (!block || block == head || m_code.at(block).isLoopHead || m_code.at(block).isHandlerBlock())
            return;
        std::span<const ValueID> parameters = m_code.at(block).parameters();
        size_t position = std::ranges::find(parameters, id) - parameters.begin();
        for (EdgeID edge = m_code.at(block).firstEdgeIn(); edge; edge = m_code.at(edge).nextInto()) {
            if (position < m_code.at(edge).arguments().size())
                functor(behindConversions(m_code.at(edge).arguments()[position]));
        }
    };
    ValueID began = behindConversions(value);
    if (m_code.at(began).opcode != Opcode::Parameter && !arithmeticWhoseKindFollowsItsOperands.contains(m_code.at(began).opcode))
        return;
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    if (parameter >= parameters.size())
        return;
    trialVisit.recordsThatMakeWhatCameRound.append({ began.index(), parameters[parameter].index() });
    ValuesReached reached;
    reached.add(began);
    size_t numberOfSteps = 0;
    for (size_t next = 0; next < reached.inOrder.size(); ++next) {
        ValueID made = reached.inOrder[next];
        if (trialVisit.recordsWhoseMakersAreNoted.contains(made.index()))
            continue;
        forEachValueThatMakes(made, [&](ValueID maker) {
            ++numberOfSteps;
            trialVisit.recordsThatMakeWhatCameRound.append({ maker.index(), made.index() });
            reached.add(maker);
        });
    }
    if (numberOfSteps <= ValuesReached::numberSearchedWithoutSet)
        return;
    for (ValueID made : reached.inOrder)
        trialVisit.recordsWhoseMakersAreNoted.add(made.index());
}

const Walker::TrialVisit* Walker::tryOfHeadParameter(ValueID headParameter, size_t& parameter) const
{
    BlockID head = m_code.at(headParameter).block();
    for (size_t place = m_trialVisits.size(); place--;) {
        const TrialVisit& trialVisit = m_trialVisits[place];
        if (trialVisit.frameIndex >= m_frames.size() || m_frames[trialVisit.frameIndex].blocks[trialVisit.inputHead.index()].head != head)
            continue;
        std::span<const ValueID> parameters = m_code.at(head).parameters();
        parameter = std::ranges::lower_bound(parameters, headParameter.index(), { }, [](ValueID each) { return each.index(); }) - parameters.begin();
        return parameter < parameters.size() && parameters[parameter] == headParameter ? &trialVisit : nullptr;
    }
    return nullptr;
}

bool Walker::startIsGuessedInWalk(const TrialVisit& trialVisit, size_t parameter) const
{
    const Vector<bool, 8>& isWider = m_loopSummaries[trialVisit.summaryIndex].startIsWiderThanWhatEnters;
    return (parameter < isWider.size() && isWider[parameter]) || (parameter < trialVisit.startsStruck.size() && trialVisit.startsStruck[parameter]);
}

TypeID Walker::climbHeadAroundWith(const TrialVisit& trialVisit, size_t numberOfTriesAround, size_t parameter, TypeID found)
{
    ValueID around = parameter < trialVisit.headParametersThatEnter.size() ? trialVisit.headParametersThatEnter[parameter] : ValueID();
    if (!around)
        return { };
    BlockID head = m_code.at(around).block();
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    size_t place = std::ranges::lower_bound(parameters, around.index(), { }, [](ValueID each) { return each.index(); }) - parameters.begin();
    TypeID assumed = m_code.at(around).type;
    TypeID enteredUnderGuessedStart;
    if (place < parameters.size() && parameters[place] == around && !m_types.isSubset(found, assumed))
        noteWhatCameRoundInTry(head, place, assumed, found, numberOfTriesAround, &enteredUnderGuessedStart);
    return enteredUnderGuessedStart;
}

bool Walker::walkAroundIsUndone() const
{
    return std::ranges::any_of(m_trialVisits, [](const TrialVisit& around) {
        return around.misfits || around.freshObjectHasLeft;
    });
}

void Walker::letParametersHoldWhatTheyTakeRound(LoopSummary& summary, const TrialVisit& trialVisit, BlockID head)
{
    if (!head || summary.isPessimistic)
        return;
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    auto placeOf = [&](uint32_t record) {
        size_t place = std::ranges::lower_bound(parameters, record, { }, [](ValueID each) { return each.index(); }) - parameters.begin();
        return place < parameters.size() && parameters[place].index() == record ? place : notFound;
    };
    auto stopsTheFlow = [&](uint32_t record) {
        size_t place = placeOf(record);
        return place != notFound && (place >= summary.types.size() || !summary.types[place] || !m_types.at(summary.types[place]).isJavaScriptValue());
    };
    Vector<RecordThatMakes, 16> road;
    Vector<uint32_t, 16> records;
    auto lay = [&](uint32_t maker, uint32_t made) {
        if (maker == made || stopsTheFlow(maker) || stopsTheFlow(made))
            return;
        road.append({ maker, made });
        records.append(maker);
        records.append(made);
    };
    for (RecordThatMakes step : trialVisit.recordsThatMakeWhatCameRound)
        lay(step.maker, step.made);
    for (uint32_t taker = 0; taker < trialVisit.headParametersThatCameRound.size() && taker < parameters.size(); ++taker) {
        for (ValueID handedOn : trialVisit.headParametersThatCameRound[taker])
            lay(handedOn.index(), parameters[taker].index());
    }
    std::ranges::sort(records);
    records.shrink(std::ranges::unique(records).begin() - records.begin());
    for (RecordThatMakes& step : road)
        step = { static_cast<uint32_t>(std::ranges::lower_bound(records, step.maker) - records.begin()), static_cast<uint32_t>(std::ranges::lower_bound(records, step.made) - records.begin()) };
    carryTypesAlongRoad(m_types, road.span(), records.size(), [&](uint32_t record) {
        return placeOf(records[record]) != notFound;
    }, [&](uint32_t record) {
        size_t place = placeOf(records[record]);
        return place != notFound ? summary.types[place] : TypeID();
    }, [&](uint32_t record, TypeID held) {
        size_t place = placeOf(records[record]);
        if (place != notFound && !m_types.isSubset(held, summary.types[place]))
            summary.types[place] = m_types.unionOf(summary.types[place], held);
    });
}

Walker::LoopSummaryKey Walker::keyOfCopyOfLoop(size_t frameIndex, BlockID inputHead) const
{
    uint64_t chainOfCalls = m_ordinalOfCopyOfLoop;
    std::span<const InlineFrame> inlineFrames = m_code.inlineFrames();
    for (uint32_t inlineFrame = m_frames[frameIndex].inlineFrame; inlineFrame; inlineFrame = inlineFrames[inlineFrame].parent)
        chainOfCalls = (chainOfCalls ^ (static_cast<uint64_t>(inlineFrames[inlineFrame].callee.index()) << 32 | inlineFrames[inlineFrame].callPosition)) * 0x9e3779b97f4a7c15ull + 1;
    return { static_cast<uint64_t>(m_frames[frameIndex].function.index()) << 32 | inputHead.index(), chainOfCalls };
}

Walker::LoopSummaryKey Walker::loopSummaryKey(LoopSummaryKey copyOfLoop, uint32_t numberOfEntriesBefore)
{
    return { copyOfLoop.first, (copyOfLoop.second ^ numberOfEntriesBefore) * 0xbf58476d1ce4e5b9ull + numberOfEntriesBefore };
}

static void askWhatIsExpectedOfParametersThatEnterLoop(Walker& walker, BlockID inputHead)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID firstOfHead = index.block(inputHead).firstParameter();
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        for (ValueID argument : edge.target.arguments()) {
            if (argument && argument < firstOfHead && index.isParameter(argument) && index.blockOf(argument) != entryBlock)
                findExpectation(walker, argument);
        }
    }
}

bool Walker::runTrialVisits(BlockID inputHead, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (hasOpenTrialVisit(frameIndex, inputHead) || m_way != WalkMode::AsBuilt)
        return false;
    if (isNestedTooDeeply()) [[unlikely]]
        return false;
    if (!index.isLoopHead(inputHead) || index.hasIrreducibleFlow() || index.isEnteredByHandlerEdge(inputHead))
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (frameIndex && !m_frames[frameIndex].returnsInPlaceOfUnit) {
        for (BlockID member : index.membersOf(loop)) {
            TerminatorKind end = index.block(member).terminator().kind();
            if (end == TerminatorKind::Return || end == TerminatorKind::ReturnCall)
                return false;
        }
    }

    LoopSummaryKey copyOfLoop = keyOfCopyOfLoop(frameIndex, inputHead);
    uint32_t numberOfEntriesBefore = m_entriesOfCopiesOfLoops.add(copyOfLoop, 0).iterator->value++;
    if (m_trialVisitDepth)
        m_entriesOfCopiesOfLoopsInTries.append(copyOfLoop);
    LoopSummaryKey key = loopSummaryKey(copyOfLoop, numberOfEntriesBefore);
    uint32_t place = m_loopSummaryIndices.add(key, static_cast<uint32_t>(m_loopSummaries.size())).iterator->value;
    if (place == m_loopSummaries.size()) {
        moveWhereSanitizerWatches(m_loopSummaries);
        m_loopSummaries.append(LoopSummary { });
        if (m_effectsSeededForNextRow) {
            m_loopSummaries.last().effects = *m_effectsSeededForNextRow;
            m_loopSummaries.last().knowsEffects = true;
        }
    } else if (m_effectsSeededForNextRow && m_otherCodeStaysInTypedBuildOfRow)
        m_loopSummaries[place].effects = *m_effectsSeededForNextRow;
    m_effectsSeededForNextRow = std::nullopt;
    if (loopCarriesValuesOfConsumerRound(inputHead) && !m_loopSummaries[place].mentionsThatLeave.contains(everyFreshObject))
        m_loopSummaries[place].mentionsThatLeave.append(everyFreshObject);
    bool hasUndoneWhatFits = false;
    bool hasAskedForTwoBuilds = false;
    SetForScope sumsAreNotAsked(m_sumsAreNotAskedInRow, m_unguardedSumLoops.contains(key));
    SetForScope keysAreReadOffLook(m_keysAreReadOffLookInRow, m_rowsWhoseKeysAreReadOffLook.contains(key));
    SetForScope keyWasTakenForCounter(m_keyWasTakenForCounterAskedInFront, false);

    size_t numberOfRowsBefore = m_hoistedRows.size();
    size_t numberOfFieldArraysBefore = m_fieldArrayReads.size();
    Vector<Reference, 8> enteringKept;
    enteringKept.append(entering);
    if (!program().moduleInstances.isEmpty())
        hoistModuleLinks(*this, inputHead);
    auto forgetFieldArrayReads = makeScopeExit([this, kept = m_fieldArrayReads.size()] {
        m_fieldArrayReads.shrink(kept);
    });
    if (!isUnderHandler() && has(Mechanism::Folding) && !isUnreachable())
        hoistLeadingInitializationChecks(*this, inputHead);
    if (m_hasFailed || isUnreachable())
        return false;
    readListsOfStableBindingsInFrontOfLoop(*this, inputHead);
    readFieldArraysInPreheader(*this, inputHead);
    hoistKeyFields(*this, inputHead);
    auto forgetHoistedRows = makeScopeExit([this, kept = m_hoistedRows.size()] {
        m_hoistedRows.shrink(kept);
    });
    readRowArraysInPreheader(*this, inputHead);
    if (m_hasFailed || isUnreachable())
        return false;
    askWhatIsExpectedOfParametersThatEnterLoop(*this, inputHead);
    if (!m_trialVisitDepth++)
        m_code.keepChangesToOlderParts(true);
    uint32_t serialOfMarkAround = m_serialOfNewestMark;
    setSerialOfNewestMark(++m_numberOfMarksTaken);
    auto forgetMark = makeScopeExit([&] {
        setSerialOfNewestMark(serialOfMarkAround);
        if (--m_trialVisitDepth)
            return;
        m_code.keepChangesToOlderParts(false);
        m_statesOfHandlersInTries.shrink(0);
        m_placesOfOpenLoopsInTries.shrink(0);
        if (keptByStrings())
            notesOfStrings(*this).codeUnitsChanged.shrink(0);
        m_inputHeadsOfIterationsAddedInTries.shrink(0);
        m_typesChangedInTries.shrink(0);
        m_entriesOfCopiesOfLoopsInTries.shrink(0);
        m_whatMeetsInParameters.clear();
        m_preheaderDepthUndoLog.shrink(0);
        m_sharedConstantsAddedInTries.shrink(0);
        m_copiesOfConstantsAddedInTries.shrink(0);
        m_inputBlocksInTries.shrink(0);
        m_expectationsInTries.shrink(0);
        if (!m_numberOfClosedArms)
            m_bindings.shrink(0);
    });
    auto putRowOfPurchaseAroundBack = makeScopeExit([this, isQuiet = m_rowSaysTurnsAreQuietOutsideArms, effects = m_effectsOutsideArmsOfRow, tests = m_testsSeenOfRow, firstRanInCopy = m_firstOtherCodeRanInCopyOfRow, callStays = m_summaryHasUninlinedCall, asksLengths = m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces, takesApart = m_turnsOfRowTakeApartListsFromInFront, handlersRunOtherCode = m_handlersRunOtherCode, ran = m_operationsThatRanOtherCodeOfRow, guards = m_guardsOfStoresAskedOfRow]() mutable {
        m_rowSaysTurnsAreQuietOutsideArms = isQuiet;
        m_effectsOutsideArmsOfRow = effects;
        m_testsSeenOfRow = WTF::move(tests);
        m_firstOtherCodeRanInCopyOfRow = firstRanInCopy;
        m_summaryHasUninlinedCall = callStays;
        m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces = asksLengths;
        m_turnsOfRowTakeApartListsFromInFront = takesApart;
        m_handlersRunOtherCode = handlersRunOtherCode;
        m_operationsThatRanOtherCodeOfRow = WTF::move(ran);
        m_guardsOfStoresAskedOfRow = WTF::move(guards);
    });
    releaseEndedPendingBlocks();
    BlockID unswitchPreheader;
    BlockID fallbackPreheader;
    NamesOfWay namesReadBeforeLoop;
    bool hasTypedVersion = false;
    if (!m_unswitchCollectors.isEmpty() && (m_unswitchCollectors.last().frameIndex != frameIndex || m_unswitchCollectors.last().inputHead != inputHead))
        m_unswitchCollectors.last().holdsLoops = true;
    if (!m_rejectedUnswitchLoops.isEmpty() && m_rejectedUnswitchLoops.contains(std::pair { m_frames[frameIndex].function.index(), inputHead.index() }))
        m_loopSummaries[place].isUnswitchRejected = true;
    if (!m_loopSummaries[place].isUnswitchRejected && mayUnswitchLoop(inputHead)) {
        unswitchPreheader = m_block;
        fallbackPreheader = m_code.addBlock(BlockRole::Ordinary, m_code.at(unswitchPreheader).frequency);
        m_code.setLoopMarks(fallbackPreheader, m_code.at(unswitchPreheader).isInLoop, false);
        m_block = fallbackPreheader;
        OpenEdge inFrontOfLoop;
        takeNamesOntoEdge(inFrontOfLoop, frameIndex);
        namesReadBeforeLoop = WTF::move(inFrontOfLoop.names);
        const BitVector& loopsToUnswitch = m_loopsToUnswitch.find(&index)->value;
        m_unswitchCollectors.append(UnswitchCollector { frameIndex, inputHead, { }, loopsToUnswitch.get(2 * loop.index() + 1), loopsToUnswitch.get(2 * loop.index()) });
    }
    auto popUnswitchCollector = makeScopeExit([this, isOpened = !!unswitchPreheader] {
        if (isOpened)
            m_unswitchCollectors.removeLast();
    });
    auto endUnswitchPreheader = makeScopeExit([&] {
        if (!unswitchPreheader || m_hasFailed)
            return;
        m_code.end(unswitchPreheader, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
        m_code.closeEdge(m_code.at(unswitchPreheader).terminator.next(), fallbackPreheader, { });
    });
    auto ownerOfSnapshot = makeUniqueWithoutFastMallocCheck<SnapshotForTry>(*this);
    SnapshotForTry& snapshot = *ownerOfSnapshot;
    IterationsSteppedAtHead steppedInFrontOfLoop = takeIterationsSteppedAt(inputHead);
    bool stepsIteration = steppedInFrontOfLoop.asTheyEntered.containsIf([](const auto& entered) { return entered.second.otherForm == IndexedIteration::OtherForm::None; });
    bool stepsMutableIterable = steppedInFrontOfLoop.asTheyEntered.containsIf([](const auto& entered) { return entered.second.otherForm == IndexedIteration::OtherForm::None && entered.second.contents != IndexedIteration::Contents::Text; });
    bool generatorIsWalkedInLoop = m_generatorsTakenByConsumers.containsIf([&](const GeneratorTakenByConsumer& taken) {
        return taken.inputHead == inputHead && taken.inlineFrame == m_frames[frameIndex].inlineFrame;
    });
    auto buildSeveralTimes = [&](const LoopPreview& learned) {
        bool unswitches = !m_unswitchCandidates.empty() && !m_isVersioningDryRun;
        m_isUnswitchedVersionQuiet = false;
        m_hasUnswitchFailed = false;
        return !generatorIsWalkedInLoop && mayBuildLoopSeveralTimes(inputHead) && ((!unswitches && buildLoopForEachMember(inputHead, learned, enteringKept.span())) || tryVersionLoop(inputHead, learned, enteringKept.span())) && (!unswitches || m_isUnswitchedVersionQuiet);
    };
    auto makeBuildsAsRowSays = [&](LoopPreview& learned, const Vector<TypeID, 8>& entered, uint32_t numberOfEdgesThatCameRound, const auto& standInFrontAgain) {
        std::pair<uint32_t, BlockID> bought { static_cast<uint32_t>(frameIndex), inputHead };
        if (std::ranges::count(m_loopsInFinalBuild, bought) >= 2)
            return false;
        m_loopsInFinalBuild.append(bought);
        auto isBoughtNoLonger = makeScopeExit([&] {
            m_loopsInFinalBuild.removeLast();
        });
        auto prepareFrontOfBuilds = [&] {
            m_sumOfTypedBuildComesRoundUnproven = false;
            BodyBlock head = index.block(inputHead);
            for (uint32_t parameter = 0; parameter < head.numberOfParameters() && parameter < enteringKept.size(); ++parameter)
                m_frames[frameIndex].references.placeOf<NamesOfValues::Writer::PreparesFrontOfBuilds>(head.parameter(parameter).index()) = enteringKept[parameter];
            for (Reference argument : enteringKept)
                letLeave(m_state, argument);
            Vector<FreshObjectKeptAtHead, 2> keptInFrontOfBuilds;
            letMentionsOfLoopLeave(inputHead, m_loopSummaries[place].mentionsThatLeave.span(), &keptInFrontOfBuilds, m_loopSummaries[place].storesIntoScopes.areKnown);
            handScopesKeptAtHeadToPreview(index, inputHead, m_loopSummaries[place].storesIntoScopes, keptInFrontOfBuilds.span(), learned);
        };
        prepareFrontOfBuilds();
        bool isBuilt;
        {
            SetForScope inheritedHeaderScope(m_inheritedSummaryHeader, inputHead);
            SetForScope inheritedEscapingUsesScope(m_inheritedEscapingUses, m_loopSummaries[place].mentionsThatLeave);
            SetForScope inheritedScopeStoresScope(m_inheritedScopeStores, m_loopSummaries[place].storesIntoScopes);
            SetForScope inheritedFrameScope(m_inheritedSummaryFrame, frameIndex);
            SetForScope inheritedEntryTypesScope(m_inheritedEntryTypes, entered);
            SetForScope inheritedParameterTypesScope(m_inheritedParameterTypes, m_loopSummaries[place].types);
            SetForScope inheritedWideningRungsScope(m_inheritedWideningRungs, m_loopSummaries[place].rungs);
            SetForScope inheritedRarelyWidenedScope(m_inheritedRarelyWidenedParameters, m_loopSummaries[place].rarelyWidenedParameters);
            SetForScope inheritedOverreadWidenedScope(m_inheritedOverreadWidenedParameters, m_loopSummaries[place].overreadWidenedParameters);
            SetForScope buys(m_buysTwoBuilds, true);
            SetForScope listsStruck(m_listsAreStruckInRow, m_loopSummaries[place].listsAreStruck);
            SetForScope copyStruck(m_isInt32VersionRejected, m_loopSummaries[place].isInt32VersionRejected);
            SetForScope hasCopy(m_hasInt32Version, false);
            SetForScope otherCodeStays(m_otherCodeStaysInTypedBuildOfRow, m_loopSummaries[place].otherCodeStaysInTypedBuild);
            SetForScope keptOutInVain(m_otherCodeIsKeptOutOfFirstCopyInVainOfRow, m_loopSummaries[place].otherCodeIsKeptOutOfFirstCopyInVain);
            SetForScope stayedInVain(m_otherCodeStayedInVainOfRow, m_loopSummaries[place].otherCodeStayedInVain);
            SetForScope readsAskSites(m_readsAskSitesThoughCallStaysOfRow, m_loopSummaries[place].readsAskSitesThoughCallStays);
            SetForScope readWasKeptFromItsSite(m_readWasKeptFromItsSiteBecauseCallStays, false);
            m_typedBuildIsLeftAtOtherCodeOnUsualWay = false;
            m_everyTurnOfTypedBuildLeaves = false;
            bool turnAroundLeavesWithoutTest = std::exchange(m_turnOfTypedBuildLeavesWithoutTest, false);
            bool innerLoopExitsOuter = std::exchange(m_innerLoopExitsOnLikelyPath, false);
            m_typedBuildTestsNothing = false;
            m_typedBuildEndedListsTestedInFront = false;
            m_typedBuildAskedNoListThatItWasBoughtFor = false;
            m_typedBuildAsksGuardsThatItWasBoughtFor = false;
            isBuilt = buildSeveralTimes(learned);
            if (isBuilt && std::exchange(m_typedBuildEndedListsTestedInFront, false)) {
                m_turnOfTypedBuildLeavesWithoutTest = false;
                m_everyTurnOfTypedBuildLeaves = false;
                standInFrontAgain();
                prepareFrontOfBuilds();
                m_loopSummaries[place].listsAreStruck = true;
                m_listsAreStruckInRow = true;
                isBuilt = buildSeveralTimes(learned);
                isBuilt = !std::exchange(m_typedBuildEndedListsTestedInFront, false) && isBuilt;
            }
            auto isKept = [&](bool built) {
                if (std::exchange(m_innerLoopExitsOnLikelyPath, false))
                    built = false;
                bool leavesWithoutTest = std::exchange(m_turnOfTypedBuildLeavesWithoutTest, turnAroundLeavesWithoutTest);
                leavesWithoutTest &= std::exchange(m_typedBuildTestsNothing, false) || stepsMutableIterable;
                if ((std::exchange(m_everyTurnOfTypedBuildLeaves, false) || leavesWithoutTest) && numberOfEdgesThatCameRound)
                    built = false;
                if (std::exchange(m_typedBuildAsksGuardsThatItWasBoughtFor, false))
                    built = false;
                return !std::exchange(m_typedBuildAskedNoListThatItWasBoughtFor, false) && built;
            };
            isBuilt = isKept(isBuilt);
            if (!isBuilt && m_hasInt32Version && !m_hasUnswitchFailed && !m_hasFailed && !m_numberOfTryWhoseWalkIsCutShort) {
                standInFrontAgain();
                prepareFrontOfBuilds();
                m_isInt32VersionRejected = true;
                m_loopSummaries[place].isInt32VersionRejected = true;
                m_typedBuildIsLeftAtOtherCodeOnUsualWay = false;
                m_turnOfTypedBuildLeavesWithoutTest = false;
                isBuilt = buildSeveralTimes(learned);
                isBuilt = isKept(!std::exchange(m_typedBuildEndedListsTestedInFront, false) && isBuilt);
            }
            if (!isBuilt && m_readWasKeptFromItsSiteBecauseCallStays && !m_hasUnswitchFailed && !m_hasFailed && !m_numberOfTryWhoseWalkIsCutShort) {
                standInFrontAgain();
                prepareFrontOfBuilds();
                m_readsAskSitesThoughCallStaysOfRow = true;
                m_typedBuildIsLeftAtOtherCodeOnUsualWay = false;
                m_turnOfTypedBuildLeavesWithoutTest = false;
                isBuilt = buildSeveralTimes(learned);
                isBuilt = isKept(!std::exchange(m_typedBuildEndedListsTestedInFront, false) && isBuilt);
                m_readsAskSitesThoughCallStaysOfRow = isBuilt;
                m_loopSummaries[place].readsAskSitesThoughCallStays = isBuilt;
            }
            if (std::exchange(m_typedBuildIsLeftAtOtherCodeOnUsualWay, false) && isBuilt && !m_otherCodeStaysInTypedBuildOfRow) {
                for (bool otherCodeStays : { true, false }) {
                    if (m_hasFailed || m_numberOfTryWhoseWalkIsCutShort)
                        break;
                    standInFrontAgain();
                    prepareFrontOfBuilds();
                    m_loopSummaries[place].otherCodeStaysInTypedBuild = otherCodeStays;
                    m_otherCodeStaysInTypedBuildOfRow = otherCodeStays;
                    m_loopSummaries[place].otherCodeIsKeptOutOfFirstCopyInVain = m_otherCodeIsKeptOutOfFirstCopyInVainOfRow;
                    m_turnOfTypedBuildLeavesWithoutTest = false;
                    isBuilt = buildSeveralTimes(learned);
                    isBuilt = isKept(!std::exchange(m_typedBuildEndedListsTestedInFront, false) && isBuilt);
                    if (isBuilt)
                        break;
                    m_loopSummaries[place].otherCodeStayedInVain = true;
                    m_otherCodeStayedInVainOfRow = true;
                }
                m_typedBuildIsLeftAtOtherCodeOnUsualWay = false;
            }
            m_innerLoopExitsOnLikelyPath = innerLoopExitsOuter && !isBuilt;
            LoopSummary& row = m_loopSummaries[place];
            if (m_inheritedEscapingUses.size() != row.mentionsThatLeave.size() || m_inheritedScopeStores.fields.size() != row.storesIntoScopes.fields.size() || m_inheritedScopeStores.areKnown != row.storesIntoScopes.areKnown) {
                row.mentionsThatLeave = m_inheritedEscapingUses;
                row.storesIntoScopes.fields = m_inheritedScopeStores.fields;
                isBuilt = false;
            }
        }
        if (!m_hasFailed || m_numberOfTryWhoseWalkIsCutShort)
            return isBuilt;
        ++m_failedLoopVersioningCount;
        return isBuilt && m_inputs.options.verifiesCode;
    };
    auto makeBuilds = [&](LoopPreview& learned, const Vector<TypeID, 8>& entered, uint32_t numberOfEdgesThatCameRound, const auto& standInFrontAgain) {
        size_t numberOfRowsInFront = m_loopSummaries.size();
        bool listsAreStruck = m_loopSummaries[place].listsAreStruck;
        bool isInt32VersionRejected = m_loopSummaries[place].isInt32VersionRejected;
        bool otherCodeStays = m_loopSummaries[place].otherCodeStaysInTypedBuild;
        bool readsAskSites = m_loopSummaries[place].readsAskSitesThoughCallStays;
        bool otherCodeStayedInVain = m_loopSummaries[place].otherCodeStayedInVain;
        bool otherCodeIsKeptOutInVain = m_loopSummaries[place].otherCodeIsKeptOutOfFirstCopyInVain;
        bool comesRoundUnprovenAround = m_sumOfTypedBuildComesRoundUnproven;
        m_keyWasTakenForCounterAskedInFront = false;
        bool isBuilt = makeBuildsAsRowSays(learned, entered, numberOfEdgesThatCameRound, standInFrontAgain);
        bool sumWasAskedInVain = std::exchange(m_sumOfTypedBuildComesRoundUnproven, comesRoundUnprovenAround) && isBuilt && !m_sumsAreNotAskedInRow;
        bool keyWasTakenInVain = m_keyWasTakenForCounterAskedInFront && !isBuilt && !m_keysAreReadOffLookInRow;
        if ((!sumWasAskedInVain && !keyWasTakenInVain) || m_hasFailed || m_numberOfTryWhoseWalkIsCutShort)
            return isBuilt;
        m_loopSummaries.shrink(numberOfRowsInFront);
        m_loopSummaryIndices.removeIf([&](const auto& row) {
            return row.value >= numberOfRowsInFront;
        });
        m_loopSummaries[place].listsAreStruck = listsAreStruck;
        m_loopSummaries[place].isInt32VersionRejected = isInt32VersionRejected;
        m_loopSummaries[place].otherCodeStaysInTypedBuild = otherCodeStays;
        m_loopSummaries[place].readsAskSitesThoughCallStays = readsAskSites;
        if (keyWasTakenInVain) {
            m_loopSummaries[place].otherCodeStayedInVain = otherCodeStayedInVain;
            m_loopSummaries[place].otherCodeIsKeptOutOfFirstCopyInVain = otherCodeIsKeptOutInVain;
            m_rowsWhoseKeysAreReadOffLook.add(key, 0);
            m_keysAreReadOffLookInRow = true;
            return false;
        }
        m_unguardedSumLoops.add(key, 0);
        m_sumsAreNotAskedInRow = true;
        return false;
    };
    auto undoPlainWalk = [&] {
        snapshot.putBack(*this);
        m_frontOfLoopLeftOpen = { };
    };
    auto mayReenter = [&](const LoopPreview& learned) {
        bool isLeftAtCaughtThrow = m_loopsLeftAtCaughtThrows.contains(std::pair { static_cast<uint32_t>(frameIndex), inputHead });
        return isLeftAtCaughtThrow && !m_loopSummaries[place].isReentryRejected && !findReentry(frameIndex, inputHead) && !m_frames[frameIndex].loopsBeingVersioned.contains(loop)
            && !learned.effects.maySuspend && learned.slots.isEmpty() && learned.typesOfParameters.size() == enteringKept.size() && steppedInFrontOfLoop.asTheyEntered.isEmpty()
            && !m_coldCopyDepth && !m_numberOfClosedArms && (!isUnderHandler() || mayVersionLoopUnderHandler(inputHead)) && !hasStringsInBuilders() && !loopCarriesValuesOfConsumerRound(inputHead) && findValuesOfLoopReadBehindIt(inputHead);
    };
    auto visitFromReentry = [&]() __attribute__((noinline)) {
        Vector<TypeID, 8> types = m_loopSummaries[place].typesAtReentry;
        HoistedRows hoistedRows = m_hoistedRows;
        Vector<FieldArrayRead, 2> fieldArrayReads = m_fieldArrayReads;
        auto restoreHoistedReads = makeScopeExit([&] {
            m_hoistedRows = WTF::move(hoistedRows);
            m_fieldArrayReads = WTF::move(fieldArrayReads);
        });
        m_hoistedRows.shrink(numberOfRowsBefore);
        m_fieldArrayReads.shrink(numberOfFieldArraysBefore);
        uint32_t narrowerNamesAtHead = inputBlock(frameIndex, inputHead).firstNarrowerName;
        letMentionsOfLoopLeave(inputHead, std::span<const ValueID>(&everyFreshObject, 1));
        for (Reference argument : enteringKept)
            letLeave(argument);
        auto addEntry = [&](BlockFrequency frequency, Vector<Reference, 8>& parameters) {
            BlockID entry = addBlock(frequency);
            m_genericVersionEntries.add(entry.index());
            for (size_t parameter = 0; parameter < enteringKept.size(); ++parameter) {
                TypeID type = joinTypes(types[parameter], ownTypeOf(enteringKept[parameter]));
                parameters.append(addParameter(entry, type, formOfParameter(type, { })));
            }
            settleParametersOf(entry);
            return entry;
        };
        Vector<Reference, 8> reentryParameters;
        BlockID reentry = addEntry(m_code.at(m_block).frequency, reentryParameters);
        m_code.setLoopMarks(reentry, true, true);
        enterWithFieldsOfScopes(reentry, enteringKept.span());
        m_state.invalidate(m_loopSummaries[place].effects);
        m_state.clearRestartMark();
        if (m_state.holdsFreshObjects())
            m_state.clear();
        WalkState stateAtReentry = m_state;
        for (ValueID value : *findValuesOfLoopReadBehindIt(inputHead))
            m_frames[frameIndex].addDuplicatedValue(value);
        noteValuesThatHandlersOutsideLoopRead(inputHead);
        Vector<Reference, 8> iterationParameters;
        BlockID caughtThrowEntry = addEntry(BlockFrequency::Unlikely, iterationParameters);
        bool iterationReenters;
        {
            ClosedArm closedArm(*this);
            SetForScope coldCopyScope(m_coldCopyDepth, m_coldCopyDepth + 1);
            SetForScope ordinalOfIteration(m_ordinalOfCopyOfLoop, m_ordinalOfCopyOfLoop * 16 + 14);
            m_frames[frameIndex].loopsBeingVersioned.append(loop);
            standAheadOfEdges(caughtThrowEntry);
            iterationReenters = !m_hasFailed && walkSingleIteration(inputHead, iterationParameters.span(), reentry);
            m_frames[frameIndex].loopsBeingVersioned.removeLast();
        }
        if (m_hasFailed || m_numberOfTryWhoseWalkIsCutShort || !iterationReenters)
            return false;
        for (BlockID member : index.membersOf(loop))
            inputBlock(frameIndex, member).firstNarrowerName = narrowerNamesAtHead;
        forgetValuesOfLoop(inputHead);
        switchToInputBlock(inputHead);
        m_frames[frameIndex].isInPreheader = true;
        m_state = WTF::move(stateAtReentry);
        standAheadOfEdges(reentry);
        for (uint32_t parameter = 0; parameter < reentryParameters.size(); ++parameter)
            bind(index.block(inputHead).parameter(parameter), reentryParameters[parameter]);
        m_loopReentries.append(LoopReentry { frameIndex, inputHead, caughtThrowEntry });
        bool isVisited = runTrialVisits(inputHead, reentryParameters.span());
        m_loopReentries.removeLast();
        bool isUsed = false;
        m_code.forEachIncomingEdgeUnsealed(caughtThrowEntry, [&](const Edge&) {
            isUsed = true;
        });
        return isVisited && isUsed && !m_hasFailed;
    };
    auto makeTypedBuildBesidePlainWalk = [&](LoopPreview& learned, const TrialVisit& trialVisit) {
        FrontOfLoopLeftOpen front = std::exchange(m_frontOfLoopLeftOpen, { });
        Vector<ValueID, 8> readBehindLoop = *findValuesOfLoopReadBehindIt(inputHead);
        uint32_t firstValue = index.block(inputHead).firstParameter().index();
        uint32_t endOfValues = firstValue;
        for (BlockID member : index.membersOf(loop))
            endOfValues = std::max(endOfValues, index.block(member).endOfValues().index());
        Vector<Reference> boundByPlainWalk;
        boundByPlainWalk.append(m_frames[frameIndex].references.span().subspan(firstValue, endOfValues - firstValue));
        m_block = front.block;
        setSerialOfNewestMark(++m_numberOfMarksTaken);
        releaseEndedPendingBlocks();
        auto ownerOfMark = makeUniqueWithoutFastMallocCheck<SnapshotForTry>(*this);
        SnapshotForTry& behindPlainWalk = *ownerOfMark;
        auto undoTypedBuild = [&] {
            behindPlainWalk.putBack(*this);
            for (uint32_t value = firstValue; value < endOfValues; ++value)
                m_frames[frameIndex].references.placeOf<NamesOfValues::Writer::UndoesTypedBuild>(value) = boundByPlainWalk[value - firstValue];
        };
        BlockID entryOfPlainWalk;
        BlockID startOfPlainWalk;
        Vector<uint32_t, 8> edgesThatLeavePlainWalk;
        auto standBesidePlainWalk = [&] {
            Frame& frameOfLoop = m_frames[frameIndex];
            m_origin = snapshot.m_origin;
#define TAKE_BACK_FIELD_OF_FRAME(name) frameOfLoop.name = snapshot.name;
            FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_TAKES_BACK(TAKE_BACK_FIELD_OF_FRAME)
#undef TAKE_BACK_FIELD_OF_FRAME
            HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> takenBack;
            for (size_t logged = snapshot.numberOfExpectationsLogged, end = m_expectationsInTries.size(); logged < end; ++logged) {
                ExpectationInTry written = m_expectationsInTries[logged];
                if (written.frame >= m_frames.size() || m_frames[written.frame].expectations.isEmpty())
                    continue;
                if (!takenBack.add(static_cast<uint64_t>(written.frame) << 32 | written.value.index()).isNewEntry)
                    continue;
                m_expectationsInTries.append(ExpectationInTry { written.frame, written.value, m_frames[written.frame].expectations[written.value.index()] });
                m_frames[written.frame].expectations[written.value.index()] = written.before;
            }
            edgesThatLeavePlainWalk = nameValuesOnEdgesThatLeavePlainWalk(inputHead, readBehindLoop.span());
            BodyBlock head = index.block(inputHead);
            for (uint32_t parameter = 0; parameter < head.numberOfParameters(); ++parameter)
                m_frames[frameIndex].references.placeOf<NamesOfValues::Writer::StandsBesidePlainWalk>(head.parameter(parameter).index()) = enteringKept[parameter];
            BlockID start = addBlock();
            startOfPlainWalk = start;
            BlockID entry = addBlock();
            entryOfPlainWalk = entry;
            Vector<Reference, 8> enteringPlainWalk;
            for (size_t parameter = 0; parameter < enteringKept.size(); ++parameter) {
                TypeID type = m_code.at(m_code.at(front.head).parameters()[parameter]).type;
                enteringPlainWalk.append(addParameter(entry, type, formOfParameter(type, { })));
            }
            settleParametersOf(entry);
            m_state.clear();
            m_block = start;
            crossEdge({ }, entry, enteringKept.span());
            m_block = entry;
            m_leadsIntoPlainWalkThatIsKept = true;
            crossEdge({ }, front.head, enteringPlainWalk.span());
            m_leadsIntoPlainWalkThatIsKept = false;
            m_block = front.block;
            m_state = *trialVisit.stateInFront;
            m_sharedGenericVersions.append(SharedGenericVersion { frameIndex, inputHead, start, entry, true, true });
            m_buildsTestsBesidePlainWalk = true;
        };
        RELEASE_ASSERT(!m_code.at(front.head).isSealed);
        m_headsOfClosedLoops.remove(front.head.index());
        m_frontsSharedWithTypedBuilds.append(FrontSharedWithTypedBuild { frameIndex, inputHead, front.builtInFront });
        standBesidePlainWalk();
        m_reportOfTypedBuildMadeLast = std::nullopt;
        bool isBuilt = makeBuilds(learned, trialVisit.entered, trialVisit.numberOfEdgesThatCameRound, [&] {
            undoTypedBuild();
            standBesidePlainWalk();
            m_reportOfTypedBuildMadeLast = std::nullopt;
        });
        std::optional<ReportOfTypedBuild> report = std::exchange(m_reportOfTypedBuildMadeLast, std::nullopt);
        hasTypedVersion = !!report;
        m_buildsTestsBesidePlainWalk = false;
        m_headsOfClosedLoops.add(front.head.index());
        m_frontsSharedWithTypedBuilds.removeLast();
        if (m_numberOfTryWhoseWalkIsCutShort)
            return true;
        if (isBuilt && !m_hasFailed && !report)
            isBuilt = false;
        if (isBuilt && !m_hasFailed) {
            const Effects& ofTypedTurns = report->effects;
            isBuilt = trialVisit.assumed.mayRunOtherCode()
                ? !ofTypedTurns.maySuspend || trialVisit.assumed.maySuspend
                : !ofTypedTurns.mayRunOtherCode() && ofTypedTurns.writes.without(heapsNotHeldAgainstRowsOfLoops).isSubsetOf(trialVisit.assumed.writes);
        }
        if (!isBuilt) {
            undoTypedBuild();
            m_block = { };
            m_frontOfLoopLeftOpen = WTF::move(front);
            return false;
        }
        if (report && report->startOfOtherCopyIsRare)
            m_code.makeBlockRare(startOfPlainWalk);
        if (report && !report->genericVersionSpeculates) {
            m_code.makeBlocksRare(front.head.index(), behindPlainWalk.code.numberOfBlocks);
            m_code.makeBlockRare(entryOfPlainWalk);
            for (uint32_t place : edgesThatLeavePlainWalk)
                m_openEdges[place].isLedByClosedArm = true;
        }
        return true;
    };
    auto mayBuildUnswitchedVersion = [&] {
        if (!unswitchPreheader)
            return false;
        const auto& candidates = m_unswitchCollectors.last().candidates;
        return hasTypedVersion ? !candidates.isEmpty() : candidates.containsIf([](const UnswitchCandidate& candidate) { return candidate.isHoistable; });
    };
    auto buildUnswitchedVersion = [&](const TrialVisit* trialVisit, bool takesInt32sAlone) __attribute__((noinline)) {
        std::shared_ptr<const BuildsBought> request = m_loopSummaries[place].bought;
        if (trialVisit) {
            if ((!trialVisit->done.mayRunOtherCode() && !takesInt32sAlone) || m_loopSummaries[place].isPessimistic)
                return;
            auto made = std::make_shared<BuildsBought>();
            LoopSummary& summary = m_loopSummaries[place];
            Vector<TypeID, 8> typesOfPlainWalk = summary.types;
            narrowSummaryToObservedTypes(summary, *trialVisit);
            made->effects = summary.effects;
            made->typesOfParameters = std::exchange(summary.types, typesOfPlainWalk);
            made->typesOfValues = summary.typesOfValues;
            made->firstValue = summary.firstValue;
            made->entered = trialVisit->entered;
            made->effectsOfQuietDrafts = trialVisit->doneByQuietDrafts;
            made->tests = trialVisit->testsSeen;
            made->firstOtherCodeRanInCopy = trialVisit->firstOtherCodeRanInCopy;
            made->asksLengthsOfViewsFromInFrontForWrittenPlaces = trialVisit->asksLengthsOfViewsFromInFrontForWrittenPlaces;
            made->takesApartListsFromInFront = trialVisit->takesApartListsFromInFront;
            made->handlersRunOtherCode = trialVisit->handlersRunOtherCode;
            made->operationsThatRanOtherCode = trialVisit->operationsThatRanOtherCode;
            made->guardsOfStoresAsked = trialVisit->guardsOfStoresAsked;
            request = WTF::move(made);
        }
        Vector<UnswitchCandidate, 2> candidates = unswitchPreheader ? m_unswitchCollectors.last().candidates : Vector<UnswitchCandidate, 2>();
        candidates.removeAllMatching([&](const UnswitchCandidate& candidate) { return !candidate.isHoistable || (takesInt32sAlone && !candidate.asksForInt32s) || (request && candidate.reads.overlaps(request->effectsOfQuietDrafts.writes)); });
        if (request && !request->effects.mayRunOtherCode() && m_unswitchCollectors.last().holdsLoops)
            return;
        Vector<ValueID, 4> operationsOutsideCandidates;
        if (!request || candidates.isEmpty() || !isQuietOnceUnswitched(loop, request->operationsThatRanOtherCode.span(), candidates.span(), operationsOutsideCandidates))
            return;
        std::optional<Vector<ValueID, 8>> readBehindLoop = findValuesOfLoopReadBehindIt(inputHead);
        if (!readBehindLoop || m_hasFailed || m_numberOfTryWhoseWalkIsCutShort || walkAroundIsUndone())
            return;
        uint32_t firstValue = index.block(inputHead).firstParameter().index();
        uint32_t endOfValues = firstValue;
        for (BlockID member : index.membersOf(loop))
            endOfValues = std::max(endOfValues, index.block(member).endOfValues().index());
        uint32_t narrowerNamesAtHead = inputBlock(frameIndex, inputHead).firstNarrowerName;
        Vector<Reference> boundBefore;
        boundBefore.append(m_frames[frameIndex].references.span().subspan(firstValue, endOfValues - firstValue));
        setSerialOfNewestMark(++m_numberOfMarksTaken);
        releaseEndedPendingBlocks();
        auto ownerOfMark = makeUniqueWithoutFastMallocCheck<SnapshotForTry>(*this);
        SnapshotForTry& behindLoop = *ownerOfMark;
        auto undo = [&] {
            behindLoop.putBack(*this);
            for (uint32_t value = firstValue; value < endOfValues; ++value)
                m_frames[frameIndex].references.placeOf<NamesOfValues::Writer::UndoesTypedBuild>(value) = boundBefore[value - firstValue];
        };
        auto resetToPreheader = [&] {
            Frame& frameOfLoop = m_frames[frameIndex];
            m_origin = snapshot.m_origin;
#define TAKE_BACK_FIELD_OF_FRAME(name) frameOfLoop.name = snapshot.name;
            FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_TAKES_BACK(TAKE_BACK_FIELD_OF_FRAME)
#undef TAKE_BACK_FIELD_OF_FRAME
            HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> takenBack;
            for (size_t logged = snapshot.numberOfExpectationsLogged, end = m_expectationsInTries.size(); logged < end; ++logged) {
                ExpectationInTry written = m_expectationsInTries[logged];
                if (written.frame >= m_frames.size() || m_frames[written.frame].expectations.isEmpty())
                    continue;
                if (!takenBack.add(static_cast<uint64_t>(written.frame) << 32 | written.value.index()).isNewEntry)
                    continue;
                m_expectationsInTries.append(ExpectationInTry { written.frame, written.value, m_frames[written.frame].expectations[written.value.index()] });
                m_frames[written.frame].expectations[written.value.index()] = written.before;
            }
            nameValuesOnEdgesThatLeavePlainWalk(inputHead, readBehindLoop->span());
            forgetValuesOfLoop(inputHead);
            if (frameOfLoop.duplicatedValues) {
                forgetNamesOfWay(frameOfLoop);
                frameOfLoop.duplicatedValues->namesOfWay = namesReadBeforeLoop;
                frameOfLoop.duplicatedValues->blockThatWayEnters = inputHead;
                Vector<std::pair<ValueID, Reference>, 4> boundInFront;
                for (size_t logged = snapshot.numberOfBindings; logged < m_bindings.size(); ++logged) {
                    const Binding& binding = m_bindings[logged];
                    bool isOfLoop = binding.input.index() >= firstValue && binding.input.index() < index.numberOfValues();
                    if (binding.frame == frameIndex && !isOfLoop && !boundInFront.containsIf([&](const auto& bound) { return bound.first == binding.input; }))
                        boundInFront.append({ binding.input, binding.previous });
                }
                for (auto [value, name] : boundInFront) {
                    if (frameOfLoop.references[value.index()] != name)
                        bind(value, name);
                }
            }
            inputBlock(frameIndex, inputHead).firstNarrowerName = narrowerNamesAtHead;
            m_block = unswitchPreheader;
            m_state = snapshot.m_state;
        };
        resetToPreheader();
        m_rowSaysTurnsAreQuietOutsideArms = false;
        m_effectsOutsideArmsOfRow = request->effectsOfQuietDrafts;
        m_testsSeenOfRow = request->tests;
        m_firstOtherCodeRanInCopyOfRow = request->firstOtherCodeRanInCopy;
        m_summaryHasUninlinedCall = false;
        m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces = request->asksLengthsOfViewsFromInFrontForWrittenPlaces;
        m_turnsOfRowTakeApartListsFromInFront = request->takesApartListsFromInFront;
        m_handlersRunOtherCode = request->handlersRunOtherCode;
        m_operationsThatRanOtherCodeOfRow = operationsOutsideCandidates;
        m_guardsOfStoresAskedOfRow = request->guardsOfStoresAsked;
        auto ownerOfLearned = makeUniqueWithoutFastMallocCheck<LoopPreview>();
        LoopPreview& learned = *ownerOfLearned;
        learned.effects = request->effects;
        learned.typesOfParameters = request->typesOfParameters;
        learned.typesOfValues = request->typesOfValues;
        learned.firstValue = request->firstValue;
        learned.hasLetMentionsLeave = true;
        learned.slots = m_loopSummaries[place].storesIntoScopes.slots;
        LoopSummary& summaryOfLoop = m_loopSummaries[place];
        Vector<TypeID, 8> typesOfLoop = std::exchange(summaryOfLoop.types, request->typesOfParameters);
        bool listsAreStruck = std::exchange(summaryOfLoop.listsAreStruck, false);
        bool isInt32VersionRejected = std::exchange(summaryOfLoop.isInt32VersionRejected, false);
        bool otherCodeStays = std::exchange(summaryOfLoop.otherCodeStaysInTypedBuild, false);
        bool otherCodeIsKeptOutInVain = std::exchange(summaryOfLoop.otherCodeIsKeptOutOfFirstCopyInVain, false);
        bool otherCodeStayedInVain = std::exchange(summaryOfLoop.otherCodeStayedInVain, false);
        bool readsAskSites = std::exchange(summaryOfLoop.readsAskSitesThoughCallStays, false);
        SetForScope everyTurnLeaves(m_everyTurnOfTypedBuildLeaves, m_everyTurnOfTypedBuildLeaves);
        SetForScope leavesWithoutTest(m_turnOfTypedBuildLeavesWithoutTest, m_turnOfTypedBuildLeavesWithoutTest);
        SetForScope innerLoopExits(m_innerLoopExitsOnLikelyPath, false);
        SetForScope report(m_reportOfTypedBuildMadeLast, m_reportOfTypedBuildMadeLast);
        SetForScope walkWasKept(m_walkOfLoopWasKept, m_walkOfLoopWasKept);
        SetForScope headKeptLast(m_headOfLoopKeptLast, m_headOfLoopKeptLast);
        SetForScope frameKeptLast(m_frameOfLoopKeptLast, m_frameOfLoopKeptLast);
        SetForScope effectsKeptLast(m_effectsOfLoopKeptLast, m_effectsOfLoopKeptLast);
        SetForScope guardsKeptLast(m_guardsOfStoresAskedInLoopKeptLast, m_guardsOfStoresAskedInLoopKeptLast);
        bool isBuilt;
        {
            SetForScope unswitchesOn(m_unswitchCandidates, std::span<const UnswitchCandidate>(candidates.span()));
            isBuilt = makeBuilds(learned, request->entered, 1, [&] {
                undo();
                resetToPreheader();
            });
        }
        m_hasUnswitchFailed = false;
        LoopSummary& summaryBehindBuild = m_loopSummaries[place];
        summaryBehindBuild.types = typesOfLoop;
        summaryBehindBuild.listsAreStruck = listsAreStruck;
        summaryBehindBuild.isInt32VersionRejected = isInt32VersionRejected;
        summaryBehindBuild.otherCodeStaysInTypedBuild = otherCodeStays;
        summaryBehindBuild.otherCodeIsKeptOutOfFirstCopyInVain = otherCodeIsKeptOutInVain;
        summaryBehindBuild.otherCodeStayedInVain = otherCodeStayedInVain;
        summaryBehindBuild.readsAskSitesThoughCallStays = readsAskSites;
        if (m_numberOfTryWhoseWalkIsCutShort)
            return;
        if (isBuilt && m_hasFailed)
            return;
        if (!isBuilt) {
            if (takesInt32sAlone)
                m_loopSummaries[place].isInt32FallbackRejected = true;
            else {
                m_loopSummaries[place].isUnswitchRejected = true;
                m_rejectedUnswitchLoops.add(std::pair { m_frames[frameIndex].function.index(), inputHead.index() });
            }
            count(StatisticID::LoopUnswitchesRejected);
            undo();
            m_block = { };
            return;
        }
        count(StatisticID::LoopsUnswitched);
        m_block = { };
        m_state.clear();
        continueIn(m_unswitchFallbackStart);
        unswitchPreheader = m_unswitchFallbackStart;
        m_block = { };
        m_state.clear();
    };
    auto buildUnswitchedVersions = [&](const TrialVisit* trialVisit) __attribute__((noinline)) {
        const auto& candidates = m_unswitchCollectors.last().candidates;
        bool foldsBranches = candidates.containsIf([](const UnswitchCandidate& candidate) { return candidate.isHoistable && !candidate.asksForInt32s; });
        bool asksForInt32s = candidates.containsIf([](const UnswitchCandidate& candidate) { return candidate.isHoistable && candidate.asksForInt32s; });
        BlockID preheader = unswitchPreheader;
        if (trialVisit && !trialVisit->done.mayRunOtherCode()) {
            if (asksForInt32s && !m_loopSummaries[place].isInt32FallbackRejected)
                buildUnswitchedVersion(trialVisit, true);
            return;
        }
        buildUnswitchedVersion(trialVisit, false);
        if (foldsBranches && asksForInt32s && hasTypedVersion && unswitchPreheader != preheader && !m_hasFailed && !m_numberOfTryWhoseWalkIsCutShort && !m_loopSummaries[place].isInt32FallbackRejected)
            buildUnswitchedVersion(trialVisit, true);
    };
    if (!m_loopSummaries[place].typesAtReentry.isEmpty()) {
        if (visitFromReentry() || m_numberOfTryWhoseWalkIsCutShort)
            return true;
        undoPlainWalk();
        m_loopSummaries[place].typesAtReentry.clear();
        m_loopSummaries[place].isReentryRejected = true;
    }
    if (m_loopSummaries[place].bought && !m_loopSummaries[place].bought->keepsPlainWalk && has(Mechanism::LoopInvariants)) {
        std::shared_ptr<const BuildsBought> bought = m_loopSummaries[place].bought;
        bool sharedCopyWaits = !m_sharedGenericVersions.isEmpty() && m_sharedGenericVersions.last().frameIndex == frameIndex && m_sharedGenericVersions.last().inputHead == inputHead && m_sharedGenericVersions.last().isAboutToBeTaken;
        bool sameTypesEnter = bought->entered.size() == enteringKept.size() && (sharedCopyWaits || !m_frames[frameIndex].loopsBeingVersioned.contains(loop));
        for (size_t parameter = 0; sameTypesEnter && parameter < enteringKept.size(); ++parameter)
            sameTypesEnter = ownTypeOf(enteringKept[parameter]) == bought->entered[parameter];
        if (sameTypesEnter) {
            m_rowSaysTurnsAreQuietOutsideArms = bought->isQuietOutsideArms;
            m_effectsOutsideArmsOfRow = bought->effectsOfQuietDrafts;
            m_testsSeenOfRow = bought->tests;
            m_firstOtherCodeRanInCopyOfRow = bought->firstOtherCodeRanInCopy;
            m_summaryHasUninlinedCall = bought->hasUninlinedCall;
            m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces = bought->asksLengthsOfViewsFromInFrontForWrittenPlaces;
            m_turnsOfRowTakeApartListsFromInFront = bought->takesApartListsFromInFront;
            m_handlersRunOtherCode = bought->handlersRunOtherCode;
            m_operationsThatRanOtherCodeOfRow = bought->operationsThatRanOtherCode;
            m_guardsOfStoresAskedOfRow = bought->guardsOfStoresAsked;
            auto ownerOfLearned = makeUniqueWithoutFastMallocCheck<LoopPreview>();
            LoopPreview& learned = *ownerOfLearned;
            learned.effects = bought->effects;
            learned.typesOfParameters = bought->typesOfParameters;
            learned.typesOfValues = bought->typesOfValues;
            learned.firstValue = bought->firstValue;
            learned.hasLetMentionsLeave = true;
            learned.slots = m_loopSummaries[place].storesIntoScopes.slots;
            Vector<TypeID, 8> typesOfPlainWalk = m_loopSummaries[place].types;
            m_loopSummaries[place].types = bought->typesOfParameters;
            bool isBuilt = makeBuilds(learned, bought->entered, bought->numberOfEdgesThatCameRound, undoPlainWalk);
            m_loopSummaries[place].types = typesOfPlainWalk;
            if (isBuilt) {
                hasTypedVersion = true;
                if (mayBuildUnswitchedVersion())
                    buildUnswitchedVersions(nullptr);
            }
            if (isBuilt || m_numberOfTryWhoseWalkIsCutShort)
                return true;
            snapshot.putBack(*this);
        }
        m_loopSummaries[place].bought = nullptr;
    }
    bool mayBeBuiltSeveralTimes = !generatorIsWalkedInLoop && has(Mechanism::LoopInvariants) && !m_frames[frameIndex].loopsBeingVersioned.contains(loop) && mayBuildLoopSeveralTimes(inputHead);
    for (;;) {
        bool undoesWhatFits = m_inputs.options.repeatsFittingLoopVisits && !m_summariesRevisitedForCheck.contains(key);
        Vector<LoopSummary> answersBefore;
        decltype(m_loopSummaryIndices) placesOfAnswersBefore;
        if (undoesWhatFits && !hasUndoneWhatFits) {
            answersBefore = m_loopSummaries;
            placesOfAnswersBefore = m_loopSummaryIndices;
        }
        bool isWalkedInBuild = false;
        bool walksKeptWalkAlone = false;
        if (m_inputs.options.replaysFinalLoopVisits) [[unlikely]] {
            isWalkedInBuild = m_frames[frameIndex].loopsBeingVersioned.contains(loop);
            walksKeptWalkAlone = tryRestoreLastVisitSummary(place, isWalkedInBuild);
        }
        bool assumedTop = !m_loopSummaries[place].knowsEffects && !m_loopSummaries[place].isPessimistic;
        m_trialVisits.open(frameIndex, inputHead, place);
        m_frames[frameIndex].loopsInTrialVisits.set(loop.index());
        if (frameIndex < m_operationsBeingWalkedInFrames.size())
            m_operationsBeingWalkedInFrames[frameIndex] = { };
        m_trialVisits.last().firstRecord = m_code.numberOfValues();
        m_trialVisits.last().numberOfRecordsAtHead = m_code.numberOfValues();
        m_trialVisits.last().numberOfHeadParametersWithSourcesInFront = m_headParametersWithSources.size();
        noteHeadParametersThatEnter(m_trialVisits.last(), enteringKept.span());
        ++m_loopVisitCount;
        bool mayBeBoughtBehindWalk = !hasAskedForTwoBuilds && !m_loopSummaries[place].severalBuildsAreStruck && !m_loopSummaries[place].isPessimistic && mayBeBuiltSeveralTimes && !(m_loopSummaries[place].whatTurnsShowed & wasWalkedAgainForFront);
        m_trialVisits.last().wantsStateInFront = mayBeBoughtBehindWalk;
        m_leavesFrontOfLoopOpenBehindWalk = mayBeBoughtBehindWalk;
        auto typesBeforeWalk = m_loopSummaries[place].types;
        auto rungsBeforeWalk = m_loopSummaries[place].rungs;
        uint32_t nestCountBeforeVisit = m_versionedLoopNestCount;
        walkCopyOfLoop(inputHead, enteringKept.span());
        bool walkBeganTexts = m_walkOfLoopBeganTexts;
        std::unique_ptr<TrialVisit> ownerOfTried = m_trialVisits.takeLast();
        m_frames[frameIndex].loopsInTrialVisits.clear(loop.index());
        TrialVisit& trialVisit = *ownerOfTried;
        if (!trialVisit.objectsMadeInFrontThatAreWritten.isEmpty()) {
            std::ranges::sort(trialVisit.objectsMadeInFrontThatAreWritten);
            trialVisit.objectsMadeInFrontThatAreWritten.shrink(std::ranges::unique(trialVisit.objectsMadeInFrontThatAreWritten).begin() - trialVisit.objectsMadeInFrontThatAreWritten.begin());
            AllocationsUsedInLoop mentioned = trialVisit.othersAreSparedAtHead ? *trialVisit.othersAreSparedAtHead : allocationsUsedInLoop(inputHead);
            bool eachIsMentioned = std::ranges::all_of(trialVisit.objectsMadeInFrontThatAreWritten, [&](uint32_t made) {
                return !trialVisit.listsMadeHereThatAreSparedAtHead.contains(made) && std::ranges::binary_search(mentioned, made);
            });
            if (!eachIsMentioned) {
                trialVisit.done.writesObjectsMadeHereAlone = false;
                trialVisit.doneOutsideArms.writesObjectsMadeHereAlone = false;
                trialVisit.doneByQuietDrafts.writesObjectsMadeHereAlone = false;
            }
        }
        if (!m_trialVisits.isEmpty()) {
            TrialVisit& around = m_trialVisits.last();
            for (uint32_t made : trialVisit.objectsMadeInFrontThatAreWritten) {
                if (made < around.numberOfRecordsAtHead && (around.objectsMadeInFrontThatAreWritten.isEmpty() || around.objectsMadeInFrontThatAreWritten.last() != made))
                    around.objectsMadeInFrontThatAreWritten.append(made);
            }
            around.done.merge(trialVisit.done);
            around.writtenWithoutOtherCode.merge(trialVisit.writtenWithoutOtherCode);
            around.doneOutsideArms.merge(trialVisit.doneOutsideArms);
            around.doneByQuietDrafts.merge(trialVisit.doneByQuietDrafts);
            handUpWhatCameRoundBesideParameters(trialVisit);
        }
        bool isCutShort = false;
        if (m_numberOfTryWhoseWalkIsCutShort) {
            if (m_numberOfTryWhoseWalkIsCutShort <= m_trialVisits.size()) {
                LoopSummary& passedBy = m_loopSummaries[place];
                passedBy.effectsAreGuessed = false;
                passedBy.effectsAreGuessedByTextOfLoop = false;
                passedBy.knowsEffects = true;
                passedBy.effects.merge(trialVisit.done);
                bool maySuspend = passedBy.effects.maySuspend;
                passedBy.effects.merge(Effects::unknownCode());
                passedBy.effects.maySuspend = maySuspend;
                return true;
            }
            m_numberOfTryWhoseWalkIsCutShort = 0;
            m_hasFailed = false;
            isCutShort = true;
            ++m_abortedLoopVisitCount;
        }
        const Effects& assumed = trialVisit.assumed;
        LoopSummary& summary = m_loopSummaries[place];
        if (trialVisit.misfits && trialVisit.somethingIsWantedInside) {
            summary.types = WTF::move(typesBeforeWalk);
            summary.rungs = WTF::move(rungsBeforeWalk);
            for (size_t parameter = 0; parameter < trialVisit.startsStruck.size() && parameter < summary.types.size(); ++parameter) {
                if (!trialVisit.startsStruck[parameter])
                    continue;
                summary.types[parameter] = summary.enteredWhereStartWasStruck[parameter];
                summary.rungs[parameter] = 0;
            }
        } else if (trialVisit.misfits)
            letParametersHoldWhatTheyTakeRound(summary, trialVisit, inputBlock(frameIndex, inputHead).head);
        summary.narrowerStartHasMisfitted |= trialVisit.misfits && trialVisit.startIsNarrowerThanRowHeld;
        auto defersPurchase = [&] {
            return !m_trialVisits.isEmpty() && index.loop(loop).numberOfLoopsWithin > 1 && !m_loopSummaries[m_trialVisits.last().summaryIndex].hasFittedOnce;
        };
        ++summary.trialVisitCount;

        bool effectsFit = assumed.mayRunOtherCode()
            ? !trialVisit.done.maySuspend || assumed.maySuspend
            : !trialVisit.done.mayRunOtherCode() && trialVisit.done.writes.without(heapsNotHeldAgainstRowsOfLoops).isSubsetOf(assumed.writes)
                && (!assumed.writesObjectsMadeHereAlone || trialVisit.done.writesObjectsMadeHereAlone || !trialVisit.done.writes.containsAny(perObjectHeaps));
        bool effectsWereGuessed = std::exchange(summary.effectsAreGuessed, false);
        if (std::exchange(summary.effectsAreGuessedByTextOfLoop, false) && !effectsFit)
            ++m_refutedLoopEffectsCount;
        if (effectsWereGuessed && effectsFit && !isCutShort)
            summary.effects = trialVisit.done;
        else
            summary.effects.merge(trialVisit.done);
        summary.knowsEffects = true;
        if ((effectsWereGuessed || (assumedTop && assumed.mayRunOtherCode())) && effectsFit && !trialVisit.done.mayRunOtherCode() && !summary.isPessimistic && trialVisit.done != assumed) {
            if (defersPurchase())
                m_trialVisits.last().somethingIsWantedInside = true;
            else
                effectsFit = false;
        }
        if (!assumedTop && effectsFit && !trialVisit.misfits && assumed.mayRunOtherCode() && !trialVisit.done.mayRunOtherCode() && !summary.isPessimistic && !summary.didNarrowAssumedEffects) {
            summary.didNarrowAssumedEffects = true;
            summary.effects = trialVisit.done;
            if (defersPurchase())
                m_trialVisits.last().somethingIsWantedInside = true;
            else
                effectsFit = false;
        }
        if (summary.effects.mayRunOtherCode()) {
            bool maySuspend = summary.effects.maySuspend;
            summary.effects.merge(Effects::unknownCode());
            summary.effects.maySuspend = maySuspend;
        }
        bool isUnswitchedVersion = !m_versionedLoops.isEmpty() && m_versionedLoops.last().frameIndex == frameIndex && m_versionedLoops.last().inputHead == inputHead && !m_versionedLoops.last().unswitchCandidates.empty();
        if (isCutShort && isUnswitchedVersion && m_hasUnswitchFailed) {
            snapshot.putBack(*this);
            m_block = { };
            return true;
        }
        if (isCutShort && m_headOfTypedBuildOverListsTestedInFront == inputHead && m_frameOfTypedBuildOverListsTestedInFront == frameIndex) {
            m_typedBuildEndedListsTestedInFront = true;
            snapshot.putBack(*this);
            m_block = { };
            return true;
        }
        bool hasFailedInside = m_hasFailed;
        bool scopesWantAnotherWalk = learnWhatTurnsStoreIntoScopes(summary, trialVisit, !hasFailedInside && !isCutShort);
        scopesWantAnotherWalk |= trialVisit.requestsCarriedResult;
        if (!hasFailedInside && !isCutShort)
            scopesWantAnotherWalk |= recordLoopSlots(summary, trialVisit) | learnSlotsOfObjectsThatComeRound(summary, trialVisit);
        bool fits = effectsFit && !trialVisit.misfits && !trialVisit.freshObjectHasLeft && !hasFailedInside;
        if (!effectsFit && !trialVisit.misfits && !trialVisit.freshObjectHasLeft && !hasFailedInside && !summary.isPessimistic && !isCutShort)
            narrowSummaryToObservedTypes(summary, trialVisit);
        if (!hasFailedInside && !summary.isPessimistic && !isCutShort) {
            uint32_t firstValue = index.block(inputHead).firstParameter().index();
            summary.firstValue = firstValue;
            const BodyLoop& outermost = index.loop(index.loop(loop).outermost);
            uint32_t firstValueOfNest = index.block(outermost.head).firstParameter().index();
            summary.typesOfValues.clearAndBeginAt(firstValue, firstValueOfNest);
            auto appendRunOfBlocks = [&](BlockID first, BlockID last) {
                summary.typesOfValues.appendRun(index.block(first).firstParameter().index() - firstValue, index.block(last).endOfValues().index() - index.block(first).firstParameter().index());
            };
            forEachBlockAndLoopRightIn(index, loop, [&](BlockID member) {
                appendRunOfBlocks(member, member);
            }, [&](LoopID inside) {
                const BodyLoop& within = index.loop(inside);
                if (within.last.index() - within.head.index() + 1 != within.numberOfMembers)
                    return false;
                appendRunOfBlocks(within.head, within.last);
                return true;
            });
            struct RowOfLoopInside {
                LoopID loop;
                std::optional<uint32_t> place;
                bool isNotedInThisWalk { false };
            };
            Vector<RowOfLoopInside, 4> rowsOfLoopsInside;
            const TypesOfValuesOfLoop* largestNotedInThisWalk = nullptr;
            uint32_t numberOfLoopsLeft = index.loop(loop).numberOfLoopsWithin - 1;
            for (uint32_t inner = loop.index() + 1; numberOfLoopsLeft && inner < index.numberOfLoops(); ++inner) {
                if (index.loop(LoopID(inner)).parent != loop)
                    continue;
                numberOfLoopsLeft -= index.loop(LoopID(inner)).numberOfLoopsWithin;
                LoopSummaryKey copyOfInner = keyOfCopyOfLoop(frameIndex, index.loop(LoopID(inner)).head);
                uint32_t numberOfEntriesOfInner = m_entriesOfCopiesOfLoops.get(copyOfInner);
                auto found = numberOfEntriesOfInner ? m_loopSummaryIndices.find(loopSummaryKey(copyOfInner, numberOfEntriesOfInner - 1)) : m_loopSummaryIndices.end();
                RowOfLoopInside row { LoopID(inner), std::nullopt, false };
                if (found != m_loopSummaryIndices.end()) {
                    row.place = found->value;
                    const LoopSummary& ofInner = m_loopSummaries[found->value];
                    row.isNotedInThisWalk = ofInner.numberOfRecordsWhenTypesOfValuesWereNoted > trialVisit.firstRecord;
                    if (row.isNotedInThisWalk && !ofInner.typesOfValues.isEmpty() && (!largestNotedInThisWalk || ofInner.typesOfValues.size() > largestNotedInThisWalk->size()))
                        largestNotedInThisWalk = &ofInner.typesOfValues;
                }
                rowsOfLoopsInside.append(row);
            }
            summary.typesOfValues.makeRoomForRuns(index.block(outermost.last).endOfValues().index() - firstValueOfNest, largestNotedInThisWalk);
            summary.numberOfRecordsWhenTypesOfValuesWereNoted = m_code.numberOfValues();
            auto noteTypesOfValuesOf = [&](BlockID member) {
                BodyBlock block = index.block(member);
                summary.typesOfValues.setEach(block.firstParameter().index() - firstValue, block.endOfValues().index() - block.firstParameter().index(), [&](size_t place) {
                    Reference bound = m_frames[frameIndex].references[firstValue + place];
                    return bound && isStringInBuilder(*this, bound) ? TypeID(TypeTable::string) : ownTypeOf(bound);
                });
            };
            auto noteTypesLeftByTurnsWrittenOut = [&] {
                for (const Vector<TrialVisit::TypeLeftByTurnsWrittenOut>* list : { &trialVisit.typesLeftByTurnsWrittenOut, &trialVisit.typesOfSumsLeftInArms }) {
                    for (const TrialVisit::TypeLeftByTurnsWrittenOut& left : *list) {
                        if (left.value < firstValue || !summary.typesOfValues.holds(left.value - firstValue))
                            continue;
                        TypeID typeInRow = summary.typesOfValues[left.value - firstValue];
                        summary.typesOfValues.set(left.value - firstValue, typeInRow ? m_types.unionOf(typeInRow, left.type) : left.type);
                    }
                }
            };
            Vector<std::pair<LoopID, uint32_t>, 4> rowsTakenWhole;
            for (const RowOfLoopInside& row : rowsOfLoopsInside) {
                if (!row.isNotedInThisWalk) {
                    for (BlockID member : index.membersOf(row.loop))
                        noteTypesOfValuesOf(member);
                }
                if (!row.place)
                    continue;
                const LoopSummary& ofInner = m_loopSummaries[*row.place];
                LoopSummary& ofThis = m_loopSummaries[place];
                if (row.isNotedInThisWalk) {
                    if (&ofInner.typesOfValues != largestNotedInThisWalk)
                        ofThis.typesOfValues.takeWhole(ofInner.typesOfValues);
                    rowsTakenWhole.append({ row.loop, *row.place });
                } else
                    ofThis.typesOfValues.takeWhereNoTypeIsNoted(ofInner.typesOfValues);
            }
            forEachBlockAndLoopRightIn(index, loop, [&](BlockID member) {
                if (index.loopOf(member) == loop)
                    noteTypesOfValuesOf(member);
            }, [&](LoopID) {
                return true;
            });
            noteTypesLeftByTurnsWrittenOut();
            for (size_t parameter = 0; parameter < summary.types.size() && parameter < index.block(inputHead).numberOfParameters(); ++parameter) {
                if (summary.typesOfValues.holds(parameter) && !summary.typesOfValues[parameter])
                    summary.typesOfValues.set(parameter, summary.types[parameter]);
            }
            auto typeSumOfTexts = [&](const BodyValue& sum) {
                ValueID value = sum.id();
                if (summary.typesOfValues[value.index() - firstValue])
                    return;
                auto typeOfOperand = [&](ValueID operand) -> TypeID {
                    if (operand.index() >= firstValue && operand.index() - firstValue < summary.typesOfValues.size())
                        return summary.typesOfValues[operand.index() - firstValue];
                    return typeOfRecordForLooks(*this, ownTypeOf(m_frames[frameIndex].references[operand.index()]));
                };
                TypeID left = typeOfOperand(sum.operand(0));
                TypeID right = typeOfOperand(sum.operand(1));
                if (left && right && hasTextByItself(m_types.at(left)) && hasTextByItself(m_types.at(right)) && (m_types.at(left).isAnyString() || m_types.at(right).isAnyString()))
                    summary.typesOfValues.set(value.index() - firstValue, TypeTable::string);
                else
                    summary.typesOfValues.sumsWithoutType().append(value.index());
            };
            bool hasAdditions = index.holdsAnyOf(OpcodeSet { Opcode::Add });
            forEachBlockAndLoopRightIn(index, loop, [&](BlockID member) {
                if (!hasAdditions)
                    return;
                for (BodyValue operation : index.block(member).values()) {
                    if (operation.opcode() == Opcode::Add)
                        typeSumOfTexts(operation);
                }
            }, [&, next = size_t(0)](LoopID inside) mutable {
                while (next < rowsTakenWhole.size() && rowsTakenWhole[next].first.index() < inside.index())
                    ++next;
                if (next == rowsTakenWhole.size() || rowsTakenWhole[next].first != inside)
                    return false;
                for (uint32_t value : std::as_const(m_loopSummaries[rowsTakenWhole[next].second].typesOfValues).sumsWithoutType())
                    typeSumOfTexts(index.value(ValueID(value)));
                return true;
            });
            if (fits && !summary.hasAskedAboutTexts && canKeepStringsInBuilders(*this) && !summary.effects.maySuspend) {
                summary.hasAskedAboutTexts = true;
                auto ownerOfLearned = makeUniqueWithoutFastMallocCheck<LoopPreview>();
                LoopPreview& learned = *ownerOfLearned;
                learned.effects = summary.effects;
                learned.typesOfParameters.appendVector(summary.types);
                learned.typesOfValues = summary.typesOfValues;
                learned.firstValue = firstValue;
                Vector<GrowingText, 2> texts;
                findGrowingTexts(*this, inputHead, learned, texts);
                if (!texts.isEmpty() && !someTextGrowsInQuietTurn(inputHead, texts.span(), trialVisit))
                    keepTextsOfLoopOutOfBuilders(inputHead);
                if (!texts.isEmpty())
                    narrowSummaryToObservedTypes(summary, trialVisit);
                if (!texts.isEmpty() && defersPurchase())
                    m_trialVisits.last().somethingIsWantedInside = true;
                else if (!texts.isEmpty())
                    fits = false;
            }
        }
        if (!trialVisit.misfits && !hasFailedInside && !summary.isPessimistic && !isCutShort) {
            noteSourcesOfHeadParameters(frameIndex, inputHead, place, trialVisit, enteringKept.span());
            summary.enteredFromHeadsAroundWhenItFitted.shrink(0);
            for (size_t parameter = 0; parameter < trialVisit.entered.size() && parameter < trialVisit.headParametersThatEnter.size(); ++parameter)
                summary.enteredFromHeadsAroundWhenItFitted.append(trialVisit.headParametersThatEnter[parameter] ? trialVisit.entered[parameter] : TypeID());
            if (!trialVisit.headParametersThatEnter.isEmpty())
                summary.cameRoundBesideParametersWhenItFitted = trialVisit.cameRoundBesideItself;
        }
        if (fits && !summary.hasAskedAboutNarrowerForm && !summary.isPessimistic) {
            summary.hasAskedAboutNarrowerForm = true;
            bool narrows = false;
            std::optional<bool> turnsCheckTheirArithmetic;
            std::optional<Vector<TypeID, 16>> typesThatReach;
            Vector<TypeID, 8> reachingParameters;
            reachingParameters.fill(TypeID(), summary.types.size());
            for (size_t parameter = 0; parameter < summary.types.size() && parameter < trialVisit.cameRound.size() && parameter < trialVisit.entered.size(); ++parameter) {
                TypeID assumed = summary.types[parameter];
                if (!assumed || !trialVisit.cameRound[parameter] || !trialVisit.entered[parameter] || !m_types.at(assumed).isJavaScriptValue() || !m_types.at(trialVisit.cameRound[parameter]).isJavaScriptValue() || !m_types.at(trialVisit.entered[parameter]).isJavaScriptValue())
                    continue;
                TypeID narrowed = m_types.unionOf(trialVisit.entered[parameter], trialVisit.cameRound[parameter]);
                if (startIsGuessedInWalk(trialVisit, parameter) && parameter < trialVisit.cameRoundBesideItself.size() && trialVisit.cameRoundBesideItself[parameter])
                    narrowed = m_types.unionOf(trialVisit.entered[parameter], trialVisit.cameRoundBesideItself[parameter]);
                BlockID headOfCode = inputBlock(frameIndex, inputHead).head;
                if (headOfCode && parameter < m_code.at(headOfCode).parameters().size()) {
                    if (!typesThatReach)
                        typesThatReach = typesThatReachHeadParametersOfNest(trialVisit.firstRecord, trialVisit.numberOfHeadParametersWithSourcesInFront, m_headParametersWithSources.size());
                    auto noted = m_sourcesOfHeadParameters.find(m_code.at(headOfCode).parameters()[parameter].index());
                    bool isOfNest = noted != m_sourcesOfHeadParameters.end() && noted->value.place >= trialVisit.numberOfHeadParametersWithSourcesInFront;
                    TypeID reaches = isOfNest ? (*typesThatReach)[noted->value.place - trialVisit.numberOfHeadParametersWithSourcesInFront] : TypeID();
                    if (reaches && m_types.at(reaches).isJavaScriptValue() && m_types.isSubset(trialVisit.entered[parameter], reaches) && m_types.isSubset(reaches, narrowed))
                        narrowed = reaches;
                }
                if (narrowed == assumed || !m_types.isSubset(narrowed, assumed))
                    continue;
                reachingParameters[parameter] = narrowed;
                if (!turnsCheckTheirArithmetic)
                    turnsCheckTheirArithmetic = turnsCheckArithmeticOnIntegers(inputHead);
                if (!narrowerTypeOfHeadPaysWalk(assumed, narrowed, *turnsCheckTheirArithmetic))
                    continue;
                summary.types[parameter] = narrowed;
                narrows = true;
            }
            if (narrows) {
                for (size_t parameter = 0; parameter < reachingParameters.size(); ++parameter) {
                    if (reachingParameters[parameter])
                        summary.types[parameter] = reachingParameters[parameter];
                }
                trialVisit.narrowingsThatWait.shrink(0);
                trialVisit.narrowingsThatWait.append({ trialVisit.firstRecord, place, trialVisit.numberOfHeadParametersWithSourcesInFront, m_headParametersWithSources.size() });
            }
            Vector<bool, 8> wasOnDoubles = std::exchange(summary.steppedOnDoubles, findParametersSteppedOnDoubles(inputHead, summary));
            for (size_t parameter = 0; parameter < summary.types.size(); ++parameter) {
                bool was = parameter < wasOnDoubles.size() && wasOnDoubles[parameter];
                bool is = parameter < summary.steppedOnDoubles.size() && summary.steppedOnDoubles[parameter];
                narrows |= was != is && summary.types[parameter] && representationOfNumber(*this, summary.types[parameter]) == Representation::Int64;
            }
            if (narrows) {
                if (defersPurchase())
                    m_trialVisits.last().somethingIsWantedInside = true;
                else
                    fits = false;
            }
        }
        if (fits && (trialVisit.somethingIsWantedInside || scopesWantAnotherWalk)) {
            if (defersPurchase())
                m_trialVisits.last().somethingIsWantedInside = true;
            else
                fits = false;
        }
        if (fits && defersPurchase())
            m_trialVisits.last().narrowingsThatWait.appendVector(trialVisit.narrowingsThatWait);
        else {
            for (const TrialVisit::NarrowingThatWaits& narrowing : trialVisit.narrowingsThatWait)
                narrowRowsInsideTo(narrowing);
        }
        if (fits && quietTurnsWroteMoreThanRowSays(summary, trialVisit))
            fits = false;
        if (fits)
            summary.hasFittedOnce = true;
        if (fits && m_inheritedOverreadWidenedParameters && m_inheritedSummaryHeader == inputHead && m_inheritedSummaryFrame == frameIndex && !m_versionedLoops.isEmpty() && m_versionedLoops.last().frameIndex == frameIndex && m_versionedLoops.last().inputHead == inputHead) {
            for (size_t parameter = 0; parameter < summary.types.size() && parameter < 64; ++parameter) {
                if ((m_inheritedOverreadWidenedParameters >> parameter & 1) && summary.types[parameter] && m_types.at(summary.types[parameter]).isInteger())
                    count(StatisticID::AbsentElementWideningsAvoided);
            }
        }
        for (size_t parameter = 0; fits && parameter < 64 && parameter < summary.types.size() && summary.sumsThatStartAtNumberThatEnters >> parameter; ++parameter) {
            if ((summary.sumsThatStartAtNumberThatEnters >> parameter & 1) && summary.types[parameter] && m_types.at(summary.types[parameter]).isNumber())
                count(StatisticID::SumsSpecializedAsNumbers);
        }
        if (fits && !trialVisit.numberOfEdgesThatCameRound && !m_versionedLoops.isEmpty() && m_versionedLoops.last().inputHead == inputHead && m_versionedLoops.last().frameIndex == frameIndex)
            m_everyTurnOfTypedBuildLeaves = true;
        bool isNeverBuiltSeveralTimes = generatorIsWalkedInLoop || !m_opensGuesses || !has(Mechanism::Guesses) || index.hasIrreducibleFlow() || index.isEnteredByHandlerEdge(inputHead);
        bool isQuietOutsideArms = !trialVisit.testsSeen.isEmpty() && !trialVisit.doneOutsideArms.mayRunOtherCode() && trialVisit.done.mayRunOtherCode();
        bool isSharedGenericVersionPending = !m_sharedGenericVersions.isEmpty() && m_sharedGenericVersions.last().frameIndex == frameIndex && m_sharedGenericVersions.last().inputHead == inputHead && m_sharedGenericVersions.last().isAboutToBeTaken;
        if (fits && !hasAskedForTwoBuilds && !summary.severalBuildsAreStruck && !summary.isPessimistic && has(Mechanism::LoopInvariants) && (isSharedGenericVersionPending || !m_frames[frameIndex].loopsBeingVersioned.contains(loop)) && !isTooLargeToVersion(index, loop)
            && !isNeverBuiltSeveralTimes && !(trialVisit.callOnUsualWayComesRound && stepsMutableIterable) && !walkAroundIsUndone()) {
            hasAskedForTwoBuilds = true;
            auto ownerOfLearned = makeUniqueWithoutFastMallocCheck<LoopPreview>();
            LoopPreview& learned = *ownerOfLearned;
            summary.rarelyWidenedParameters = findRarelyWidenedParameters(trialVisit, summary, m_frames[frameIndex].blocks[inputHead.index()].head);
            summary.overreadWidenedParameters = findOverreadWidenedParameters(trialVisit, summary, m_frames[frameIndex].blocks[inputHead.index()].head);
            learned.effects = summary.effects;
            learned.typesOfParameters.appendVector(summary.types);
            learned.typesOfValues = summary.typesOfValues;
            learned.firstValue = summary.firstValue;
            learned.hasLetMentionsLeave = true;
            learned.slots = summary.storesIntoScopes.slots;
            bool mayByText;
            {
                m_rowSaysTurnsAreQuietOutsideArms = isQuietOutsideArms;
                m_effectsOutsideArmsOfRow = trialVisit.doneByQuietDrafts;
                m_testsSeenOfRow = trialVisit.testsSeen;
                m_firstOtherCodeRanInCopyOfRow = trialVisit.firstOtherCodeRanInCopy;
                m_summaryHasUninlinedCall = trialVisit.hasUninlinedCall;
                m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces = trialVisit.asksLengthsOfViewsFromInFrontForWrittenPlaces;
                m_turnsOfRowTakeApartListsFromInFront = trialVisit.takesApartListsFromInFront;
                m_handlersRunOtherCode = trialVisit.handlersRunOtherCode;
                m_operationsThatRanOtherCodeOfRow = trialVisit.operationsThatRanOtherCode;
                m_guardsOfStoresAskedOfRow = trialVisit.guardsOfStoresAsked;
                BodyBlock head = index.block(inputHead);
                Vector<Reference, 8> boundBehindWalk;
                for (uint32_t parameter = 0; parameter < head.numberOfParameters() && parameter < enteringKept.size(); ++parameter) {
                    boundBehindWalk.append(m_frames[frameIndex].references[head.parameter(parameter).index()]);
                    m_frames[frameIndex].references.placeOf<NamesOfValues::Writer::VersioningDryRun>(head.parameter(parameter).index()) = enteringKept[parameter];
                }
                SetForScope asksOnly(m_isVersioningDryRun, true);
                SetForScope listsStruckWhileAsking(m_listsAreStruckInRow, summary.listsAreStruck);
                auto ownerOfStateBehindWalk = makeUniqueWithoutFastMallocCheck<WalkState>(WTF::move(m_state));
                WalkState& stateBehindWalk = *ownerOfStateBehindWalk;
                BlockID blockBehindWalk = m_block;
                Frame& frameOfLoop = m_frames[frameIndex];
                auto blockOfFrameBehindWalk = frameOfLoop.block;
                bool hasHandlerBehindWalk = frameOfLoop.hasHandler;
                bool handlerOnlyClosesIterationsBehindWalk = frameOfLoop.handlerOnlyClosesIterations;
                auto narrowedValuesBehindWalk = frameOfLoop.narrowedValues;
                auto firstNarrowerNameBehindWalk = frameOfLoop.firstNarrowerName;
                auto listIsLongBehindWalk = frameOfLoop.listOfNarrowerNamesIsLong;
                Vector<ExpectationInTry> expectationsBehindWalk;
                size_t numberOfExpectationsLoggedBehindWalk = 0;
                auto wholeExpectationsBehindWalk = frameOfLoop.wholeExpectations;
                m_state = snapshot.m_state;
                m_block = snapshot.m_block;
                frameOfLoop.block = snapshot.block;
                frameOfLoop.hasHandler = snapshot.hasHandler;
                frameOfLoop.handlerOnlyClosesIterations = snapshot.handlerOnlyClosesIterations;
                frameOfLoop.narrowedValues = snapshot.narrowedValues;
                frameOfLoop.firstNarrowerName = snapshot.firstNarrowerName;
                frameOfLoop.listOfNarrowerNamesIsLong = snapshot.listOfNarrowerNamesIsLong;
                for (size_t place = m_expectationsInTries.size(); place-- > snapshot.numberOfExpectationsLogged;) {
                    const ExpectationInTry& logged = m_expectationsInTries[place];
                    if (logged.frame >= m_frames.size() || m_frames[logged.frame].expectations.isEmpty())
                        continue;
                    expectationsBehindWalk.append(ExpectationInTry { logged.frame, logged.value, m_frames[logged.frame].expectations[logged.value.index()] });
                    m_frames[logged.frame].expectations[logged.value.index()] = logged.before;
                }
                numberOfExpectationsLoggedBehindWalk = m_expectationsInTries.size();
                for (Reference argument : enteringKept)
                    letLeave(m_state, argument);
                Vector<FreshObjectKeptAtHead, 2> keptWhileAsking;
                letMentionsOfLoopLeave(inputHead, summary.mentionsThatLeave.span(), &keptWhileAsking, summary.storesIntoScopes.areKnown);
                auto findMissingTests = [&] {
                    Vector<ExpectationInTry> rememberedByPurchase;
                    for (size_t place = m_expectationsInTries.size(); place-- > numberOfExpectationsLoggedBehindWalk;) {
                        const ExpectationInTry& logged = m_expectationsInTries[place];
                        if (logged.frame >= m_frames.size() || m_frames[logged.frame].expectations.isEmpty())
                            continue;
                        rememberedByPurchase.append(ExpectationInTry { logged.frame, logged.value, m_frames[logged.frame].expectations[logged.value.index()] });
                        m_frames[logged.frame].expectations[logged.value.index()] = logged.before;
                    }
                    auto putRememberedBack = makeScopeExit([&] {
                        while (!rememberedByPurchase.isEmpty()) {
                            ExpectationInTry remembered = rememberedByPurchase.takeLast();
                            m_frames[remembered.frame].expectations[remembered.value.index()] = remembered.before;
                        }
                    });
                    auto typeInRow = [&](ValueID input) -> TypeID {
                        TypeID known;
                        if (input.index() >= summary.firstValue && input.index() - summary.firstValue < summary.typesOfValues.size())
                            known = summary.typesOfValues[input.index() - summary.firstValue];
                        else if (Reference built = referenceOf(input))
                            known = typeOf(built);
                        return known ? known : TypeID(TypeTable::anything);
                    };
                    BitVector operandsOfTestsSeen;
                    for (const GuessCountedOnByLooks& seen : m_testsSeenOfRow)
                        operandsOfTestsSeen.set(seen.operand.index());
                    for (BlockID member : index.membersOf(loop)) {
                        for (BodyValue operation : index.block(member).values()) {
                            forEachTypeThatGuessWouldProveAt(*this, operation, TypesOfInputs(typeInRow), [&](ValueID operand, TypeID expected, TypeID) {
                                if (operandsOfTestsSeen.get(operand.index()))
                                    return;
                                operandsOfTestsSeen.set(operand.index());
                                m_testsSeenOfRow.append(GuessCountedOnByLooks { operation.id(), operand, expected });
                            });
                        }
                    }
                };
                auto missingTestFinder = scopedLambda<void()>(findMissingTests);
                SetForScope finderOfMissingTests(m_missingTestFinder, trialVisit.done.mayRunOtherCode() ? &missingTestFinder : nullptr);
                Vector<std::pair<uint32_t, IndexedIteration>, 2> steppedBehindWalk;
                for (const auto& [placeOfIteration, entered] : steppedInFrontOfLoop.asTheyEntered)
                    steppedBehindWalk.append({ placeOfIteration, std::as_const(m_indexedIterations)[placeOfIteration] });
                uint32_t numberInFrontBehindWalk = m_numberOfIterationsInFrontOfTheirLoops;
                putBackIterationsSteppedAt(steppedInFrontOfLoop);
                m_genericVersionWouldSpeculate = false;
                mayByText = buildSeveralTimes(learned);
                for (auto& [placeOfIteration, behindWalk] : steppedBehindWalk)
                    m_indexedIterations[placeOfIteration] = WTF::move(behindWalk);
                m_numberOfIterationsInFrontOfTheirLoops = numberInFrontBehindWalk;
                if (mayByText)
                    findMissingTestsOfRow();
                m_state = WTF::move(stateBehindWalk);
                m_block = blockBehindWalk;
                Frame& frameBehindAsking = m_frames[frameIndex];
                frameBehindAsking.block = blockOfFrameBehindWalk;
                frameBehindAsking.hasHandler = hasHandlerBehindWalk;
                frameBehindAsking.handlerOnlyClosesIterations = handlerOnlyClosesIterationsBehindWalk;
                frameBehindAsking.narrowedValues = narrowedValuesBehindWalk;
                frameBehindAsking.firstNarrowerName = firstNarrowerNameBehindWalk;
                frameBehindAsking.listOfNarrowerNamesIsLong = listIsLongBehindWalk;
                while (m_expectationsInTries.size() > numberOfExpectationsLoggedBehindWalk) {
                    ExpectationInTry written = m_expectationsInTries.takeLast();
                    if (written.frame < m_frames.size() && !m_frames[written.frame].expectations.isEmpty())
                        m_frames[written.frame].expectations[written.value.index()] = written.before;
                }
                frameBehindAsking.wholeExpectations = WTF::move(wholeExpectationsBehindWalk);
                while (!expectationsBehindWalk.isEmpty()) {
                    ExpectationInTry behind = expectationsBehindWalk.takeLast();
                    m_frames[behind.frame].expectations[behind.value.index()] = behind.before;
                }
                for (uint32_t parameter = 0; parameter < boundBehindWalk.size(); ++parameter)
                    m_frames[frameIndex].references.placeOf<NamesOfValues::Writer::VersioningDryRun>(head.parameter(parameter).index()) = boundBehindWalk[parameter];
            }
            if (mayByText) {
                Vector<GuessCountedOnByLooks, 4> testsOfPurchase = m_testsSeenOfRow;
                Vector<TypeID, 8> typesOfPlainWalk = summary.types;
                narrowSummaryToObservedTypes(summary, trialVisit);
                learned.typesOfParameters.shrink(0);
                learned.typesOfParameters.appendVector(summary.types);
                bool leavesURLUnmade = keptByStrings() && notesOfStrings(*this).urlsThatAreNeverMade.containsIf([&](const NotesOfStrings::URLThatIsNeverMade& url) {
                    return url.inlineFrame == m_frames[frameIndex].inlineFrame && url.made.index() >= learned.firstValue && url.made.index() - learned.firstValue < learned.typesOfValues.size();
                });
                bool keepsPlainWalk = m_frontOfLoopLeftOpen.block && trialVisit.stateInFront && trialVisit.freshObjectsKept.isEmpty() && trialVisit.secondCounters.isEmpty() && m_genericVersionWouldSpeculate && !leavesURLUnmade && (m_versionedLoopNestCount == nestCountBeforeVisit || !m_versionedLoops.containsIf([&](const VersionedLoop& versionedLoop) { return versionedLoop.frameIndex != frameIndex || versionedLoop.inputHead != inputHead; }))
                    && !isSharedGenericVersionPending && !hasStringsInBuilders() && !walkBeganTexts && steppedInFrontOfLoop.asTheyEntered.isEmpty() && !handlersOutsideLoopReadItsValues(inputHead)
                    && !enteringKept.containsIf([&](Reference enters) { return isStringInBuilder(*this, enters); })
                    && !m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead))
                    && m_code.at(m_frontOfLoopLeftOpen.head).parameters().size() == enteringKept.size() && m_frontOfLoopLeftOpen.block.index() < m_frontOfLoopLeftOpen.head.index()
                    && !findRowThatHeadReadsOffListInFront(*this, inputHead, learned);
                bool isBuilt;
                bool sumsWereAsked = !m_sumsAreNotAskedInRow;
                bool keysWereTakenForCounters = !m_keysAreReadOffLookInRow;
                bool textsStayedOutOfBuilders = textsOfLoopStayOutOfBuilders(inputHead);
                if (keepsPlainWalk)
                    isBuilt = makeTypedBuildBesidePlainWalk(learned, trialVisit);
                else {
                    undoPlainWalk();
                    m_reportOfTypedBuildMadeLast = std::nullopt;
                    isBuilt = makeBuilds(learned, trialVisit.entered, trialVisit.numberOfEdgesThatCameRound, undoPlainWalk);
                    hasTypedVersion = isBuilt && m_reportOfTypedBuildMadeLast;
                }
                if (isBuilt && !m_hasFailed && !textsStayedOutOfBuilders && textsOfLoopStayOutOfBuilders(inputHead) && !m_numberOfTryWhoseWalkIsCutShort) {
                    keepsPlainWalk = false;
                    undoPlainWalk();
                    isBuilt = makeBuilds(learned, trialVisit.entered, trialVisit.numberOfEdgesThatCameRound, undoPlainWalk);
                }
                m_loopSummaries[place].types = typesOfPlainWalk;
                if (m_numberOfTryWhoseWalkIsCutShort)
                    return true;
                if (isBuilt && !m_hasFailed && mayReenter(learned)) {
                    m_loopSummaries[place].typesAtReentry = learned.typesOfParameters;
                    undoPlainWalk();
                    if (visitFromReentry() || m_numberOfTryWhoseWalkIsCutShort)
                        return true;
                    undoPlainWalk();
                    m_loopSummaries[place].typesAtReentry.clear();
                    m_loopSummaries[place].isReentryRejected = true;
                    hasAskedForTwoBuilds = false;
                    hasTypedVersion = false;
                    continue;
                }
                if (isBuilt) {
                    auto bought = std::make_shared<BuildsBought>();
                    bought->effects = learned.effects;
                    bought->typesOfParameters = learned.typesOfParameters;
                    bought->typesOfValues = learned.typesOfValues;
                    bought->firstValue = learned.firstValue;
                    bought->entered = trialVisit.entered;
                    bought->isQuietOutsideArms = isQuietOutsideArms;
                    bought->effectsOfQuietDrafts = trialVisit.doneByQuietDrafts;
                    bought->tests = testsOfPurchase;
                    bought->firstOtherCodeRanInCopy = trialVisit.firstOtherCodeRanInCopy;
                    bought->hasUninlinedCall = trialVisit.hasUninlinedCall;
                    bought->asksLengthsOfViewsFromInFrontForWrittenPlaces = trialVisit.asksLengthsOfViewsFromInFrontForWrittenPlaces;
                    bought->takesApartListsFromInFront = trialVisit.takesApartListsFromInFront;
                    bought->handlersRunOtherCode = trialVisit.handlersRunOtherCode;
                    bought->operationsThatRanOtherCode = trialVisit.operationsThatRanOtherCode;
                    bought->guardsOfStoresAsked = trialVisit.guardsOfStoresAsked;
                    bought->numberOfEdgesThatCameRound = trialVisit.numberOfEdgesThatCameRound;
                    bought->keepsPlainWalk = keepsPlainWalk;
                    m_loopSummaries[place].bought = WTF::move(bought);
                    if (keepsPlainWalk && trialVisit.assumedSummary)
                        recordSummaryAtLastVisit(place, isWalkedInBuild, trialVisit.assumedSummary);
                    if (mayBuildUnswitchedVersion())
                        buildUnswitchedVersions(&trialVisit);
                    return true;
                }
                if ((sumsWereAsked && m_sumsAreNotAskedInRow) || (keysWereTakenForCounters && m_keysAreReadOffLookInRow)) {
                    hasAskedForTwoBuilds = false;
                    undoPlainWalk();
                    continue;
                }
                m_loopSummaries[place].severalBuildsAreStruck = true;
                if (!keepsPlainWalk) {
                    undoPlainWalk();
                    continue;
                }
                if (place < answersBefore.size())
                    answersBefore[place].severalBuildsAreStruck = true;
            }
        }
        bool isUndoneForNet = false;
        if (undoesWhatFits && fits && !hasUndoneWhatFits) {
            hasUndoneWhatFits = true;
            m_summariesRevisitedForCheck.add(key, 0);
            fits = false;
            isUndoneForNet = true;
        }
        if (fits && frontOfQuietTurnsIsWorthAWalk(m_loopSummaries[place], trialVisit)) {
            if (defersPurchase())
                m_trialVisits.last().somethingIsWantedInside = true;
            else
                fits = false;
        }
        if (!fits && walksKeptWalkAlone) {
            fail("a visit of a loop does not fit the loop summary that its retained visit was made under"_s, InternalErrorKind::LoopSummaryMismatch);
            return true;
        }
        if (fits) {
            m_effectsOfLoopKeptLast = trialVisit.done;
            m_guardsOfStoresAskedInLoopKeptLast = trialVisit.guardsOfStoresAsked;
            m_walkOfLoopWasKept = true;
            m_headOfLoopKeptLast = inputHead;
            m_frameOfLoopKeptLast = frameIndex;
            if (trialVisit.assumedSummary)
                recordSummaryAtLastVisit(place, isWalkedInBuild, trialVisit.assumedSummary);
            endFrontOfLoopLeftOpen();
            if (mayBuildUnswitchedVersion())
                buildUnswitchedVersions(&trialVisit);
            break;
        }
        if (hasFailedInside && ((!trialVisit.misfits && effectsFit) || std::exchange(m_loopSummaries[place].hasFailedWhereItMisfitted, true))) {
            if (m_loopSummaries[place].isPessimistic)
                return true;
            m_loopSummaries[place].isPessimistic = true;
        }
        bool goesWithWalkAround = !isUndoneForNet && walkAroundIsUndone();
        if (goesWithWalkAround && !hasFailedInside && !isCutShort) {
            m_walkOfLoopWasKept = false;
            endFrontOfLoopLeftOpen();
            break;
        }
        snapshot.putBack(*this);
        if (goesWithWalkAround && hasFailedInside) {
            m_block = { };
            return true;
        }
        if (isUndoneForNet) {
            m_loopSummaries = WTF::move(answersBefore);
            m_loopSummaryIndices = WTF::move(placesOfAnswersBefore);
        }
    }
    return true;
}

bool Walker::looksAtLoopsAroundHaveCounted(const Draft& draft, const Effects& effects)
{
    if (looksAtLoopsAroundHaveSeen(effects))
        return true;
    const LinkTables& linked = facts().linked();
    if (effectRuleOf(draft.opcode) != EffectRule::Call || !isWrittenInTopFrame(draft) || linked.calls.isEmpty())
        return false;
    FunctionID callee = linked.calleeAt(currentFunction(), draft.inputValueGiven);
    if (!callee)
        return false;
    Effects ofCallee = linked.function(callee).effectsOnReturn();
    ofCallee.merge(linked.function(callee).effectsOnThrow());
    return looksAtLoopsAroundHaveSeen(effectsOf(typing(), draft, typesOfOperands(draft), { }, &ofCallee));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
