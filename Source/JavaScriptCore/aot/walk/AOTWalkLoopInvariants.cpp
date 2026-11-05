/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTWalkListReadsInLoops.h"
#include <algorithm>
#include <optional>
#include <wtf/IterationStatus.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsWrittenOnceForLoopInvariants { AbstractHeap::ShapeWords };

static constexpr OpcodeSet readsKeptByStateInLoopInvariants { Opcode::GetModuleVariable, Opcode::GetEnvironmentField };

static bool findsSameInEveryTurnForLoopInvariants(const Program& program, const BodyValue& operation, const Effects& effectsOfLoop)
{
    Opcode opcode = operation.opcode();
    if (!readsKeptByStateInLoopInvariants.contains(opcode))
        return false;
    Effects effects = declaredEffectsOfOperation(program, opcode, operation);
    return effects.isUnobservable() && !effects.reads.overlaps(effectsOfLoop.writes);
}

void hoistInvariants(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering)
{
    if (!walker.has(Mechanism::LoopInvariants) || !walker.has(Mechanism::Folding) || preview.effects.mayRunOtherCode() || walker.loopCarriesValuesOfConsumerRound(inputHead))
        return;

    Walker::ClosedArm withoutGuesses(walker);
    BodyBlock head = walker.currentBodyIndex().block(inputHead);
    for (uint32_t index = 0; index < head.numberOfParameters(); ++index)
        walker.bind(head.parameter(index), entering[index]);
    Vector<ValueID, 8> hoisted;
    uint32_t position = 0;
    for (BodyValue operation : head.values()) {
        uint32_t place = position++;
        bool takesOnlyWhatStandsInFront = true;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            takesOnlyWhatStandsInFront &= operand.index() < head.firstParameter().index() || hoisted.contains(operand);
        });
        if (takesOnlyWhatStandsInFront && findsSameInEveryTurnForLoopInvariants(walker.program(), operation, preview.effects)) {
            walker.walkStretch(inputHead, place, place + 1);
            hoisted.append(operation.id());
            continue;
        }
        if (operation.opcode() != Opcode::LoopHint && !letsNothingElseRun(operation.opcode()))
            return;
    }
}

static constexpr OpcodeSet readsByNameForLoopInvariants { Opcode::PropGet, Opcode::PropHas };
static constexpr OpcodeSet storesByNameForLoopInvariants { Opcode::PropSet, Opcode::PropDefine };
static constexpr OpcodeSet accessesByKeyForLoopInvariants { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas };

static Effects effectsOfTypedReadByNameForLoopInvariants()
{
    Effects effects = Effects::unknownCode();
    effects.writes = effects.writes.restrictedTo(heapsWhoseWritesNobodySees);
    effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    effects.mayCallUnknownCode = false;
    effects.mayBeTerminated = false;
    return effects;
}

