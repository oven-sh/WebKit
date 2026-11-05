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
    if (!walker.has(Mechanism::TypedOperations) || !boundsProvenOnEdge(index, terminator, 0))
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
    return std::ranges::any_of(m_triesOfLoops, [&](const TryOfLoop& tried) {
        return tried.frameIndex == frameIndex && tried.inputHead == inputHead;
    });
}

#define FOR_EACH_FIELD_OF_FRAME_IN_TRY(macro) \
    macro(block) macro(returnDominator) macro(endOfValues) macro(walkedUpTo) macro(endOfStretch) macro(numberOfStretchesBeingWalked) \
    macro(placeTakenByEndOfBlockAskedLast) macro(hasHandler) macro(numberOfFramesUpToHandlerBelow) macro(firstOperationToWalk) macro(hasPassedByWhatFeedsThrow) macro(standsInFrontOfHead) \
    macro(narrowedValues) macro(firstNarrowerName) macro(listOfNarrowerNamesIsLong) macro(beginOfStretch) macro(loopLookedAt) macro(firstValueLookedAt) macro(typesOfLooks) \
    macro(loopsBeingBuiltTwice) macro(headOfTurnsWrittenOut) macro(turnsOfForIn) macro(valuesListedForTurnsWrittenOut) macro(stepWhosePositionEdgesBring) \
    macro(placeOfForInWhosePositionEdgesBring) macro(blocksWalkedForEachWay) macro(wholeExpectations) macro(whereCopyLeavesForCall) macro(whereHeadEnds)

#define FOR_EACH_FIELD_OF_WALKER_IN_TRY(macro) \
    macro(m_state) macro(m_block) macro(m_origin) macro(m_opensGuesses) macro(m_firstFreeOpenEdge) macro(m_pendingBlocks) macro(m_numberOfClosedArms) \
    macro(m_numberOfIterationsInFrontOfTheirLoops) macro(m_isLeadingHandlerEdge) \
    macro(m_numberOfLoopsThatMayBeLeft) macro(m_numberOfScopesWhereProofsHoldEverywhere) \
    macro(m_loopsOverGenerators) macro(m_environmentCellsOfUnit) macro(m_operationsCopiedAtWrittenCall) macro(m_closuresThatAllocationAloneTakes) macro(m_numberOfCopiesBuiltForSize) \
    macro(m_isWalkingAccess) macro(m_pollInFrontOfLoopBeingEntered) macro(m_edgeOfCodeBeingLed) macro(m_callCopiedByLinkOfChain) \
    macro(m_numberOfFramesAtCallCopiedByLinkOfChain) macro(m_callIsAccountedFor) macro(m_calleeAccountedFor) \
    macro(m_holdsReadsOfVariablesAsConstants) macro(m_countsOfUnit) macro(m_numberOfGuessesOpened) macro(m_numberOfListsAskedInFrontOfTypedCopies) macro(m_mechanisms) \
    macro(m_waysIntoClosedCopies) macro(m_closedCopiesShared) macro(m_copyThatMakesNewList) macro(m_receiverOfCopyThatMakesNewList) macro(m_depthOfCopyThatMakesNewList) \
    macro(m_hasURLsThatAreNeverMade) \
    macro(m_wordsThatSayNothing) macro(m_valueBeingWalked) macro(m_numberOfFreshCellsLeftAloneAtReturns) macro(m_numberOfTurnsThatBeginAgain) macro(m_numberOfLoopsBuiltTwice) \
    macro(m_numberOfAssignmentsWrittenOut) macro(m_numberOfJoinsWalkedForEachWay) macro(m_numberOfEmptySlotsTakenAsCases) macro(m_numberOfSlicesMadeOfElementsInSight) \
    macro(m_numbersOfProofsChecked) macro(m_numberOfProofsThatNoTestTells) macro(m_numberOfProofsThatFormTells) macro(m_numberOfDoorsThatCanNeverPass) macro(m_numberOfChecksThatWalkAnswers) \
    macro(m_generatorsTakenByConsumers) macro(m_generatorsWalkedInConsumers) macro(m_resumptionsToLeave) macro(m_numberOfFramesOverHandlersOfCode) \
    macro(m_numberOfFramesAtRowOfShapeTests) macro(m_numberOfObjectsMetAsOne)

struct Walker::SnapshotForTry {
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
#define DECLARE_FIELD_OF_WALKER(name) std::remove_reference_t<decltype(Walker::name)> name;
    FOR_EACH_FIELD_OF_WALKER_IN_TRY(DECLARE_FIELD_OF_WALKER)
#undef DECLARE_FIELD_OF_WALKER
    Code::Mark code;
    Vector<Code::OpenBlock, 8> openBlocks;
    size_t numberOfFrames { 0 };
    size_t numberOfScopesThatClosuresHold { 0 };
    size_t numberOfScopesWhoseFieldsHaveComeRound { 0 };
    size_t numberOfScopesCopiedOnWaysThatAreNotRare { 0 };
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
        , numberOfScopesCopiedOnWaysThatAreNotRare(walker.m_scopesCopiedOnWaysThatAreNotRare.size())
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
        if (!walker.m_generatorsWalkedInConsumers.isEmpty()) {
            for (size_t frameIndex = 0; frameIndex < numberOfFrames; ++frameIndex)
                numbersOfReturnsOfCopies.append(walker.m_frames[frameIndex].returnsOfCopy ? walker.m_frames[frameIndex].returnsOfCopy->size() : 0);
        }
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
        walker.m_hasFailed = false;
        walker.m_failure = { };
        walker.m_kindOfFailure = InternalErrorKind::Unknown;
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
        walker.m_openEdges.putBack(numberOfOpenEdges, numberOfOpenEdgesLogged);
        walker.m_openLoops.putBack(numberOfOpenLoops, numberOfOpenLoopsLogged);
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
                auto& codeUnits = changed.isBesideCharacter ? notes.codeUnitsBesideCharacters : notes.codeUnitsBesideCharCodes;
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
                walker.m_frames[binding.frame].references[binding.input.index()] = binding.previous;
        }
        walker.m_frames.first().booleansOfUnit = booleansOfUnit;
        while (walker.m_inputBlocksInTries.size() > numberOfInputBlocksLogged) {
            InputBlockInTry logged = walker.m_inputBlocksInTries.takeLast();
            if (logged.frame < walker.m_frames.size())
                walker.m_frames[logged.frame].blocks[logged.block.index()] = logged.before;
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
        walker.m_scopesCopiedOnWaysThatAreNotRare.shrink(numberOfScopesCopiedOnWaysThatAreNotRare);
        while (walker.m_blocksWithFieldsOfScopesInOrder.size() > numberOfBlocksWithFieldsOfScopes)
            walker.m_fieldsOfScopesOfHeads.remove(walker.m_blocksWithFieldsOfScopesInOrder.takeLast());
        walker.m_code.abandonBackTo(code, openBlocks.span());
    }
};

