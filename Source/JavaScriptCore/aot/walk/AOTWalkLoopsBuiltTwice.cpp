/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinPristine.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTOperandKinds.h"
#include "AOTSumOverRecords.h"
#include "AOTWalkBirths.h"
#include "AOTWalkBlocksThatEveryTurnPasses.h"
#include "AOTWalkCountersOfLoops.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkLoopsOverGenerators.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkStrings.h"
#include "AOTWalkTextBuilders.h"
#include <algorithm>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t maxVersionedLoopSize = 100;

static bool isTypeOfLink(const TypeArena& types, TypeID held)
{
    Type type = types.at(held);
    return type.hasObjects() && type.hasOnlyLayouts() && !type.hasOnlyClosures() && !type.primitives;
}

static Reference buildLoopCarriedCheck(Walker& walker, Reference value, TypeID tested, Origin origin)
{
    bool asksUpperEndAlone = value.isValue() && tested != TypeTable::int32 && walker.representationOf(value) == Representation::Int64 && walker.types().at(tested).isInteger()
        && walker.types().at(walker.typeOf(value)).isInteger() && walker.types().at(walker.typeOf(value)).integers.minimum >= walker.types().at(tested).integers.minimum;
    if (asksUpperEndAlone)
        return buildOperatorInNumberForms(walker, Opcode::LessEq, value, integerInNumberForms(walker, walker.types().at(tested).integers.maximum), origin);
    if (!isTypeOfLink(walker.types(), tested))
        return buildTestOfNumber(walker, value, tested, origin);
    Draft shape(Opcode::ShapeTest, origin);
    shape.setObjectOperand(value);
    shape.setLayouts(walker.types().at(tested).layouts);
    shape.setRejectsMarkedObjects(!walker.shouldAllowMarkedObjects());
    return walker.build(WTF::move(shape));
}

bool Walker::exitsLoopBeingVersioned(size_t frameIndex, BlockID to) const
{
    if (!m_generatorsWalkedInConsumers.isEmpty() && leavesRegionOfIteration(frameIndex, to))
        return true;
    const Frame& frame = m_frames[frameIndex];
    auto isLeft = [&](LoopID loop) {
        return loop.index() < frame.index->numberOfLoops() && !frame.index->isInLoop(to, loop);
    };
    if (std::ranges::any_of(frame.loopsBeingVersioned, isLeft))
        return true;
    return std::ranges::any_of(m_reversionableLoops, [&](const std::pair<size_t, LoopID>& walked) {
        return walked.first == frameIndex && isLeft(walked.second);
    });
}

void Walker::takeNamesOntoEdge(OpenEdge& edge, size_t frameIndex)
{
    const Frame& frame = m_frames[frameIndex];
    if (!frame.duplicatedValues)
        return;
    edge.names = frame.duplicatedValues->namesOfWay;
    for (ValueID value : frame.duplicatedValues->boundOnWay) {
        if (Reference name = nameBehindStandIn(frameIndex, value, frame.references[value.index()]); name && frame.duplicatedValues->contains(value))
            edge.names.setNameIn(frame.duplicatedValues->slotOf(value), name);
    }
}

void Walker::takeNamesOfWay(BlockID inputBlock, NamesOfWay&& names)
{
    Frame& frame = m_frames.last();
    frame.duplicatedValues->namesOfWay = WTF::move(names);
    frame.duplicatedValues->blockThatWayEnters = inputBlock;
    for (uint32_t carried = frame.index->numberOfValues(); carried < frame.references.size(); ++carried) {
        if (Reference name = frame.duplicatedValues->nameOn(frame.duplicatedValues->namesOfWay, ValueID(carried)))
            bind(ValueID(carried), name);
    }
    frame.duplicatedValues->namesGivenForGood.removeAllMatching([&](const Frame::DuplicatedValues::NamesGivenForGood& given) {
        if (given.block != inputBlock)
            return false;
        for (ValueID value : given.values) {
            Reference name = referenceBoundIn(frame, value);
            frame.duplicatedValues->remove(value);
            if (name)
                bind(value, name);
        }
        return true;
    });
}

void Walker::forgetNamesOfWay(Frame& frame)
{
    Frame::DuplicatedValues& listed = *frame.duplicatedValues;
    for (ValueID value : listed.boundOnWay) {
        if (const Frame::DuplicatedValues::Slot& slot = listed.slots[listed.slotOf(value)]; slot.ordinal && slot.hasSeveralNames && value.index() < frame.index->numberOfValues())
            frame.references.placeOf<NamesOfValues::Writer::ForgetsNamesOfWay>(value.index()) = Reference();
    }
    listed.boundOnWay.shrink(0);
    listed.namesOfWay.clear();
    listed.blockThatWayEnters = { };
}

bool Walker::isNamedByWaysInto(const Frame& frame, ValueID value, BlockID inputBlock) const
{
    if (value == frame.stepWhosePositionEdgesBring)
        return true;
    if (!frame.isDuplicated(value))
        return false;
    if (value.index() >= frame.index->numberOfValues())
        return true;
    if (!m_liveness.isComputedFor(*frame.index))
        m_liveness.compute(*frame.index);
    return m_liveness.isLiveAtHead(value, inputBlock);
}

Reference Walker::nameBroughtByWay(const Frame& frame, ValueID value) const
{
    Reference name = frame.duplicatedValues->nameOn(frame.duplicatedValues->namesOfWay, value);
    return name && isNamedByWaysInto(frame, value, frame.duplicatedValues->blockThatWayEnters) ? name : Reference();
}

void Walker::walkCopyOfLoop(BlockID inputHead, std::span<const Reference> entering)
{
    SetForScope deeper(m_nestingDepth, m_nestingDepth + 1);
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);

    bool leavesFrontOpen = std::exchange(m_leavesFrontOfLoopOpenBehindWalk, false);
    size_t numberOfTextsInFront = m_stringsInBuilders.size();
    m_walkOfLoopBeganTexts = false;
    uint32_t namesAtHead = inputBlock(frameIndex, inputHead).firstNarrowerName;
    Vector<LoopID, 8> touched;
    if (m_frames[frameIndex].loopsWithBlocksTouched.get(loop.index()))
        touched.append(loop);
    while (!touched.isEmpty()) {
        LoopID holder = touched.takeLast();
        forEachBlockAndLoopRightIn(index, holder, [&](BlockID member) {
            if (!m_frames[frameIndex].blocks[member.index()].isAsNew())
                inputBlock(frameIndex, member) = InputBlock { };
        }, [&](LoopID inside) {
            if (m_frames[frameIndex].loopsWithBlocksTouched.get(inside.index()))
                touched.append(inside);
            return true;
        });
        m_frames[frameIndex].loopsWithBlocksTouched.clear(holder.index());
    }
    inputBlock(frameIndex, inputHead).firstNarrowerName = namesAtHead;

    BlockID from = m_block;
    m_code.end(from, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
    uint32_t place = addOpenEdge(frameIndex, inputHead);
    if (place == noOpenEdge)
        return;
    OpenEdge& open = m_openEdges[place];
    open.edge = m_code.at(from).terminator.next();
    for (Reference argument : entering) {
        open.arguments.append(argument);
        open.types.append(ownTypeOf(argument));
    }
    open.state = WTF::move(m_state);
    takeNamesOntoEdge(open, frameIndex);
    m_block = { };
    NamesOfWay bindingsBeforeLoop;
    if (m_frames[frameIndex].duplicatedValues)
        bindingsBeforeLoop = m_frames[frameIndex].duplicatedValues->namesOfWay;

    size_t numberOfLoopsAround = m_openLoops.size();
    forEachBlockAndLoopRightIn(index, loop, [&](BlockID member) {
        if (m_hasFailed || (index.loopOf(member) != loop && m_frames[frameIndex].blocks[member.index()].firstOpenEdge == noOpenEdge))
            return;
        walkBlock(member);
    }, [&](LoopID inside) {
        if (m_hasFailed || m_frames[frameIndex].blocks[index.loop(inside).head.index()].firstOpenEdge == noOpenEdge)
            return false;
        m_loopThatItsTryHasWalked = { };
        walkBlock(index.loop(inside).head);
        if (m_loopThatItsTryHasWalked == std::pair { frameIndex, inside })
            return true;
        for (BlockID member : index.membersOf(inside).subspan(1)) {
            if (!m_hasFailed && m_frames[frameIndex].blocks[member.index()].firstOpenEdge != noOpenEdge)
                walkBlock(member);
        }
        return true;
    });
    m_frontOfLoopLeftOpen = { };
    if (leavesFrontOpen && !m_hasFailed && m_openLoops.size() > numberOfLoopsAround) {
        endBlocksInFrontOfLoopsFrom(numberOfLoopsAround + 1);
        const OpenLoop& walked = m_openLoops.last();
        if (walked.frame == frameIndex && walked.loop == loop && walked.preheaderBlock && !m_code.at(walked.preheaderBlock).hasEnded()) {
            m_frontOfLoopLeftOpen = FrontOfLoopLeftOpen { walked.preheaderBlock, closeFrontOfQuietTurns(walked), walked.entering, walked.origin, walked.builtInFront };
            closeNewestLoop();
        }
    }
    endBlocksInFrontOfLoopsFrom(numberOfLoopsAround);
    m_walkOfLoopBeganTexts = m_stringsInBuilders.size() > numberOfTextsInFront;
    if (!m_hasFailed)
        endStringsInBuildersFrom(numberOfTextsInFront);
    walkCleanupsBesideCopyOfLoop(inputHead);
    m_block = { };
    m_state.clear();
    if (m_frames[frameIndex].duplicatedValues) {
        forgetNamesOfWay(m_frames[frameIndex]);
        m_frames[frameIndex].duplicatedValues->namesOfWay = WTF::move(bindingsBeforeLoop);
        m_frames[frameIndex].duplicatedValues->blockThatWayEnters = inputHead;
    }
}

const Walker::LoopReentry* Walker::findReentry(size_t frameIndex, BlockID inputHead) const
{
    size_t place = m_loopReentries.findIf([&](const LoopReentry& reentry) { return reentry.frameIndex == frameIndex && reentry.inputHead == inputHead; });
    return place == notFound ? nullptr : &m_loopReentries[place];
}

bool Walker::walkSingleIteration(BlockID inputHead, std::span<const Reference> entering, BlockID reentry)
{
    size_t frameIndex = m_frames.size() - 1;
    BodyBlock head = m_frames[frameIndex].index->block(inputHead);
    m_frames[frameIndex].isInPreheader = false;
    NamesOfWay namesBeforeLoop;
    if (m_frames[frameIndex].duplicatedValues)
        namesBeforeLoop = m_frames[frameIndex].duplicatedValues->namesOfWay;
    BlockID headerAround = std::exchange(m_frames[frameIndex].fullyUnrolledHeader, inputHead);
    walkCopyOfLoop(inputHead, entering);
    m_frames[frameIndex].fullyUnrolledHeader = headerAround;
    if (m_hasFailed || inputBlock(frameIndex, inputHead).firstOpenEdge == noOpenEdge)
        return false;
    switchToInputBlock(inputHead);
    Vector<uint32_t, 4> edges = takeOpenEdges(inputHead);
    closeEdgesInto(addBlock(BlockFrequency::Rare), inputHead, edges.span(), false);
    bool reenters = false;
    if (!m_hasFailed && !isUnreachable()) {
        Vector<Reference, 8> comingRound;
        for (uint32_t parameter = 0; parameter < head.numberOfParameters(); ++parameter)
            comingRound.append(referenceOf(head.parameter(parameter)));
        for (Reference argument : comingRound)
            letLeave(argument);
        reenters = canEnterWith(reentry, comingRound.span());
        if (reenters)
            enterWithFieldsOfScopes(reentry, comingRound.span());
        else
            endAsUnreachable(m_origin);
    }
    m_block = { };
    m_state.clear();
    if (m_frames[frameIndex].duplicatedValues) {
        forgetNamesOfWay(m_frames[frameIndex]);
        m_frames[frameIndex].duplicatedValues->namesOfWay = WTF::move(namesBeforeLoop);
        m_frames[frameIndex].duplicatedValues->blockThatWayEnters = inputHead;
    }
    return reenters;
}

ValueID Walker::findReadAskedAtHeadOfLoop(BlockID inputHead, std::span<const std::pair<Reference, TypeID>> tested)
{
    const BodyIndex& index = currentBodyIndex();
    BodyBlock head = index.block(inputHead);
    if (!has(Mechanism::TypedOperations) || !has(Mechanism::EscapeAnalysis) || hasStringsInBuilders() || m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames.last().inlineFrame, inputHead)))
        return { };
    for (BodyValue written : head.values()) {
        if (written.opcode() == Opcode::LoopHint)
            continue;
        if (written.opcode() != Opcode::PropGet || written.isFreeName())
            return { };
        ValueID input = written.objectOperand();
        Reference object = input && input.index() < head.firstParameter().index() ? referenceOf(input) : Reference();
        bool lengthIsFoundByNote = object && object.isValue() && program().nameClassOf(written.key()) == NameClass::Length && findKnown(*this, KnownNote::LengthOfViewIsFoundAbove, object) && loopTakesInstancesBelowViewForItsLength(*this, inputHead, object);
        if (!object || !object.isValue() || lengthIsFoundByNote || isKnownPristine(*this, object))
            return { };
        TypeID type = typeOf(object);
        for (auto [value, typeTested] : tested) {
            if (value == object && typeTested)
                type = m_types.intersectionOf(type, typeTested);
        }
        auto typeOfInput = [&](ValueID asked) {
            return asked == input ? type : TypeID();
        };
        return type && typedFormOf(*this, written, TypesOfInputs(typeOfInput)) == TypedForm::BuiltOnObjectAsMade ? written.id() : ValueID();
    }
    return { };
}

bool Walker::tryGuardObjectAtHeader(const BodyValue& read)
{
    const VersionedLoop versionedLoop = m_versionedLoops.last();
    size_t frameIndex = m_frames.size() - 1;
    if (versionedLoop.frameIndex != frameIndex || m_frames[frameIndex].block != versionedLoop.inputHead || isUnreachable() || hasStringsInBuilders())
        return false;
    if (findReadAskedAtHeadOfLoop(versionedLoop.inputHead, { }) != read.id())
        return false;

    BodyBlock head = currentBodyIndex().block(versionedLoop.inputHead);
    Vector<Reference, 8> held;
    for (uint32_t index = 0; index < head.numberOfParameters(); ++index)
        held.append(referenceOf(head.parameter(index)));
    if (loopCarriesValuesOfConsumerRound(versionedLoop.inputHead))
        held.appendVector(carriedValuesInFrame(frameIndex));
    BlockID specializedHeader = inputBlock(frameIndex, versionedLoop.inputHead).head;
    appendHeldSteps(*this, specializedHeader, held);
    if (!canEnterGenericVersion(specializedHeader, held.span()))
        return false;

    Reference object = referenceOf(read.objectOperand());
    bool takesInstancesBelow = read.key() == program().keyOf(BuiltinNames::length) && loopTakesInstancesBelowViewForItsLength(*this, versionedLoop.inputHead, object);
    Reference test = takesInstancesBelow ? buildTestOfViewOrInstanceBelow(*this, object, originOf(read)) : buildTestOfObjectAsMade(*this, object, originOf(read));
    if (!test || m_hasFailed)
        return false;
    BlockID passed = addBlock();
    BlockID failed = addBlock(BlockFrequency::Rare);
    letGuessFailUnasked(failed, originOf(read));
    branch(test, passed, failed);
    continueIn(failed);
    jumpToGenericVersion(specializedHeader, held.span());
    continueIn(passed);
    if (m_hasFailed || isUnreachable())
        return false;
    if (takesInstancesBelow)
        noteThatLengthOfViewIsFoundAbove(*this, object);
    else if (m_types.at(typeOf(object)).hasObjects())
        markObjectPristine(*this, object);
    noteThatViewKeepsItsLength(*this, object, test);
    noteObjectPassedTestOfBeingAsMade(*this, object, test);
    recordSpeculation();
    return true;
}

std::optional<HoistedArrayGuard> Walker::findListThatHeadReadsUnderKeyFromInFront(BlockID inputHead, std::span<const HoistedArrayGuard> lists)
{
    const BodyIndex& index = currentBodyIndex();
    BodyBlock head = index.block(inputHead);
    uint32_t firstValueOfLoop = head.firstParameter().index();
    for (BodyValue written : head.values()) {
        if (written.opcode() == Opcode::LoopHint || written.opcode() == Opcode::Integer)
            continue;
        if (written.opcode() != Opcode::ElemGet)
            return std::nullopt;
        ValueID key = index.stripCasts(written.propertyOperand());
        bool keyStandsInFront = key && (key.index() < firstValueOfLoop || (!index.isParameter(key) && index.opcodeOf(key) == Opcode::Integer));
        Reference named = written.objectOperand().index() < firstValueOfLoop ? referenceOf(written.objectOperand()) : Reference();
        if (!keyStandsInFront || !named || !named.isValue())
            return std::nullopt;
        for (const HoistedArrayGuard& list : lists) {
            if (list.name != named || list.written)
                continue;
            HoistedArrayGuard asked = list;
            if (asked.admitsInt32Storage) {
                TypeID seenByLink = facts().linked().resultOfReadByKey(currentFunction(), written.id());
                if (!seenByLink || !m_types.at(m_types.withoutNullish(seenByLink)).isNumberWithoutFraction())
                    return std::nullopt;
                asked.storage = ArrayStorage::Int32;
                asked.admitsInt32Storage = false;
            }
            ValueID listAsWritten = index.stripCasts(written.objectOperand());
            bool turnsStoreIntoIt = std::ranges::any_of(index.membersOf(index.loopOf(inputHead)), [&](BlockID member) {
                return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
                    return operation.opcode() == Opcode::ElemSet && index.stripCasts(operation.objectOperand()) == listAsWritten;
                });
            });
            asked.isWritable |= turnsStoreIntoIt;
            return hasArrayStorageForm(asked.storage) ? std::optional(asked) : std::nullopt;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<PassedTest> Walker::tryGuardArrayAtHeader(const BodyValue& read)
{
    if (m_versionedLoops.isEmpty() || !m_versionedLoops.last().arrayGuardedAtHeader)
        return std::nullopt;
    const VersionedLoop& last = m_versionedLoops.last();
    struct {
        size_t frameIndex;
        BlockID inputHead;
        Reference arrayGuardedAtHeader;
        ArrayStorage storageGuardedAtHeader;
        bool isArrayGuardedAtHeaderWritable;
    } versionedLoop { last.frameIndex, last.inputHead, last.arrayGuardedAtHeader, last.storageGuardedAtHeader, last.isArrayGuardedAtHeaderWritable };
    size_t frameIndex = m_frames.size() - 1;
    if (versionedLoop.frameIndex != frameIndex || m_frames[frameIndex].block != versionedLoop.inputHead || isUnreachable() || hasStringsInBuilders() || !isIterationEffectFree())
        return std::nullopt;
    Reference position = referenceOf(read.propertyOperand());
    if (referenceOf(read.objectOperand()) != versionedLoop.arrayGuardedAtHeader || findHoistedArrayGuard(*this, versionedLoop.arrayGuardedAtHeader) || !position || !m_types.at(typeOf(position)).isInteger())
        return std::nullopt;

    BodyBlock head = currentBodyIndex().block(versionedLoop.inputHead);
    Vector<Reference, 8> held;
    for (uint32_t index = 0; index < head.numberOfParameters(); ++index)
        held.append(referenceOf(head.parameter(index)));
    if (loopCarriesValuesOfConsumerRound(versionedLoop.inputHead))
        held.appendVector(carriedValuesInFrame(frameIndex));
    BlockID specializedHeader = inputBlock(frameIndex, versionedLoop.inputHead).head;
    appendHeldSteps(*this, specializedHeader, held);
    if (!canEnterGenericVersion(specializedHeader, held.span()))
        return std::nullopt;

    Origin origin = originOf(read);
    origin.isTailCall = false;
    for (bool asksForKind : { false, true }) {
        Reference test = asksForKind ? askWhetherStorageOfListIsOfKind(*this, versionedLoop.arrayGuardedAtHeader, versionedLoop.storageGuardedAtHeader, origin, versionedLoop.isArrayGuardedAtHeaderWritable) : askWhetherListIsAsMade(*this, versionedLoop.arrayGuardedAtHeader, origin);
        if (!test || m_hasFailed)
            return std::nullopt;
        if (!test.isValue())
            continue;
        BlockID passed = addBlock();
        BlockID failed = addBlock(BlockFrequency::Rare);
        letGuessFailUnasked(failed, origin);
        branch(test, passed, failed);
        continueIn(failed);
        jumpToGenericVersion(specializedHeader, held.span());
        continueIn(passed);
        if (m_hasFailed || isUnreachable())
            return std::nullopt;
    }
    noteKindOfStorageThatTurnFound(*this, versionedLoop.arrayGuardedAtHeader, versionedLoop.storageGuardedAtHeader, versionedLoop.isArrayGuardedAtHeaderWritable);
    recordSpeculation();
    Draft isPresent(Opcode::IsElementPresent, origin);
    chooseConservativeFields(isPresent);
    isPresent.setArrayOperand(versionedLoop.arrayGuardedAtHeader);
    isPresent.setPositionOperand(position);
    isPresent.setArrayStorage(versionedLoop.storageGuardedAtHeader);
    isPresent.setKeyIsIntegral(true);
    Reference isThere = build(WTF::move(isPresent));
    if (!isThere || !isThere.isValue() || m_hasFailed)
        return std::nullopt;
    BlockID found = addBlock();
    BlockID absent = addBlock(BlockFrequency::Rare);
    letGuessFailUnasked(absent, origin);
    branchOnTest(isThere, found, absent);
    continueIn(absent);
    jumpToGenericVersion(specializedHeader, held.span());
    continueIn(found);
    if (m_hasFailed || isUnreachable())
        return std::nullopt;
    count(StatisticID::ArraysGuardedAtLoopHeaders);
    return showElementPresentBehindTest(versionedLoop.arrayGuardedAtHeader, position, findPassedTest(isThere));
}

void Walker::markIterationRestartPoint(BlockID inputHead, const Effects& ofTurns)
{
    bool staysInOuterIteration = isIterationEffectFree() && m_versionedLoops.last().canUseIterationGuards && ofTurns.mayBeRunAgainUnseen() && m_versionedLoops.last().frameIndex == m_frames.size() - 1 && m_versionedLoops.last().inputHead != inputHead;
    if (!staysInOuterIteration)
        m_state.clearRestartMark();
    if (m_versionedLoops.isEmpty() || m_coldCopyDepth || isUnreachable())
        return;
    VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (versionedLoop.frameIndex != m_frames.size() - 1 || versionedLoop.inputHead != inputHead)
        return;
    versionedLoop.isLeftAtOtherCodeOnUsualWay = false;
    versionedLoop.isLeftAtAccessOnUsualWay = false;
    BodyBlock head = currentBodyIndex().block(inputHead);
    std::span<const ValueID> taken = m_code.at(versionedLoop.entry).parameters();
    versionedLoop.valuesAtHeader.shrink(0);
    versionedLoop.moduleVariablesAtHeader.shrink(0);
    versionedLoop.undoBlocks.shrink(0);
    m_undoableStoreStates.shrink(0);
    versionedLoop.effectsCheckBeginning = { };
    std::span<const ValueID> heldByHead = m_code.at(m_block).parameters();
    size_t numberTaken = taken.size() - numberOfFieldsOfScopesThatBlockTakes(versionedLoop.entry);
    if (numberTaken < head.numberOfParameters() || heldByHead.size() - numberOfFieldsOfScopesThatBlockTakes(m_block) != numberTaken)
        return;
    for (uint32_t index = 0; index < numberTaken; ++index) {
        Reference reference = index < head.numberOfParameters() ? referenceOf(head.parameter(index)) : Reference::value(heldByHead[index]);
        TypeID type = ownTypeOf(reference);
        if (!type || !m_types.isSubset(type, m_code.at(taken[index]).type)) {
            versionedLoop.valuesAtHeader.shrink(0);
            return;
        }
        versionedLoop.valuesAtHeader.append(reference);
    }
    versionedLoop.isEntered = true;
    versionedLoop.restartPoint = markRestartPoint();
    for (auto [position, held] : versionedLoop.backEdgeChecks) {
        if (!isTypeOfLink(m_types, held) || position >= head.numberOfParameters())
            continue;
        Reference link = referenceOf(head.parameter(position));
        if (TypeID type = ownTypeOf(link); type && m_types.isSubset(type, held))
            m_state.recordCellFacts(link, CellFact::BearsNoMark);
    }
    if (m_inputs.options.checksEffects) [[unlikely]]
        m_versionedLoops.last().effectsCheckBeginning = build(Draft(Opcode::BeginEffectsCheck, m_origin));
    if (m_versionedLoops.last().canUseIterationGuards)
        saveModuleVariablesAtHeader(inputHead);
}

void Walker::saveModuleVariablesAtHeader(BlockID inputHead)
{
    const BodyIndex& index = currentBodyIndex();
    bool turnsStoreElements = std::ranges::any_of(index.membersOf(index.loopOf(inputHead)), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [](BodyValue operation) {
            return operation.opcode() == Opcode::ElemSet;
        });
    });
    if (!turnsStoreElements)
        return;
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::PutModuleVariable)
                continue;
            VariableID variable = operation.variable();
            bool isHeld = m_versionedLoops.last().moduleVariablesAtHeader.containsIf([&](const auto& held) {
                return held.first == variable;
            });
            if (isHeld || plan().deferredDefinitions().findDeferred(variable) || plan().variable(variable).function)
                continue;
            Reference held = has(Mechanism::Folding) ? m_state.variable(variable) : Reference();
            if (!held) {
                Draft read(Opcode::GetModuleVariable, m_origin);
                read.setVariable(variable);
                chooseConservativeFields(read);
                if (has(Mechanism::Folding) && m_state.isPastDeadZone(variable))
                    read.setIsKnownInitialized(true);
                held = build(WTF::move(read));
                if (m_hasFailed || isUnreachable())
                    return;
                if (held)
                    m_state.noteVariable(variable, held);
            }
            TypeID type = held ? typeOf(held) : TypeID();
            if (type && !(m_types.at(type).primitives & Primitive::Absent))
                m_versionedLoops.last().moduleVariablesAtHeader.append({ variable, held });
        }
    }
}

bool Walker::isVariableStoreUndoable(VariableID variable) const
{
    return canRestartIteration() && m_versionedLoops.last().moduleVariablesAtHeader.containsIf([&](const auto& held) {
        return held.first == variable;
    });
}

bool Walker::canGuardAfterStores() const
{
    return canRestartIteration() && m_versionedLoops.last().canUseIterationGuards;
}

bool Walker::askLesserBoundAt(const BodyValue& relation)
{
    VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (relation.id() != versionedLoop.relationThatAsksLesserBound || versionedLoop.frameIndex != m_frames.size() - 1)
        return false;
    versionedLoop.headAsksLesserBound = false;
    versionedLoop.headThatAsksLesserBound = { };
    versionedLoop.counterBelowRoom.isBelowBound = { };
    versionedLoop.counterBelowRoom.counter = { };
    if (!versionedLoop.lesserBound || !m_opensGuesses || m_frames.last().block != versionedLoop.inputHead || !canRestartIteration())
        return false;
    if (versionedLoop.viewWhoseLengthIsBound && !findKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, versionedLoop.viewWhoseLengthIsBound))
        return false;
    if (versionedLoop.variableThatIsBound && !shouldAssumeInvariantLoopBounds() && !findKnown(*this, KnownNote::VariableIsAsInFrontOfLoop, versionedLoop.readOfVariableThatIsBound, { }, versionedLoop.variableThatIsBound.index()))
        return false;
    Reference counter = referenceOf(relation.operand(0));
    if (!counter || !counter.isRecord() || !m_types.at(typeOf(counter)).isInteger())
        return false;
    if (shouldFailSpeculations()) [[unlikely]] {
        BlockID handsOver;
        letGuessFailUnasked(handsOver, originOf(relation));
        BlockID goesOn = currentBlock();
        if (handsOver) {
            continueIn(handsOver);
            if (!isUnreachable())
                restartIteration();
            continueIn(goesOn);
        }
    }
    if (checksBounds() && !isUnreachable() && m_versionedLoops.last().variableThatIsBound) {
        FieldOfScope read;
        read.variable = m_versionedLoops.last().variableThatIsBound;
        Reference believed = m_versionedLoops.last().readOfVariableThatIsBound;
        TypeID type = typeOf(believed);
        if (type && m_types.at(type).isJavaScriptValue() && !(m_types.at(type).primitives & Primitive::Absent)) {
            WalkState atHead = m_state;
            checkThatMemoryAnswersAsBelieved(believed, draftVariableRead(read));
            m_state = WTF::move(atHead);
        } else
            ++m_untestableProofCount;
    }
    if (checksBounds() && !isUnreachable() && m_versionedLoops.last().boundBesideLesserBound) {
        Reference number = referenceOf(relation.operand(1));
        if (number && m_types.at(typeOf(number)).isNumber()) {
            WalkState atHead = m_state;
            Draft written(relation.opcode(), originOf(relation));
            chooseConservativeFields(written);
            written.setLeftOperand(counter);
            written.setRightOperand(number);
            Reference asWritten = buildTestOfCheck(WTF::move(written));
            checkThatValuesAreTheSame(asWritten, buildIntegerOperator(*this, relation.opcode(), counter, m_versionedLoops.last().boundBesideLesserBound, originOf(relation)), originOf(relation));
            m_state = WTF::move(atHead);
        } else
            ++m_untestableProofCount;
    }
    Reference test = buildIntegerOperator(*this, relation.opcode(), counter, m_versionedLoops.last().lesserBound, originOf(relation));
    if (!test || !test.isValue() || isUnreachable())
        return false;
    bind(relation.id(), test);
    m_versionedLoops.last().headAsksLesserBound = true;
    m_versionedLoops.last().headThatAsksLesserBound = inputBlock(m_frames.size() - 1, m_versionedLoops.last().inputHead).head;
    if (m_versionedLoops.last().variableThatIsBound)
        count(StatisticID::VariableLoopBoundsHoisted);
    m_versionedLoops.last().counterBelowRoom.isBelowBound = test.id();
    claimThatCounterNeverLiesBelowItsStart();
    recordSpeculation();
    if (m_versionedLoops.last().counterBelowRoom.start)
        count(StatisticID::LoopBoundsClampedToTypedArrays);
    return true;
}

void Walker::claimThatCounterNeverLiesBelowItsStart()
{
    VersionedLoop& versionedLoop = m_versionedLoops.last();
    VersionedLoop::CounterBelowRoomOfViews& held = versionedLoop.counterBelowRoom;
    BlockID head = inputBlock(versionedLoop.frameIndex, versionedLoop.inputHead).head;
    if (!held.start || !head || m_trialVisits.isEmpty() || m_openLoops.isEmpty() || m_openLoops.last().head != head)
        return;
    const TrialVisit& trialVisit = m_trialVisits.last();
    std::span<const ValueID> entering = m_openLoops.last().recordsEntering();
    if (trialVisit.frameIndex != versionedLoop.frameIndex || trialVisit.inputHead != versionedLoop.inputHead || m_loopSummaries[trialVisit.summaryIndex].counterWasSteppedDown || held.position >= entering.size() || held.position >= m_code.at(head).parameters().size())
        return;
    if (!everyWayStepsUpFrom(m_code, m_types, entering[held.position], held.start, { }, scopedLambda<bool(ValueID)>([](ValueID) { return false; }), scopedLambda<bool(BlockID)>([](BlockID) { return true; })))
        return;
    held.counter = m_code.at(head).parameters()[held.position];
    noteKnown(*this, KnownNote::NeverLiesBelow, Reference::value(held.counter), referenceTo(held.start), 0, LocationMasks(), 1);
}

bool Walker::counterComesRoundNotBelowItself(BlockID head, std::span<const Reference> arguments)
{
    for (const VersionedLoop& versionedLoop : m_versionedLoops) {
        const VersionedLoop::CounterBelowRoomOfViews& held = versionedLoop.counterBelowRoom;
        if (!held.counter || m_code.blockOf(held.counter) != head)
            continue;
        bool stepHolds = !held.stepNonNegativeCheck || findPassedTest(referenceTo(held.stepNonNegativeCheck));
        if (stepHolds && held.position < arguments.size() && everyWayStepsUpFrom(m_code, m_types, arguments[held.position].id(), held.counter, held.stepNonNegativeCheck, scopedLambda<bool(ValueID)>([&](ValueID each) {
            return m_code.at(each).opcode != Opcode::Parameter || !m_code.at(m_code.blockOf(each)).isLoopHead;
        }), scopedLambda<bool(BlockID)>([](BlockID) { return true; })))
            continue;
        for (size_t place = m_trialVisits.size(); place--;) {
            TrialVisit& trialVisit = m_trialVisits[place];
            if (trialVisit.frameIndex >= m_frames.size() || inputBlock(trialVisit.frameIndex, trialVisit.inputHead).head != head)
                continue;
            trialVisit.misfits = true;
            m_loopSummaries[trialVisit.summaryIndex].counterWasSteppedDown = true;
            return false;
        }
        fail("a counter comes round below itself outside a try"_s);
        return false;
    }
    return true;
}

static bool stepsDownFromCounter(const Code& code, const TypeArena& types, ValueID value, ValueID counter)
{
    return everyWayStepsDownFrom(code, types, value, counter, scopedLambda<bool(ValueID)>([&](ValueID each) {
        return code.at(each).opcode != Opcode::Parameter || !code.at(code.blockOf(each)).isLoopHead;
    }), scopedLambda<bool(BlockID)>([](BlockID) { return true; }));
}

void Walker::noteLoweredCounter(const BodyValue& access)
{
    const BodyIndex& index = currentBodyIndex();
    if (m_trialVisits.isEmpty() || m_openLoops.isEmpty() || isUnreachable() || access.id().index() >= index.numberOfValues() || index.value(access.id()).words() != access.words())
        return;
    ValueID written = findLoweredCounterUnder(*this, index, access.propertyOperand());
    Reference built = written ? referenceOf(written) : Reference();
    if (!built || !built.isValue())
        return;
    ValueID counter = built.id();
    while (cellPreservingOpcodes.contains(m_code.at(counter).opcode))
        counter = m_code.at(counter).operand(0);
    if (m_code.isConstant(counter) || m_code.at(counter).opcode != Opcode::Parameter || !m_types.at(m_code.at(counter).type).isInteger() || findKnown(*this, KnownNote::NeverAbove, Reference::value(counter)))
        return;
    BlockID head = m_code.blockOf(counter);
    const OpenLoop* loop = nullptr;
    for (const OpenLoop& each : m_openLoops) {
        if (each.head == head)
            loop = &each;
    }
    size_t tried = m_trialVisits.reverseFindIf([&](const TrialVisit& each) { return each.frameIndex < m_frames.size() && inputBlock(each.frameIndex, each.inputHead).head == head; });
    if (!loop || tried == notFound || m_loopSummaries[m_trialVisits[tried].summaryIndex].counterWasRaised)
        return;
    size_t place = m_code.at(head).placeOfParameter(counter);
    std::span<const ValueID> entering = loop->recordsEntering();
    if (place >= entering.size() || !m_types.at(m_code.at(entering[place]).type).isInteger())
        return;
    bool hasComeRoundLowered = std::ranges::all_of(loop->edgesHeldBack, [&](const OpenLoop::EdgeHeldBack& held) {
        std::span<const ValueID> arguments = held.arguments.span();
        return place < arguments.size() && stepsDownFromCounter(m_code, m_types, arguments[place], counter);
    });
    for (EdgeID edge = m_code.at(head).firstEdgeIn(); edge && hasComeRoundLowered; edge = m_code.at(edge).nextInto()) {
        std::span<const ValueID> arguments = m_code.at(edge).arguments();
        hasComeRoundLowered = place < arguments.size() && (arguments[place] == entering[place] || stepsDownFromCounter(m_code, m_types, arguments[place], counter));
    }
    if (!hasComeRoundLowered)
        return;
    if (!m_trialVisits[tried].loweredCounters.contains(counter))
        m_trialVisits[tried].loweredCounters.append(counter);
    noteKnown(*this, KnownNote::NeverAbove, Reference::value(counter), { }, 0, LocationMasks(), referenceTo(entering[place]).bits());
}

bool Walker::secondCountersComeRound(BlockID head, std::span<const Reference> arguments)
{
    for (TrialVisit& trialVisit : m_trialVisits) {
        if (trialVisit.secondCounters.isEmpty() || trialVisit.frameIndex >= m_frames.size() || inputBlock(trialVisit.frameIndex, trialVisit.inputHead).head != head)
            continue;
        auto recordAt = [&](size_t place) {
            return place < arguments.size() && arguments[place].isValue() ? arguments[place].id() : ValueID();
        };
        for (const SecondCounter& second : trialVisit.secondCounters) {
            if (stepsSecondCounter(m_code, m_types, second, recordAt(m_code.at(head).placeOfParameter(second.stepped)), recordAt(m_code.at(head).placeOfParameter(second.counter))))
                continue;
            trialVisit.misfits = true;
            m_loopSummaries[trialVisit.summaryIndex].secondCountersAreStruck = true;
            return false;
        }
    }
    return true;
}

std::optional<Range> Walker::stepsOfHeadParameter(BlockID inputHead, uint32_t position)
{
    uint64_t key = static_cast<uint64_t>(currentFunction().index()) << 32 | currentBodyIndex().block(inputHead).parameter(position).index();
    Range& steps = m_stepsOfHeadParameters.ensure(key, [&] {
        return findIterationSteps(*this, inputHead, position).value_or(Range::empty());
    }).iterator->value;
    return steps.isEmpty() ? std::nullopt : std::optional { steps };
}

bool Walker::narrowSecondCounters(EdgeID& edge, BlockID from, BlockID to, uint32_t targetIndex, Vector<Reference, 8>& arguments)
{
    if (m_refusesSecondCounters)
        return true;
    const Terminator& end = m_code.at(from).terminator;
    if (m_openLoops.isEmpty() || m_trialVisits.isEmpty() || !end.isConditionalBranch() || m_code.isConstant(end.conditionOperand()) || !reckonsOnWhatItHasBuilt())
        return true;
    ValueID testID = end.conditionOperand();
    const Operation& test = m_code.at(testID);
    const OpenLoop& loop = m_openLoops.last();
    if (!allRelations.contains(test.opcode) || test.form() != OperatorForm::Integers || loop.frame != m_frames.size() - 1 || !loop.edgesHeldBack.isEmpty())
        return true;
    std::span<const ValueID> parameters = m_code.at(loop.head).parameters();
    if (parameters.size() < 2)
        return true;
    size_t tried = m_trialVisits.reverseFindIf([&](const TrialVisit& each) { return each.frameIndex < m_frames.size() && inputBlock(each.frameIndex, each.inputHead).head == loop.head; });
    if (tried == notFound || m_loopSummaries[m_trialVisits[tried].summaryIndex].secondCountersAreStruck)
        return true;
    const BodyIndex& index = currentBodyIndex();
    BlockID inputHead = m_trialVisits[tried].inputHead;
    BodyBlock head = index.block(inputHead);
    bool isOnTakenEdge = !m_edgeOfCodeBeingLed.value_or(targetIndex);
    claimSecondCounters(tried, testID, isOnTakenEdge);
    auto fields = m_fieldsOfScopesOfHeads.find(loop.head.index());
    bool someSlotSteps = fields != m_fieldsOfScopesOfHeads.end() && std::ranges::any_of(fields->value, [](const FieldOfScope& field) { return !field.slotSteps.isEmpty(); });
    if (!someSlotSteps && head.numberOfParameters() < 2)
        return true;
    noteSteppedKeyLimits(tried, testID, isOnTakenEdge);
    for (uint32_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
        ValueID counter = underlyingCellOf(m_code, test.operand(operandIndex));
        if (!counter || m_code.isConstant(counter) || m_code.at(counter).opcode != Opcode::Parameter || m_code.blockOf(counter) != loop.head)
            continue;
        size_t placeOfCounter = m_code.at(loop.head).placeOfParameter(counter);
        std::optional<Range> counted = placeOfCounter < head.numberOfParameters() ? stepsOfHeadParameter(inputHead, static_cast<uint32_t>(placeOfCounter)) : std::nullopt;
        if (!counted || (counted->minimum <= 0 && counted->maximum >= 0))
            continue;
        for (uint32_t position = 0; position < head.numberOfParameters(); ++position) {
            ValueID stepped = m_code.at(loop.head).parameters()[position];
            Type type = m_types.at(m_code.at(stepped).type);
            if (position == placeOfCounter || !type.isInteger())
                continue;
            std::optional<Range> steps = stepsOfHeadParameter(inputHead, position);
            if (!steps) {
                if (std::optional<InvariantStep> invariant = findInvariantStep(*this, inputHead, position))
                    steps = invariant->steps;
            }
            if (!steps || (!steps->minimum && !steps->maximum))
                continue;
            steps = Range { std::min<int64_t>(steps->minimum, 0), std::max<int64_t>(steps->maximum, 0) };
            SecondCounter second { counter, stepped, *steps, counted->minimum > 0 };
            Reference name = referenceOf(head.parameter(position));
            if (!name || !name.isValue() || underlyingCellOf(m_code, name.id()) != stepped)
                continue;
            Reference built = buildSecondCounter(second, name, testID, operandIndex, isOnTakenEdge, edge, to);
            if (isUnreachable() && built)
                return false;
            if (!built)
                continue;
            for (Reference& argument : arguments) {
                if (argument == name)
                    argument = built;
            }
            noteNarrowerName(to, name, built);
            m_trialVisits[tried].secondCounters.append(second);
        }
        for (size_t place = 0; fields != m_fieldsOfScopesOfHeads.end() && place < fields->value.size(); ++place) {
            const FieldOfScope& field = fields->value[place];
            Reference parameter = field.parameter;
            if (field.slotSteps.isEmpty() || (!field.slotSteps.minimum && !field.slotSteps.maximum) || !parameter || !parameter.isValue() || valueKnownOf(field) != parameter || !m_types.at(m_code.at(parameter.id()).type).isInteger())
                continue;
            SecondCounter second { counter, parameter.id(), Range { std::min<int64_t>(field.slotSteps.minimum, 0), std::max<int64_t>(field.slotSteps.maximum, 0) }, counted->minimum > 0 };
            Reference built = buildSecondCounter(second, parameter, testID, operandIndex, isOnTakenEdge, edge, to);
            if (isUnreachable() && built)
                return false;
            if (!built)
                continue;
            renameCarriedFields(parameter, built);
            m_trialVisits[tried].secondCounters.append(second);
        }
    }
    return true;
}

void Walker::claimSecondCounters(size_t tried, ValueID test, bool isOnTakenEdge)
{
    if (m_versionedLoops.isEmpty())
        return;
    VersionedLoop& versionedLoop = m_versionedLoops.last();
    TrialVisit& trialVisit = m_trialVisits[tried];
    versionedLoop.secondCounterTest = { };
    if (versionedLoop.secondCounterGuards.isEmpty() || versionedLoop.frameIndex != m_frames.size() - 1 || versionedLoop.inputHead != trialVisit.inputHead)
        return;
    std::span<const ValueID> parameters = m_code.at(m_openLoops.last().head).parameters();
    for (uint32_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
        ValueID counter = underlyingCellOf(m_code, m_code.at(test).operand(operandIndex));
        if (!counter || m_code.isConstant(counter) || !std::ranges::contains(parameters, counter))
            continue;
        bool isLess = m_code.at(test).opcode == Opcode::Less || m_code.at(test).opcode == Opcode::LessEq;
        for (const VersionedLoop::SecondCounterGuard& guard : versionedLoop.secondCounterGuards) {
            if (guard.position < parameters.size() && parameters[guard.position] != counter && m_types.at(m_code.at(parameters[guard.position]).type).isInteger())
                trialVisit.secondCounters.append({ counter, parameters[guard.position], Range { std::min<int64_t>(guard.steps.minimum, 0), std::max<int64_t>(guard.steps.maximum, 0) }, (isLess == isOnTakenEdge) == !operandIndex });
        }
        m_claimsSecondCounters = true;
        versionedLoop.secondCounterTest = test;
        versionedLoop.secondCounterTestResult = isOnTakenEdge;
        return;
    }
}

static ValueID valueUnderAddends(const Code& code, ValueID position)
{
    static constexpr OpcodeSet changedByConstant { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::Increment, Opcode::Decrement };
    while (!code.isConstant(position)) {
        const Operation& made = code.at(position);
        if (changedByConstant.contains(made.opcode))
            position = made.valueOperand();
        else if ((made.opcode == Opcode::Add || made.opcode == Opcode::Sub) && made.form() == OperatorForm::Integers && code.isConstant(made.rightOperand()))
            position = made.leftOperand();
        else if (made.opcode == Opcode::Add && made.form() == OperatorForm::Integers && code.isConstant(made.leftOperand()))
            position = made.rightOperand();
        else
            break;
    }
    return position;
}

Vector<Walker::VersionedLoop::SecondCounterGuard, 1> Walker::guardSecondCounters(const AccessesNearCounter& found, std::span<const Reference> entering, BlockID& genericStart, Origin origin, NOESCAPE const ScopedLambda<Reference(ValueID)>& builtInFront)
{
    Vector<VersionedLoop::SecondCounterGuard, 1> guards;
    auto isInteger = [&](Reference value) {
        return value && value.isRecord() && m_types.at(typeOf(value)).isInteger();
    };
    auto integer = [&](int64_t value) {
        Draft draft(Opcode::Integer, origin);
        draft.setInteger(value);
        return constant(WTF::move(draft));
    };
    auto compute = [&](Opcode opcode, Reference left, Reference right) -> Reference {
        if (!isInteger(left) || !isInteger(right) || isUnreachable())
            return { };
        Reference made = buildIntegerOperator(*this, opcode, left, right, origin);
        return isInteger(made) && m_types.at(typeOf(made)).integers.isSubsetOf(Range::exact()) ? made : Reference();
    };
    auto continueIfHolds = [&](Opcode opcode, Reference left, Reference right) -> std::optional<PassedTest> {
        Reference test = isUnreachable() ? Reference() : buildIntegerOperator(*this, opcode, left, right, origin);
        if (!test || isUnreachable())
            return std::nullopt;
        if (std::optional<PassedTest> known = findPassedTest(test))
            return known;
        if (!test.isValue())
            return std::nullopt;
        if (!genericStart)
            genericStart = addBlock(BlockFrequency::Rare);
        return continueIfCheckPasses(test, genericStart);
    };
    auto testOf = [](const PassedTest& passed) {
        return passed.sides().empty() ? ValueID() : passed.sides().front().test;
    };
    Reference counter = entering[found.counter];
    Reference bound = builtInFront(found.bound);
    Reference iterations = found.counterRises ? compute(Opcode::Sub, bound, counter) : compute(Opcode::Sub, counter, bound);
    if (iterations && !found.admitsEqual)
        iterations = compute(Opcode::Sub, iterations, integer(1));
    for (const AccessBesideCounter& access : found.accesses) {
        Reference view = builtInFront(access.view);
        Reference enters = access.position < entering.size() ? entering[access.position] : Reference();
        if (!iterations || isUnreachable() || m_hasFailed || !view || !view.isValue() || !isViewOfNumbersReadInLine(*this, typeOf(view)) || !isInteger(enters))
            continue;
        auto afterLastStep = [&](int64_t step) -> Reference {
            if (!step)
                return enters;
            if (access.step) {
                Reference added = compute(Opcode::Mul, iterations, builtInFront(access.step));
                return added ? compute(access.isSubtracted ? Opcode::Sub : Opcode::Add, enters, added) : Reference();
            }
            Reference added = std::abs(step) == 1 ? iterations : compute(Opcode::Mul, iterations, integer(std::abs(step)));
            return added ? compute(step > 0 ? Opcode::Add : Opcode::Sub, enters, added) : Reference();
        };
        Reference most = afterLastStep(std::max<int64_t>(access.steps.maximum, 0));
        Reference least = afterLastStep(std::min<int64_t>(access.steps.minimum, 0));
        Reference last = most && access.addends.maximum ? compute(Opcode::Add, most, integer(access.addends.maximum)) : most;
        Reference first = least && access.addends.minimum ? compute(Opcode::Add, least, integer(access.addends.minimum)) : least;
        if (!first || !last)
            continue;
        std::optional<uint64_t> noted = findKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, view);
        Reference length = noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : buildRoomOfViewBesideAddend(*this, view, { }, origin);
        if (!isInteger(length) || isUnreachable())
            continue;
        if (!noted)
            noteKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, view, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), length.bits(), true);
        std::optional<PassedTest> isFromZeroUp = findNonNegativeProof(first);
        if (!isFromZeroUp)
            isFromZeroUp = continueIfHolds(Opcode::GreaterEq, first, integer(0));
        std::optional<PassedTest> isBelow = isFromZeroUp ? continueIfHolds(Opcode::Less, last, length) : std::nullopt;
        if (!isBelow)
            continue;
        guards.append({ view, length, access.position, access.steps, least, most, testOf(*isFromZeroUp), testOf(*isBelow) });
        count(StatisticID::SecondCounterViewsGuarded);
    }
    std::stable_sort(guards.begin(), guards.end(), [](const VersionedLoop::SecondCounterGuard& one, const VersionedLoop::SecondCounterGuard& other) { return one.position < other.position; });
    return guards;
}

Reference Walker::buildSecondCounter(const SecondCounter& second, Reference name, ValueID test, uint32_t operandIndex, bool isOnTakenEdge, EdgeID& edge, BlockID to)
{
    Draft narrowed(Opcode::Narrowed, m_origin);
    narrowed.setValueOperand(name);
    narrowed.setNarrowsHolderOfOperand(false);
    narrowed.setNarrowsTermOfOperand(true);
    narrowed.setNarrowsWithCounter(true);
    narrowed.setTestOperand(Reference::value(test));
    narrowed.setOperandIndex(static_cast<uint32_t>(second.steps.maximum) << 2 | (second.counterRises ? 2 : 0) | operandIndex);
    if (second.steps.minimum)
        narrowed.setCaseIndex(static_cast<uint32_t>(-second.steps.minimum));
    narrowed.setIsOnTakenEdge(isOnTakenEdge);
    TypeID bounded = resultTypeOf(narrowed);
    if (!bounded || bounded == m_code.at(name.id()).type)
        return { };
    if (!m_block) {
        m_block = addBlockFor(to);
        m_code.closeEdge(edge, m_block, { });
        edge = { };
    }
    count(StatisticID::SecondCountersBounded);
    m_claimsSecondCounters = true;
    return build(WTF::move(narrowed));
}

void Walker::noteSteppedKeyLimits(size_t placeOfTrial, ValueID test, bool isOnTakenEdge)
{
    TrialVisit& trialVisit = m_trialVisits[placeOfTrial];
    if (m_versionedLoops.isEmpty() || m_versionedLoops.last().frameIndex != trialVisit.frameIndex || m_versionedLoops.last().inputHead != trialVisit.inputHead)
        return;
    BlockID header = m_openLoops.last().head;
    std::span<const ValueID> parameters = m_code.at(header).parameters();
    std::span<const ValueID> entering = m_openLoops.last().recordsEntering();
    for (VersionedLoop::SteppedKeyLimit& limit : m_versionedLoops.last().steppedKeyLimits) {
        limit.headerTest = { };
        if (std::max(limit.steppedPosition, limit.counterPosition) >= std::min(parameters.size(), entering.size()))
            continue;
        limit.beside = SteppedWithCounter { parameters[limit.steppedPosition], parameters[limit.counterPosition], entering[limit.steppedPosition], entering[limit.counterPosition], limit.counterRises };
        auto isCounter = [&](ValueID operand) { return underlyingCellOf(m_code, operand) == limit.beside.counter; };
        bool areIntegers = m_types.at(m_code.at(limit.beside.stepped).type).isInteger() && m_types.at(m_code.at(limit.beside.counter).type).isInteger();
        if (!areIntegers || (!isCounter(m_code.at(test).operand(0)) && !isCounter(m_code.at(test).operand(1))))
            continue;
        limit.headerTest = test;
        limit.headerTestResult = isOnTakenEdge;
        trialVisit.secondCounters.append(secondCounterOf(limit.beside));
    }
}

std::optional<PassedTest> Walker::findSteppedKeyProof(Reference list, Reference position, Reference length)
{
    if (!list || !list.isRecord() || !position.isValue() || !length.isRecord())
        return std::nullopt;
    for (size_t place = m_versionedLoops.size(); place--;) {
        for (const VersionedLoop::SteppedKeyLimit& limit : m_versionedLoops[place].steppedKeyLimits) {
            if (!limit.headerTest || limit.length != length || underlyingCellOf(m_code, limit.list.id()) != underlyingCellOf(m_code, list.id()) || underlyingCellOf(m_code, position.id()) != limit.beside.stepped)
                continue;
            GroundsOfPosition grounds { .view = list.id(), .position = position.id(), .length = length.id(), .isBelow = limit.headerTest, .isBelowResult = limit.headerTestResult, .limitWithinLengthCheck = limit.limitCheck, .steppedBeside = limit.beside };
            std::optional<PassedTest> shown = findPassedTest(referenceTo(limit.headerTest), limit.headerTestResult) ? showPositionInside(grounds) : std::nullopt;
            if (!shown)
                continue;
            m_code.noteSteppedWithCounter(limit.beside);
            count(StatisticID::SteppedKeyChecksHoisted);
            return shown;
        }
    }
    return std::nullopt;
}

bool Walker::loweredCountersComeRound(BlockID head, std::span<const Reference> arguments)
{
    for (TrialVisit& trialVisit : m_trialVisits) {
        if (trialVisit.loweredCounters.isEmpty() || trialVisit.frameIndex >= m_frames.size() || inputBlock(trialVisit.frameIndex, trialVisit.inputHead).head != head)
            continue;
        for (ValueID counter : trialVisit.loweredCounters) {
            size_t place = m_code.at(head).placeOfParameter(counter);
            if (place < arguments.size() && stepsDownFromCounter(m_code, m_types, arguments[place].id(), counter))
                continue;
            trialVisit.misfits = true;
            m_loopSummaries[trialVisit.summaryIndex].counterWasRaised = true;
            return false;
        }
    }
    return true;
}

Reference Walker::findLoweredCounterStart(Reference position, int64_t& offset) const
{
    offset = 0;
    if (!position || !position.isValue() || m_state.answers().isEmpty())
        return { };
    ValueID counter = position.id();
    for (;;) {
        const Operation& record = m_code.at(counter);
        bool addsConstant = (record.opcode == Opcode::Add || record.opcode == Opcode::Sub) && m_code.isConstant(record.operand(1)) && m_code.at(record.operand(1)).opcode == Opcode::Integer;
        if (record.opcode == Opcode::Increment || record.opcode == Opcode::Decrement)
            offset += record.opcode == Opcode::Increment ? 1 : -1;
        else if (addsConstant) {
            int64_t constant = m_code.at(record.operand(1)).integer();
            if (constant != static_cast<int32_t>(constant))
                return { };
            offset += record.opcode == Opcode::Add ? constant : -constant;
        } else if (!cellPreservingOpcodes.contains(record.opcode))
            break;
        counter = record.operand(0);
        if (m_code.isConstant(counter))
            return { };
    }
    std::optional<uint64_t> start = findKnown(*this, KnownNote::NeverAbove, Reference::value(counter));
    return start ? Reference::fromBits(static_cast<uint32_t>(*start)) : Reference();
}

std::optional<PassedTest> Walker::findLoweredCounterProof(Reference view, Reference position, Reference length)
{
    int64_t offset = 0;
    Reference start = findLoweredCounterStart(position, offset);
    if (!start || !view || !view.isRecord() || !length || !length.isRecord())
        return std::nullopt;
    ValueID counter = position.id();
    while (m_code.at(counter).opcode != Opcode::Parameter)
        counter = m_code.at(counter).operand(0);
    GroundsOfPosition grounds { .view = view.id(), .position = position.id(), .length = length.id(), .loweredCounter = counter, .loweredFrom = start.id() };
    std::optional<PassedTest> shown = showPositionInside(grounds);
    for (ValueID named : { length.id(), start.id() }) {
        while (!shown) {
            const Operation& record = m_code.at(named);
            if (record.opcode == Opcode::Narrowed && record.testOperand() && !record.narrowsTermOfOperand()) {
                grounds.isBelow = record.testOperand();
                grounds.isBelowResult = record.isOnTakenEdge();
                shown = showPositionInside(grounds);
            }
            bool stepsByConstant = record.opcode == Opcode::Increment || record.opcode == Opcode::Decrement || ((record.opcode == Opcode::Add || record.opcode == Opcode::Sub) && m_code.isConstant(record.operand(1)));
            if (!cellPreservingOpcodes.contains(record.opcode) && !stepsByConstant)
                break;
            named = record.operand(0);
        }
    }
    if (shown)
        count(StatisticID::PositionsShownByLoweredCounters);
    return shown;
}

std::optional<PassedTest> Walker::findNonNegativeDifferenceTest(Reference position)
{
    struct Term {
        ValueID value;
        bool isSubtracted;
    };
    if (!position || !position.isValue() || !m_types.at(typeOf(position)).isInteger())
        return std::nullopt;
    static constexpr size_t mostTerms = 8;
    Vector<Term, mostTerms> terms { Term { position.id(), false } };
    for (size_t place = 0; place < terms.size() && terms.size() < mostTerms;) {
        ValueID root = terms[place].value;
        while (valuePreservingOpcodes.contains(m_code.at(root).opcode))
            root = m_code.at(root).operand(0);
        const Operation& record = m_code.at(root);
        if (record.opcode != Opcode::Add && record.opcode != Opcode::Sub) {
            ++place;
            continue;
        }
        terms.append(Term { record.operand(1), terms[place].isSubtracted != (record.opcode == Opcode::Sub) });
        terms[place].value = record.operand(0);
    }
    auto holds = [&](Reference lesser, Reference greater, bool admitsEqual, bool answer) -> std::optional<PassedTest> {
        std::optional<uint64_t> noted = findKnown(*this, KnownNote::Relation, answer ? lesser : greater, answer ? greater : lesser, admitsEqual);
        std::optional<PassedTest> passed = noted && static_cast<bool>(*noted & 1) == answer ? findPassedTest(Reference::fromBits(static_cast<uint32_t>(*noted >> 32)), answer) : std::nullopt;
        if (!passed || passed->sides().empty())
            return std::nullopt;
        SumOverRecords sum(m_code, m_types);
        return sum.add(position.id(), 1) && sum.takeAwayWhatIsFromZeroUp(passed->sides().front().test, answer) && sum.isFromZeroUp() ? passed : std::nullopt;
    };
    for (const Term& lesser : terms) {
        for (const Term& greater : terms) {
            if (!lesser.isSubtracted || greater.isSubtracted)
                continue;
            for (bool admitsEqual : { true, false }) {
                for (bool answer : { true, false }) {
                    if (std::optional<PassedTest> found = holds(referenceTo(lesser.value), referenceTo(greater.value), admitsEqual, answer)) {
                        count(StatisticID::DifferencesProvenNonNegative);
                        return found;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<PassedTest> Walker::findWhatHoldsPositionBelowRoomOfView(Reference view, Reference position)
{
    TypeID type = view && view.isRecord() && position && position.isRecord() ? typeOf(position) : TypeID();
    if (!type || !m_types.at(type).isInteger())
        return std::nullopt;
    for (size_t place = m_versionedLoops.size(); place--;) {
        const VersionedLoop::CounterBelowRoomOfViews& held = m_versionedLoops[place].counterBelowRoom;
        std::optional<uint64_t> length = held.isBelowBound ? findKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, view) : std::nullopt;
        if (!length)
            continue;
        GroundsOfPosition grounds { .view = view.id(), .position = position.id(), .length = Reference::fromBits(static_cast<uint32_t>(*length)).id(), .isBelow = held.isBelowBound, .stepNonNegativeCheck = held.stepNonNegativeCheck };
        if (held.counter && findKnown(*this, KnownNote::NeverLiesBelow, Reference::value(held.counter), referenceTo(held.start))) {
            grounds.counter = held.counter;
            grounds.start = held.start;
        }
        std::optional<PassedTest> shown = showPositionInside(grounds);
        for (size_t asked = 0; !shown && asked < held.fromZeroUp.size(); ++asked) {
            grounds.isFromZeroUp = held.fromZeroUp[asked];
            shown = showPositionInside(grounds);
        }
        if (shown)
            return shown;
    }
    ValueID stepped;
    for (size_t place = m_versionedLoops.size(); place--;) {
        const VersionedLoop& versionedLoop = m_versionedLoops[place];
        BlockID head = versionedLoop.secondCounterTest ? inputBlock(versionedLoop.frameIndex, versionedLoop.inputHead).head : BlockID();
        if (!head)
            continue;
        if (!stepped)
            stepped = valueUnderAddends(m_code, position.id());
        if (m_code.isConstant(stepped) || m_code.at(stepped).opcode != Opcode::Parameter || m_code.blockOf(stepped) != head)
            continue;
        uint32_t positionOfStepped = m_code.at(head).placeOfParameter(stepped);
        auto first = std::ranges::lower_bound(versionedLoop.secondCounterGuards, positionOfStepped, { }, &VersionedLoop::SecondCounterGuard::position);
        for (auto found = first; found != versionedLoop.secondCounterGuards.end() && found->position == positionOfStepped; ++found) {
            const VersionedLoop::SecondCounterGuard& guard = *found;
            if (underlyingCellOf(m_code, guard.view.id()) != underlyingCellOf(m_code, view.id()) || !findKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, guard.view))
                continue;
            GroundsOfPosition grounds { .view = view.id(), .position = position.id(), .length = guard.length.id(), .isBelow = guard.isBelow, .isFromZeroUp = guard.isFromZeroUp, .counter = stepped, .start = guard.least.id(),
                .loweredCounter = stepped, .loweredFrom = guard.most.id(), .counterTest = versionedLoop.secondCounterTest, .counterTestResult = versionedLoop.secondCounterTestResult };
            if (std::optional<PassedTest> shown = showPositionInside(grounds)) {
                recordSpeculation();
                return shown;
            }
        }
    }
    return std::nullopt;
}

std::optional<PassedTest> Walker::showPositionInside(const GroundsOfPosition& grounds)
{
    bool isShown = groundsShowPositionInside(m_code, m_types, grounds.position, grounds, scopedLambda<bool(ValueID, bool)>([&](ValueID relation, bool testResult) {
        return !!findPassedTest(referenceTo(relation), testResult);
    }));
    if (!isShown)
        return std::nullopt;
    PassedTest passed;
    auto restOn = [&](ValueID relation, bool testResult) {
        if (std::optional<PassedTest> one = relation ? findPassedTest(referenceTo(relation), testResult) : std::nullopt)
            passed = passed.with(*one);
    };
    restOn(grounds.isBelow, grounds.isBelowResult);
    restOn(grounds.isFromZeroUp, grounds.isNonNegativeResult);
    restOn(grounds.stepNonNegativeCheck, true);
    restOn(grounds.limitWithinLengthCheck, true);
    restOn(grounds.counterTest, grounds.counterTestResult);
    passed.m_groundsOfPosition = grounds;
    return passed;
}

std::optional<PassedTest> Walker::boundsProofFromChecks(Reference view, Reference position, Reference length, const PassedTest& fromZeroUp, const PassedTest& below)
{
    if (!view || !view.isRecord() || !position || !position.isRecord() || !length || !length.isRecord())
        return std::nullopt;
    GroundsOfPosition grounds { .view = view.id(), .position = position.id(), .length = length.id() };
    if (!below.sides().empty()) {
        grounds.isBelow = below.sides().front().test;
        grounds.isBelowResult = below.sides().front().testResult;
    }
    if (!fromZeroUp.sides().empty()) {
        grounds.isFromZeroUp = fromZeroUp.sides().front().test;
        grounds.isNonNegativeResult = fromZeroUp.sides().front().testResult;
    }
    return showPositionInside(grounds);
}

static TypeID numberSpeculatedInIteration(Walker& walker, ValueID operand)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.stripCasts(operand);
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return TypeTable::number;
    const LinkTables& linked = walker.facts().linked();
    PropertyKeyID key = index.value(read).key();
    ModuleID module = walker.program().at(walker.currentFunction()).module;
    bool isInteger = module && linked.namesReadAsIntegers.contains(module, key) && !liesInThread(key) && !linked.keysStoredWhatIsNoInt32.get(key.index());
    return isInteger ? TypeTable::int32 : TypeTable::number;
}

Walker::NumberAskedBesideNumber Walker::askValueBesideNumberForNumber(const BodyValue& operation)
{
    if (effectRuleOf(operation.opcode()) != EffectRule::Operator || numberOfNamedOperandsOf(operation.opcode()) != 2 || !m_opensGuesses || !canBuildIterationGuardAfterStores() || !canRestartIteration())
        return { };
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    bool isEnteredAnew = m_frames[versionedLoop.frameIndex].index->loopDepthOf(versionedLoop.inputHead) > 1;
    if ((!currentGenericVersionSpeculates() && canBuildIterationGuard() && !isEnteredAnew) || program().at(currentFunction()).has(FunctionFlags::IsChecked) || leavesOutQuestionThatEveryTurnWouldAsk())
        return { };
    auto isUnknown = [&](Reference value) {
        TypeID type = typeOf(value);
        return m_types.isUninformative(type) || m_types.isUninformative(m_types.withPrimitives(type, Primitive::UndefinedOrNull));
    };
    static constexpr OpcodeSet operatorsAskedOnBothSides = allRelations | OpcodeSet { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod };
    Reference left = referenceOf(operation.operand(0));
    Reference right = referenceOf(operation.operand(1));
    if (operatorsAskedOnBothSides.contains(operation.opcode()) && left && left.isValue() && right && right.isValue() && left != right && isUnknown(left) && isUnknown(right)) {
        guardExpectedType(*this, operation.operand(0), numberSpeculatedInIteration(*this, operation.operand(0)), draftOf(operation).origin(), GuardedIterations::SomeWithUnguardedVersion);
        if (isUnreachable() || m_hasFailed)
            return { };
    }
    for (uint32_t asked = 0; asked < 2; ++asked) {
        Reference value = referenceOf(operation.operand(asked));
        Reference beside = referenceOf(operation.operand(1 - asked));
        if (!value || !value.isValue() || !beside || !isUnknown(value) || !m_types.at(typeOf(beside)).isNumber())
            continue;
        guardExpectedType(*this, operation.operand(asked), numberSpeculatedInIteration(*this, operation.operand(asked)), draftOf(operation).origin(), GuardedIterations::SomeWithUnguardedVersion);
        if (isUnreachable() || m_hasFailed)
            return { };
        Reference proven = value;
        while (Reference checked = m_state.checkedNameOf(proven))
            proven = checked;
        if (proven == value)
            return { };
        count(StatisticID::NumberOperandsGuardedInIterations);
        return { asked, proven };
    }
    return { };
}

void Walker::proveComparedNumber(const BodyValue& operation)
{
    if (!canBuildIterationGuard() || program().at(currentFunction()).has(FunctionFlags::IsChecked) || leavesOutQuestionThatEveryTurnWouldAsk())
        return;
    for (uint32_t side : { 0u, 1u }) {
        Reference value = referenceOf(operation.operand(side));
        Reference element = referenceOf(operation.operand(1 - side));
        if (!value || !value.isValue() || !element || !element.isValue() || !m_types.isUninformative(typeOf(value)) || !m_types.at(typeOf(element)).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) || (representationOf(element) != Representation::Double && representationOf(element) != Representation::OptionalDouble) || !isInvariantInSpecializedVersion(value, IterationPurity::KnownEffectsOnly))
            continue;
        guardExpectedType(*this, operation.operand(side), TypeTable::number, draftOf(operation).origin());
        return;
    }
}

void Walker::proveWhatLooksCountOnAt(const BodyValue& operation)
{
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (!versionedLoop.canUseIterationGuards || !m_opensGuesses || !canRestartIteration())
        return;
    if (operation.opcode() == Opcode::StrictEq || allRelations.contains(operation.opcode()))
        proveComparedNumber(operation);
    if (versionedLoop.frameIndex != m_frames.size() - 1 || !canRestartIteration())
        return;
    auto typeAsBuilt = [&](ValueID input) -> TypeID {
        Reference built = referenceOf(input);
        TypeID known = built ? typeOf(built) : TypeID();
        return known ? known : TypeID(TypeTable::anything);
    };
    static constexpr OpcodeSet operators = opcodesOfFamily(OperationFamily::Operator);
    if (operators.contains(operation.opcode()) && typedFormOf(*this, operation, TypesOfInputs(typeAsBuilt)) != TypedForm::None) {
        bool proofPays = false;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            const Type& type = m_types.at(typeAsBuilt(operand));
            Reference built = referenceOf(operand);
            proofPays |= type.hasObjects() || (!type.isNumber() && (!built || !built.isRecord() || representationOf(built) == Representation::JSValue));
        });
        if (!proofPays)
            return;
    }
    auto isRawNumberWithoutProof = [&](ValueID operand, TypeID proven) {
        Reference built = referenceOf(operand);
        return built && built.isRecord() && m_types.at(typeOf(built)).isNumber() && representationOf(built) != Representation::JSValue && !m_types.at(proven).isInteger();
    };
    Vector<std::pair<ValueID, TypeID>, 2> wanted;
    forEachTypeThatGuessWouldProveAt(*this, operation, TypesOfInputs(typeAsBuilt), [&](ValueID operand, TypeID expected, TypeID proven) {
        if (operators.contains(operation.opcode()) && isRawNumberWithoutProof(operand, proven))
            return;
        if (!guessAtCalleeAsksForBuiltinWhereItFails(*this, operation, operand) && (operand != operation.operand(0) || !rowIsToldApartFromViewsInTurn(*this, operation, expected)))
            wanted.append({ operand, expected });
    });
    for (auto [operand, expected] : wanted) {
        if (canRestartIteration() && referenceOf(operand))
            guardExpectedType(*this, operand, expected, originOf(operation));
    }
}

static bool leadsFromPreheader(const BodyIndex& index, ValueID value, uint32_t firstValueOfLoop)
{
    static constexpr OpcodeSet reads { Opcode::PropGet, Opcode::PrivateGet, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::CheckInitialized };
    for (value = index.stripCasts(value); value && value.index() >= firstValueOfLoop; value = index.stripCasts(index.value(value).operand(0))) {
        if (index.isParameter(value) || !reads.contains(index.opcodeOf(value)))
            return false;
        if (index.opcodeOf(value) == Opcode::GetModuleVariable)
            return true;
    }
    return !!value;
}

static bool isCountedAgainstBound(const BodyIndex& index, BlockID head)
{
    BodyTerminator end = index.block(head).terminator();
    ValueID condition = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!condition || index.isParameter(condition) || !allRelations.contains(index.opcodeOf(condition)))
        return false;
    auto isHeadParameter = [&](ValueID operand) {
        ValueID value = index.stripCasts(operand);
        return value && index.isParameter(value) && index.blockOf(value) == head;
    };
    BodyValue relation = index.value(condition);
    return isHeadParameter(relation.operand(0)) || isHeadParameter(relation.operand(1));
}

static bool innerLoopsOnlyReadHoistedArrays(const Walker& walker, BlockID inputHead)
{
    static constexpr OpcodeSet reads { Opcode::ElemGet, Opcode::PropGet, Opcode::PrivateGet };
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    if (index.loop(loop).numberOfLoopsWithin < 2)
        return true;
    for (BlockID member : index.membersOf(loop)) {
        if (index.loopOf(member) == loop)
            continue;
        if (index.isLoopHead(member) && isCountedAgainstBound(index, member))
            return false;
        for (BodyValue operation : index.block(member).values()) {
            Opcode opcode = operation.opcode();
            if (reads.contains(opcode) ? !leadsFromPreheader(index, operation.objectOperand(), firstValueOfLoop) : effectRuleOf(opcode) != EffectRule::Operator && !declaredEffectsOfOperation(walker.program(), opcode, operation).mayBeRunAgainUnseen())
                return false;
        }
    }
    return true;
}

bool Walker::areIterationsRestartable(BlockID inputHead, const LoopPreview& preview)
{
    size_t frameIndex = m_frames.size() - 1;
    if ((!preview.effects.mayRunOtherCode() && !pushOfLoopFillsLiteral(inputHead) && pushesOntoLiteralsOf(currentBodyIndex().loopOf(inputHead)).empty() && !searchesNumbersByHalves(*this, inputHead)) || hasStringsInBuilders() || m_coldCopyDepth || (isUnderHandler() && !mayVersionLoopUnderHandler(inputHead))
        || (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) && !iterationsSteppedAtHandOverPositionsAlone(inputHead)))
        return false;
    return innerLoopsOnlyReadHoistedArrays(*this, inputHead) || hasUnrolledInnerLoops(inputHead);
}

TypeID Walker::backEdgeCheckType(BlockID inputHead, uint32_t parameter) const
{
    if (m_versionedLoops.isEmpty())
        return { };
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (versionedLoop.frameIndex != m_frames.size() - 1 || versionedLoop.inputHead != inputHead)
        return { };
    for (auto [position, held] : versionedLoop.backEdgeChecks) {
        if (position == parameter)
            return held;
    }
    return { };
}

TypeID Walker::typeAskedOfWhatOtherCodeBringsRound(BlockID head, TypeID ofParameter, TypeID comingRound)
{
    if (m_versionedLoops.isEmpty())
        return { };
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (!versionedLoop.turnsThatRunOtherCodeComeRound || versionedLoop.frameIndex != m_frames.size() - 1 || inputBlock(versionedLoop.frameIndex, versionedLoop.inputHead).head != head)
        return { };
    if (m_types.isSubset(comingRound, TypeTable::number) || !m_types.isSubset(ofParameter, TypeTable::number))
        return { };
    if (m_types.at(ofParameter).isInteger() && m_types.overlap(comingRound, TypeTable::int32))
        return TypeTable::int32;
    return m_types.overlap(comingRound, TypeTable::number) ? TypeID(TypeTable::number) : TypeID();
}

bool Walker::canEnterWith(BlockID entry, std::span<const Reference> besideFieldsOfScopes) const
{
    std::span<const ValueID> taken = m_code.at(entry).parameters();
    if (taken.size() - numberOfFieldsOfScopesThatBlockTakes(entry) != besideFieldsOfScopes.size())
        return false;
    for (size_t index = 0; index < besideFieldsOfScopes.size(); ++index) {
        TypeID type = ownTypeOf(besideFieldsOfScopes[index]);
        if (!type || !m_types.isSubset(type, m_code.at(taken[index]).type))
            return false;
    }
    return true;
}

void Walker::enterWithFieldsOfScopes(BlockID entry, std::span<const Reference> besideFieldsOfScopes)
{
    if (!canEnterWith(entry, besideFieldsOfScopes)) {
        fail("the arguments do not fit the entry block of a loop version"_s);
        return;
    }
    Vector<Reference, 8> handedOn(besideFieldsOfScopes);
    EdgeID noEdge;
    if (!bringFieldsOfScopesRound(noEdge, entry, handedOn))
        return;
    if (m_block && m_code.at(entry).frequency == BlockFrequency::Unlikely)
        m_code.makeBlockUnlikely(m_block, true);
    crossEdge({ }, entry, handedOn.span());
}

bool Walker::needsBackEdgeRecheck(BlockID head, uint32_t parameter, Reference loopCarriedValue)
{
    if (m_versionedLoops.isEmpty())
        return false;
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (inputBlock(versionedLoop.frameIndex, versionedLoop.inputHead).head != head)
        return false;
    TypeID held = backEdgeCheckType(versionedLoop.inputHead, parameter);
    return held && isTypeOfLink(m_types, held) && m_types.isSubset(m_code.at(m_code.at(head).parameters()[parameter]).type, held) && !m_state.cellFacts(loopCarriedValue).contains(CellFact::BearsNoMark);
}

bool Walker::canEnterGenericVersion(BlockID head, std::span<const Reference> besideFieldsOfScopes)
{
    if (m_versionedLoops.isEmpty())
        return false;
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (inputBlock(versionedLoop.frameIndex, versionedLoop.inputHead).head != head)
        return false;
    if (m_code.at(head).parameters().size() - numberOfFieldsOfScopesThatBlockTakes(head) != besideFieldsOfScopes.size())
        return false;
    return canEnterWith(versionedLoop.entry, besideFieldsOfScopes);
}

void Walker::jumpToGenericVersion(BlockID head, std::span<const Reference> besideFieldsOfScopes)
{
    if (isUnreachable())
        return;
    if (!head || m_versionedLoops.isEmpty() || inputBlock(m_versionedLoops.last().frameIndex, m_versionedLoops.last().inputHead).head != head) {
        fail("an iteration jumps to the generic version of another loop"_s);
        return;
    }
    if (m_code.at(head).parameters().size() - numberOfFieldsOfScopesThatBlockTakes(head) != besideFieldsOfScopes.size()) {
        fail("an iteration jumps to a generic version without all of the header's values"_s);
        return;
    }
    enterWithFieldsOfScopes(m_versionedLoops.last().entry, besideFieldsOfScopes);
}

Reference Walker::askAgainWhereItComesRound(EdgeID& edge, BlockID head, TypeID heldTo, std::span<const Reference> besideFieldsOfScopes, const ScopedLambda<Reference()>& whatComesRound)
{
    if (!heldTo || !canEnterGenericVersion(head, besideFieldsOfScopes))
        return { };
    if (edge)
        layBlockOnEdge(edge, head);
    Reference loopCarriedValue = whatComesRound();
    bool isLink = m_versionedLoops.last().backEdgeChecks.containsIf([&](const auto& counter) { return counter.second == heldTo; });
    Reference test = !loopCarriedValue ? Reference() : isLink ? buildLoopCarriedCheck(*this, loopCarriedValue, heldTo, m_origin) : buildTestOfNumber(*this, loopCarriedValue, heldTo, m_origin);
    if (!test || !test.isValue())
        return { };
    BlockID passed = addBlock();
    BlockID failed = addBlock(BlockFrequency::Rare);
    letGuessFailUnasked(failed, m_origin);
    branch(test, passed, failed);
    continueIn(failed);
    jumpToGenericVersion(head, besideFieldsOfScopes);
    continueIn(passed);
    return isUnreachable() ? Reference() : buildProofOfNumber(*this, loopCarriedValue, test, m_origin);
}

bool Walker::isIterationEffectFree() const
{
    return !m_versionedLoops.isEmpty() && m_versionedLoops.last().isEntered && !isUnreachable() && m_state.isEffectFreeSince(m_versionedLoops.last().restartPoint) && !handlerPreventsIterationRestart() && !hasStringsInBuilders();
}

bool Walker::isUncaughtIterationEffectFree() const
{
    return isIterationEffectFree() && !mayCatchInIteration();
}

bool Walker::canRestartIteration() const
{
    return !m_versionedLoops.isEmpty() && m_versionedLoops.last().isEntered && !isUnreachable() && m_state.canRestartFrom(m_versionedLoops.last().restartPoint) && !handlerPreventsIterationRestart() && !hasStringsInBuilders();
}

bool Walker::hasOnlyKnownEffectsInIteration(uint32_t restartPoint) const
{
    uint32_t mark = m_state.restartMark();
    for (size_t index = m_frames.size(); index-- && mark && WalkState::restartPointOf(mark) != restartPoint;) {
        const Frame& frame = m_frames[index];
        if (frame.displacedRestartMark && WalkState::restartPointOf(mark) == frame.inlineeRestartPoint)
            mark = frame.displacedRestartMark;
    }
    return restartPoint && WalkState::restartPointOf(mark) == restartPoint;
}

std::optional<uint32_t> Walker::specializedOpenLoop(IterationPurity purity) const
{
    if (m_versionedLoops.isEmpty() || !m_versionedLoops.last().isEntered || isUnreachable())
        return std::nullopt;
    uint32_t restartPoint = m_versionedLoops.last().restartPoint;
    bool hasPurity = purity == IterationPurity::EffectFree ? m_state.isEffectFreeSince(restartPoint) : purity == IterationPurity::KnownEffectsOnly ? m_state.hasOnlyKnownEffectsSince(restartPoint) : hasOnlyKnownEffectsInIteration(restartPoint);
    if (!hasPurity || mayCatchInIteration() || hasStringsInBuilders() || !has(Mechanism::LoopInvariants))
        return std::nullopt;
    std::optional<uint32_t> innermost = innermostOpenLoop();
    if (!innermost)
        return std::nullopt;
    const OpenLoop& loop = m_openLoops[*innermost];
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (!versionedLoop.genericStart || loop.frame != versionedLoop.frameIndex || m_frames[loop.frame].index->loop(loop.loop).head != versionedLoop.inputHead)
        return std::nullopt;
    if (!loop.preheaderBlock || m_code.at(loop.preheaderBlock).hasEnded())
        return std::nullopt;
    return innermost;
}

bool Walker::isInLoopOfIteration() const
{
    std::optional<uint32_t> innermost = m_versionedLoops.isEmpty() ? std::nullopt : innermostOpenLoop();
    if (!innermost)
        return false;
    const OpenLoop& loop = m_openLoops[*innermost];
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    return loop.frame == versionedLoop.frameIndex && m_frames[loop.frame].index->loop(loop.loop).head != versionedLoop.inputHead;
}

bool Walker::turnHoldsInnerLoops() const
{
    if (m_versionedLoops.isEmpty() || !m_versionedLoops.last().canUseIterationGuards)
        return false;
    const BodyIndex& index = *m_frames[m_versionedLoops.last().frameIndex].index;
    return index.loop(index.loopOf(m_versionedLoops.last().inputHead)).numberOfLoopsWithin > 1;
}

bool Walker::isInvariantInSpecializedVersion(Reference reference, IterationPurity purity) const
{
    std::optional<uint32_t> loop = specializedOpenLoop(purity);
    return loop && reference && reference.isRecord() && isInvariantInLoop(*loop, reference) && typeOf(reference) == ownTypeOf(reference);
}

bool Walker::iterationsPreservePrototypeChains() const
{
    std::optional<uint32_t> loop = specializedOpenLoop();
    if (!loop)
        return false;
    const Effects& ofTurns = m_openLoops[*loop].effects;
    return !ofTurns.mayRunOtherCode() && !ofTurns.writes.overlaps(LocationMasks(AbstractHeapSet { AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks }));
}

bool Walker::iterationsPreserveMemberGuards() const
{
    std::optional<uint32_t> loop = specializedOpenLoop();
    if (!loop)
        return false;
    const Effects& ofTurns = m_openLoops[*loop].effects;
    return !ofTurns.mayRunOtherCode() && !ofTurns.writes.overlaps(declaredBoundInTable(Opcode::AreMembersUnchanged).reads);
}

bool Walker::passesOnEveryIteration(const VersionedLoop& versionedLoop, UnrolledIterations unrolledIterations) const
{
    const BodyIndex& index = *m_frames[versionedLoop.frameIndex].index;
    BlockID block = m_frames[versionedLoop.frameIndex].block;
    if (unrolledIterations == UnrolledIterations::AsPassed)
        block = blockAroundConstantTrips(versionedLoop.frameIndex, block, index.loopOf(versionedLoop.inputHead));
    return index.dominates(block, versionedLoop.lastBackEdgeDominator);
}

bool Walker::canHoistNamedLookup(PropertyKeyID key) const
{
    std::optional<uint32_t> loop = specializedOpenLoop(IterationPurity::KnownEffectsOnly);
    if (!loop)
        return false;
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (!passesOnEveryIteration(versionedLoop) || !executesOnEveryIteration(*loop, currentHoistPoint()))
        return false;
    const Effects& ofTurns = m_openLoops[*loop].effects;
    LocationMasks found(AbstractHeapSet { AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks });
    found.add(program(), MemoryLocation::objectField(key));
    return !ofTurns.mayRunOtherCode() && !ofTurns.writes.overlaps(found);
}

bool Walker::isGuardedByEquality(uint32_t loopIndex) const
{
    const OpenLoop& loop = m_openLoops[loopIndex];
    const BodyIndex& index = *m_frames.last().index;
    BlockID first = loop.frame == m_frames.size() - 1 ? index.loop(loop.loop).head : entryBlock;
    for (BlockID block = m_frames.last().block; block != first;) {
        BlockID from = index.immediateDominator(block);
        if (!from)
            return false;
        BodyTerminator end = index.terminator(from);
        ValueID condition = end.isConditionalBranch() ? index.stripCasts(end.operand(0)) : ValueID();
        if (condition && !index.isParameter(condition) && allEqualities.contains(index.opcodeOf(condition)) && end.taken().block() == block && index.numberOfIncomingEdges(block) == 1)
            return true;
        block = from;
    }
    return false;
}

bool Walker::canReadNamedInPreheader(PropertyKeyID key) const
{
    LocationMasks found(AbstractHeapSet { AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks });
    found.add(program(), MemoryLocation::objectField(key));
    return canReadInPreheader(found);
}

bool Walker::canReadInPreheader(const LocationMasks& reads) const
{
    std::optional<uint32_t> loop = specializedOpenLoop(IterationPurity::KnownEffectsOnly);
    if (!loop || !isAfterFewBranches(*loop, currentHoistPoint()) || isGuardedByEquality(*loop))
        return false;
    const Effects& ofTurns = m_openLoops[*loop].effects;
    return !ofTurns.mayRunOtherCode() && !ofTurns.writes.overlaps(reads);
}

bool Walker::canHoistGlobalAddress() const
{
    std::optional<uint32_t> loop = specializedOpenLoop(IterationPurity::KnownEffectsOnly);
    return loop && !m_openLoops[*loop].effects.mayRunOtherCode();
}

bool Walker::canHoistKeyedAddress() const
{
    std::optional<uint32_t> loop = specializedOpenLoop(IterationPurity::KnownEffectsOnly);
    if (!loop)
        return false;
    const Effects& ofIterations = m_openLoops[*loop].effects;
    return !ofIterations.mayRunOtherCode() && !ofIterations.writes.overlaps(declaredBoundInTable(Opcode::GetKeyedPropertyAddress).reads);
}

bool Walker::iterationsPreserveVariable(VariableID variable) const
{
    std::optional<uint32_t> loop = specializedOpenLoop(IterationPurity::KnownEffectsOnly);
    if (!loop)
        return false;
    const Effects& ofIterations = m_openLoops[*loop].effects;
    LocationMasks location;
    location.add(program(), MemoryLocation::moduleVariable(variable));
    return !ofIterations.mayRunOtherCode() && !ofIterations.writes.overlaps(location);
}

Reference Walker::readVariableInPreheader(VariableID variable, TypeID tested, Origin origin)
{
    constexpr auto purity = IterationPurity::KnownEffectsOnly;
    Reference read;
    Reference test;
    {
        SpecializedPreheaderScope preheaderScope(*this, true, purity);
        FieldOfScope field;
        field.variable = variable;
        read = build(draftReadBeforeHeader(field, ReadBeforeHeaderUse::GuardOperand, true));
        if (!read || !read.isValue() || m_hasFailed)
            return { };
        Draft question(Opcode::TypeTest, origin);
        question.setValueOperand(read);
        question.setTestedType(tested);
        test = build(WTF::move(question));
    }
    if (!test || m_hasFailed || !tryBranchInPreheader(test, true, purity))
        return { };
    SpecializedPreheaderScope preheaderScope(*this, true, purity);
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(read);
    proof.setTestOperand(test);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    return build(WTF::move(proof));
}

bool Walker::canHoistKeyedLookup(Opcode lookup) const
{
    return canHoistLookup(lookup == Opcode::GetOwnValueUnderKeyOrAbsent ? readsOfOwnKeyLookup(program()) : declaredBoundInTable(lookup).reads);
}

bool Walker::canHoistLookup(const LocationMasks& reads) const
{
    std::optional<uint32_t> loop = specializedOpenLoop(IterationPurity::KnownEffectsOnly);
    if (!loop)
        return false;
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (!passesOnEveryIteration(versionedLoop) || !executesOnEveryIteration(*loop, currentHoistPoint()))
        return false;
    const Effects& ofTurns = m_openLoops[*loop].effects;
    return !ofTurns.mayRunOtherCode() && !ofTurns.writes.overlaps(reads.without(unobservableWriteHeaps));
}

Walker::AllocationsUsedInLoop Walker::allocationsUsedInLoop(BlockID inputHead)
{
    AllocationsUsedInLoop mentioned;
    const BodyIndex& index = currentBodyIndex();
    uint32_t firstValue = index.block(inputHead).firstParameter().index();
    auto mention = [&](ValueID value) {
        if (!value || value.index() >= firstValue)
            return;
        Reference reference = referenceOf(value);
        if (!reference || !reference.isValue())
            return;
        ValueID made = underlyingCellOf(m_code, reference.id());
        if (!m_code.isConstant(made) && allocatesNewCell(m_code.at(made)) && (mentioned.isEmpty() || mentioned.last() != made.index()))
            mentioned.append(made.index());
    };
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        BodyBlock block = index.block(member);
        for (BodyValue operation : block.values())
            operation.forEachOperand([&](ValueID operand, uint32_t) { mention(operand); });
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
    std::ranges::sort(mentioned);
    mentioned.shrink(std::ranges::unique(mentioned).begin() - mentioned.begin());
    return mentioned;
}

std::optional<Walker::IsListedAllocation> Walker::classifyAgainstAllocations(const AllocationsUsedInLoop& mentioned, Reference other) const
{
    if (!other.isValue())
        return other.isConstant() ? std::optional(IsListedAllocation::No) : std::nullopt;
    ValueID root = underlyingCellOf(m_code, other.id());
    if (!m_code.isConstant(root) && m_code.at(root).opcode == Opcode::Parameter) {
        for (const IndexedIteration& iteration : m_indexedIterations) {
            if (iteration.arrayBeforeJoin && iteration.arrayBeforeJoin.isValue() && iteration.list && iteration.list.isValue() && underlyingCellOf(m_code, iteration.list.id()) == root)
                root = underlyingCellOf(m_code, iteration.arrayBeforeJoin.id());
        }
    }
    if (!m_code.isConstant(root) && allocatesNewCell(m_code.at(root)))
        return std::ranges::binary_search(mentioned, root.index()) ? IsListedAllocation::Yes : IsListedAllocation::No;
    if (m_inputs.options.shouldAssumeLaterValuesUnaliased) [[unlikely]]
        return IsListedAllocation::No;
    return mentioned.isEmpty() || root.index() < mentioned.first() ? std::optional(IsListedAllocation::No) : std::nullopt;
}

void Walker::recordTrialEffectsOfWriter(const Effects& effects, const std::optional<Vector<uint32_t, 2>>& objectsMadeHere)
{
    if (!objectsMadeHere || m_trialVisits.isEmpty()) {
        recordTrialEffects(effects);
        return;
    }
    Effects heard = effects;
    heard.writesObjectsMadeHereAlone = true;
    TrialVisit& innermost = m_trialVisits.last();
    for (uint32_t made : *objectsMadeHere) {
        if (made < innermost.numberOfRecordsAtHead && (innermost.objectsMadeInFrontThatAreWritten.isEmpty() || innermost.objectsMadeInFrontThatAreWritten.last() != made))
            innermost.objectsMadeInFrontThatAreWritten.append(made);
    }
    recordTrialEffects(heard);
}

bool Walker::iterationsPreserveArrayGuards(Reference list, IterationPurity purity)
{
    std::optional<uint32_t> loop = specializedOpenLoop(purity);
    if (!loop)
        return false;
    if (m_trialVisits.isEmpty() || !m_trialVisits.last().othersAreSparedAtHead)
        return preservesHoistedArrayGuards(*this, m_openLoops[*loop].effects);
    const TrialVisit& trialVisit = m_trialVisits.last();
    std::optional<IsListedAllocation> other = classifyAgainstAllocations(*trialVisit.othersAreSparedAtHead, list);
    if (!other || (*other == IsListedAllocation::Yes && !trialVisit.listsMadeHereThatAreSparedAtHead.contains(underlyingCellOf(m_code, list.id()).index())))
        return preservesHoistedArrayGuards(*this, m_openLoops[*loop].effects);
    HoistedArrayGuard asked { list, ArrayStorage::Other, false };
    return preservesHoistedArrayGuards(*this, m_openLoops[*loop].effects, { &asked, 1 }, *trialVisit.othersAreSparedAtHead);
}

bool Walker::iterationsPreserveArrayLength(Reference array, IterationPurity purity) const
{
    std::optional<uint32_t> loop = specializedOpenLoop(purity);
    if (!loop)
        return false;
    const Effects& ofTurns = m_openLoops[*loop].effects;
    if (!ofTurns.mayRunOtherCode() && !ofTurns.writes.contains(AbstractHeap::ArrayLengths))
        return true;
    if (ofTurns.mayRunOtherCode() || !ofTurns.writesObjectsMadeHereAlone || m_trialVisits.isEmpty() || !m_trialVisits.last().othersAreSparedAtHead)
        return false;
    return classifyAgainstAllocations(*m_trialVisits.last().othersAreSparedAtHead, array) == IsListedAllocation::No;
}

void Walker::noteLengthOfViewAskedForWrittenPlace(Reference view)
{
    if (m_trialVisits.isEmpty() || m_openLoops.isEmpty() || !view.isValue() || !isInvariantInLoop(m_openLoops.size() - 1, view) || leavesOutQuestionThatEveryTurnWouldAsk(QuestionIsAsked::Anywhere))
        return;
    m_trialVisits.last().asksLengthsOfViewsFromInFrontForWrittenPlaces = true;
}

void Walker::noteListFromInFrontTakenApart(Reference list)
{
    if (m_trialVisits.isEmpty() || m_openLoops.isEmpty() || !list.isValue() || !isInvariantInLoop(m_openLoops.size() - 1, list) || leavesOutQuestionThatEveryTurnWouldAsk(QuestionIsAsked::Anywhere))
        return;
    m_trialVisits.last().takesApartListsFromInFront = true;
}

bool Walker::iterationsPreserveTypedArrays(IterationPurity purity) const
{
    std::optional<uint32_t> loop = specializedOpenLoop(purity);
    if (!loop)
        return false;
    const Effects& ofTurns = m_openLoops[*loop].effects;
    return !ofTurns.mayRunOtherCode() && !ofTurns.writes.overlaps(LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }));
}

bool Walker::areElementStoresNumeric(IterationPurity purity) const
{
    std::optional<uint32_t> loop = specializedOpenLoop(purity);
    return loop && !(m_openLoops[*loop].effects.writes.maskOf(AbstractHeap::Elements) & cellHoldingElementsMask);
}

bool Walker::hasNoElementStores() const
{
    std::optional<uint32_t> loop = specializedOpenLoop();
    return loop && !m_openLoops[*loop].effects.writes.maskOf(AbstractHeap::Elements);
}

bool Walker::isAllocatedInRestartableIteration(Reference object) const
{
    if (!object || !object.isValue())
        return false;
    ValueID cell = underlyingCellOf(m_code, object.id());
    return !m_code.isConstant(cell) && allocatesNewCell(m_code.at(cell)) && isBuiltInRestartableIteration(Reference::value(cell));
}

bool Walker::isBuiltInRestartableIteration(Reference reference) const
{
    if (!reference || !reference.isValue())
        return false;
    if (std::optional<uint32_t> loop = specializedOpenLoop())
        return !isInvariantInLoop(*loop, reference);
    if (allocatesNewCell(m_code.at(reference.id())) && !m_versionedLoops.isEmpty()) {
        const VersionedLoop& versionedLoop = m_versionedLoops.last();
        for (uint32_t place = m_openLoops.size(); place--;) {
            const OpenLoop& loop = m_openLoops[place];
            if (loop.frame == versionedLoop.frameIndex && m_frames[loop.frame].index->loop(loop.loop).head == versionedLoop.inputHead)
                return !isInvariantInLoop(place, reference);
        }
    }
    return false;
}

bool Walker::leavesOutQuestionThatEveryTurnWouldAsk(QuestionIsAsked isAsked) const
{
    if (!m_walksCopyWithoutQuestionsOfEveryTurn || m_versionedLoops.isEmpty())
        return false;
    std::optional<uint32_t> place = innermostOpenLoop();
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (isAsked == QuestionIsAsked::InCopiedCalleesAlone && m_frames.size() - 1 <= versionedLoop.frameIndex)
        return false;
    if (!place || m_openLoops[*place].frame != versionedLoop.frameIndex || m_frames[versionedLoop.frameIndex].index->loop(m_openLoops[*place].loop).head != versionedLoop.inputHead)
        return true;
    HoistPoint placeInText = currentHoistPoint();
    placeInText.block = { };
    return passesOnEveryIteration(versionedLoop, UnrolledIterations::AsWritten) && executesOnEveryIteration(*place, placeInText, UnrolledIterations::AsWritten);
}

bool Walker::tryBranchInPreheader(Reference test, bool passesWithYes, IterationPurity purity, GuardedIterations guardedIterations)
{
    std::optional<uint32_t> place = specializedOpenLoop(purity);
    if (!place || !test || !test.isValue() || !isInvariantInLoop(*place, test) || representationOf(test) != Representation::Boolean)
        return false;
    VersionedLoop& versionedLoop = m_versionedLoops.last();
    HoistPoint placeInText = currentHoistPoint();
    placeInText.block = { };
    bool asksAllIterations = guardedIterations == GuardedIterations::All || guardedIterations == GuardedIterations::AllBesideCopy;
    UnrolledIterations unrolledIterations = asksAllIterations || isInUnswitchedVersion() || m_walksCopyWithoutSomeIterationGuesses ? UnrolledIterations::AsPassed : UnrolledIterations::AsWritten;
    bool dominatesBackEdges = passesOnEveryIteration(versionedLoop, unrolledIterations) && executesOnEveryIteration(*place, placeInText, unrolledIterations);
    bool everyIterationChecksKindOutsideCopy = guardedIterations == GuardedIterations::AllBesideCopy && dominatesBackEdges && versionedLoop.startOfCopyWithoutSomeIterationGuesses;
    bool someTurnsAloneAskBesideCopy = !asksAllIterations && !dominatesBackEdges && versionedLoop.startOfCopyWithoutSomeIterationGuesses;
    bool everyTurnAsksForNumberBesideCopy = !asksAllIterations && dominatesBackEdges && versionedLoop.startOfCopyWithoutSomeIterationGuesses && m_frames.size() - 1 > versionedLoop.frameIndex && m_code.at(test.id()).opcode == Opcode::TypeTest && m_types.at(m_code.at(test.id()).testedType()).isNumber();
    bool everyTurnAsksForLengthOfViewBesideCopy = !asksAllIterations && dominatesBackEdges && versionedLoop.startOfCopyWithoutSomeIterationGuesses && m_code.at(test.id()).opcode == Opcode::Less
        && m_code.at(m_code.valueBehindNames<valuePreservingOpcodes>(m_code.at(test.id()).operand(OperandIndex::Less::right))).opcode == Opcode::GetTypedArrayLength;
    bool failingWayLeadsToCopyBeside = everyIterationChecksKindOutsideCopy || someTurnsAloneAskBesideCopy || everyTurnAsksForNumberBesideCopy || everyTurnAsksForLengthOfViewBesideCopy;
    if (guardedIterations == GuardedIterations::SomeWithUnguardedVersion && !dominatesBackEdges && !someTurnsAloneAskBesideCopy)
        return false;
    if ((asksAllIterations || m_code.at(m_block).frequency != BlockFrequency::Normal) && !dominatesBackEdges)
        return false;
    if (m_openLoops[*place].reloadBlocks.contains(m_code.blockOf(test.id())))
        return !someTurnsAloneAskBesideCopy && branchInFrontOfQuietTurns(*place, test, passesWithYes);
    if (failingWayLeadsToCopyBeside)
        versionedLoop.guessOfSomeTurnsIsAskedInFront = true;
    if (everyTurnAsksForNumberBesideCopy || everyTurnAsksForLengthOfViewBesideCopy)
        versionedLoop.questionOfEveryTurnIsAskedInFront = true;
    OpenLoop& loop = m_openLoops.changeable(*place);
    BlockID front = loop.preheaderBlock;
    BlockID next = m_code.addBlock(BlockRole::Ordinary, m_code.at(front).frequency);
    m_code.setLoopMarks(next, m_code.at(front).isInLoop, false);
    BlockID failing = failingWayLeadsToCopyBeside ? versionedLoop.startOfCopyWithoutSomeIterationGuesses : versionedLoop.genericStart;

    TerminatorDraft terminator(TerminatorKind::Branch, loop.origin);
    terminator.setConditionOperand(test);
    m_code.end(front, terminator, 2);
    m_code.closeEdge(m_code.at(front).terminator.taken(), passesWithYes ? next : failing, { });
    m_code.closeEdge(m_code.at(front).terminator.notTaken(), passesWithYes ? failing : next, { });

    while (m_preheaderDepths.size() <= next.index())
        m_preheaderDepths.append(0);
    setPreheaderDepth(next, *place + 1);
    loop.earlierPreheaderBlocks.append(front);
    loop.preheaderBlock = next;
    return true;
}

std::optional<PassedTest> Walker::tryHoistConditionalCheck(Reference test, bool passesWithYes, IterationPurity purity)
{
    if (!tryBranchInPreheader(test, passesWithYes, purity, GuardedIterations::SomeWithUnguardedVersion))
        return std::nullopt;
    return PassedTest(test.id(), passesWithYes);
}

Walker::SpecializedPreheaderScope::SpecializedPreheaderScope(Walker& walker, bool shouldHoist, IterationPurity purity)
    : m_walker(walker)
    , m_isActive(shouldHoist)
{
    std::optional<uint32_t> loop = m_isActive ? walker.specializedOpenLoop(purity) : std::nullopt;
    m_isActive = !!loop;
    if (!m_isActive)
        return;
    m_savedBlock = std::exchange(walker.m_block, walker.m_openLoops[*loop].preheaderBlock);
    m_numberOfLoopsThatMightBeLeft = std::exchange(walker.m_numberOfLoopsThatMayBeLeft, *loop);
}

Walker::SpecializedPreheaderScope::~SpecializedPreheaderScope()
{
    if (!m_isActive)
        return;
    m_walker.m_block = m_walker.m_hasFailed ? BlockID() : m_savedBlock;
    m_walker.m_numberOfLoopsThatMayBeLeft = m_numberOfLoopsThatMightBeLeft;
}

bool Walker::someOperationOfRowThatRanOtherCodeIsCall(const BodyIndex& index) const
{
    return std::ranges::any_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
        return ran && effectRuleOf(index.value(ran).opcode()) == EffectRule::Call;
    });
}

bool Walker::canUseIterationGuards(BlockID inputHead)
{
    if (m_coldCopyDepth || !m_opensGuesses || !has(Mechanism::Guesses) || (isUnderHandler() && !mayVersionLoopUnderHandler(inputHead)) || hasStringsInBuilders())
        return false;
    if (m_restartablePreviewHeader == inputHead)
        return true;
    return !m_versionedLoops.isEmpty() && m_versionedLoops.last().canUseIterationGuards && m_versionedLoops.last().frameIndex == m_frames.size() - 1 && m_versionedLoops.last().inputHead == inputHead;
}

bool Walker::textNamesHolderOfCopyAtWork(ValueID written) const
{
    if (m_holderThatIsNoBuiltin.frameIndex != m_frames.size() - 1 || !written)
        return false;
    ValueID behindCasts = currentBodyIndex().stripCasts(written);
    if (std::ranges::contains(m_holderThatIsNoBuiltin.names, written) || std::ranges::contains(m_holderThatIsNoBuiltin.names, behindCasts))
        return true;
    const BodyIndex& index = currentBodyIndex();
    if (ValueID variable = m_holderThatIsNoBuiltin.readOfVariableInLoop; variable && behindCasts && !index.isParameter(behindCasts)) {
        BodyValue read = index.value(behindCasts);
        if (ValueID checked = read.opcode() == Opcode::CheckInitialized ? index.stripCasts(read.operand(0)) : ValueID(); checked && !index.isParameter(checked))
            read = index.value(checked);
        BodyValue ofHolder = index.value(variable);
        if (read.opcode() == ofHolder.opcode())
            return ofHolder.opcode() == Opcode::GetModuleVariable ? read.variable() == ofHolder.variable() : isSameScope(program(), index, read.environmentOperand(), ofHolder.environmentOperand()) && read.layout() == ofHolder.layout() && read.field() == ofHolder.field();
    }
    return isHolderOfCopyAtWork(referenceBoundTo(behindCasts ? behindCasts : written));
}

bool Walker::isHolderOfCopyAtWork(Reference value) const
{
    if (!value || !value.isValue())
        return false;
    ValueID root = underlyingCellOf(m_code, value.id());
    if (m_holderThatIsNoBuiltin.value && m_holderThatIsNoBuiltin.value.isValue()) {
        ValueID rootOfHolder = underlyingCellOf(m_code, m_holderThatIsNoBuiltin.value.id());
        if (rootOfHolder == root)
            return true;
        if (m_holderThatIsNoBuiltin.readOfVariableInLoop && !m_code.isConstant(root) && !m_code.isConstant(rootOfHolder) && m_code.at(root).opcode == Opcode::GetModuleVariable && m_code.at(rootOfHolder).opcode == Opcode::GetModuleVariable && m_code.at(root).variable() == m_code.at(rootOfHolder).variable())
            return true;
    }
    if (m_holderThatIsNoBuiltin.frameIndex >= m_frames.size())
        return false;
    return std::ranges::any_of(m_holderThatIsNoBuiltin.names, [&](ValueID name) {
        Reference bound = referenceBoundIn(m_frames[m_holderThatIsNoBuiltin.frameIndex], name);
        return bound && bound.isValue() && underlyingCellOf(m_code, bound.id()) == root;
    });
}

bool Walker::canBuildIterationGuard() const
{
    if (m_looksTakeFormsBehindLooksAsBuilt)
        return true;
    return isIterationEffectFree() && m_versionedLoops.last().canUseIterationGuards;
}

bool Walker::canRestartAtThrow() const
{
    return canBuildIterationGuard() && (m_looksTakeFormsBehindLooksAsBuilt || !mayCatchInIteration() || m_versionedLoops.last().restartsAtCaughtThrows);
}

bool Walker::canBuildIterationGuardAfterStores() const
{
    return canBuildIterationGuard() || (canRestartIteration() && m_versionedLoops.last().canUseIterationGuards);
}

bool Walker::stretchMayRunAsWrittenInstead() const
{
    const StretchWithArmsAsWritten& stretch = m_stretchWithArmsAsWritten;
    return stretch.asWritten && stretch.numberOfFrames == m_frames.size() && !isUnreachable() && m_state.isEffectFreeSince(stretch.restartPoint);
}

void Walker::runStretchAsWrittenInstead()
{
    jumpTo(m_stretchWithArmsAsWritten.asWritten);
}

uint32_t Walker::markRestartPoint()
{
    uint32_t restartPoint = ++m_restartPointCount;
    m_state.setRestartPoint(restartPoint);
    return restartPoint;
}

bool Walker::canRecordUndoableStore() const
{
    return !m_isUndoingStores && canRestartIteration() && m_code.at(m_block).frequency == BlockFrequency::Normal;
}

void Walker::recordPendingUndoableStore(UndoableStore store)
{
    if (m_undoableStores.isEmpty())
        m_undoableStores.append(UndoableStore { });
    store.previous = m_state.newestUndoableStore();
    store.restartPoint = m_versionedLoops.last().restartPoint;
    m_undoableStores.append(store);
    m_pendingUndoableStore = m_undoableStores.size() - 1;
}

void Walker::popVersionedLoop()
{
    m_versionedLoops.removeLast();
    m_undoableStoreStates.clear();
    if (m_versionedLoops.isEmpty())
        m_undoableStores.shrink(0);
}

void Walker::recordUndoableElementStore(Reference list, Reference position, Reference held, ArrayStorage storage)
{
    UndoableStore store;
    store.object = list;
    store.held = held;
    store.position = position;
    store.isOfInt32Storage = storage == ArrayStorage::Int32;
    recordPendingUndoableStore(store);
}

void Walker::recordUndoableAppend(Reference array, Reference position, Reference held, Reference length, ArrayStorage storage, uint32_t previous)
{
    if (m_undoableStores.isEmpty())
        m_undoableStores.append(UndoableStore { });
    UndoableStore store;
    store.object = array;
    store.held = held;
    store.position = position;
    store.length = length;
    store.isOfInt32Storage = storage == ArrayStorage::Int32;
    store.previous = previous;
    store.restartPoint = m_versionedLoops.last().restartPoint;
    m_undoableStores.append(store);
    m_state.recordUndoableStore(store.restartPoint, m_undoableStores.size() - 1);
    saveUndoableStoreState(m_undoableStores.size() - 1);
}

void Walker::recordUndoableFieldStore(Reference heir, LayoutID layout, uint32_t field, Reference held)
{
    UndoableStore store;
    store.object = heir;
    store.held = held;
    store.layoutOfHeir = layout;
    store.fieldOfHeir = field;
    recordPendingUndoableStore(store);
}

void Walker::recordUndoableGlobalStore(PropertyKeyID key, Reference address, Reference held)
{
    UndoableStore store;
    store.object = address;
    store.held = held;
    store.key = key;
    store.isOfGlobalObject = true;
    recordPendingUndoableStore(store);
}

void Walker::recordUndoableKeyedStore(Reference address, Reference object, Reference held)
{
    UndoableStore store;
    store.object = object;
    store.held = held;
    store.address = address;
    recordPendingUndoableStore(store);
}

void Walker::recordUndoableScopeStore(Reference scope, FieldReference field, Reference held)
{
    for (uint32_t place = m_state.newestUndoableStore(); place; place = m_undoableStores[place].previous) {
        const UndoableStore& before = m_undoableStores[place];
        if (before.isOfScope && before.object == scope && before.layoutOfHeir == field.layout && before.fieldOfHeir == field.field) {
            m_pendingUndoableStore = m_state.newestUndoableStore();
            return;
        }
    }
    UndoableStore store;
    store.object = scope;
    store.held = held;
    store.layoutOfHeir = field.layout;
    store.fieldOfHeir = field.field;
    store.isOfScope = true;
    recordPendingUndoableStore(store);
}

void Walker::recordUndoableVariableStore(VariableID variable, Reference held)
{
    for (uint32_t place = m_state.newestUndoableStore(); place; place = m_undoableStores[place].previous) {
        if (m_undoableStores[place].variable == variable) {
            m_pendingUndoableStore = m_state.newestUndoableStore();
            return;
        }
    }
    UndoableStore store;
    store.held = held;
    store.variable = variable;
    recordPendingUndoableStore(store);
}

bool Walker::isDefinedOnPath(Reference object, const WalkState& state) const
{
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    for (uint32_t place = m_openLoops.size(); place--;) {
        const OpenLoop& loop = m_openLoops[place];
        if (loop.frame == versionedLoop.frameIndex && m_frames[loop.frame].index->loop(loop.loop).head == versionedLoop.inputHead && isInvariantInLoop(place, object))
            return true;
    }
    return !state.listSlotsOf(object).isEmpty();
}

std::optional<Walker::MergedUndoableStores> Walker::mergeUndoableStores(std::span<const uint32_t> edges)
{
    if (edges.size() < 2 || m_versionedLoops.isEmpty() || !m_versionedLoops.last().isEntered)
        return std::nullopt;
    uint32_t restartPoint = m_versionedLoops.last().restartPoint;
    MergedUndoableStores merged;
    merged.commonStore = m_openEdges[edges[0]].state.newestUndoableStore();
    for (uint32_t place : edges) {
        const WalkState& state = m_openEdges[place].state;
        if (!state.canRestartFrom(restartPoint))
            return std::nullopt;
        for (uint32_t other = state.newestUndoableStore(); other != merged.commonStore;) {
            uint32_t& newer = merged.commonStore > other ? merged.commonStore : other;
            newer = m_undoableStores[newer].previous;
        }
    }
    auto forEachStoreOnPath = [&](size_t path, NOESCAPE const auto& functor) {
        for (uint32_t place = m_openEdges[edges[path]].state.newestUndoableStore(); place != merged.commonStore; place = m_undoableStores[place].previous)
            functor(place);
    };
    Vector<uint32_t, 8> stores;
    for (size_t path = 0; path < edges.size(); ++path)
        forEachStoreOnPath(path, [&](uint32_t place) { stores.append(place); });
    std::ranges::sort(stores);
    stores.shrink(std::ranges::unique(stores).begin() - stores.begin());
    if (stores.isEmpty() || std::ranges::any_of(stores, [&](uint32_t place) { return m_undoableStores[place].position || m_undoableStores[place].layoutOfHeir || m_undoableStores[place].isOfGlobalObject || m_undoableStores[place].address || m_undoableStores[place].variable; }))
        return std::nullopt;

    struct StoresUnderKey {
        uint32_t first;
        bool shareRecord { true };
        size_t path { notFound };
        size_t record { notFound };
    };
    Vector<uint32_t, 8> byKey(stores);
    std::ranges::stable_sort(byKey, { }, [&](uint32_t place) { return m_undoableStores[place].key.bits(); });
    Vector<StoresUnderKey, 4> keys;
    Vector<uint32_t, 8> keyOfStore(stores.size());
    auto indexOfStore = [&](uint32_t place) { return std::ranges::lower_bound(stores, place) - stores.begin(); };
    for (uint32_t place : byKey) {
        const UndoableStore& store = m_undoableStores[place];
        if (keys.isEmpty() || m_undoableStores[keys.last().first].key != store.key)
            keys.append({ place });
        const UndoableStore& first = m_undoableStores[keys.last().first];
        keys.last().shareRecord &= store.object == first.object && store.receivers == first.receivers && store.mode == first.mode && store.mayHoldNothing == first.mayHoldNothing;
        keyOfStore[indexOfStore(place)] = keys.size() - 1;
    }
    for (size_t path = 0; path < edges.size(); ++path) {
        forEachStoreOnPath(path, [&](uint32_t place) {
            StoresUnderKey& key = keys[keyOfStore[indexOfStore(place)]];
            key.shareRecord &= std::exchange(key.path, path) != path;
        });
    }
    Vector<size_t, 8> recordOfStore(stores.size());
    for (size_t index = 0; index < stores.size(); ++index) {
        StoresUnderKey& key = keys[keyOfStore[index]];
        if (!key.shareRecord || key.record == notFound) {
            key.record = merged.stores.size();
            merged.stores.append({ m_undoableStores[stores[index]], Vector<Reference, 4>(edges.size()) });
        }
        recordOfStore[index] = key.record;
    }
    static constexpr size_t maxValuesPerStore = 4;
    if (merged.stores.size() * edges.size() > maxValuesPerStore * (stores.size() + edges.size()))
        return std::nullopt;
    for (size_t path = 0; path < edges.size(); ++path) {
        forEachStoreOnPath(path, [&](uint32_t place) {
            merged.stores[recordOfStore[indexOfStore(place)]].heldOnPaths[path] = m_undoableStores[place].held;
        });
    }
    Reference nothing = constant(Draft(Opcode::Absent));
    for (size_t path = 0; path < edges.size(); ++path) {
        Reference definedObject;
        for (MergedUndoableStore& field : merged.stores) {
            if (field.heldOnPaths[path])
                continue;
            if (field.store.object != definedObject && !isDefinedOnPath(field.store.object, m_openEdges[edges[path]].state))
                return std::nullopt;
            definedObject = field.store.object;
            field.heldOnPaths[path] = nothing;
            field.store.mayHoldNothing = true;
        }
    }
    count(StatisticID::UndoableStoresMergedAtJoins);
    return merged;
}

void Walker::recordMergedUndoableStores(const MergedUndoableStores& merged)
{
    uint32_t restartPoint = m_versionedLoops.last().restartPoint;
    Vector<uint32_t, 2> places;
    for (const MergedUndoableStore& field : merged.stores) {
        UndoableStore store = field.store;
        store.previous = m_state.newestUndoableStore();
        store.restartPoint = restartPoint;
        m_undoableStores.append(store);
        places.append(m_undoableStores.size() - 1);
        m_state.recordUndoableStore(restartPoint, places.last());
    }
    WalkState behindStores = m_state;
    Effects undone = Effects::none();
    for (const MergedUndoableStore& field : merged.stores)
        undone.writes.add(program(), MemoryLocation::objectField(field.store.key));
    behindStores.invalidate(undone);
    for (uint32_t place : places) {
        behindStores.recordUndoableStore(restartPoint, place);
        m_undoableStoreStates.append({ place, behindStores });
    }
}

void Walker::letRestOfStretchRunAsWrittenBehindFirstStore(ValueID store)
{
    StretchWithArmsAsWritten& stretch = m_stretchWithArmsAsWritten;
    if (!stretch.asWritten || stretch.restAsWritten || stretch.restartsIterationOnFailure || stretch.numberOfFrames != m_frames.size() || isUnreachable() || store.index() < stretch.first || store.index() - stretch.first + 1 >= stretch.size)
        return;
    stretch.restAsWritten = addBlock(BlockFrequency::Unlikely);
    stretch.placeOfRest = store.index() - stretch.first + 1;
    for (uint32_t place = 0; place < stretch.placeOfRest; ++place)
        stretch.boundInFrontOfRest.append(referenceBoundTo(ValueID(stretch.first + place)));
    stretch.asWritten = stretch.restAsWritten;
    stretch.restartPoint = markRestartPoint();
    count(StatisticID::PartialRegionFallbacksEmitted);
}

void Walker::restartIterationAtThrow()
{
    if (m_versionedLoops.isEmpty() || !mayCatchInIteration()) {
        restartIteration();
        return;
    }
    size_t place = m_versionedLoops.size() - 1;
    const VersionedLoop& versionedLoop = m_versionedLoops[place];
    std::pair<uint32_t, BlockID> left { static_cast<uint32_t>(versionedLoop.frameIndex), versionedLoop.inputHead };
    if (!m_loopsLeftAtCaughtThrows.contains(left))
        m_loopsLeftAtCaughtThrows.append(left);
    BlockID genericEntry = versionedLoop.entry;
    const LoopReentry* reentry = findReentry(versionedLoop.frameIndex, versionedLoop.inputHead);
    if (reentry && !m_state.newestUndoableStore() && canEnterWith(reentry->caughtThrowEntry, versionedLoop.valuesAtHeader.span()))
        m_versionedLoops[place].entry = reentry->caughtThrowEntry;
    restartIteration();
    m_versionedLoops[place].entry = genericEntry;
}

void Walker::restartIteration()
{
    if (!m_hasFailed && !isUnreachable() && !m_inputs.options.checksEffects && !m_state.canRestartFrom(m_versionedLoops.last().restartPoint)) {
        fail("a loop iteration that cannot be undone restarts in the generic version"_s, InternalErrorKind::IterationCannotBeUndone);
        return;
    }
    if (m_state.restartMark() & WalkState::undoableStoresBit) {
        for (size_t place = 0; place < m_versionedLoops.last().moduleVariablesAtHeader.size(); ++place) {
            auto [variable, held] = m_versionedLoops.last().moduleVariablesAtHeader[place];
            Draft store(Opcode::PutModuleVariable, m_origin);
            store.setVariable(variable);
            store.setValueOperand(held);
            chooseConservativeFields(store);
            build(WTF::move(store));
            if (m_hasFailed || isUnreachable())
                return;
            ++m_undoneStoreCount;
        }
    }
    if (uint32_t newest = m_state.newestUndoableStore()) {
        SetForScope undoingStoresScope(m_isUndoingStores, true);
        auto stateAfterStore = [&](uint32_t place) -> const WalkState* {
            auto kept = std::ranges::lower_bound(m_undoableStoreStates, place, { }, [](const auto& state) { return state.first; });
            return kept != m_undoableStoreStates.end() && kept->first == place ? &kept->second : nullptr;
        };
        auto blockOf = [&](uint32_t place) {
            const auto& known = m_versionedLoops.last().undoBlocks;
            auto found = std::ranges::lower_bound(known, place, { }, [](const auto& block) { return block.first; });
            return found != known.end() && found->first == place ? found->second : BlockID();
        };
        bool eachStoreHasOneBlock = true;
        for (uint32_t place = newest; place && eachStoreHasOneBlock && !blockOf(place); place = m_undoableStores[place].previous)
            eachStoreHasOneBlock = !!stateAfterStore(place);
        for (uint32_t place = newest; place; place = m_undoableStores[place].previous) {
            if (eachStoreHasOneBlock) {
                if (BlockID known = blockOf(place)) {
                    jumpTo(known, { });
                    m_block = { };
                    m_state.clear();
                    ++m_iterationRestartCount;
                    return;
                }
                BlockID undoBlock = addBlock(BlockFrequency::Rare);
                jumpTo(undoBlock, { });
                if (m_hasFailed || isUnreachable())
                    return;
                auto& known = m_versionedLoops.last().undoBlocks;
                known.insert(std::ranges::lower_bound(known, place, { }, [](const auto& block) { return block.first; }) - known.begin(), std::pair { place, undoBlock });
                m_state = *stateAfterStore(place);
            }
            UndoableStore store = m_undoableStores[place];
            RELEASE_ASSERT(store.restartPoint == m_versionedLoops.last().restartPoint);
            if (store.length) {
                Draft element(Opcode::RestoreArrayStore, m_origin);
                chooseConservativeFields(element);
                element.setArrayOperand(store.object);
                element.setPositionOperand(store.position);
                element.setValueOperand(store.held);
                element.setLengthOperand(store.length);
                element.setArrayStorage(store.isOfInt32Storage ? ArrayStorage::Int32 : ArrayStorage::Double);
                element.setUndoesIterationStore(true);
                build(WTF::move(element));
                if (m_hasFailed || isUnreachable())
                    return;
                ++m_undoneStoreCount;
                continue;
            }
            if (store.position) {
                Draft element(store.isOfInt32Storage ? Opcode::PutOwnInt32Index : Opcode::PutOwnDoubleIndex, m_origin);
                chooseConservativeFields(element);
                element.setArrayOperand(store.object);
                element.setPositionOperand(store.position);
                element.setValueOperand(store.held);
                element.setUndoesIterationStore(true);
                build(WTF::move(element));
                if (m_hasFailed || isUnreachable())
                    return;
                ++m_undoneStoreCount;
                continue;
            }
            if (store.variable) {
                Draft variable(Opcode::PutModuleVariable, m_origin);
                variable.setVariable(store.variable);
                variable.setValueOperand(store.held);
                chooseConservativeFields(variable);
                build(WTF::move(variable));
                if (m_hasFailed || isUnreachable())
                    return;
                ++m_undoneStoreCount;
                continue;
            }
            if (store.isOfScope) {
                if (ValueID allocation = store.object.isValue() ? underlyingCellOf(m_code, store.object.id()) : ValueID(); allocation && !m_code.isConstant(allocation) && allocatesNewCell(m_code.at(allocation)) && isBuiltInRestartableIteration(Reference::value(allocation)))
                    continue;
                Draft field(Opcode::PutEnvironmentField, m_origin);
                chooseConservativeFields(field);
                field.setEnvironmentOperand(store.object);
                field.setValueOperand(store.held);
                field.setLayout(store.layoutOfHeir);
                field.setField(store.fieldOfHeir);
                build(WTF::move(field));
                if (m_hasFailed || isUnreachable())
                    return;
                ++m_undoneStoreCount;
                continue;
            }
            if (store.layoutOfHeir) {
                Draft property(Opcode::PutPropertyOfHeir, m_origin);
                chooseConservativeFields(property);
                property.setObjectOperand(store.object);
                property.setValueOperand(store.held);
                property.setLayout(store.layoutOfHeir);
                property.setField(store.fieldOfHeir);
                property.setUndoesIterationStore(true);
                build(WTF::move(property));
                if (m_hasFailed || isUnreachable())
                    return;
                ++m_undoneStoreCount;
                continue;
            }
            if (store.address) {
                Draft property(Opcode::PutKeyedProperty, m_origin);
                chooseConservativeFields(property);
                property.setAddressOperand(store.address);
                property.setObjectOperand(store.object);
                property.setValueOperand(store.held);
                property.setUndoesIterationStore(true);
                build(WTF::move(property));
                if (m_hasFailed || isUnreachable())
                    return;
                ++m_undoneStoreCount;
                continue;
            }
            if (store.isOfGlobalObject) {
                Draft property(Opcode::PutGlobalBinding, m_origin);
                chooseConservativeFields(property);
                property.setAddressOperand(store.object);
                property.setKey(store.key);
                property.setValueOperand(store.held);
                property.setUndoesIterationStore(true);
                build(WTF::move(property));
                if (m_hasFailed || isUnreachable())
                    return;
                ++m_undoneStoreCount;
                continue;
            }
            if (store.isIntoIterationObject)
                continue;
            BlockID behindStore = store.mayHoldNothing ? addBlock(BlockFrequency::Rare) : BlockID();
            if (store.mayHoldNothing) {
                Draft test(Opcode::TypeTest, m_origin);
                test.setValueOperand(store.held);
                test.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
                Reference holdsNothing = build(WTF::move(test));
                if (m_hasFailed || isUnreachable())
                    return;
                BlockID hasStored = addBlock(BlockFrequency::Rare);
                branch(holdsNothing, behindStore, hasStored);
                continueIn(hasStored);
                Draft narrowed(Opcode::Narrowed, m_origin);
                narrowed.setValueOperand(store.held);
                narrowed.setTestOperand(holdsNothing);
                narrowed.setOperandIndex(OperandIndex::TypeTest::value);
                narrowed.setIsOnTakenEdge(false);
                store.held = build(WTF::move(narrowed));
                if (m_hasFailed || isUnreachable())
                    return;
            }
            Draft draft(Opcode::PutSlot, m_origin);
            chooseConservativeFields(draft);
            draft.setObjectOperand(store.object);
            draft.setValueOperand(store.held);
            draft.setKey(store.key);
            draft.setReceivers(store.receivers);
            draft.setLevel(KnowledgeLevel::Proven);
            draft.setMode(store.mode);
            draft.setIsDefinition(false);
            draft.setUndoesIterationStore(true);
            build(WTF::move(draft));
            if (m_hasFailed || isUnreachable())
                return;
            ++m_undoneStoreCount;
            if (behindStore) {
                jumpTo(behindStore, { });
                if (m_hasFailed || isUnreachable())
                    return;
            }
        }
    }
    if (Reference head = m_versionedLoops.last().effectsCheckBeginning; head && head.isValue()) [[unlikely]] {
        Draft edge(Opcode::CheckIterationRestart, m_origin);
        edge.setBeginningOperand(head);
        build(WTF::move(edge));
        if (m_hasFailed || isUnreachable())
            return;
    }
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    jumpToGenericVersion(inputBlock(versionedLoop.frameIndex, versionedLoop.inputHead).head, versionedLoop.valuesAtHeader.span());
    m_block = { };
    m_state.clear();
    ++m_iterationRestartCount;
}

BitVector Walker::findLoopsThatReckonWithElements(const BodyIndex& index)
{
    BitVector loops;
    if (!index.holdsAnyOf(OpcodeSet { Opcode::ElemGet, Opcode::ElemSet }))
        return loops;
    static constexpr uint32_t fewestStoresThatVersionLargeLoop = 8;
    Vector<uint32_t> numberOfStores;
    auto isNumericOperator = [](Opcode opcode) {
        return opcode != Opcode::Add && allNumericOperators.contains(opcode);
    };
    for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
        LoopID loop = index.loopOf(BlockID(block));
        for (BodyValue operation : index.block(BlockID(block)).values()) {
            if (operation.opcode() == Opcode::ElemSet && loop) {
                ValueID stored = index.stripCasts(operation.operand(OperandIndex::ElemSet::value));
                if (!index.isParameter(stored) && isNumericOperator(index.opcodeOf(stored))) {
                    if (numberOfStores.isEmpty())
                        numberOfStores.fill(0, index.numberOfLoops());
                    if (++numberOfStores[loop.index()] == fewestStoresThatVersionLargeLoop)
                        loops.set(loop.index());
                }
            }
            if (!isNumericOperator(operation.opcode()))
                continue;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                ValueID element = index.stripCasts(operand);
                if (index.isParameter(element) || index.opcodeOf(element) != Opcode::ElemGet)
                    return;
                if (LoopID loopOfElement = index.loopOf(index.blockOf(element)))
                    loops.set(loopOfElement.index());
            });
        }
    }
    return loops;
}

bool Walker::isTooLargeToVersion(const BodyIndex& index, LoopID loop, const BitVector& loopsThatReckonWithElements)
{
    uint32_t numberOfOperations = 0;
    for (BlockID member : index.membersOf(loop)) {
        numberOfOperations += index.block(member).numberOfValues() + 1;
        if (numberOfOperations > maxVersionedLoopSize)
            return !loopsThatReckonWithElements.get(loop.index());
    }
    return false;
}

bool Walker::isTooLargeToVersion(const BodyIndex& index, LoopID loop)
{
    return isTooLargeToVersion(index, loop, m_loopsThatReckonWithElements.ensure(&index, [&] {
        return findLoopsThatReckonWithElements(index);
    }).iterator->value);
}

static bool hasDirectElementStore(const BodyIndex& index, LoopID loop)
{
    if (!index.opcodes().contains(Opcode::TryPutByValDirect))
        return false;
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [](BodyValue operation) {
            return operation.opcode() == Opcode::TryPutByValDirect;
        });
    });
}

bool Walker::hasDirectElementStoreInLoop() const
{
    if (m_versionedLoops.isEmpty())
        return false;
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    const BodyIndex& index = *m_frames[versionedLoop.frameIndex].index;
    return hasDirectElementStore(index, index.loopOf(versionedLoop.inputHead));
}

bool awaitsInSomeIteration(const BodyIndex& index, ValueID suspension)
{
    if (index.opcodeOf(suspension) != Opcode::Await || index.hasIrreducibleFlow())
        return false;
    BlockID block = index.blockOf(suspension);
    LoopID loop = index.loopOf(block);
    return loop && !BackEdgeDominators(index, loop).contains(block);
}

static bool callsInEveryIteration(const BodyIndex& index, LoopID loop)
{
    BackEdgeDominators everyIteration(index, loop);
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return everyIteration.contains(member) && std::ranges::any_of(index.block(member).values(), [](BodyValue operation) {
            return effectRuleOf(operation.opcode()) == EffectRule::Call;
        });
    });
}

bool Walker::mayBuildLoopSeveralTimes(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!has(Mechanism::Guesses) || !m_opensGuesses || !index.isLoopHead(inputHead) || index.hasIrreducibleFlow() || index.isEnteredByHandlerEdge(inputHead))
        return false;
    if (frameIndex && !m_frames[frameIndex].loopDepthAtCall && index.loopDepthOf(inputHead) == 1 && std::ranges::any_of(facts().linked().parametersOf(m_frames[frameIndex].function), &LinkedParameter::isOnlyCalled) && !hasDirectElementStore(index, index.loopOf(inputHead)))
        return false;
    if (drawChoice(DrawnFor::LoopBuiltTwice, currentFunction().index(), inputHead.index(), false) == ChoiceOfCost::No)
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) && !canVersionLoopWithIterations(inputHead))
        return false;

    if (isTooLargeToVersion(index, loop))
        return false;
    if ((index.holdsAnyOf(OpcodeSet { Opcode::IteratorOpen }) || holdsLoopsThatStepByHand(m_frames[frameIndex].function)) && loopHoldsGenerators(index, loop))
        return false;
    bool yieldsWalkTurnsOfConsumer = loopCarriesValuesOfConsumerRound(inputHead);
    if (yieldsWalkTurnsOfConsumer && !isConsumerSmallEnoughToVersion(inputHead, maxVersionedLoopSize))
        return false;
    bool returnsLeaveCopy = frameIndex && !m_frames[frameIndex].returnsInPlaceOfUnit;
    bool bodyMaySuspend = index.holdsAnyOf(opcodesWithSuspendingBoundInTable) || index.holdsAnyOf(OpcodeSet { Opcode::IteratorResume, Opcode::IteratorClose });
    if (!returnsLeaveCopy && !bodyMaySuspend)
        return true;
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        TerminatorKind end = block.terminator().kind();
        if (returnsLeaveCopy && (end == TerminatorKind::Return || end == TerminatorKind::ReturnCall))
            return false;
        if (!bodyMaySuspend)
            continue;
        for (BodyValue operation : block.values()) {
            if (!maySuspendAsWritten(operation) || (yieldsWalkTurnsOfConsumer && operation.opcode() == Opcode::Yield))
                continue;
            if (index.loopOf(member) != loop || !awaitsInSomeIteration(index, operation.id()) || callsInEveryIteration(index, loop))
                return false;
        }
    }
    return true;
}

std::span<const uint32_t> Walker::numbersOfHandlersThatRead(Frame& frame)
{
    std::span<const BlockID> handlerBlocks = handlerBlocksOf(frame);
    if (!handlerBlocks.empty() && frame.numberOfHandlersThatRead.isEmpty()) {
        if (!m_liveness.isComputedFor(*frame.index))
            m_liveness.compute(*frame.index);
        frame.numberOfHandlersThatRead.fill(0, frame.index->numberOfValues());
        m_liveness.countHeadsWhereLive(handlerBlocks, frame.numberOfHandlersThatRead.mutableSpan());
    }
    return frame.numberOfHandlersThatRead.span();
}

uint32_t Walker::numberOfHandlersThatRead(Frame& frame, ValueID value)
{
    return numbersOfHandlersThatRead(frame).empty() ? 0 : frame.numberOfHandlersThatRead[value.index()];
}

static bool mustBeNamedByDefinitionInLoopsBuiltTwice(const BodyIndex& index, ValueID value)
{
    std::optional<BodyUser> user = index.isParameter(value) ? std::nullopt : index.firstUser(value);
    if (!user)
        return false;
    auto namesByDefinition = [&](const auto& reader) {
        bool isFound = false;
        reader.forEachOperand([&](ValueID operand, uint32_t place) {
            isFound |= operand == value && mustBeDefinition(reader.operandKind(place));
        });
        return isFound;
    };
    if (user->kind == BodyUseKind::Operand)
        return namesByDefinition(index.value(user->value));
    if (user->kind == BodyUseKind::TerminatorOperand)
        return namesByDefinition(index.terminator(user->block));
    return user->kind == BodyUseKind::BlockSubject && index.block(user->block).role() == BlockRole::IterationCleanup;
}

static bool belongsToDisposal(const BodyIndex& index, std::span<const BlockID> handlers, BlockID handler)
{
    return std::ranges::any_of(handlers, [&](BlockID cleanup) {
        return index.block(cleanup).role() == BlockRole::DisposalCleanup && (cleanup == handler || index.dominates(cleanup, handler));
    });
}

static BlockID findOuterCloseHandler(Walker& walker, const BodyIndex& index, LoopID loop, ValueID open)
{
    bool isOpenedByPattern = !index.isParameter(open) && (findDestructuredEntriesIteration(walker, index, index.value(open)) || destructuresReturnedLiteral(walker, index, open));
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        if (block.hasHandler() && !index.isInLoop(block.handler().block(), loop) && (isOpenedByPattern ? iteratorClosedLeavingLoop(index, block, loop) : iteratorClosedLeavingFunction(index, block)) == open)
            return block.handler().block();
    }
    return { };
}

std::optional<Vector<ValueID, 8>> Walker::findValuesOfLoopReadBehindIt(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (m_valuesReadBehindLoopFoundLast.body == &index && m_valuesReadBehindLoopFoundLast.inputHead == inputHead)
        return m_valuesReadBehindLoopFoundLast.found;
    m_valuesReadBehindLoopFoundLast.body = &index;
    m_valuesReadBehindLoopFoundLast.inputHead = inputHead;
    m_valuesReadBehindLoopFoundLast.found = std::nullopt;
    m_valuesReadBehindLoopFoundLast.readByHandlersOutside.shrink(0);
    m_valuesReadBehindLoopFoundLast.cleanupsOutside.shrink(0);
    LoopID loop = index.loopOf(inputHead);
    if (!m_liveness.isComputedFor(index))
        m_liveness.compute(index);
    uint32_t firstValue = index.block(inputHead).firstParameter().index();
    ValueID endOfValues = index.block(index.loop(loop).last).endOfValues();
    Vector<ValueID, 8> readBehindLoop;
    BitVector isListed;
    Vector<ValueID, 2> readByHandlersOutside;
    Vector<BlockID, 2> cleanupsOutside;
    bool onlyDestructuresReturnedLiterals = true;
    bool onlyDisposalsRead = true;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (index.isInLoop(target.block(), loop))
                continue;
            m_liveness.forEachLiveAtHead(target.block(), ValueID(firstValue), endOfValues, [&](ValueID live) {
                if (index.isInLoop(index.blockOf(live), loop) && isListed.add(live.index() - firstValue))
                    readBehindLoop.append(live);
            });
        }
    }
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value) {
            uint32_t numberOutsideLoop = numberOfHandlersThatRead(m_frames[frameIndex], ValueID(value));
            if (!numberOutsideLoop)
                continue;
            for (BlockID inside : index.membersOf(loop))
                numberOutsideLoop -= index.block(inside).isHandlerBlock() && m_liveness.isLiveAtHead(ValueID(value), inside);
            if (!numberOutsideLoop)
                continue;
            if (!mustBeNamedByDefinitionInLoopsBuiltTwice(index, ValueID(value))) {
                readByHandlersOutside.append(ValueID(value));
                for (BlockID handler : handlerBlocksOf(m_frames[frameIndex]))
                    onlyDisposalsRead &= index.isInLoop(handler, loop) || !m_liveness.isLiveAtHead(ValueID(value), handler) || belongsToDisposal(index, handlerBlocksOf(m_frames[frameIndex]), handler);
                continue;
            }
            BlockID cleanup = numberOutsideLoop == 1 ? findOuterCloseHandler(*this, index, loop, ValueID(value)) : BlockID();
            if (!cleanup)
                return std::nullopt;
            cleanupsOutside.append(cleanup);
            onlyDestructuresReturnedLiterals = onlyDestructuresReturnedLiterals && destructuresReturnedLiteral(*this, index, ValueID(value));
        }
    }
    if (!readByHandlersOutside.isEmpty() || !cleanupsOutside.isEmpty()) {
        onlyDestructuresReturnedLiterals &= readByHandlersOutside.isEmpty();
        if (!onlyDestructuresReturnedLiterals && !(onlyDisposalsRead && cleanupsOutside.isEmpty()) && turnsOfLoopCallForGuesses(index, loop))
            return std::nullopt;
        for (ValueID value : readBehindLoop) {
            if (mustBeNamedByDefinitionInLoopsBuiltTwice(index, value))
                return std::nullopt;
        }
    }
    for (ValueID value : readByHandlersOutside) {
        ASSERT(value.index() >= firstValue);
        if (isListed.add(value.index() - firstValue))
            readBehindLoop.append(value);
    }
    std::ranges::sort(cleanupsOutside);
    m_valuesReadBehindLoopFoundLast.readByHandlersOutside = WTF::move(readByHandlersOutside);
    m_valuesReadBehindLoopFoundLast.cleanupsOutside = WTF::move(cleanupsOutside);
    m_valuesReadBehindLoopFoundLast.found = readBehindLoop;
    return readBehindLoop;
}

bool Walker::handlersOutsideLoopReadItsValues(BlockID inputHead)
{
    return findValuesOfLoopReadBehindIt(inputHead) && (!m_valuesReadBehindLoopFoundLast.readByHandlersOutside.isEmpty() || !m_valuesReadBehindLoopFoundLast.cleanupsOutside.isEmpty());
}

void Walker::walkCleanupsBesideCopyOfLoop(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    const Frame& frame = m_frames[frameIndex];
    if (m_hasFailed || !frame.duplicatedValues || frame.duplicatedValues->cleanupsWalkedBesideCopiesOfLoops.isEmpty())
        return;
    Vector<BlockID, 2> cleanups;
    for (auto [loop, cleanup] : frame.duplicatedValues->cleanupsWalkedBesideCopiesOfLoops) {
        if (loop == frame.index->loopOf(inputHead))
            cleanups.append(cleanup);
    }
    SetForScope walksCleanup(m_walksCleanupBesideCopyOfLoop, true);
    for (BlockID cleanup : cleanups) {
        if (m_hasFailed || m_frames[frameIndex].blocks[cleanup.index()].firstOpenEdge == noOpenEdge)
            continue;
        walkBlock(cleanup);
        inputBlock(frameIndex, cleanup) = InputBlock { };
    }
}

void Walker::noteValuesThatHandlersOutsideLoopRead(BlockID inputHead)
{
    if (!handlersOutsideLoopReadItsValues(inputHead))
        return;
    count(StatisticID::LoopsVersionedUnderOuterHandlers);
    Frame& frame = m_frames.last();
    const BodyIndex& index = *frame.index;
    LoopID loop = index.loopOf(inputHead);
    Frame::DuplicatedValues& listed = frame.ensureDuplicatedValues();
    listed.cleanupsWalkedBesideCopiesOfLoops.removeAllMatching([&](const std::pair<LoopID, BlockID>& walked) {
        return !frame.loopsBeingVersioned.contains(walked.first);
    });
    listed.valuesBoxedOnEdgesIntoHandlers.removeAllMatching([&](const std::pair<BlockID, ValueID>& boxed) {
        return frame.blocks[boxed.first.index()].isEntered;
    });
    for (BlockID cleanup : m_valuesReadBehindLoopFoundLast.cleanupsOutside)
        listed.cleanupsWalkedBesideCopiesOfLoops.append({ loop, cleanup });
    if (m_valuesReadBehindLoopFoundLast.readByHandlersOutside.isEmpty())
        return;
    if (!m_liveness.isComputedFor(index))
        m_liveness.compute(index);
    for (BlockID member : index.membersOf(loop)) {
        if (!index.block(member).hasHandler() || index.isInLoop(index.block(member).handler().block(), loop))
            continue;
        BlockID handler = index.block(member).handler().block();
        for (ValueID value : m_valuesReadBehindLoopFoundLast.readByHandlersOutside) {
            if (m_liveness.isLiveAtHead(value, handler) && !listed.valuesBoxedOnEdgesIntoHandlers.contains(std::pair { handler, value }))
                listed.valuesBoxedOnEdgesIntoHandlers.append({ handler, value });
        }
    }
}

bool Walker::boxValuesThatHandlerOutsideLoopReads(size_t frameIndex, BlockID handler, const OpenLoop* loopLeft, Vector<std::pair<ValueID, Reference>, 2>& names)
{
    const Frame& frame = m_frames[frameIndex];
    for (auto [reader, value] : frame.duplicatedValues->valuesBoxedOnEdgesIntoHandlers) {
        if (reader != handler || !frame.isDuplicated(value))
            continue;
        Reference named = nameBehindStandIn(frameIndex, value, frame.lastNameOf(value));
        Reference name = loopLeft ? valueEnteringLoop(*loopLeft, named) : named;
        TypeID type = name ? ownTypeOf(name) : TypeID();
        if (!type)
            continue;
        Representation form = boxedFormOf(type);
        if (isSameWord(representationOf(name), form)) {
            if (name != named)
                names.append({ value, name });
            continue;
        }
        m_code.noteBoxForHandler();
        name = convert(name, form, m_origin);
        if (!name)
            return false;
        names.append({ value, name });
    }
    return true;
}

void Walker::endFrontOfLoopLeftOpen()
{
    FrontOfLoopLeftOpen front = std::exchange(m_frontOfLoopLeftOpen, { });
    if (!front.block || m_code.at(front.block).hasEnded())
        return;
    m_code.end(front.block, TerminatorDraft(TerminatorKind::Jump, front.origin), 1);
    m_code.closeEdge(m_code.at(front.block).terminator.next(), front.head, *front.entering);
}

Vector<uint32_t, 8> Walker::nameValuesOnEdgesThatLeavePlainWalk(BlockID inputHead, std::span<const ValueID> readBehindLoop)
{
    Vector<uint32_t, 8> edges;
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);
    Vector<ValueID, 8> listedBehindWalk;
    for (ValueID value : readBehindLoop) {
        if (m_frames[frameIndex].addDuplicatedValue(value))
            listedBehindWalk.append(value);
    }
    Vector<BlockID, 8> entered;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            BlockID to = target.block();
            if (index.isInLoop(to, loop) || entered.contains(to))
                continue;
            entered.append(to);
            if (std::as_const(m_frames)[frameIndex].blocks[to.index()].isEntered)
                continue;
            InputBlock& behindLoop = inputBlock(frameIndex, to);
            behindLoop.firstNarrowerName = narrowerNamesOfDominator;
            for (uint32_t place = behindLoop.firstOpenEdge; place != noOpenEdge; place = std::as_const(m_openEdges)[place].next) {
                OpenEdge& open = m_openEdges[place];
                edges.append(place);
                if (open.block) {
                    open.edge = m_code.endWithJump(open.block, m_origin);
                    open.block = { };
                }
                const Frame& frame = m_frames[frameIndex];
                for (ValueID value : readBehindLoop) {
                    uint32_t slot = frame.duplicatedValues->slotOf(value);
                    if (Reference bound = frame.references[value.index()]; bound && (!open.names.nameIn(slot) || listedBehindWalk.contains(value)))
                        open.names.setNameIn(slot, bound);
                }
            }
        }
    }
    return edges;
}

void Walker::forgetValuesOfLoop(BlockID inputHead)
{
    Frame& frame = m_frames.last();
    const BodyIndex& index = *frame.index;
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        BodyBlock block = index.block(member);
        for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value)
            frame.references.placeOf<NamesOfValues::Writer::ForgetsValuesOfLoop>(value) = Reference();
    }
}

OperatorForm operatorFormFor(Walker&, const Draft&);

static bool sameNamesLeadFromSameHolderInTurn(const BodyIndex& index, ValueID first, ValueID second, uint32_t firstValueOfLoop)
{
    first = index.stripCasts(first);
    second = index.stripCasts(second);
    if (first == second)
        return true;
    if (!first || !second || first.index() < firstValueOfLoop || second.index() < firstValueOfLoop || index.isParameter(first) || index.isParameter(second) || index.opcodeOf(first) != index.opcodeOf(second))
        return false;
    BodyValue one = index.value(first);
    BodyValue other = index.value(second);
    switch (one.opcode()) {
    case Opcode::CheckInitialized:
        return sameNamesLeadFromSameHolderInTurn(index, one.operand(0), other.operand(0), firstValueOfLoop);
    case Opcode::GetModuleVariable:
        return one.variable() == other.variable();
    case Opcode::GetEnvironmentField:
        return sameNamesLeadFromSameHolderInTurn(index, one.environmentOperand(), other.environmentOperand(), firstValueOfLoop) && one.layout() == other.layout() && one.field() == other.field();
    case Opcode::PropGet:
        return !one.isFreeName() && !other.isFreeName() && one.key() == other.key() && sameNamesLeadFromSameHolderInTurn(index, one.objectOperand(), other.objectOperand(), firstValueOfLoop);
    default:
        return false;
    }
}

bool Walker::buildLoopForEachMember(BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!m_buysTwoBuilds && !m_isVersioningDryRun)
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (!m_genericVersionsInsideSpecialized.isEmpty() && m_genericVersionsInsideSpecialized.last() != std::pair { frameIndex, inputHead } && index.loop(loop).numberOfLoopsWithin > 1)
        return false;
    bool sharedCopyOfThisLoopWaits = m_sharedGenericVersions.containsIf([&](const SharedGenericVersion& shared) { return shared.frameIndex == frameIndex && shared.inputHead == inputHead; });
    bool standsInCopyOfLoopAround = !m_sharedGenericVersions.isEmpty();
    if (m_coldCopyDepth || sharedCopyOfThisLoopWaits || (standsInCopyOfLoopAround && index.loop(loop).numberOfLoopsWithin > 1) || (isUnreachable() && !m_isVersioningDryRun) || hasStringsInBuilders())
        return false;
    if (m_frames[frameIndex].loopsBeingVersioned.contains(loop))
        return false;
    IterationsSteppedAtHead steppedAtHead;
    if (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead))) {
        steppedAtHead = takeIterationsSteppedAt(inputHead);
        if (steppedAtHead.asTheyEntered.size() != 1 || standsInCopyOfLoopAround)
            return false;
        const IndexedIteration& entered = steppedAtHead.asTheyEntered[0].second;
        if (!entered.isStringOrArray || !entered.hasProtocolFallback || !entered.isDeadAfterLoop || entered.state || entered.table || entered.contents != IndexedIteration::Contents::List || !entered.list || !entered.list.isValue())
            return false;
    }
    bool tellsTextFromListOfIteration = !steppedAtHead.asTheyEntered.isEmpty();
    std::optional<Vector<ValueID, 8>> readBehindLoop = findValuesOfLoopReadBehindIt(inputHead);
    if (!readBehindLoop)
        return false;
    size_t operatorsMadeInFront = m_operatorsMadeInFrontOfLoops.size();
    ValueID row = isUnreachable() || tellsTextFromListOfIteration ? ValueID() : findRowThatHeadReadsOffListInFront(*this, inputHead, preview);
    if (row && m_isVersioningDryRun)
        return true;
    BlockID rowIsNotFound;
    if (row)
        rowIsNotFound = addBlock();
    RowFoundUnderKey operandsOfRow = row ? readVariablesOfRowInFrontOfLoop(*this, index.value(row), rowIsNotFound, m_origin) : m_rowOfCopyFoundUnderKey;
    SetForScope operandsOfRowInLooks(m_rowOfCopyFoundUnderKey, operandsOfRow);
    if (row) {
        Reference made = isUnreachable() || m_hasFailed ? Reference() : readRowInFrontOfLoop(*this, index.value(row), rowIsNotFound, m_origin);
        if (made && made.isValue() && !isUnreachable())
            noteOperatorMadeInFrontOfLoop(row, made);
    }
    auto goOnWithBothWays = [&] {
        if (!rowIsNotFound || m_hasFailed)
            return;
        BlockID goesOn = addBlock();
        if (!isUnreachable())
            jumpTo(goesOn, { });
        continueIn(rowIsNotFound);
        if (!isUnreachable())
            jumpTo(goesOn, { });
        continueIn(goesOn);
        rowIsNotFound = { };
    };
    Vector<HoistedSpeculation, 4> members;
    if (tellsTextFromListOfIteration) {
        Reference held = steppedAtHead.asTheyEntered[0].second.list;
        members.append(HoistedSpeculation { held, TypeID(TypeTable::string), { }, { } });
        members.append(HoistedSpeculation { held, m_types.builtinClass(BuiltinClasses::Array), { }, { } });
    } else
        members = findMembersToldApartInFrontOfLoop(*this, inputHead, preview);
    m_operatorsMadeInFrontOfLoops.shrink(operatorsMadeInFront);
    if (!members.isEmpty() && members[0].slotReadInLoop && members[0].slotReadInLoop->holderIsProven && !members[0].slotReadInLoop->holderIsAskedWhetherItIsFilled && members[0].slotReadInLoop->slotsThatFindHolder.isEmpty()) {
        BodyValue written = index.value(members[0].slotReadInLoop->read);
        Draft read(Opcode::GetSlot, originOf(written));
        chooseConservativeFields(read);
        read.setObjectOperand(members[0].slotReadInLoop->nameOfHolder);
        read.setKey(keyOfAccessToSlot(program(), written));
        read.setReceivers(members[0].slotReadInLoop->layoutsOfHolder);
        read.setLevel(KnowledgeLevel::Proven);
        TypeID given = resultTypeOf(read);
        if (given && m_types.at(given).hasObjects() && !m_types.at(given).hasUnboundedUnknown() && !m_types.overlap(given, m_types.builtinClass(BuiltinClasses::Array)))
            members.shrink(0);
    }
    bool isToldApart = !members.isEmpty() && (members.size() >= 2 || members[0].nothingWasExpected || members[0].restIsWalkedAsWithoutGuess);
    std::optional<HoistedCounterCheck> counterOfAnyKind = isToldApart || row || standsInCopyOfLoopAround || !steppedAtHead.asTheyEntered.isEmpty() ? std::nullopt : findCounterOfAnyKind(*this, inputHead, preview, entering);
    if (counterOfAnyKind) {
        members.shrink(0);
        members.append(HoistedSpeculation { entering[counterOfAnyKind->position], counterOfAnyKind->tested, { }, { } });
        isToldApart = true;
    }
    if (!isToldApart || !members[0].readOfRowInLoop)
        goOnWithBothWays();
    if (!isToldApart)
        return false;
    if (standsInCopyOfLoopAround && !members[0].readOfRowInLoop) {
        const SharedGenericVersion& around = m_sharedGenericVersions.last();
        bool isFoundInTurnAround = around.frameIndex == frameIndex && std::ranges::any_of(members[0].names, [&](ValueID name) {
            return index.isInLoop(index.blockOf(name), index.loopOf(around.inputHead));
        });
        if (!isFoundInTurnAround)
            return false;
    }
    if (m_isVersioningDryRun)
        return true;
    SetForScope isBought(m_buysTwoBuilds, false);
    SetForScope ordinalOfCopy(m_ordinalOfCopyOfLoop, m_ordinalOfCopyOfLoop * 16 + 3);

    struct StartOfCopy {
        BlockID block;
        WalkState state;
        Reference proof;
        bool isListBehindListOfNumbers { false };
        bool asksOtherHoldersForLists { false };
        TypeID tested { };
        bool expectsOtherHoldersOfItsClass { false };
        bool expectsTablesToBeViews { false };
        bool expectsThoseWithWordOfListToo { false };
        Vector<std::pair<size_t, Reference>, 1> elementsThatEnterAsNumbers { };
        UncheckedKeyHashMap<uint32_t, Reference, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> lengthsReadInFront { };
        bool asksBoundForText { false };
        Vector<std::pair<ValueID, Reference>, 1> charactersReadBeforeLoop { };
        Vector<std::pair<ValueID, Reference>, 1> characterFieldsReadInFront { };
    };
    Vector<std::pair<ValueID, ValueID>, 2> namesOfLengthsReadInFront;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> namesAskedAbout;
    if (!members[0].slotReadInLoop && !members[0].readOfVariableInLoop && !members[0].readOfRowInLoop) {
        uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
        auto isLengthOfHolder = [&](ValueID read) {
            if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet)
                return false;
            BodyValue written = index.value(read);
            return !written.isFreeName() && program().nameClassOf(written.key()) == NameClass::Length && referenceOf(written.objectOperand()) == members[0].value;
        };
        auto nothingSeenIsWrittenUpToLoop = [&](ValueID read) {
            BlockID block;
            for (const BodyIncomingEdge& edge : incomingEdgesOf(index, inputHead)) {
                if (index.isInLoop(edge.from, loop))
                    continue;
                if (block)
                    return false;
                block = edge.from;
            }
            while (block) {
                bool holdsRead = index.blockOf(read) == block;
                for (BodyValue operation : index.block(block).values()) {
                    if (holdsRead && operation.id().index() <= read.index())
                        continue;
                    if (!declaredEffectsOfOperation(program(), operation.opcode(), operation).mayBeRunAgainUnseen())
                        return false;
                    bool someOperandMayBeObject = false;
                    if (operandsMayRunCode(operation.opcode())) {
                        operation.forEachOperand([&](ValueID operand, uint32_t) {
                            if (!operand || isLengthOfHolder(index.stripCasts(operand)))
                                return;
                            Reference built = referenceOf(operand);
                            someOperandMayBeObject |= !built || !built.isRecord() || m_types.isUninformative(typeOf(built)) || m_types.at(typeOf(built)).hasObjects();
                        });
                    }
                    if (someOperandMayBeObject)
                        return false;
                }
                if (holdsRead)
                    return true;
                std::span<const BodyIncomingEdge> arriving = incomingEdgesOf(index, block);
                if (arriving.size() != 1 || index.isLoopHead(block) || index.isEnteredByHandlerEdge(block))
                    return false;
                block = arriving[0].from;
            }
            return false;
        };
        auto noteLengthReadInFront = [&](ValueID name, uint32_t) {
            if (!name || name.index() >= firstValueOfLoop || !namesAskedAbout.add(name.index()).isNewEntry)
                return;
            ValueID read = index.stripCasts(name);
            if (!isLengthOfHolder(read) || !nothingSeenIsWrittenUpToLoop(read))
                return;
            namesOfLengthsReadInFront.append({ name, read });
        };
        for (BlockID member : index.membersOf(loop)) {
            for (BodyValue operation : index.block(member).values())
                operation.forEachOperand(noteLengthReadInFront);
            index.block(member).terminator().forEachOperand(noteLengthReadInFront);
        }
    }
    Origin origin = m_origin;
    Vector<StartOfCopy, 4> starts;
    bool listWasAskedForStorageOfNumbers = false;
    BlockID whereHolderOfSlotFails;
    Reference proofOfHolderOfSlot;
    Vector<Reference, 1> proofsOfHoldersAroundSlot;
    if (std::optional<SlotReadInLoopOffHolderInFront> slot = members[0].slotReadInLoop) {
        whereHolderOfSlotFails = addBlock(BlockFrequency::Rare);
        bool testOfHolderIsMissing = false;
        auto readSlotInFront = [&](Reference holder, ValueID readInLoop, TypeID tested, LayoutSetID layouts, bool holderIsProven) -> std::pair<Reference, Reference> {
            Reference proof = holder;
            if (!holderIsProven || !m_types.at(typeOf(holder)).isObject()) {
                HoistedSpeculation ofHolder { holder, tested, layouts, { }, false, { }, std::nullopt };
                Reference test = buildTestInFrontOfLoop(*this, ofHolder, origin);
                if (!test || !test.isValue() || isUnreachable()) {
                    testOfHolderIsMissing = true;
                    return { };
                }
                BlockID passed = addBlock();
                branch(test, passed, whereHolderOfSlotFails);
                continueIn(passed);
                proof = buildProofInFrontOfLoop(*this, ofHolder, test, origin);
            }
            if (!proof || isUnreachable())
                return { };
            BodyValue written = index.value(readInLoop);
            Draft isFilled(Opcode::IsSlotFilled, originOf(written));
            chooseConservativeFields(isFilled);
            isFilled.setObjectOperand(proof);
            isFilled.setKey(keyOfAccessToSlot(program(), written));
            isFilled.setReceivers(layouts);
            isFilled.setLevel(KnowledgeLevel::Proven);
            Reference holdsIt = build(WTF::move(isFilled));
            if (holdsIt && holdsIt.isValue() && !isUnreachable()) {
                BlockID filled = addBlock();
                branch(holdsIt, filled, whereHolderOfSlotFails);
                continueIn(filled);
            }
            if (!holdsIt || isUnreachable())
                return { proof, Reference() };
            Draft read(Opcode::GetSlot, originOf(written));
            chooseConservativeFields(read);
            read.setObjectOperand(proof);
            read.setKey(keyOfAccessToSlot(program(), written));
            read.setReceivers(layouts);
            read.setLevel(KnowledgeLevel::Proven);
            return { proof, build(WTF::move(read)) };
        };
        Reference given = slot->nameOfHolder;
        for (const SlotThatFindsHolderOfSlotReadInLoop& around : slot->slotsThatFindHolder) {
            if (!given || !given.isValue())
                break;
            auto [proof, found] = readSlotInFront(given, around.read, around.testedOfHolder, around.layoutsOfHolder, around.holderIsProven && !around.holderIsAskedWhetherItIsFilled);
            proofsOfHoldersAroundSlot.append(proof);
            given = found;
        }
        if (given && given.isValue() && !isUnreachable()) {
            auto [proof, found] = readSlotInFront(given, slot->read, slot->testedOfHolder, slot->layoutsOfHolder, slot->holderIsProven && !slot->holderIsAskedWhetherItIsFilled);
            proofOfHolderOfSlot = proof;
            given = found;
        } else
            given = { };
        if (testOfHolderIsMissing && proofsOfHoldersAroundSlot.isEmpty() && !proofOfHolderOfSlot)
            return false;
        if (m_hasFailed)
            return true;
        if (given && given.isValue() && !isUnreachable() && m_types.at(typeOf(given)).hasObjects() && !m_types.at(typeOf(given)).hasUnboundedUnknown()) {
            members.removeAllMatching([&](const HoistedSpeculation& member) {
                return !m_types.overlap(member.tested, typeOf(given));
            });
        }
        if (!given || !given.isValue() || isUnreachable() || members.isEmpty()) {
            BlockID joined = addBlock();
            if (!isUnreachable())
                jumpTo(joined, { });
            continueIn(whereHolderOfSlotFails);
            jumpTo(joined, { });
            continueIn(joined);
            return false;
        }
        for (HoistedSpeculation& member : members)
            member.value = given;
    }
    for (const HoistedSpeculation& member : members) {
        Reference test = isUnreachable() ? Reference() : counterOfAnyKind ? buildTestOfNumber(*this, member.value, member.tested, origin) : buildTestInFrontOfLoop(*this, member, origin);
        if (!test)
            continue;
        BlockID passed = addBlock();
        BlockID next = addBlock();
        branch(test, passed, next);
        continueIn(passed);
        Reference proof = counterOfAnyKind ? buildProofOfNumber(*this, member.value, test, origin) : buildProofInFrontOfLoop(*this, member, test, origin);
        const ClassPart* part = proof && proof.isValue() && !isUnreachable() && m_types.at(typeOf(proof)).isObject() ? singleBuiltinClassOf(m_types, m_types.at(typeOf(proof))) : nullptr;
        if (part && pristineTestOf(program().builtins(), part->builtinClass) == PristineTest::InstanceStructure) {
            PristineAspectSet aspects { PristineAspects::Names };
            if (program().admitsReplacedBuiltins)
                aspects.add(PristineAspects::CellAlone);
            Draft isToldByItsStructure(Opcode::IsPristine, origin);
            chooseConservativeFields(isToldByItsStructure);
            isToldByItsStructure.setValueOperand(proof);
            isToldByItsStructure.setBuiltinClass(part->builtinClass);
            isToldByItsStructure.setAspects(aspects);
            Reference hasStructure = build(WTF::move(isToldByItsStructure));
            if (hasStructure && hasStructure.isValue() && !isUnreachable()) {
                BlockID hasIt = addBlock();
                branch(hasStructure, hasIt, next);
                continueIn(hasIt);
            }
        }
        bool isList = part && part->builtinClass == BuiltinClasses::Array;
        bool widerListFollows = std::ranges::any_of(members, [&](const HoistedSpeculation& other) {
            return &other > &member && other.tested != member.tested && m_types.isSubset(member.tested, other.tested);
        });
        if (isList && !isUnreachable() && isDeclaredToHoldNumbersAlone(program(), m_types, typeOf(proof)) && (widerListFollows || member.restIsWalkedAsWithoutGuess)) {
            Draft numericStorageCheck(Opcode::HasArrayStorage, origin);
            chooseConservativeFields(numericStorageCheck);
            numericStorageCheck.setArrayOperand(proof);
            numericStorageCheck.setArrayStorage(ArrayStorage::Double);
            numericStorageCheck.setAdmitsInt32Storage(true);
            numericStorageCheck.setIsWritable(false);
            Reference hasKind = build(WTF::move(numericStorageCheck));
            if (hasKind && hasKind.isValue() && !isUnreachable()) {
                BlockID hasIt = addBlock();
                branch(hasKind, hasIt, next);
                continueIn(hasIt);
                listWasAskedForStorageOfNumbers = true;
                isList = false;
            }
        }
        Vector<std::pair<size_t, Reference>, 1> elementsThatEnterAsNumbers;
        if (counterOfAnyKind && proof)
            elementsThatEnterAsNumbers.append({ counterOfAnyKind->position, proof });
        for (size_t place = 0; part && part->builtinClass == BuiltinClasses::Array && !isList && place < entering.size() && !isUnreachable(); ++place) {
            Reference element = entering[place];
            if (!element || !element.isValue() || !(m_code.at(element.id()).opcode == Opcode::ElemGet ? m_code.at(element.id()).objectOperand() == member.value.id() : m_code.at(element.id()).opcode == Opcode::GetArrayElement && m_code.at(element.id()).arrayOperand() == member.value.id()))
                continue;
            HoistedSpeculation isNumber { element, TypeID(TypeTable::number), { }, { } };
            Reference holdsNumber = buildTestInFrontOfLoop(*this, isNumber, origin);
            if (!holdsNumber || !holdsNumber.isValue())
                continue;
            BlockID holdsIt = addBlock();
            branch(holdsNumber, holdsIt, next);
            continueIn(holdsIt);
            if (Reference number = buildProofInFrontOfLoop(*this, isNumber, holdsNumber, origin)) {
                elementsThatEnterAsNumbers.append({ place, number });
                count(StatisticID::EnteringElementsGuardedAsNumbers);
            }
        }
        UncheckedKeyHashMap<uint32_t, Reference, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> lengthsReadInFront;
        for (auto [name, read] : namesOfLengthsReadInFront) {
            if (!proof || !proof.isValue() || isUnreachable() || m_hasFailed || !part || part->builtinClass != BuiltinClasses::Array)
                break;
            if (lengthsReadInFront.contains(read.index()))
                continue;
            Reference asRead = referenceOf(read);
            if (!asRead || !asRead.isValue() || m_types.at(typeOf(asRead)).isInteger())
                continue;
            BodyValue written = index.value(read);
            Reference holderBefore = referenceBoundTo(written.objectOperand());
            Reference readBefore = referenceBoundTo(read);
            bind(written.objectOperand(), proof);
            Reference length;
            {
                ClosedArm takesNamesBack(*this);
                walkOperation(written);
                length = referenceOf(read);
            }
            bind(written.objectOperand(), holderBefore);
            bind(read, readBefore);
            if (m_hasFailed)
                return true;
            if (!length || !length.isValue() || length == asRead || isUnreachable() || !m_types.at(typeOf(length)).isInteger())
                continue;
            Reference isInt32 = buildTestOfNumber(*this, asRead, TypeTable::int32, origin);
            if (!isInt32 || !isInt32.isValue())
                continue;
            BlockID isOne = addBlock();
            branch(isInt32, isOne, next);
            continueIn(isOne);
            Draft isStillLength(Opcode::StrictEq, origin);
            chooseConservativeFields(isStillLength);
            isStillLength.setLeftOperand(buildProofOfNumber(*this, asRead, isInt32, origin));
            isStillLength.setRightOperand(length);
            isStillLength.setForm(operatorFormFor(*this, isStillLength));
            Reference isSame = build(WTF::move(isStillLength));
            if (!isSame || !isSame.isValue() || isUnreachable())
                continue;
            BlockID itIs = addBlock();
            branch(isSame, itIs, next);
            continueIn(itIs);
            lengthsReadInFront.add(read.index(), length);
            count(StatisticID::CachedLengthsRevalidatedInPreheaders);
        }
        Vector<std::pair<ValueID, Reference>, 1> charactersReadBeforeLoop;
        Vector<std::pair<ValueID, Reference>, 1> characterFieldsReadInFront;
        bool comparesCharacters = proof && member.tested == TypeID(TypeTable::string) && member.restIsWalkedAsWithoutGuess && !member.readOfVariableInLoop;
        for (ValueID compared : comparesCharacters ? findUnknownsComparedWithElements(*this, inputHead, members[0].names.span()) : Vector<ValueID, 2>()) {
            bool isInFront = compared.index() < index.block(inputHead).firstParameter().index();
            bool isField = !isInFront && !isUnreachable() && mayHoistUnknownField(*this, compared);
            Reference value = isField ? readUnknownFieldAtSite(*this, index.value(index.stripCasts(compared))) : isInFront && !isUnreachable() ? referenceOf(compared) : Reference();
            if (!value || !value.isValue() || isUnreachable() || !m_types.isUninformative(m_types.withoutPrimitives(typeOf(value), Primitive::Absent)))
                continue;
            HoistedSpeculation isString { value, TypeID(TypeTable::string), { }, { } };
            Reference holdsString = buildTestInFrontOfLoop(*this, isString, origin);
            if (!holdsString || !holdsString.isValue())
                continue;
            BlockID holdsIt = addBlock();
            branch(holdsString, holdsIt, next);
            continueIn(holdsIt);
            Reference string = buildProofInFrontOfLoop(*this, isString, holdsString, origin);
            Reference length = string && !isUnreachable() ? buildLengthOfString(*this, string, origin) : Reference();
            if (!length || m_hasFailed)
                continue;
            Reference isOneCharacter = buildOperatorForStrings(*this, Opcode::StrictEq, length, integerForStrings(*this, 1), origin);
            if (!isOneCharacter || !isOneCharacter.isValue() || isUnreachable())
                continue;
            BlockID isOne = addBlock();
            branch(isOneCharacter, isOne, next);
            continueIn(isOne);
            if (Reference character = buildOnlyCharacter(*this, string, origin))
                (isField ? characterFieldsReadInFront : charactersReadBeforeLoop).append({ compared, character });
        }
        BlockID firstStart = m_block;
        bool isOfViewsAlone = m_types.at(member.tested).isObject() && m_types.at(member.tested).hasOnlyClassObjects() && !m_types.overlap(member.tested, m_types.builtinClass(BuiltinClasses::Array));
        bool isListOfNumbers = proof && part && part->builtinClass == BuiltinClasses::Array && isDeclaredToHoldNumbersAlone(program(), m_types, typeOf(proof));
        if (proof && !isUnreachable() && (isOfViewsAlone || isListOfNumbers) && !findTablesThatMayBeViewsForGuesses(*this, inputHead, member.value).isEmpty()) {
            starts.append(StartOfCopy { m_block, m_state, proof, false });
            starts.last().expectsTablesToBeViews = true;
            firstStart = addBlock();
        }
        if (proof && !isUnreachable() && part && part->builtinClass == BuiltinClasses::Array && isDeclaredToHoldNumbersAlone(program(), m_types, typeOf(proof)) && otherHolderOfLoopMayBeListForGuesses(*this, inputHead, member.value)) {
            starts.append(StartOfCopy { firstStart, m_state, proof, isList && listWasAskedForStorageOfNumbers, true });
            starts.append(StartOfCopy { addBlock(), m_state, proof, isList && listWasAskedForStorageOfNumbers });
        } else if (size_t numberOfOthers = proof && !isUnreachable() && (member.tested == TypeID(TypeTable::string) || (m_types.at(member.tested).isObject() && m_types.at(member.tested).hasOnlyClassObjects() && !m_types.overlap(member.tested, m_types.builtinClass(BuiltinClasses::Array)))) ? findOtherHoldersExpectedOfClassOfFirstForGuesses(*this, inputHead, member.value, member.tested, member.tested != TypeID(TypeTable::string)).size() : 0) {
            size_t numberOfReaders = member.tested == TypeID(TypeTable::string) ? numberOfOthers : findOtherHoldersExpectedOfClassOfFirstForGuesses(*this, inputHead, member.value, member.tested).size();
            if (numberOfOthers > numberOfReaders) {
                starts.append(StartOfCopy { firstStart, m_state, proof, false });
                starts.last().expectsOtherHoldersOfItsClass = true;
                starts.last().expectsThoseWithWordOfListToo = true;
                firstStart = addBlock();
            }
            if (numberOfReaders) {
                starts.append(StartOfCopy { firstStart, m_state, proof, false });
                starts.last().expectsOtherHoldersOfItsClass = true;
                firstStart = addBlock();
            }
            starts.append(StartOfCopy { firstStart, m_state, proof, false });
        } else if (proof && !isUnreachable() && m_types.at(member.tested).isObject() && m_types.at(member.tested).hasOnlyClassObjects() && !m_types.overlap(member.tested, m_types.builtinClass(BuiltinClasses::Array)) && otherHolderOfLoopMayBeListForGuesses(*this, inputHead, member.value)) {
            starts.append(StartOfCopy { firstStart, m_state, proof, false, true });
            starts.append(StartOfCopy { addBlock(), m_state, proof, false });
            count(StatisticID::ArraysGuardedBesideTypedArrays);
        } else if (proof && !isUnreachable() && (isOfViewsAlone || isListOfNumbers) && headOnlyMeasuresConstantOfFileForGuesses(*this, inputHead, member.value)) {
            starts.append(StartOfCopy { firstStart, m_state, proof, isList && listWasAskedForStorageOfNumbers, true });
            starts.append(StartOfCopy { addBlock(), m_state, proof, isList && listWasAskedForStorageOfNumbers });
            starts.last().asksBoundForText = true;
        } else if (proof && !isUnreachable())
            starts.append(StartOfCopy { firstStart, m_state, proof, isList && listWasAskedForStorageOfNumbers });
        for (StartOfCopy& start : starts) {
            if (!start.tested) {
                start.tested = member.tested;
                start.elementsThatEnterAsNumbers = elementsThatEnterAsNumbers;
                start.lengthsReadInFront = lengthsReadInFront;
                start.charactersReadBeforeLoop = charactersReadBeforeLoop;
                start.characterFieldsReadInFront = characterFieldsReadInFront;
            }
        }
        continueIn(next);
    }
    goOnWithBothWays();
    if (m_hasFailed)
        return true;
    recordSpeculation();
    ++m_versionedLoopCount;
    m_versionedLoopNestCount += index.loop(loop).numberOfLoopsWithin > 1;
    auto someMemberIs = [&](BuiltinClassID builtinClass) {
        return members.containsIf([&](const HoistedSpeculation& member) { return m_types.isSubset(member.tested, m_types.builtinClass(builtinClass)) || m_types.isSubset(m_types.builtinClass(builtinClass), member.tested); });
    };
    bool tellsListFromBytes = someMemberIs(BuiltinClasses::Array) && someMemberIs(BuiltinClasses::Uint8Array);
    if (tellsListFromBytes)
        count(StatisticID::ArrayOrUint8ArrayLoopsVersioned);
    if (standsInCopyOfLoopAround)
        count(StatisticID::LoopsVersionedInsideOuterVersions);
    if (starts.containsIf([](const StartOfCopy& start) { return start.isListBehindListOfNumbers; }))
        count(StatisticID::LoopsVersionedForAnyArray);
    if (members.containsIf([&](const HoistedSpeculation& member) { return !!classOfViewBesideInstancesBelowIt(*this, member.tested); }))
        count(StatisticID::TypedArraySubclassLoopsVersioned);
    if (someMemberIs(BuiltinClasses::Array) && members.containsIf([&](const HoistedSpeculation& member) { return member.tested == TypeID(TypeTable::string); }))
        count(tellsTextFromListOfIteration ? StatisticID::ForOfLoopsVersionedByIterable : StatisticID::ArrayOrStringLoopsVersioned);
    bool asksForListAlone = members[0].nothingWasExpected && !isDeclaredToHoldNumbersAlone(program(), m_types, members.last().tested) && std::ranges::all_of(members, [&](const HoistedSpeculation& member) {
        return m_types.isSubset(member.tested, m_types.builtinClass(BuiltinClasses::Array));
    });
    if (asksForListAlone)
        count(StatisticID::CounterIndexedLoopsVersioned);

    if (members[0].slotReadInLoop && m_types.at(members[0].tested).hasOnlyClosures())
        count(StatisticID::SlotClosureLoopsVersioned);

    BlockID genericStart = addBlock(BlockFrequency::Rare);
    BlockID startOfRest;
    bool isReadInSomeTurnsOnly = !members[0].names.isEmpty() && !members[0].slotReadInLoop && !members[0].readOfVariableInLoop && !members[0].readOfRowInLoop;
    if (isReadInSomeTurnsOnly) {
        BackEdgeDominators backEdgeDominators(index, loop);
        for (BlockID member : index.membersOf(loop)) {
            if (!backEdgeDominators.contains(member))
                continue;
            auto mentions = [&](ValueID operand, uint32_t) {
                isReadInSomeTurnsOnly &= !members[0].names.contains(operand);
            };
            for (BodyValue operation : index.block(member).values())
                operation.forEachOperand(mentions);
            index.block(member).terminator().forEachOperand(mentions);
        }
    }
    BlockID startForWhatIsNoBuiltin;
    WalkState atEndOfTests;
    bool hasRest = (((tellsListFromBytes || asksForListAlone) && members[0].nothingWasExpected) || isReadInSomeTurnsOnly || members[0].restIsWalkedAsWithoutGuess) && !isUnreachable();
    auto onlyOperatorsAndSuchReadsRanOtherCode = [&] {
        SetForScope asInThatCopy(m_walksCopyForWhatIsNoBuiltin, true);
        SetForScope ofThisHolder(m_holderThatIsNoBuiltin, HolderThatIsNoBuiltin { members[0].names.span(), members[0].value, frameIndex, members[0].readOfVariableInLoop });
        return !m_operationsThatRanOtherCodeOfRow.isEmpty() && std::ranges::all_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
            if (!ran || !index.isInLoop(index.blockOf(ran), loop))
                return false;
            BodyValue operation = index.value(ran);
            return effectRuleOf(operation.opcode()) == EffectRule::Operator || siteMayAnswerReadWithoutOtherCode(*this, operation);
        });
    };
    if (tellsListFromBytes && steppedAtHead.asTheyEntered.isEmpty() && !isUnreachable() && onlyOperatorsAndSuchReadsRanOtherCode()) {
        startForWhatIsNoBuiltin = addBlock(BlockFrequency::Rare);
        atEndOfTests = m_state;
        jumpTo(startForWhatIsNoBuiltin, { });
        starts.append(StartOfCopy { startForWhatIsNoBuiltin, m_state, { } });
    }
    if (hasRest) {
        startOfRest = addBlock();
        if (!startForWhatIsNoBuiltin) {
            atEndOfTests = m_state;
            jumpTo(startOfRest, { });
        }
        starts.append(StartOfCopy { startOfRest, { }, { } });
    }
    if (startForWhatIsNoBuiltin || startOfRest)
        continueIn(genericStart);
    else
        jumpTo(genericStart, { });
    if (whereHolderOfSlotFails && !m_hasFailed) {
        continueIn(whereHolderOfSlotFails);
        jumpTo(genericStart, { });
    }
    if (m_hasFailed)
        return true;
    m_block = genericStart;
    if ((startForWhatIsNoBuiltin || startOfRest) && !whereHolderOfSlotFails)
        m_state = WTF::move(atEndOfTests);
    BlockID genericVersionEntry = addBlock(findReentry(frameIndex, inputHead) ? BlockFrequency::Unlikely : BlockFrequency::Rare);
    m_genericVersionEntries.add(genericVersionEntry.index());
    Vector<Reference, 8> genericEntryParameters;
    for (size_t place = 0; place < entering.size(); ++place) {
        TypeID type = joinTypes(preview.typesOfParameters[place], ownTypeOf(entering[place]));
        genericEntryParameters.append(addParameter(genericVersionEntry, type, formOfParameter(type, { })));
    }
    Vector<Reference, 8> enteringWithPositions(entering);
    Vector<Reference, 1> genericEntryPositions;
    for (const auto& stepped : steppedAtHead.asTheyEntered) {
        TypeID type = m_types.integer(Range { 0, doneIterationPosition });
        genericEntryPositions.append(addParameter(genericVersionEntry, type, formOfParameter(type, { })));
        enteringWithPositions.append(stepped.second.position);
    }
    addStepEntryParameters(*this, inputHead, genericVersionEntry, entering, genericEntryParameters.span(), enteringWithPositions);
    addScopeFieldParameters(genericVersionEntry, preview);
    settleParametersOf(genericVersionEntry);

    for (ValueID value : *readBehindLoop)
        m_frames[frameIndex].addDuplicatedValue(value);
    noteValuesThatHandlersOutsideLoopRead(inputHead);
    m_frames[frameIndex].loopsBeingVersioned.append(loop);
    uint32_t narrowerNamesBeforeLoop = inputBlock(frameIndex, inputHead).firstNarrowerName;
    Vector<Reference, 4> carriedInFrontOfLoop;
    if (loopCarriesValuesOfConsumerRound(inputHead))
        carriedInFrontOfLoop = carriedValuesInFrame(frameIndex);

    forgetAllButFreshObjectsAndFieldsThatBlockTakes(genericVersionEntry);
    WalkState stateBeforeGenericVersion;
    if (startOfRest)
        stateBeforeGenericVersion = m_state;
    enterWithFieldsOfScopes(genericVersionEntry, enteringWithPositions.span());
    for (size_t place = 0; place < genericEntryPositions.size(); ++place)
        m_indexedIterations[steppedAtHead.asTheyEntered[place].first].position = genericEntryPositions[place];
    enterGenericVersion(genericVersionEntry, preview);
    {
        ClosedArm closedArm(*this);
        SetForScope coldCopyScope(m_coldCopyDepth, m_coldCopyDepth + !tellsListFromBytes);
        SetForScope holderIsNoBuiltin(m_walksCopyWhereHolderIsNoBuiltin, m_walksCopyWhereHolderIsNoBuiltin || tellsListFromBytes);
        walkCopyOfLoop(inputHead, genericEntryParameters.span());
    }
    forgetValuesOfLoop(inputHead);

    m_sharedGenericVersions.append(SharedGenericVersion { frameIndex, inputHead, genericStart, genericVersionEntry, false });
    LoopLookedAt lookedAtAfterGenericVersion = m_frames[frameIndex].lookedAt;
    bool turnsOfSomeCopyComeRound = false;
    for (StartOfCopy& start : starts) {
        if (m_hasFailed)
            break;
        if (m_frames[frameIndex].lookedAt.loop != loop)
            m_frames[frameIndex].lookedAt = lookedAtAfterGenericVersion;
        bool isRest = startOfRest && start.block == startOfRest;
        bool isForWhatIsNoBuiltin = startForWhatIsNoBuiltin && start.block == startForWhatIsNoBuiltin;
        if (isRest) {
            m_block = { };
            m_state.clear();
            PendingBlock* pending = findPendingBlock(startOfRest);
            size_t numberOfArrivals = pending ? pending->numberOfArrivals : 0;
            size_t numberOfEdges = 0;
            m_code.forEachIncomingEdgeUnsealed(startOfRest, [&](const Edge&) {
                ++numberOfEdges;
            });
            if (!numberOfEdges)
                continue;
            continueIn(startOfRest);
            if (numberOfEdges > numberOfArrivals) {
                if (numberOfArrivals)
                    m_state.meetWith(stateBeforeGenericVersion);
                else
                    m_state = WTF::move(stateBeforeGenericVersion);
                m_block = startOfRest;
            }
        } else {
            m_block = start.block;
            m_state = WTF::move(start.state);
        }
        WalkState atEndOfTestsForCopy;
        BlockID firstBlockOfCopyForWhatIsNoBuiltin;
        uint32_t firstValueOfCopyForWhatIsNoBuiltin = m_code.numberOfValues();
        if (isForWhatIsNoBuiltin) {
            atEndOfTestsForCopy = m_state;
            firstBlockOfCopyForWhatIsNoBuiltin = addBlock(BlockFrequency::Rare);
            m_block = firstBlockOfCopyForWhatIsNoBuiltin;
        }
        BlockID whereFrontFails = start.asksOtherHoldersForLists || start.expectsOtherHoldersOfItsClass || start.expectsTablesToBeViews ? (&start + 1)->block : startOfRest && !isRest && !isForWhatIsNoBuiltin ? startOfRest : genericStart;
        m_sharedGenericVersions.last().start = whereFrontFails;
        m_sharedGenericVersions.last().otherHoldersAreAskedForLists = start.asksOtherHoldersForLists;
        m_sharedGenericVersions.last().boundIsAskedForText = start.asksBoundForText;
        if (whereFrontFails == startOfRest)
            count(StatisticID::LoopVersionsFailingIntoFallback);
        for (uint32_t number = 0; number < carriedInFrontOfLoop.size(); ++number)
            bind(ValueID(index.numberOfValues() + number), carriedInFrontOfLoop[number]);
        putBackIterationsSteppedAt(steppedAtHead);
        for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
            IndexedIteration& iteration = m_indexedIterations[place];
            bool isText = start.tested == TypeID(TypeTable::string);
            iteration.contents = isText ? IndexedIteration::Contents::Text : IndexedIteration::Contents::List;
            iteration.builtinClass = isText ? BuiltinClasses::String : BuiltinClasses::Array;
            iteration.list = start.proof;
            iteration.hasProtocolFallback = false;
            iteration.isStringOrArray = false;
            iteration.iterator = { };
            iteration.next = { };
            iteration.wayToStep = { };
        }
        Vector<std::pair<ValueID, Reference>, 8> boundBefore;
        for (ValueID name : members[0].names) {
            if (!start.proof)
                break;
            boundBefore.append({ name, referenceBoundTo(name) });
            bind(name, start.proof);
        }
        for (auto [name, read] : namesOfLengthsReadInFront) {
            auto taken = start.lengthsReadInFront.find(read.index());
            if (taken == start.lengthsReadInFront.end())
                continue;
            boundBefore.append({ name, referenceBoundTo(name) });
            bind(name, taken->value);
        }
        for (auto [name, character] : start.charactersReadBeforeLoop) {
            boundBefore.append({ name, referenceBoundTo(name) });
            bind(name, character);
        }
        SetForScope hasHoistedFields(m_hasHoistedFields, m_hasHoistedFields || !start.characterFieldsReadInFront.isEmpty());
        for (auto [name, character] : start.characterFieldsReadInFront) {
            BodyValue field = index.value(index.stripCasts(name));
            noteHoistedUnknownField(*this, field.key(), referenceOf(field.objectOperand()), character);
        }
        if (start.proof && members[0].slotReadInLoop) {
            const SlotReadInLoopOffHolderInFront& slot = *members[0].slotReadInLoop;
            if (!slot.holderIsReadOffVariableInLoop) {
                boundBefore.append({ slot.holder, referenceBoundTo(slot.holder) });
                bind(slot.holder, proofsOfHoldersAroundSlot.isEmpty() ? proofOfHolderOfSlot : proofsOfHoldersAroundSlot[0]);
            } else if (ValueID variableOfHolder = readOfVariableThatFindsReceiverInLoop(program(), index, index.value(slot.slotsThatFindHolder.isEmpty() ? slot.read : slot.slotsThatFindHolder[0].read), index.block(inputHead).firstParameter().index())) {
                BodyValue read = index.value(variableOfHolder);
                Reference filled = proofsOfHoldersAroundSlot.isEmpty() ? proofOfHolderOfSlot : proofsOfHoldersAroundSlot[0];
                if (read.opcode() == Opcode::GetModuleVariable) {
                    if (program().at(read.variable()).initialization != VariableInitialization::Uninitialized || leavesDeadZoneForGood(program(), read.variable()))
                        m_state.noteEndOfDeadZone(read.variable());
                    m_state.noteVariable(read.variable(), filled);
                    noteValueOfStableBinding(*this, read.variable(), filled);
                } else if (Reference environment = loopInvariantScope(*this, index, read.environmentOperand(), index.block(inputHead).firstParameter().index())) {
                    FieldReference field { read.layout(), read.field() };
                    m_state.noteEndOfDeadZone(environment, field);
                    noteEnvironmentField(environment, field, filled);
                    noteValueOfStableBinding(*this, environment, field, filled);
                }
            }
            for (size_t place = 0; place < proofsOfHoldersAroundSlot.size(); ++place)
                m_state.noteSlot(proofsOfHoldersAroundSlot[place], slot.slotsThatFindHolder[place].field, place + 1 < proofsOfHoldersAroundSlot.size() ? proofsOfHoldersAroundSlot[place + 1] : proofOfHolderOfSlot);
            m_state.noteSlot(proofOfHolderOfSlot, slot.field, start.proof);
        }
        bool rowIsFoundUnderKey = start.proof && members[0].readOfRowInLoop && operandsOfRow.holder && operandsOfRow.key && !readTakesIndexOfProvenList(m_types, typeOf(operandsOfRow.holder), typeOf(operandsOfRow.key));
        SetForScope rowOfCopy(m_rowOfCopyFoundUnderKey, start.proof && members[0].readOfRowInLoop ? RowFoundUnderKey { operandsOfRow.holder, operandsOfRow.key, rowIsFoundUnderKey ? start.proof : Reference(), operandsOfRow.variableOfHolder, operandsOfRow.variableOfKey } : RowFoundUnderKey { });
        if (start.proof && members[0].readOfRowInLoop)
            noteRowReadInFrontOfLoop(*this, index.value(members[0].readOfRowInLoop), start.proof);
        if (start.proof && members[0].readOfVariableInLoop) {
            BodyValue read = index.value(members[0].readOfVariableInLoop);
            count(StatisticID::VariableReceiverGuardsHoisted);
            if (read.opcode() == Opcode::GetModuleVariable) {
                m_state.noteEndOfDeadZone(read.variable());
                m_state.noteVariable(read.variable(), start.proof);
                noteValueOfStableBinding(*this, read.variable(), start.proof);
            } else if (Reference environment = loopInvariantScope(*this, index, read.environmentOperand(), index.block(inputHead).firstParameter().index())) {
                FieldReference field { read.layout(), read.field() };
                m_state.noteEndOfDeadZone(environment, field);
                noteEnvironmentField(environment, field, start.proof);
                noteValueOfStableBinding(*this, environment, field, start.proof);
            }
        }
        Vector<std::pair<ValueID, RememberedExpectation>, 2> expectedBefore;
        if (start.expectsOtherHoldersOfItsClass) {
            for (ValueID other : findOtherHoldersExpectedOfClassOfFirstForGuesses(*this, inputHead, members[0].value, start.tested, start.expectsThoseWithWordOfListToo)) {
                RememberedExpectation& remembered = rememberedExpectationOf(other);
                expectedBefore.append({ other, remembered });
                remembered = { start.tested, true, false, true };
            }
        }
        if (start.expectsTablesToBeViews) {
            for (ValueID table : findTablesThatMayBeViewsForGuesses(*this, inputHead, members[0].value)) {
                RememberedExpectation& remembered = rememberedExpectationOf(table);
                expectedBefore.append({ table, remembered });
                remembered = { viewsOfNumbersOfEveryKindForGuesses(m_types), true, false, true };
            }
        }
        if (start.proof && start.tested == TypeID(TypeTable::string) && members[0].restIsWalkedAsWithoutGuess) {
            for (ValueID compared : findUnknownsComparedWithElements(*this, inputHead, members[0].names.span())) {
                RememberedExpectation& remembered = rememberedExpectationOf(compared);
                expectedBefore.append({ compared, remembered });
                remembered = { start.tested, true, false, true };
            }
        }
        if (start.proof && start.tested && members[0].slotReadInLoop) {
            BodyValue written = index.value(members[0].slotReadInLoop->read);
            for (BlockID member : index.membersOf(loop)) {
                for (BodyValue operation : index.block(member).values()) {
                    bool readsSlot = operation.opcode() == Opcode::PrivateGet || (operation.opcode() == Opcode::PropGet && !operation.isFreeName());
                    if (!readsSlot || !sameNamesLeadFromSameHolderInTurn(index, operation.objectOperand(), written.objectOperand(), index.block(inputHead).firstParameter().index()))
                        continue;
                    if (keyOfAccessToSlot(program(), operation) != keyOfAccessToSlot(program(), written) && m_types.at(start.tested).hasOnlyClosures())
                        continue;
                    if (keyOfAccessToSlot(program(), operation) != keyOfAccessToSlot(program(), written) && (members[0].slotReadInLoop->holderIsProven ? !readFindsListThatTurnsReckonWithForGuesses(*this, operation.id()) : !wordOfFieldThatFindsValueIsDrawnFromUseForGuesses(*this, operation.id())))
                        continue;
                    RememberedExpectation& remembered = rememberedExpectationOf(operation.id());
                    expectedBefore.append({ operation.id(), remembered });
                    remembered = { start.tested, true, false, true };
                }
            }
        }
        if (start.proof && start.tested && members[0].readOfVariableInLoop) {
            BodyValue written = index.value(members[0].readOfVariableInLoop);
            for (BlockID member : index.membersOf(loop)) {
                for (BodyValue operation : index.block(member).values()) {
                    if (operation.opcode() != written.opcode())
                        continue;
                    if (written.opcode() == Opcode::GetModuleVariable ? operation.variable() != written.variable() : !isSameScope(program(), index, operation.environmentOperand(), written.environmentOperand()) || operation.layout() != written.layout() || operation.field() != written.field())
                        continue;
                    RememberedExpectation& remembered = rememberedExpectationOf(operation.id());
                    expectedBefore.append({ operation.id(), remembered });
                    remembered = { start.tested, true, false, true };
                }
            }
        }
        SetForScope listAskedInVain(m_listAskedForStorageOfNumbersInVain, start.isListBehindListOfNumbers ? start.proof : Reference());
        SetForScope numericLists(m_walksNumericListCopy, start.proof && members[0].restIsWalkedAsWithoutGuess && start.tested && m_types.at(start.tested).isObject() && isDeclaredToHoldNumbersAlone(program(), m_types, start.tested));
        m_sharedGenericVersions.last().isAboutToBeTaken = true;
        m_versionedLoops.append(VersionedLoop { frameIndex, inputHead, genericVersionEntry, { }, whereFrontFails, { }, false });
        m_versionedLoops.last().hasCallInIteration = someOperationOfRowThatRanOtherCodeIsCall(index), m_versionedLoops.last().hasUninlinedCall = m_summaryHasUninlinedCall;
        m_versionedLoops.last().mayRestartUnderHandler = mayVersionLoopUnderHandler(inputHead);
        m_versionedLoops.last().restartsAtCaughtThrows = m_handlersRunOtherCode;
        m_versionedLoops.last().lastBackEdgeDominator = BackEdgeDominators(index, index.loopOf(inputHead)).lastOf(index.loopOf(inputHead));
        ++m_ordinalOfCopyOfLoop;
        Vector<Reference, 8> enteringCopy(entering);
        for (auto [place, number] : start.elementsThatEnterAsNumbers)
            enteringCopy[place] = number;
        m_everyTurnOfTypedBuildLeaves = false;
        SetForScope holderIsNoBuiltin(m_walksCopyWhereHolderIsNoBuiltin, m_walksCopyWhereHolderIsNoBuiltin || ((isRest || isForWhatIsNoBuiltin) && tellsListFromBytes));
        SetForScope isNoBuiltin(m_walksCopyForWhatIsNoBuiltin, isForWhatIsNoBuiltin);
        SetForScope whichIsNoBuiltin(m_holderThatIsNoBuiltin, isForWhatIsNoBuiltin ? HolderThatIsNoBuiltin { members[0].names.span(), members[0].value, frameIndex, members[0].readOfVariableInLoop } : HolderThatIsNoBuiltin { });
        walkCopyOfLoop(inputHead, enteringCopy.span());
        turnsOfSomeCopyComeRound |= !std::exchange(m_everyTurnOfTypedBuildLeaves, false);
        popVersionedLoop();
        m_sharedGenericVersions.last().isAboutToBeTaken = false;
        for (auto [name, reference] : boundBefore | std::views::reverse)
            bind(name, reference);
        for (auto& [other, before] : expectedBefore | std::views::reverse)
            rememberedExpectationOf(other) = before;
        forgetValuesOfLoop(inputHead);
        if (isForWhatIsNoBuiltin && !m_hasFailed) {
            bool readsPlainStorage = false;
            for (uint32_t value = firstValueOfCopyForWhatIsNoBuiltin; value < m_code.numberOfValues() && !readsPlainStorage; ++value)
                readsPlainStorage = m_code.at(ValueID(value)).opcode == Opcode::GetElementInStorageOrAbsent;
            m_block = start.block;
            m_state = WTF::move(atEndOfTestsForCopy);
            jumpTo(readsPlainStorage ? firstBlockOfCopyForWhatIsNoBuiltin : genericStart, { });
            count(readsPlainStorage ? StatisticID::OrdinaryObjectLoopsVersioned : StatisticID::OrdinaryObjectLoopVersionsDiscarded);
            m_block = { };
            m_state.clear();
        }
    }
    putBackIterationsSteppedAt(steppedAtHead);
    m_everyTurnOfTypedBuildLeaves = !turnsOfSomeCopyComeRound;
    m_sharedGenericVersions.removeLast();
    forgetWhatWasBelievedAt(genericVersionEntry);

    for (BlockID member : index.membersOf(loop))
        inputBlock(frameIndex, member).firstNarrowerName = narrowerNamesBeforeLoop;
    m_frames[frameIndex].loopsBeingVersioned.removeLast();
    m_block = { };
    m_state.clear();
    return true;
}

static bool turnsReadListsFoundFromInFront(const Program& program, const BodyIndex& index, BlockID inputHead)
{
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    WhatReadsFromInFrontOfLoopFind foundFromInFront;
    BitVector valuesMadeOutOfWhatStandsInFront;
    auto isMadeOutOfWhatStandsInFront = [&](ValueID written) {
        ValueID value = index.stripCasts(written);
        return value && (value.index() < firstValueOfLoop || valuesMadeOutOfWhatStandsInFront.get(value.index() - firstValueOfLoop));
    };
    auto readsRowFoundUnderKeyMadeOutOfWhatStandsInFront = [&](const BodyValue& read) {
        ValueID row = index.stripCasts(read.objectOperand());
        if (!row || row.index() < firstValueOfLoop || index.isParameter(row) || index.opcodeOf(row) != Opcode::ElemGet)
            return false;
        ValueID holder = index.stripCasts(index.value(row).objectOperand());
        return holder && holder.index() < firstValueOfLoop && isMadeOutOfWhatStandsInFront(index.value(row).propertyOperand());
    };
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() == Opcode::ElemGet && (readsListFoundByReadFromInFront(program, index, operation, firstValueOfLoop, foundFromInFront) || readsRowFoundUnderKeyMadeOutOfWhatStandsInFront(operation)))
                return true;
            bool isMadeOutOfIt = operation.opcode() == Opcode::Integer || allNumericOperators.contains(operation.opcode());
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                isMadeOutOfIt = isMadeOutOfIt && isMadeOutOfWhatStandsInFront(operand);
            });
            if (isMadeOutOfIt)
                valuesMadeOutOfWhatStandsInFront.set(operation.id().index() - firstValueOfLoop);
        }
    }
    return false;
}

static ValueID valueThatIsHandedOnAs(Walker& walker, const BodyIndex& index, ValueID name, unsigned joinsLeft = 3)
{
    for (;;) {
        if (!index.isParameter(name)) {
            if (!inputIdentityOpcodes.contains(index.opcodeOf(name)))
                return name;
            name = index.value(name).operand(0);
            continue;
        }
        if (ValueID narrowed = index.valueNarrowedInto(name)) {
            name = narrowed;
            continue;
        }
        BlockID join = index.blockOf(name);
        if (!joinsLeft || index.isLoopHead(join) || join == entryBlock)
            return name;
        uint32_t place = name.index() - index.block(join).firstParameter().index();
        ValueID arrives;
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, join)) {
            std::span<const ValueID> arguments = edge.target.arguments();
            ValueID handedOn = place < arguments.size() ? valueThatIsHandedOnAs(walker, index, arguments[place], joinsLeft - 1) : ValueID();
            if (!handedOn || (arrives && arrives != handedOn))
                return name;
            arrives = handedOn;
        }
        if (!arrives)
            return name;
        name = arrives;
    }
}

static bool whatComesRoundIsReadOff(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t position)
{
    ValueID parameter = index.block(inputHead).parameter(position);
    LoopID loop = index.loopOf(inputHead);
    bool followsLink = false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        if (position >= arguments.size())
            return false;
        ValueID loopCarriedValue = valueThatIsHandedOnAs(walker, index, arguments[position]);
        if (loopCarriedValue == parameter)
            continue;
        if (index.isParameter(loopCarriedValue) || index.opcodeOf(loopCarriedValue) != Opcode::PropGet || index.value(loopCarriedValue).isFreeName() || valueThatIsHandedOnAs(walker, index, index.value(loopCarriedValue).objectOperand()) != parameter)
            return false;
        followsLink = true;
    }
    return followsLink;
}

static ValueID nameUnderWhichTurnsReadInTwoBlocksOff(Walker& walker, const BodyIndex& index, BlockID inputHead, ValueID parameter)
{
    BlockID first;
    ValueID asWritten;
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::PropGet || operation.isFreeName())
                continue;
            if (valueThatIsHandedOnAs(walker, index, operation.objectOperand()) != parameter)
                continue;
            if (first && first != member)
                return asWritten;
            first = member;
            asWritten = operation.objectOperand();
        }
    }
    return { };
}

static void appendLinksHeldWhereTheyComeRound(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, Vector<HoistedCounterCheck, 2>& counters)
{
    TypeArena& types = walker.types();
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock head = index.block(inputHead);
    for (uint32_t position = 0; position < head.numberOfParameters() && position < entering.size(); ++position) {
        if (!entering[position] || !entering[position].isValue() || counters.containsIf([&](const HoistedCounterCheck& counter) { return counter.position == position; }))
            continue;
        ValueID parameter = head.parameter(position);
        ValueID asWritten = nameUnderWhichTurnsReadInTwoBlocksOff(walker, index, inputHead, parameter);
        if (!asWritten || !whatComesRoundIsReadOff(walker, index, inputHead, position))
            continue;
        TypeID expected = findExpectation(walker, parameter).type;
        if (!expected)
            expected = findExpectation(walker, asWritten).type;
        if (!expected)
            continue;
        Type type = types.at(expected);
        if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures() || !holdsOneMember(type.layouts) || (type.primitives & ~Primitive::UndefinedOrNull))
            continue;
        if (!types.overlap(walker.typeOf(entering[position]), types.withoutNullish(expected)))
            continue;
        TypeID held = types.unionOf(expected, TypeArena::fromPrimitives(Primitive::UndefinedOrNull));
        if (position < preview.typesOfParameters.size() && preview.typesOfParameters[position] && types.isSubset(preview.typesOfParameters[position], held))
            continue;
        counters.append({ position, types.withoutNullish(expected), true, true });
    }
}

static bool isElementUnderCounter(Walker& walker, const BodyIndex& index, ValueID value, const BitVector& isUnderCounter, unsigned joinsLeft = 3)
{
    for (;;) {
        value = index.stripCasts(value);
        if (!value)
            return false;
        if (index.isParameter(value)) {
            ValueID narrowed = index.valueNarrowedInto(value);
            if (!narrowed)
                break;
            value = narrowed;
            continue;
        }
        if (index.opcodeOf(value) != Opcode::ToPropertyKey)
            return index.opcodeOf(value) == Opcode::ElemGet && isUnderCounter.get(value.index());
        value = index.value(value).operand(0);
    }
    BlockID join = index.blockOf(value);
    if (!joinsLeft || index.isLoopHead(join) || join == entryBlock)
        return false;
    uint32_t place = value.index() - index.block(join).firstParameter().index();
    return std::ranges::all_of(walker.incomingEdgesOf(index, join), [&](const BodyIncomingEdge& edge) {
        std::span<const ValueID> arguments = edge.target.arguments();
        return place < arguments.size() && isElementUnderCounter(walker, index, arguments[place], isUnderCounter, joinsLeft - 1);
    });
}

static bool accessesViewUnderIntegerElement(Walker& walker, const BodyIndex& index, ValueID access, const BitVector& isUnderCounter, NOESCAPE const ScopedLambda<TypeID(ValueID)>& typeInFront)
{
    if (index.opcodeOf(access) != Opcode::ElemGet && index.opcodeOf(access) != Opcode::ElemSet)
        return false;
    BodyValue operation = index.value(access);
    TypeID holder = typeInFront(index.stripCasts(operation.objectOperand()));
    return holder && isViewOfNumbersReadInLine(walker, holder) && isElementUnderCounter(walker, index, operation.propertyOperand(), isUnderCounter);
}

static bool everyElementReadIsGuarded(Walker& walker, BlockID inputHead)
{
    const BodyIndex& index = walker.currentBodyIndex();
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue read : index.block(member).values()) {
            if (read.opcode() == Opcode::ElemGet && !findHoistedArrayGuard(walker, read))
                return false;
        }
    }
    return true;
}

bool Walker::tryVersionLoop(BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering)
{
    if (!m_buysTwoBuilds && !m_isVersioningDryRun)
        return false;
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);
    if (!m_genericVersionsInsideSpecialized.isEmpty() && m_genericVersionsInsideSpecialized.last() != std::pair { frameIndex, inputHead } && index.loop(loop).numberOfLoopsWithin > 1)
        return false;
    SharedGenericVersion* shared = !m_sharedGenericVersions.isEmpty() && m_sharedGenericVersions.last().frameIndex == frameIndex && m_sharedGenericVersions.last().inputHead == inputHead && (m_isVersioningDryRun ? m_sharedGenericVersions.last().isAboutToBeTaken : std::exchange(m_sharedGenericVersions.last().isAboutToBeTaken, false)) ? &m_sharedGenericVersions.last() : nullptr;
    if (!shared && m_frames[frameIndex].loopsBeingVersioned.contains(loop))
        return false;
    bool isPreheaderEnteredFromIteration = shared && shared->isEnteredFromIteration;
    BlockID sharedStart = shared && !isPreheaderEnteredFromIteration ? shared->start : BlockID();
    BlockID sharedEntry = shared ? shared->entry : BlockID();
    bool sharedCopyIsPlainWalk = shared && shared->isPlainWalk;
    bool frontLeadsToNextStartOfMember = shared && shared->otherHoldersAreAskedForLists;
    if (sharedCopyIsPlainWalk)
        m_sharedGenericVersions.removeLast();

    auto forgetOperatorsMadeInFront = makeScopeExit([this, kept = m_operatorsMadeInFrontOfLoops.size()] {
        m_operatorsMadeInFrontOfLoops.shrink(kept);
    });
    std::span<const UnswitchCandidate> unswitchCandidates = m_unswitchCandidates;
    SetForScope noNestedCandidates(m_unswitchCandidates, std::span<const UnswitchCandidate>());
    bool unswitches = !unswitchCandidates.empty() && !shared && !m_isVersioningDryRun;
    bool everyCallIsTested = m_summaryHasUninlinedCall && someOperationOfRowThatRanOtherCodeIsCall(currentBodyIndex()) && std::ranges::all_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
        const BodyIndex& body = currentBodyIndex();
        return ran && ran.index() < body.numberOfValues() && !body.isParameter(ran) && (effectRuleOf(body.opcodeOf(ran)) != EffectRule::Call || callsFewHeldClosures(*this, body.value(ran)));
    });
    SetForScope callsAreTested(m_summaryHasUninlinedCall, m_summaryHasUninlinedCall && !everyCallIsTested);
    SetForScope previewOutOfRow(m_previewOutOfRow, &preview);
    SetForScope isBought(m_buysTwoBuilds, false);
    IterationsSteppedAtHead steppedAtHead = takeIterationsSteppedAt(inputHead);
    auto takeIterationsAsTheyEntered = [&] {
        for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
            IndexedIteration& iteration = m_indexedIterations[place];
            iteration.hasProtocolFallback = entered.hasProtocolFallback;
            if (entered.isArrayOrMap)
                iteration.table = entered.table;
            iteration.list = entered.list;
            iteration.state = entered.state;
            iteration.iterator = entered.iterator;
            iteration.next = entered.next;
            iteration.stateHoldsVerdictOnNext = entered.stateHoldsVerdictOnNext;
        }
    };
    auto takeIterationsAsSteppedByIndexAlone = [&] {
        for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
            if (!entered.hasProtocolFallback)
                continue;
            IndexedIteration& iteration = m_indexedIterations[place];
            iteration.hasProtocolFallback = false;
            if (entered.isArrayOrMap)
                iteration.table = { };
            iteration.state = { };
            iteration.iterator = { };
            iteration.next = { };
            iteration.stateHoldsVerdictOnNext = false;
        }
    };
    auto takeAsTheyEnteredAtEnd = makeScopeExit(takeIterationsAsTheyEntered);
    takeIterationsAsSteppedByIndexAlone();
    bool stepsWhatIsNoEnumeration = steppedAtHead.asTheyEntered.containsIf([](const auto& entered) { return entered.second.otherForm == IndexedIteration::OtherForm::None && entered.second.contents != IndexedIteration::Contents::View && entered.second.contents != IndexedIteration::Contents::Text; });
    Vector<HoistedSpeculation, 4> guessesAtListsAlone;
    ListsReadInLoop listsReadWithoutGuesses;
    if (stepsWhatIsNoEnumeration) {
        SetForScope asked(m_restartablePreviewHeader, areIterationsRestartable(inputHead, preview) ? inputHead : m_restartablePreviewHeader);
        listsReadWithoutGuesses = findListsReadInLoop(*this, inputHead, preview, entering, guessesAtListsAlone);
        if (!listsReadWithoutGuesses.areLeftAsTheyAre)
            return false;
    }
    Vector<HoistedSpeculation, 4> guesses = findHoistedSpeculations(*this, inputHead, preview);
    BlockID headOfLooksTriedForLists = areIterationsRestartable(inputHead, preview) ? inputHead : m_restartablePreviewHeader;
    ListsReadInLoop listsRead;
    if (stepsWhatIsNoEnumeration && guesses.isEmpty()) {
        listsRead = WTF::move(listsReadWithoutGuesses);
        guesses = WTF::move(guessesAtListsAlone);
    } else {
        SetForScope asked(m_restartablePreviewHeader, headOfLooksTriedForLists);
        listsRead = findListsReadInLoop(*this, inputHead, preview, entering, guesses);
    }
    Vector<HoistedCounterCheck, 2> counters = findHoistedCounterChecks(*this, inputHead, preview, entering, guesses);
    appendLinksHeldWhereTheyComeRound(*this, inputHead, preview, entering, counters);
    appendCarriedNumbers(*this, inputHead, preview, entering, listsRead.lists.span(), guesses.span(), counters);
    Vector<HoistedVariableCheck, 2> hoistedVariableChecks;
    Vector<VariableID, 2> hoistedImportedFunctionGuards;
    guessDivisorsOfIntegersInFrontOfLoop(*this, inputHead, preview, entering, guesses, hoistedVariableChecks);
    if (!unswitches && !sharedCopyIsPlainWalk && !m_walksCopyThatAsksCountersAlone && steppedAtHead.asTheyEntered.isEmpty() && !loopCarriesValuesOfConsumerRound(inputHead))
        speculateOnKeyTerms(*this, inputHead, preview, entering, guesses, hoistedVariableChecks);
    guessFactorsInVariables(*this, inputHead, preview, hoistedVariableChecks);
    guessViewsInVariables(*this, inputHead, hoistedVariableChecks);
    if (!isUnderHandler())
        findNumbersOnlyReckonedWith(*this, inputHead, guesses, hoistedVariableChecks);
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() == Opcode::TypeTest && operation.testedType() == TypeTable::absent) {
                ValueID asked = operation.operand(0);
                if (index.isParameter(asked) || index.opcodeOf(asked) != Opcode::GetModuleVariable)
                    continue;
                VariableID place = index.value(asked).variable();
                TypeID seen = facts().held().variable(place).proven;
                TypeID objects = seen && !holdsOnlyPrimitives(seen) ? m_types.withOnlyObjects(seen) : TypeID();
                if (objects && singleLayoutOf(m_types, m_types.at(objects)) && !hoistedVariableChecks.containsIf([&](const HoistedVariableCheck& other) { return other.variable == place; }))
                    hoistedVariableChecks.append({ place, objects, StatisticID::AbsenceTestedVariableReadsHoisted });
                continue;
            }
            if (operation.opcode() == Opcode::Call) {
                ValueID called = index.stripCasts(operation.calleeOperand());
                if (called && !index.isParameter(called) && index.opcodeOf(called) == Opcode::CheckInitialized)
                    called = index.stripCasts(index.value(called).operand(0));
                if (!called || index.isParameter(called) || index.opcodeOf(called) != Opcode::GetModuleVariable)
                    continue;
                VariableID place = index.value(called).variable();
                if (index.loopOf(member) != loop || hoistedVariableChecks.containsIf([&](const HoistedVariableCheck& other) { return other.variable == place; }))
                    continue;
                TypeID closures = fewClosuresWhoseCopiesPayInLoop(*this, facts().held().variable(place).proven, operation.argumentsOperands().size());
                TypeID proven = m_positionsOfBuild->variable(TypeLevel::Proven, place);
                if (closures && !(proven && m_types.isSubset(proven, closures)) && BackEdgeDominators(index, loop).contains(member)) {
                    hoistedVariableChecks.append({ place, closures, StatisticID::CalleeVariableReadsHoisted });
                    if (program().at(place).module != program().at(currentFunction()).module)
                        hoistedImportedFunctionGuards.append(place);
                }
                continue;
            }
            if (operation.opcode() != Opcode::GetModuleVariable)
                continue;
            VariableID variable = operation.variable();
            if (program().at(variable).module == program().at(currentFunction()).module || !facts().linked().variables[variable.index()].isFilled)
                continue;
            if (m_state.isPastDeadZone(variable) || plan().deferredDefinitions().findDeferred(variable))
                continue;
            TypeID held = m_positionsOfBuild->variable(TypeLevel::Proven, variable);
            if (!held || !m_types.at(held).isObject() || !singleFunctionOf(m_types, m_types.at(held)))
                continue;
            if (!hoistedVariableChecks.containsIf([&](const HoistedVariableCheck& asked) { return asked.variable == variable; }))
                hoistedVariableChecks.append({ variable, held, StatisticID::ImportedFunctionReadsHoisted }), hoistedImportedFunctionGuards.append(variable);
        }
    }
    for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
        bool isStepOfText = entered.contents == IndexedIteration::Contents::Text;
        bool isAskedInFront = entered.hasProtocolFallback && (isStepOfText || entered.contents == IndexedIteration::Contents::View) && entered.isDeadAfterLoop;
        if (isAskedInFront && !guesses.containsIf([&](const HoistedSpeculation& asked) { return asked.value == entered.list; }))
            guesses.append(HoistedSpeculation { entered.list, isStepOfText ? TypeID(TypeTable::string) : m_types.builtinClass(entered.builtinClass), { }, { } });
    }
    for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
        if (entered.hasProtocolFallback && !guesses.containsIf([&](const HoistedSpeculation& asked) { return asked.value == entered.list; }))
            return false;
    }
    Vector<std::pair<Reference, TypeID>, 4> testedInFront;
    for (const HoistedSpeculation& guess : guesses)
        testedInFront.append({ guess.value, guess.tested });
    bool asksAtHead = !!findReadAskedAtHeadOfLoop(inputHead, testedInFront.span());
    std::optional<HoistedArrayGuard> arrayGuardedAtHeader;
    if (m_listsAreStruckInRow && !asksAtHead && (!shared || sharedCopyIsPlainWalk) && steppedAtHead.asTheyEntered.isEmpty() && has(Mechanism::TypedOperations) && !hasStringsInBuilders()) {
        Vector<HoistedSpeculation, 4> guessesBesideLists = guesses;
        SetForScope strikeIsLifted(m_listsAreStruckInRow, false);
        ListsReadInLoop struck = findListsReadInLoop(*this, inputHead, preview, entering, guessesBesideLists);
        arrayGuardedAtHeader = findListThatHeadReadsUnderKeyFromInFront(inputHead, struck.lists.span());
    }
    asksAtHead |= !!arrayGuardedAtHeader;
    asksAtHead |= guesses.isEmpty() && listsRead.lists.isEmpty() && counters.isEmpty() && steppedAtHead.asTheyEntered.isEmpty() && !m_coldCopyDepth && !hasStringsInBuilders() && benefitsFromHeaderGuard(*this, inputHead, entering);
    Vector<VariableID, 4> variablesCheckedInTurns;
    struct CheckedInstanceField {
        Reference instance;
        FieldReference field;
    };
    Vector<CheckedInstanceField, 4> checkedInstanceFields;
    uint32_t firstLoopValue = index.block(inputHead).firstParameter().index();
    for (BlockID member : index.membersOf(loop)) {
        if (!has(Mechanism::Folding) || m_coldCopyDepth)
            break;
        for (BodyValue operation : index.block(member).values()) {
            ValueID read = operation.opcode() == Opcode::CheckInitialized ? index.stripCasts(operation.operand(0)) : ValueID();
            if (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::GetEnvironmentField) {
                BodyValue fieldRead = index.value(read);
                FieldReference field { fieldRead.layout(), fieldRead.field() };
                ValueID holder = index.stripCasts(fieldRead.environmentOperand());
                Reference instance = leavesDeadZoneForGood(program(), field) ? loopInvariantScope(*this, index, holder, firstLoopValue) : Reference();
                if (!instance || !instance.isRecord() || m_state.isPastDeadZone(instance, field))
                    continue;
                bool isListed = checkedInstanceFields.containsIf([&](const CheckedInstanceField& listed) { return listed.instance == instance && listed.field == field; });
                TypeID widest = widestTypeOfOperation(*this, fieldRead);
                if (!isListed && widest && (m_types.at(widest).primitives & Primitive::Absent))
                    checkedInstanceFields.append({ instance, field });
                continue;
            }
            if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::GetModuleVariable)
                continue;
            VariableID variable = index.value(read).variable();
            if (variablesCheckedInTurns.contains(variable) || !leavesDeadZoneForGood(program(), variable) || m_state.isPastDeadZone(variable))
                continue;
            const DeferredVariable* deferred = plan().deferredDefinitions().findDeferred(variable);
            if (deferred && deferred->mark == Primitive::Absent)
                continue;
            TypeID widest = widestTypeOfOperation(*this, index.value(read));
            if (widest && (m_types.at(widest).primitives & Primitive::Absent))
                variablesCheckedInTurns.append(variable);
        }
    }
    bool hasNoCheckedBinding = variablesCheckedInTurns.isEmpty() && checkedInstanceFields.isEmpty();
    bool genericVersionHasEntry = sharedEntry || asksAtHead || (!hasStringsInBuilders() && (steppedAtHead.asTheyEntered.isEmpty() ? !m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) : iterationsSteppedAtHandOverPositionsAlone(inputHead)));
    bool stepsByProtocol = steppedAtHead.asTheyEntered.containsIf([](const auto& entered) { return entered.second.otherForm != IndexedIteration::OtherForm::None || entered.second.contents != IndexedIteration::Contents::List; });
    bool asksForInt32s = unswitches && std::ranges::all_of(unswitchCandidates, &UnswitchCandidate::asksForInt32s);
    bool takesInt32s = false;
    for (HoistedArrayGuard& list : listsRead.lists) {
        if (unswitches)
            list.hasAlternateStorage = false;
        if (asksForInt32s && list.storage == ArrayStorage::Double && list.admitsInt32Storage && !list.isWritable) {
            list.storage = ArrayStorage::Int32;
            list.admitsInt32Storage = false;
            takesInt32s = true;
        }
        if (!list.hasAlternateStorage || (!m_walksCopyForAnotherKindOfStorage && (!shared || sharedCopyIsPlainWalk) && genericVersionHasEntry && !stepsByProtocol && !m_coldCopyDepth && !m_walksCopyWithoutSomeIterationGuesses))
            continue;
        if (list.keepsStorageWithoutAlternate) {
            list.hasAlternateStorage = false;
            continue;
        }
        list.storage = list.alternateStorage;
        list.admitsInt32Storage = list.storage != ArrayStorage::Other ? !((list.keepsDoublesWithoutAlternate || list.isStepped) && list.isWritable) : list.alternateTakesAnyKind;
        list.hasAlternateStorage = false;
    }
    if (unswitches && !genericVersionHasEntry)
        return false;
    auto isTermOfKey = [](const HoistedSpeculation& guess) {
        return guess.isTermOfKey;
    };
    if (!genericVersionHasEntry)
        guesses.removeAllMatching(isTermOfKey);
    bool otherCopyAsksCountersAlone = (listsRead.someListIsTakenUnderCounterAskedInFront || guesses.containsIf(isTermOfKey)) && !m_walksCopyThatAsksCountersAlone && !m_isVersioningDryRun && !unswitches;
    if (otherCopyAsksCountersAlone && (sharedCopyIsPlainWalk || !genericVersionHasEntry || !steppedAtHead.asTheyEntered.isEmpty() || loopCarriesValuesOfConsumerRound(inputHead)))
        return false;
    bool isGenericVersionSharedByFallback = otherCopyAsksCountersAlone && sharedEntry;
    bool otherCopyBoughtNothing = false;
    bool frontLeadsOnAsItDid = isGenericVersionSharedByFallback && frontLeadsToNextStartOfMember;
    if (isGenericVersionSharedByFallback)
        sharedEntry = { };
    if (isGenericVersionSharedByFallback && !frontLeadsOnAsItDid)
        sharedStart = { };
    auto isCheckedOnBackEdge = [](const HoistedCounterCheck& counter) {
        return counter.isCheckedOnBackEdge;
    };
    bool canBackEdgesEnterGeneric = genericVersionHasEntry && steppedAtHead.asTheyEntered.isEmpty();
    if (canBackEdgesEnterGeneric && counters.containsIf(isCheckedOnBackEdge) && canKeepStringsInBuilders(*this) && !preview.effects.maySuspend) {
        Vector<GrowingText, 2> texts;
        findGrowingTexts(*this, inputHead, preview, texts);
        canBackEdgesEnterGeneric = texts.isEmpty();
    }
    bool sumsAreAskedInFrontAlone = false;
    if (!canBackEdgesEnterGeneric) {
        bool sumsMayStay = genericVersionHasEntry && !steppedAtHead.asTheyEntered.isEmpty();
        counters.removeAllMatching([&](const HoistedCounterCheck& counter) {
            return counter.isCheckedOnBackEdge && !(sumsMayStay && counter.isSum && entering[counter.position] && !m_types.isSubset(typeOf(entering[counter.position]), counter.tested));
        });
        for (HoistedCounterCheck& counter : counters) {
            sumsAreAskedInFrontAlone |= counter.isCheckedOnBackEdge;
            counter.isCheckedOnBackEdge = false;
        }
    }
    auto isNoSum = [](const HoistedCounterCheck& counter) { return !counter.isSum; };
    bool hasTestInFront = !guesses.isEmpty() || !listsRead.lists.isEmpty() || counters.containsIf(isNoSum) || asksAtHead || unswitches;
    bool storesIntoListsTestedInFront = listsRead.lists.containsIf([](const HoistedArrayGuard& list) {
        return list.isWritable;
    });
    ArrayLookupByName readArrayLookup;
    bool guessesAtNothingButListsTestedInFront = !guesses.containsIf([&](const HoistedSpeculation& guess) {
        return !hasKind(m_types, guess.tested, OperandKind::Array) || readArrayLookup.find(listsRead.lists.span(), guess.value) == notFound;
    });
    bool guessesOnlyAtListsTestedInFront = storesIntoListsTestedInFront && guessesAtNothingButListsTestedInFront;
    auto testMayWellFail = [](const HoistedCounterCheck& counter) { return counter.testMayWellFail; };
    bool genericVersionSpeculates = std::ranges::all_of(counters, testMayWellFail) && !asksAtHead;
    Vector<MemberConditionRecord, 4> guardsAskedOfObjectsOfTurns;
    auto guardMethodOfIterationObject = [&](ValueID named, PropertyKeyID key) {
        ValueID object = index.stripCasts(named);
        if (index.isParameter(object) || index.opcodeOf(object) != Opcode::Construct || !index.isInLoop(index.blockOf(object), loop))
            return;
        FunctionID constructor = facts().linked().calleeAt(currentFunction(), object);
        if (TypeID made = constructor ? typeOfObjectsMadeBy(*this, constructor) : TypeID())
            findGuardsOfMethodAboveObjectsOf(*this, made, key, guardsAskedOfObjectsOfTurns);
    };
    BlockID lastHandler;
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        for (BodyValue operation : block.values()) {
            if (operation.opcode() == Opcode::PropGet && !operation.isFreeName())
                guardMethodOfIterationObject(operation.objectOperand(), operation.key());
        }
        if (!block.hasHandler() || std::exchange(lastHandler, block.handler().block()) == block.handler().block())
            continue;
        if (BodyBlock cleanup = index.block(lastHandler); cleanup.role() == BlockRole::DisposalCleanup)
            guardMethodOfIterationObject(cleanup.subject(), m_types.wellKnownSymbolKey(WellKnownSymbol::Dispose));
    }
    hasTestInFront |= !guardsAskedOfObjectsOfTurns.isEmpty();
    bool checksMethodsInPreheader = !guardsAskedOfObjectsOfTurns.isEmpty();
    bool guardsNotedAreOnlyTestInFront = !hasTestInFront && !m_guardsOfStoresAskedOfRow.isEmpty();
    for (const MemberConditionRecord& condition : m_guardsOfStoresAskedOfRow) {
        if (!guardsAskedOfObjectsOfTurns.contains(condition))
            guardsAskedOfObjectsOfTurns.append(condition);
    }
    hasTestInFront |= !guardsAskedOfObjectsOfTurns.isEmpty();
    auto typeInRow = [&](ValueID input) -> TypeID {
        TypeID known;
        if (input.index() >= preview.firstValue && input.index() - preview.firstValue < preview.typesOfValues.size())
            known = preview.typesOfValues[input.index() - preview.firstValue];
        else if (Reference built = referenceOf(input))
            known = typeOf(built);
        return known ? known : TypeID(TypeTable::anything);
    };
    auto asksItsSiteAlone = [&](const BodyValue& operation) {
        if (canLookUpCallee(*this, operation))
            return true;
        if ((operation.opcode() == Opcode::PropGet || operation.opcode() == Opcode::PropSet) && mayUseGlobalBindingInIterations(*this, operation, typeInRow(operation.objectOperand())))
            return true;
        if (operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet) {
            TypeID holder = typeInRow(operation.objectOperand());
            TypeID key = typeInRow(operation.propertyOperand());
            if (mayUseKeyedAddress(*this, operation, holder, key) || mayDispatchOverNames(*this, operation, holder, key))
                return true;
        }
        if ((operation.opcode() == Opcode::PropGet || operation.opcode() == Opcode::PropSet) && mayUseNamedAddress(*this, operation, typeInRow(operation.objectOperand())))
            return true;
        if (!siteMayAnswerReadWithoutOtherCode(*this, operation))
            return false;
        TypeID holder = typeInRow(operation.objectOperand());
        return m_types.isUninformative(holder) || (m_types.at(holder).isObject() && (m_types.at(holder).hasOnlyClosures() || m_types.at(holder).anyObject));
    };
    auto everyOperationThatRanOtherCodeMayBeTyped = [&](bool countsOnListsFoundFromInFront) {
        if (m_operationsThatRanOtherCodeOfRow.isEmpty())
            return false;
        WhatReadsFromInFrontOfLoopFind foundFromInFront;
        bool sitesMayAnswer = !m_summaryHasUninlinedCall && someReadThatAsksItsSiteIsReckonedWith(*this, m_operationsThatRanOtherCodeOfRow.span(), TypesOfInputs(typeInRow));
        bool someListIsRead = !listsRead.lists.isEmpty() || (countsOnListsFoundFromInFront && std::ranges::any_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
            return ran && readsListFoundByReadFromInFront(program(), index, index.value(ran), index.block(inputHead).firstParameter().index(), foundFromInFront);
        }));
        SetForScope purchaseAsks(m_purchaseAsksWhetherTurnsCanBeQuiet, std::ranges::none_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
            return ran && effectRuleOf(index.value(ran).opcode()) == EffectRule::Call && !hasFewExpectedCallees(*this, index.value(ran));
        }));
        for (ValueID ran : m_operationsThatRanOtherCodeOfRow) {
            if (!ran || !index.isInLoop(index.blockOf(ran), loop))
                return false;
            BodyValue operation = index.value(ran);
            if (false)
                return false;
            bool storesIntoListTested = operation.opcode() == Opcode::ElemSet && (storesIntoListTestedInFrontOfLoop(*this, index, operation, index.block(inputHead).firstParameter().index(), listsRead.lists.span()) || storesListedNumberIntoView(*this, index, operation, typeInRow(operation.objectOperand()), index.block(inputHead).firstParameter().index(), listsRead.lists.span(), guesses.span(), { }));
            storesIntoListTested |= operation.opcode() == Opcode::ElemSet && areIterationsRestartable(inputHead, preview) && storesNumberIntoRow(*this, index, operation, typeInRow(operation.valueOperand()), index.block(inputHead).firstParameter().index(), listsRead.lists.span());
            bool readsListTested = !listsRead.lists.isEmpty() && (operation.opcode() == Opcode::ElemGet || storesIntoListTested || (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() == program().keyOf(BuiltinNames::length)) || findIndexedArrayIteration(*this, index, operation));
            ValueID step = !listsRead.lists.isEmpty() && operation.opcode() == Opcode::ResultAt ? operation.operand(0) : ValueID();
            readsListTested |= step && !index.isParameter(step) && findIndexedArrayIteration(*this, index, index.value(step));
            readsListTested |= stepsViewByIndexAlone(*this, index, operation) || convertsCheckedCounter(*this, operation, inputHead, entering) || convertsInvariantKey(*this, operation);
            readsListTested |= stepsTextByIndexAlone(*this, index, operation);
            readsListTested |= countsOnListsFoundFromInFront && readsListFoundByReadFromInFront(program(), index, operation, index.block(inputHead).firstParameter().index(), foundFromInFront);
            ValueID row = countsOnListsFoundFromInFront && operation.opcode() == Opcode::ElemGet ? index.stripCasts(operation.objectOperand()) : ValueID();
            readsListTested |= row && !index.isParameter(row) && index.opcodeOf(row) == Opcode::ElemGet && readsListFoundByReadFromInFront(program(), index, index.value(row), index.block(inputHead).firstParameter().index(), foundFromInFront);
            bool isReadInSomeIterations = operation.opcode() == Opcode::PropGet && !operation.isFreeName() && index.loopOf(index.blockOf(ran)) == loop && !index.hasIrreducibleFlow() && !BackEdgeDominators(index, loop).contains(index.blockOf(ran));
            if (!readsListTested && !isReadInSomeIterations && effectRuleOf(operation.opcode()) != EffectRule::Operator && !guessesMayTypeOperationInCleanTurn(*this, operation, TypesOfInputs(typeInRow), sitesMayAnswer))
                return false;
        }
        return true;
    };
    std::unique_ptr<LoopPreview> specializedPreview;
    bool turnsAreQuietByRow = (unswitches && m_operationsThatRanOtherCodeOfRow.isEmpty()) || m_rowSaysTurnsAreQuietOutsideArms || everyOperationThatRanOtherCodeMayBeTyped(false) || m_firstOtherCodeRanInCopyOfRow;
    bool turnsAreQuietBehindListsFoundFromInFrontAlone = !turnsAreQuietByRow && everyOperationThatRanOtherCodeMayBeTyped(true);
    genericVersionSpeculates &= !checksMethodsInPreheader || turnsAreQuietBehindListsFoundFromInFrontAlone;
    if (areIterationsRestartable(inputHead, preview) && (turnsAreQuietByRow || turnsAreQuietBehindListsFoundFromInFrontAlone)) {
        specializedPreview = makeUniqueWithoutFastMallocCheck<LoopPreview>();
        specializedPreview->effects = m_effectsOutsideArmsOfRow;
        specializedPreview->typesOfParameters = preview.typesOfParameters;
        specializedPreview->typesOfValues = preview.typesOfValues;
        specializedPreview->firstValue = preview.firstValue;
        findMissingTestsOfRow();
        specializedPreview->guessesCountedOn = m_testsSeenOfRow;
        for (ValueID ran : m_operationsThatRanOtherCodeOfRow) {
            if (!ran || index.opcodeOf(ran) != Opcode::PropGet)
                continue;
            BodyValue read = index.value(ran);
            ValueID receiver = read.objectOperand();
            if (read.isFreeName() || !singleBuiltinClassOf(m_types, m_types.at(typeInRow(receiver))) || specializedPreview->guessesCountedOn.containsIf([&](const GuessCountedOnByLooks& seen) { return seen.operand == receiver; }))
                continue;
            specializedPreview->guessesCountedOn.append(GuessCountedOnByLooks { ran, receiver, typeInRow(receiver) });
        }
        specializedPreview->hasLetMentionsLeave = true;
        specializedPreview->slots = preview.slots;
        Vector<std::pair<ValueID, TypeID>, 2> membersAbovePrimitives;
        bool asksMembersByTheirBytes = false;
        for (const GuessCountedOnByLooks& seen : m_testsSeenOfRow) {
            BodyValue read = index.value(seen.operation);
            if (read.opcode() != Opcode::PropGet || read.objectOperand() != seen.operand)
                continue;
            if (typeOfMemberAskedByItsByte(*this, read, typeInRow(seen.operand))) {
                asksMembersByTheirBytes = true;
                continue;
            }
            if (read.isFreeName())
                continue;
            if (TypeID member = typeFoundAbovePrimitiveBehindWatch(*this, read, typeInRow(seen.operand)))
                membersAbovePrimitives.append({ seen.operation, member });
        }
        Vector<TypeID> membersAskedByTheirBytes;
        auto memberAskedByItsByte = [&](ValueID input) -> TypeID {
            ValueID read = index.stripCasts(input);
            return read.index() >= preview.firstValue && read.index() - preview.firstValue < membersAskedByTheirBytes.size() ? membersAskedByTheirBytes[read.index() - preview.firstValue] : TypeID();
        };
        if (asksMembersByTheirBytes) {
            membersAskedByTheirBytes.grow(preview.typesOfValues.size());
            for (BlockID member : index.membersOf(loop)) {
                for (BodyValue read : index.block(member).values()) {
                    if (read.opcode() != Opcode::PropGet || read.id().index() < preview.firstValue || read.id().index() - preview.firstValue >= membersAskedByTheirBytes.size())
                        continue;
                    TypeID holder = memberAskedByItsByte(read.objectOperand());
                    membersAskedByTheirBytes[read.id().index() - preview.firstValue] = typeOfMemberAskedByItsByte(*this, read, holder ? holder : typeInRow(read.objectOperand()));
                }
            }
        }
        if (!membersAbovePrimitives.isEmpty() || asksMembersByTheirBytes) {
            specializedPreview->countsOnMembersAbovePrimitives = true;
            auto typeBehindWatch = [&](ValueID input) -> TypeID {
                for (auto [read, member] : membersAbovePrimitives) {
                    if (read == input)
                        return member;
                }
                if (TypeID member = memberAskedByItsByte(input))
                    return member;
                return typeInRow(input);
            };
            specializedPreview->guessesCountedOn.removeAllMatching([&](const GuessCountedOnByLooks& seen) {
                if (membersAbovePrimitives.containsIf([&](const auto& found) { return found.first == seen.operation; }) || memberAskedByItsByte(seen.operation))
                    return true;
                ValueID made = index.stripCasts(seen.operand);
                return !index.isParameter(made) && index.opcodeOf(made) == Opcode::Call && wouldCallBuiltin(*this, index.value(made), TypesOfInputs(typeBehindWatch)) != BuiltinCallAskedAbout::None;
            });
        }
        if (canKeepStringsInBuilders(*this)) {
            Vector<GrowingText, 2> texts;
            findGrowingTexts(*this, inputHead, *specializedPreview, texts);
            if (!texts.isEmpty() && someIterationLeavesStringsUnchanged(inputHead, texts.span()))
                keepTextsOfLoopOutOfBuilders(inputHead);
            else if (!texts.isEmpty())
                specializedPreview = nullptr;
        }
    }
    bool hasSpecializedPreview = !!specializedPreview;
    bool turnsThatRunOtherCodeComeRound = hasSpecializedPreview && std::ranges::any_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
        return wayBehindOperationComesRound(inputHead, ran);
    });
    Effects specializedEffectsFromSummary = m_effectsOutsideArmsOfRow;
    specializedEffectsFromSummary.writes.remove(AbstractHeapSet { AbstractHeap::MemberGuards });
    Vector<FieldOfScope, 4> fieldsTestedInFront;
    if (genericVersionHasEntry && !sharedEntry && steppedAtHead.asTheyEntered.isEmpty())
        findFieldsOfScopesFromOutsideThatQuietTurnsStore(inputHead, specializedPreview ? specializedPreview->effects : preview.effects, preview.slots.span(), fieldsTestedInFront, false);
    auto staysInt32ByRow = [&](const FieldOfScope& field) {
        TypeID read = resultTypeOf(draftVariableRead(field));
        if (field.slotKey)
            return field.slotIsInt32Position && !m_types.isSubset(read, TypeTable::int32) && m_types.isSubset(TypeTable::int32, read);
        return field.isSteppedOrAddedTo && (!field.isReadBySumsAlone || field.isSteppedByOne) && !m_types.isSubset(read, TypeTable::int32) && m_types.isSubset(TypeTable::int32, read) && std::ranges::all_of(field.addedInTurns, [&](ValueID added) {
            return m_types.isSubset(typeInRow(added), TypeTable::int32);
        });
    };
    fieldsTestedInFront.removeAllMatching([&](const FieldOfScope& field) {
        if (field.slotKey)
            return !staysInt32ByRow(field);
        TypeID read = resultTypeOf(draftVariableRead(field));
        if (!read || !m_types.overlap(read, TypeTable::number))
            return true;
        if (std::ranges::any_of(field.storedInTurns, [&](ValueID stored) {
            TypeID byRow = typeInRow(stored);
            return byRow && !m_types.overlap(byRow, TypeTable::number);
        }))
            return true;
        if (!m_types.at(read).isNumber())
            return !field.isComputedFromItself;
        return !staysInt32ByRow(field) && !mayBeInDeadZone(field);
    });
    for (FieldOfScope& field : fieldsTestedInFront)
        field.type = field.slotIsSteppedByOne ? m_types.integer(Range { field.slotOnlyGrows ? 0 : -maxExactInteger, maxExactInteger }) : staysInt32ByRow(field) ? TypeID(TypeTable::int32) : m_types.intersectionOf(resultTypeOf(draftVariableRead(field)), TypeTable::number);
    for (const HoistedVariableCheck& asked : hoistedVariableChecks) {
        if (!genericVersionHasEntry || sharedEntry || !steppedAtHead.asTheyEntered.isEmpty())
            break;
        FieldOfScope field;
        field.variable = asked.variable;
        field.scope = asked.scope;
        field.field = asked.field;
        field.type = asked.tested;
        fieldsTestedInFront.append(WTF::move(field));
    }
    hasTestInFront |= !fieldsTestedInFront.isEmpty();
    guardsNotedAreOnlyTestInFront &= fieldsTestedInFront.isEmpty();
    genericVersionSpeculates &= fieldsTestedInFront.isEmpty() || (counters.containsIf(testMayWellFail) && std::ranges::all_of(fieldsTestedInFront, [&](const FieldOfScope& field) {
        return field.variable && hoistedImportedFunctionGuards.contains(field.variable);
    }));
    auto numberInFront = [&](auto& self, ValueID written, unsigned operatorsLeft) -> std::optional<double> {
        auto onlyNumberOf = [&](TypeID id) -> std::optional<double> {
            Type type = id ? m_types.at(id) : Type();
            return id && type.isInteger() && !type.integers.isEmpty() && type.integers.minimum == type.integers.maximum ? std::optional(static_cast<double>(type.integers.minimum)) : std::nullopt;
        };
        ValueID value = index.stripCasts(written);
        if (!value || value.index() < index.block(inputHead).firstParameter().index() || index.isParameter(value))
            return std::nullopt;
        BodyValue operation = index.value(value);
        switch (operation.opcode()) {
        case Opcode::Integer:
            return operation.as<Opcode::Integer>().integer();
        case Opcode::GetModuleVariable:
            for (const FieldOfScope& field : fieldsTestedInFront) {
                if (field.variable == operation.variable())
                    return onlyNumberOf(field.type);
            }
            return std::nullopt;
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
        case Opcode::Div: {
            std::optional<double> left = operatorsLeft ? self(self, operation.operand(0), operatorsLeft - 1) : std::nullopt;
            std::optional<double> right = left ? self(self, operation.operand(1), operatorsLeft - 1) : std::nullopt;
            if (!right)
                return std::nullopt;
            switch (operation.opcode()) {
            case Opcode::Add:
                return *left + *right;
            case Opcode::Sub:
                return *left - *right;
            case Opcode::Mul:
                return *left * *right;
            default:
                return *left / *right;
            }
        }
        default:
            return std::nullopt;
        }
    };
    auto integerInFront = [&](ValueID written) -> std::optional<int64_t> {
        static constexpr unsigned mostOperatorsOfBound = 3;
        std::optional<double> number = numberInFront(numberInFront, written, mostOperatorsOfBound);
        if (!number || !(*number > std::numeric_limits<int32_t>::min() && *number < std::numeric_limits<int32_t>::max()) || *number != static_cast<int64_t>(*number))
            return std::nullopt;
        return static_cast<int64_t>(*number);
    };
    CounterHeldBelowRoomOfViews heldBelowRoom;
    AccessesNearCounter accessesNearCounter;
    bool storesUnderCounterAloneRanOtherCode = false;
    if (const Effects& ofTurns = specializedPreview ? specializedPreview->effects : preview.effects; genericVersionHasEntry && steppedAtHead.asTheyEntered.isEmpty() && !asksAtHead && !m_coldCopyDepth
        && has(Mechanism::TypedOperations) && has(Mechanism::UnboxedNumbers) && !isUnderHandler() && !ofTurns.maySuspend) {
        bool viewsMayBeHeld = ofTurns.mayRunOtherCode() ? !specializedPreview : !ofTurns.writes.overlaps(LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }));
        auto isListOnlyReadInLoop = [&](ValueID written) {
            Reference built = referenceOf(written);
            if (!built || !built.isValue())
                return false;
            auto isOnlyRead = [](const HoistedArrayGuard& list) {
                return hasArrayStorageForm(list.storage) && !list.isWritable && !list.turnsStoreIntoLists;
            };
            if (std::optional<HoistedArrayGuard> testedFurtherOut = findHoistedArrayGuard(*this, built))
                return isOnlyRead(*testedFurtherOut);
            return listsRead.lists.containsIf([&](const HoistedArrayGuard& list) {
                return list.name == built && isOnlyRead(list);
            });
        };
        auto typeThatEnters = [&](uint32_t position) -> TypeID {
            for (const HoistedCounterCheck& counter : counters) {
                if (counter.position == position && !counter.testMayWellFail)
                    return counter.tested;
            }
            return position < entering.size() && entering[position] && entering[position].isRecord() ? typeOf(entering[position]) : TypeID();
        };
        auto typeOfNameInFront = [&](Reference built) -> TypeID {
            if (!built || !built.isRecord())
                return { };
            for (const HoistedSpeculation& guess : guesses) {
                if (guess.value == built && guess.tested)
                    return guess.tested;
            }
            for (const HoistedCounterCheck& counter : counters) {
                if (!counter.testMayWellFail && entering[counter.position] == built)
                    return counter.tested;
            }
            for (const FieldOfScope& field : fieldsTestedInFront) {
                if (field.variable && m_state.variable(field.variable) == built)
                    return field.type;
            }
            return typeOf(built);
        };
        auto typeInFront = [&](ValueID written) {
            return typeOfNameInFront(referenceOf(written));
        };
        if (viewsMayBeHeld || !listsRead.lists.isEmpty() || m_headOfTypedBuildOverListsTestedInFront)
            heldBelowRoom = findCounterHeldBelowRoomOfViews(*this, inputHead, scopedLambda<TypeID(uint32_t)>(typeThatEnters), scopedLambda<TypeID(ValueID)>(typeInFront), scopedLambda<TypeID(Reference)>(typeOfNameInFront), scopedLambda<bool(ValueID)>(isListOnlyReadInLoop), scopedLambda<std::optional<int64_t>(ValueID)>(integerInFront), ofTurns);
        if (viewsMayBeHeld && !ofTurns.mayRunOtherCode() && !m_refusesSecondCounters)
            accessesNearCounter = findAccessesNearCounter(*this, inputHead, scopedLambda<TypeID(uint32_t)>(typeThatEnters), scopedLambda<TypeID(ValueID)>(typeInFront));
        bool holdsViews = heldBelowRoom.viewWhoseLengthIsBound || heldBelowRoom.accesses.containsIf([](const AccessUnderCounterOfLoop& access) { return !access.isOfList; });
        if (holdsViews) {
            heldBelowRoom.accesses.removeAllMatching([](const AccessUnderCounterOfLoop& access) { return access.isOfList; });
            if (!viewsMayBeHeld)
                heldBelowRoom.accesses.shrink(0);
        }
        if (holdsViews && ofTurns.mayRunOtherCode() && !heldBelowRoom.accesses.isEmpty()) {
            BitVector isUnderCounter;
            for (const AccessUnderCounterOfLoop& access : heldBelowRoom.accesses)
                isUnderCounter.set(access.access.index());
            storesUnderCounterAloneRanOtherCode = std::ranges::all_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
                if (ran && accessesViewUnderIntegerElement(*this, index, ran, isUnderCounter, scopedLambda<TypeID(ValueID)>(typeInFront)))
                    return true;
                if (!ran || !isUnderCounter.get(ran.index()) || index.opcodeOf(ran) != Opcode::ElemSet)
                    return false;
                ValueID stored = index.stripCasts(index.value(ran).valueOperand());
                return isUnderCounter.get(stored.index()) && !index.isParameter(stored) && index.opcodeOf(stored) == Opcode::ElemGet;
            });
            Reference preheaderBound = m_operationsThatRanOtherCodeOfRow.isEmpty() && heldBelowRoom.bound ? referenceOf(heldBelowRoom.bound) : Reference();
            bool boundMayBeNonInteger = m_operationsThatRanOtherCodeOfRow.isEmpty() && !heldBelowRoom.viewWhoseLengthIsBound && !(preheaderBound && preheaderBound.isRecord() && m_types.at(typeOf(preheaderBound)).isInteger());
            if (!storesUnderCounterAloneRanOtherCode || boundMayBeNonInteger)
                heldBelowRoom.accesses.shrink(0);
        }
    }
    bool holdsCounterBelowRoom = !heldBelowRoom.accesses.isEmpty();
    bool guardsSecondCounters = !accessesNearCounter.accesses.isEmpty();
    bool headMayAskLesserOfBoundAndGreatestInt32 = !holdsCounterBelowRoom && genericVersionHasEntry && steppedAtHead.asTheyEntered.isEmpty() && !asksAtHead && !m_coldCopyDepth && !isUnderHandler() && !(specializedPreview ? specializedPreview->effects : preview.effects).maySuspend;
    LoopBoundComparison loopBoundComparison;
    if (headMayAskLesserOfBoundAndGreatestInt32)
        loopBoundComparison = findLoopBoundComparison(*this, inputHead, specializedPreview && !turnsThatRunOtherCodeComeRound ? specializedPreview->effects : preview.effects, entering, counters.span());
    auto invariantLoopBound = [&] {
        Reference made = nameOfOperatorMadeInFrontOfLoop(loopBoundComparison.number);
        return made ? made : referenceOf(loopBoundComparison.number);
    };
    auto asksWhetherBoundOfHeadIsNumber = [&](const HoistedSpeculation& guess) {
        return loopBoundComparison.relation && !loopBoundComparison.variable && guess.value == invariantLoopBound() && m_types.at(guess.tested).isNumber();
    };
    bool someReadAsksItsSite = std::ranges::any_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
        return ran && asksItsSiteAlone(index.value(ran));
    }) && !m_summaryHasUninlinedCall;
    bool countsOnShapeOfWhatTurnMakes = specializedPreview && std::ranges::any_of(specializedPreview->guessesCountedOn, [&](const GuessCountedOnByLooks& seen) {
        return seen.operand.index() >= preview.firstValue && seen.expected && m_types.at(seen.expected).hasObjects() && m_types.isUninformative(typeInRow(seen.operand));
    });
    bool leavesSpecializedVersionOnOutliers = someReadAsksItsSite || countsOnShapeOfWhatTurnMakes || turnsReadRowsOfNumbersThatTheyFind(*this, inputHead, listsRead.lists.span(), guesses.span());
    genericVersionSpeculates &= guessesOnlyAtListsTestedInFront || ((leavesSpecializedVersionOnOutliers || sumsAreAskedInFrontAlone) && guessesAtNothingButListsTestedInFront) || std::ranges::all_of(guesses, asksWhetherBoundOfHeadIsNumber);
    auto boundVariableRead = [&] {
        FieldOfScope read;
        read.variable = loopBoundComparison.variable;
        read.anticipatedRead = loopBoundComparison.number;
        return read;
    };
    auto loopBoundType = [&] {
        return loopBoundComparison.variable ? resultTypeOf(draftVariableRead(boundVariableRead())) : typeOf(invariantLoopBound());
    };
    bool isBoughtForBoundAlone = loopBoundComparison.relation && (!hasTestInFront || guardsNotedAreOnlyTestInFront) && hasNoCheckedBinding && !specializedPreview && !m_types.at(loopBoundType()).isNumberWithinBounds();
    hasTestInFront |= isBoughtForBoundAlone;
    bool hasNothingToAskInFront = !hasTestInFront && hasNoCheckedBinding && !specializedPreview && !holdsCounterBelowRoom && !guardsSecondCounters;
    bool turnAroundHoldsLists = isIterationEffectFree() && m_versionedLoops.last().frameIndex == frameIndex && m_versionedLoops.last().inputHead != inputHead && everyElementReadIsGuarded(*this, inputHead);
    bool isBoughtForListsFoundFromInFront = hasNothingToAskInFront && !turnAroundHoldsLists && genericVersionHasEntry && !preview.effects.mayRunOtherCode() && (turnsReadListsFoundFromInFront(program(), index, inputHead) || m_turnsOfRowTakeApartListsFromInFront || (m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces && !preview.effects.writes.overlaps(LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }))));
    if (hasNothingToAskInFront && !isBoughtForListsFoundFromInFront) {
        if (counters.isEmpty())
            return false;
        hasTestInFront = true;
    }
    genericVersionSpeculates &= hasNoCheckedBinding || specializedPreview || !listsRead.lists.isEmpty();
    genericVersionSpeculates &= !specializedPreview || !specializedPreview->countsOnMembersAbovePrimitives || !specializedPreview->guessesCountedOn.isEmpty();
    genericVersionSpeculates |= steppedAtHead.asTheyEntered.containsIf([&](const auto& stepped) {
        return stepped.second.isStringOrArray && guesses.containsIf([&](const HoistedSpeculation& guess) { return guess.value == stepped.second.list; });
    });
    genericVersionSpeculates &= !unswitches;
    if (!genericVersionSpeculates && counters.containsIf(testMayWellFail)) {
        counters.removeAllMatching(testMayWellFail);
        hasTestInFront = unswitches || !guesses.isEmpty() || !listsRead.lists.isEmpty() || !counters.isEmpty() || asksAtHead || !guardsAskedOfObjectsOfTurns.isEmpty() || !fieldsTestedInFront.isEmpty();
        if (!hasTestInFront && hasNoCheckedBinding && !specializedPreview && !holdsCounterBelowRoom && !guardsSecondCounters)
            return false;
    }
    for (size_t place = 0; specializedPreview && place < specializedPreview->guessesCountedOn.size(); ++place) {
        ValueID parameter = index.stripCasts(specializedPreview->guessesCountedOn[place].operand);
        BodyBlock head = index.block(inputHead);
        if (!index.isParameter(parameter) || index.blockOf(parameter) != inputHead)
            continue;
        uint32_t position = parameter.index() - head.firstParameter().index();
        if (position >= entering.size())
            continue;
        Reference enters = entering[position];
        bool isTestedAlready = std::ranges::find(entering.first(position), enters) != entering.first(position).end()
            || counters.containsIf([&](const HoistedCounterCheck& counter) { return counter.position == position; })
            || guesses.containsIf([&](const HoistedSpeculation& guess) { return guess.value == enters; });
        if (isTestedAlready)
            continue;
        if (TypeID tested = typeThatTypeTestAsksForWhereExpected(*this, enters, specializedPreview->guessesCountedOn[place].expected))
            counters.append({ position, tested });
    }
    if (headMayAskLesserOfBoundAndGreatestInt32 && specializedPreview && !loopBoundComparison.relation)
        loopBoundComparison = findLoopBoundComparison(*this, inputHead, turnsThatRunOtherCodeComeRound ? preview.effects : specializedPreview->effects, entering, counters.span());
    if ((!sharedEntry || sharedCopyIsPlainWalk) && !hasTestInFront && hasNoCheckedBinding && !counters.containsIf(isNoSum) && !holdsCounterBelowRoom && !guardsSecondCounters && !isBoughtForListsFoundFromInFront) {
        uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
        const Effects& ofTurns = specializedPreview->effects;
        BitVector callsFoundTheSame;
        auto isTheSameInEveryTurn = [&](auto& self, ValueID written, unsigned depth) -> bool {
            ValueID value = index.stripCasts(written);
            bool isNarrowed = false;
            for (ValueID wider; value.index() >= firstValueOfLoop && index.isParameter(value) && (wider = index.valueNarrowedInto(value)); isNarrowed = true)
                value = index.stripCasts(wider);
            if (value.index() < firstValueOfLoop)
                return true;
            if (!depth || index.isParameter(value))
                return false;
            BodyValue definition = index.value(value);
            if (isNarrowed && definition.opcode() == Opcode::Call)
                return false;
            if (definition.opcode() == Opcode::Builtin || definition.opcode() == Opcode::String)
                return true;
            if (definition.opcode() == Opcode::TypeTest || definition.opcode() == Opcode::ToBoolean || definition.opcode() == Opcode::LogicalNot)
                return self(self, definition.operand(0), depth - 1);
            if (definition.opcode() == Opcode::Call) {
                if (callsFoundTheSame.get(value.index() - firstValueOfLoop))
                    return true;
                bool takesTheSame = true;
                definition.forEachOperand([&](ValueID operand, uint32_t) {
                    takesTheSame = takesTheSame && self(self, operand, depth - 1);
                });
                if (takesTheSame)
                    callsFoundTheSame.set(value.index() - firstValueOfLoop);
                return takesTheSame;
            }
            if (definition.opcode() == Opcode::CheckInitialized)
                return self(self, definition.operand(0), depth - 1);
            if (definition.opcode() == Opcode::GetModuleVariable)
                return !declaredEffectsOfOperation(program(), Opcode::GetModuleVariable, definition).reads.overlaps(ofTurns.writes);
            if (definition.opcode() == Opcode::GetEnvironmentField)
                return !declaredEffectsOfOperation(program(), Opcode::GetEnvironmentField, definition).reads.overlaps(ofTurns.writes) && self(self, definition.environmentOperand(), depth - 1);
            if (definition.opcode() == Opcode::ElemGet)
                return !readsOfOwnKeyLookup(program()).overlaps(ofTurns.writes) && self(self, definition.objectOperand(), depth - 1) && self(self, definition.propertyOperand(), depth - 1);
            if (definition.opcode() != Opcode::PrivateGet && (definition.opcode() != Opcode::PropGet || definition.isFreeName()))
                return false;
            LocationMasks slot;
            slot.add(program(), MemoryLocation::objectField(keyOfAccessToSlot(program(), definition)));
            if (slot.overlaps(ofTurns.writes))
                return false;
            return self(self, definition.objectOperand(), depth - 1);
        };
        BackEdgeDominators backEdgeDominators(index, index.loopOf(inputHead));
        auto isAfterInvariantBranches = [&](BlockID block) {
            while (block != inputHead && !backEdgeDominators.contains(block)) {
                BlockID from = index.immediateDominator(block);
                if (!from)
                    return false;
                BodyTerminator fork = index.terminator(from);
                bool isDecidedBeforeLoop = index.numberOfIncomingEdges(block) != 1 || fork.kind() == TerminatorKind::Jump || (fork.kind() == TerminatorKind::Branch && isTheSameInEveryTurn(isTheSameInEveryTurn, fork.as<TerminatorKind::Branch>().conditionOperand(), 4));
                if (!isDecidedBeforeLoop)
                    return false;
                block = from;
            }
            return true;
        };
        bool somethingLeavesTheTurns = specializedPreview->countsOnMembersAbovePrimitives || specializedPreview->guessesCountedOn.containsIf([&](const GuessCountedOnByLooks& guess) {
            return isTheSameInEveryTurn(isTheSameInEveryTurn, guess.operand, 4);
        }) || (someReadAsksItsSite && std::ranges::any_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
            if (!ran || !asksItsSiteAlone(index.value(ran)) || !isAfterInvariantBranches(index.blockOf(ran)))
                return false;
            bool storesNumber = index.opcodeOf(ran) == Opcode::PropSet && m_types.at(typeInRow(index.value(ran).valueOperand())).isNumber();
            return isTheSameInEveryTurn(isTheSameInEveryTurn, storesNumber ? index.value(ran).objectOperand() : ran, 4);
        })) || std::ranges::any_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
            if (!ran || !backEdgeDominators.contains(index.blockOf(ran)))
                return false;
            BodyValue read = index.value(ran);
            bool readsList = read.opcode() == Opcode::ElemGet || (read.opcode() == Opcode::PropGet && !read.isFreeName() && read.key() == program().keyOf(BuiltinNames::length));
            TypeID list = readsList ? typeInRow(read.objectOperand()) : TypeID();
            ValueID key = read.opcode() == Opcode::ElemGet ? index.stripCasts(read.propertyOperand()) : ValueID();
            bool goesByFractions = key && index.isParameter(key) && !m_types.at(typeInRow(key)).isInteger();
            return list && (m_types.isUninformative(list) || (key && !goesByFractions && hasKind(m_types, list, OperandKind::Array))) && isTheSameInEveryTurn(isTheSameInEveryTurn, read.objectOperand(), 4);
        });
        auto someLookupInMapLeavesTheTurns = [&] {
            if (m_summaryHasUninlinedCall)
                return false;
            for (BlockID member : index.membersOf(loop)) {
                if (!backEdgeDominators.contains(member))
                    continue;
                for (BodyValue operation : index.block(member).values()) {
                    if (!lookupInMapIsOnlyReckonedWithAsNumber(*this, operation, TypesOfInputs(typeInRow)) || !isTheSameInEveryTurn(isTheSameInEveryTurn, operation.thisValueOperand(), 4))
                        continue;
                    ValueID key = index.stripCasts(operation.argumentsOperands()[0]);
                    if ((!index.isParameter(key) && index.opcodeOf(key) == Opcode::String) || isTheSameInEveryTurn(isTheSameInEveryTurn, key, 4))
                        return true;
                }
            }
            return false;
        };
        if (!somethingLeavesTheTurns && !m_firstOtherCodeRanInCopyOfRow && !someLookupInMapLeavesTheTurns())
            return false;
    }

    std::optional<Vector<ValueID, 8>> foundBehindLoop = findValuesOfLoopReadBehindIt(inputHead);
    if (!foundBehindLoop)
        return false;
    if (m_isVersioningDryRun) {
        m_genericVersionWouldSpeculate = genericVersionSpeculates;
        return true;
    }
    Vector<ValueID, 8> readBehindLoop = WTF::move(*foundBehindLoop);

    if (unswitches && !specializedPreview && preview.effects.mayRunOtherCode())
        return false;
    Origin origin = m_origin;
    BlockID genericStart = unswitches ? addBlock() : sharedStart;
    bool checksVariableInFront = false;
    bool hasRareGenericStartEdge = false;
    bool hasLikelyGenericStartEdge = false;
    auto checkInPreheader = [&](Draft&& read, Reference held = { }) -> Reference {
        Reference found = isUnreachable() ? Reference() : held ? held : build(WTF::move(read));
        if (!found || !found.isValue() || !(m_types.at(typeOf(found)).primitives & Primitive::Absent))
            return { };
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(found);
        test.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
        Reference isEmpty = build(WTF::move(test));
        if (!isEmpty || !isEmpty.isValue())
            return { };
        BlockID filled = addBlock();
        if (!genericStart)
            genericStart = addBlock(BlockFrequency::Rare);
        branch(isEmpty, genericStart, filled);
        checksVariableInFront = true;
        hasRareGenericStartEdge = true;
        continueIn(filled);
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(found);
        narrowed.setTestOperand(isEmpty);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(false);
        return build(WTF::move(narrowed));
    };
    for (VariableID variable : variablesCheckedInTurns) {
        Draft read(Opcode::GetModuleVariable, origin);
        read.setVariable(variable);
        bool isDeferred = !!plan().deferredDefinitions().findDeferred(variable);
        Reference held = checkInPreheader(WTF::move(read), isDeferred ? Reference() : m_state.variable(variable));
        if (!held)
            continue;
        m_state.noteEndOfDeadZone(variable);
        if (takeObjectOfDeferredVariable(*this, variable, held, origin))
            continue;
        m_state.noteVariable(variable, held);
        noteValueOfStableBinding(*this, variable, held);
    }
    for (const CheckedInstanceField& checked : checkedInstanceFields) {
        Draft read(Opcode::GetEnvironmentField, origin);
        read.setEnvironmentOperand(checked.instance);
        read.setLayout(checked.field.layout);
        read.setField(checked.field.field);
        Reference held = checkInPreheader(WTF::move(read));
        if (!held)
            continue;
        m_state.noteEndOfDeadZone(checked.instance, checked.field);
        noteEnvironmentField(checked.instance, checked.field, held);
        noteValueOfStableBinding(*this, checked.instance, checked.field, held);
    }
    if ((sharedCopyIsPlainWalk ? !checksVariableInFront : genericStart == sharedStart) && !hasTestInFront && !specializedPreview && !holdsCounterBelowRoom && !guardsSecondCounters && !isBoughtForListsFoundFromInFront)
        return false;
    letGuessFailUnasked(genericStart, origin);
    if (shouldFailSpeculations()) [[unlikely]] {
        hasRareGenericStartEdge = true;
        hasTestInFront = true;
    }
    if (!guardsAskedOfObjectsOfTurns.isEmpty() && !isUnreachable()) {
        Vector<LayoutID, 4> begun;
        for (const MemberConditionRecord& condition : guardsAskedOfObjectsOfTurns) {
            if (!program().at(condition.layout).has(LayoutFlags::IsConstructorFunction) || begun.contains(condition.layout))
                continue;
            begun.append(condition.layout);
            Draft begin(Opcode::BeginMemberGuards, origin);
            begin.setLayout(condition.layout);
            build(WTF::move(begin));
        }
        Draft test(Opcode::AreMembersUnchanged, origin);
        test.setConditions(guardsAskedOfObjectsOfTurns.span());
        if (Reference holds = build(WTF::move(test))) {
            BlockID passed = addBlock();
            if (!genericStart)
                genericStart = addBlock(BlockFrequency::Rare);
            branch(holds, passed, genericStart);
            hasRareGenericStartEdge = true;
            continueIn(passed);
            noteGuardsShownByTest(*this, guardsAskedOfObjectsOfTurns.span(), holds);
        }
    }
    Reference boundRoundedInFront;
    Reference lesserOfBoundAndGreatestInt32;
    Reference readOfVariableThatIsBound;
    auto askBoundOfCounter = [&] {
        Reference bound = loopBoundComparison.variable ? valueKnownOf(boundVariableRead()) : invariantLoopBound();
        if (!bound) {
            Draft inFront = draftReadBeforeHeader(boundVariableRead(), ReadBeforeHeaderUse::GuardOperand);
            inFront.setIsKnownInitialized(false);
            bound = build(WTF::move(inFront));
        }
        if (!bound || !bound.isValue())
            return false;
        bool isNumberByType = m_types.at(typeOf(bound)).isNumber();
        BlockFrequency whereItIsRounded = isNumberByType ? BlockFrequency::Normal : BlockFrequency::Rare;
        auto round = [&](Reference number) {
            Draft rounding(Opcode::IntegerBound, origin);
            chooseConservativeFields(rounding);
            rounding.setNumberOperand(number);
            Opcode asked = index.opcodeOf(loopBoundComparison.relation);
            rounding.setComparison(!shouldMisroundLoopBounds() ? asked : asked == Opcode::Less ? Opcode::LessEq : Opcode::Less);
            rounding.setNumberIsOnLeft(false);
            return build(WTF::move(rounding));
        };
        Reference rounded = isNumberByType ? round(bound) : Reference();
        if (isNumberByType && (!rounded || !rounded.isValue()))
            return false;
        Reference isInt32 = buildTestOfNumber(*this, isNumberByType ? rounded : bound, TypeTable::int32, origin);
        if (!isInt32 || !isInt32.isValue())
            return false;
        if (!genericStart)
            genericStart = addBlock(BlockFrequency::Rare);
        hasRareGenericStartEdge = true;
        auto continueIf = [&](Reference test) {
            if (!test || !test.isValue() || isUnreachable())
                return false;
            BlockID passed = addBlock(BlockFrequency::Rare);
            branch(test, passed, genericStart);
            continueIn(passed);
            return true;
        };
        Vector<JoinArm, 3> arms;
        Reference number = bound;
        if (!isNumberByType) {
            BlockID takesBound = addBlock();
            BlockID isNoInt32 = addBlock(BlockFrequency::Rare);
            branch(isInt32, takesBound, isNoInt32);
            continueIn(takesBound);
            if (Reference held = buildProofOfNumber(*this, bound, isInt32, origin))
                arms.append(JoinArm { takesBound, { held, held } });
            else if (!isUnreachable())
                jumpTo(genericStart);
            continueIn(isNoInt32);
            Reference isNumber = buildTestOfNumber(*this, bound, TypeTable::number, origin);
            number = continueIf(isNumber) ? buildProofOfNumber(*this, bound, isNumber, origin) : Reference();
        }
        if (!isNumberByType && number && !isUnreachable())
            rounded = round(number);
        Reference roundedIsInt32 = isNumberByType ? isInt32 : rounded && rounded.isValue() && !isUnreachable() ? buildTestOfNumber(*this, rounded, TypeTable::int32, origin) : Reference();
        if (roundedIsInt32 && roundedIsInt32.isValue()) {
            BlockID takesRounded = addBlock(whereItIsRounded);
            BlockID liesOutside = addBlock(BlockFrequency::Rare);
            branch(roundedIsInt32, takesRounded, liesOutside);
            continueIn(liesOutside);
            Draft written(Opcode::Integer, origin);
            written.setInteger(std::numeric_limits<int32_t>::max());
            Reference greatest = constant(WTF::move(written));
            if (continueIf(buildIntegerOperator(*this, Opcode::Less, greatest, rounded, origin)))
                arms.append(JoinArm { currentBlock(), { greatest, rounded } });
            else if (!isUnreachable())
                jumpTo(genericStart);
            continueIn(takesRounded);
            if (Reference held = buildProofOfNumber(*this, rounded, roundedIsInt32, origin))
                arms.append(JoinArm { takesRounded, { held, held } });
        } else if (!isUnreachable())
            jumpTo(genericStart);
        if (arms.isEmpty())
            return false;
        std::array<Reference, 2> met;
        if (isNumberByType) {
            for (JoinArm& arm : arms)
                arm.values.shrink(1);
        }
        if (!joinArms(arms.span(), addBlock(), std::span<Reference>(met).first(isNumberByType ? 1 : 2)))
            return false;
        lesserOfBoundAndGreatestInt32 = met[0];
        boundRoundedInFront = isNumberByType ? rounded : met[1];
        if (loopBoundComparison.variable) {
            ValueID read = loopBoundComparison.number;
            if (index.opcodeOf(read) == Opcode::CheckInitialized)
                read = index.stripCasts(index.value(read).operand(0));
            LocationMasks restsOn;
            if (!numberReadOutOfVariableStays(*this, loopBoundComparison.variable))
                restsOn = declaredEffectsOfOperation(program(), Opcode::GetModuleVariable, index.value(read)).reads;
            noteKnown(*this, KnownNote::VariableIsAsInFrontOfLoop, bound, { }, loopBoundComparison.variable.index(), restsOn, 1, true);
            readOfVariableThatIsBound = bound;
        }
        count(StatisticID::InvariantNumberGuardsHoisted);
        return true;
    };
    Vector<Reference, 4> proofs;
    for (const HoistedSpeculation& guess : guesses) {
        if (asksWhetherBoundOfHeadIsNumber(guess) && !isUnreachable() && (lesserOfBoundAndGreatestInt32 || askBoundOfCounter())) {
            proofs.append(Reference());
            continue;
        }
        Reference test = isUnreachable() ? Reference() : buildTestInFrontOfLoop(*this, guess, origin);
        if (!test) {
            proofs.append(Reference());
            continue;
        }
        BlockID passed = addBlock();
        if (!genericStart)
            genericStart = addBlock(BlockFrequency::Rare);
        branch(test, passed, genericStart);
        hasRareGenericStartEdge = true;
        continueIn(passed);
        Reference proof = buildProofInFrontOfLoop(*this, guess, test, origin);
        if (proof && proof.isValue() && guess.tested && m_types.at(guess.tested).isInt32() && representationOf(proof) == Representation::Double && m_types.at(typeOf(proof)).isInt32()) {
            if (Reference word = convert(proof, guess.isTermOfKey ? Representation::Int64 : Representation::Int32, origin); word && word.isValue() && word != proof) {
                Draft number(Opcode::IntegerToNumber, origin);
                number.setOperand(0, word);
                number.representation = Representation::Double;
                if (Reference held = build(WTF::move(number)))
                    proof = held;
            }
        }
        if (proof && proof.isValue() && guess.isTermOfKey && representationOf(proof) == Representation::JSValue && m_types.at(typeOf(proof)).isInt32()) {
            if (Reference word = convert(proof, Representation::Int64, origin))
                proof = word;
        }
        if (proof && proof.isValue() && guess.isOnlyReckonedWith && representationOf(proof) == Representation::JSValue && m_types.at(typeOf(proof)).isNumber()) {
            if (Reference number = convert(proof, Representation::Double, origin))
                proof = number;
        }
        proofs.append(proof);
        noteFieldArrayProof(guess.value, proof);
    }
    for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
        size_t guess = entered.hasProtocolFallback ? guesses.findIf([&](const HoistedSpeculation& asked) { return asked.value == entered.list; }) : notFound;
        if (guess != notFound && guess < proofs.size() && proofs[guess])
            m_indexedIterations[place].list = proofs[guess];
    }
    Vector<Reference, 8> specializedEntryValues;
    specializedEntryValues.append(entering);
    for (const HoistedCounterCheck& counter : counters) {
        Reference asked = entering[counter.position];
        if (asked && counter.isCheckedOnBackEdge && m_types.isSubset(typeOf(asked), counter.tested) && (!isTypeOfLink(m_types, counter.tested) || m_state.cellFacts(asked).contains(CellFact::BearsNoMark)))
            continue;
        if (!isUnreachable() && representationOf(asked) == Representation::OptionalDouble)
            asked = convert(asked, Representation::JSValue, origin);
        Reference test = isUnreachable() || !asked ? Reference() : buildLoopCarriedCheck(*this, asked, counter.tested, origin);
        if (!test)
            continue;
        BlockID passed = addBlock();
        if (!genericStart)
            genericStart = addBlock(counter.testMayWellFail ? BlockFrequency::Normal : BlockFrequency::Rare);
        branch(test, passed, genericStart);
        (counter.testMayWellFail ? hasLikelyGenericStartEdge : hasRareGenericStartEdge) = true;
        continueIn(passed);
        Reference proof = buildProofOfNumber(*this, asked, test, origin);
        specializedEntryValues[counter.position] = proof;
        if (Reference entered = entering[counter.position]; proof && proof.isValue() && entered.isValue() && proof != entered && !m_state.checkedNameOf(entered)) {
            m_state.noteCheckedName(entered, proof);
            count(StatisticID::CounterStartsNarrowed);
        }
    }
    for (const FieldOfScope& field : fieldsTestedInFront) {
        bool isOfAnotherFile = field.variable && hoistedImportedFunctionGuards.contains(field.variable);
        Reference read = isUnreachable() || isOfAnotherFile ? Reference() : valueKnownOf(field);
        if (!read && !isUnreachable()) {
            Draft inFront = draftReadBeforeHeader(field, ReadBeforeHeaderUse::GuardOperand, isOfAnotherFile);
            size_t queued = hoistedVariableChecks.findIf([&](const HoistedVariableCheck& asked) { return asked.variable == field.variable && asked.scope == field.scope && asked.field == field.field; });
            count(queued == notFound ? StatisticID::LoopStoredVariableReadsHoisted : hoistedVariableChecks[queued].counted);
            read = build(WTF::move(inFront));
        }
        Reference test = read ? buildTestOfNumber(*this, read, field.slotKey ? TypeID(TypeTable::int32) : field.type, origin) : Reference();
        if (!test || !test.isValue())
            continue;
        BlockID passed = addBlock();
        if (!genericStart)
            genericStart = addBlock(BlockFrequency::Rare);
        branch(test, passed, shouldIgnoreVariableChecks() ? passed : genericStart);
        continueIn(passed);
        if (isOfAnotherFile)
            holdWhatIsFoundInPlace(read, field.type, ProofSource::Variable);
        Reference proof = buildProofOfNumber(*this, read, test, origin);
        bool isOnlyReckonedWith = field.variable && hoistedVariableChecks.containsIf([&](const HoistedVariableCheck& asked) { return asked.variable == field.variable && asked.isOnlyReckonedWith; });
        if (proof && proof.isValue() && isOnlyReckonedWith && representationOf(proof) == Representation::JSValue && m_types.at(typeOf(proof)).isNumber()) {
            if (Reference number = convert(proof, Representation::Double, origin))
                proof = number;
        }
        if (proof && field.variable) {
            if (program().at(field.variable).initialization != VariableInitialization::Uninitialized || leavesDeadZoneForGood(program(), field.variable))
                m_state.noteEndOfDeadZone(field.variable);
            m_state.noteVariable(field.variable, proof);
            noteValueOfStableBinding(*this, field.variable, proof);
            if (proof != read && !m_state.checkedNameOf(read))
                m_state.noteCheckedName(read, proof);
        } else if (proof && field.slotKey) {
            Reference fromZeroUp = field.slotIsSteppedByOne && field.slotOnlyGrows ? buildOperatorInNumberForms(*this, Opcode::GreaterEq, proof, integerInNumberForms(*this, 0), origin) : Reference();
            if (fromZeroUp && fromZeroUp.isValue() && !m_code.isConstant(fromZeroUp.id())) {
                BlockID behind = addBlock();
                branch(fromZeroUp, behind, shouldIgnoreVariableChecks() ? behind : genericStart);
                continueIn(behind);
                proof = buildNarrowedInNumberForms(*this, proof, fromZeroUp, 0, true, origin);
            }
            if (proof)
                m_state.noteSlot(field.scope, field.field, proof);
        }
        else if (proof)
            noteEnvironmentField(field.scope, field.field, proof);
    }
    bool holdsCounterBelowLengthsOfLists = heldBelowRoom.accesses.containsIf([](const AccessUnderCounterOfLoop& access) { return access.isOfList; });
    std::optional<WalkState> knownInFrontOfTestsOfLists;
    auto buildTestsOfLists = [&] {
        if (listsRead.lists.containsIf([](const HoistedArrayGuard& list) { return list.hasAlternateStorage; }) && !listsRead.lists.containsIf(isConvertedInPreheader))
            knownInFrontOfTestsOfLists = m_state;
        SetForScope asked(m_restartablePreviewHeader, headOfLooksTriedForLists);
        buildTestsOfListsReadInLoop(*this, inputHead, entering, listsRead, guesses.span(), proofs.span(), genericStart, origin);
    };
    if (holdsCounterBelowLengthsOfLists)
        buildTestsOfLists();
    Reference lesserBound;
    Reference boundBesideLeastRoom;
    Reference viewWhoseLengthIsBound;
    VersionedLoop::CounterBelowRoomOfViews counterBelowRoom;
    if (holdsCounterBelowRoom && !isUnreachable()) {
        uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
        auto provenInFront = [&](Reference built) {
            for (size_t place = 0; place < guesses.size(); ++place) {
                if (guesses[place].value == built && proofs[place])
                    return proofs[place];
            }
            if (Reference checked = built && built.isValue() ? m_state.checkedNameOf(built) : Reference())
                return checked;
            return built;
        };
        auto builtInFront = [&](ValueID written) -> Reference {
            ValueID value = index.stripCasts(written);
            if (value.index() >= firstValueOfLoop) {
                std::optional<int64_t> number = integerInFront(value);
                if (!number) {
                    Reference field = fieldValueInPreheader(*this, index, value, firstValueOfLoop);
                    return field ? provenInFront(field) : Reference();
                }
                Draft integer(Opcode::Integer, origin);
                integer.setInteger(*number);
                return constant(WTF::move(integer));
            }
            return provenInFront(referenceOf(written.index() < firstValueOfLoop ? written : value));
        };
        auto viewBeforeLoop = [&](const AccessUnderCounterOfLoop& access) -> Reference {
            if (!access.variable)
                return access.fieldView ? provenInFront(access.fieldView) : builtInFront(access.view);
            if (Reference held = m_state.variable(access.variable))
                return held;
            FieldOfScope field;
            field.variable = access.variable;
            Reference held = isUnreachable() ? Reference() : build(draftReadBeforeHeader(field, ReadBeforeHeaderUse::GuardOperand));
            if (held && held.isValue())
                m_state.noteVariable(access.variable, held);
            return held;
        };
        auto isInteger = [&](Reference value) {
            return value && value.isRecord() && m_types.at(typeOf(value)).isInteger();
        };
        auto continueIfNonNegative = [&](Reference asked) -> std::optional<PassedTest> {
            if (isUnreachable() || !isInteger(asked))
                return std::nullopt;
            if (std::optional<PassedTest> byItsType = findNonNegativeProof(asked))
                return byItsType;
            Draft zero(Opcode::Integer, origin);
            zero.setInteger(0);
            Reference test = buildIntegerOperator(*this, Opcode::GreaterEq, asked, constant(WTF::move(zero)), origin);
            if (std::optional<PassedTest> known = findPassedTest(test))
                return known;
            if (!test || !test.isValue())
                return std::nullopt;
            if (!genericStart)
                genericStart = addBlock(BlockFrequency::Rare);
            hasRareGenericStartEdge = true;
            return continueIfCheckPasses(test, genericStart);
        };
        auto testOf = [&](const PassedTest& passed) {
            return passed.sides().empty() ? ValueID() : passed.sides().front().test;
        };
        auto lengthInFrontOfLoop = [&](Reference view, bool isList = false) -> Reference {
            if (std::optional<uint64_t> noted = findKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, view))
                return Reference::fromBits(static_cast<uint32_t>(*noted));
            Reference length = isUnreachable() ? Reference() : isList ? buildArrayLength(*this, view, origin) : buildRoomOfViewBesideAddend(*this, view, { }, origin);
            if (length && !isUnreachable())
                noteKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, view, { }, 0, isList ? placesUnderLengthsOfLists : LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), length.bits(), true);
            return length;
        };
        auto lesserOf = [&](Reference one, Reference other) -> Reference {
            Reference isLess = buildIntegerOperator(*this, Opcode::Less, one, other, origin);
            if (!isLess || !isLess.isValue() || isUnreachable())
                return { };
            BlockID takesOne = addBlock();
            BlockID takesOther = addBlock();
            branch(isLess, takesOne, takesOther);
            continueIn(takesOther);
            if (other.isValue()) {
                Draft narrowed(Opcode::Narrowed, origin);
                narrowed.setValueOperand(other);
                narrowed.setTestOperand(isLess);
                narrowed.setOperandIndex(OperandIndex::Less::right);
                narrowed.setIsOnTakenEdge(false);
                if (Reference held = build(WTF::move(narrowed)))
                    other = held;
            }
            Vector<JoinArm, 2> arms;
            arms.append(JoinArm { takesOne, { one } });
            arms.append(JoinArm { takesOther, { other } });
            Reference lesser;
            return joinArms(arms.span(), addBlock(), singleElementSpan(lesser)) ? lesser : Reference();
        };
        Reference enters = specializedEntryValues[heldBelowRoom.position];
        Reference bound = heldBelowRoom.viewWhoseLengthIsBound ? lengthInFrontOfLoop(builtInFront(heldBelowRoom.viewWhoseLengthIsBound)) : builtInFront(heldBelowRoom.bound);
        std::optional<PassedTest> stepsUp = !isInteger(enters) || !isInteger(bound) ? std::nullopt : heldBelowRoom.stepThatMayBeNegative ? continueIfNonNegative(builtInFront(heldBelowRoom.stepThatMayBeNegative)) : std::optional(PassedTest());
        struct AddendsAskedOfView {
            Reference view;
            Reference fromInFront;
            Range written;
        };
        Vector<AddendsAskedOfView, 4> addendsAskedOfViews;
        UncheckedKeyHashMap<uint32_t, Range, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> writtenAddendsOfViews;
        auto writtenAddend = [&](const AccessUnderCounterOfLoop& access) -> std::optional<int64_t> {
            int64_t sum = 0;
            for (const AddendOfKey& addend : access.addends) {
                std::optional<int64_t> integer = integerInFront(addend.value);
                if (!integer)
                    return std::nullopt;
                sum += addend.isSubtracted ? -*integer : *integer;
            }
            return sum;
        };
        auto addendInFront = [&](const AccessUnderCounterOfLoop& access) -> Reference {
            Reference sum;
            for (bool isSubtracted : { false, true }) {
                for (const AddendOfKey& addend : access.addends) {
                    if (addend.isSubtracted != isSubtracted)
                        continue;
                    Reference term = builtInFront(addend.value);
                    if (!isInteger(term))
                        return { };
                    if (!sum && isSubtracted) {
                        Draft zero(Opcode::Integer, origin);
                        zero.setInteger(0);
                        sum = constant(WTF::move(zero));
                    }
                    sum = sum ? buildIntegerOperator(*this, isSubtracted ? Opcode::Sub : Opcode::Add, sum, term, origin) : term;
                    if (!isInteger(sum) || isUnreachable())
                        return { };
                }
            }
            return sum;
        };
        auto integerOf = [&](int64_t value) -> Reference {
            if (!value)
                return { };
            Draft integer(Opcode::Integer, origin);
            integer.setInteger(value);
            return constant(WTF::move(integer));
        };
        for (const AccessUnderCounterOfLoop& access : heldBelowRoom.accesses) {
            Reference view = viewBeforeLoop(access);
            if (std::optional<int64_t> written = view ? writtenAddend(access) : std::nullopt) {
                Range& range = writtenAddendsOfViews.add(view.bits(), Range { *written, *written }).iterator->value;
                range = Range { std::min(range.minimum, *written), std::max(range.maximum, *written) };
            }
        }
        std::ranges::stable_partition(heldBelowRoom.accesses, [](const AccessUnderCounterOfLoop& access) {
            return !access.iterationChecksAddendSign;
        });
        Reference leastRoom;
        for (size_t place = 0; stepsUp && !m_hasFailed && place < heldBelowRoom.accesses.size(); ++place) {
            const AccessUnderCounterOfLoop& access = heldBelowRoom.accesses[place];
            Reference view = viewBeforeLoop(access);
            bool isWritten = view && writtenAddend(access);
            Reference addend = isWritten ? integerOf(writtenAddendsOfViews.get(view.bits()).maximum) : addendInFront(access);
            if (!view || !view.isValue() || (addend && !isInteger(addend)) || (!isWritten && !addend))
                continue;
            if (access.isOfList ? !findHoistedArrayGuard(*this, view) : !isViewOfNumbersReadInLine(*this, typeOf(view)))
                continue;
            Range written = isWritten ? writtenAddendsOfViews.get(view.bits()) : Range { 0, 0 };
            bool isAsked = addendsAskedOfViews.containsIf([&](const AddendsAskedOfView& asked) {
                if (asked.view != view)
                    return false;
                return isWritten ? !asked.fromInFront && asked.written.minimum <= written.minimum && written.maximum <= asked.written.maximum : asked.fromInFront == addend;
            });
            if (!isAsked) {
                Reference leastAddend = isWritten ? integerOf(written.minimum) : addend;
                bool isSignCheckedInIteration = access.iterationChecksAddendSign && findNonNegativeProof(enters);
                if (isSignCheckedInIteration)
                    count(StatisticID::AddendSignsCheckedInIterations);
                std::optional<PassedTest> leastIsNotNegative = isSignCheckedInIteration ? std::optional(PassedTest()) : continueIfNonNegative(leastAddend ? buildIntegerOperator(*this, Opcode::Add, enters, leastAddend, origin) : enters);
                if (!leastIsNotNegative)
                    continue;
                Reference room = lengthInFrontOfLoop(view, access.isOfList);
                if (room && addend && !isUnreachable())
                    room = buildIntegerOperator(*this, Opcode::Sub, room, addend, origin);
                if (!isInteger(room))
                    continue;
                Reference least = leastRoom && !isUnreachable() ? lesserOf(room, leastRoom) : room;
                if (!least || isUnreachable())
                    continue;
                leastRoom = least;
                addendsAskedOfViews.append(AddendsAskedOfView { view, isWritten ? Reference() : addend, written });
                if (ValueID test = testOf(*leastIsNotNegative); test && !counterBelowRoom.fromZeroUp.contains(test))
                    counterBelowRoom.fromZeroUp.append(test);
            }
            if (access.isOfList)
                count(StatisticID::LoopBoundsClampedToArrays);
        }
        if (leastRoom && heldBelowRoom.viewWhoseLengthIsBound && !isUnreachable())
            viewWhoseLengthIsBound = builtInFront(heldBelowRoom.viewWhoseLengthIsBound);
        if (leastRoom && !isUnreachable() && !m_hasFailed) {
            if (index.opcodeOf(heldBelowRoom.relation) == Opcode::LessEq) {
                Draft one(Opcode::Integer, origin);
                one.setInteger(1);
                leastRoom = buildIntegerOperator(*this, Opcode::Sub, leastRoom, constant(WTF::move(one)), origin);
            }
            lesserBound = isInteger(leastRoom) ? lesserOf(bound, leastRoom) : Reference();
            boundBesideLeastRoom = bound;
            counterBelowRoom.start = enters.id();
            counterBelowRoom.position = heldBelowRoom.position;
            counterBelowRoom.stepNonNegativeCheck = testOf(*stepsUp);
        }
    }
    Vector<VersionedLoop::SecondCounterGuard, 1> secondCounterGuards;
    if (guardsSecondCounters && genericVersionSpeculates && (!sharedEntry || sharedCopyIsPlainWalk) && !isUnreachable()) {
        uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
        auto builtInFront = [&](ValueID written) -> Reference {
            ValueID value = index.stripCasts(written);
            if (value.index() >= firstValueOfLoop) {
                Draft integer(Opcode::Integer, origin);
                integer.setInteger(index.value(value).as<Opcode::Integer>().integer());
                return constant(WTF::move(integer));
            }
            Reference built = referenceOf(written.index() < firstValueOfLoop ? written : value);
            for (size_t place = 0; place < guesses.size(); ++place) {
                if (guesses[place].value == built && proofs[place])
                    return proofs[place];
            }
            Reference checked = built && built.isValue() ? m_state.checkedNameOf(built) : Reference();
            return checked ? checked : built;
        };
        secondCounterGuards = guardSecondCounters(accessesNearCounter, specializedEntryValues.span(), genericStart, origin, scopedLambda<Reference(ValueID)>(builtInFront));
        hasRareGenericStartEdge |= !secondCounterGuards.isEmpty();
    }
    if (loopBoundComparison.relation && !lesserOfBoundAndGreatestInt32 && !isUnreachable())
        askBoundOfCounter();
    if (isBoughtForBoundAlone && !lesserOfBoundAndGreatestInt32) {
        if (!hasRareGenericStartEdge && !m_hasFailed)
            return false;
        fail("a loop bought for its bound alone asks no bound"_s);
        return true;
    }
    if (!holdsCounterBelowLengthsOfLists)
        buildTestsOfLists();
    Vector<VersionedLoop::SteppedKeyLimit, 2> steppedKeyLimits;
    auto provenInPreheader = [&](Reference built) {
        for (size_t place = 0; built && place < guesses.size() && place < proofs.size(); ++place) {
            if (guesses[place].value == built && proofs[place])
                return proofs[place];
        }
        Reference checked = built && built.isValue() ? m_state.checkedNameOf(built) : Reference();
        return checked ? checked : built;
    };
    auto findTestedArray = [&](ValueID object) -> Reference {
        Reference built = provenInPreheader(referenceOf(object));
        size_t listed = built ? listsRead.lists.findIf([&](const HoistedArrayGuard& list) { return list.name == built && hasArrayStorageForm(list.storage); }) : notFound;
        return listed == notFound ? Reference() : listsRead.lists[listed].name;
    };
    auto findSteps = [&](uint32_t position) { return stepsOfHeadParameter(inputHead, position); };
    bool mayBoundSteppedKeys = listsRead.areTested && genericVersionHasEntry && !m_refusesSecondCounters && !m_coldCopyDepth && !unswitches && !isUnderHandler() && steppedAtHead.asTheyEntered.isEmpty() && !asksAtHead;
    for (const SteppedKey& key : mayBoundSteppedKeys && !isUnreachable() && !m_hasFailed ? findSteppedKeys(*this, inputHead, scopedLambda<std::optional<Range>(uint32_t)>(findSteps), scopedLambda<Reference(ValueID)>(findTestedArray)) : Vector<SteppedKey, 2>()) {
        static constexpr Range summedExactly { -maxExactInteger / 4, maxExactInteger / 4 };
        auto isSummedExactly = [&](Reference number) {
            return number && number.isRecord() && m_types.at(typeOf(number)).isInteger() && m_types.at(typeOf(number)).integers.isSubsetOf(summedExactly);
        };
        Reference stepped = specializedEntryValues[key.steppedPosition];
        Reference counter = specializedEntryValues[key.counterPosition];
        Reference bound = key.counterRises ? provenInPreheader(referenceOf(key.bound)) : Reference();
        if (isUnreachable() || !isSummedExactly(stepped) || !isSummedExactly(counter) || (key.counterRises && !isSummedExactly(bound)) || m_types.at(typeOf(stepped)).integers.minimum < 0)
            continue;
        Reference limit = buildIntegerOperator(*this, Opcode::Add, stepped, key.counterRises ? bound : counter, origin);
        if (limit && key.counterRises)
            limit = buildIntegerOperator(*this, Opcode::Sub, limit, counter, origin);
        Reference length = limit ? buildArrayLength(*this, key.list, origin) : Reference();
        Reference test = length ? buildIntegerOperator(*this, key.admitsBound ? Opcode::Less : Opcode::LessEq, limit, length, origin) : Reference();
        if (!test || !test.isValue() || m_code.isConstant(test.id()) || isUnreachable())
            continue;
        if (!genericStart)
            genericStart = addBlock(BlockFrequency::Rare);
        hasRareGenericStartEdge = true;
        if (!continueIfCheckPasses(test, genericStart))
            continue;
        steppedKeyLimits.append(VersionedLoop::SteppedKeyLimit { .list = key.list, .length = length, .steppedPosition = key.steppedPosition, .counterPosition = key.counterPosition, .counterRises = key.counterRises, .limitCheck = test.id() });
    }
    bool hasGenericStartEdgeFromGuards = false;
    if (sharedCopyIsPlainWalk && !hasRareGenericStartEdge) {
        m_code.forEachIncomingEdgeUnsealed(genericStart, [&](const Edge&) {
            hasGenericStartEdgeFromGuards = true;
        });
    }
    if (m_hasFailed)
        return true;
    if (!genericStart && !asksAtHead && hasTestInFront && !counters.containsIf(isCheckedOnBackEdge)) {
        fail("a loop is versioned without a guard"_s);
        return true;
    }
    recordSpeculation();
    ++m_versionedLoopCount;
    m_versionedLoopNestCount += index.loop(loop).numberOfLoopsWithin > 1;
    if (steppedAtHead.asTheyEntered.containsIf([](const auto& entered) { return entered.second.hasProtocolFallback && entered.second.isDeadAfterLoop && entered.second.contents == IndexedIteration::Contents::Text; }))
        count(StatisticID::StringForOfLoopsVersioned);
    BlockID genericVersionEntry = sharedEntry;
    Vector<Reference, 8> genericEntryParameters;
    Vector<Reference, 2> genericEntryPositions;
    Vector<Reference, 4> carriedInFrontOfLoop;
    Vector<Reference, 4> genericEntryCarriedValues;
    Vector<Reference, 8> enteringStepParts;
    if (loopCarriesValuesOfConsumerRound(inputHead))
        carriedInFrontOfLoop = carriedValuesInFrame(frameIndex);
    auto bindCarriedValues = [&](std::span<const Reference> carried) {
        for (uint32_t number = 0; number < carried.size(); ++number)
            bind(ValueID(index.numberOfValues() + number), carried[number]);
    };
    if (!sharedEntry && genericVersionHasEntry) {
        genericVersionEntry = addBlock(otherCopyAsksCountersAlone || findReentry(frameIndex, inputHead) ? BlockFrequency::Unlikely : genericVersionSpeculates ? BlockFrequency::Normal : BlockFrequency::Rare);
        m_genericVersionEntries.add(genericVersionEntry.index());
        for (size_t index = 0; index < entering.size(); ++index) {
            TypeID type = joinTypes(preview.typesOfParameters[index], ownTypeOf(entering[index]));
            genericEntryParameters.append(addParameter(genericVersionEntry, type, formOfParameter(type, { })));
        }
        for (size_t place = 0; place < steppedAtHead.asTheyEntered.size(); ++place) {
            TypeID type = m_types.integer(Range { 0, doneIterationPosition });
            genericEntryPositions.append(addParameter(genericVersionEntry, type, formOfParameter(type, { })));
        }
        for (Reference carried : carriedInFrontOfLoop) {
            size_t placeInRow = entering.size() + genericEntryPositions.size() + genericEntryCarriedValues.size();
            TypeID type = ownTypeOf(carried);
            if (placeInRow < preview.typesOfParameters.size() && preview.typesOfParameters[placeInRow])
                type = joinTypes(preview.typesOfParameters[placeInRow], type);
            genericEntryCarriedValues.append(addParameter(genericVersionEntry, type, formOfParameter(type, { })));
        }
        addStepEntryParameters(*this, inputHead, genericVersionEntry, entering, genericEntryParameters.span(), enteringStepParts);
        addScopeFieldParameters(genericVersionEntry, preview);
        addParametersForSlotsOfObjects(genericVersionEntry, inputHead, preview, genericEntryParameters.span());
        settleParametersOf(genericVersionEntry);
    }
    takeIterationsAsTheyEntered();
    for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
        if (!genericVersionSpeculates || !entered.isStringOrArray || entered.contents != IndexedIteration::Contents::List)
            continue;
        m_indexedIterations[place].contents = IndexedIteration::Contents::Text;
        m_indexedIterations[place].builtinClass = BuiltinClasses::String;
    }

    for (ValueID value : readBehindLoop)
        m_frames[frameIndex].addDuplicatedValue(value);
    noteValuesThatHandlersOutsideLoopRead(inputHead);
    m_frames[frameIndex].loopsBeingVersioned.append(loop);
    uint32_t narrowerNamesBeforeLoop = inputBlock(frameIndex, inputHead).firstNarrowerName;

    SetForScope ordinalOfCopy(m_ordinalOfCopyOfLoop, m_ordinalOfCopyOfLoop * 16 + 1);
    BlockID specializedStart = isUnreachable() ? BlockID() : m_block;
    WalkState stateWhereTestsPassed = m_state;
    if (isPreheaderEnteredFromIteration && !genericStart)
        genericStart = addBlock(BlockFrequency::Rare);
    WalkState ownGenericStartState;
    if (isPreheaderEnteredFromIteration)
        ownGenericStartState = stateWhereTestsPassed;
    bool visitsGenericVersionLast = unswitches && !m_state.holdsFreshObjects();
    if (!sharedEntry) {
        if (visitsGenericVersionLast) {
            m_block = { };
            m_state.clear();
        } else if (unswitches) {
            m_block = addBlock(BlockFrequency::Rare);
            m_state = stateWhereTestsPassed;
        } else if (genericStart && !frontLeadsOnAsItDid)
            continueIn(genericStart);
        else {
            m_state.clear();
            if (genericVersionEntry) {
                genericStart = addBlock(BlockFrequency::Rare);
                m_state = stateWhereTestsPassed;
            }
            m_block = genericStart;
        }
        if (genericVersionEntry && !visitsGenericVersionLast) {
            if (!isUnreachable()) {
                Vector<Reference, 8> withPositions;
                withPositions.append(entering);
                for (size_t place = 0; place < genericEntryPositions.size(); ++place)
                    withPositions.append(steppedAtHead.asTheyEntered[place].second.position);
                withPositions.appendVector(carriedInFrontOfLoop);
                withPositions.appendVector(enteringStepParts);
                enterWithFieldsOfScopes(genericVersionEntry, withPositions.span());
            }
            bindCarriedValues(genericEntryCarriedValues.span());
            for (size_t place = 0; place < genericEntryPositions.size(); ++place)
                m_indexedIterations[steppedAtHead.asTheyEntered[place].first].position = genericEntryPositions[place];
            enterGenericVersion(genericVersionEntry, preview);
        }
        if (otherCopyAsksCountersAlone && genericVersionEntry) {
            count(StatisticID::CounterOnlyFallbackVersionsBuilt);
            uint32_t versionedLoopCountBefore = m_versionedLoopCount;
            SetForScope asksCountersAlone(m_walksCopyThatAsksCountersAlone, true);
            SetForScope endedLists(m_typedBuildEndedListsTestedInFront, m_typedBuildEndedListsTestedInFront);
            SetForScope asksGuards(m_typedBuildAsksGuardsThatItWasBoughtFor, m_typedBuildAsksGuardsThatItWasBoughtFor);
            SetForScope everyTurnLeaves(m_everyTurnOfTypedBuildLeaves, m_everyTurnOfTypedBuildLeaves);
            SetForScope leavesWithoutTest(m_turnOfTypedBuildLeavesWithoutTest, m_turnOfTypedBuildLeavesWithoutTest);
            SetForScope testsNothing(m_typedBuildTestsNothing, m_typedBuildTestsNothing);
            SetForScope isLeftAtOtherCode(m_typedBuildIsLeftAtOtherCodeOnUsualWay, m_typedBuildIsLeftAtOtherCodeOnUsualWay);
            SetForScope askedNoList(m_typedBuildAskedNoListThatItWasBoughtFor, m_typedBuildAskedNoListThatItWasBoughtFor);
            SetForScope report(m_reportOfTypedBuildMadeLast, m_reportOfTypedBuildMadeLast);
            SetForScope walkWasKept(m_walkOfLoopWasKept, m_walkOfLoopWasKept);
            std::pair<uint32_t, BlockID> boughtFirst { static_cast<uint32_t>(frameIndex), inputHead };
            bool firstPurchaseIsListed = !m_loopsInFinalBuild.isEmpty() && m_loopsInFinalBuild.last() == boughtFirst;
            if (firstPurchaseIsListed)
                m_loopsInFinalBuild.removeLast();
            bool wayAroundBeginsLoop = !m_versionedLoops.isEmpty() && m_versionedLoops.last().frameIndex == frameIndex && m_versionedLoops.last().inputHead == inputHead;
            size_t placeOfWayAround = m_versionedLoops.size() - wayAroundBeginsLoop;
            BlockID startOfWayAround = wayAroundBeginsLoop ? std::exchange(m_versionedLoops[placeOfWayAround].genericStart, BlockID()) : BlockID();
            if (isGenericVersionSharedByFallback) {
                m_sharedGenericVersions.last().isAboutToBeTaken = true;
                m_sharedGenericVersions.last().isEnteredFromIteration = true;
            } else {
                m_frames[frameIndex].loopsBeingVersioned.removeLast();
                m_reversionableLoops.append({ frameIndex, loop });
            }
            walkCopyOfLoop(inputHead, genericEntryParameters.span());
            if (wayAroundBeginsLoop)
                m_versionedLoops[placeOfWayAround].genericStart = startOfWayAround;
            if (isGenericVersionSharedByFallback) {
                m_sharedGenericVersions.last().isAboutToBeTaken = false;
                m_sharedGenericVersions.last().isEnteredFromIteration = false;
            } else {
                m_reversionableLoops.removeLast();
                m_frames[frameIndex].loopsBeingVersioned.append(loop);
            }
            if (firstPurchaseIsListed)
                m_loopsInFinalBuild.append(boughtFirst);
            otherCopyBoughtNothing = listsRead.someListIsTakenUnderCounterAskedInFront && !isGenericVersionSharedByFallback && m_versionedLoopCount == versionedLoopCountBefore;
        } else if (visitsGenericVersionLast) {
        } else if (genericVersionSpeculates) {
            bool isInsideSpecializedVersion = m_versionedLoops.containsIf([&](const VersionedLoop& versionedLoop) { return versionedLoop.frameIndex != frameIndex || versionedLoop.inputHead != inputHead; });
            if (isInsideSpecializedVersion)
                m_genericVersionsInsideSpecialized.append({ frameIndex, inputHead });
            walkCopyOfLoop(inputHead, genericVersionEntry ? genericEntryParameters.span() : entering);
            if (isInsideSpecializedVersion)
                m_genericVersionsInsideSpecialized.removeLast();
        } else {
            ClosedArm closedArm(*this);
            ++m_coldCopyDepth;
            walkCopyOfLoop(inputHead, genericVersionEntry ? genericEntryParameters.span() : entering);
            --m_coldCopyDepth;
        }
    }
    if (otherCopyBoughtNothing)
        m_typedBuildEndedListsTestedInFront = true;
    forgetValuesOfLoop(inputHead);

    BlockID startOfCopyBeside = !unswitches && specializedStart && genericVersionEntry && !lesserBound && !m_summaryHasUninlinedCall && !m_walksCopyWithoutSomeIterationGuesses && !m_hasFailed ? addBlock() : BlockID();
    WalkState stateOfCopyBeside;
    if (startOfCopyBeside)
        stateOfCopyBeside = stateWhereTestsPassed;
    bool otherCodeCameRoundByRow = turnsThatRunOtherCodeComeRound;
    bool copyBesideIsEntered = false;
    bool copyBesideIsBoughtByItself = false;
    for (bool isCopyBeside : { false, true }) {
        if (!specializedStart || m_hasFailed || (isCopyBeside && !copyBesideIsEntered))
            break;
        SetForScope walksCopyBeside(m_walksCopyWithoutSomeIterationGuesses, m_walksCopyWithoutSomeIterationGuesses || isCopyBeside);
        if (isCopyBeside) {
            for (BlockID member : index.membersOf(loop))
                inputBlock(frameIndex, member).firstNarrowerName = narrowerNamesBeforeLoop;
            forgetValuesOfLoop(inputHead);
            turnsThatRunOtherCodeComeRound = otherCodeCameRoundByRow;
            count(StatisticID::LoopsReversionedWithoutConditionalSpeculations);
        }
        m_block = isCopyBeside ? startOfCopyBeside : specializedStart;
        m_state = isCopyBeside ? WTF::move(stateOfCopyBeside) : WTF::move(stateWhereTestsPassed);
        bindCarriedValues(carriedInFrontOfLoop.span());
        putBackIterationsSteppedAt(steppedAtHead);
        if (isCopyBeside && copyBesideIsBoughtByItself) {
            count(StatisticID::LoopsReversionedWithoutHoistedGuards);
            SetForScope endedLists(m_typedBuildEndedListsTestedInFront, m_typedBuildEndedListsTestedInFront);
            SetForScope asksGuards(m_typedBuildAsksGuardsThatItWasBoughtFor, m_typedBuildAsksGuardsThatItWasBoughtFor);
            SetForScope everyTurnLeaves(m_everyTurnOfTypedBuildLeaves, m_everyTurnOfTypedBuildLeaves);
            SetForScope leavesWithoutTest(m_turnOfTypedBuildLeavesWithoutTest, m_turnOfTypedBuildLeavesWithoutTest);
            SetForScope testsNothing(m_typedBuildTestsNothing, m_typedBuildTestsNothing);
            SetForScope isLeftAtOtherCode(m_typedBuildIsLeftAtOtherCodeOnUsualWay, m_typedBuildIsLeftAtOtherCodeOnUsualWay);
            SetForScope askedNoList(m_typedBuildAskedNoListThatItWasBoughtFor, m_typedBuildAskedNoListThatItWasBoughtFor);
            SetForScope report(m_reportOfTypedBuildMadeLast, m_reportOfTypedBuildMadeLast);
            SetForScope walkWasKept(m_walkOfLoopWasKept, m_walkOfLoopWasKept);
            SetForScope leavesOutQuestions(m_walksCopyWithoutQuestionsOfEveryTurn, true);
            m_sharedGenericVersions.append(SharedGenericVersion { frameIndex, inputHead, genericStart, genericVersionEntry, true });
            m_versionedLoops.append(VersionedLoop { frameIndex, inputHead, genericVersionEntry, { }, genericStart, { }, false });
            m_versionedLoops.last().hasCallInIteration = someOperationOfRowThatRanOtherCodeIsCall(index), m_versionedLoops.last().hasUninlinedCall = m_summaryHasUninlinedCall;
            m_versionedLoops.last().mayRestartUnderHandler = mayVersionLoopUnderHandler(inputHead);
            m_versionedLoops.last().restartsAtCaughtThrows = m_handlersRunOtherCode;
            m_versionedLoops.last().lastBackEdgeDominator = BackEdgeDominators(index, index.loopOf(inputHead)).lastOf(index.loopOf(inputHead));
            ++m_ordinalOfCopyOfLoop;
            std::pair<uint32_t, BlockID> boughtFirst { static_cast<uint32_t>(frameIndex), inputHead };
            bool firstPurchaseIsListed = !m_loopsInFinalBuild.isEmpty() && m_loopsInFinalBuild.last() == boughtFirst;
            if (firstPurchaseIsListed)
                m_loopsInFinalBuild.removeLast();
            walkCopyOfLoop(inputHead, entering);
            if (firstPurchaseIsListed)
                m_loopsInFinalBuild.append(boughtFirst);
            popVersionedLoop();
            m_sharedGenericVersions.removeLast();
            break;
        }
        for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
            size_t guess = entered.hasProtocolFallback ? guesses.findIf([&](const HoistedSpeculation& asked) { return asked.value == entered.list; }) : notFound;
            if (guess == notFound || !proofs[guess])
                continue;
            IndexedIteration& iteration = m_indexedIterations[place];
            iteration.hasProtocolFallback = false;
            if (entered.isArrayOrMap)
                iteration.table = { };
            iteration.list = proofs[guess];
            iteration.state = { };
            iteration.iterator = { };
            iteration.next = { };
            iteration.stateHoldsVerdictOnNext = false;
            if (entered.contents == IndexedIteration::Contents::Text && isUnderHandler() && !isUnderHandlerOtherThanCleanups())
                flatStringMadeWhereWalkStands(*this, proofs[guess], m_origin);
        }
        Vector<std::pair<ValueID, Reference>, 8> boundBefore;
        for (size_t place = 0; place < guesses.size(); ++place) {
            if (!proofs[place])
                continue;
            for (ValueID name : guesses[place].names) {
                boundBefore.append({ name, referenceBoundTo(name) });
                bind(name, proofs[place]);
                if (isMadeInFrontOfLoop(name))
                    noteOperatorMadeInFrontOfLoop(name, proofs[place]);
            }
            if (guesses[place].fieldInPreheader)
                noteFieldProof(*this, guesses[place].fieldInPreheader->holder, guesses[place].fieldInPreheader->key, proofs[place]);
            if (!guesses[place].readOfVariableInLoop)
                continue;
            BodyValue read = index.value(guesses[place].readOfVariableInLoop);
            if (read.opcode() == Opcode::GetModuleVariable) {
                if (program().at(read.variable()).initialization != VariableInitialization::Uninitialized || leavesDeadZoneForGood(program(), read.variable()))
                    m_state.noteEndOfDeadZone(read.variable());
                m_state.noteVariable(read.variable(), proofs[place]);
                noteValueOfStableBinding(*this, read.variable(), proofs[place]);
            } else if (Reference environment = loopInvariantScope(*this, index, read.environmentOperand(), index.block(inputHead).firstParameter().index())) {
                FieldReference field { read.layout(), read.field() };
                m_state.noteEndOfDeadZone(environment, field);
                noteEnvironmentField(environment, field, proofs[place]);
                noteValueOfStableBinding(*this, environment, field, proofs[place]);
            }
        }
        if (genericVersionEntry)
            m_versionedLoops.append(VersionedLoop { frameIndex, inputHead, genericVersionEntry, { }, genericStart, { }, specializedPreview || (listsRead.areTested && headOfLooksTriedForLists == inputHead) });
        if (genericVersionEntry)
            m_versionedLoops.last().genericVersionSpeculates = genericVersionSpeculates && (!sharedEntry || sharedCopyIsPlainWalk || frontLeadsToNextStartOfMember);
        if (genericVersionEntry && arrayGuardedAtHeader) {
            m_versionedLoops.last().arrayGuardedAtHeader = arrayGuardedAtHeader->name;
            m_versionedLoops.last().storageGuardedAtHeader = arrayGuardedAtHeader->storage;
            m_versionedLoops.last().isArrayGuardedAtHeaderWritable = arrayGuardedAtHeader->isWritable;
        }
        if (genericVersionEntry) {
            m_versionedLoops.last().mayRestartUnderHandler = mayVersionLoopUnderHandler(inputHead);
            m_versionedLoops.last().restartsAtCaughtThrows = m_handlersRunOtherCode;
        }
        if (genericVersionEntry)
            m_versionedLoops.last().lastBackEdgeDominator = BackEdgeDominators(index, index.loopOf(inputHead)).lastOf(index.loopOf(inputHead));
        if (unswitches)
            m_versionedLoops.last().unswitchCandidates = unswitchCandidates, m_versionedLoops.last().isUnswitched = takesInt32s;
        if (genericVersionEntry && !isCopyBeside)
            m_versionedLoops.last().startOfCopyWithoutSomeIterationGuesses = startOfCopyBeside;
        if (genericVersionEntry)
            m_versionedLoops.last().variableBackEdgeChecks.appendVector(fieldsTestedInFront);
        if (genericVersionEntry)
            m_versionedLoops.last().hasCallInIteration = someOperationOfRowThatRanOtherCodeIsCall(index), m_versionedLoops.last().hasUninlinedCall = m_summaryHasUninlinedCall;
        bool stepsTextsAlone = std::ranges::all_of(steppedAtHead.asTheyEntered, [](const auto& entered) {
            return entered.second.contents == IndexedIteration::Contents::Text && entered.second.otherForm == IndexedIteration::OtherForm::None;
        });
        bool otherCodeMayStay = !unswitches && turnsThatRunOtherCodeComeRound && genericVersionEntry && (!sharedEntry || sharedCopyIsPlainWalk) && !asksAtHead && !holdsCounterBelowRoom && !guardsSecondCounters && listsRead.lists.isEmpty() && stepsTextsAlone
            && std::ranges::all_of(fieldsTestedInFront, [&](const FieldOfScope& field) {
                return !field.variable || program().at(field.variable).initialization != VariableInitialization::Uninitialized || leavesDeadZoneForGood(program(), field.variable);
            });
        bool otherCodeStays = otherCodeMayStay && m_otherCodeStaysInTypedBuildOfRow;
        turnsThatRunOtherCodeComeRound = otherCodeStays || (otherCodeMayStay && !sharedEntry && !counters.containsIf(isNoSum) && hasNoCheckedBinding);
        bool otherCodeIsKeptOutOfFirstCopy = startOfCopyBeside && !isCopyBeside && !otherCodeStays && !m_otherCodeIsKeptOutOfFirstCopyInVainOfRow && turnsThatRunOtherCodeComeRound;
        if (otherCodeIsKeptOutOfFirstCopy)
            turnsThatRunOtherCodeComeRound = false;
        if (genericVersionEntry)
            m_versionedLoops.last().turnsThatRunOtherCodeComeRound = turnsThatRunOtherCodeComeRound;
        if (genericVersionEntry)
            m_versionedLoops.last().steppedKeyLimits = WTF::move(steppedKeyLimits);
        if (genericVersionEntry && lesserBound) {
            m_versionedLoops.last().relationThatAsksLesserBound = heldBelowRoom.relation;
            m_versionedLoops.last().lesserBound = lesserBound;
            m_versionedLoops.last().boundBesideLesserBound = boundBesideLeastRoom;
            m_versionedLoops.last().viewWhoseLengthIsBound = viewWhoseLengthIsBound;
            m_versionedLoops.last().counterBelowRoom = WTF::move(counterBelowRoom);
        } else if (genericVersionEntry && lesserOfBoundAndGreatestInt32) {
            m_versionedLoops.last().relationThatAsksLesserBound = loopBoundComparison.relation;
            m_versionedLoops.last().lesserBound = lesserOfBoundAndGreatestInt32;
            m_versionedLoops.last().boundBesideLesserBound = boundRoundedInFront;
            m_versionedLoops.last().variableThatIsBound = loopBoundComparison.variable;
            m_versionedLoops.last().readOfVariableThatIsBound = readOfVariableThatIsBound;
        }
        if (genericVersionEntry)
            m_versionedLoops.last().secondCounterGuards = secondCounterGuards;
        for (const HoistedCounterCheck& counter : counters) {
            if (genericVersionEntry && counter.isCheckedOnBackEdge) {
                m_versionedLoops.last().backEdgeChecks.append({ counter.position, counter.tested });
                if (m_types.at(counter.tested).hasObjects())
                    count(StatisticID::LoopCarriedObjectsShapeChecked);
            }
        }
        ++m_ordinalOfCopyOfLoop;
        if (turnsThatRunOtherCodeComeRound)
            m_effectsSeededForNextRow = preview.effects;
        else if (hasSpecializedPreview || listsRead.areTested)
            m_effectsSeededForNextRow = specializedEffectsFromSummary;
        else if (genericVersionEntry && (lesserBound || !secondCounterGuards.isEmpty()) && !storesUnderCounterAloneRanOtherCode)
            m_effectsSeededForNextRow = preview.effects;
        else if (genericVersionEntry && lesserBound) {
            Effects ofStores = Effects::none();
            ofStores.writes = LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayElements });
            ofStores.merge(m_effectsOutsideArmsOfRow);
            m_effectsSeededForNextRow = ofStores;
        }
        uint32_t guessesOpenedInFront = m_speculationCount;
        uint32_t listsAskedInFront = m_hoistedArrayGuardCount;
        uint32_t elementsAskedInFront = m_hoistedElementCheckCount;
        uint32_t numbersReadInFront = m_hoistedNumberReadCount;
        m_walkOfLoopWasKept = false;
        m_buildsTestsBesidePlainWalk = false;
        {
            SetForScope headOverLists(m_headOfTypedBuildOverListsTestedInFront, listsRead.areTested ? inputHead : BlockID());
            SetForScope frameOverLists(m_frameOfTypedBuildOverListsTestedInFront, frameIndex);
            Vector<uint32_t, 2> listsMadeHere;
            for (const HoistedArrayGuard& list : listsRead.lists) {
                if (listsRead.areTested && list.name.isValue() && !list.isWritable)
                    listsMadeHere.append(underlyingCellOf(m_code, list.name.id()).index());
            }
            SetForScope listsOverLists(m_listsMadeHereOfTypedBuildOverListsTestedInFront, WTF::move(listsMadeHere));
            walkCopyOfLoop(inputHead, specializedEntryValues.span());
        }
        m_typedBuildTestsNothing = !hasTestInFront && hasNoCheckedBinding && m_speculationCount == guessesOpenedInFront;
        m_typedBuildAskedNoListThatItWasBoughtFor = (turnsAreQuietBehindListsFoundFromInFrontAlone || isBoughtForListsFoundFromInFront) && !hasTestInFront && hasNoCheckedBinding && !holdsCounterBelowRoom && !guardsSecondCounters && m_hoistedArrayGuardCount == listsAskedInFront;
        m_walkOfLoopWasKept &= m_headOfLoopKeptLast == inputHead && m_frameOfLoopKeptLast == frameIndex;
        m_typedBuildAskedNoListThatItWasBoughtFor |= m_walkOfLoopWasKept && m_effectsOfLoopKeptLast.maySuspend;
        m_typedBuildAsksGuardsThatItWasBoughtFor = guardsNotedAreOnlyTestInFront && !isBoughtForBoundAlone && !isBoughtForListsFoundFromInFront && hasNoCheckedBinding && !specializedPreview && !holdsCounterBelowRoom && !guardsSecondCounters && m_walkOfLoopWasKept
            && std::ranges::any_of(m_guardsOfStoresAskedOfRow, [&](const MemberConditionRecord& noted) { return m_guardsOfStoresAskedInLoopKeptLast.contains(noted); });
        if (isBoughtForListsFoundFromInFront && m_walkOfLoopWasKept && !m_typedBuildAskedNoListThatItWasBoughtFor)
            count(StatisticID::InvariantArrayLoopsVersioned);
        bool failingInFrontIsNoRarity = genericVersionEntry && m_versionedLoops.last().frontAsksWhatNothingSpeaksFor;
        if (failingInFrontIsNoRarity && genericStart)
            m_code.makeBlockUnlikely(genericStart, true);
        if (m_walkOfLoopWasKept)
            m_reportOfTypedBuildMadeLast = ReportOfTypedBuild { m_effectsOfLoopKeptLast, genericVersionSpeculates, !failingInFrontIsNoRarity && !hasLikelyGenericStartEdge && (hasRareGenericStartEdge || !hasGenericStartEdgeFromGuards) };
        if (listsRead.areTested && !m_hasFailed && m_walkOfLoopWasKept && !preservesHoistedArrayGuards(*this, m_effectsOfLoopKeptLast, listsRead.lists.span(), inputHead))
            m_typedBuildEndedListsTestedInFront = true;
        else if (listsRead.areTested && !m_hasFailed && m_walkOfLoopWasKept && !preservesHoistedArrayGuards(*this, m_effectsOfLoopKeptLast))
            count(StatisticID::VersionedLoopsFillingLocalArrays);
        if (!m_hasFailed && m_walkOfLoopWasKept && m_hoistedElementCheckCount != elementsAskedInFront && (m_effectsOfLoopKeptLast.writes.maskOf(AbstractHeap::Elements) & cellHoldingElementsMask))
            m_typedBuildEndedListsTestedInFront = true;
        bool turnsStoreElements = m_effectsOfLoopKeptLast.mayRunOtherCode() || m_effectsOfLoopKeptLast.writes.maskOf(AbstractHeap::Elements);
        if (!m_hasFailed && m_walkOfLoopWasKept && m_hoistedNumberReadCount != numbersReadInFront && turnsStoreElements)
            m_typedBuildEndedListsTestedInFront = true;
        m_hoistedNumberReadCount = numbersReadInFront;
        m_hoistedElementCheckCount = elementsAskedInFront;
        m_effectsSeededForNextRow = std::nullopt;
        if ((isBoughtForBoundAlone || (holdsCounterBelowRoom && !hasTestInFront)) && hasNoCheckedBinding && !specializedPreview && !(genericVersionEntry && m_versionedLoops.last().headAsksLesserBound))
            m_everyTurnOfTypedBuildLeaves = true;
        m_typedBuildIsLeftAtOtherCodeOnUsualWay = false;
        if (BlockID typedHead = inputBlock(frameIndex, inputHead).head; typedHead && otherCodeMayStay && !turnsThatRunOtherCodeComeRound && m_versionedLoops.last().isLeftAtOtherCodeOnUsualWay) {
            std::span<const ValueID> carried = m_code.at(typedHead).parameters();
            std::span<const ValueID> taken = m_code.at(genericVersionEntry).parameters();
            m_typedBuildIsLeftAtOtherCodeOnUsualWay = !sharedEntry && !genericVersionSpeculates;
            for (size_t place = 0; place < std::min(carried.size(), taken.size()); ++place)
                m_typedBuildIsLeftAtOtherCodeOnUsualWay |= m_code.at(taken[place]).representation == Representation::JSValue && m_code.at(carried[place]).representation != Representation::JSValue;
            if (m_versionedLoops.last().headAsksLesserBound && m_everyTurnOfTypedBuildLeaves && !m_otherCodeStayedInVainOfRow) {
                m_typedBuildIsLeftAtOtherCodeOnUsualWay = true;
                m_everyTurnOfTypedBuildLeaves = false;
            }
        }
        copyBesideIsEntered = !isCopyBeside && genericVersionEntry && m_versionedLoops.last().guessOfSomeTurnsIsAskedInFront;
        copyBesideIsBoughtByItself = copyBesideIsEntered && m_versionedLoops.last().questionOfEveryTurnIsAskedInFront;
        if (otherCodeIsKeptOutOfFirstCopy && (m_versionedLoops.last().isLeftAtOtherCodeOnUsualWay || m_versionedLoops.last().isLeftAtAccessOnUsualWay || m_everyTurnOfTypedBuildLeaves)) {
            m_everyTurnOfTypedBuildLeaves = false;
            m_typedBuildIsLeftAtOtherCodeOnUsualWay = true;
            m_otherCodeIsKeptOutOfFirstCopyInVainOfRow = true;
            copyBesideIsEntered = false;
        }
        if (unswitches) {
            const VersionedLoop& versionedLoop = m_versionedLoops.last();
            m_unswitchFallbackStart = genericStart;
            m_isUnswitchedVersionQuiet = m_walkOfLoopWasKept && !m_hasUnswitchFailed && versionedLoop.isUnswitched && !versionedLoop.isLeftAtOtherCodeOnUsualWay && !versionedLoop.isLeftAtAccessOnUsualWay && !m_effectsOfLoopKeptLast.mayRunOtherCode();
        }
        if (genericVersionEntry)
            popVersionedLoop();
        for (auto [name, reference] : boundBefore)
            bind(name, reference);
    }
    if (visitsGenericVersionLast && !m_hasFailed) {
        forgetValuesOfLoop(inputHead);
        bool isEntered = false;
        m_code.forEachIncomingEdgeUnsealed(genericVersionEntry, [&](const Edge&) {
            isEntered = true;
        });
        m_state.clear();
        m_block = genericVersionEntry;
        if (!isEntered || !m_isUnswitchedVersionQuiet)
            endAsUnreachable(origin);
        else {
            for (BlockID member : index.membersOf(loop))
                inputBlock(frameIndex, member).firstNarrowerName = narrowerNamesBeforeLoop;
            bindCarriedValues(genericEntryCarriedValues.span());
            enterGenericVersion(genericVersionEntry, preview);
            ClosedArm closedArm(*this);
            ++m_coldCopyDepth;
            walkCopyOfLoop(inputHead, genericEntryParameters.span());
            --m_coldCopyDepth;
        }
    }
    if (startOfCopyBeside && !copyBesideIsEntered && !m_hasFailed) {
        m_state = WTF::move(stateOfCopyBeside);
        m_block = startOfCopyBeside;
        jumpTo(genericStart, { });
    }
    if (listsRead.alternateStorageEntry && !m_hasFailed) {
        for (BlockID member : index.membersOf(loop))
            inputBlock(frameIndex, member).firstNarrowerName = narrowerNamesBeforeLoop;
        forgetValuesOfLoop(inputHead);
        switchToInputBlock(inputHead);
        m_frames[frameIndex].isInPreheader = true;
        if (isUnderHandler()) {
            for (size_t place = 0; place < entering.size(); ++place)
                bind(index.block(inputHead).parameter(place), entering[place]);
        }
        continueIn(listsRead.alternateStorageEntry);
        bindCarriedValues(carriedInFrontOfLoop.span());
        if (knownInFrontOfTestsOfLists && !isUnreachable())
            m_state = WTF::move(*knownInFrontOfTestsOfLists);
        if (!isUnreachable() && (!genericVersionEntry || !genericStart || !specializedStart || (sharedEntry && !sharedCopyIsPlainWalk))) {
            if (!genericStart) {
                fail("the version for the alternate storage has no generic version to fall back to"_s);
                return true;
            }
            jumpTo(genericStart, { });
        } else if (!isUnreachable()) {
            putBackIterationsSteppedAt(steppedAtHead);
            count(StatisticID::LoopsReversionedForOtherIndexingType);
            SetForScope forAnotherKind(m_walksCopyForAnotherKindOfStorage, true);
            SetForScope forBoxedNumbers(m_walksLoopOverBoxedNumbers, listsRead.lists.containsIf([](const HoistedArrayGuard& list) { return list.isNextWalkedForBoxedNumbers && list.hasAlternateStorage; }));
            SetForScope loopOfOtherKind(m_loopOfOtherKind, std::pair { static_cast<uint32_t>(m_frames.size()), inputHead });
            SetForScope walkWasKept(m_walkOfLoopWasKept, m_walkOfLoopWasKept);
            if (sharedCopyIsPlainWalk) {
                SetForScope besidePlainWalk(m_buildsTestsBesidePlainWalk, true);
                SetForScope buys(m_buysTwoBuilds, true);
                bool endedLists = m_typedBuildEndedListsTestedInFront;
                bool asksGuards = m_typedBuildAsksGuardsThatItWasBoughtFor;
                bool everyTurnLeaves = m_everyTurnOfTypedBuildLeaves;
                bool leavesWithoutTest = m_turnOfTypedBuildLeavesWithoutTest;
                bool testsNothing = m_typedBuildTestsNothing;
                bool isLeftAtOtherCode = m_typedBuildIsLeftAtOtherCodeOnUsualWay;
                bool askedNoList = m_typedBuildAskedNoListThatItWasBoughtFor;
                std::optional<ReportOfTypedBuild> ofFirstCopy = std::exchange(m_reportOfTypedBuildMadeLast, std::nullopt);
                size_t sharedInFront = m_sharedGenericVersions.size();
                m_sharedGenericVersions.append(SharedGenericVersion { frameIndex, inputHead, genericStart, genericVersionEntry, true, true });
                ++m_ordinalOfCopyOfLoop;
                bool isBuilt = tryVersionLoop(inputHead, preview, entering);
                m_sharedGenericVersions.shrink(sharedInFront);
                if (!isBuilt && !m_hasFailed && !isUnreachable())
                    jumpTo(genericStart, { });
                std::optional<ReportOfTypedBuild> ofSecondCopy = std::exchange(m_reportOfTypedBuildMadeLast, std::nullopt);
                if (ofFirstCopy && (ofSecondCopy || !isBuilt)) {
                    if (ofSecondCopy) {
                        ofFirstCopy->effects.merge(ofSecondCopy->effects);
                        ofFirstCopy->genericVersionSpeculates &= ofSecondCopy->genericVersionSpeculates;
                        ofFirstCopy->startOfOtherCopyIsRare &= ofSecondCopy->startOfOtherCopyIsRare;
                    }
                    m_reportOfTypedBuildMadeLast = WTF::move(ofFirstCopy);
                }
                m_typedBuildEndedListsTestedInFront |= endedLists;
                m_typedBuildAsksGuardsThatItWasBoughtFor |= asksGuards;
                m_everyTurnOfTypedBuildLeaves |= everyTurnLeaves;
                m_turnOfTypedBuildLeavesWithoutTest |= leavesWithoutTest;
                m_typedBuildTestsNothing |= testsNothing;
                m_typedBuildIsLeftAtOtherCodeOnUsualWay |= isLeftAtOtherCode;
                m_typedBuildAskedNoListThatItWasBoughtFor |= askedNoList;
            } else {
                SetForScope endedLists(m_typedBuildEndedListsTestedInFront, m_typedBuildEndedListsTestedInFront);
                SetForScope asksGuards(m_typedBuildAsksGuardsThatItWasBoughtFor, m_typedBuildAsksGuardsThatItWasBoughtFor);
                SetForScope everyTurnLeaves(m_everyTurnOfTypedBuildLeaves, m_everyTurnOfTypedBuildLeaves);
                SetForScope leavesWithoutTest(m_turnOfTypedBuildLeavesWithoutTest, m_turnOfTypedBuildLeavesWithoutTest);
                SetForScope testsNothing(m_typedBuildTestsNothing, m_typedBuildTestsNothing);
                SetForScope isLeftAtOtherCode(m_typedBuildIsLeftAtOtherCodeOnUsualWay, m_typedBuildIsLeftAtOtherCodeOnUsualWay);
                SetForScope askedNoList(m_typedBuildAskedNoListThatItWasBoughtFor, m_typedBuildAskedNoListThatItWasBoughtFor);
                SetForScope report(m_reportOfTypedBuildMadeLast, m_reportOfTypedBuildMadeLast);
                m_sharedGenericVersions.append(SharedGenericVersion { frameIndex, inputHead, genericStart, genericVersionEntry, true });
                m_versionedLoops.append(VersionedLoop { frameIndex, inputHead, genericVersionEntry, { }, genericStart, { }, false });
                m_versionedLoops.last().hasCallInIteration = someOperationOfRowThatRanOtherCodeIsCall(index), m_versionedLoops.last().hasUninlinedCall = m_summaryHasUninlinedCall;
                m_versionedLoops.last().mayRestartUnderHandler = mayVersionLoopUnderHandler(inputHead);
                m_versionedLoops.last().restartsAtCaughtThrows = m_handlersRunOtherCode;
                m_versionedLoops.last().lastBackEdgeDominator = BackEdgeDominators(index, index.loopOf(inputHead)).lastOf(index.loopOf(inputHead));
                ++m_ordinalOfCopyOfLoop;
                std::pair<uint32_t, BlockID> boughtFirst { static_cast<uint32_t>(frameIndex), inputHead };
                bool firstPurchaseIsListed = !m_loopsInFinalBuild.isEmpty() && m_loopsInFinalBuild.last() == boughtFirst;
                if (firstPurchaseIsListed)
                    m_loopsInFinalBuild.removeLast();
                walkCopyOfLoop(inputHead, entering);
                if (firstPurchaseIsListed)
                    m_loopsInFinalBuild.append(boughtFirst);
                popVersionedLoop();
                m_sharedGenericVersions.removeLast();
            }
        }
    }
    takeIterationsAsTheyEntered();

    for (BlockID member : index.membersOf(loop))
        inputBlock(frameIndex, member).firstNarrowerName = narrowerNamesBeforeLoop;
    forgetValuesOfLoop(inputHead);
    if (isPreheaderEnteredFromIteration && genericStart && !m_hasFailed) {
        if (m_block != genericStart) {
            m_block = { };
            m_state.clear();
            continueIn(genericStart);
        }
        if (isUnreachable()) {
            bool hoistedTestsLeadHere = false;
            m_code.forEachIncomingEdgeUnsealed(genericStart, [&](const Edge&) {
                hoistedTestsLeadHere = true;
            });
            if (hoistedTestsLeadHere) {
                m_block = genericStart;
                m_state = WTF::move(ownGenericStartState);
            }
        }
        if (!isUnreachable() && canEnterWith(sharedEntry, entering))
            enterWithFieldsOfScopes(sharedEntry, entering);
        else if (!isUnreachable()) {
            count(StatisticID::GenericLoopVersionsDuplicated);
            {
                ClosedArm closedArm(*this);
                SetForScope coldCopyScope(m_coldCopyDepth, m_coldCopyDepth + 1);
                walkCopyOfLoop(inputHead, entering);
            }
            for (BlockID member : index.membersOf(loop))
                inputBlock(frameIndex, member).firstNarrowerName = narrowerNamesBeforeLoop;
            forgetValuesOfLoop(inputHead);
        }
    }
    if (genericVersionEntry && !sharedEntry)
        forgetWhatWasBelievedAt(genericVersionEntry);
    m_frames[frameIndex].loopsBeingVersioned.removeLast();
    leaveBlockPending();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