std::optional<Effects> effectsOfTypedFormAhead(Walker& walker, const BodyValue& operation, const TypesOfInputs& types, FunctionID* halfRunBehindLooks)
{
    Opcode opcode = operation.opcode();
    bool isByKey = accessesByKeyForLoopInvariants.contains(opcode);
    if (!walker.has(Mechanism::TypedOperations) || !(isByKey || readsByNameForLoopInvariants.contains(opcode) || storesByNameForLoopInvariants.contains(opcode)))
        return std::nullopt;

    walker.takeFormAskedAhead();
    TypedForm typedForm = typedFormOf(walker, operation, types);
    bool isBuiltBehindLooks = !isByKey && walker.looksTakeFormsBehindLooksAsBuilt() && typedForm != TypedForm::None && typedForm != TypedForm::BuiltAsCall && typedForm != TypedForm::BuiltOnObjectAsMade;
    bool isFoundAbovePrimitive = typedForm == TypedForm::BuiltOnObjectAsMade && walker.looksTakeFormsBehindLooksAsBuilt()
        && (!walker.types().at(types(operation.objectOperand())).hasObjects() || findMemberThatGuessAsksByItsByte(walker, operation, types(operation.objectOperand())));
    if (typedForm != TypedForm::Built && !isBuiltBehindLooks && !isFoundAbovePrimitive && !(typedForm == TypedForm::BuiltOnObjectAsMade && readsLengthOfViewTestedInFrontOfLoop(walker, operation)))
        return std::nullopt;
    Walker::FormAskedAhead form = walker.takeFormAskedAhead();
    if (form.opcode == Opcode::CallDirect) {
        if (halfRunBehindLooks)
            *halfRunBehindLooks = form.function;
        return std::nullopt;
    }

    if (isByKey)
        return form.level == KnowledgeLevel::Proven ? std::optional<Effects>(declaredBoundOf(form.opcode)) : std::nullopt;
    if (readsByNameForLoopInvariants.contains(opcode)) {
        Effects effects = effectsOfTypedReadByNameForLoopInvariants();
        if (TypeID object = types(operation.objectOperand()); object && walker.types().at(walker.types().withoutNullish(object)).hasOnlyLayouts())
            effects.writes.remove(AbstractHeapSet { AbstractHeap::MemberGuards });
        if (form.opcode == Opcode::GetDeferredClosureSlotOrUndefined) {
            for (Opcode read : { Opcode::GetDeferredClosureSlot, Opcode::GetDeferredClosureSlotOrUndefined }) {
                Draft built(read);
                built.setKey(operation.key());
                effects.merge(declaredEffectsOfOperation(walker.program(), read, built));
            }
        }
        if (form.opcode == Opcode::GetWatchedBuiltinValue)
            effects.merge(declaredBoundOf(Opcode::GetWatchedBuiltinValue));
        if (form.opcode == Opcode::InitializeBuiltin)
            effects.merge(declaredBoundOf(Opcode::InitializeBuiltin));
        return effects;
    }

    Draft store(Opcode::PutSlotOrAdd);
    store.setKey(operation.key());
    static_assert(declaredBoundOf(Opcode::PutSlot).writes.isSubsetOf(declaredBoundOf(Opcode::PutSlotOrAdd).writes));
    Effects effects = declaredEffectsOfOperation(walker.program(), Opcode::PutSlotOrAdd, store);
    if (typedStoreAheadMaySetMarkOfCell(walker, types(operation.objectOperand()), operation.key(), types(operation.valueOperand())))
        effects.writes.add(AbstractHeap::NamedProperties);
    return effects;
}

bool maySuspendAsWritten(const BodyValue& operation)
{
    switch (operation.opcode()) {
    case Opcode::IteratorResume:
        return operation.as<Opcode::IteratorResume>().iteratorKind() != IteratorKind::Sync;
    case Opcode::IteratorClose:
        return operation.as<Opcode::IteratorClose>().iteratorKind() != IteratorKind::Sync;
    default:
        return maySuspend(operation.opcode());
    }
}

Walker::ProofsHoldEverywhere::ProofsHoldEverywhere(Walker& walker)
    : m_walker(walker)
{
    ++walker.m_numberOfScopesWhereProofsHoldEverywhere;
}

Walker::ProofsHoldEverywhere::~ProofsHoldEverywhere()
{
    --m_walker.m_numberOfScopesWhereProofsHoldEverywhere;
}

void Walker::openLoop(OpenLoop&& loop)
{
    loop.pollInFront = std::exchange(m_pollInFrontOfLoopBeingEntered, { });
    uint32_t place = static_cast<uint32_t>(m_openLoops.size()) + 1;
    Frame& frame = m_frames[loop.frame];
    if (frame.placesOfOpenLoops.isEmpty())
        frame.placesOfOpenLoops.fill(0, frame.index->numberOfLoops());
    RELEASE_ASSERT(!frame.placesOfOpenLoops[loop.loop.index()]);
    if (m_serialOfNewestMark)
        m_placesOfOpenLoopsInTries.append(PlaceOfOpenLoopInTry { static_cast<uint32_t>(loop.frame), loop.loop, 0 });
    frame.placesOfOpenLoops[loop.loop.index()] = place;
    if (loop.blockInFront) {
        while (m_placesOfLoopsBehindBlocks.size() <= loop.blockInFront.index())
            m_placesOfLoopsBehindBlocks.append(0);
        RELEASE_ASSERT(!m_placesOfLoopsBehindBlocks[loop.blockInFront.index()]);
        setPlaceOfLoopBehindBlock(loop.blockInFront, place);
    }
    m_openLoops.append(WTF::move(loop));
}

