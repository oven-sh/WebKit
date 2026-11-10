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
#include "AOTOperandKinds.h"
#include "AOTPlan.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkStrings.h"
#include <algorithm>
#include <optional>
#include <wtf/IterationStatus.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsWrittenOnceForLoopInvariants { AbstractHeap::ShapeWords };

static constexpr OpcodeSet answeredByTextAloneForLoopInvariants { Opcode::StringEqualsLiteral, Opcode::ValueEqualsLiteral };

static constexpr OpcodeSet readsKeptByStateInLoopInvariants { Opcode::GetModuleVariable, Opcode::GetEnvironmentField };

static bool findsSameInEveryTurnForLoopInvariants(const Program& program, const BodyValue& operation, const Effects& effectsOfLoop)
{
    Opcode opcode = operation.opcode();
    if (!readsKeptByStateInLoopInvariants.contains(opcode))
        return false;
    Effects effects = declaredEffectsOfOperation(program, opcode, operation);
    return effects.isUnobservable() && !effects.reads.overlaps(effectsOfLoop.writes);
}

static bool readsFieldOfScopeThatTurnsStoreForLoopInvariants(const BodyValue& operation, const LoopPreview& preview)
{
    return operation.opcode() == Opcode::GetEnvironmentField && std::ranges::binary_search(preview.fieldsOfScopesThatTurnsStore, FieldReference { operation.layout(), operation.field() });
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
        if (takesOnlyWhatStandsInFront && !readsFieldOfScopeThatTurnsStoreForLoopInvariants(operation, preview) && findsSameInEveryTurnForLoopInvariants(walker.program(), operation, preview.effects)) {
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

std::optional<Effects> effectsOfTypedFormAhead(Walker& walker, const BodyValue& operation, const TypesOfInputs& types, FunctionID* halfRunBehindLooks, bool sitesMayAnswer)
{
    Opcode opcode = operation.opcode();
    bool isByKey = accessesByKeyForLoopInvariants.contains(opcode);
    if (!walker.has(Mechanism::TypedOperations) || !(isByKey || readsByNameForLoopInvariants.contains(opcode) || storesByNameForLoopInvariants.contains(opcode)))
        return std::nullopt;

    walker.takeFormAskedAhead();
    TypedForm typedForm = typedFormOf(walker, operation, types);
    if (typedForm == TypedForm::None && sitesMayAnswer && walker.looksTakeFormsBehindLooksAsBuilt() && siteMayAnswerReadWithoutOtherCode(walker, operation))
        return Effects::ofReadByNameThatRunsNoCode();
    bool isBuiltBehindLooks = !isByKey && walker.looksTakeFormsBehindLooksAsBuilt() && typedForm != TypedForm::None && typedForm != TypedForm::BuiltAsCall && typedForm != TypedForm::BuiltOnObjectAsMade;
    bool isFoundAbovePrimitive = typedForm == TypedForm::BuiltOnObjectAsMade && walker.looksTakeFormsBehindLooksAsBuilt();
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
    TypeID typeOfReceiver = opcode == Opcode::PropGet || opcode == Opcode::PropSet ? types(operation.objectOperand()) : TypeID();
    if (LayoutID heir = typeOfReceiver ? layoutKnownByStructureOfClass(walker, walker.types().withoutNullish(typeOfReceiver)) : LayoutID(); heir && findField(walker.program(), heir, operation.key())) {
        bool readsField = opcode == Opcode::PropGet && typeFoundBehindLooks(walker, operation, walker.types().withoutNullish(typeOfReceiver));
        bool storesIntoField = opcode == Opcode::PropSet && typedForm == TypedForm::BuiltBehindLookAtCell;
        if (storesIntoField || readsField) {
            Opcode access = storesIntoField ? Opcode::PutPropertyOfHeir : Opcode::GetPropertyOfHeir;
            Effects ofAccess = declaredBoundOf(access);
            nameFieldInEffectsOfAccessToHeir(walker.program(), access, operation.key(), ofAccess);
            Effects effects = declaredBoundOf(Opcode::HasStructureOfClass);
            effects.merge(ofAccess);
            return effects;
        }
    }
    if (readsByNameForLoopInvariants.contains(opcode)) {
        Effects effects = Effects::ofReadByNameThatRunsNoCode();
        if (TypeID object = types(operation.objectOperand()); object && walker.types().at(walker.types().withoutNullish(object)).hasOnlyLayouts())
            effects.writes.remove(AbstractHeapSet { AbstractHeap::MemberGuards });
        if (form.opcode == Opcode::GetDeferredClosureSlotOrUndefined) {
            for (Opcode read : { Opcode::GetDeferredClosureSlot, Opcode::GetDeferredClosureSlotOrUndefined }) {
                Draft built(read);
                built.setKey(operation.key());
                effects.merge(declaredEffectsOfOperation(walker.program(), read, built));
            }
        }
        if (form.opcode == Opcode::GetWatchedBuiltinValue && !(sitesMayAnswer && walker.looksTakeFormsBehindLooksAsBuilt()))
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
    if (loop.preheaderBlock) {
        while (m_preheaderDepths.size() <= loop.preheaderBlock.index())
            m_preheaderDepths.append(0);
        RELEASE_ASSERT(!m_preheaderDepths[loop.preheaderBlock.index()]);
        setPreheaderDepth(loop.preheaderBlock, place);
    }
    m_openLoops.append(WTF::move(loop));
}

void Walker::layChecksOfParametersOfHead(BlockID head, uint32_t numberOfParametersOfInput)
{
    if (m_hasFailed || isUnreachable() || m_openLoops.isEmpty() || m_openLoops.last().head != head)
        return;
    TypeID anyWord = m_types.withPrimitives(TypeTable::anything, Primitive::Absent);
    SetForScope buildInPlaceScope(m_isBuildingInPlace, true);
    Origin origin = m_origin;
    origin.isTailCall = false;
    Vector<ValueID, 8> parameters(m_code.at(head).parameters().first(numberOfParametersOfInput));
    for (ValueID parameter : parameters) {
        Representation form = m_code.at(parameter).representation;
        if ((form != Representation::JSValue && form != Representation::Cell && form != Representation::Cell32) || !m_types.at(m_code.at(parameter).type).isJavaScriptValue())
            continue;
        Draft check(Opcode::CheckProof, origin);
        check.setValueOperand(Reference::value(parameter));
        check.setCheckedType(anyWord);
        check.setTypeWithoutProof(anyWord);
        check.setSource(ProofSource::OtherOperation);
        ValueID laid(m_code.numberOfValues());
        build(WTF::move(check));
        if (m_hasFailed || isUnreachable())
            return;
        if (m_code.numberOfValues() == laid.index() + 1 && m_code.at(laid).opcode == Opcode::CheckProof)
            m_code.leaveCheckOfProofToTestBehind(laid);
    }
}

void Walker::askParametersOfHeadsForWhatIsShown()
{
    for (uint32_t index = 0; index < m_code.numberOfValues() && !m_hasFailed; ++index) {
        const Operation& check = m_code.at(ValueID(index));
        if (check.opcode != Opcode::CheckProof || check.source() != ProofSource::OtherOperation || check.checkedType() != check.typeWithoutProof() || !check.isLeftToTestBehind())
            continue;
        ValueID parameter = check.valueOperand();
        BlockID head = m_code.at(parameter).block();
        if (m_code.at(parameter).opcode != Opcode::Parameter || head != check.block() || !m_headsOfClosedLoops.contains(head.index()))
            continue;
        TypeID shown = typeShown(Reference::value(parameter));
        if (!shown || !m_types.at(shown).isJavaScriptValue())
            continue;
        TypeID checked = typeThatUnitMayTestFor(m_types.testablePartOf(shown), true);
        if (!m_types.isSubset(TypeTable::anything, checked))
            m_code.setCheckedTypeOfProof(ValueID(index), checked);
    }
}

void Walker::closeNewestLoop()
{
    const OpenLoop& loop = m_openLoops.last();
    if (!m_hasFailed && !m_versionedLoops.isEmpty() && m_versionedLoops.last().headThatAsksLesserBound == loop.head) {
        const VersionedLoop& versionedLoop = m_versionedLoops.last();
        LocationMasks restsOn;
        if (versionedLoop.viewWhoseLengthIsBound)
            restsOn.add(AbstractHeapSet { AbstractHeap::TypedArrayStorage });
        if (versionedLoop.variableThatIsBound && !numberReadOutOfVariableStays(*this, versionedLoop.variableThatIsBound))
            restsOn.add(program(), MemoryLocation::moduleVariable(versionedLoop.variableThatIsBound));
        if (!restsOn.isEmpty() && (loop.effects.mayRunOtherCode() || restsOn.overlaps(loop.effects.writes)))
            fail("a hoisted loop bound is used where the iterations may write what it was read from"_s, m_inputs.options.verifiesCode || shouldAssumeInvariantLoopBounds() ? InternalErrorKind::StaleHoistedLoopBound : InternalErrorKind::Unknown);
    }
    settleFateOfPoll(loop);
    m_headsOfClosedLoops.add(loop.head.index());
    if (loop.firstReloadBlock)
        m_headsOfClosedLoops.add(loop.firstReloadBlock.index());
    if (loop.frame < m_frames.size() && !m_frames[loop.frame].placesOfOpenLoops.isEmpty()) {
        if (m_serialOfNewestMark)
            m_placesOfOpenLoopsInTries.append(PlaceOfOpenLoopInTry { static_cast<uint32_t>(loop.frame), loop.loop, m_frames[loop.frame].placesOfOpenLoops[loop.loop.index()] });
        m_frames[loop.frame].placesOfOpenLoops[loop.loop.index()] = 0;
    }
    if (loop.preheaderBlock)
        setPreheaderDepth(loop.preheaderBlock, 0);
    for (BlockID earlier : loop.earlierPreheaderBlocks)
        setPreheaderDepth(earlier, 0);
    for (BlockID block : loop.reloadBlocks)
        setPreheaderDepth(block, 0);
    forgetWhatWasBelievedAt(loop.head);
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
    if ((loop.carriedResultCount = std::exchange(m_pendingCarriedResultCount, 0)))
        loop.carriedResultsSummary = m_trialVisits.last().summaryIndex;
    if (!has(Mechanism::LoopInvariants)) {
        openLoop(WTF::move(loop));
        crossEdge({ }, head, entering);
        return;
    }

    auto readyToCross = std::make_shared<ArgumentsOfEdge>();
    if (!makeReadyToCross(head, entering, true, *readyToCross))
        return;
    loop.entering = WTF::move(readyToCross);
    layFrontOfQuietTurns(loop, inputHead);
    if (m_hasFailed)
        return;
    loop.preheaderBlock = m_block;
    BlockID front = loop.firstReloadBlock;
    openLoop(WTF::move(loop));
    if (front) {
        noteBlockOfFrontOfQuietTurns(m_openLoops.size() - 1, front);
        letFrontOfQuietTurnsFailUnasked(m_openLoops.size() - 1);
    }
    m_block = { };
    if (!m_frontsSharedWithTypedBuilds.isEmpty() && m_frontsSharedWithTypedBuilds.last().frameIndex == m_frames.size() - 1 && m_frontsSharedWithTypedBuilds.last().inputHead == inputHead) {
        const ValuesBuiltInFront& shared = m_frontsSharedWithTypedBuilds.last().builtInFront;
        for (uint32_t place = 0; place < shared.size(); ++place)
            noteBuiltInFrontOfLoop(static_cast<uint32_t>(m_openLoops.size() - 1), ValueID::fromBits(shared[place]));
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
        if (loop.preheaderBlock && !m_code.at(loop.preheaderBlock).hasEnded()) {
            m_code.end(loop.preheaderBlock, TerminatorDraft(TerminatorKind::Jump, loop.origin), 1);
            m_code.closeEdge(m_code.at(loop.preheaderBlock).terminator.next(), entered, *loop.entering);
        }
        closeNewestLoop();
    }
}

void Walker::endBlocksInFrontOfLoopsFrom(size_t numberOfLoopsKept)
{
    while (m_openLoops.size() > numberOfLoopsKept) {
        const OpenLoop& loop = m_openLoops.last();
        BlockID entered = closeFrontOfQuietTurns(loop);
        if (loop.preheaderBlock && !m_code.at(loop.preheaderBlock).hasEnded()) {
            m_code.end(loop.preheaderBlock, TerminatorDraft(TerminatorKind::Jump, loop.origin), 1);
            m_code.closeEdge(m_code.at(loop.preheaderBlock).terminator.next(), entered, *loop.entering);
        }
        closeNewestLoop();
    }
}

bool Walker::isInvariantInLoop(uint32_t indexOfLoop, Reference reference) const
{
    if (!reference.isValue())
        return reference.isConstant();
    if (reference.id().index() < m_openLoops[indexOfLoop].firstRecord)
        return true;
    BlockID block = m_code.blockOf(reference.id());
    uint32_t place = block && block.index() < m_preheaderDepths.size() ? m_preheaderDepths[block.index()] : 0;
    if (!place || place - 1 > indexOfLoop)
        return false;
    const Operation& operation = m_code.at(reference.id());
    return place - 1 < indexOfLoop || (operation.opcode != Opcode::Strcat && (operation.opcode != Opcode::FlattenString || !operation.isReadOperand()));
}

static bool mayLeaveSomeLoopForLoopInvariants(const TypeArena& types, const Draft& draft, const Effects& effects, bool proofsHoldEverywhere)
{
    Opcode opcode = draft.opcode;
    if (resultRuleOf(opcode) == ResultRule::None || resultRuleOf(opcode) == ResultRule::Allocation || interpretedOf(opcode) == Interpretation::Allocation)
        return false;
    if (opcode == Opcode::IsElementPresent || opcode == Opcode::HasStructureOfClass)
        return false;
    if (opcode == Opcode::Narrowed || opcode == Opcode::ResultAt || boundToOf(opcode) != BoundTo::Nothing)
        return false;
    if (!effects.writes.isEmpty() || effects.mayThrow || !effects.alwaysCompletes() || effects.reads.containsAny(bookkeepingHeaps))
        return false;
    if (!proofsHoldEverywhere && (reliesOf(opcode).contains(Proof::SlotFilled) || reliesOf(opcode).contains(Proof::InBounds)))
        return false;

    if (conversionsOfForm.contains(opcode))
        return true;
    if (branchesOf(opcode) != Branches::No)
        return !effects.reads.isEmpty() || (opcode == Opcode::TypeTest && types.at(draft.testedType()).hasCells()) || (opcode == Opcode::ToBoolean && draft.form() == OperatorForm::Generic);
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
    return { loop.frame, m_frames[loop.frame].index->loop(loop.loop).head, loop.preheaderBlock };
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

bool Walker::isGuaranteedToExecute(uint32_t loopIndex, const HoistPoint& point) const
{
    const OpenLoop& loop = m_openLoops[loopIndex];
    if (loop.hasImplicitExit || !executesOnEveryIteration(loopIndex, point))
        return false;
    auto blockInFrame = [&](size_t frameIndex) {
        return frameIndex == point.frameIndex ? point.inputBlock : m_frames[frameIndex].block;
    };
    const BodyIndex& index = *m_frames[loop.frame].index;
    if (!loop.hasFoundExitDominator) {
        loop.hasFoundExitDominator = true;
        BlockID head = index.loop(loop.loop).head;
        for (BlockID member : index.membersOf(loop.loop)) {
            if (member == head || !index.isReachable(member))
                continue;
            bool exits = false;
            index.block(member).forEachOutgoingEdge([&](BodyTarget target, bool) {
                exits |= !index.isInLoop(target.block(), loop.loop);
            });
            if (exits)
                loop.exitDominator = nearestCommonDominator(index, loop.exitDominator, member);
        }
    }
    if (loop.exitDominator && !index.dominates(blockInFrame(loop.frame), loop.exitDominator))
        return false;

    for (size_t frameIndex = loop.frame + 1; frameIndex <= point.frameIndex; ++frameIndex) {
        const Frame& frame = m_frames[frameIndex];
        const BodyIndex& callee = *frame.index;
        if (!frame.exitDominator) {
            for (BlockID block : callee.blocksInDominatorOrder()) {
                TerminatorKind kind = callee.terminator(block).kind();
                if (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall || kind == TerminatorKind::Throw || kind == TerminatorKind::Rethrow)
                    frame.exitDominator = nearestCommonDominator(callee, frame.exitDominator, block);
            }
            if (!frame.exitDominator)
                frame.exitDominator = entryBlock;
        }
        if (!callee.dominates(blockInFrame(frameIndex), frame.exitDominator))
            return false;
    }
    return true;
}

bool Walker::headerMayExitBefore(uint32_t loopIndex, const HoistPoint& point) const
{
    const OpenLoop& loop = m_openLoops[loopIndex];
    const BodyIndex& index = *m_frames[loop.frame].index;
    BlockID head = index.loop(loop.loop).head;
    if ((loop.frame == point.frameIndex ? point.inputBlock : m_frames[loop.frame].block) == head)
        return false;
    bool exits = false;
    index.block(head).forEachOutgoingEdge([&](BodyTarget target, bool) {
        exits |= !index.isInLoop(target.block(), loop.loop);
    });
    return exits;
}

std::optional<uint32_t> Walker::findLoopToBuildInFrontOf(const Draft& draft, const Effects& effects, TypeID proven) const
{
    if (draft.opcode == Opcode::GetValueAtSiteOrAbsent && (effects.reads.containsAny(AbstractHeapSet { AbstractHeap::RuntimeState }) || effects.writes.containsAny(AbstractHeapSet { AbstractHeap::RuntimeState }))) {
        Effects besideItsSite = effects;
        besideItsSite.reads.remove(AbstractHeapSet { AbstractHeap::RuntimeState });
        besideItsSite.writes.remove(AbstractHeapSet { AbstractHeap::RuntimeState });
        return findLoopToBuildInFrontOf(draft, besideItsSite, proven);
    }
    if (draft.opcode == Opcode::GetBuiltinValue && effects.mayThrow && !effects.mayRunOtherCode() && !effects.mayBeTerminated) {
        if (isUnderHandler())
            return std::nullopt;
        Effects behindFirstRead = Effects::none();
        behindFirstRead.reads.add(observableHeaps);
        behindFirstRead.mayCollect = effects.mayCollect;
        return findLoopToBuildInFrontOf(draft, behindFirstRead, proven);
    }
    if (m_isBuildingInPlace)
        return std::nullopt;
    uint32_t numberOfLoops = std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft);
    if (!numberOfLoops || !mayLeaveSomeLoopForLoopInvariants(m_types, draft, effects, m_numberOfScopesWhereProofsHoldEverywhere))
        return std::nullopt;
    if (drawChoice(DrawnFor::PlaceInFrontOfLoop, currentFunction().index(), draft.position, false) == ChoiceOfCost::No)
        return std::nullopt;
    if ((draft.opcode == Opcode::GetModuleVariable || draft.opcode == Opcode::GetEnvironmentField) && draft.isKnownInitialized())
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
    if (answeredByTextAloneForLoopInvariants.contains(draft.opcode))
        reads = reads.without(AbstractHeapSet { AbstractHeap::StringStorage });
    if (shouldAssumeInvariantArrayStorage())
        reads = reads.without(AbstractHeapSet { AbstractHeap::ArrayStorage, AbstractHeap::ArrayLengths, AbstractHeap::TypedArrayStorage });
    HoistPoint point = currentHoistPoint();
    std::optional<uint32_t> outermost;
    forEachOpenLoopAround(numberOfLoops, 0, [&](uint32_t index) {
        const OpenLoop& loop = m_openLoops[index];
        bool readsWhatTurnsMayWrite = !reads.isEmpty() && (loop.effects.mayRunOtherCode() || reads.overlaps(loop.effects.writes));
        if (!loop.preheaderBlock || m_code.at(loop.preheaderBlock).hasEnded() || (readsWhatTurnsMayWrite && (reads.containsAny(unobservableWriteHeaps) || reads.overlaps(loop.placesThatQuietTurnsWrite) || !mayReadInFrontOfQuietTurns(index))))
            return IterationStatus::Done;
        if (effects.mayCollect && (!loop.effects.mayCollect || m_code.at(loop.preheaderBlock).handler))
            return IterationStatus::Done;
        bool operandsStandInFront = std::ranges::all_of(draft.operands(), [&](Reference operand) {
            return !operand || isInvariantInLoop(index, operand);
        });
        if (!operandsStandInFront)
            return IterationStatus::Done;
        static constexpr OpcodeSet neverFailOfList { Opcode::GetArrayStorage, Opcode::GetArrayLength, Opcode::GetArrayCapacity, Opcode::HasArrayStorage };
        static constexpr OpcodeSet neverFailWhereTheyMayLeaveLoops { Opcode::GetSlot, Opcode::GetModuleVariable };
        bool findsList = neverFailWhereTheyMayLeaveLoops.contains(draft.opcode) && proven && (hasKind(m_types, proven, OperandKind::Array) || isArrayOrInitialPrimitive(draft, proven));
        static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
        static constexpr OpcodeSet writtenNumbers { Opcode::Integer, Opcode::Double };
        auto isKeyOfReadAlone = [&] {
            const BodyIndex& text = currentBodyIndex();
            ValueID user = isWrittenInTopFrame(draft) && draft.inputValueGiven ? text.onlyUsingValue(draft.inputValueGiven) : ValueID();
            return user && text.opcodeOf(user) == Opcode::ElemGet && text.value(user).propertyOperand() == draft.inputValueGiven;
        };
        bool isSumWithWrittenNumber = sums.contains(draft.opcode) && effects.reads.isEmpty() && !effects.mayCollect && isKeyOfReadAlone() && std::ranges::any_of(draft.operands(), [&](Reference operand) {
            return operand && (operand.isConstant() || (operand.isValue() && writtenNumbers.contains(m_code.at(operand.id()).opcode)));
        });
        bool mayStandWhereNotEveryTurnAsks = draft.opcode == Opcode::ResolveCallTarget || ((findsList || isSumWithWrittenNumber || neverFailOfList.contains(draft.opcode)) && m_code.at(m_block).frequency != BlockFrequency::Rare);
        if (!mayStandWhereNotEveryTurnAsks && !executesOnEveryIteration(index, point))
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

bool Walker::comparisonWithTextLeavesLoop(const Draft& comparison) const
{
    return !m_openLoops.isEmpty() && findLoopToBuildInFrontOf(comparison, declaredBoundInTable(comparison.opcode), TypeTable::boolean);
}

std::optional<uint32_t> Walker::findLoopThatReaderVouchesFor(const Draft& draft, uint32_t outermostLoopThatMayBeLeft) const
{
    VariableID variable = draft.variable();
    if (!facts().linked().variables[variable.index()].isFilledAtEveryRead || program().at(variable).module != program().at(currentFunction()).module)
        return std::nullopt;
    std::optional<uint32_t> outermost;
    forEachOpenLoopAround(std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft), m_frames.size() - 1, [&](uint32_t index) {
        if (index < outermostLoopThatMayBeLeft)
            return IterationStatus::Done;
        outermost = index;
        return IterationStatus::Continue;
    });
    return outermost;
}

std::optional<uint32_t> Walker::innermostOpenLoop() const
{
    if (m_coldCopyDepth)
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
    return reference && isInvariantInLoop(indexOfLoop, reference);
}

Reference Walker::buildInFrontOfOpenLoop(uint32_t indexOfLoop, Draft&& draft, const Effects* foundByCaller)
{
    if (isUnreachable() || indexOfLoop >= m_openLoops.size() || indexOfLoop >= m_numberOfLoopsThatMayBeLeft)
        return { };
    BlockID inFront = m_openLoops[indexOfLoop].preheaderBlock;
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
        BlockID inFront = m_openLoops[index].preheaderBlock;
        if (!inFront || m_code.at(inFront).hasEnded() || !isInvariantInLoop(index, test))
            continue;
        SetForScope preheaderBlock(m_block, inFront);
        SetForScope loopsThatMayBeLeft(m_numberOfLoopsThatMayBeLeft, index);
        buildCheckOfProof(test, holds, TypeTable::boolean, source, origin);
        return;
    }
    buildCheckOfProof(test, holds, TypeTable::boolean, source, origin);
}

std::optional<Walker::LoopEntryCondition> Walker::findLoopEntryCondition(uint32_t indexOfLoop) const
{
    if (isUnreachable() || indexOfLoop >= m_openLoops.size() || indexOfLoop >= m_numberOfLoopsThatMayBeLeft)
        return std::nullopt;
    const OpenLoop& open = m_openLoops[indexOfLoop];
    BlockID inFront = open.preheaderBlock;
    if (!inFront || m_code.at(inFront).hasEnded() || open.frame >= m_frames.size())
        return std::nullopt;
    const Frame& frame = m_frames[open.frame];
    const BodyIndex& index = *frame.index;
    BodyBlock head = index.block(index.loop(open.loop).head);
    BodyTerminator end = head.terminator();
    if (end.kind() != TerminatorKind::Branch)
        return std::nullopt;
    ValueID asked = end.as<TerminatorKind::Branch>().conditionOperand();
    if (!asked || index.isParameter(asked) || !head.holds(asked) || !allRelations.contains(index.opcodeOf(asked)))
        return std::nullopt;
    BodyTargets::Iterator target = end.targets().begin();
    BlockID behindYes = (*target).block();
    if (!index.isInLoop(behindYes, open.loop) || index.isInLoop((*++target).block(), open.loop))
        return std::nullopt;
    if (behindYes == head.id() || !index.dominates(behindYes, frame.block))
        return std::nullopt;

    std::span<const ValueID> entering = open.recordsEntering();
    auto asItEnters = [&](ValueID written) -> Reference {
        ValueID value = index.stripCasts(written);
        Reference reference;
        if (index.isParameter(value) && index.blockOf(value) == head.id()) {
            uint32_t position = value.index() - head.firstParameter().index();
            if (position < entering.size() && entering[position])
                reference = m_code.isConstant(entering[position]) ? Reference::constant(entering[position]) : Reference::value(entering[position]);
        } else
            reference = referenceBoundIn(frame, written);
        bool isNumberInFront = reference && reference.isRecord() && isInvariantInLoop(indexOfLoop, reference) && typeOf(reference) == ownTypeOf(reference) && m_types.at(ownTypeOf(reference)).isNumber();
        return isNumberInFront ? reference : Reference();
    };
    BodyValue relation = index.value(asked);
    Reference left = asItEnters(relation.operand(0));
    Reference right = asItEnters(relation.operand(1));
    if (!left || !right) {
        Reference headerTest = referenceBoundIn(frame, asked);
        if (!headerTest || !headerTest.isValue() || m_code.blockOf(headerTest.id()) != open.head || m_code.at(headerTest.id()).opcode != index.opcodeOf(asked))
            return std::nullopt;
        std::span<const ValueID> parameters = m_code.at(open.head).parameters();
        auto valueOnEntry = [&](ValueID operand) -> Reference {
            if (auto parameter = std::ranges::find(parameters, operand); parameter != parameters.end()) {
                size_t position = parameter - parameters.begin();
                operand = position < entering.size() ? entering[position] : ValueID();
            }
            Reference reference = !operand ? Reference() : m_code.isConstant(operand) ? Reference::constant(operand) : Reference::value(operand);
            bool isInvariantNumber = reference && isInvariantInLoop(indexOfLoop, reference) && typeOf(reference) == ownTypeOf(reference) && m_types.at(ownTypeOf(reference)).isNumber();
            return isInvariantNumber ? reference : Reference();
        };
        left = valueOnEntry(m_code.at(headerTest.id()).operand(0));
        right = valueOnEntry(m_code.at(headerTest.id()).operand(1));
        if (!left || !right)
            return std::nullopt;
    }
    return LoopEntryCondition { asked, left, right };
}

Reference Walker::buildLoopEntryCondition(uint32_t indexOfLoop)
{
    std::optional<LoopEntryCondition> asked = findLoopEntryCondition(indexOfLoop);
    if (!asked)
        return { };
    const OpenLoop& open = m_openLoops[indexOfLoop];
    Origin origin = open.origin;
    Opcode opcode = m_frames[open.frame].index->opcodeOf(asked->written);
    BlockID inside = std::exchange(m_block, open.preheaderBlock);
    SetForScope loopsThatMayBeLeft(m_numberOfLoopsThatMayBeLeft, indexOfLoop);
    Reference begins = buildOperatorForStrings(*this, opcode, asked->left, asked->right, origin);
    m_block = m_hasFailed ? BlockID() : inside;
    return begins && begins.isValue() && representationOf(begins) == Representation::Boolean ? begins : Reference();
}

void Walker::beginHoistingConstantDiamond(const BodyTerminator& branch, Reference condition)
{
    const Frame& frame = m_frames.last();
    const BodyIndex& index = *frame.index;
    BlockID join;
    uint32_t numberOfBlock = branch.block().index();
    for (BodyTarget target : branch.targets()) {
        if (target.block().index() != ++numberOfBlock)
            return;
        BodyBlock arm = index.block(target.block());
        if (index.numberOfIncomingEdges(arm.id()) != 1 || arm.hasHandler() || arm.terminator().kind() != TerminatorKind::Jump)
            return;
        for (BodyValue made : arm.values()) {
            if (!primitiveConstants.contains(made.opcode()))
                return;
        }
        BodyTarget met = arm.terminator().next();
        if ((join && join != met.block()) || !met.proofs().empty())
            return;
        join = met.block();
    }
    if (!join || join.index() != numberOfBlock + 1 || index.numberOfIncomingEdges(join) != 2 || index.isLoopHead(join) || index.isEnteredByHandlerEdge(join) || index.block(join).hasHandler())
        return;

    std::optional<uint32_t> place = innermostOpenLoop();
    auto isLoopInvariant = [&](Reference reference) {
        return reference && reference.isRecord() && isInvariantInLoop(*place, reference) && typeOf(reference) == ownTypeOf(reference);
    };
    if (!place || !condition.isValue() || !isLoopInvariant(condition))
        return;
    if (m_hoistedDiamond || m_isBuildingInPlace || !has(Mechanism::LoopInvariants) || isUnderHandler() || hasStringsInBuilders() || !m_generatorsWalkedInConsumers.isEmpty())
        return;
    if (frame.inlinedHeadBlocks || !frame.blocksBeingTailDuplicated.isEmpty())
        return;
    BlockID front = m_openLoops[*place].preheaderBlock;
    if (!front || m_code.at(front).hasEnded() || m_code.at(front).handler || m_openLoops[*place].firstReloadBlock || !executesOnEveryIteration(*place, currentHoistPoint()))
        return;
    static constexpr OpcodeSet lookThroughOpcodes { Opcode::ToBoolean, Opcode::LogicalNot, Opcode::Convert };
    ValueID asked = branch.operand(0);
    while (!index.isParameter(asked) && lookThroughOpcodes.contains(index.opcodeOf(asked)))
        asked = index.value(asked).operand(0);
    if (!index.isParameter(asked) && !shouldAssumeInvariantOperands()) {
        bool hasInvariantOperands = true;
        auto ask = [&](ValueID named) {
            Reference spokenOf = named ? referenceOf(named) : Reference();
            if (spokenOf && spokenOf.isValue() && !isLoopInvariant(spokenOf))
                hasInvariantOperands = false;
        };
        index.value(asked).forEachOperand([&](ValueID operand, uint32_t) {
            ask(operand);
            ValueID sum = operand && asked == branch.operand(0) ? index.stripCasts(operand) : ValueID();
            if (!sum || index.isParameter(sum) || index.opcodeOf(sum) != Opcode::Add)
                return;
            ask(index.value(sum).operand(0));
            ask(index.value(sum).operand(1));
        });
        if (!hasInvariantOperands)
            return;
    }
    uint32_t targetIndex = 0;
    for (BodyTarget target : branch.targets()) {
        for (uint32_t proofIndex = 0; proofIndex < target.proofs().size(); ++proofIndex) {
            std::optional<BodyEdgeResult> result = index.edgeResult(branch, targetIndex, target, proofIndex);
            if (!result || !isLoopInvariant(referenceOf(result->subject)))
                return;
        }
        ++targetIndex;
        for (ValueID brought : target.arguments()) {
            if (!isLoopInvariant(referenceOf(brought)))
                return;
        }
        for (ValueID brought : index.block(target.block()).terminator().next().arguments()) {
            if (index.blockOf(brought) != target.block() && !isLoopInvariant(referenceOf(brought)))
                return;
        }
    }

    m_openLoops.changeable(*place).earlierPreheaderBlocks.append(front);
    m_hoistedDiamond = HoistedDiamond { join, *place, m_block, m_state, m_code.numberOfValues() };
    m_state.clear();
    m_block = front;
    count(StatisticID::ConstantDiamondsHoisted);
}

bool Walker::areOperandsLoopInvariant(const Draft& draft) const
{
    auto preheaderDepth = [&](BlockID block) {
        return block && block.index() < m_preheaderDepths.size() ? m_preheaderDepths[block.index()] : 0;
    };
    uint32_t place = m_hoistedDiamond ? m_hoistedDiamond->placeOfLoop + 1 : preheaderDepth(m_block);
    if (!place)
        return true;
    uint32_t firstDiamondRecord = m_hoistedDiamond ? m_hoistedDiamond->firstRecord : std::numeric_limits<uint32_t>::max();
    uint32_t firstRecordOfLoop = m_openLoops[place - 1].firstRecord;
    for (Reference operand : draft.operands()) {
        if (!operand || !operand.isValue() || operand.id().index() < firstRecordOfLoop || operand.id().index() >= firstDiamondRecord)
            continue;
        uint32_t placeOfOperand = preheaderDepth(m_code.blockOf(operand.id()));
        if (!placeOfOperand || placeOfOperand > place)
            return false;
    }
    return true;
}

bool Walker::isHoistedDiamondParameter(Reference reference) const
{
    std::optional<uint32_t> loop = specializedOpenLoop();
    return loop && reference.isValue() && reference.id().index() >= m_openLoops[*loop].firstRecord && m_code.at(reference.id()).opcode == Opcode::Parameter && isInvariantInLoop(*loop, reference);
}

void Walker::finishHoistingConstantDiamond(BlockID inputBlock)
{
    HoistedDiamond& arms = *m_hoistedDiamond;
    if (inputBlock != arms.inputJoin)
        return;
    OpenLoop& loop = m_openLoops.changeable(arms.placeOfLoop);
    m_code.setLoopMarks(m_block, m_code.at(loop.earlierPreheaderBlocks.last()).isInLoop, false);
    while (m_preheaderDepths.size() <= m_block.index())
        m_preheaderDepths.append(0);
    setPreheaderDepth(m_block, arms.placeOfLoop + 1);
    loop.preheaderBlock = m_block;
    m_block = arms.resumeBlock;
    m_state = WTF::move(arms.resumeState);
    m_hoistedDiamond = std::nullopt;
}

bool Walker::isInSightWhereWalkBuilds(Reference reference) const
{
    if (m_numberOfLoopsThatMayBeLeft >= m_openLoops.size() || !reference.isValue() || reference.id().index() < m_openLoops[m_numberOfLoopsThatMayBeLeft].firstRecord)
        return true;
    BlockID block = m_code.blockOf(reference.id());
    uint32_t place = block && block.index() < m_preheaderDepths.size() ? m_preheaderDepths[block.index()] : 0;
    return place && place - 1 <= m_numberOfLoopsThatMayBeLeft;
}

bool Walker::executesInEverySpecializedIteration() const
{
    std::optional<uint32_t> loop = specializedOpenLoop();
    return loop && executesOnEveryIteration(*loop, currentHoistPoint());
}

bool Walker::isInvariantInInnermostLoop(Reference reference) const
{
    std::optional<uint32_t> loop = innermostOpenLoop();
    return loop && reference && reference.isRecord() && isInvariantInLoop(*loop, reference) && typeOf(reference) == ownTypeOf(reference);
}

bool Walker::isLookupLoopInvariant(std::span<const Reference> operands, const Effects& ofLookup) const
{
    std::optional<uint32_t> loop = innermostOpenLoop();
    if (!loop || ofLookup.mayThrowByRule() || !ofLookup.alwaysCompletes() || !std::ranges::all_of(operands, [&](Reference operand) { return isInvariantInInnermostLoop(operand); }))
        return false;
    const Effects& ofLoop = m_openLoops[*loop].effects;
    return !ofLoop.mayRunOtherCode() && !ofLookup.reads.without(unobservableWriteHeaps).overlaps(ofLoop.writes);
}

const Walker::OpenLoop* Walker::openLoopOfHeader(BlockID head) const
{
    for (size_t place = m_openLoops.size(); place--;) {
        if (m_openLoops[place].head == head)
            return &m_openLoops[place];
    }
    return nullptr;
}

void Walker::appendCarriedResults(BlockID head, Vector<Reference, 8>& arguments)
{
    const OpenLoop* loop = m_carriedResultRequests.isEmpty() ? nullptr : openLoopOfHeader(head);
    if (!loop || !loop->carriedResultCount)
        return;
    const Vector<CarriedResult>& requests = m_carriedResultRequests.find(loop->carriedResultsSummary)->value;
    for (uint32_t slot = 0; slot < loop->carriedResultCount; ++slot) {
        Reference result = referenceBoundIn(m_frames[loop->frame], requests[slot].site);
        BlockID block = result && result.isValue() && !m_code.isConstant(result.id()) ? m_code.blockOf(result.id()) : BlockID();
        bool fits = block && block.index() > head.index() && ownTypeOf(result) && m_types.isSubset(ownTypeOf(result), requests[slot].type);
        arguments.append(fits ? result : constant(Draft(Opcode::Absent, m_origin)));
    }
}

Reference Walker::buildOnFirstArrival(ValueID site, const Draft& call, const Effects& effects)
{
    std::optional<uint32_t> place = innermostOpenLoop();
    if (!place || m_isBuildingInPlace || !has(Mechanism::LoopInvariants) || isUnreachable() || isUnderHandler() || hasStringsInBuilders())
        return { };
    const OpenLoop& loop = m_openLoops[*place];
    if (loop.frame != m_frames.size() - 1 || !executesOnEveryIteration(*place, currentHoistPoint()))
        return { };
    TypeID resultType = resultTypeOf(call);
    if (!resultType || isNeverType(resultType) || !m_types.at(resultType).isJavaScriptValue() || (m_types.at(resultType).primitives & Primitive::Absent))
        return { };
    auto isForSite = [&](const CarriedResult& requested) { return requested.site == site; };
    size_t slot = loop.carriedResultCount ? m_carriedResultRequests.find(loop.carriedResultsSummary)->value.findIf(isForSite) : notFound;
    if (slot >= loop.carriedResultCount) {
        if (!m_trialVisits.isEmpty() && m_trialVisits.last().frameIndex == loop.frame && m_trialVisits.last().inputHead == m_frames[loop.frame].index->loop(loop.loop).head) {
            Vector<CarriedResult>& requests = m_carriedResultRequests.add(m_trialVisits.last().summaryIndex, Vector<CarriedResult> { }).iterator->value;
            if (!requests.containsIf(isForSite)) {
                requests.append(CarriedResult { site, m_types.withPrimitives(resultType, Primitive::Absent) });
                m_trialVisits.last().requestsCarriedResult = true;
            }
        }
        return { };
    }
    if (!m_types.isSubset(resultType, m_carriedResultRequests.find(loop.carriedResultsSummary)->value[slot].type))
        return { };
    std::span<const ValueID> parameters = m_code.at(loop.head).parameters();
    Reference carried = Reference::value(parameters[parameters.size() - loop.carriedResultCount + slot]);

    Origin origin = call.origin();
    origin.isTailCall = false;
    Draft question(Opcode::TypeTest, origin);
    question.setValueOperand(carried);
    question.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
    Reference isAbsent = build(WTF::move(question));
    if (!isAbsent || !isAbsent.isValue() || isUnreachable())
        return { };
    BlockID firstArrival = addBlock(BlockFrequency::Unlikely);
    BlockID laterArrival = addBlock();
    BlockID meeting = addBlock();
    branch(isAbsent, firstArrival, laterArrival);

    continueIn(firstArrival);
    Reference computed = build(Draft(call), &effects);
    if (hasFailed())
        return { };
    if (!computed || isUnreachable()) {
        fail("a call made on its first arrival in a loop has no result"_s);
        return { };
    }
    Vector<JoinArm, 2> arms { JoinArm { currentBlock(), { computed } } };

    continueIn(laterArrival);
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(carried);
    proof.setTestOperand(isAbsent);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(false);
    Reference passedOn = build(WTF::move(proof));
    if (hasFailed())
        return { };
    if (!passedOn || isUnreachable()) {
        fail("a result passed around a loop cannot be narrowed"_s);
        return { };
    }
    arms.append(JoinArm { currentBlock(), { passedOn } });

    Reference result;
    if (!joinArms(arms.span(), meeting, singleElementSpan(result)))
        return { };
    count(StatisticID::ResultsCarriedAroundLoops);
    return result;
}

void Walker::scanForImplicitExits(uint32_t loopIndex, PendingGuards& pending)
{
    const OpenLoop& loop = m_openLoops[loopIndex];
    if (loop.hasImplicitExit)
        return;
    uint32_t end = m_code.numberOfValues();
    bool isFound = false;
    for (uint32_t index = std::max(loop.firstRecord, loop.implicitExitScanEnd); index < end && !isFound; ++index) {
        if (m_code.isConstant(ValueID(index)))
            continue;
        EffectTraits traits = m_code.at(ValueID(index)).traits();
        if (!traits.mayThrowByRule() && !traits.mayRunOtherCode())
            continue;
        BlockID block = m_code.blockOf(ValueID(index));
        uint32_t depth = block && block.index() < m_preheaderDepths.size() ? m_preheaderDepths[block.index()] : 0;
        isFound = !depth || depth - 1 > loopIndex;
    }
    isFound = isFound || !collectGuardExits(loopIndex, pending);
    OpenLoop& scanned = m_openLoops.changeable(loopIndex);
    scanned.hasImplicitExit = isFound;
    scanned.implicitExitScanEnd = end;
}

bool Walker::collectGuardExits(uint32_t loopIndex, PendingGuards& pending) const
{
    const OpenLoop& loop = m_openLoops[loopIndex];
    const VersionedLoop* exited = nullptr;
    bool isOwnVersion = false;
    for (size_t place = m_versionedLoops.size(); place-- && !exited;) {
        const VersionedLoop& versionedLoop = m_versionedLoops[place];
        for (uint32_t index = loopIndex + 1; index-- && !exited;) {
            const OpenLoop& open = m_openLoops[index];
            if (open.frame != versionedLoop.frameIndex || m_frames[open.frame].index->loop(open.loop).head != versionedLoop.inputHead)
                continue;
            exited = &versionedLoop;
            isOwnVersion = index == loopIndex;
        }
    }
    if (!exited || !exited->entry)
        return true;

    struct BranchInto {
        BlockID from;
        const Terminator* end { nullptr };
    };
    auto onlyBranchInto = [&](BlockID block) -> BranchInto {
        EdgeID first = m_code.at(block).firstEdgeIn();
        if (!first || m_code.at(first).nextInto())
            return { };
        const Terminator& end = m_code.at(m_code.at(first).from).terminator;
        if (!end.isConditionalBranch() || m_code.at(end.taken()).to == m_code.at(end.notTaken()).to)
            return { };
        return { m_code.at(first).from, &end };
    };
    ValueID headerTest;
    if (BodyTerminator end = m_frames[loop.frame].index->block(exited->inputHead).terminator(); isOwnVersion && exited->headAsksLesserBound && end.kind() == TerminatorKind::Branch) {
        if (Reference test = referenceBoundIn(m_frames[loop.frame], end.as<TerminatorKind::Branch>().conditionOperand()); test && test.isValue())
            headerTest = test.id();
    }
    auto leavesAtHeaderTest = [&](BlockID block) {
        for (unsigned step = 0; headerTest && step < 2; ++step) {
            BranchInto branch = onlyBranchInto(block);
            if (!branch.end)
                return false;
            if (branch.end->conditionOperand() == headerTest)
                return m_code.at(branch.end->notTaken()).to == block;
            block = branch.from;
        }
        return false;
    };

    for (EdgeID edge = loop.guardExitScanEnd ? m_code.at(loop.guardExitScanEnd).nextInto() : m_code.at(exited->entry).firstEdgeIn(); edge; edge = m_code.at(edge).nextInto()) {
        pending.lastEdge = edge;
        BlockID leaving = m_code.at(edge).from;
        if (leaving.index() <= loop.head.index() || leaving == loop.reloadFallbackBlock || leavesAtHeaderTest(leaving))
            continue;
        BranchInto branch = onlyBranchInto(leaving);
        if (!isOwnVersion || !branch.end || m_code.isConstant(branch.end->conditionOperand()) || !isInvariantInLoop(loopIndex, Reference::value(branch.end->conditionOperand())))
            return false;
        InvariantGuard guard { branch.end->conditionOperand(), m_code.at(branch.end->notTaken()).to == leaving };
        if (!pending.guards.containsIf([&](const InvariantGuard& other) { return other.test == guard.test; }))
            pending.guards.append(guard);
    }
    return true;
}

bool Walker::hoistInvariantGuards(uint32_t loopIndex, IterationPurity purity, const PendingGuards& pending)
{
    if (!pending.guards.isEmpty() && (specializedOpenLoop(purity) != loopIndex || m_openLoops[loopIndex].firstReloadBlock))
        return false;
    for (const InvariantGuard& guard : pending.guards) {
        if (!tryBranchInPreheader(Reference::value(guard.test), guard.expectedResult, purity)) {
            m_openLoops.changeable(loopIndex).hasImplicitExit = true;
            return false;
        }
    }
    if (pending.lastEdge && pending.lastEdge != m_openLoops[loopIndex].guardExitScanEnd)
        m_openLoops.changeable(loopIndex).guardExitScanEnd = pending.lastEdge;
    return true;
}

bool Walker::tryPreparePreheaderForCall()
{
    std::optional<uint32_t> loop = specializedOpenLoop();
    if (!loop || !isGuaranteedToExecute(*loop, currentHoistPoint()))
        return false;
    PendingGuards pending;
    scanForImplicitExits(*loop, pending);
    return !m_openLoops[*loop].hasImplicitExit && tryHoistEntryTest() && hoistInvariantGuards(*loop, IterationPurity::EffectFree, pending);
}

bool Walker::tryHoistEntryTest(IterationPurity purity)
{
    std::optional<uint32_t> loop = specializedOpenLoop(purity);
    if (loop && m_openLoops[*loop].hasEntryTestInPreheader)
        return true;
    Reference begins = loop ? buildLoopEntryCondition(*loop) : Reference();
    BlockID preheader = loop ? m_openLoops[*loop].preheaderBlock : BlockID();
    if (!begins || !tryBranchInPreheader(begins, true, purity))
        return false;
    if (m_openLoops[*loop].preheaderBlock != preheader)
        m_openLoops.changeable(*loop).hasEntryTestInPreheader = true;
    return true;
}

std::optional<uint32_t> Walker::outermostLoopToMakeTextInFrontOf(std::span<const Reference> operands, const Effects& ofText) const
{
    if (isUnreachable() || isUnderHandler() || m_coldCopyDepth)
        return std::nullopt;
    for (Reference operand : operands) {
        if (!operand || !operand.isRecord() || typeOf(operand) != ownTypeOf(operand))
            return std::nullopt;
    }

    std::optional<uint32_t> outermost;
    forEachOpenLoopAround(std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft), 0, [&](uint32_t index) {
        const OpenLoop& loop = m_openLoops[index];
        if (!loop.preheaderBlock || m_code.at(loop.preheaderBlock).hasEnded() || m_code.at(loop.preheaderBlock).handler)
            return IterationStatus::Done;
        if (!ofText.writes.isSubsetOf(loop.effects.writes) || (ofText.mayCollect && !loop.effects.mayCollect))
            return IterationStatus::Done;
        bool operandsStandInFront = std::ranges::all_of(operands, [&](Reference operand) {
            return isInvariantInLoop(index, operand);
        });
        if (!operandsStandInFront)
            return IterationStatus::Done;
        outermost = index;
        return IterationStatus::Continue;
    });
    return outermost;
}

std::optional<uint32_t> Walker::outermostLoopToLookUpInFrontOf(std::span<const Reference> operands, const Effects& ofLookup)
{
    if (m_isBuildingInPlace || !has(Mechanism::LoopInvariants) || isUnreachable() || isUnderHandler() || m_coldCopyDepth || ofLookup.mayThrowByRule() || !ofLookup.alwaysCompletes())
        return std::nullopt;
    for (Reference operand : operands) {
        if (!operand || !operand.isRecord() || typeOf(operand) != ownTypeOf(operand))
            return std::nullopt;
    }

    if (drawChoice(DrawnFor::PlaceInFrontOfLoop, currentFunction().index(), m_origin.position, false) == ChoiceOfCost::No)
        return std::nullopt;

    LocationMasks reads = ofLookup.reads.without(unobservableWriteHeaps);
    HoistPoint point = currentHoistPoint();
    std::optional<uint32_t> outermost;
    forEachOpenLoopAround(std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft), 0, [&](uint32_t index) {
        const OpenLoop& loop = m_openLoops[index];
        bool mayLeave = loop.preheaderBlock && !m_code.at(loop.preheaderBlock).hasEnded() && !m_code.at(loop.preheaderBlock).handler && !loop.firstReloadBlock
            && !loop.effects.mayRunOtherCode() && !reads.overlaps(loop.effects.writes) && ofLookup.writes.isSubsetOf(loop.effects.writes) && (!ofLookup.mayCollect || loop.effects.mayCollect)
            && std::ranges::all_of(operands, [&](Reference operand) { return isInvariantInLoop(index, operand); }) && isGuaranteedToExecute(index, point);
        if (!mayLeave)
            return IterationStatus::Done;
        PendingGuards pending;
        scanForImplicitExits(index, pending);
        if (loop.hasImplicitExit)
            return IterationStatus::Done;
        constexpr IterationPurity purity = IterationPurity::KnownEffectsOnly;
        bool headerMayExit = headerMayExitBefore(index, point);
        if ((headerMayExit || !pending.guards.isEmpty()) && specializedOpenLoop(purity) == index) {
            if ((!headerMayExit || tryHoistEntryTest(purity)) && hoistInvariantGuards(index, purity, pending))
                outermost = index;
            return IterationStatus::Done;
        }
        if (!hoistInvariantGuards(index, purity, pending))
            return IterationStatus::Done;
        outermost = index;
        point = hoistPointInFrontOf(index);
        return IterationStatus::Continue;
    });
    return outermost;
}

void Walker::holdLookupInFrontAgainstLookupInTurn(Reference believed, Draft&& lookedUpAgain, const Effects& ofLookup, ProofSource source)
{
    if (isUnreachable() || hasFailed() || !believed || !believed.isValue())
        return;
    WalkState asBelieved = m_state;
    Origin origin = lookedUpAgain.origin();
    origin.isTailCall = false;
    auto isSame = [&](Reference left, Reference right) {
        Draft comparison(Opcode::StrictEq, origin);
        chooseConservativeFields(comparison);
        comparison.setLeftOperand(left);
        comparison.setRightOperand(right);
        return buildTestOfCheck(WTF::move(comparison));
    };
    uint32_t numberOfRecordsInFront = m_code.numberOfValues();
    Reference found;
    {
        SetForScope buildInPlaceScope(m_isBuildingInPlace, true);
        found = build(WTF::move(lookedUpAgain), &ofLookup);
    }
    Reference same = !isUnreachable() && wasBuiltInPlaceSince(numberOfRecordsInFront, found) ? isSame(found, believed) : Reference();
    if (same && same.isValue() && !isUnreachable()) {
        if (m_types.isSubset(typeOf(believed), TypeTable::boolean))
            buildCheckOfProof(same, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, source, origin);
        else {
            BlockID whereAnswersAreSame = addBlock();
            BlockID whereTheyDiffer = addBlock(BlockFrequency::Rare);
            branch(same, whereAnswersAreSame, whereTheyDiffer);
            continueIn(whereTheyDiffer);
            for (Reference answer : { found, believed }) {
                Reference isItself = isUnreachable() ? Reference() : isSame(answer, answer);
                if (isItself)
                    buildCheckOfProof(isItself, TypeArena::fromPrimitives(Primitive::False), TypeTable::boolean, source, origin);
            }
            if (!isUnreachable())
                jumpTo(whereAnswersAreSame);
            continueIn(whereAnswersAreSame);
        }
    }
    m_state = WTF::move(asBelieved);
}

void Walker::holdValueRememberedAgainstValueAtSite(Reference believed, Draft&& readAgain)
{
    if (isUnreachable() || hasFailed() || !believed || !believed.isValue())
        return;
    WalkState asBelieved = m_state;
    Origin origin = readAgain.origin();
    origin.isTailCall = false;
    uint32_t numberOfRecordsInFront = m_code.numberOfValues();
    Reference found;
    {
        SetForScope buildInPlaceScope(m_isBuildingInPlace, true);
        found = build(WTF::move(readAgain));
    }
    if (!isUnreachable() && wasBuiltInPlaceSince(numberOfRecordsInFront, found)) {
        Draft question(Opcode::TypeTest, origin);
        question.setValueOperand(found);
        question.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
        Reference isAbsent = buildTestOfCheck(WTF::move(question));
        if (isAbsent && isAbsent.isValue() && !isUnreachable()) {
            BlockID whereSiteAnswers = addBlock();
            BlockID behind = addBlock();
            branch(isAbsent, behind, whereSiteAnswers);
            continueIn(whereSiteAnswers);
            Draft present(Opcode::Narrowed, origin);
            present.setValueOperand(found);
            present.setTestOperand(isAbsent);
            present.setOperandIndex(OperandIndex::TypeTest::value);
            present.setIsOnTakenEdge(false);
            if (Reference answer = build(WTF::move(present)); answer && !isUnreachable())
                checkThatValuesAreTheSame(answer, believed, origin);
            if (!isUnreachable())
                jumpTo(behind);
            continueIn(behind);
        }
    }
    m_state = WTF::move(asBelieved);
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
    LocationMasks writes = effects.writes.without(unobservableWriteHeaps);
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
                if (!ofCopied.mayRunOtherCode() && ofCopied.writes.without(unobservableWriteHeaps).isSubsetOf(loop.effects.writes))
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

static bool isSameAsRecordInFrontOfLoop(const Code& code, ValueID id, const Draft& draft, uint32_t firstRecordOfLoop)
{
    const Operation& record = code.at(id);
    if (record.opcode != draft.opcode || record.representation != draft.representation || record.flags != draft.flags || !std::ranges::equal(record.payload(), draft.payload()))
        return false;
    if (record.opcode == Opcode::FlattenString && record.isReadOperand() && id.index() < firstRecordOfLoop)
        return false;
    return std::ranges::equal(record.operands(), draft.operands(), [](ValueID held, Reference taken) {
        return taken ? held == taken.id() : !held;
    });
}

ValueID Walker::findSameInFrontOfPlainWalk(const Draft& draft) const
{
    const ValuesBuiltInFront& shared = m_frontsSharedWithTypedBuilds.last().builtInFront;
    for (uint32_t place = 0; place < shared.size(); ++place) {
        ValueID id = ValueID::fromBits(shared[place]);
        if (isSameAsRecordInFrontOfLoop(m_code, id, draft, UINT32_MAX))
            return id;
    }
    return { };
}

ValueID Walker::findSameInFrontOfLoop(uint32_t indexOfLoop, const Draft& draft) const
{
    const OpenLoop& loop = m_openLoops[indexOfLoop];
    auto isSame = [&](ValueID id) {
        return isSameAsRecordInFrontOfLoop(m_code, id, draft, loop.firstRecord);
    };
    if (loop.tableOfBuiltInFront.isEmpty()) {
        for (uint32_t place = 0; place < loop.builtInFront.size(); ++place) {
            ValueID id = ValueID::fromBits(loop.builtInFront[place]);
            if (isSame(id))
                return id;
        }
        return { };
    }
    uint32_t mask = loop.tableOfBuiltInFront.size() - 1;
    for (uint32_t place = hashOfRecordInFrontOfLoop(draft) & mask; ValueID id = ValueID::fromBits(loop.tableOfBuiltInFront[place]); place = (place + 1) & mask) {
        if (isSame(id))
            return id;
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
            if (!loop.preheaderBlock || !isInvariantInLoop(index, operand) || !executesOnEveryIteration(index, point))
                return IterationStatus::Done;
            point = hoistPointInFrontOf(index);
            block = loop.preheaderBlock;
            numberOfLoopsAround = index;
            if (operand.isValue() && loop.reloadBlocks.contains(m_code.blockOf(operand.id()))) {
                block = loop.reloadBlocks.last();
                return IterationStatus::Done;
            }
            return IterationStatus::Continue;
        });
    }
    if (numberOfLoopsAround < m_openLoops.size() && m_openLoops[numberOfLoopsAround].firstReloadBlock) {
        const OpenLoop& loop = m_openLoops[numberOfLoopsAround];
        if (block == loop.preheaderBlock || block == loop.reloadBlocks.last())
            block = operand.isValue() && loop.reloadBlocks.contains(m_code.blockOf(operand.id())) ? loop.reloadBlocks.last() : loop.preheaderBlock;
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
        for (uint32_t place = 0; place < loop.builtInFront.size(); ++place) {
            ValueID id = ValueID::fromBits(loop.builtInFront[place]);
            if (isSame(id))
                return Reference::value(id);
        }
    } else {
        uint32_t mask = loop.tableOfBuiltInFront.size() - 1;
        for (uint32_t place = hashOfRecordInFrontOfLoop(ConversionSoughtInFrontOfLoop { opcode, to, 0, operand }) & mask; ValueID id = ValueID::fromBits(loop.tableOfBuiltInFront[place]); place = (place + 1) & mask) {
            if (isSame(id))
                return Reference::value(id);
        }
    }
    ValueID id = m_code.appendConversion(block, opcode, operand.id(), to, proven, origin);
    noteBuiltInFrontOfLoop(numberOfLoopsAround, id);
    return Reference::value(id);
}

void Walker::noteBuiltInFrontOfLoop(uint32_t indexOfLoop, ValueID built)
{
    OpenLoop& loop = m_openLoops.changeable(indexOfLoop);
    loop.builtInFront.append(built.bits());
    if (loop.builtInFront.size() <= mostBuiltInFrontOfLoopWithoutTable)
        return;
    auto add = [&](ValueID id) {
        uint32_t mask = loop.tableOfBuiltInFront.size() - 1;
        uint32_t place = hashOfRecordInFrontOfLoop(m_code.at(id)) & mask;
        while (ValueID::fromBits(loop.tableOfBuiltInFront[place]))
            place = (place + 1) & mask;
        loop.tableOfBuiltInFront.mutableAt(place) = id.bits();
    };
    if (loop.builtInFront.size() * 2 <= loop.tableOfBuiltInFront.size())
        return add(built);
    loop.tableOfBuiltInFront.fill(ValueID().bits(), std::max<uint32_t>(mostBuiltInFrontOfLoopWithoutTable * 4, loop.tableOfBuiltInFront.size() * 2));
    for (uint32_t place = 0; place < loop.builtInFront.size(); ++place)
        add(ValueID::fromBits(loop.builtInFront[place]));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