void Walker::letMentionsOfLoopLeave(BlockID inputHead, std::span<const ValueID> mentionsThatLeave, Vector<FreshObjectKeptAtHead, 2>* kept, bool keepsScopes)
{
    if (!m_state.holdsFreshObjects())
        return;
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

void Walker::holdFreshObjectsAgainstWhatComesRound(BlockID head)
{
    for (size_t place = m_triesOfLoops.size(); place--;) {
        TryOfLoop& tried = m_triesOfLoops[place];
        if (tried.frameIndex >= m_frames.size() || inputBlock(tried.frameIndex, tried.inputHead).head != head)
            continue;
        for (const FreshObjectKeptAtHead& kept : tried.freshObjectsKept) {
            if (!m_state.isFresh(kept.object))
                loseObjectKeptAtHead(tried, kept);
        }
        if (tried.stateAtHeadThatKeepsScopes)
            holdFieldsOfScopesAgainstWhatComesRound(tried, head);
        return;
    }
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
        if (isNew) {
            answer.types.append(entering);
            answer.rungs.append(0);
            bool startIsStruck = parameter < answer.enteredWhereStartWasStruck.size() && answer.enteredWhereStartWasStruck[parameter];
            if (entering && m_types.at(entering).isJavaScriptValue() && !(parameter < isUnchanged.size() && isUnchanged[parameter]) && !startIsStruck) {
                Type record = m_types.at(entering);
                if (record.primitives == Primitive::Integer && !record.integers.isEmpty() && record.integers.isSubsetOf(Range::int32())) {
                    if (record.integers.minimum == record.integers.maximum)
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
                TypeID expected = findExpectation(*this, index.block(inputHead).parameter(parameter)).type;
                if (expected && m_types.at(expected).isJavaScriptValue() && !m_types.at(expected).isAnything() && !(m_types.at(expected).primitives & Primitive::Number) && !(m_types.at(entering).primitives & Primitive::Number) && m_types.at(entering).isUndefinedOrNull())
                    answer.types[parameter] = m_types.unionOf(entering, expected);
            }
        }
        bool mayStayIntegerInThisBuild = (!sameTypesEnterAsInRowHandedOn || (isTypedBuildOfPurchase && isComparedByTestOfHeadInLoops(index, inputHead, parameter))) && entering && m_types.at(entering).isInteger() && parameter < m_typesOfRowHandedOn.size() && m_typesOfRowHandedOn[parameter] && !m_types.at(m_typesOfRowHandedOn[parameter]).isInteger();
        if (isNew && rowIsHandedOn && !mayStayIntegerInThisBuild && parameter < m_typesOfRowHandedOn.size() && parameter < m_enteredOfRowHandedOn.size() && m_enteredOfRowHandedOn[parameter] == entering) {
            answer.types[parameter] = m_typesOfRowHandedOn[parameter];
            answer.rungs[parameter] = m_rungsOfRowHandedOn[parameter];
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
    const Frame& frame = m_frames.last();
    const BodyIndex& index = *frame.index;
    constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Increment, Opcode::Decrement };
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (!arithmetic.contains(operation.opcode()))
                continue;
            Reference bound = frame.references[operation.id().index()];
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
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue step : index.block(member).values()) {
            if (!steps.contains(step.opcode()))
                continue;
            uint32_t numberOfOperands = numberOfNamedOperandsOf(step.opcode());
            bool stepsParameterOfHead = false;
            for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
                ValueID stepped = index.valueBehindCasts(step.operand(operandIndex));
                stepsParameterOfHead |= index.isParameter(stepped) && index.blockOf(stepped) == inputHead;
            }
            if (!stepsParameterOfHead)
                continue;
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
    }
    if (isStepped.isEmpty())
        return isStepped;

    Vector<uint32_t> madeOf;
    madeOf.fill(0, endOfValues - firstValue);
    auto partsOf = [&](ValueID value) -> uint32_t {
        return value && value.index() >= firstValue && value.index() < endOfValues ? madeOf[value.index() - firstValue] : 0;
    };
    auto mayBeTakenAsInteger = [&](ValueID value) {
        TypeID type = typeInRow(value);
        return !type || m_types.at(type).isInteger();
    };
    for (uint32_t parameter = 0; parameter < head.numberOfParameters(); ++parameter) {
        if (isStepped[parameter])
            madeOf[head.parameter(parameter).index() - firstValue] = 1u << parameter;
    }
    uint32_t takenAsIntegers = 0;
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        for (BodyValue operation : block.values()) {
            uint32_t parts = 0;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                parts |= partsOf(operand);
            });
            if (!parts)
                continue;
            Opcode opcode = operation.opcode();
            TypeID result = typeInRow(operation.id());
            if (opcodesThatStateTypes.contains(opcode) || (steps.contains(opcode) && result && m_types.at(result).isNumber()))
                madeOf[operation.id().index() - firstValue] = parts;
            else if (!allRelations.contains(opcode) || (mayBeTakenAsInteger(operation.operand(0)) && mayBeTakenAsInteger(operation.operand(1))))
                takenAsIntegers |= parts;
        }
        BodyTerminator end = block.terminator();
        end.forEachOperand([&](ValueID operand, uint32_t) {
            takenAsIntegers |= partsOf(operand);
        });
        for (BodyTarget target : end.targets()) {
            bool isJoinBehind = target.block().index() > member.index() && index.isInLoop(target.block(), loop) && !index.isLoopHead(target.block());
            BodyBlock to = index.block(target.block());
            uint32_t place = 0;
            for (ValueID argument : target.arguments()) {
                uint32_t parts = partsOf(argument);
                if (parts && isJoinBehind && place < to.numberOfParameters())
                    madeOf[to.parameter(place).index() - firstValue] |= parts;
                else if (parts && target.block() != inputHead && index.isInLoop(target.block(), loop))
                    takenAsIntegers |= parts;
                ++place;
            }
        }
        if (block.hasHandler()) {
            for (ValueID argument : block.handler().arguments())
                takenAsIntegers |= partsOf(argument);
        }
    }
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
bool Walker::forEachValueThatMeetsIn(ValueID value, BlockID openHead, uint32_t firstRecord, const Functor& functor) const
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
        code.forEachEdgeInto(block, [&](const Edge& edge) {
            everyEdgeFillsIt = everyEdgeFillsIt && position < edge.arguments().size();
            if (everyEdgeFillsIt)
                take(edge.arguments()[position], inJoin);
        });
        return everyEdgeFillsIt;
    };
    take(value, nullptr);
    for (size_t place = 0; place < joins.size(); ++place) {
        if (!takeWhatEntersJoin(joins[place], nullptr))
            return false;
    }
    std::ranges::sort(joins, { }, [](ValueID each) { return each.index(); });
    for (ValueID join : joins) {
        WhatMeetsInParameter inJoin { { }, { }, firstRecord };
        takeWhatEntersJoin(join, &inJoin);
        m_whatMeetsInParameters.set(join.index(), inJoin);
    }
    return true;
}