void Walker::closeNewestLoop()
{
    const OpenLoop& loop = m_openLoops.last();
    settleFateOfPoll(loop);
    m_headsOfClosedLoops.add(loop.head.index());
    if (loop.frontOfQuietTurns)
        m_headsOfClosedLoops.add(loop.frontOfQuietTurns.index());
    if (loop.frame < m_frames.size() && !m_frames[loop.frame].placesOfOpenLoops.isEmpty()) {
        if (m_serialOfNewestMark)
            m_placesOfOpenLoopsInTries.append(PlaceOfOpenLoopInTry { static_cast<uint32_t>(loop.frame), loop.loop, m_frames[loop.frame].placesOfOpenLoops[loop.loop.index()] });
        m_frames[loop.frame].placesOfOpenLoops[loop.loop.index()] = 0;
    }
    if (loop.blockInFront)
        setPlaceOfLoopBehindBlock(loop.blockInFront, 0);
    for (BlockID earlier : loop.earlierBlocksInFront)
        setPlaceOfLoopBehindBlock(earlier, 0);
    for (BlockID block : loop.blocksOfFrontOfQuietTurns)
        setPlaceOfLoopBehindBlock(block, 0);
    m_openLoops.removeLast();
}

template<typename Functor>
void Walker::forEachOpenLoopAround(uint32_t numberOfLoops, size_t firstFrame, const Functor& functor) const
{
    for (size_t frameIndex = m_frames.size(); frameIndex-- > firstFrame;) {
        const Frame& frame = m_frames[frameIndex];
        if (frame.placesOfOpenLoops.isEmpty())
            continue;
        for (LoopID loop = frame.index->loopOf(frame.block); loop; loop = frame.index->loop(loop).parent) {
            uint32_t place = frame.placesOfOpenLoops[loop.index()];
            if (place && place <= numberOfLoops && functor(place - 1) == IterationStatus::Done)
                return;
        }
    }
}

void Walker::leaveBlockOpenInFrontOfLoop(BlockID inputHead, BlockID head, std::span<const Reference> entering, const Effects& effectsOfLoop)
{
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    RELEASE_ASSERT(parameters.size() == entering.size());

    OpenLoop loop;
    loop.head = head;
    loop.effects = effectsOfLoop;
    loop.frame = static_cast<uint32_t>(m_frames.size() - 1);
    loop.loop = currentBodyIndex().loopOf(inputHead);
    loop.firstRecord = parameters.empty() ? m_code.numberOfValues() : parameters[0].index();
    loop.origin = m_origin;
    if (!has(Mechanism::LoopInvariants)) {
        openLoop(WTF::move(loop));
        crossEdge({ }, head, entering);
        return;
    }

    if (!makeReadyToCross(head, entering, true, loop.entering))
        return;
    layFrontOfQuietTurns(loop, inputHead);
    if (m_hasFailed)
        return;
    loop.blockInFront = m_block;
    BlockID front = loop.frontOfQuietTurns;
    openLoop(WTF::move(loop));
    if (front) {
        noteBlockOfFrontOfQuietTurns(m_openLoops.size() - 1, front);
        letFrontOfQuietTurnsFailUnasked(m_openLoops.size() - 1);
    }
    m_block = { };
    if (!m_frontsSharedWithTypedBuilds.isEmpty() && m_frontsSharedWithTypedBuilds.last().frameIndex == m_frames.size() - 1 && m_frontsSharedWithTypedBuilds.last().inputHead == inputHead) {
        for (ValueID built : m_frontsSharedWithTypedBuilds.last().builtInFront)
            noteBuiltInFrontOfLoop(static_cast<uint32_t>(m_openLoops.size() - 1), built);
    }
}

void Walker::endBlocksInFrontOfLoops(size_t frameIndex, BlockID next)
{
    size_t numberOfLoopsAroundTurn = 0;
    for (const GeneratorWalkedInConsumer& walked : m_generatorsWalkedInConsumers) {
        if (walked.frameOfTurn == frameIndex)
            numberOfLoopsAroundTurn = std::max<size_t>(numberOfLoopsAroundTurn, walked.numberOfLoopsAroundTurn);
    }
    while (m_openLoops.size() > numberOfLoopsAroundTurn) {
        const OpenLoop& loop = m_openLoops.last();
        if (loop.frame < frameIndex)
            return;
        if (loop.frame == frameIndex && next && next.index() <= m_frames[frameIndex].index->loop(loop.loop).last.index())
            return;
        BlockID entered = closeFrontOfQuietTurns(loop);
        if (loop.blockInFront && !m_code.at(loop.blockInFront).hasEnded()) {
            m_code.end(loop.blockInFront, TerminatorDraft(TerminatorKind::Jump, loop.origin), 1);
            m_code.closeEdge(m_code.at(loop.blockInFront).terminator.next(), entered, loop.entering);
        }
        closeNewestLoop();
    }
}

