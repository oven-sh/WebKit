/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
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
#include <wtf/BitVector.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsNotHeldAgainstRowsOfLoops = bookkeepingHeaps | AbstractHeapSet { AbstractHeap::StringStorage };
static_assert(!heapsNotHeldAgainstRowsOfLoops.containsAny(AbstractHeapSet { AbstractHeap::ArrayStorage, AbstractHeap::ArrayLengths }));

static ValueID valueHandedOnInLoops(const BodyIndex& index, ValueID value)
{
    for (;;) {
        value = index.valueBehindCasts(value);
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
    return index.valueBehindCasts(relation.operand(0)) == parameter || index.valueBehindCasts(relation.operand(1)) == parameter;
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

LoopPreview previewThatClaimsNothingOfLoops(Walker& walker, BlockID inputHead, std::span<const TypeID> entryTypes, std::span<const TypeID> settledAround)
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
    if (!m_triesOfLoops.containsIf([](const TryOfLoop& tried) { return tried.knowsWhatIsAssumed && !tried.assumed.mayRunOtherCode(); }))
        return false;
    return m_numberOfHearingsUnderQuietTries++ % m_inputs.options.periodOfHearingsThatCutWalks == m_inputs.options.phaseOfHearingsThatCutWalks;
}

void Walker::renameFrameInTries(size_t from, size_t to)
{
    for (TryOfLoop& tried : m_triesOfLoops) {
        if (tried.frameIndex == from)
            tried.frameIndex = to;
    }
    for (auto& bought : m_loopsBeingBought) {
        if (bought.first == from)
            bought.first = static_cast<uint32_t>(to);
    }
    if (m_headOfRowHandedOn && m_frameOfRowHandedOn == from)
        m_frameOfRowHandedOn = to;
    if (from < m_operationsBeingWalkedInFrames.size()) {
        if (m_operationsBeingWalkedInFrames.size() <= to)
            m_operationsBeingWalkedInFrames.grow(to + 1);
        m_operationsBeingWalkedInFrames[to] = m_operationsBeingWalkedInFrames[from];
    }
    if (to > from) {
        m_sizesOfLogsWhereFramesWentOnTop.append({ m_bindings.size(), m_inputBlocksInTries.size(), m_expectationsInTries.size(), m_statesOfHandlersInTries.size(), m_placesOfOpenLoopsInTries.size() });
        return;
    }
    SizesOfLogsThatNameFrames since = m_sizesOfLogsWhereFramesWentOnTop.takeLast();
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
    for (size_t place = m_triesOfLoops.size(); place--;) {
        const TryOfLoop& tried = m_triesOfLoops[place];
        if (tried.frameIndex != m_frames.size() - 1 || tried.inputHead != inputHead)
            continue;
        AnswerOfLoop& answer = m_answersOfLoops[tried.answer];
        if (answer.claimsNothing)
            break;
        while (answer.types.size() <= placeAmongParameters) {
            answer.types.append(TypeID());
            answer.rungs.append(0);
        }
        TypeID& held = answer.types[placeAmongParameters];
        held = held ? m_types.unionOf(held, entering) : entering;
        return held;
    }
    return m_types.unionOf(m_types.anything(UnknownSeed::LostByWalk), entering);
}

bool Walker::isBeingTried(size_t frameIndex, BlockID inputHead) const
{
    const Frame& frame = m_frames[frameIndex];
    return frame.index->isLoopHead(inputHead) && frame.loopsBeingTried.get(frame.index->loopOf(inputHead).index());
}

#define FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_TAKES_BACK(macro) \
    macro(block) macro(endOfValues) macro(firstValueListedByBirthInFront) macro(endOfValuesListedByBirthInFront) macro(hasHandler) macro(handlerOnlyClosesIterations) macro(standsInFrontOfHead) macro(narrowedValues) macro(firstNarrowerName) macro(listOfNarrowerNamesIsLong) macro(loopLookedAt) macro(firstValueLookedAt) \
    macro(typesOfLooks) macro(wholeExpectations) macro(shapesOfValues) macro(blockOfNamesForHandler) macro(namesForHandler)

#define FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_LEAVES(macro) \
    macro(returnDominator) macro(walkedUpTo) macro(endOfStretch) macro(numberOfStretchesBeingWalked) macro(placeTakenByEndOfBlockAskedLast) \
    macro(numberOfFramesUpToHandlerBelow) macro(firstOperationToWalk) macro(hasPassedByWhatFeedsThrow) macro(beginOfStretch) macro(loopsBeingBuiltTwice) macro(headOfTurnsWrittenOut) \
    macro(turnsOfForIn) macro(valuesListedForTurnsWrittenOut) macro(stepWhosePositionEdgesBring) macro(placeOfForInWhosePositionEdgesBring) \
    macro(whereCopyLeavesForCall) macro(whereHeadEnds)

#define FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_LEAVES_AS_LONG_AS_IT_LEAVES_VALUES_BUILT_TWICE(macro) \
    macro(blocksWalkedForEachWay)

#define FOR_EACH_FIELD_OF_FRAME_IN_TRY(macro) \
    FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_TAKES_BACK(macro) FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_LEAVES(macro) \
    FOR_EACH_FIELD_OF_FRAME_IN_TRY_THAT_STANDING_BESIDE_PLAIN_WALK_LEAVES_AS_LONG_AS_IT_LEAVES_VALUES_BUILT_TWICE(macro)

#define FOR_EACH_FIELD_OF_FRAME_THAT_LOGS_PUT_BACK(macro) \
    macro(references) macro(expectations) macro(blocks) macro(statesOfHandlers) macro(placesOfStatesOfHandlers) macro(placesOfOpenLoops) \
    macro(narrowerNames) macro(placesOfNarrowerNames) macro(findsNarrowerNamesByReference) macro(narrowerNamesInTables) macro(firstNarrowerNamesInTables) macro(listInTables) \
    macro(valuesBuiltTwice) macro(booleansOfUnit) macro(returnsOfCopy)

#define FOR_EACH_FIELD_OF_FRAME_SET_WHERE_FRAME_IS_LAID(macro) \
    macro(function) macro(body) macro(index) macro(variablesStoredFirst) macro(loopDepthAtCall) macro(numberOfValuesBuiltBefore) macro(typeExpectedOfAnswer) macro(answerIsDeclared) macro(useOfAnswerAsKey) \
    macro(inlineFrame) macro(callsAreCountedByPlan) macro(callCopied) macro(isCopiedByLinkOfChain) macro(numberOfCopy) macro(mayLeaveForCall) macro(isWalkedWhereItsGuardFailed) macro(blocksOfHead) macro(handlerOfCodeBelow)

#define FOR_EACH_FIELD_OF_FRAME_THAT_ITS_WRITER_PUTS_BACK(macro) \
    macro(returnsInPlaceOfUnit) macro(isOfUnit) macro(countAmongFramesOfFunction) macro(loopsBeingTried)

#define FOR_EACH_FIELD_OF_FRAME_THAT_HANGS_ON_NO_STATE(macro) \
    macro(bodyCallsItself) \
    macro(valueHandedBack) macro(hasLookedForValueHandedBack) macro(handlerBlocks) macro(numberOfHandlersThatRead) macro(parametersHandedRoundUnchangedByLoop) \
    macro(outermostLoopsLookedThrough) macro(accessesUnderProvenKeys) macro(usesInLoops) macro(loopsWithBlocksTouched) macro(arithmeticInLoops) macro(operatorsFoundBehindValuesForGuesses) macro(endsOfUsesOfTexts)

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
    macro(m_state) macro(m_block) macro(m_origin) macro(m_opensGuesses) macro(m_firstFreeOpenEdge) macro(m_pendingBlocks) macro(m_numberOfClosedArms) \
    macro(m_numberOfIterationsInFrontOfTheirLoops) macro(m_isLeadingHandlerEdge) \
    macro(m_numberOfLoopsThatMayBeLeft) macro(m_numberOfScopesWhereProofsHoldEverywhere) \
    macro(m_loopsOverGenerators) macro(m_environmentCellsOfUnit) macro(m_operationsCopiedAtWrittenCall) macro(m_closuresThatAllocationAloneTakes) macro(m_valuesDefinedBehindBirth) macro(m_valuesWrittenBehindBirth) macro(m_layoutOfBirthThatListsWhatIsBehind) macro(m_numberOfCopiesBuiltForSize) macro(m_walksCopyWhereHolderIsNoBuiltin) macro(m_otherCopiesInsideTypedCopies) \
    macro(m_isWalkingAccess) macro(m_pollInFrontOfLoopBeingEntered) macro(m_edgeOfCodeBeingLed) macro(m_callCopiedByLinkOfChain) \
    macro(m_numberOfFramesAtCallCopiedByLinkOfChain) macro(m_callIsAccountedFor) macro(m_calleeAccountedFor) \
    macro(m_holdsReadsOfVariablesAsConstants) macro(m_countsOfUnit) macro(m_numberOfGuessesOpened) macro(m_numberOfListsAskedInFrontOfTypedCopies) macro(m_numberOfElementsAskedInFrontOfTypedCopies) macro(m_numberOfNumbersReadInFrontOfTypedCopies) macro(m_mechanisms) \
    macro(m_waysIntoClosedCopies) macro(m_closedCopiesShared) macro(m_listAskedForStorageOfNumbersInVain) macro(m_walksCleanupBesideCopyOfLoop) macro(m_copyThatMakesNewList) macro(m_receiverOfCopyThatMakesNewList) macro(m_depthOfCopyThatMakesNewList) \
    macro(m_hasURLsThatAreNeverMade) \
    macro(m_storageAskedOfLiteralBeingBorn) \
    macro(m_wordsThatSayNothing) macro(m_valueBeingWalked) macro(m_numberOfFreshCellsLeftAloneAtReturns) macro(m_numberOfTurnsThatBeginAgain) macro(m_numberOfStoresPutBack) macro(m_numberOfLoopsBuiltTwice) macro(m_numberOfLoopsThatHoldLoopsBuiltTwice) \
    macro(m_numberOfAssignmentsWrittenOut) macro(m_numberOfJoinsWalkedForEachWay) macro(m_numberOfQuestionsOfTruthAnsweredByWays) macro(m_numberOfCalleesTestedForFunctionsWrittenForTheirFields) macro(m_numberOfQuestionsForPlainInstancesLeftAsWritten) macro(m_numberOfQuestionsByNameLeftAsWritten) macro(m_numberOfEmptySlotsTakenAsCases) macro(m_numberOfSlicesMadeOfElementsInSight) \
    macro(m_numbersOfProofsChecked) macro(m_numberOfProofsThatNoTestTells) macro(m_numberOfProofsThatFormTells) macro(m_numberOfDoorsThatCanNeverPass) macro(m_numberOfChecksThatWalkAnswers) \
    macro(m_generatorsTakenByConsumers) macro(m_generatorsWalkedInConsumers) macro(m_resumptionsToLeave) macro(m_numberOfFramesOverHandlersOfCode) macro(m_believedAheadOfEdges) macro(m_leadsIntoPlainWalkThatIsKept) \
    macro(m_numberOfFramesAtRowOfShapeTests) macro(m_stretchWithArmsAsWritten) macro(m_numberOfObjectsMetAsOne) macro(m_numberOfRecordsBuiltToAskGuardsAgain)

#define FOR_EACH_FIELD_OF_WALKER_THAT_LOGS_PUT_BACK(macro) \
    macro(m_entriesOfCopiesOfLoops) macro(m_entriesOfCopiesOfLoopsInTries) macro(m_arrivalsHeld) macro(m_statesOfHandlersInTries) macro(m_placesOfOpenLoopsInTries) \
    macro(m_inputBlocksInTries) macro(m_expectationsInTries) macro(m_code) macro(m_frames) macro(m_numberOfValuesWalkedAndKept) macro(m_hasFailed) macro(m_typesShown) macro(m_typesAsked) \
    macro(m_failure) macro(m_kindOfFailure) macro(m_reports) macro(m_openEdges) macro(m_bindings) macro(m_sharedConstants) macro(m_numberOfSharedConstants) \
    macro(m_narrowedTypedLast) macro(m_copiesOfConstants) macro(m_numberOfCopiesOfConstants) macro(m_iterationsByIndex) macro(m_inputHeadsOfIterationsAddedInTries) \
    macro(m_typesChangedInTries) macro(m_placesOfLoopsChangedInTries) macro(m_sharedConstantsAddedInTries) macro(m_copiesOfConstantsAddedInTries) macro(m_fieldsOfScopesOfHeads) \
    macro(m_scopesThatClosuresHold) macro(m_scopesWhoseFieldsHaveComeRound) macro(m_blocksWithFieldsOfScopesInOrder) macro(m_inputHeadsOfIterations) \
    macro(m_openLoops) macro(m_placesOfLoopsBehindBlocks) macro(m_formAskedAhead) macro(m_coroutineState) macro(m_keptByStrings) macro(m_textsInStringBuilders) \
    macro(m_placesOfTextsInStringBuilders)

#define FOR_EACH_FIELD_OF_WALKER_SET_WHERE_UNIT_BEGINS(macro) \
    macro(m_isWalkingLastAlone) macro(m_inputs) macro(m_arrivals) macro(m_mechanismsOfBuild) macro(m_stepsOfBuildByOpcode) macro(m_stepsOfWalkByOpcode) macro(m_workerHasBegunUnit) macro(m_newestWalkerOfWorker) \
    macro(m_hasBegunUnit) macro(m_explainsClosedAccessesTo) macro(m_positionsOfBuild) macro(m_positions) macro(m_positionsKnowingWhatFunctionsHandBack) macro(m_positionsWithoutProofs) \
    macro(m_storesOfCapturedValues) macro(m_way)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ITS_WRITER_PUTS_BACK(macro) \
    macro(m_valuesStoodInFor) macro(m_whereListsAreMadeBehindEntry) macro(m_makerOfListsBehindEntry) macro(m_accessWhoseKeyIsTested) macro(m_depthOfCopiesAroundThatAccess) macro(m_accessWhoseKeyArmHolds) macro(m_depthOfCopiesAroundKeyHeldByArm) macro(m_staticFieldSeenInVariableByArm) macro(m_isMakingObjectsOfConstant) \
    macro(m_numberOfMarksOfTries) macro(m_nestingDepth) macro(m_serialOfNewestMark) macro(m_loopsBeingBought) macro(m_sizesOfLogsWhereFramesWentOnTop) macro(m_buysTwoBuilds) \
    macro(m_numberOfFormsHeardAsArms) macro(m_operationsThatRanOtherCodeOfRow) macro(m_guardsOfStoresAskedOfRow) macro(m_previewOutOfRow) macro(m_listsAreStruckInRow) macro(m_otherCodeStaysInTypedBuildOfRow) macro(m_otherCodeIsKeptOutOfFirstCopyInVainOfRow) macro(m_readsAskSitesThoughCallStaysOfRow) macro(m_sumsAreNotAskedInRow) macro(m_headOfTypedBuildOverListsTestedInFront) \
    macro(m_frameOfTypedBuildOverListsTestedInFront) macro(m_listsMadeHereOfTypedBuildOverListsTestedInFront) macro(m_headOfRowHandedOn) macro(m_frameOfRowHandedOn) macro(m_enteredOfRowHandedOn) macro(m_typesOfRowHandedOn) \
    macro(m_rungsOfRowHandedOn) macro(m_parametersThatRareWaysWidenedHandedOn) macro(m_parametersThatElementsOutsideViewsWidenedHandedOn) macro(m_firstOtherCodeRanInCopyOfRow) macro(m_someCallStaysInTurnOfRow) macro(m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces) macro(m_rowSaysTurnsAreQuietOutsideArms) macro(m_effectsOutsideArmsOfRow) \
    macro(m_testsSeenOfRow) macro(m_asksOnlyWhetherLoopMayBeBuiltTwice) macro(m_operatorsMadeInFrontOfLoops) macro(m_ordinalOfCopyOfLoop) macro(m_walksCopyWithoutGuessesOfSomeTurns) macro(m_walksCopyWithoutQuestionsOfEveryTurn) macro(m_walksCopyForAnotherKindOfStorage) macro(m_frontsSharedWithTypedBuilds) macro(m_inputHeadAskedAboutTypedCopy) \
    macro(m_numberOfStretchesBehindBytesOfMembers) macro(m_numbersOfFramesOfFunctions) macro(m_resultOfReadBeingTyped) macro(m_resultOfReadIsRemembered) macro(m_contentsBeingTyped) macro(m_handlerMadeOverCopies) \
    macro(m_numberOfFramesBelowHandlerMade) macro(m_buildsWhereItStands) macro(m_rowOfCopyFoundUnderKey) macro(m_numberOfLoopsAroundEdgeBeingLed) macro(m_readWhoseGuessIsOpen) macro(m_weighsWhatIsWritten) macro(m_makesLetterOfStep)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ITS_WRITER_SETS_AND_CLEARS(macro) \
    macro(m_questionOfTruthAnsweredOnWays) macro(m_answerOfThatQuestionOnWays) macro(m_frameOfThatQuestionOnWays) \
    macro(m_numberOfFieldsOfScopesOfHeadBeingOpened) macro(m_buildsTestsBesidePlainWalk) macro(m_expectationsRememberedForLook) macro(m_typesOfInputsInLook) \
    macro(m_looksTakeFormsBehindLooksAsBuilt) macro(m_walksCopyWhereItsGuardFailed) macro(m_expectedOfArgumentsOfCopies) macro(m_purchaseAsksWhetherTurnsCanBeQuiet) macro(m_resultOfCallBeingTyped) macro(m_heldInFrontOfStoreBeingBuilt) macro(m_storeBeingBuiltToPutBack) macro(m_isPuttingStoresBack)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_CHOOSES_WHAT_IS_TESTED(macro) \
    macro(m_whatMeetsInParameters) macro(m_keysStoredInFrontOfLastRow) macro(m_valuesThatEveryWayHandsToParameters) macro(m_loopsWhosePushFillsLiteral)

#define FOR_EACH_FIELD_OF_WALKER_THAT_HANGS_ON_NO_STATE(macro) \
    macro(m_listsThatCallsAloneTake) \
    macro(methodsGivenUnderKeys) macro(m_storesIntoRowsOfBlocks) macro(m_usersWithinBlocks) macro(m_valuesTakenForKeys) macro(m_loopsThatReckonWithElements) macro(m_numbersInListsOfWrittenObjects) macro(m_lastStoresByKeyOfLoops) macro(m_storesByKeyOfBlocks) macro(m_namesStoredInOutermostLoops) macro(m_whetherElementsOfLoopsMeetWhatGoesRound) macro(receiversMadeInBodies) macro(gettersExpectedInFunction) macro(m_placesSeenToStandInStretches) macro(m_keptByGeneratorsInLoops) \
    macro(m_listsOfMembersByTag) macro(m_membersByTagOfSets) macro(m_types) macro(m_roundsKept) macro(m_listsOfNames) macro(m_numbersOfListsOfNames) \
    macro(m_functionsInReachOfUnit) macro(m_surveysOfConstants) macro(m_constantsThatListFieldsHeldFilled) macro(m_positionsOfSlowBody) macro(m_positionsKnowingNothing) \
    macro(m_heldPositions) macro(m_valuesReadBehindLoopFoundLast) macro(m_accessesOfSlotsInLoopListedLast) macro(m_accessesOfScopesByFunctions) macro(m_listsAreIteratedByIndex) macro(m_objectPrototypeMayGainName) macro(m_keptByTypedAccesses) \
    macro(m_keptByUnionTableCalls) macro(m_keptByCopiesOwnKeys) macro(m_keptByPositionsInTexts) macro(m_keptForInt32Promises) macro(m_liveness) macro(m_callsListedOfFunction) \
    macro(m_sidesOfChoicesThatArriveAtBlocks) macro(m_namesGatheredForRoots)

#define FOR_EACH_FIELD_OF_WALKER_KEYED_BY_RECORDS_OR_BLOCKS_OF_CODE(macro) \
    macro(m_blocksBuiltWhereCountStoodForLength) macro(m_sourcesOfHeadParameters) macro(m_headParametersWithSources) macro(m_objectsMetAsOne) macro(m_fillersOfAllocations) \
    macro(m_headsOfClosedLoops) macro(m_entriesOfClosedCopies) macro(m_iterationsOfHeads)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_COUNTS_UP(macro) \
    macro(m_numberOfMarksTaken) macro(m_numberOfTypedCopiesEntered) macro(m_numberOfOperationsBuiltThatAreSeenIfRunAgain) macro(m_numberOfOperationsBuiltThatMayEnterByValue) macro(m_numberOfHearingsUnderQuietTries)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_NUMBERS_WHAT_STATES_HOLD(macro) \
    macro(m_numbersOfStates) macro(m_answersOfStates)

#define FOR_EACH_FIELD_OF_WALKER_THAT_NUMBERS_WHAT_STATES_OF_OPEN_TURNS_HOLD_AND_IS_EMPTIED_WHERE_NO_WAY_IS_OPEN(macro) \
    macro(m_storesToPutBack) macro(m_statesBehindStoresToPutBack)

#define FOR_EACH_FIELD_OF_WALKER_THAT_TRIES_LEARN(macro) \
    macro(m_rowsWalkedAgainForCheck) macro(m_rowsAsWalkedLastInBuilds) macro(m_answersOfLoops) macro(m_placesOfAnswersOfLoops) macro(m_triesOfLoops) macro(m_numberOfTryWhoseWalkIsCutShort) \
    macro(m_typedBuildEndedListsTestedInFront) macro(m_effectsOfLoopKeptLast) macro(m_guardsOfStoresAskedInLoopKeptLast) macro(m_typedBuildAsksGuardsThatItWasBoughtFor) macro(m_mentionsThatLeaveHandedOn) macro(m_storesIntoScopesHandedOn) macro(m_everyTurnOfTypedBuildLeaves) macro(m_readWasKeptFromItsSiteBecauseCallStays) \
    macro(m_turnOfTypedBuildLeavesWithoutTest) macro(m_typedBuildTestsNothing) macro(m_typedBuildIsLeftAtOtherCodeOnUsualWay) macro(m_sumOfTypedBuildComesRoundUnproven) macro(m_rowsWhoseSumsAreNotAsked) macro(m_effectsSeededForNextRow) macro(m_loopsWhoseTextsStayOutOfBuilders) macro(m_walkOfLoopBeganTexts) \
    macro(m_frontOfLoopLeftOpen) macro(m_leavesFrontOfLoopOpenBehindWalk) macro(m_otherCopyOpensItsGuessesAsAsked) macro(m_reportOfTypedBuildMadeLast) macro(m_walkOfLoopWasKept) \
    macro(m_headOfLoopKeptLast) macro(m_frameOfLoopKeptLast) macro(m_typedBuildAskedNoListThatItWasBoughtFor)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_REPORTS(macro) \
    macro(m_numberOfWalksOfLoops) macro(m_numberOfWalksOfLoopsUndone) macro(m_numberOfWalksOfLoopsCutShort) macro(m_numberOfPurchasesOfLoopsThatFailed) macro(m_numberOfValuesWalked) macro(m_functionOfFailure) \
    macro(m_failureIsSaid) macro(m_whatJudgeOfOperandsRefused)

#define FOR_EACH_FIELD_OF_WALKER_WRITTEN_IN_FRONT_OF_EVERY_READ(macro) \
    macro(m_questionOfTruthLeftToItsOwnTests) macro(m_frameOfThatQuestionOfTruth) macro(m_blockBuiltWhereListsWereMade) macro(m_opcodeBeingWalked) macro(m_operationsBeingWalkedInFrames) macro(m_lookHasAskedAboutUnbuiltValue) macro(m_operandTypes) macro(m_loopThatItsTryHasWalked) macro(m_operationWritesWhatTurnHasMade)

#define FOR_EACH_FIELD_OF_WALKER_WRITTEN_IN_FRONT_OF_EVERY_LOOP(macro) \
    macro(m_argumentsObjectsPutOffBehindEntry)

#define FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_WITHHOLDS_WHEN_STALE(macro) \
    macro(m_unitHasMadeScopes) macro(m_parametersThatSomeScanFoundToReadTexts)

#define FOR_EACH_FIELD_OF_WALKER(macro) \
    FOR_EACH_FIELD_OF_WALKER_IN_TRY(macro) \
    FOR_EACH_FIELD_OF_WALKER_THAT_LOGS_PUT_BACK(macro) FOR_EACH_FIELD_OF_WALKER_SET_WHERE_UNIT_BEGINS(macro) FOR_EACH_FIELD_OF_WALKER_THAT_ITS_WRITER_PUTS_BACK(macro) \
    FOR_EACH_FIELD_OF_WALKER_THAT_ITS_WRITER_SETS_AND_CLEARS(macro) FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_CHOOSES_WHAT_IS_TESTED(macro) FOR_EACH_FIELD_OF_WALKER_THAT_HANGS_ON_NO_STATE(macro) \
    FOR_EACH_FIELD_OF_WALKER_KEYED_BY_RECORDS_OR_BLOCKS_OF_CODE(macro) FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_COUNTS_UP(macro) FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_NUMBERS_WHAT_STATES_HOLD(macro) \
    FOR_EACH_FIELD_OF_WALKER_THAT_NUMBERS_WHAT_STATES_OF_OPEN_TURNS_HOLD_AND_IS_EMPTIED_WHERE_NO_WAY_IS_OPEN(macro) FOR_EACH_FIELD_OF_WALKER_THAT_TRIES_LEARN(macro) FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_REPORTS(macro) FOR_EACH_FIELD_OF_WALKER_WRITTEN_IN_FRONT_OF_EVERY_READ(macro) \
    FOR_EACH_FIELD_OF_WALKER_WRITTEN_IN_FRONT_OF_EVERY_LOOP(macro) FOR_EACH_FIELD_OF_WALKER_THAT_ONLY_WITHHOLDS_WHEN_STALE(macro)

#define FOR_EACH_FIELD_OF_WAY_THAT_CHOOSES_COST_OR_PLACE(macro) macro(otherCopyOpensItsGuesses) macro(frameIndex) macro(inputHead) macro(entry) macro(startFromInFront) macro(guessesCountedOn) macro(looksCountOnTurnsThatBeginAgain) macro(turnsCall) macro(someCallStaysInTurn) macro(relationThatAsksLesserBound) macro(turnsThatRunOtherCodeComeRound) macro(lastBlockThatEveryTurnPasses) macro(reckonsWithWhatItReads) macro(startOfCopyWithoutGuessesOfSomeTurns) macro(listAskedAtHead) macro(storageAskedOfListAtHead) macro(listAskedAtHeadIsWritable)
#define FOR_EACH_FIELD_OF_WAY_THAT_WALK_OF_TURN_WRITES(macro) macro(frontAsksWhatNothingSpeaksFor) macro(heldAtHead) macro(isEntered) macro(numberOfTurn) macro(headAsksLesserBound) macro(headThatAsksLesserBound) macro(headOfTurnInEffectsNet) macro(isLeftAtOtherCodeOnUsualWay) macro(variablesOfFilesHeldAtHead) macro(blocksThatPutStoresBack) macro(guessOfSomeTurnsIsAskedInFront) macro(questionOfEveryTurnIsAskedInFront) macro(isLeftAtAccessOnUsualWay)
#define FOR_EACH_FIELD_OF_WAY_ASKED_AGAIN_WHERE_IT_COMES_ROUND(macro) macro(countersHeld) macro(variablesHeld)
#define FOR_EACH_FIELD_OF_WAY_USED_WHILE_STATE_HOLDS_ITS_NOTE(macro) macro(lesserBound) macro(boundBesideLesserBound) macro(viewWhoseLengthIsBound) macro(variableThatIsBound) macro(readOfVariableThatIsBound) macro(counterBelowRoom)
#define FOR_EACH_FIELD_OF_WAY(macro) FOR_EACH_FIELD_OF_WAY_THAT_CHOOSES_COST_OR_PLACE(macro) FOR_EACH_FIELD_OF_WAY_THAT_WALK_OF_TURN_WRITES(macro) FOR_EACH_FIELD_OF_WAY_ASKED_AGAIN_WHERE_IT_COMES_ROUND(macro) FOR_EACH_FIELD_OF_WAY_USED_WHILE_STATE_HOLDS_ITS_NOTE(macro)

struct Walker::SnapshotForTry {
    static void holdListsToFieldsOf(Walker::WayIntoClosedCopy& way)
    {
        [[maybe_unused]] auto& [NAMES_BETWEEN_COMMAS(FOR_EACH_FIELD_OF_WAY(COMMA_AND_NAME))] = way;
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
        std::unique_ptr<Walker::Frame::ValuesBuiltTwice> valuesBuiltTwice;
#define DECLARE_FIELD_OF_FRAME(name) decltype(Walker::Frame::name) name;
        FOR_EACH_FIELD_OF_FRAME_IN_TRY(DECLARE_FIELD_OF_FRAME)
#undef DECLARE_FIELD_OF_FRAME
        size_t numberOfNarrowerNames { 0 };
        size_t numberOfPlacesOfNarrowerNames { 0 };
        uint32_t listInTables { noNarrowerName };

        ConsumerOfOpenGenerator(const Walker::Frame& frame, size_t frameIndex)
            : frameIndex(frameIndex)
            , valuesBuiltTwice(frame.valuesBuiltTwice ? frame.valuesBuiltTwice->copyForUndo() : nullptr)
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
            if (!valuesBuiltTwice)
                frame.valuesBuiltTwice = nullptr;
            else
                frame.valuesBuiltTwice->putBack(*valuesBuiltTwice);
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
    std::unique_ptr<Walker::Frame::ValuesBuiltTwice> valuesBuiltTwice;
    decltype(Walker::Frame::booleansOfUnit) booleansOfUnit;
#define DECLARE_FIELD_OF_FRAME(name) decltype(Walker::Frame::name) name;
    FOR_EACH_FIELD_OF_FRAME_IN_TRY(DECLARE_FIELD_OF_FRAME)
#undef DECLARE_FIELD_OF_FRAME
#define DECLARE_FIELD_OF_WALKER(name) std::conditional_t<std::is_same_v<decltype(Walker::name), BlockBeingBuilt>, BlockID, std::remove_reference_t<decltype(Walker::name)>> name;
    FOR_EACH_FIELD_OF_WALKER_IN_TRY(DECLARE_FIELD_OF_WALKER)
#undef DECLARE_FIELD_OF_WALKER
    Code::Mark code;
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
        : valuesBuiltTwice(walker.m_frames.last().valuesBuiltTwice ? walker.m_frames.last().valuesBuiltTwice->copyForUndo() : nullptr),
        booleansOfUnit(walker.m_frames.first().booleansOfUnit),
        FOR_EACH_FIELD_OF_FRAME_IN_TRY(COPY_FIELD_OF_FRAME)
#undef COPY_FIELD_OF_FRAME
#define COPY_FIELD_OF_WALKER(name) name(walker.name),
        FOR_EACH_FIELD_OF_WALKER_IN_TRY(COPY_FIELD_OF_WALKER)
#undef COPY_FIELD_OF_WALKER
        code(walker.m_code.mark())
        , numberOfFrames(walker.m_frames.size())
        , numberOfScopesThatClosuresHold(walker.m_scopesThatClosuresHold.size())
        , numberOfScopesWhoseFieldsHaveComeRound(walker.m_scopesWhoseFieldsHaveComeRound.size())
        , numberOfBlocksWithFieldsOfScopes(walker.m_blocksWithFieldsOfScopesInOrder.size())
        , numberOfTexts(walker.m_textsInStringBuilders.size())
        , numberOfArrivalsHeld(walker.m_arrivalsHeld.size())
        , numberOfTextsLogged(walker.m_textsInStringBuilders.sizeOfLog())
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
        , numberOfIterations(walker.m_iterationsByIndex.size())
        , numberOfIterationsLogged(walker.m_iterationsByIndex.sizeOfLog())
        , numberOfInputHeadsOfIterationsAdded(walker.m_inputHeadsOfIterationsAddedInTries.size())
        , numberOfTypesChanged(walker.m_typesChangedInTries.size())
        , numberOfEntriesOfCopiesOfLoops(walker.m_entriesOfCopiesOfLoopsInTries.size())
        , numberOfPlacesOfLoopsChanged(walker.m_placesOfLoopsChangedInTries.size())
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
            addOpenBlock(walker.m_openLoops.last().blockInFront);
        for (const PendingBlock& pending : walker.m_pendingBlocks)
            addOpenBlock(pending.block);
        std::ranges::sort(blocksThatAreOpen, { }, [](BlockID block) { return block.index(); });
        blocksThatAreOpen.shrink(std::ranges::unique(blocksThatAreOpen).begin() - blocksThatAreOpen.begin());
        for (BlockID block : blocksThatAreOpen)
            openBlocks.append(walker.m_code.stateOf(block));
    }

    void putBack(Walker& walker) const
    {
        ++walker.m_numberOfWalksOfLoopsUndone;
        walker.m_numberOfValuesWalkedAndKept = numberOfValuesWalkedAndKept;
        if (!walker.failureOutlivesTries()) {
            walker.m_hasFailed = false;
            walker.m_failure = { };
            walker.m_kindOfFailure = InternalErrorKind::Unknown;
        }
        walker.m_frames.shrink(numberOfFrames);
        walker.m_arrivalsHeld.shrink(numberOfArrivalsHeld);
        if (!valuesBuiltTwice)
            walker.m_frames.last().valuesBuiltTwice = nullptr;
        else
            walker.m_frames.last().valuesBuiltTwice->putBack(*valuesBuiltTwice);
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
        walker.forgetPlacesOfTextsInStringBuildersFrom(numberOfTexts);
        walker.m_textsInStringBuilders.putBack(numberOfTexts, numberOfTextsLogged);
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
        walker.m_iterationsByIndex.putBack(numberOfIterations, numberOfIterationsLogged);
        walker.forgetSharedConstantsAddedBehind(numberOfSharedConstantsAdded);
        walker.forgetCopiesOfConstantsAddedBehind(numberOfCopiesOfConstantsAdded);
        while (walker.m_placesOfLoopsChangedInTries.size() > numberOfPlacesOfLoopsChanged) {
            auto changed = walker.m_placesOfLoopsChangedInTries.takeLast();
            walker.m_placesOfLoopsBehindBlocks[changed.first] = changed.second;
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
            if (logged.frame < walker.m_frames.size() && !walker.m_frames[logged.frame].expectations.isEmpty() && !walker.m_inputs.options.undoLeavesExpectations)
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
        if (!value || value.index() >= firstValue)
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
        Reference object = referenceTo(rootOfCellInSweptCode(m_code, reference.id()));
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

Walker::TryOfLoop* Walker::findTryOfBlockAheadOfEdges(BlockID block)
{
    for (size_t place = m_triesOfLoops.size(); place--;) {
        TryOfLoop& tried = m_triesOfLoops[place];
        if (tried.frameIndex >= m_frames.size())
            continue;
        if (m_frames[tried.frameIndex].blocks[tried.inputHead.index()].head == block)
            return &tried;
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
    TryOfLoop* tried = findTryOfBlockAheadOfEdges(to);
    m_believedAheadOfEdges[believedHere].state.forEachBeliefOfFreshHolderLackingIn(m_state, [&](Reference holder, std::optional<FieldReference> fieldOfScope, Reference believed) {
        bool isCarriedRound = believed && believed.isValue() && m_code.at(believed.id()).opcode == Opcode::Parameter && m_code.at(believed.id()).block() == to;
        if (!tried) {
            if (!isCarriedRound)
                fail("what arrives at a block is not what was believed there"_s);
            return;
        }
        if (fieldOfScope && tried->keepsScopesAtHead && isScopeThatUnitMakes(holder)) {
            auto addInOrder = [&](Vector<FieldReference, 4>& fields) {
                auto place = std::ranges::lower_bound(fields, *fieldOfScope);
                if (place == fields.end() || *place != *fieldOfScope)
                    fields.insert(place - fields.begin(), *fieldOfScope);
            };
            addInOrder(tried->fieldsOfScopesStored);
            if (!m_state.environmentField(holder, *fieldOfScope))
                addInOrder(tried->fieldsOfScopesEnded);
            if (!isCarriedRound)
                tried->misfits = true;
            return;
        }
        if (isCarriedRound || (!believed && isScopeThatUnitMakes(holder) && m_state.isFresh(holder) && m_state.factsOfCell(holder).containsAll(m_believedAheadOfEdges[believedHere].state.factsOfCell(holder))))
            return;
        size_t kept = tried->freshObjectsKept.findIf([&](const FreshObjectKeptAtHead& object) {
            return object.object == holder;
        });
        if (believed == holder && kept != notFound && m_state.isFresh(holder) && !tried->objectsMadeInFrontThatAreWritten.contains(holder.id().index())) {
            count(StatisticID::ListsKeptAtHeadsThoughAnAnswerAboutThemWasForgotten);
            return;
        }
        loseObjectKeptAtHead(*tried, kept == notFound ? FreshObjectKeptAtHead { everyFreshObject, holder } : tried->freshObjectsKept[kept]);
    });
}

void Walker::loseObjectKeptAtHead(TryOfLoop& tried, const FreshObjectKeptAtHead& kept)
{
    tried.freshObjectHasLeft = true;
    Vector<ValueID, 2>& leave = m_answersOfLoops[tried.answer].mentionsThatLeave;
    if (!leave.contains(kept.mention))
        leave.append(kept.mention);
    bool isBuildOfRowHandedOn = m_headOfRowHandedOn == tried.inputHead && m_frameOfRowHandedOn == tried.frameIndex;
    if (isBuildOfRowHandedOn && isScopeThatUnitMakes(kept.object) && !m_mentionsThatLeaveHandedOn.contains(kept.mention))
        m_mentionsThatLeaveHandedOn.append(kept.mention);
}

TypeID Walker::climbLadderOfLoop(AnswerOfLoop& answer, size_t parameter, TypeID assumed, TypeID found, bool isFirstInTry)
{
    TypeID united = m_types.unionOf(assumed, found);
    const TryOfLoop& tried = m_triesOfLoops.last();
    TypeID top = m_headOfRowHandedOn == tried.inputHead && m_frameOfRowHandedOn == tried.frameIndex && &m_answersOfLoops[tried.answer] == &answer && parameter < m_typesOfRowHandedOn.size() ? m_typesOfRowHandedOn[parameter] : TypeID();
    uint8_t rung = isFirstInTry || !answer.rungs[parameter] ? answer.rungs[parameter] : answer.rungs[parameter] - 1;
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
            answer.rungs[parameter] = 4;
            return m_types.unionOf(m_types.anything(UnknownSeed::LostByWalk), united);
        }
        wider = m_types.unionOf(wider, united);
        if (!m_types.isSubset(wider, assumed)) {
            answer.rungs[parameter] = std::max<uint8_t>(answer.rungs[parameter], rung + 1);
            if (top && !m_types.isSubset(wider, top) && m_types.isSubset(united, top) && !m_types.isSubset(top, assumed))
                return top;
            return wider;
        }
    }
}

LoopPreview Walker::previewOutOfTries(BlockID inputHead, std::span<const TypeID> entryTypes)
{
    AnswerOfLoop& answer = m_answersOfLoops[m_triesOfLoops.last().answer];
    if (answer.claimsNothing)
        return previewThatClaimsNothingOfLoops(*this, inputHead, entryTypes, { });
    const BodyIndex& index = currentBodyIndex();
    Vector<bool, 8> isUnchanged;
    if (LoopID loop = index.loopOf(inputHead); answer.types.isEmpty() && !index.loop(loop).parent && index.loop(loop).numberOfLoopsWithin == 1)
        isUnchanged = parametersHandedRoundUnchangedInLoops(index, loop);
    else if (answer.types.isEmpty()) {
        LoopID outermost = loop;
        while (index.loop(outermost).parent)
            outermost = index.loop(outermost).parent;
        Frame& frame = m_frames.last();
        if (!frame.outermostLoopsLookedThrough.get(outermost.index())) {
            frame.outermostLoopsLookedThrough.set(outermost.index());
            findParametersHandedRoundUnchangedInLoopsWithin(index, outermost, frame.parametersHandedRoundUnchangedByLoop);
        }
        isUnchanged = frame.parametersHandedRoundUnchangedByLoop[loop.index()];
    }
    LoopPreview preview;
    preview.effects = answer.knowsEffects ? answer.effects : Effects::unknownCode();
    if (!answer.knowsEffects && (index.holdsAnyOf(opcodesWithSuspendingBoundInTable) || index.holdsAnyOf(OpcodeSet { Opcode::IteratorResume, Opcode::IteratorClose }))) {
        for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
            for (BodyValue operation : index.block(member).values())
                preview.effects.maySuspend |= maySuspendAsWritten(operation);
        }
    }
    if (!answer.knowsEffects && m_triesOfLoops.size() >= 2) {
        const Effects& assumedAround = m_triesOfLoops[m_triesOfLoops.size() - 2].assumed;
        if (!assumedAround.mayRunOtherCode() && !preview.effects.maySuspend)
            preview.effects = assumedAround;
    }
    if (!answer.knowsEffects && preview.effects.mayRunOtherCode() && !preview.effects.maySuspend) {
        const LinkedFunction& linked = facts().linked().function(currentFunction());
        if (!linked.effectsOnReturn().mayRunOtherCode() && !linked.effectsOnThrow().mayRunOtherCode()) {
            preview.effects = linked.effectsOnReturn();
            preview.effects.merge(linked.effectsOnThrow());
            answer.effectsAreGuessed = true;
        }
    }
    m_triesOfLoops.last().entered.shrink(0);
    m_triesOfLoops.last().entered.append(entryTypes);
    bool rowIsHandedOn = m_headOfRowHandedOn == inputHead && m_frameOfRowHandedOn == m_frames.size() - 1;
    bool sameTypesEnterAsInRowHandedOn = rowIsHandedOn && std::ranges::equal(entryTypes, m_enteredOfRowHandedOn);
    bool isTypedBuildOfPurchase = !m_waysIntoClosedCopies.isEmpty() && m_waysIntoClosedCopies.last().frameIndex == m_frames.size() - 1 && m_waysIntoClosedCopies.last().inputHead == inputHead;
    Vector<TypeID, 8> enteredWhenItFitted = std::exchange(answer.enteredFromHeadsAroundWhenItFitted, { });
    for (uint32_t parameter = 0; parameter < entryTypes.size(); ++parameter) {
        TypeID entering = entryTypes[parameter];
        bool isNew = answer.types.size() <= parameter;
        bool comesOutOfElementsOutsideViews = rowIsHandedOn && isTypedBuildOfPurchase && parameter < 64 && (m_parametersThatElementsOutsideViewsWidenedHandedOn >> parameter & 1);
        bool integerIsKeptInTypedBuild = comesOutOfElementsOutsideViews;
        if (isNew) {
            answer.types.append(entering);
            answer.rungs.append(0);
            bool startIsStruck = parameter < answer.enteredWhereStartWasStruck.size() && answer.enteredWhereStartWasStruck[parameter];
            if (entering && m_types.at(entering).isJavaScriptValue() && !(parameter < isUnchanged.size() && isUnchanged[parameter]) && !startIsStruck) {
                Type record = m_types.at(entering);
                if (record.primitives == Primitive::Integer && !record.integers.isEmpty() && record.integers.isSubsetOf(Range::int32())) {
                    if (std::optional<NumberThatDivisionsBringRound> broughtRound = findNumberThatDivisionsBringRound(*this, inputHead, parameter, entryTypes, WhatEntersHead::IsInteger)) {
                        answer.types[parameter] = m_types.unionOf(m_types.integer(broughtRound->integers), entering);
                        integerIsKeptInTypedBuild = true;
                    } else if (comesOutOfElementsOutsideViews)
                        answer.types[parameter] = m_types.unionOf(m_types.integer(Range { 0, Range::aroundEveryFraction().maximum }), entering);
                    else if (operatorsOnBitsAloneBringRound(*this, inputHead, parameter, entryTypes))
                        answer.types[parameter] = m_types.integer(Range::int32());
                    else if (record.integers.minimum == record.integers.maximum)
                        answer.types[parameter] = m_types.unionOf(widenedInLoops(m_types, entering, m_types.withIntegers(entering, Range { record.integers.minimum, record.integers.maximum + 1 })), entering);
                } else {
                    TypeID widened = m_types.withLiteralStringsWidened(entering);
                    if (record.primitives & Primitive::Boolean)
                        widened = m_types.unionOf(widened, TypeTable::boolean);
                    answer.types[parameter] = widened;
                }
                if (answer.types[parameter] != entering)
                    answer.rungs[parameter] = 1;
                size_t parameterAround = 0;
                ValueID around = parameter < m_triesOfLoops.last().headParametersThatEnter.size() ? m_triesOfLoops.last().headParametersThatEnter[parameter] : ValueID();
                const TryOfLoop* tryAround = around ? tryOfHeadParameter(around, parameterAround) : nullptr;
                if (answer.types[parameter] != entering || (tryAround && startIsGuessedInWalk(*tryAround, parameterAround))) {
                    while (answer.startIsWiderThanWhatEnters.size() <= parameter)
                        answer.startIsWiderThanWhatEnters.append(false);
                    answer.startIsWiderThanWhatEnters[parameter] = true;
                }
            }
            if (entering && m_types.at(entering).isJavaScriptValue() && !(parameter < isUnchanged.size() && isUnchanged[parameter])) {
                SetForScope standsInLoop(m_frames.last().standsInFrontOfHead, false);
                TypeID expected = findExpectation(*this, index.block(inputHead).parameter(parameter)).type;
                if (expected && m_types.at(expected).isJavaScriptValue() && !m_types.at(expected).isAnything() && !(m_types.at(expected).primitives & Primitive::Number) && !(m_types.at(entering).primitives & Primitive::Number) && m_types.at(entering).isUndefinedOrNull())
                    answer.types[parameter] = m_types.unionOf(entering, expected);
            }
        }
        bool mayStayIntegerInThisBuild = (!sameTypesEnterAsInRowHandedOn || (isTypedBuildOfPurchase && (integerIsKeptInTypedBuild || isComparedByTestOfHeadInLoops(index, inputHead, parameter)))) && entering && m_types.at(entering).isInteger() && parameter < m_typesOfRowHandedOn.size() && m_typesOfRowHandedOn[parameter] && !m_types.at(m_typesOfRowHandedOn[parameter]).isInteger();
        if (isNew && rowIsHandedOn && !mayStayIntegerInThisBuild && parameter < m_typesOfRowHandedOn.size() && parameter < m_enteredOfRowHandedOn.size() && m_enteredOfRowHandedOn[parameter] == entering) {
            TypeID ofPlainWalk = m_typesOfRowHandedOn[parameter];
            TypeID ownStart = answer.types[parameter];
            bool rowSaysWhatSumOfUnknownsGives = ofPlainWalk && ownStart && m_types.at(ownStart).isNumber() && m_types.at(ofPlainWalk).hasOnlyPrimitives(Primitive::Number | Primitive::AnyString | Primitive::BigInt);
            bool startsBelowPlainWalk = isTypedBuildOfPurchase && parameter < 64 && ((m_parametersThatRareWaysWidenedHandedOn >> parameter & 1) || rowSaysWhatSumOfUnknownsGives) && ofPlainWalk && ownStart && ownStart != ofPlainWalk
                && m_types.at(ofPlainWalk).isJavaScriptValue() && m_types.at(ownStart).isJavaScriptValue() && m_types.isSubset(ownStart, ofPlainWalk) && !m_types.isSubset(ofPlainWalk, topsOfKindsInLoops(m_types, ownStart));
            if (startsBelowPlainWalk && !(m_parametersThatRareWaysWidenedHandedOn >> parameter & 1))
                answer.sumsThatStartAtNumberThatEnters |= 1ull << parameter;
            if (!startsBelowPlainWalk) {
                answer.types[parameter] = ofPlainWalk;
                answer.rungs[parameter] = m_rungsOfRowHandedOn[parameter];
            }
        }
        TypeID& held = answer.types[parameter];
        if (parameter < enteredWhenItFitted.size() && enteredWhenItFitted[parameter] && entering && held && entering != enteredWhenItFitted[parameter] && !answer.narrowerStartHasMisfitted && m_types.at(entering).isJavaScriptValue() && m_types.at(held).isJavaScriptValue()) {
            TypeID beside = parameter < answer.cameRoundBesideParametersWhenItFitted.size() ? answer.cameRoundBesideParametersWhenItFitted[parameter] : TypeID();
            TypeID start = beside ? m_types.unionOf(entering, beside) : entering;
            m_triesOfLoops.last().startIsNarrowerThanRowHeld |= !m_types.isSubset(held, start);
            held = start;
        }
        if (entering && held && !m_types.isSubset(entering, held))
            held = m_types.unionOf(held, entering);
        preview.typesOfParameters.append(held);
    }
    preview.parametersSteppedOnDoubles = answer.steppedOnDoubles;
    if (answer.numberOfTries == 0 && answer.mentionsThatLeave.isEmpty() && m_headOfRowHandedOn == inputHead && m_frameOfRowHandedOn == m_frames.size() - 1)
        answer.mentionsThatLeave = m_mentionsThatLeaveHandedOn;
    if (!answer.storesIntoScopes.areKnown && rowIsHandedOn)
        answer.storesIntoScopes = m_storesIntoScopesHandedOn;
    if (rowIsHandedOn && unitHasMadeScopes())
        handBackScopesThatBuildLetsLeave(answer.mentionsThatLeave.span());
    m_triesOfLoops.last().freshObjectsKept.shrink(0);
    letMentionsOfLoopLeave(inputHead, answer.mentionsThatLeave.span(), &m_triesOfLoops.last().freshObjectsKept);
    handScopesKeptAtHeadToPreview(answer.storesIntoScopes, m_triesOfLoops.last().freshObjectsKept.span(), preview);
    preview.hasLetMentionsLeave = true;
    if (!answer.typesOfValues.isEmpty()) {
        preview.typesOfValues = answer.typesOfValues;
        preview.firstValue = answer.firstValue;
    }
    if (m_inputs.options.walksLastWalksOfLoopsAlone && !m_isWalkingLastAlone)
        m_triesOfLoops.last().rowAsAssumed = std::make_shared<AnswerOfLoop>(answer);
    return preview;
}

void Walker::noteTypeOfSumLeftInArm(ValueID sum)
{
    Reference bound = referenceBoundTo(sum);
    if (!bound)
        return;
    TypeID type = isTextInStringBuilder(*this, bound) ? TypeID(TypeTable::string) : typeOfRecordForLooks(*this, ownTypeOf(bound));
    if (type)
        m_triesOfLoops.last().typesOfSumsLeftInArms.append({ sum.index(), type });
}

void Walker::narrowRowToWhatCameRound(AnswerOfLoop& answer, const TryOfLoop& tried)
{
    for (size_t parameter = 0; parameter < answer.types.size() && parameter < tried.cameRound.size() && parameter < tried.entered.size(); ++parameter) {
        TypeID assumed = answer.types[parameter];
        if (!assumed || !tried.cameRound[parameter] || !tried.entered[parameter] || !m_types.at(assumed).isJavaScriptValue() || !m_types.at(tried.cameRound[parameter]).isJavaScriptValue() || !m_types.at(tried.entered[parameter]).isJavaScriptValue())
            continue;
        TypeID narrowed = m_types.unionOf(tried.entered[parameter], tried.cameRound[parameter]);
        if (m_types.isSubset(narrowed, assumed))
            answer.types[parameter] = narrowed;
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

Vector<bool, 8> Walker::findParametersSteppedOnDoubles(BlockID inputHead, const AnswerOfLoop& answer)
{
    const BodyIndex& index = currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    BodyBlock head = index.block(inputHead);
    Vector<bool, 8> isStepped;
    if (answer.typesOfValues.isEmpty() || head.numberOfParameters() > 32)
        return isStepped;
    uint32_t firstValue = answer.firstValue;
    uint32_t endOfValues = firstValue + static_cast<uint32_t>(answer.typesOfValues.size());
    auto typeInRow = [&](ValueID value) -> TypeID {
        if (!value)
            return { };
        if (value.index() >= firstValue)
            return value.index() < endOfValues ? answer.typesOfValues[value.index() - firstValue] : TypeID();
        Reference reference = referenceOf(value);
        return reference && reference.isRecord() ? m_code.at(reference.id()).type : TypeID();
    };
    constexpr OpcodeSet steps { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    auto forEachBlockInLoops = [&](const auto& functor) {
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            if (index.loopOf(BlockID(number)))
                functor(index.block(BlockID(number)));
        }
    };
    if (!m_frames.last().usesInLoops) {
        m_frames.last().usesInLoops = makeUniqueWithoutFastMallocCheck<UsesInLoops>();
        UsesInLoops& listed = *m_frames.last().usesInLoops;
        listed.newestStepOfLoop.fill(UINT32_MAX, index.numberOfLoops());
        forEachBlockInLoops([&](BodyBlock block) {
            for (BodyValue step : block.values()) {
                if (!steps.contains(step.opcode()))
                    continue;
                for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(step.opcode()); ++operandIndex) {
                    ValueID stepped = index.valueBehindCasts(step.operand(operandIndex));
                    BlockID holder = index.isParameter(stepped) ? index.blockOf(stepped) : BlockID();
                    if (!holder || !index.isLoopHead(holder) || !index.isInLoop(block.id(), index.loopOf(holder)))
                        continue;
                    uint32_t older = std::exchange(listed.newestStepOfLoop[index.loopOf(holder).index()], static_cast<uint32_t>(listed.steps.size()));
                    listed.steps.append({ step.id(), older });
                }
            }
        });
    }
    UsesInLoops& listed = *m_frames.last().usesInLoops;
    for (uint32_t place = listed.newestStepOfLoop[loop.index()]; place != UINT32_MAX; place = listed.steps[place].second) {
        BodyValue step = index.value(listed.steps[place].first);
        uint32_t numberOfOperands = numberOfNamedOperandsOf(step.opcode());
        TypeID result = typeInRow(step.id());
        TypeID first = typeInRow(step.operand(0));
        TypeID second = numberOfOperands == 2 ? typeInRow(step.operand(1)) : TypeID();
        if (!result || !first || (numberOfOperands == 2 && !second))
            continue;
        const Type& made = m_types.at(result);
        if (!made.isInteger() || (made.integers.maximum < maxExactInteger && made.integers.minimum > -maxExactInteger))
            continue;
        if (chooseArithmeticForm(program(), m_types, step.opcode(), OperatorTypes { first, second }, result) != OperatorForm::CheckedIntegers)
            continue;
        for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
            ValueID stepped = index.valueBehindCasts(step.operand(operandIndex));
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
        auto list = [&](ValueID used, UseInLoops::Kind kind, BlockID block, uint32_t user, uint32_t place) {
            if (used)
                listed.uses.append({ kind, block, user, place, std::exchange(listed.newestUseOf[used.index()], static_cast<uint32_t>(listed.uses.size())) });
        };
        forEachBlockInLoops([&](BodyBlock block) {
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
            TypeID result = typeInRow(operation.id());
            if (opcodesThatStateTypes.contains(opcode) || (steps.contains(opcode) && result && m_types.at(result).isNumber()))
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

ValueID Walker::headParameterBehind(Reference value) const
{
    if (!value || !value.isValue())
        return { };
    ValueID id = value.id();
    while (conversionsOfForm.contains(m_code.at(id).opcode))
        id = m_code.at(id).operand(0);
    const Operation& record = m_code.at(id);
    return record.opcode == Opcode::Parameter && record.block() && m_code.at(record.block()).isLoopHead ? id : ValueID();
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
        code.forEachEdgeIntoSoFar(block, [&](const Edge& edge) {
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

void Walker::noteHeadParametersThatEnter(TryOfLoop& tried, std::span<const Reference> entering) const
{
    if (m_triesOfLoops.size() < 2)
        return;
    const TryOfLoop* around = &m_triesOfLoops[m_triesOfLoops.size() - 2];
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
                const TryOfLoop* taken = !headParameter && source && m_code.at(source).opcode == Opcode::Parameter && m_code.at(source).block() && m_code.at(m_code.at(source).block()).isLoopHead ? tryOfHeadParameter(source, parameterAround) : nullptr;
                if (taken) {
                    headParameter = source;
                    explained = parameterAround < taken->cameRoundBesideItself.size() && taken->cameRoundBesideItself[parameterAround] ? m_types.unionOf(type, taken->cameRoundBesideItself[parameterAround]) : type;
                } else if (!source || source != headParameter)
                    beside = beside ? m_types.unionOf(beside, type) : type;
            });
            if (!headParameter || !m_types.isSubset(ownTypeOf(enters), beside ? m_types.unionOf(explained, beside) : explained))
                headParameter = { };
        }
        tried.headParametersThatEnter.append(headParameter);
        tried.enteredBesideHeadParameters.append(headParameter ? beside : TypeID());
    }
}

void Walker::handUpWhatCameRoundBesideParameters(const TryOfLoop& tried)
{
    BlockID head = m_frames[tried.frameIndex].blocks[tried.inputHead.index()].head;
    for (size_t parameter = 0; parameter < tried.headParametersThatEnter.size(); ++parameter) {
        if (!tried.headParametersThatEnter[parameter])
            continue;
        if (head && parameter < m_code.at(head).parameters().size())
            m_whatMeetsInParameters.set(m_code.at(head).parameters()[parameter].index(), WhatMeetsInParameter { tried.headParametersThatEnter[parameter], tried.enteredBesideHeadParameters[parameter], 0 });
        TypeID beside = parameter < tried.cameRoundBesideItself.size() ? tried.cameRoundBesideItself[parameter] : TypeID();
        size_t parameterAround = 0;
        const TryOfLoop* taken = beside ? tryOfHeadParameter(tried.headParametersThatEnter[parameter], parameterAround) : nullptr;
        if (!taken)
            continue;
        Vector<TypeID, 8>& all = m_triesOfLoops[taken - m_triesOfLoops.begin()].cameRoundBesideItself;
        if (all.size() <= parameterAround)
            all.grow(parameterAround + 1);
        all[parameterAround] = all[parameterAround] ? m_types.unionOf(all[parameterAround], beside) : beside;
    }
}

void Walker::noteSourcesOfHeadParameters(size_t frameIndex, BlockID inputHead, uint32_t answer, const TryOfLoop& tried, std::span<const Reference> entering)
{
    BlockID head = inputBlock(frameIndex, inputHead).head;
    if (!head || !m_code.at(head).isLoopHead)
        return;
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    for (size_t parameter = 0; parameter < parameters.size() && parameter < entering.size() && parameter < tried.entered.size(); ++parameter) {
        SourcesOfHeadParameter sources;
        sources.answer = answer;
        sources.parameter = static_cast<uint32_t>(parameter);
        if (parameter < tried.madeInTurns.size())
            sources.made = tried.madeInTurns[parameter];
        if (parameter < tried.headParametersThatCameRound.size())
            sources.headParametersHandedOn = tried.headParametersThatCameRound[parameter];
        if (ValueID around = headParameterBehind(entering[parameter]))
            sources.headParametersHandedOn.append(around);
        else if (tried.entered[parameter])
            sources.made = sources.made ? m_types.unionOf(sources.made, tried.entered[parameter]) : tried.entered[parameter];
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

void Walker::narrowRowsInsideTo(const TryOfLoop::NarrowingThatWaits& narrowing)
{
    std::span<const uint32_t> headParametersOfNest = m_headParametersWithSources.span().subspan(narrowing.firstHeadParameter, narrowing.endOfHeadParameters - narrowing.firstHeadParameter);
    Vector<TypeID, 16> typesThatReach = typesThatReachHeadParametersOfNest(narrowing.firstRecordOfNest, narrowing.firstHeadParameter, narrowing.endOfHeadParameters);
    for (uint32_t headParameter : headParametersOfNest) {
        const SourcesOfHeadParameter& sources = m_sourcesOfHeadParameters.find(headParameter)->value;
        if (sources.answer == narrowing.ownAnswer || sources.answer >= m_answersOfLoops.size())
            continue;
        AnswerOfLoop& inside = m_answersOfLoops[sources.answer];
        if (sources.parameter >= inside.types.size() || inside.claimsNothing)
            continue;
        TypeID reaches = typesThatReach[sources.place - narrowing.firstHeadParameter];
        TypeID held = inside.types[sources.parameter];
        if (reaches && held && m_types.at(held).isJavaScriptValue() && m_types.at(reaches).isJavaScriptValue() && m_types.isSubset(reaches, held))
            inside.types[sources.parameter] = reaches;
    }
}

void Walker::noteAllThatComesRoundInTry(BlockID head, size_t parameter, TypeID found, Reference value)
{
    for (size_t place = m_triesOfLoops.size(); place--;) {
        TryOfLoop& tried = m_triesOfLoops[place];
        if (tried.frameIndex >= m_frames.size() || m_frames[tried.frameIndex].blocks[tried.inputHead.index()].head != head)
            continue;
        if (tried.cameRound.size() <= parameter)
            tried.cameRound.grow(parameter + 1);
        tried.cameRound[parameter] = tried.cameRound[parameter] ? m_types.unionOf(tried.cameRound[parameter], found) : found;
        if (tried.valuesThatCameRound.size() <= parameter)
            tried.valuesThatCameRound.grow(parameter + 1);
        tried.valuesThatCameRound[parameter].append(value.isValue() ? value.id() : ValueID());
        if (((parameter < tried.headParametersThatEnter.size() && tried.headParametersThatEnter[parameter]) || startIsGuessedInWalk(tried, parameter)) && parameter < m_code.at(head).parameters().size()) {
            if (tried.cameRoundBesideItself.size() <= parameter)
                tried.cameRoundBesideItself.grow(parameter + 1);
            TypeID& all = tried.cameRoundBesideItself[parameter];
            if (TypeID beside = typeThatComesRoundBesideParameterItself(m_code.at(head).parameters()[parameter], value))
                all = all ? m_types.unionOf(all, beside) : beside;
            TypeID ofParameter = m_code.at(m_code.at(head).parameters()[parameter]).type;
            if (!m_types.isSubset(found, ofParameter) && !m_types.isSubset(found, all ? m_types.unionOf(all, ofParameter) : ofParameter))
                all = all ? m_types.unionOf(all, found) : found;
        }
        if (tried.madeInTurns.size() <= parameter) {
            tried.madeInTurns.grow(parameter + 1);
            tried.headParametersThatCameRound.grow(parameter + 1);
        }
        if (ValueID handedOn = headParameterBehind(value)) {
            if (!tried.headParametersThatCameRound[parameter].contains(handedOn))
                tried.headParametersThatCameRound[parameter].append(handedOn);
            return;
        }
        tried.madeInTurns[parameter] = tried.madeInTurns[parameter] ? m_types.unionOf(tried.madeInTurns[parameter], found) : found;
        if (value.isValue())
            noteOwnParametersThatMeetInValue(tried, parameter, value.id(), head);
        return;
    }
}

uint64_t Walker::findParametersThatElementsOutsideViewsWidened(const TryOfLoop& tried, const AnswerOfLoop& answer, BlockID head) const
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
    for (size_t parameter = 0; parameter < tried.valuesThatCameRound.size() && parameter < answer.types.size() && parameter < tried.entered.size() && parameter < 64; ++parameter) {
        TypeID held = answer.types[parameter];
        TypeID entered = tried.entered[parameter];
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
                isFollowed &= !isSummed && place < tried.valuesThatCameRound.size() && place < tried.entered.size() && tried.entered[place] && m_types.at(tried.entered[place]).isInteger();
                for (size_t value = 0; isFollowed && value < tried.valuesThatCameRound[place].size(); ++value)
                    add(into, tried.valuesThatCameRound[place][value]);
            } else if (record.opcode == Opcode::Parameter && id.index() >= tried.firstRecord && !m_code.at(record.block()).isLoopHead && !m_code.at(record.block()).isHandlerBlock()) {
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
        for (ValueID value : tried.valuesThatCameRound[parameter])
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

uint64_t Walker::findParametersThatRareWaysWidened(const TryOfLoop& tried, const AnswerOfLoop& answer, BlockID head)
{
    constexpr size_t mostValuesFollowed = 32;
    uint64_t widened = 0;
    std::span<const ValueID> own = head ? m_code.at(head).parameters() : std::span<const ValueID>();
    for (size_t parameter = 0; parameter < tried.valuesThatCameRound.size() && parameter < answer.types.size() && parameter < tried.entered.size() && parameter < 64; ++parameter) {
        TypeID held = answer.types[parameter];
        TypeID onLikelyWays = tried.entered[parameter];
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
        for (ValueID value : tried.valuesThatCameRound[parameter])
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
            if (record.opcode == Opcode::Parameter && id.index() >= tried.firstRecord && !m_code.at(block).isLoopHead && !m_code.at(block).isHandlerBlock()) {
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
                if (place >= tried.valuesThatCameRound.size() || place >= tried.entered.size() || !tried.entered[place] || !m_types.at(tried.entered[place]).isJavaScriptValue()) {
                    isFollowed = false;
                    break;
                }
                onLikelyWays = m_types.unionOf(onLikelyWays, tried.entered[place]);
                for (ValueID value : tried.valuesThatCameRound[place])
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
                count(StatisticID::ParametersThatAbsentElementsAloneWidened);
        }
    }
    return widened;
}

bool Walker::noteWhatCameRoundInTry(BlockID head, size_t parameter, TypeID assumed, TypeID found, size_t numberOfTriesAround, TypeID* enteredUnderGuessedStart)
{
    if (!m_types.at(assumed).isJavaScriptValue() || !m_types.at(found).isJavaScriptValue())
        return false;
    for (size_t place = std::min(numberOfTriesAround, m_triesOfLoops.size()); place--;) {
        TryOfLoop& tried = m_triesOfLoops[place];
        if (tried.frameIndex >= m_frames.size() || m_frames[tried.frameIndex].blocks[tried.inputHead.index()].head != head)
            continue;
        AnswerOfLoop& answer = m_answersOfLoops[tried.answer];
        if (parameter >= answer.types.size() || answer.claimsNothing)
            return false;
        uint64_t bit = parameter < 64 ? 1ull << parameter : 0;
        tried.misfits = true;
        if (tried.cameRoundBesideItself.size() <= parameter)
            tried.cameRoundBesideItself.grow(parameter + 1);
        TypeID& besideItself = tried.cameRoundBesideItself[parameter];
        if (enteredUnderGuessedStart || !besideItself)
            besideItself = besideItself ? m_types.unionOf(besideItself, found) : found;
        TypeID enteredAround = climbHeadAroundWith(tried, place, parameter, besideItself);
        bool isStruckNow = !(parameter < tried.startsStruck.size() && tried.startsStruck[parameter]);
        TypeID entered;
        if (!isStruckNow)
            entered = answer.enteredWhereStartWasStruck[parameter];
        else if (startIsGuessedInWalk(tried, parameter)) {
            entered = enteredAround ? enteredAround : parameter < tried.entered.size() ? tried.entered[parameter] : TypeID();
            if (enteredAround && tried.enteredBesideHeadParameters[parameter])
                entered = m_types.unionOf(entered, tried.enteredBesideHeadParameters[parameter]);
        }
        if (entered && m_types.at(entered).isJavaScriptValue()) {
            if (enteredUnderGuessedStart)
                *enteredUnderGuessedStart = entered;
            if (isStruckNow) {
                answer.startIsWiderThanWhatEnters[parameter] = false;
                if (answer.enteredWhereStartWasStruck.size() <= parameter)
                    answer.enteredWhereStartWasStruck.grow(parameter + 1);
                answer.enteredWhereStartWasStruck[parameter] = entered;
                while (tried.startsStruck.size() <= parameter)
                    tried.startsStruck.append(false);
                tried.startsStruck[parameter] = true;
                answer.rungs[parameter] = 0;
            }
            answer.types[parameter] = climbLadderOfLoop(answer, parameter, entered, besideItself, isStruckNow);
            tried.parametersThatClimbed |= bit;
            return true;
        }
        TypeID climbed = climbLadderOfLoop(answer, parameter, assumed, found, !(tried.parametersThatClimbed & bit));
        tried.parametersThatClimbed |= bit;
        answer.types[parameter] = m_types.unionOf(answer.types[parameter], climbed);
        return true;
    }
    return false;
}

void Walker::noteOwnParametersThatMeetInValue(TryOfLoop& tried, size_t parameter, ValueID value, BlockID head)
{
    constexpr OpcodeSet arithmeticWhoseKindFollowsItsOperands { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Increment, Opcode::Decrement };
    auto behindConversions = [&](ValueID id) {
        while (conversionsOfForm.contains(m_code.at(id).opcode))
            id = m_code.at(id).operand(0);
        return id;
    };
    auto forEachValueThatMakes = [&](ValueID id, const auto& functor) {
        const Operation& record = m_code.at(id);
        if (id.index() < tried.firstRecord)
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
    tried.recordsThatMakeWhatCameRound.append({ began.index(), parameters[parameter].index() });
    ValuesReached reached;
    reached.add(began);
    size_t numberOfSteps = 0;
    for (size_t next = 0; next < reached.inOrder.size(); ++next) {
        ValueID made = reached.inOrder[next];
        if (tried.recordsWhoseMakersAreNoted.contains(made.index()))
            continue;
        forEachValueThatMakes(made, [&](ValueID maker) {
            ++numberOfSteps;
            tried.recordsThatMakeWhatCameRound.append({ maker.index(), made.index() });
            reached.add(maker);
        });
    }
    if (numberOfSteps <= ValuesReached::numberSearchedWithoutSet)
        return;
    for (ValueID made : reached.inOrder)
        tried.recordsWhoseMakersAreNoted.add(made.index());
}

const Walker::TryOfLoop* Walker::tryOfHeadParameter(ValueID headParameter, size_t& parameter) const
{
    BlockID head = m_code.at(headParameter).block();
    for (size_t place = m_triesOfLoops.size(); place--;) {
        const TryOfLoop& tried = m_triesOfLoops[place];
        if (tried.frameIndex >= m_frames.size() || m_frames[tried.frameIndex].blocks[tried.inputHead.index()].head != head)
            continue;
        std::span<const ValueID> parameters = m_code.at(head).parameters();
        parameter = std::ranges::lower_bound(parameters, headParameter.index(), { }, [](ValueID each) { return each.index(); }) - parameters.begin();
        return parameter < parameters.size() && parameters[parameter] == headParameter ? &tried : nullptr;
    }
    return nullptr;
}

bool Walker::startIsGuessedInWalk(const TryOfLoop& tried, size_t parameter) const
{
    const Vector<bool, 8>& isWider = m_answersOfLoops[tried.answer].startIsWiderThanWhatEnters;
    return (parameter < isWider.size() && isWider[parameter]) || (parameter < tried.startsStruck.size() && tried.startsStruck[parameter]);
}

TypeID Walker::climbHeadAroundWith(const TryOfLoop& tried, size_t numberOfTriesAround, size_t parameter, TypeID found)
{
    ValueID around = parameter < tried.headParametersThatEnter.size() ? tried.headParametersThatEnter[parameter] : ValueID();
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
    return std::ranges::any_of(m_triesOfLoops, [](const TryOfLoop& around) {
        return around.misfits || around.freshObjectHasLeft;
    });
}

void Walker::letParametersHoldWhatTheyTakeRound(AnswerOfLoop& answer, const TryOfLoop& tried, BlockID head)
{
    if (!head || answer.claimsNothing)
        return;
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    auto placeOf = [&](uint32_t record) {
        size_t place = std::ranges::lower_bound(parameters, record, { }, [](ValueID each) { return each.index(); }) - parameters.begin();
        return place < parameters.size() && parameters[place].index() == record ? place : notFound;
    };
    auto stopsTheFlow = [&](uint32_t record) {
        size_t place = placeOf(record);
        return place != notFound && (place >= answer.types.size() || !answer.types[place] || !m_types.at(answer.types[place]).isJavaScriptValue());
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
    for (RecordThatMakes step : tried.recordsThatMakeWhatCameRound)
        lay(step.maker, step.made);
    for (uint32_t taker = 0; taker < tried.headParametersThatCameRound.size() && taker < parameters.size(); ++taker) {
        for (ValueID handedOn : tried.headParametersThatCameRound[taker])
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
        return place != notFound ? answer.types[place] : TypeID();
    }, [&](uint32_t record, TypeID held) {
        size_t place = placeOf(records[record]);
        if (place != notFound && !m_types.isSubset(held, answer.types[place]))
            answer.types[place] = m_types.unionOf(answer.types[place], held);
    });
}

Walker::KeyOfRowOfLoop Walker::keyOfCopyOfLoop(size_t frameIndex, BlockID inputHead) const
{
    uint64_t chainOfCalls = m_ordinalOfCopyOfLoop;
    std::span<const InlineFrame> inlineFrames = m_code.inlineFrames();
    for (uint32_t inlineFrame = m_frames[frameIndex].inlineFrame; inlineFrame; inlineFrame = inlineFrames[inlineFrame].parent)
        chainOfCalls = (chainOfCalls ^ (static_cast<uint64_t>(inlineFrames[inlineFrame].callee.index()) << 32 | inlineFrames[inlineFrame].callPosition)) * 0x9e3779b97f4a7c15ull + 1;
    return { static_cast<uint64_t>(m_frames[frameIndex].function.index()) << 32 | inputHead.index(), chainOfCalls };
}

Walker::KeyOfRowOfLoop Walker::keyOfRowOfLoop(KeyOfRowOfLoop copyOfLoop, uint32_t numberOfEntriesBefore)
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

bool Walker::tryLoop(BlockID inputHead, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (isBeingTried(frameIndex, inputHead) || m_way != WayOfWalking::AsBuilt)
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

    KeyOfRowOfLoop copyOfLoop = keyOfCopyOfLoop(frameIndex, inputHead);
    uint32_t numberOfEntriesBefore = m_entriesOfCopiesOfLoops.add(copyOfLoop, 0).iterator->value++;
    if (m_numberOfMarksOfTries)
        m_entriesOfCopiesOfLoopsInTries.append(copyOfLoop);
    KeyOfRowOfLoop key = keyOfRowOfLoop(copyOfLoop, numberOfEntriesBefore);
    uint32_t place = m_placesOfAnswersOfLoops.add(key, static_cast<uint32_t>(m_answersOfLoops.size())).iterator->value;
    if (place == m_answersOfLoops.size()) {
        moveWhereSanitizerWatches(m_answersOfLoops);
        m_answersOfLoops.append(AnswerOfLoop { });
        if (m_effectsSeededForNextRow) {
            m_answersOfLoops.last().effects = *m_effectsSeededForNextRow;
            m_answersOfLoops.last().knowsEffects = true;
        }
    } else if (m_effectsSeededForNextRow && m_otherCodeStaysInTypedBuildOfRow)
        m_answersOfLoops[place].effects = *m_effectsSeededForNextRow;
    m_effectsSeededForNextRow = std::nullopt;
    if (loopCarriesValuesOfConsumerRound(inputHead) && !m_answersOfLoops[place].mentionsThatLeave.contains(everyFreshObject))
        m_answersOfLoops[place].mentionsThatLeave.append(everyFreshObject);
    bool hasUndoneWhatFits = false;
    bool hasAskedForTwoBuilds = false;
    SetForScope sumsAreNotAsked(m_sumsAreNotAskedInRow, m_rowsWhoseSumsAreNotAsked.contains(key));

    Vector<Reference, 8> enteringKept;
    enteringKept.append(entering);
    readListsOfStableBindingsInFrontOfLoop(*this, inputHead);
    askWhatIsExpectedOfParametersThatEnterLoop(*this, inputHead);
    if (!m_numberOfMarksOfTries++)
        m_code.keepChangesToOlderParts(true);
    uint32_t serialOfMarkAround = m_serialOfNewestMark;
    setSerialOfNewestMark(++m_numberOfMarksTaken);
    auto forgetMark = makeScopeExit([&] {
        setSerialOfNewestMark(serialOfMarkAround);
        if (--m_numberOfMarksOfTries)
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
        m_placesOfLoopsChangedInTries.shrink(0);
        m_sharedConstantsAddedInTries.shrink(0);
        m_copiesOfConstantsAddedInTries.shrink(0);
        m_inputBlocksInTries.shrink(0);
        m_expectationsInTries.shrink(0);
        if (!m_numberOfClosedArms)
            m_bindings.shrink(0);
    });
    auto putRowOfPurchaseAroundBack = makeScopeExit([this, isQuiet = m_rowSaysTurnsAreQuietOutsideArms, effects = m_effectsOutsideArmsOfRow, tests = m_testsSeenOfRow, firstRanInCopy = m_firstOtherCodeRanInCopyOfRow, callStays = m_someCallStaysInTurnOfRow, asksLengths = m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces, ran = m_operationsThatRanOtherCodeOfRow, guards = m_guardsOfStoresAskedOfRow]() mutable {
        m_rowSaysTurnsAreQuietOutsideArms = isQuiet;
        m_effectsOutsideArmsOfRow = effects;
        m_testsSeenOfRow = WTF::move(tests);
        m_firstOtherCodeRanInCopyOfRow = firstRanInCopy;
        m_someCallStaysInTurnOfRow = callStays;
        m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces = asksLengths;
        m_operationsThatRanOtherCodeOfRow = WTF::move(ran);
        m_guardsOfStoresAskedOfRow = WTF::move(guards);
    });
    letGoOfPendingBlocksThatHaveEnded();
    auto ownerOfSnapshot = makeUniqueWithoutFastMallocCheck<SnapshotForTry>(*this);
    SnapshotForTry& snapshot = *ownerOfSnapshot;
    IterationsSteppedAtHead steppedInFrontOfLoop = takeIterationsSteppedAt(inputHead);
    bool stepsIteration = steppedInFrontOfLoop.asTheyEntered.containsIf([](const auto& entered) { return entered.second.otherForm == IterationByIndex::OtherForm::None; });
    bool generatorIsWalkedInLoop = m_generatorsTakenByConsumers.containsIf([&](const GeneratorTakenByConsumer& taken) {
        return taken.inputHead == inputHead && taken.inlineFrame == m_frames[frameIndex].inlineFrame;
    });
    auto buildSeveralTimes = [&](const LoopPreview& learned) {
        return !generatorIsWalkedInLoop && mayBuildLoopSeveralTimes(inputHead) && (buildLoopForEachMember(inputHead, learned, enteringKept.span()) || buildLoopTwice(inputHead, learned, enteringKept.span()));
    };
    auto makeBuildsAsRowSays = [&](LoopPreview& learned, const Vector<TypeID, 8>& entered, uint32_t numberOfEdgesThatCameRound, const auto& standInFrontAgain) {
        std::pair<uint32_t, BlockID> bought { static_cast<uint32_t>(frameIndex), inputHead };
        if (std::ranges::count(m_loopsBeingBought, bought) >= 2)
            return false;
        m_loopsBeingBought.append(bought);
        auto isBoughtNoLonger = makeScopeExit([&] {
            m_loopsBeingBought.removeLast();
        });
        auto prepareFrontOfBuilds = [&] {
            m_sumOfTypedBuildComesRoundUnproven = false;
            BodyBlock head = index.block(inputHead);
            for (uint32_t parameter = 0; parameter < head.numberOfParameters() && parameter < enteringKept.size(); ++parameter)
                m_frames[frameIndex].references.placeOf<NamesOfValues::Writer::PreparesFrontOfBuilds>(head.parameter(parameter).index()) = enteringKept[parameter];
            for (Reference argument : enteringKept)
                letLeave(m_state, argument);
            Vector<FreshObjectKeptAtHead, 2> keptInFrontOfBuilds;
            letMentionsOfLoopLeave(inputHead, m_answersOfLoops[place].mentionsThatLeave.span(), &keptInFrontOfBuilds, m_answersOfLoops[place].storesIntoScopes.areKnown);
            handScopesKeptAtHeadToPreview(m_answersOfLoops[place].storesIntoScopes, keptInFrontOfBuilds.span(), learned);
        };
        prepareFrontOfBuilds();
        bool isBuilt;
        {
            SetForScope headHandedOn(m_headOfRowHandedOn, inputHead);
            SetForScope mentionsHandedOn(m_mentionsThatLeaveHandedOn, m_answersOfLoops[place].mentionsThatLeave);
            SetForScope storesHandedOn(m_storesIntoScopesHandedOn, m_answersOfLoops[place].storesIntoScopes);
            SetForScope frameHandedOn(m_frameOfRowHandedOn, frameIndex);
            SetForScope enteredHandedOn(m_enteredOfRowHandedOn, entered);
            SetForScope typesHandedOn(m_typesOfRowHandedOn, m_answersOfLoops[place].types);
            SetForScope rungsHandedOn(m_rungsOfRowHandedOn, m_answersOfLoops[place].rungs);
            SetForScope widenedHandedOn(m_parametersThatRareWaysWidenedHandedOn, m_answersOfLoops[place].parametersThatRareWaysWidened);
            SetForScope widenedByElementsHandedOn(m_parametersThatElementsOutsideViewsWidenedHandedOn, m_answersOfLoops[place].parametersThatElementsOutsideViewsWidened);
            SetForScope buys(m_buysTwoBuilds, true);
            SetForScope listsStruck(m_listsAreStruckInRow, m_answersOfLoops[place].listsAreStruck);
            SetForScope otherCodeStays(m_otherCodeStaysInTypedBuildOfRow, m_answersOfLoops[place].otherCodeStaysInTypedBuild);
            SetForScope keptOutInVain(m_otherCodeIsKeptOutOfFirstCopyInVainOfRow, m_answersOfLoops[place].otherCodeIsKeptOutOfFirstCopyInVain);
            SetForScope readsAskSites(m_readsAskSitesThoughCallStaysOfRow, m_answersOfLoops[place].readsAskSitesThoughCallStays);
            SetForScope readWasKeptFromItsSite(m_readWasKeptFromItsSiteBecauseCallStays, false);
            m_typedBuildIsLeftAtOtherCodeOnUsualWay = false;
            m_everyTurnOfTypedBuildLeaves = false;
            bool turnAroundLeavesWithoutTest = std::exchange(m_turnOfTypedBuildLeavesWithoutTest, false);
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
                m_answersOfLoops[place].listsAreStruck = true;
                m_listsAreStruckInRow = true;
                isBuilt = buildSeveralTimes(learned);
            }
            auto isKept = [&](bool built) {
                bool leavesWithoutTest = std::exchange(m_turnOfTypedBuildLeavesWithoutTest, turnAroundLeavesWithoutTest);
                leavesWithoutTest &= std::exchange(m_typedBuildTestsNothing, false) || stepsIteration;
                if ((std::exchange(m_everyTurnOfTypedBuildLeaves, false) || leavesWithoutTest) && numberOfEdgesThatCameRound)
                    built = false;
                if (std::exchange(m_typedBuildAsksGuardsThatItWasBoughtFor, false))
                    built = false;
                return !std::exchange(m_typedBuildAskedNoListThatItWasBoughtFor, false) && built;
            };
            isBuilt = isKept(isBuilt);
            if (!isBuilt && m_readWasKeptFromItsSiteBecauseCallStays && !m_hasFailed && !m_numberOfTryWhoseWalkIsCutShort) {
                standInFrontAgain();
                prepareFrontOfBuilds();
                m_readsAskSitesThoughCallStaysOfRow = true;
                m_typedBuildIsLeftAtOtherCodeOnUsualWay = false;
                m_turnOfTypedBuildLeavesWithoutTest = false;
                isBuilt = buildSeveralTimes(learned);
                isBuilt = isKept(!std::exchange(m_typedBuildEndedListsTestedInFront, false) && isBuilt);
                m_readsAskSitesThoughCallStaysOfRow = isBuilt;
                m_answersOfLoops[place].readsAskSitesThoughCallStays = isBuilt;
            }
            if (std::exchange(m_typedBuildIsLeftAtOtherCodeOnUsualWay, false) && isBuilt && !m_otherCodeStaysInTypedBuildOfRow) {
                for (bool otherCodeStays : { true, false }) {
                    if (m_hasFailed || m_numberOfTryWhoseWalkIsCutShort)
                        break;
                    standInFrontAgain();
                    prepareFrontOfBuilds();
                    m_answersOfLoops[place].otherCodeStaysInTypedBuild = otherCodeStays;
                    m_otherCodeStaysInTypedBuildOfRow = otherCodeStays;
                    m_answersOfLoops[place].otherCodeIsKeptOutOfFirstCopyInVain = m_otherCodeIsKeptOutOfFirstCopyInVainOfRow;
                    m_turnOfTypedBuildLeavesWithoutTest = false;
                    isBuilt = buildSeveralTimes(learned);
                    isBuilt = isKept(!std::exchange(m_typedBuildEndedListsTestedInFront, false) && isBuilt);
                    if (isBuilt)
                        break;
                }
                m_typedBuildIsLeftAtOtherCodeOnUsualWay = false;
            }
            AnswerOfLoop& row = m_answersOfLoops[place];
            if (m_mentionsThatLeaveHandedOn.size() != row.mentionsThatLeave.size() || m_storesIntoScopesHandedOn.fields.size() != row.storesIntoScopes.fields.size() || m_storesIntoScopesHandedOn.areKnown != row.storesIntoScopes.areKnown) {
                row.mentionsThatLeave = m_mentionsThatLeaveHandedOn;
                row.storesIntoScopes.fields = m_storesIntoScopesHandedOn.fields;
                isBuilt = false;
            }
        }
        if (!m_hasFailed || m_numberOfTryWhoseWalkIsCutShort)
            return isBuilt;
        ++m_numberOfPurchasesOfLoopsThatFailed;
        return isBuilt && m_inputs.options.verifiesCode;
    };
    auto makeBuilds = [&](LoopPreview& learned, const Vector<TypeID, 8>& entered, uint32_t numberOfEdgesThatCameRound, const auto& standInFrontAgain) {
        size_t numberOfRowsInFront = m_answersOfLoops.size();
        bool listsAreStruck = m_answersOfLoops[place].listsAreStruck;
        bool otherCodeStays = m_answersOfLoops[place].otherCodeStaysInTypedBuild;
        bool readsAskSites = m_answersOfLoops[place].readsAskSitesThoughCallStays;
        bool comesRoundUnprovenAround = m_sumOfTypedBuildComesRoundUnproven;
        bool isBuilt = makeBuildsAsRowSays(learned, entered, numberOfEdgesThatCameRound, standInFrontAgain);
        if (!std::exchange(m_sumOfTypedBuildComesRoundUnproven, comesRoundUnprovenAround) || !isBuilt || m_sumsAreNotAskedInRow || m_hasFailed || m_numberOfTryWhoseWalkIsCutShort)
            return isBuilt;
        m_answersOfLoops.shrink(numberOfRowsInFront);
        m_placesOfAnswersOfLoops.removeIf([&](const auto& row) {
            return row.value >= numberOfRowsInFront;
        });
        m_answersOfLoops[place].listsAreStruck = listsAreStruck;
        m_answersOfLoops[place].otherCodeStaysInTypedBuild = otherCodeStays;
        m_answersOfLoops[place].readsAskSitesThoughCallStays = readsAskSites;
        m_rowsWhoseSumsAreNotAsked.add(key, 0);
        m_sumsAreNotAskedInRow = true;
        return false;
    };
    auto undoPlainWalk = [&] {
        snapshot.putBack(*this);
        m_frontOfLoopLeftOpen = { };
    };
    auto makeTypedBuildBesidePlainWalk = [&](LoopPreview& learned, const TryOfLoop& tried) {
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
        letGoOfPendingBlocksThatHaveEnded();
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
                if (written.frame >= m_frames.size() || m_frames[written.frame].expectations.isEmpty() || m_inputs.options.undoLeavesExpectations)
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
            m_state = *tried.stateInFront;
            m_closedCopiesShared.append(ClosedCopyShared { frameIndex, inputHead, start, entry, true, true });
            m_buildsTestsBesidePlainWalk = true;
        };
        RELEASE_ASSERT(!m_code.at(front.head).isSealed);
        m_headsOfClosedLoops.remove(front.head.index());
        m_frontsSharedWithTypedBuilds.append(FrontSharedWithTypedBuild { frameIndex, inputHead, front.builtInFront });
        standBesidePlainWalk();
        m_reportOfTypedBuildMadeLast = std::nullopt;
        bool isBuilt = makeBuilds(learned, tried.entered, tried.numberOfEdgesThatCameRound, [&] {
            undoTypedBuild();
            standBesidePlainWalk();
            m_reportOfTypedBuildMadeLast = std::nullopt;
        });
        std::optional<ReportOfTypedBuild> report = std::exchange(m_reportOfTypedBuildMadeLast, std::nullopt);
        m_buildsTestsBesidePlainWalk = false;
        m_headsOfClosedLoops.add(front.head.index());
        m_frontsSharedWithTypedBuilds.removeLast();
        if (m_numberOfTryWhoseWalkIsCutShort)
            return true;
        if (isBuilt && !m_hasFailed && !report)
            isBuilt = false;
        if (isBuilt && !m_hasFailed) {
            const Effects& ofTypedTurns = report->effects;
            isBuilt = tried.assumed.mayRunOtherCode()
                ? !ofTypedTurns.maySuspend || tried.assumed.maySuspend
                : !ofTypedTurns.mayRunOtherCode() && ofTypedTurns.writes.without(heapsNotHeldAgainstRowsOfLoops).isSubsetOf(tried.assumed.writes);
        }
        if (!isBuilt) {
            undoTypedBuild();
            m_block = { };
            m_frontOfLoopLeftOpen = WTF::move(front);
            return false;
        }
        if (report && report->startOfOtherCopyIsRare)
            m_code.makeBlockRare(startOfPlainWalk);
        if (report && !report->otherCopyOpensItsGuesses) {
            m_code.makeBlocksRare(front.head.index(), behindPlainWalk.code.numberOfBlocks);
            m_code.makeBlockRare(entryOfPlainWalk);
            for (uint32_t place : edgesThatLeavePlainWalk)
                m_openEdges[place].isLedByClosedArm = true;
        }
        return true;
    };
    if (m_answersOfLoops[place].bought && !m_answersOfLoops[place].bought->keepsPlainWalk && has(Mechanism::LoopInvariants)) {
        std::shared_ptr<const BuildsBought> bought = m_answersOfLoops[place].bought;
        bool sharedCopyWaits = !m_closedCopiesShared.isEmpty() && m_closedCopiesShared.last().frameIndex == frameIndex && m_closedCopiesShared.last().inputHead == inputHead && m_closedCopiesShared.last().isAboutToBeTaken;
        bool sameTypesEnter = bought->entered.size() == enteringKept.size() && (sharedCopyWaits || !m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop));
        for (size_t parameter = 0; sameTypesEnter && parameter < enteringKept.size(); ++parameter)
            sameTypesEnter = ownTypeOf(enteringKept[parameter]) == bought->entered[parameter];
        if (sameTypesEnter) {
            m_rowSaysTurnsAreQuietOutsideArms = bought->isQuietOutsideArms;
            m_effectsOutsideArmsOfRow = bought->effectsOfQuietDrafts;
            m_testsSeenOfRow = bought->tests;
            m_firstOtherCodeRanInCopyOfRow = bought->firstOtherCodeRanInCopy;
            m_someCallStaysInTurnOfRow = bought->someCallStaysInTurn;
            m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces = bought->asksLengthsOfViewsFromInFrontForWrittenPlaces;
            m_operationsThatRanOtherCodeOfRow = bought->operationsThatRanOtherCode;
            m_guardsOfStoresAskedOfRow = bought->guardsOfStoresAsked;
            auto ownerOfLearned = makeUniqueWithoutFastMallocCheck<LoopPreview>();
            LoopPreview& learned = *ownerOfLearned;
            learned.effects = bought->effects;
            learned.typesOfParameters = bought->typesOfParameters;
            learned.typesOfValues = bought->typesOfValues;
            learned.firstValue = bought->firstValue;
            learned.hasLetMentionsLeave = true;
            Vector<TypeID, 8> typesOfPlainWalk = m_answersOfLoops[place].types;
            m_answersOfLoops[place].types = bought->typesOfParameters;
            bool isBuilt = makeBuilds(learned, bought->entered, bought->numberOfEdgesThatCameRound, undoPlainWalk);
            m_answersOfLoops[place].types = typesOfPlainWalk;
            if (isBuilt || m_numberOfTryWhoseWalkIsCutShort)
                return true;
            snapshot.putBack(*this);
        }
        m_answersOfLoops[place].bought = nullptr;
    }
    bool mayBeBuiltSeveralTimes = !generatorIsWalkedInLoop && has(Mechanism::LoopInvariants) && !m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop) && mayBuildLoopSeveralTimes(inputHead);
    for (;;) {
        bool undoesWhatFits = m_inputs.options.walksLoopsThatFitAgain && !m_rowsWalkedAgainForCheck.contains(key);
        Vector<AnswerOfLoop> answersBefore;
        decltype(m_placesOfAnswersOfLoops) placesOfAnswersBefore;
        if (undoesWhatFits && !hasUndoneWhatFits) {
            answersBefore = m_answersOfLoops;
            placesOfAnswersBefore = m_placesOfAnswersOfLoops;
        }
        bool isWalkedInBuild = false;
        bool walksKeptWalkAlone = false;
        if (m_inputs.options.walksLastWalksOfLoopsAlone) [[unlikely]] {
            isWalkedInBuild = m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop);
            walksKeptWalkAlone = layRowBackForLastWalkAlone(place, isWalkedInBuild);
        }
        bool assumedTop = !m_answersOfLoops[place].knowsEffects && !m_answersOfLoops[place].claimsNothing;
        moveWhereSanitizerWatches(m_triesOfLoops);
        m_triesOfLoops.append(TryOfLoop { frameIndex, inputHead, place, Effects::none(), false });
        m_frames[frameIndex].loopsBeingTried.set(loop.index());
        if (frameIndex < m_operationsBeingWalkedInFrames.size())
            m_operationsBeingWalkedInFrames[frameIndex] = { };
        m_triesOfLoops.last().firstRecord = m_code.numberOfValues();
        m_triesOfLoops.last().numberOfRecordsAtHead = m_code.numberOfValues();
        m_triesOfLoops.last().numberOfHeadParametersWithSourcesInFront = m_headParametersWithSources.size();
        noteHeadParametersThatEnter(m_triesOfLoops.last(), enteringKept.span());
        ++m_numberOfWalksOfLoops;
        bool mayBeBoughtBehindWalk = !hasAskedForTwoBuilds && !m_answersOfLoops[place].severalBuildsAreStruck && !m_answersOfLoops[place].claimsNothing && mayBeBuiltSeveralTimes && !(m_answersOfLoops[place].whatTurnsShowed & wasWalkedAgainForFront);
        m_triesOfLoops.last().wantsStateInFront = mayBeBoughtBehindWalk;
        m_leavesFrontOfLoopOpenBehindWalk = mayBeBoughtBehindWalk;
        auto typesBeforeWalk = m_answersOfLoops[place].types;
        auto rungsBeforeWalk = m_answersOfLoops[place].rungs;
        uint32_t numberOfLoopsThatHoldLoopsBuiltTwiceBeforeWalk = m_numberOfLoopsThatHoldLoopsBuiltTwice;
        walkCopyOfLoop(inputHead, enteringKept.span());
        bool walkBeganTexts = m_walkOfLoopBeganTexts;
        auto ownerOfTried = makeUniqueWithoutFastMallocCheck<TryOfLoop>(m_triesOfLoops.takeLast());
        m_frames[frameIndex].loopsBeingTried.clear(loop.index());
        TryOfLoop& tried = *ownerOfTried;
        if (!tried.objectsMadeInFrontThatAreWritten.isEmpty()) {
            std::ranges::sort(tried.objectsMadeInFrontThatAreWritten);
            tried.objectsMadeInFrontThatAreWritten.shrink(std::ranges::unique(tried.objectsMadeInFrontThatAreWritten).begin() - tried.objectsMadeInFrontThatAreWritten.begin());
            ObjectsMadeHereThatLoopMentions mentioned = tried.othersAreSparedAtHead ? *tried.othersAreSparedAtHead : objectsMadeHereThatLoopMentions(inputHead);
            bool eachIsMentioned = std::ranges::all_of(tried.objectsMadeInFrontThatAreWritten, [&](uint32_t made) {
                return !tried.listsMadeHereThatAreSparedAtHead.contains(made) && std::ranges::binary_search(mentioned, made);
            });
            if (!eachIsMentioned) {
                tried.done.writesObjectsMadeHereAlone = false;
                tried.doneOutsideArms.writesObjectsMadeHereAlone = false;
                tried.doneByQuietDrafts.writesObjectsMadeHereAlone = false;
            }
        }
        if (!m_triesOfLoops.isEmpty()) {
            TryOfLoop& around = m_triesOfLoops.last();
            for (uint32_t made : tried.objectsMadeInFrontThatAreWritten) {
                if (made < around.numberOfRecordsAtHead && (around.objectsMadeInFrontThatAreWritten.isEmpty() || around.objectsMadeInFrontThatAreWritten.last() != made))
                    around.objectsMadeInFrontThatAreWritten.append(made);
            }
            around.done.merge(tried.done);
            around.writtenWithoutOtherCode.merge(tried.writtenWithoutOtherCode);
            around.doneOutsideArms.merge(tried.doneOutsideArms);
            around.doneByQuietDrafts.merge(tried.doneByQuietDrafts);
            handUpWhatCameRoundBesideParameters(tried);
        }
        bool isCutShort = false;
        if (m_numberOfTryWhoseWalkIsCutShort) {
            if (m_numberOfTryWhoseWalkIsCutShort <= m_triesOfLoops.size()) {
                AnswerOfLoop& passedBy = m_answersOfLoops[place];
                passedBy.effectsAreGuessed = false;
                passedBy.knowsEffects = true;
                passedBy.effects.merge(tried.done);
                bool maySuspend = passedBy.effects.maySuspend;
                passedBy.effects.merge(Effects::unknownCode());
                passedBy.effects.maySuspend = maySuspend;
                return true;
            }
            m_numberOfTryWhoseWalkIsCutShort = 0;
            m_hasFailed = false;
            isCutShort = true;
            ++m_numberOfWalksOfLoopsCutShort;
        }
        const Effects& assumed = tried.assumed;
        AnswerOfLoop& answer = m_answersOfLoops[place];
        if (tried.misfits && tried.somethingIsWantedInside) {
            answer.types = WTF::move(typesBeforeWalk);
            answer.rungs = WTF::move(rungsBeforeWalk);
            for (size_t parameter = 0; parameter < tried.startsStruck.size() && parameter < answer.types.size(); ++parameter) {
                if (!tried.startsStruck[parameter])
                    continue;
                answer.types[parameter] = answer.enteredWhereStartWasStruck[parameter];
                answer.rungs[parameter] = 0;
            }
        } else if (tried.misfits)
            letParametersHoldWhatTheyTakeRound(answer, tried, inputBlock(frameIndex, inputHead).head);
        answer.narrowerStartHasMisfitted |= tried.misfits && tried.startIsNarrowerThanRowHeld;
        auto defersPurchase = [&] {
            return !m_triesOfLoops.isEmpty() && index.loop(loop).numberOfLoopsWithin > 1 && !m_answersOfLoops[m_triesOfLoops.last().answer].hasFittedOnce;
        };
        ++answer.numberOfTries;

        bool effectsFit = assumed.mayRunOtherCode()
            ? !tried.done.maySuspend || assumed.maySuspend
            : !tried.done.mayRunOtherCode() && tried.done.writes.without(heapsNotHeldAgainstRowsOfLoops).isSubsetOf(assumed.writes)
                && (!assumed.writesObjectsMadeHereAlone || tried.done.writesObjectsMadeHereAlone || !tried.done.writes.containsAny(perObjectHeaps));
        bool effectsWereGuessed = std::exchange(answer.effectsAreGuessed, false);
        if (effectsWereGuessed && effectsFit && !isCutShort)
            answer.effects = tried.done;
        else
            answer.effects.merge(tried.done);
        answer.knowsEffects = true;
        if ((effectsWereGuessed || (assumedTop && assumed.mayRunOtherCode())) && effectsFit && !tried.done.mayRunOtherCode() && !answer.claimsNothing && index.loop(loop).numberOfLoopsWithin <= 1) {
            if (defersPurchase())
                m_triesOfLoops.last().somethingIsWantedInside = true;
            else
                effectsFit = false;
        }
        if (!assumedTop && effectsFit && !tried.misfits && assumed.mayRunOtherCode() && !tried.done.mayRunOtherCode() && !answer.claimsNothing && !answer.wasFoundQuietUnderOtherCode && index.loop(loop).numberOfLoopsWithin <= 1) {
            answer.wasFoundQuietUnderOtherCode = true;
            answer.effects = tried.done;
            if (defersPurchase())
                m_triesOfLoops.last().somethingIsWantedInside = true;
            else
                effectsFit = false;
        }
        if (answer.effects.mayRunOtherCode()) {
            bool maySuspend = answer.effects.maySuspend;
            answer.effects.merge(Effects::unknownCode());
            answer.effects.maySuspend = maySuspend;
        }
        if (isCutShort && m_headOfTypedBuildOverListsTestedInFront == inputHead && m_frameOfTypedBuildOverListsTestedInFront == frameIndex) {
            m_typedBuildEndedListsTestedInFront = true;
            snapshot.putBack(*this);
            m_block = { };
            return true;
        }
        bool hasFailedInside = m_hasFailed;
        if (!tried.freshObjectsKept.isEmpty() && !tried.done.mayRunOtherCode() && tried.done.writes.containsAny(AbstractHeapSet { AbstractHeap::TypedArrayElements, AbstractHeap::TypedArrayStorage })) {
            for (const FreshObjectKeptAtHead& kept : tried.freshObjectsKept) {
                if (isScopeThatUnitMakes(kept.object))
                    continue;
                if (!answer.mentionsThatLeave.contains(kept.mention))
                    answer.mentionsThatLeave.append(kept.mention);
                tried.freshObjectHasLeft = true;
            }
        }
        bool scopesWantAnotherWalk = learnWhatTurnsStoreIntoScopes(answer, tried, !hasFailedInside && !isCutShort);
        bool fits = effectsFit && !tried.misfits && !tried.freshObjectHasLeft && !hasFailedInside;
        if (!effectsFit && !tried.misfits && !tried.freshObjectHasLeft && !hasFailedInside && !answer.claimsNothing && !isCutShort)
            narrowRowToWhatCameRound(answer, tried);
        if (!hasFailedInside && !answer.claimsNothing && !isCutShort) {
            uint32_t firstValue = index.block(inputHead).firstParameter().index();
            answer.firstValue = firstValue;
            answer.typesOfValues.clear();
            auto appendRunOfBlocks = [&](BlockID first, BlockID last) {
                answer.typesOfValues.appendRun(index.block(first).firstParameter().index() - firstValue, index.block(last).endOfValues().index() - index.block(first).firstParameter().index());
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
            answer.typesOfValues.makeRoomForRuns();
            answer.numberOfRecordsWhenTypesOfValuesWereNoted = m_code.numberOfValues();
            auto noteTypesOfValuesOf = [&](BlockID member) {
                BodyBlock block = index.block(member);
                TypeID* typesOfBlock = answer.typesOfValues.slotAt(block.firstParameter().index() - firstValue);
                for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value) {
                    TypeID& typeInRow = typesOfBlock[value - block.firstParameter().index()];
                    typeInRow = ownTypeOf(m_frames[frameIndex].references[value]);
                    if (Reference bound = m_frames[frameIndex].references[value]; bound && isTextInStringBuilder(*this, bound))
                        typeInRow = TypeTable::string;
                }
            };
            auto noteTypesLeftByTurnsWrittenOut = [&] {
                for (const Vector<TryOfLoop::TypeLeftByTurnsWrittenOut>* list : { &tried.typesLeftByTurnsWrittenOut, &tried.typesOfSumsLeftInArms }) {
                    for (const TryOfLoop::TypeLeftByTurnsWrittenOut& left : *list) {
                        TypeID* typeInRow = left.value >= firstValue ? answer.typesOfValues.slotAt(left.value - firstValue) : nullptr;
                        if (typeInRow)
                            *typeInRow = *typeInRow ? m_types.unionOf(*typeInRow, left.type) : left.type;
                    }
                }
            };
            Vector<std::pair<LoopID, uint32_t>, 4> rowsTakenWhole;
            uint32_t numberOfLoopsLeft = index.loop(loop).numberOfLoopsWithin - 1;
            for (uint32_t inner = loop.index() + 1; numberOfLoopsLeft && inner < index.numberOfLoops(); ++inner) {
                BlockID innerHead = index.loop(LoopID(inner)).head;
                if (index.loop(LoopID(inner)).parent != loop)
                    continue;
                numberOfLoopsLeft -= index.loop(LoopID(inner)).numberOfLoopsWithin;
                KeyOfRowOfLoop copyOfInner = keyOfCopyOfLoop(frameIndex, innerHead);
                uint32_t numberOfEntriesOfInner = m_entriesOfCopiesOfLoops.get(copyOfInner);
                auto found = numberOfEntriesOfInner ? m_placesOfAnswersOfLoops.find(keyOfRowOfLoop(copyOfInner, numberOfEntriesOfInner - 1)) : m_placesOfAnswersOfLoops.end();
                bool isNotedInThisWalk = found != m_placesOfAnswersOfLoops.end() && m_answersOfLoops[found->value].numberOfRecordsWhenTypesOfValuesWereNoted > tried.firstRecord;
                if (!isNotedInThisWalk) {
                    for (BlockID member : index.membersOf(LoopID(inner)))
                        noteTypesOfValuesOf(member);
                }
                if (found == m_placesOfAnswersOfLoops.end())
                    continue;
                const AnswerOfLoop& ofInner = m_answersOfLoops[found->value];
                AnswerOfLoop& ofThis = m_answersOfLoops[place];
                ofInner.typesOfValues.forEachRun([&](uint32_t first, std::span<const TypeID> typesInside) {
                    TypeID* typesInRow = ofThis.typesOfValues.slotAt(ofInner.firstValue + first - firstValue);
                    RELEASE_ASSERT(typesInRow && ofThis.typesOfValues.slotAt(ofInner.firstValue + first - firstValue + typesInside.size() - 1) == typesInRow + (typesInside.size() - 1));
                    if (isNotedInThisWalk) {
                        std::ranges::copy(typesInside, typesInRow);
                        return;
                    }
                    for (size_t position = 0; position < typesInside.size(); ++position) {
                        if (!typesInRow[position])
                            typesInRow[position] = typesInside[position];
                    }
                });
                if (isNotedInThisWalk)
                    rowsTakenWhole.append({ LoopID(inner), found->value });
            }
            forEachBlockAndLoopRightIn(index, loop, [&](BlockID member) {
                if (index.loopOf(member) == loop)
                    noteTypesOfValuesOf(member);
            }, [&](LoopID) {
                return true;
            });
            noteTypesLeftByTurnsWrittenOut();
            for (size_t parameter = 0; parameter < answer.types.size() && parameter < index.block(inputHead).numberOfParameters(); ++parameter) {
                if (TypeID* typeInRow = answer.typesOfValues.slotAt(parameter); typeInRow && !*typeInRow)
                    *typeInRow = answer.types[parameter];
            }
            auto typeSumOfTexts = [&](ValueID value) {
                TypeID& typeInRow = *answer.typesOfValues.slotAt(value.index() - firstValue);
                if (typeInRow || index.isParameter(value) || index.opcodeOf(value) != Opcode::Add)
                    return;
                BodyValue sum = index.value(value);
                auto typeOfOperand = [&](ValueID operand) -> TypeID {
                    if (operand.index() >= firstValue && operand.index() - firstValue < answer.typesOfValues.size())
                        return answer.typesOfValues[operand.index() - firstValue];
                    return typeOfRecordForLooks(*this, ownTypeOf(m_frames[frameIndex].references[operand.index()]));
                };
                TypeID left = typeOfOperand(sum.operand(0));
                TypeID right = typeOfOperand(sum.operand(1));
                if (left && right && hasTextByItself(m_types.at(left)) && hasTextByItself(m_types.at(right)) && (m_types.at(left).isAnyString() || m_types.at(right).isAnyString()))
                    typeInRow = TypeTable::string;
                else
                    answer.typesOfValues.sumsWithoutType().append(value.index());
            };
            forEachBlockAndLoopRightIn(index, loop, [&](BlockID member) {
                for (uint32_t value = index.block(member).firstParameter().index(); value < index.block(member).endOfValues().index(); ++value)
                    typeSumOfTexts(ValueID(value));
            }, [&, next = size_t(0)](LoopID inside) mutable {
                while (next < rowsTakenWhole.size() && rowsTakenWhole[next].first.index() < inside.index())
                    ++next;
                if (next == rowsTakenWhole.size() || rowsTakenWhole[next].first != inside)
                    return false;
                for (uint32_t value : std::as_const(m_answersOfLoops[rowsTakenWhole[next].second].typesOfValues).sumsWithoutType())
                    typeSumOfTexts(ValueID(value));
                return true;
            });
            if (fits && !answer.hasAskedAboutTexts && mayKeepTextsInStringBuilders(*this) && !answer.effects.maySuspend) {
                answer.hasAskedAboutTexts = true;
                auto ownerOfLearned = makeUniqueWithoutFastMallocCheck<LoopPreview>();
                LoopPreview& learned = *ownerOfLearned;
                learned.effects = answer.effects;
                learned.typesOfParameters.appendVector(answer.types);
                learned.typesOfValues = answer.typesOfValues;
                learned.firstValue = firstValue;
                Vector<GrowingText, 2> texts;
                findGrowingTexts(*this, inputHead, learned, texts);
                if (!texts.isEmpty() && !someTextGrowsInQuietTurn(inputHead, texts.span(), tried))
                    keepTextsOfLoopOutOfBuilders(inputHead);
                if (!texts.isEmpty())
                    narrowRowToWhatCameRound(answer, tried);
                if (!texts.isEmpty() && defersPurchase())
                    m_triesOfLoops.last().somethingIsWantedInside = true;
                else if (!texts.isEmpty())
                    fits = false;
            }
        }
        if (!tried.misfits && !hasFailedInside && !answer.claimsNothing && !isCutShort) {
            noteSourcesOfHeadParameters(frameIndex, inputHead, place, tried, enteringKept.span());
            answer.enteredFromHeadsAroundWhenItFitted.shrink(0);
            for (size_t parameter = 0; parameter < tried.entered.size() && parameter < tried.headParametersThatEnter.size(); ++parameter)
                answer.enteredFromHeadsAroundWhenItFitted.append(tried.headParametersThatEnter[parameter] ? tried.entered[parameter] : TypeID());
            if (!tried.headParametersThatEnter.isEmpty())
                answer.cameRoundBesideParametersWhenItFitted = tried.cameRoundBesideItself;
        }
        if (fits && !answer.hasAskedAboutNarrowerForm && !answer.claimsNothing) {
            answer.hasAskedAboutNarrowerForm = true;
            bool narrows = false;
            std::optional<bool> turnsCheckTheirArithmetic;
            std::optional<Vector<TypeID, 16>> typesThatReach;
            Vector<TypeID, 8> reachingParameters;
            reachingParameters.fill(TypeID(), answer.types.size());
            for (size_t parameter = 0; parameter < answer.types.size() && parameter < tried.cameRound.size() && parameter < tried.entered.size(); ++parameter) {
                TypeID assumed = answer.types[parameter];
                if (!assumed || !tried.cameRound[parameter] || !tried.entered[parameter] || !m_types.at(assumed).isJavaScriptValue() || !m_types.at(tried.cameRound[parameter]).isJavaScriptValue() || !m_types.at(tried.entered[parameter]).isJavaScriptValue())
                    continue;
                TypeID narrowed = m_types.unionOf(tried.entered[parameter], tried.cameRound[parameter]);
                if (startIsGuessedInWalk(tried, parameter) && parameter < tried.cameRoundBesideItself.size() && tried.cameRoundBesideItself[parameter])
                    narrowed = m_types.unionOf(tried.entered[parameter], tried.cameRoundBesideItself[parameter]);
                BlockID headOfCode = inputBlock(frameIndex, inputHead).head;
                if (headOfCode && parameter < m_code.at(headOfCode).parameters().size()) {
                    if (!typesThatReach)
                        typesThatReach = typesThatReachHeadParametersOfNest(tried.firstRecord, tried.numberOfHeadParametersWithSourcesInFront, m_headParametersWithSources.size());
                    auto noted = m_sourcesOfHeadParameters.find(m_code.at(headOfCode).parameters()[parameter].index());
                    bool isOfNest = noted != m_sourcesOfHeadParameters.end() && noted->value.place >= tried.numberOfHeadParametersWithSourcesInFront;
                    TypeID reaches = isOfNest ? (*typesThatReach)[noted->value.place - tried.numberOfHeadParametersWithSourcesInFront] : TypeID();
                    if (reaches && m_types.at(reaches).isJavaScriptValue() && m_types.isSubset(tried.entered[parameter], reaches) && m_types.isSubset(reaches, narrowed))
                        narrowed = reaches;
                }
                if (narrowed == assumed || !m_types.isSubset(narrowed, assumed))
                    continue;
                reachingParameters[parameter] = narrowed;
                if (!turnsCheckTheirArithmetic)
                    turnsCheckTheirArithmetic = turnsCheckArithmeticOnIntegers(inputHead);
                if (!narrowerTypeOfHeadPaysWalk(assumed, narrowed, *turnsCheckTheirArithmetic))
                    continue;
                answer.types[parameter] = narrowed;
                narrows = true;
            }
            if (narrows) {
                for (size_t parameter = 0; parameter < reachingParameters.size(); ++parameter) {
                    if (reachingParameters[parameter])
                        answer.types[parameter] = reachingParameters[parameter];
                }
                tried.narrowingsThatWait.shrink(0);
                tried.narrowingsThatWait.append({ tried.firstRecord, place, tried.numberOfHeadParametersWithSourcesInFront, m_headParametersWithSources.size() });
            }
            answer.steppedOnDoubles = findParametersSteppedOnDoubles(inputHead, answer);
            for (size_t parameter = 0; parameter < answer.steppedOnDoubles.size() && parameter < answer.types.size(); ++parameter)
                narrows |= answer.steppedOnDoubles[parameter] && answer.types[parameter] && representationOfNumber(*this, answer.types[parameter]) == Representation::Int64;
            if (narrows) {
                if (defersPurchase())
                    m_triesOfLoops.last().somethingIsWantedInside = true;
                else
                    fits = false;
            }
        }
        if (fits && (tried.somethingIsWantedInside || scopesWantAnotherWalk)) {
            if (defersPurchase())
                m_triesOfLoops.last().somethingIsWantedInside = true;
            else
                fits = false;
        }
        if (fits && defersPurchase())
            m_triesOfLoops.last().narrowingsThatWait.appendVector(tried.narrowingsThatWait);
        else {
            for (const TryOfLoop::NarrowingThatWaits& narrowing : tried.narrowingsThatWait)
                narrowRowsInsideTo(narrowing);
        }
        if (fits && quietTurnsWroteMoreThanRowSays(answer, tried))
            fits = false;
        if (fits)
            answer.hasFittedOnce = true;
        if (fits && m_parametersThatElementsOutsideViewsWidenedHandedOn && m_headOfRowHandedOn == inputHead && m_frameOfRowHandedOn == frameIndex && !m_waysIntoClosedCopies.isEmpty() && m_waysIntoClosedCopies.last().frameIndex == frameIndex && m_waysIntoClosedCopies.last().inputHead == inputHead) {
            for (size_t parameter = 0; parameter < answer.types.size() && parameter < 64; ++parameter) {
                if ((m_parametersThatElementsOutsideViewsWidenedHandedOn >> parameter & 1) && answer.types[parameter] && m_types.at(answer.types[parameter]).isInteger())
                    count(StatisticID::IntegersKeptWhereElementsOutsideViewsWidened);
            }
        }
        for (size_t parameter = 0; fits && parameter < 64 && parameter < answer.types.size() && answer.sumsThatStartAtNumberThatEnters >> parameter; ++parameter) {
            if ((answer.sumsThatStartAtNumberThatEnters >> parameter & 1) && answer.types[parameter] && m_types.at(answer.types[parameter]).isNumber())
                count(StatisticID::SumsThatGoRoundAsNumbersInTypedBuildsAlone);
        }
        if (fits && !tried.numberOfEdgesThatCameRound && !m_waysIntoClosedCopies.isEmpty() && m_waysIntoClosedCopies.last().inputHead == inputHead && m_waysIntoClosedCopies.last().frameIndex == frameIndex)
            m_everyTurnOfTypedBuildLeaves = true;
        bool isNeverBuiltSeveralTimes = generatorIsWalkedInLoop || !m_opensGuesses || !has(Mechanism::Guesses) || index.hasIrreducibleFlow() || index.isEnteredByHandlerEdge(inputHead) || index.holdsParametersReadByReleaseHintsAlone();
        bool isQuietOutsideArms = !tried.testsSeen.isEmpty() && !tried.doneOutsideArms.mayRunOtherCode() && tried.done.mayRunOtherCode();
        bool sharedClosedCopyWaits = !m_closedCopiesShared.isEmpty() && m_closedCopiesShared.last().frameIndex == frameIndex && m_closedCopiesShared.last().inputHead == inputHead && m_closedCopiesShared.last().isAboutToBeTaken;
        if (fits && !hasAskedForTwoBuilds && !answer.severalBuildsAreStruck && !answer.claimsNothing && has(Mechanism::LoopInvariants) && (sharedClosedCopyWaits || !m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop)) && !walkAroundIsUndone() && !isTooLargeToBeBuiltTwice(index, loop)
            && !isNeverBuiltSeveralTimes && !(tried.callOnUsualWayComesRound && stepsIteration)) {
            hasAskedForTwoBuilds = true;
            auto ownerOfLearned = makeUniqueWithoutFastMallocCheck<LoopPreview>();
            LoopPreview& learned = *ownerOfLearned;
            answer.parametersThatRareWaysWidened = findParametersThatRareWaysWidened(tried, answer, m_frames[frameIndex].blocks[inputHead.index()].head);
            answer.parametersThatElementsOutsideViewsWidened = findParametersThatElementsOutsideViewsWidened(tried, answer, m_frames[frameIndex].blocks[inputHead.index()].head);
            learned.effects = answer.effects;
            learned.typesOfParameters.appendVector(answer.types);
            learned.typesOfValues = answer.typesOfValues;
            learned.firstValue = answer.firstValue;
            learned.hasLetMentionsLeave = true;
            bool mayByText;
            {
                m_rowSaysTurnsAreQuietOutsideArms = isQuietOutsideArms;
                m_effectsOutsideArmsOfRow = tried.doneByQuietDrafts;
                m_testsSeenOfRow = tried.testsSeen;
                m_firstOtherCodeRanInCopyOfRow = tried.firstOtherCodeRanInCopy;
                m_someCallStaysInTurnOfRow = tried.someCallStaysInTurn;
                m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces = tried.asksLengthsOfViewsFromInFrontForWrittenPlaces;
                m_operationsThatRanOtherCodeOfRow = tried.operationsThatRanOtherCode;
                m_guardsOfStoresAskedOfRow = tried.guardsOfStoresAsked;
                BodyBlock head = index.block(inputHead);
                Vector<Reference, 8> boundBehindWalk;
                for (uint32_t parameter = 0; parameter < head.numberOfParameters() && parameter < enteringKept.size(); ++parameter) {
                    boundBehindWalk.append(m_frames[frameIndex].references[head.parameter(parameter).index()]);
                    m_frames[frameIndex].references.placeOf<NamesOfValues::Writer::AsksWhetherLoopMayBeBuiltTwice>(head.parameter(parameter).index()) = enteringKept[parameter];
                }
                SetForScope asksOnly(m_asksOnlyWhetherLoopMayBeBuiltTwice, true);
                SetForScope listsStruckWhileAsking(m_listsAreStruckInRow, answer.listsAreStruck);
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
                letMentionsOfLoopLeave(inputHead, answer.mentionsThatLeave.span(), &keptWhileAsking, answer.storesIntoScopes.areKnown);
                if (tried.done.mayRunOtherCode()) {
                    auto typeInRow = [&](ValueID input) -> TypeID {
                        TypeID known;
                        if (input.index() >= answer.firstValue && input.index() - answer.firstValue < answer.typesOfValues.size())
                            known = answer.typesOfValues[input.index() - answer.firstValue];
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
                }
                Vector<std::pair<uint32_t, IterationByIndex>, 2> steppedBehindWalk;
                for (const auto& [placeOfIteration, entered] : steppedInFrontOfLoop.asTheyEntered)
                    steppedBehindWalk.append({ placeOfIteration, std::as_const(m_iterationsByIndex)[placeOfIteration] });
                uint32_t numberInFrontBehindWalk = m_numberOfIterationsInFrontOfTheirLoops;
                putBackIterationsSteppedAt(steppedInFrontOfLoop);
                m_otherCopyOpensItsGuessesAsAsked = false;
                mayByText = buildSeveralTimes(learned);
                for (auto& [placeOfIteration, behindWalk] : steppedBehindWalk)
                    m_iterationsByIndex[placeOfIteration] = WTF::move(behindWalk);
                m_numberOfIterationsInFrontOfTheirLoops = numberInFrontBehindWalk;
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
                    m_frames[frameIndex].references.placeOf<NamesOfValues::Writer::AsksWhetherLoopMayBeBuiltTwice>(head.parameter(parameter).index()) = boundBehindWalk[parameter];
            }
            if (mayByText) {
                Vector<GuessCountedOnByLooks, 4> testsOfPurchase = m_testsSeenOfRow;
                Vector<TypeID, 8> typesOfPlainWalk = answer.types;
                narrowRowToWhatCameRound(answer, tried);
                learned.typesOfParameters.shrink(0);
                learned.typesOfParameters.appendVector(answer.types);
                bool leavesURLUnmade = keptByStrings() && notesOfStrings(*this).urlsThatAreNeverMade.containsIf([&](const NotesOfStrings::URLThatIsNeverMade& url) {
                    return url.inlineFrame == m_frames[frameIndex].inlineFrame && url.made.index() >= learned.firstValue && url.made.index() - learned.firstValue < learned.typesOfValues.size();
                });
                bool keepsPlainWalk = m_frontOfLoopLeftOpen.block && tried.stateInFront && tried.freshObjectsKept.isEmpty() && m_otherCopyOpensItsGuessesAsAsked && !leavesURLUnmade && (m_numberOfLoopsThatHoldLoopsBuiltTwice == numberOfLoopsThatHoldLoopsBuiltTwiceBeforeWalk || !m_waysIntoClosedCopies.containsIf([&](const WayIntoClosedCopy& way) { return way.frameIndex != frameIndex || way.inputHead != inputHead; }))
                    && !sharedClosedCopyWaits && !hasTextsInStringBuilders() && !walkBeganTexts && steppedInFrontOfLoop.asTheyEntered.isEmpty() && !handlersOutsideLoopReadItsValues(inputHead)
                    && !enteringKept.containsIf([&](Reference enters) { return isTextInStringBuilder(*this, enters); })
                    && !m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead))
                    && m_code.at(m_frontOfLoopLeftOpen.head).parameters().size() == enteringKept.size() && m_frontOfLoopLeftOpen.block.index() < m_frontOfLoopLeftOpen.head.index()
                    && !findRowThatHeadReadsOffListInFront(*this, inputHead, learned);
                bool isBuilt;
                bool sumsWereAsked = !m_sumsAreNotAskedInRow;
                bool textsStayedOutOfBuilders = textsOfLoopStayOutOfBuilders(inputHead);
                if (keepsPlainWalk)
                    isBuilt = makeTypedBuildBesidePlainWalk(learned, tried);
                else {
                    undoPlainWalk();
                    isBuilt = makeBuilds(learned, tried.entered, tried.numberOfEdgesThatCameRound, undoPlainWalk);
                }
                if (isBuilt && !m_hasFailed && !textsStayedOutOfBuilders && textsOfLoopStayOutOfBuilders(inputHead) && !m_numberOfTryWhoseWalkIsCutShort) {
                    keepsPlainWalk = false;
                    undoPlainWalk();
                    isBuilt = makeBuilds(learned, tried.entered, tried.numberOfEdgesThatCameRound, undoPlainWalk);
                }
                m_answersOfLoops[place].types = typesOfPlainWalk;
                if (m_numberOfTryWhoseWalkIsCutShort)
                    return true;
                if (isBuilt) {
                    auto bought = std::make_shared<BuildsBought>();
                    bought->effects = learned.effects;
                    bought->typesOfParameters = learned.typesOfParameters;
                    bought->typesOfValues = learned.typesOfValues;
                    bought->firstValue = learned.firstValue;
                    bought->entered = tried.entered;
                    bought->isQuietOutsideArms = isQuietOutsideArms;
                    bought->effectsOfQuietDrafts = tried.doneByQuietDrafts;
                    bought->tests = testsOfPurchase;
                    bought->firstOtherCodeRanInCopy = tried.firstOtherCodeRanInCopy;
                    bought->someCallStaysInTurn = tried.someCallStaysInTurn;
                    bought->asksLengthsOfViewsFromInFrontForWrittenPlaces = tried.asksLengthsOfViewsFromInFrontForWrittenPlaces;
                    bought->operationsThatRanOtherCode = tried.operationsThatRanOtherCode;
                    bought->guardsOfStoresAsked = tried.guardsOfStoresAsked;
                    bought->numberOfEdgesThatCameRound = tried.numberOfEdgesThatCameRound;
                    bought->keepsPlainWalk = keepsPlainWalk;
                    m_answersOfLoops[place].bought = WTF::move(bought);
                    if (keepsPlainWalk && tried.rowAsAssumed)
                        noteRowAsWalkedLast(place, isWalkedInBuild, tried.rowAsAssumed);
                    return true;
                }
                if (sumsWereAsked && m_sumsAreNotAskedInRow) {
                    hasAskedForTwoBuilds = false;
                    undoPlainWalk();
                    continue;
                }
                m_answersOfLoops[place].severalBuildsAreStruck = true;
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
            m_rowsWalkedAgainForCheck.add(key, 0);
            fits = false;
            isUndoneForNet = true;
        }
        if (fits && frontOfQuietTurnsIsWorthAWalk(m_answersOfLoops[place], tried)) {
            if (defersPurchase())
                m_triesOfLoops.last().somethingIsWantedInside = true;
            else
                fits = false;
        }
        if (!fits && walksKeptWalkAlone) {
            fail("a walk of a loop does not fit the row that its kept walk was walked under"_s, InternalErrorKind::LastWalkOfLoopMisfitsRow);
            return true;
        }
        if (fits) {
            m_effectsOfLoopKeptLast = tried.done;
            m_guardsOfStoresAskedInLoopKeptLast = tried.guardsOfStoresAsked;
            m_walkOfLoopWasKept = true;
            m_headOfLoopKeptLast = inputHead;
            m_frameOfLoopKeptLast = frameIndex;
            if (tried.rowAsAssumed)
                noteRowAsWalkedLast(place, isWalkedInBuild, tried.rowAsAssumed);
            endFrontOfLoopLeftOpen();
            break;
        }
        if (hasFailedInside && ((!tried.misfits && effectsFit) || std::exchange(m_answersOfLoops[place].hasFailedWhereItMisfitted, true))) {
            if (m_answersOfLoops[place].claimsNothing)
                return true;
            m_answersOfLoops[place].claimsNothing = true;
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
            m_answersOfLoops = WTF::move(answersBefore);
            m_placesOfAnswersOfLoops = WTF::move(placesOfAnswersBefore);
        }
    }
    return true;
}

bool Walker::looksAtLoopsAroundHaveCounted(const Draft& draft, const Effects& effects)
{
    if (looksAtLoopsAroundHaveSeen(effects))
        return true;
    const LinkTables& linked = facts().linked();
    if (effectRuleOf(draft.opcode) != EffectRule::Call || !isWrittenInFrameOnTop(draft) || linked.calls.isEmpty())
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