TypeID Walker::typeThatComesRoundBesideParameterItself(ValueID headParameter, Reference value) const
{
    if (!value || !value.isValue())
        return value ? ownTypeOf(value) : TypeID();
    TypeID beside;
    bool isLookedThrough = forEachValueThatMeetsIn(value.id(), m_code.at(headParameter).block(), headParameter.index(), [&](ValueID source, TypeID type) {
        if (source != headParameter)
            beside = beside ? m_types.unionOf(beside, type) : type;
    });
    return isLookedThrough ? beside : ownTypeOf(value);
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
            bool isLookedThrough = forEachValueThatMeetsIn(enters.id(), m_frames[around->frameIndex].blocks[around->inputHead.index()].head, around->firstRecord, [&](ValueID source, TypeID type) {
                size_t parameterAround = 0;
                const TryOfLoop* taken = !headParameter && source && m_code.at(source).opcode == Opcode::Parameter && m_code.at(source).block() && m_code.at(m_code.at(source).block()).isLoopHead ? tryOfHeadParameter(source, parameterAround) : nullptr;
                if (taken) {
                    headParameter = source;
                    explained = parameterAround < taken->cameRoundBesideItself.size() && taken->cameRoundBesideItself[parameterAround] ? m_types.unionOf(type, taken->cameRoundBesideItself[parameterAround]) : type;
                } else if (!source || source != headParameter)
                    beside = beside ? m_types.unionOf(beside, type) : type;
            });
            if (!isLookedThrough || !headParameter || !m_types.isSubset(ownTypeOf(enters), beside ? m_types.unionOf(explained, beside) : explained))
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
        if (m_sourcesOfHeadParameters.set(parameters[parameter].index(), WTF::move(sources)).isNewEntry)
            m_headParametersWithSources.append(parameters[parameter].index());
    }
}

Walker::TypesByRecord Walker::typesThatReachHeadParametersOfNest(uint32_t firstRecordOfNest, std::span<const uint32_t> headParametersOfNest)
{
    TypesByRecord typesThatReach;
    HashMap<uint32_t, Vector<uint32_t, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> takers;
    Vector<uint32_t, 16> waiting;
    auto join = [&](TypeID& all, TypeID type) {
        if (!type)
            return false;
        TypeID joined = all ? m_types.unionOf(all, type) : type;
        return std::exchange(all, joined) != joined;
    };
    for (uint32_t headParameter : headParametersOfNest) {
        const SourcesOfHeadParameter& sources = m_sourcesOfHeadParameters.find(headParameter)->value;
        TypeID all = sources.made;
        for (ValueID handedOn : sources.headParametersHandedOn) {
            if (handedOn.index() >= firstRecordOfNest && m_sourcesOfHeadParameters.contains(handedOn.index()))
                takers.add(handedOn.index(), Vector<uint32_t, 2> { }).iterator->value.append(headParameter);
            else
                join(all, m_code.at(handedOn).type);
        }
        typesThatReach.set(headParameter, all);
        waiting.append(headParameter);
    }
    while (!waiting.isEmpty()) {
        uint32_t handedOn = waiting.takeLast();
        auto found = takers.find(handedOn);
        if (found == takers.end())
            continue;
        TypeID type = typesThatReach.get(handedOn);
        for (uint32_t taker : found->value) {
            if (join(typesThatReach.find(taker)->value, type))
                waiting.append(taker);
        }
    }
    return typesThatReach;
}