void Walker::endBlocksInFrontOfLoopsFrom(size_t numberOfLoopsKept)
{
    while (m_openLoops.size() > numberOfLoopsKept) {
        const OpenLoop& loop = m_openLoops.last();
        BlockID entered = closeFrontOfQuietTurns(loop);
        if (loop.blockInFront && !m_code.at(loop.blockInFront).hasEnded()) {
            m_code.end(loop.blockInFront, TerminatorDraft(TerminatorKind::Jump, loop.origin), 1);
            m_code.closeEdge(m_code.at(loop.blockInFront).terminator.next(), entered, loop.entering);
        }
        closeNewestLoop();
    }
}

bool Walker::standsInFrontOfLoop(uint32_t indexOfLoop, Reference reference) const
{
    if (!reference.isValue())
        return reference.isConstant();
    if (reference.id().index() < m_openLoops[indexOfLoop].firstRecord)
        return true;
    BlockID block = m_code.blockOf(reference.id());
    uint32_t place = block && block.index() < m_placesOfLoopsBehindBlocks.size() ? m_placesOfLoopsBehindBlocks[block.index()] : 0;
    return place && place - 1 <= indexOfLoop;
}

static bool mayLeaveSomeLoopForLoopInvariants(const TypeArena& types, const Draft& draft, const Effects& effects, bool proofsHoldEverywhere)
{
    Opcode opcode = draft.opcode;
    if (resultRuleOf(opcode) == ResultRule::None || resultRuleOf(opcode) == ResultRule::Allocation || interpretedOf(opcode) == Interpretation::Allocation)
        return false;
    if (opcode == Opcode::IsElementPresent)
        return false;
    if (opcode == Opcode::Narrowed || opcode == Opcode::AssumeType || opcode == Opcode::ResultAt || boundToOf(opcode) != BoundTo::Nothing)
        return false;
    if (!effects.writes.isEmpty() || effects.mayThrow || !effects.alwaysCompletes() || effects.reads.containsAny(bookkeepingHeaps))
        return false;
    if (!proofsHoldEverywhere && (reliesOf(opcode).contains(Proof::SlotFilled) || reliesOf(opcode).contains(Proof::InBounds)))
        return false;

    if (conversionsOfForm.contains(opcode))
        return true;
    if (branchesOf(opcode) != Branches::No)
        return !effects.reads.isEmpty() || (opcode == Opcode::TypeTest && types.at(draft.testedType()).hasCells());
    return true;
}

static BlockID nearestCommonDominator(const BodyIndex& index, BlockID first, BlockID second)
{
    if (!first)
        return second;
    while (!index.dominates(first, second))
        first = index.immediateDominator(first);
    return first;
}

Walker::HoistPoint Walker::hoistPointInFrontOf(uint32_t loopIndex) const
{
    const OpenLoop& loop = m_openLoops[loopIndex];
    return { loop.frame, m_frames[loop.frame].index->loop(loop.loop).head, loop.blockInFront };
}

Walker::HoistPoint Walker::currentHoistPoint() const
{
    if (m_numberOfLoopsThatMayBeLeft < m_openLoops.size())
        return hoistPointInFrontOf(m_numberOfLoopsThatMayBeLeft);
    return { m_frames.size() - 1, m_frames.last().block, m_block };
}

bool Walker::executesOnEveryIteration(uint32_t loopIndex, const HoistPoint& point) const
{
    if (point.block && m_code.at(point.block).frequency != BlockFrequency::Normal)
        return false;
    const OpenLoop& loop = m_openLoops[loopIndex];
    auto blockInFrame = [&](size_t frameIndex) {
        return frameIndex == point.frameIndex ? point.inputBlock : m_frames[frameIndex].block;
    };
    const BodyIndex& index = *m_frames[loop.frame].index;
    if (!loop.backEdgeDominator) {
        BlockID head = index.loop(loop.loop).head;
        for (BlockID member : index.membersOf(loop.loop)) {
            if (!index.isReachable(member))
                continue;
            for (BodyTarget target : index.terminator(member).targets()) {
                if (target.block() == head)
                    loop.backEdgeDominator = nearestCommonDominator(index, loop.backEdgeDominator, member);
            }
        }
        if (!loop.backEdgeDominator)
            loop.backEdgeDominator = head;
    }
    if (!index.dominates(blockInFrame(loop.frame), loop.backEdgeDominator))
        return false;

    for (size_t frameIndex = loop.frame + 1; frameIndex <= point.frameIndex; ++frameIndex) {
        const Frame& frame = m_frames[frameIndex];
        const BodyIndex& callee = *frame.index;
        if (!frame.returnDominator) {
            for (BlockID block : callee.blocksInDominatorOrder()) {
                TerminatorKind kind = callee.terminator(block).kind();
                if (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall)
                    frame.returnDominator = nearestCommonDominator(callee, frame.returnDominator, block);
            }
            if (!frame.returnDominator)
                frame.returnDominator = entryBlock;
        }
        if (!callee.dominates(blockInFrame(frameIndex), frame.returnDominator))
            return false;
    }
    return true;
}

std::optional<uint32_t> Walker::findLoopToBuildInFrontOf(const Draft& draft, const Effects& effects) const
{
    uint32_t numberOfLoops = std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft);
    if (!numberOfLoops || !mayLeaveSomeLoopForLoopInvariants(m_types, draft, effects, m_numberOfScopesWhereProofsHoldEverywhere))
        return std::nullopt;
    if (drawChoice(DrawnFor::PlaceInFrontOfLoop, currentFunction().index(), draft.position) == ChoiceOfCost::No)
        return std::nullopt;
    if ((draft.opcode == Opcode::GetModuleVariable || draft.opcode == Opcode::GetEnvironmentField) && draft.followsItsStore())
        return std::nullopt;
    if (draft.opcode == Opcode::GetModuleVariable && (isVariableFilledAtFirstRead(plan(), draft.variable()) || plan().promotedEnvironments().find(draft.variable())))
        return std::nullopt;
    if (draft.opcode == Opcode::GetEnvironmentField && plan().promotedEnvironments().isPromoted(draft.layout()))
        return std::nullopt;
    if (draft.opcode == Opcode::GetTypedArrayLength && viewMayFollowSharedMemoryThatGrows(*this, draft.viewOperand()))
        return std::nullopt;
    for (Reference operand : draft.operands()) {
        if (operand && (!operand.isRecord() || typeOf(operand) != ownTypeOf(operand)))
            return std::nullopt;
    }

    LocationMasks reads = effects.reads.without(heapsWrittenOnceForLoopInvariants);
    HoistPoint point = currentHoistPoint();
    std::optional<uint32_t> outermost;
    forEachOpenLoopAround(numberOfLoops, 0, [&](uint32_t index) {
        const OpenLoop& loop = m_openLoops[index];
        bool readsWhatTurnsMayWrite = !reads.isEmpty() && (loop.effects.mayRunOtherCode() || reads.overlaps(loop.effects.writes));
        if (!loop.blockInFront || (readsWhatTurnsMayWrite && (reads.containsAny(heapsWhoseWritesNobodySees) || reads.overlaps(loop.placesThatQuietTurnsWrite) || !mayReadInFrontOfQuietTurns(index))))
            return IterationStatus::Done;
        if (effects.mayCollect && (!loop.effects.mayCollect || m_code.at(loop.blockInFront).handler))
            return IterationStatus::Done;
        bool operandsStandInFront = std::ranges::all_of(draft.operands(), [&](Reference operand) {
            return !operand || standsInFrontOfLoop(index, operand);
        });
        if (!operandsStandInFront)
            return IterationStatus::Done;
        if (draft.opcode != Opcode::ResolveCallTarget && !executesOnEveryIteration(index, point))
            return IterationStatus::Done;
        if (readsWhatTurnsMayWrite && loop.watchesTurns) {
            noteReadThatCouldStandInFront(loop, reads);
            return IterationStatus::Done;
        }
        outermost = index;
        if (belongsInFrontOfQuietTurns(index, draft, effects))
            return IterationStatus::Done;
        point = hoistPointInFrontOf(index);
        return IterationStatus::Continue;
    });
    return outermost;
}