void Walker::narrowRowsInsideTo(const TryOfLoop::NarrowingThatWaits& narrowing)
{
    std::span<const uint32_t> headParametersOfNest = m_headParametersWithSources.span().subspan(narrowing.firstHeadParameter, narrowing.endOfHeadParameters - narrowing.firstHeadParameter);
    TypesByRecord typesThatReach = typesThatReachHeadParametersOfNest(narrowing.firstRecordOfNest, headParametersOfNest);
    for (uint32_t headParameter : headParametersOfNest) {
        const SourcesOfHeadParameter& sources = m_sourcesOfHeadParameters.find(headParameter)->value;
        if (sources.answer == narrowing.ownAnswer || sources.answer >= m_answersOfLoops.size())
            continue;
        AnswerOfLoop& inside = m_answersOfLoops[sources.answer];
        if (sources.parameter >= inside.types.size() || inside.claimsNothing)
            continue;
        TypeID reaches = typesThatReach.get(headParameter);
        TypeID held = inside.types[sources.parameter];
        if (reaches && held && m_types.at(held).isJavaScriptValue() && m_types.at(reaches).isJavaScriptValue() && m_types.isSubset(reaches, held))
            inside.types[sources.parameter] = reaches;
    }
}

void Walker::noteAllThatComesRoundInTry(BlockID head, size_t parameter, TypeID found, Reference value)
{
    for (size_t place = m_triesOfLoops.size(); place--;) {
        TryOfLoop& tried = m_triesOfLoops[place];
        if (tried.frameIndex >= m_frames.size() || inputBlock(tried.frameIndex, tried.inputHead).head != head)
            continue;
        if (tried.cameRound.size() <= parameter)
            tried.cameRound.grow(parameter + 1);
        tried.cameRound[parameter] = tried.cameRound[parameter] ? m_types.unionOf(tried.cameRound[parameter], found) : found;
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

bool Walker::noteWhatCameRoundInTry(BlockID head, size_t parameter, TypeID assumed, TypeID found, size_t numberOfTriesAround, TypeID* enteredUnderGuessedStart)
{
    if (!m_types.at(assumed).isJavaScriptValue() || !m_types.at(found).isJavaScriptValue())
        return false;
    for (size_t place = std::min(numberOfTriesAround, m_triesOfLoops.size()); place--;) {
        TryOfLoop& tried = m_triesOfLoops[place];
        if (tried.frameIndex >= m_frames.size() || inputBlock(tried.frameIndex, tried.inputHead).head != head)
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
    if (m_code.at(value).opcode != Opcode::Parameter && !conversionsOfForm.contains(m_code.at(value).opcode) && !arithmeticWhoseKindFollowsItsOperands.contains(m_code.at(value).opcode))
        return;
    ValuesReached reached;
    Vector<ValueID, 2> own;
    ValueID began;
    reached.add(value);
    for (size_t next = 0; next < reached.inOrder.size(); ++next) {
        ValueID id = reached.inOrder[next];
        while (conversionsOfForm.contains(m_code.at(id).opcode))
            id = m_code.at(id).operand(0);
        if (!next)
            began = id;
        const Operation& record = m_code.at(id);
        if (arithmeticWhoseKindFollowsItsOperands.contains(record.opcode) && id.index() >= tried.firstRecord) {
            for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(record.opcode); ++operandIndex) {
                if (ValueID operand = record.operand(operandIndex); operand && m_types.at(m_code.at(operand).type).hasOnlyPrimitives(Primitive::Number | Primitive::BigInt))
                    reached.add(operand);
            }
            continue;
        }
        BlockID block = record.opcode == Opcode::Parameter ? record.block() : BlockID();
        if (block == head) {
            if (!own.contains(id))
                own.append(id);
            continue;
        }
        if (!block || id.index() < tried.firstRecord || m_code.at(block).isLoopHead || m_code.at(block).isHandlerBlock())
            continue;
        if (auto noted = tried.ownParametersThatMeetInRecords.find(id.index()); noted != tried.ownParametersThatMeetInRecords.end()) {
            for (ValueID earlier : noted->value) {
                if (!own.contains(earlier))
                    own.append(earlier);
            }
            continue;
        }
        std::span<const ValueID> parameters = m_code.at(block).parameters();
        size_t position = std::ranges::find(parameters, id) - parameters.begin();
        for (EdgeID edge = m_code.at(block).firstEdgeIn(); edge; edge = m_code.at(edge).nextInto()) {
            if (position >= m_code.at(edge).arguments().size())
                continue;
            reached.add(m_code.at(edge).arguments()[position]);
        }
    }
    if (tried.ownParametersThatMeetInWhatCameRound.size() <= parameter)
        tried.ownParametersThatMeetInWhatCameRound.grow(parameter + 1);
    for (ValueID found : own) {
        if (!tried.ownParametersThatMeetInWhatCameRound[parameter].contains(found))
            tried.ownParametersThatMeetInWhatCameRound[parameter].append(found);
    }
    tried.ownParametersThatMeetInRecords.set(began.index(), WTF::move(own));
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
    Vector<Vector<uint32_t, 2>, 8> takers;
    takers.grow(parameters.size());
    Vector<uint32_t, 8> waiting;
    for (const Vector<Vector<ValueID, 2>, 8>* lists : { &tried.headParametersThatCameRound, &tried.ownParametersThatMeetInWhatCameRound }) {
        for (uint32_t taker = 0; taker < lists->size() && taker < answer.types.size(); ++taker) {
            for (ValueID handedOn : lists->at(taker)) {
                uint32_t giver = std::ranges::lower_bound(parameters, handedOn.index(), { }, [](ValueID each) { return each.index(); }) - parameters.begin();
                if (giver >= parameters.size() || parameters[giver] != handedOn || giver == taker || giver >= answer.types.size())
                    continue;
                if (takers[giver].isEmpty())
                    waiting.append(giver);
                takers[giver].append(taker);
            }
        }
    }
    while (!waiting.isEmpty()) {
        uint32_t giver = waiting.takeLast();
        TypeID given = answer.types[giver];
        if (!given || !m_types.at(given).isJavaScriptValue())
            continue;
        for (uint32_t taker : takers[giver]) {
            TypeID held = answer.types[taker];
            if (!held || !m_types.at(held).isJavaScriptValue() || m_types.isSubset(given, held))
                continue;
            answer.types[taker] = m_types.unionOf(held, given);
            waiting.append(taker);
        }
    }
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

bool Walker::tryLoop(BlockID inputHead, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (isBeingTried(frameIndex, inputHead))
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
        m_answersOfLoops.append(AnswerOfLoop { });
        if (m_effectsSeededForNextRow) {
            m_answersOfLoops.last().effects = *m_effectsSeededForNextRow;
            m_answersOfLoops.last().knowsEffects = true;
        }
    }
    m_effectsSeededForNextRow = std::nullopt;
    bool hasUndoneWhatFits = false;
    bool hasAskedForTwoBuilds = false;

    Vector<Reference, 8> enteringKept;
    enteringKept.append(entering);
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
    auto putRowOfPurchaseAroundBack = makeScopeExit([this, isQuiet = m_rowSaysTurnsAreQuietOutsideArms, effects = m_effectsOutsideArmsOfRow, tests = m_testsSeenOfRow, first = m_firstOperationThatRunsOtherCodeOfRow, firstRanInCopy = m_firstOtherCodeRanInCopyOfRow, ran = m_operationsThatRanOtherCodeOfRow]() mutable {
        m_rowSaysTurnsAreQuietOutsideArms = isQuiet;
        m_effectsOutsideArmsOfRow = effects;
        m_testsSeenOfRow = WTF::move(tests);
        m_firstOperationThatRunsOtherCodeOfRow = first;
        m_firstOtherCodeRanInCopyOfRow = firstRanInCopy;
        m_operationsThatRanOtherCodeOfRow = WTF::move(ran);
    });
    auto ownerOfSnapshot = makeUniqueWithoutFastMallocCheck<SnapshotForTry>(*this);
    SnapshotForTry& snapshot = *ownerOfSnapshot;
    IterationsSteppedAtHead steppedInFrontOfLoop = takeIterationsSteppedAt(inputHead);
    bool generatorIsWalkedInLoop = loopCarriesValuesOfConsumerRound(inputHead) || m_generatorsTakenByConsumers.containsIf([&](const GeneratorTakenByConsumer& taken) {
        return taken.inputHead == inputHead && taken.inlineFrame == m_frames[frameIndex].inlineFrame;
    });
    auto buildSeveralTimes = [&](const LoopPreview& learned) {
        return !generatorIsWalkedInLoop && mayBuildLoopSeveralTimes(inputHead) && (buildLoopForEachMember(inputHead, learned, enteringKept.span()) || buildLoopTwice(inputHead, learned, enteringKept.span()));
    };
    auto makeBuilds = [&](LoopPreview& learned, const Vector<TypeID, 8>& entered, uint32_t numberOfEdgesThatCameRound, const auto& standInFrontAgain) {
        std::pair<uint32_t, BlockID> bought { static_cast<uint32_t>(frameIndex), inputHead };
        if (std::ranges::count(m_loopsBeingBought, bought) >= 2)
            return false;
        m_loopsBeingBought.append(bought);
        auto isBoughtNoLonger = makeScopeExit([&] {
            m_loopsBeingBought.removeLast();
        });
        for (Reference argument : enteringKept)
            letLeave(m_state, argument);
        Vector<FreshObjectKeptAtHead, 2> keptInFrontOfBuilds;
        letMentionsOfLoopLeave(inputHead, m_answersOfLoops[place].mentionsThatLeave.span(), &keptInFrontOfBuilds, m_answersOfLoops[place].storesIntoScopes.areKnown);
        handScopesKeptAtHeadToPreview(m_answersOfLoops[place].storesIntoScopes, keptInFrontOfBuilds.span(), learned);
        bool isBuilt;
        {
            SetForScope headHandedOn(m_headOfRowHandedOn, inputHead);
            SetForScope mentionsHandedOn(m_mentionsThatLeaveHandedOn, m_answersOfLoops[place].mentionsThatLeave);
            SetForScope storesHandedOn(m_storesIntoScopesHandedOn, m_answersOfLoops[place].storesIntoScopes);
            SetForScope frameHandedOn(m_frameOfRowHandedOn, frameIndex);
            SetForScope enteredHandedOn(m_enteredOfRowHandedOn, entered);
            SetForScope typesHandedOn(m_typesOfRowHandedOn, m_answersOfLoops[place].types);
            SetForScope rungsHandedOn(m_rungsOfRowHandedOn, m_answersOfLoops[place].rungs);
            SetForScope buys(m_buysTwoBuilds, true);
            SetForScope listsStruck(m_listsAreStruckInRow, m_answersOfLoops[place].listsAreStruck);
            m_everyTurnOfTypedBuildLeaves = false;
            m_turnOfTypedBuildLeavesWithoutTest = false;
            m_typedBuildTestsNothing = false;
            m_typedBuildEndedListsTestedInFront = false;
            m_typedBuildAskedNoListThatItWasBoughtFor = false;
            isBuilt = buildSeveralTimes(learned);
            if (isBuilt && std::exchange(m_typedBuildEndedListsTestedInFront, false)) {
                standInFrontAgain();
                for (Reference argument : enteringKept)
                    letLeave(m_state, argument);
                keptInFrontOfBuilds.shrink(0);
                letMentionsOfLoopLeave(inputHead, m_answersOfLoops[place].mentionsThatLeave.span(), &keptInFrontOfBuilds, m_answersOfLoops[place].storesIntoScopes.areKnown);
                handScopesKeptAtHeadToPreview(m_answersOfLoops[place].storesIntoScopes, keptInFrontOfBuilds.span(), learned);
                m_answersOfLoops[place].listsAreStruck = true;
                m_listsAreStruckInRow = true;
                isBuilt = buildSeveralTimes(learned);
            }
            bool leavesAndTestsNothing = std::exchange(m_turnOfTypedBuildLeavesWithoutTest, false) & std::exchange(m_typedBuildTestsNothing, false);
            if (isBuilt && (std::exchange(m_everyTurnOfTypedBuildLeaves, false) || leavesAndTestsNothing) && numberOfEdgesThatCameRound)
                isBuilt = false;
            if (std::exchange(m_typedBuildAskedNoListThatItWasBoughtFor, false))
                isBuilt = false;
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
        auto ownerOfMark = makeUniqueWithoutFastMallocCheck<SnapshotForTry>(*this);
        SnapshotForTry& behindPlainWalk = *ownerOfMark;
        auto undoTypedBuild = [&] {
            behindPlainWalk.putBack(*this);
            for (uint32_t value = firstValue; value < endOfValues; ++value)
                m_frames[frameIndex].references[value] = boundByPlainWalk[value - firstValue];
        };
        BlockID entryOfPlainWalk;
        BlockID startOfPlainWalk;
        Vector<uint32_t, 8> edgesThatLeavePlainWalk;
        auto standBesidePlainWalk = [&] {
            Frame& frameOfLoop = m_frames[frameIndex];
            m_origin = snapshot.m_origin;
            frameOfLoop.block = snapshot.block;
            frameOfLoop.hasHandler = snapshot.hasHandler;
            frameOfLoop.standsInFrontOfHead = snapshot.standsInFrontOfHead;
            frameOfLoop.narrowedValues = snapshot.narrowedValues;
            frameOfLoop.firstNarrowerName = snapshot.firstNarrowerName;
            frameOfLoop.listOfNarrowerNamesIsLong = snapshot.listOfNarrowerNamesIsLong;
            frameOfLoop.loopLookedAt = snapshot.loopLookedAt;
            frameOfLoop.firstValueLookedAt = snapshot.firstValueLookedAt;
            frameOfLoop.typesOfLooks = snapshot.typesOfLooks;
            frameOfLoop.wholeExpectations = snapshot.wholeExpectations;
            for (size_t logged = m_expectationsInTries.size(); logged-- > snapshot.numberOfExpectationsLogged;) {
                ExpectationInTry written = m_expectationsInTries[logged];
                if (written.frame >= m_frames.size() || m_frames[written.frame].expectations.isEmpty() || m_inputs.options.undoLeavesExpectations)
                    continue;
                m_expectationsInTries.append(ExpectationInTry { written.frame, written.value, m_frames[written.frame].expectations[written.value.index()] });
                m_frames[written.frame].expectations[written.value.index()] = written.before;
            }
            edgesThatLeavePlainWalk = nameValuesOnEdgesThatLeavePlainWalk(inputHead, readBehindLoop.span());
            BodyBlock head = index.block(inputHead);
            for (uint32_t parameter = 0; parameter < head.numberOfParameters(); ++parameter)
                m_frames[frameIndex].references[head.parameter(parameter).index()] = enteringKept[parameter];
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
            crossEdge({ }, front.head, enteringPlainWalk.span());
            m_block = front.block;
            m_state = *tried.stateInFront;
            m_closedCopiesShared.append(ClosedCopyShared { frameIndex, inputHead, start, entry, true, true });
            m_buildsTestsBesidePlainWalk = true;
        };
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
            m_firstOperationThatRunsOtherCodeOfRow = bought->firstOperationThatRanOtherCode;
            m_firstOtherCodeRanInCopyOfRow = bought->firstOtherCodeRanInCopy;
            m_operationsThatRanOtherCodeOfRow = bought->operationsThatRanOtherCode;
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
        bool assumedTop = !m_answersOfLoops[place].knowsEffects && !m_answersOfLoops[place].claimsNothing;
        m_triesOfLoops.append(TryOfLoop { frameIndex, inputHead, place, Effects::none(), false });
        if (frameIndex < m_operationsBeingWalkedInFrames.size())
            m_operationsBeingWalkedInFrames[frameIndex] = { };
        m_triesOfLoops.last().firstRecord = m_code.numberOfValues();
        m_triesOfLoops.last().numberOfHeadParametersWithSourcesInFront = m_headParametersWithSources.size();
        noteHeadParametersThatEnter(m_triesOfLoops.last(), enteringKept.span());
        ++m_numberOfWalksOfLoops;
        bool mayBeBoughtBehindWalk = !hasAskedForTwoBuilds && !m_answersOfLoops[place].severalBuildsAreStruck && !m_answersOfLoops[place].claimsNothing && mayBeBuiltSeveralTimes && !(m_answersOfLoops[place].whatTurnsShowed & wasWalkedAgainForFront);
        m_triesOfLoops.last().wantsStateInFront = mayBeBoughtBehindWalk;
        m_leavesFrontOfLoopOpenBehindWalk = mayBeBoughtBehindWalk;
        auto typesBeforeWalk = m_answersOfLoops[place].types;
        auto rungsBeforeWalk = m_answersOfLoops[place].rungs;
        walkCopyOfLoop(inputHead, enteringKept.span());
        bool walkBeganTexts = m_walkOfLoopBeganTexts;
        auto ownerOfTried = makeUniqueWithoutFastMallocCheck<TryOfLoop>(m_triesOfLoops.takeLast());
        TryOfLoop& tried = *ownerOfTried;
        if (!m_triesOfLoops.isEmpty()) {
            TryOfLoop& around = m_triesOfLoops.last();
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
            return !m_triesOfLoops.isEmpty() && !m_answersOfLoops[m_triesOfLoops.last().answer].hasFittedOnce;
        };
        ++answer.numberOfTries;

        bool effectsFit = assumed.mayRunOtherCode()
            ? !tried.done.maySuspend || assumed.maySuspend
            : !tried.done.mayRunOtherCode() && tried.done.writes.without(heapsNotHeldAgainstRowsOfLoops).isSubsetOf(assumed.writes);
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
            if (std::ranges::is_sorted(index.membersOf(loop), { }, [](BlockID member) { return member.index(); })) {
                for (BlockID member : index.membersOf(loop))
                    answer.typesOfValues.appendRun(index.block(member).firstParameter().index() - firstValue, index.block(member).endOfValues().index() - index.block(member).firstParameter().index());
            } else {
                Vector<BlockID, 16> membersInOrder(index.membersOf(loop));
                std::ranges::sort(membersInOrder, { }, [](BlockID member) { return member.index(); });
                for (BlockID member : membersInOrder)
                    answer.typesOfValues.appendRun(index.block(member).firstParameter().index() - firstValue, index.block(member).endOfValues().index() - index.block(member).firstParameter().index());
            }
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
                for (const TryOfLoop::TypeLeftByTurnsWrittenOut& left : tried.typesLeftByTurnsWrittenOut) {
                    TypeID* typeInRow = left.value >= firstValue ? answer.typesOfValues.slotAt(left.value - firstValue) : nullptr;
                    if (typeInRow)
                        *typeInRow = *typeInRow ? m_types.unionOf(*typeInRow, left.type) : left.type;
                }
            };
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
                    for (size_t position = 0; position < typesInside.size(); ++position) {
                        if (!typesInRow[position])
                            typesInRow[position] = typesInside[position];
                    }
                });
            }
            for (BlockID member : index.membersOf(loop)) {
                if (index.loopOf(member) == loop)
                    noteTypesOfValuesOf(member);
            }
            noteTypesLeftByTurnsWrittenOut();
            for (size_t parameter = 0; parameter < answer.types.size() && parameter < index.block(inputHead).numberOfParameters(); ++parameter) {
                if (TypeID* typeInRow = answer.typesOfValues.slotAt(parameter); typeInRow && !*typeInRow)
                    *typeInRow = answer.types[parameter];
            }
            answer.typesOfValues.forEachRun([&](uint32_t first, std::span<const TypeID> typesInRun) {
                for (size_t position = 0; position < typesInRun.size(); ++position) {
                    ValueID value(firstValue + first + static_cast<uint32_t>(position));
                    if (typesInRun[position] || index.isParameter(value) || index.opcodeOf(value) != Opcode::Add)
                        continue;
                    BodyValue sum = index.value(value);
                    auto typeOfOperand = [&](ValueID operand) -> TypeID {
                        if (operand.index() >= firstValue && operand.index() - firstValue < answer.typesOfValues.size())
                            return answer.typesOfValues[operand.index() - firstValue];
                        return typeOfRecordForLooks(*this, ownTypeOf(m_frames[frameIndex].references[operand.index()]));
                    };
                    TypeID left = typeOfOperand(sum.operand(0));
                    TypeID right = typeOfOperand(sum.operand(1));
                    if (left && right && hasTextByItself(m_types.at(left)) && hasTextByItself(m_types.at(right)) && (m_types.at(left).isAnyString() || m_types.at(right).isAnyString()))
                        *answer.typesOfValues.slotAt(first + position) = TypeTable::string;
                }
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
            std::optional<TypesByRecord> typesThatReach;
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
                        typesThatReach = typesThatReachHeadParametersOfNest(tried.firstRecord, m_headParametersWithSources.span().subspan(tried.numberOfHeadParametersWithSourcesInFront));
                    TypeID reaches = typesThatReach->get(m_code.at(headOfCode).parameters()[parameter].index());
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
        if (fits && !tried.numberOfEdgesThatCameRound && !m_waysIntoClosedCopies.isEmpty() && m_waysIntoClosedCopies.last().inputHead == inputHead && m_waysIntoClosedCopies.last().frameIndex == frameIndex)
            m_everyTurnOfTypedBuildLeaves = true;
        bool isQuietOutsideArms = !tried.testsSeen.isEmpty() && !tried.doneOutsideArms.mayRunOtherCode() && tried.done.mayRunOtherCode();
        bool sharedClosedCopyWaits = !m_closedCopiesShared.isEmpty() && m_closedCopiesShared.last().frameIndex == frameIndex && m_closedCopiesShared.last().inputHead == inputHead && m_closedCopiesShared.last().isAboutToBeTaken;
        if (fits && !hasAskedForTwoBuilds && !answer.severalBuildsAreStruck && !answer.claimsNothing && has(Mechanism::LoopInvariants) && (sharedClosedCopyWaits || !m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop)) && !walkAroundIsUndone() && !isTooLargeToBeBuiltTwice(index, loop)) {
            hasAskedForTwoBuilds = true;
            auto ownerOfLearned = makeUniqueWithoutFastMallocCheck<LoopPreview>();
            LoopPreview& learned = *ownerOfLearned;
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
                m_firstOperationThatRunsOtherCodeOfRow = tried.firstOperationThatRunsOtherCode;
                m_firstOtherCodeRanInCopyOfRow = tried.firstOtherCodeRanInCopy;
                m_operationsThatRanOtherCodeOfRow = tried.operationsThatRanOtherCode;
                BodyBlock head = index.block(inputHead);
                Vector<Reference, 8> boundBehindWalk;
                for (uint32_t parameter = 0; parameter < head.numberOfParameters() && parameter < enteringKept.size(); ++parameter) {
                    boundBehindWalk.append(m_frames[frameIndex].references[head.parameter(parameter).index()]);
                    m_frames[frameIndex].references[head.parameter(parameter).index()] = enteringKept[parameter];
                }
                SetForScope asksOnly(m_asksOnlyWhetherLoopMayBeBuiltTwice, true);
                SetForScope listsStruckWhileAsking(m_listsAreStruckInRow, answer.listsAreStruck);
                auto ownerOfStateBehindWalk = makeUniqueWithoutFastMallocCheck<WalkState>(m_state);
                WalkState& stateBehindWalk = *ownerOfStateBehindWalk;
                BlockID blockBehindWalk = m_block;
                Frame& frameOfLoop = m_frames[frameIndex];
                auto blockOfFrameBehindWalk = frameOfLoop.block;
                bool hasHandlerBehindWalk = frameOfLoop.hasHandler;
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
                    for (BlockID member : index.membersOf(loop)) {
                        for (BodyValue operation : index.block(member).values()) {
                            forEachTypeThatGuessWouldProveAt(*this, operation, TypesOfInputs(typeInRow), [&](ValueID operand, TypeID expected, TypeID) {
                                if (!m_testsSeenOfRow.containsIf([&](const GuessCountedOnByLooks& seen) { return seen.operand == operand; }))
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
                m_state = stateBehindWalk;
                m_block = blockBehindWalk;
                Frame& frameBehindAsking = m_frames[frameIndex];
                frameBehindAsking.block = blockOfFrameBehindWalk;
                frameBehindAsking.hasHandler = hasHandlerBehindWalk;
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
                    m_frames[frameIndex].references[head.parameter(parameter).index()] = boundBehindWalk[parameter];
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
                bool keepsPlainWalk = m_frontOfLoopLeftOpen.block && tried.stateInFront && tried.freshObjectsKept.isEmpty() && m_otherCopyOpensItsGuessesAsAsked && !leavesURLUnmade
                    && !sharedClosedCopyWaits && !hasTextsInStringBuilders() && !walkBeganTexts && steppedInFrontOfLoop.asTheyEntered.isEmpty()
                    && !enteringKept.containsIf([&](Reference enters) { return isTextInStringBuilder(*this, enters); })
                    && !m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead))
                    && m_code.at(m_frontOfLoopLeftOpen.head).parameters().size() == enteringKept.size() && m_frontOfLoopLeftOpen.block.index() < m_frontOfLoopLeftOpen.head.index();
                bool isBuilt;
                bool textsStayedOutOfBuilders = textsOfLoopStayOutOfBuilders(inputHead);
                if (keepsPlainWalk)
                    isBuilt = makeTypedBuildBesidePlainWalk(learned, tried);
                else {
                    undoPlainWalk();
                    isBuilt = makeBuilds(learned, tried.entered, tried.numberOfEdgesThatCameRound, undoPlainWalk);
                }
                if (isBuilt && !textsStayedOutOfBuilders && textsOfLoopStayOutOfBuilders(inputHead) && !m_numberOfTryWhoseWalkIsCutShort) {
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
                    bought->firstOperationThatRanOtherCode = tried.firstOperationThatRunsOtherCode;
                    bought->firstOtherCodeRanInCopy = tried.firstOtherCodeRanInCopy;
                    bought->operationsThatRanOtherCode = tried.operationsThatRanOtherCode;
                    bought->numberOfEdgesThatCameRound = tried.numberOfEdgesThatCameRound;
                    bought->keepsPlainWalk = keepsPlainWalk;
                    m_answersOfLoops[place].bought = WTF::move(bought);
                    if (keepsPlainWalk && tried.rowAsAssumed)
                        m_answersOfLoops[place].asWalkedLast = tried.rowAsAssumed;
                    return true;
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
        if (!fits && m_isWalkingLastAlone) {
            fail("a walk of a loop does not fit the row that its kept walk was walked under"_s);
            return true;
        }
        if (fits) {
            m_effectsOfLoopKeptLast = tried.done;
            m_walkOfLoopWasKept = true;
            m_headOfLoopKeptLast = inputHead;
            m_frameOfLoopKeptLast = frameIndex;
            if (tried.rowAsAssumed)
                m_answersOfLoops[place].asWalkedLast = tried.rowAsAssumed;
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