std::optional<uint32_t> Walker::innermostOpenLoop() const
{
    if (m_numberOfCopiesBuiltForSize)
        return std::nullopt;
    std::optional<uint32_t> innermost;
    forEachOpenLoopAround(std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft), 0, [&](uint32_t index) {
        innermost = index;
        return IterationStatus::Done;
    });
    return innermost;
}

bool Walker::standsInFrontOfOpenLoop(uint32_t indexOfLoop, Reference reference) const
{
    return reference && standsInFrontOfLoop(indexOfLoop, reference);
}

Reference Walker::buildInFrontOfOpenLoop(uint32_t indexOfLoop, Draft&& draft, const Effects* foundByCaller)
{
    if (isUnreachable() || indexOfLoop >= m_openLoops.size() || indexOfLoop >= m_numberOfLoopsThatMayBeLeft)
        return { };
    BlockID inFront = m_openLoops[indexOfLoop].blockInFront;
    if (!inFront || m_code.at(inFront).hasEnded())
        return { };
    BlockID inside = std::exchange(m_block, inFront);
    SetForScope loopsThatMayBeLeft(m_numberOfLoopsThatMayBeLeft, indexOfLoop);
    Reference built = build(WTF::move(draft), foundByCaller);
    m_block = m_hasFailed ? BlockID() : inside;
    return built;
}

void Walker::buildCheckThatTestHolds(Reference test, ProofSource source, Origin origin)
{
    TypeID holds = TypeArena::fromPrimitives(Primitive::True);
    uint32_t numberOfLoops = std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft);
    for (uint32_t index = 0; index < numberOfLoops && !isUnreachable(); ++index) {
        BlockID inFront = m_openLoops[index].blockInFront;
        if (!inFront || m_code.at(inFront).hasEnded() || !standsInFrontOfLoop(index, test))
            continue;
        SetForScope blockInFront(m_block, inFront);
        SetForScope loopsThatMayBeLeft(m_numberOfLoopsThatMayBeLeft, index);
        buildCheckOfProof(test, holds, TypeTable::boolean, source, origin);
        return;
    }
    buildCheckOfProof(test, holds, TypeTable::boolean, source, origin);
}

std::optional<uint32_t> Walker::outermostLoopToMakeTextInFrontOf(std::span<const Reference> operands, const Effects& ofText) const
{
    if (isUnreachable() || isUnderHandler() || m_numberOfCopiesBuiltForSize)
        return std::nullopt;
    for (Reference operand : operands) {
        if (!operand || !operand.isRecord() || typeOf(operand) != ownTypeOf(operand))
            return std::nullopt;
    }

    std::optional<uint32_t> outermost;
    forEachOpenLoopAround(std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft), 0, [&](uint32_t index) {
        const OpenLoop& loop = m_openLoops[index];
        if (!loop.blockInFront || m_code.at(loop.blockInFront).hasEnded() || m_code.at(loop.blockInFront).handler)
            return IterationStatus::Done;
        if (!ofText.writes.isSubsetOf(loop.effects.writes) || (ofText.mayCollect && !loop.effects.mayCollect))
            return IterationStatus::Done;
        bool operandsStandInFront = std::ranges::all_of(operands, [&](Reference operand) {
            return standsInFrontOfLoop(index, operand);
        });
        if (!operandsStandInFront)
            return IterationStatus::Done;
        outermost = index;
        return IterationStatus::Continue;
    });
    return outermost;
}

uint32_t Walker::numberOfOpenLoopsAround(size_t frameIndex, BlockID inputBlock) const
{
    const BodyIndex& index = *m_frames[frameIndex].index;
    uint32_t number = 0;
    for (uint32_t place = 0; place < m_openLoops.size(); ++place) {
        const OpenLoop& open = m_openLoops[place];
        if (open.frame < frameIndex || (open.frame == frameIndex && index.isInLoop(inputBlock, open.loop)))
            number = place + 1;
    }
    return number;
}

bool Walker::looksAtLoopsAroundHaveSeen(const Effects& effects) const
{
    LocationMasks writes = effects.writes.without(heapsWhoseWritesNobodySees);
    if (writes.isEmpty() && !effects.mayRunOtherCode())
        return true;
    bool haveSeen = true;
    forEachOpenLoopAround(std::min({ static_cast<uint32_t>(m_openLoops.size()), m_numberOfLoopsThatMayBeLeft, m_numberOfLoopsAroundEdgeBeingLed }), 0, [&](uint32_t index) {
        const OpenLoop& loop = m_openLoops[index];
        if (loop.effects.mayRunOtherCode())
            return IterationStatus::Continue;
        if (effects.mayRunOtherCode() || !writes.isSubsetOf(loop.effects.writes)) {
            const LinkTables& linked = facts().linked();
            if (loop.frame + 1 < m_frames.size() && m_frames[loop.frame + 1].function.index() < linked.functions.size()) {
                const LinkedFunction& copied = linked.function(m_frames[loop.frame + 1].function);
                Effects ofCopied = copied.effectsOnReturn();
                ofCopied.merge(copied.effectsOnThrow());
                if (!ofCopied.mayRunOtherCode() && ofCopied.writes.without(heapsWhoseWritesNobodySees).isSubsetOf(loop.effects.writes))
                    return IterationStatus::Continue;
            }
            haveSeen = false;
            return IterationStatus::Done;
        }
        return IterationStatus::Continue;
    });
    return haveSeen;
}

static constexpr size_t mostBuiltInFrontOfLoopWithoutTable = 16;

static uint32_t numberOfOperandInFrontOfLoop(ValueID held) { return held ? held.index() : UINT32_MAX; }
static uint32_t numberOfOperandInFrontOfLoop(Reference taken) { return taken.isRecord() ? taken.id().index() : taken.bits(); }

template<typename Record>
static unsigned hashOfRecordInFrontOfLoop(const Record& record)
{
    unsigned hash = static_cast<unsigned>(record.opcode) * 0x9E3779B1u + static_cast<unsigned>(record.representation) * 0x85EBCA6Bu + static_cast<unsigned>(record.flags);
    for (uint32_t word : record.payload())
        hash = (hash ^ word) * 0x85EBCA6Bu + (hash >> 15);
    for (auto operand : record.operands())
        hash = (hash ^ numberOfOperandInFrontOfLoop(operand)) * 0x85EBCA6Bu + (hash >> 15);
    return hash ^ hash >> 16;
}

static bool isSameAsRecordInFrontOfLoop(const Code& code, ValueID id, const Draft& draft)
{
    const Operation& record = code.at(id);
    if (record.opcode != draft.opcode || record.representation != draft.representation || record.flags != draft.flags || !std::ranges::equal(record.payload(), draft.payload()))
        return false;
    return std::ranges::equal(record.operands(), draft.operands(), [](ValueID held, Reference taken) {
        return taken ? held == taken.id() : !held;
    });
}

ValueID Walker::findSameInFrontOfPlainWalk(const Draft& draft) const
{
    for (ValueID id : m_frontsSharedWithTypedBuilds.last().builtInFront) {
        if (isSameAsRecordInFrontOfLoop(m_code, id, draft))
            return id;
    }
    return { };
}

ValueID Walker::findSameInFrontOfLoop(uint32_t indexOfLoop, const Draft& draft) const
{
    auto isSame = [&](ValueID id) {
        return isSameAsRecordInFrontOfLoop(m_code, id, draft);
    };
    const OpenLoop& loop = m_openLoops[indexOfLoop];
    if (loop.tableOfBuiltInFront.isEmpty()) {
        for (ValueID id : loop.builtInFront) {
            if (isSame(id))
                return id;
        }
        return { };
    }
    size_t mask = loop.tableOfBuiltInFront.size() - 1;
    for (size_t place = hashOfRecordInFrontOfLoop(draft) & mask; loop.tableOfBuiltInFront[place]; place = (place + 1) & mask) {
        if (isSame(loop.tableOfBuiltInFront[place]))
            return loop.tableOfBuiltInFront[place];
    }
    return { };
}

struct ConversionSoughtInFrontOfLoop {
    std::span<const uint32_t> payload() const { return { }; }
    std::span<const Reference> operands() const { return { &operand, 1 }; }

    Opcode opcode;
    Representation representation;
    uint8_t flags { 0 };
    Reference operand;
};

Reference Walker::appendConversionAmongOpenLoops(Opcode opcode, Representation to, Reference operand, TypeID proven, Origin origin)
{
    static constexpr OpcodeSet conversionsThatMayLeaveLoops { Opcode::Box, Opcode::Unbox, Opcode::Compress };
    static_assert([]() consteval {
        bool mayLeave = true;
        conversionsThatMayLeaveLoops.forEach([&](Opcode conversion) {
            mayLeave &= conversionsOfForm.contains(conversion) && resultRuleOf(conversion) != ResultRule::None && resultRuleOf(conversion) != ResultRule::Allocation && interpretedOf(conversion) != Interpretation::Allocation
                && boundToOf(conversion) == BoundTo::Nothing && !reliesOf(conversion).contains(Proof::SlotFilled) && !reliesOf(conversion).contains(Proof::InBounds);
        });
        return mayLeave;
    }());
    RELEASE_ASSERT(conversionsThatMayLeaveLoops.contains(opcode) && operand.isRecord());

    BlockID block = m_block;
    uint32_t numberOfLoopsAround = std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft);
    if (numberOfLoopsAround && proven == ownTypeOf(operand)) {
        HoistPoint point = currentHoistPoint();
        forEachOpenLoopAround(numberOfLoopsAround, 0, [&](uint32_t index) {
            const OpenLoop& loop = m_openLoops[index];
            if (!loop.blockInFront || !standsInFrontOfLoop(index, operand) || !executesOnEveryIteration(index, point))
                return IterationStatus::Done;
            point = hoistPointInFrontOf(index);
            block = loop.blockInFront;
            numberOfLoopsAround = index;
            if (operand.isValue() && loop.blocksOfFrontOfQuietTurns.contains(m_code.blockOf(operand.id()))) {
                block = loop.blocksOfFrontOfQuietTurns.last();
                return IterationStatus::Done;
            }
            return IterationStatus::Continue;
        });
    }
    if (numberOfLoopsAround < m_openLoops.size() && m_openLoops[numberOfLoopsAround].frontOfQuietTurns) {
        const OpenLoop& loop = m_openLoops[numberOfLoopsAround];
        if (block == loop.blockInFront || block == loop.blocksOfFrontOfQuietTurns.last())
            block = operand.isValue() && loop.blocksOfFrontOfQuietTurns.contains(m_code.blockOf(operand.id())) ? loop.blocksOfFrontOfQuietTurns.last() : loop.blockInFront;
    }
    if (numberOfLoopsAround >= m_openLoops.size())
        return Reference::value(m_code.appendConversion(block, opcode, operand.id(), to, proven, origin));
    if (m_code.at(block).hasEnded())
        return { };

    const OpenLoop& loop = m_openLoops[numberOfLoopsAround];
    auto isSame = [&](ValueID id) {
        const Operation& record = m_code.at(id);
        return record.opcode == opcode && record.representation == to && record.operand(0) == operand.id();
    };
    if (loop.tableOfBuiltInFront.isEmpty()) {
        for (ValueID id : loop.builtInFront) {
            if (isSame(id))
                return Reference::value(id);
        }
    } else {
        size_t mask = loop.tableOfBuiltInFront.size() - 1;
        for (size_t place = hashOfRecordInFrontOfLoop(ConversionSoughtInFrontOfLoop { opcode, to, 0, operand }) & mask; loop.tableOfBuiltInFront[place]; place = (place + 1) & mask) {
            if (isSame(loop.tableOfBuiltInFront[place]))
                return Reference::value(loop.tableOfBuiltInFront[place]);
        }
    }
    ValueID id = m_code.appendConversion(block, opcode, operand.id(), to, proven, origin);
    noteBuiltInFrontOfLoop(numberOfLoopsAround, id);
    return Reference::value(id);
}

void Walker::noteBuiltInFrontOfLoop(uint32_t indexOfLoop, ValueID built)
{
    OpenLoop& loop = m_openLoops.changeable(indexOfLoop);
    loop.builtInFront.append(built);
    if (loop.builtInFront.size() <= mostBuiltInFrontOfLoopWithoutTable)
        return;
    auto add = [&](ValueID id) {
        size_t mask = loop.tableOfBuiltInFront.size() - 1;
        size_t place = hashOfRecordInFrontOfLoop(m_code.at(id)) & mask;
        while (loop.tableOfBuiltInFront[place])
            place = (place + 1) & mask;
        loop.tableOfBuiltInFront[place] = id;
    };
    if (loop.builtInFront.size() * 2 <= loop.tableOfBuiltInFront.size())
        return add(built);
    loop.tableOfBuiltInFront.fill(ValueID(), std::max<size_t>(mostBuiltInFrontOfLoopWithoutTable * 4, loop.tableOfBuiltInFront.size() * 2));
    for (ValueID id : loop.builtInFront)
        add(id);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
