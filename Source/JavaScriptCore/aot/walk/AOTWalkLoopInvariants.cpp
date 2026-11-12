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
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkStrings.h"
#include <algorithm>
#include <optional>
#include <wtf/IterationStatus.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsWrittenOnceForLoopInvariants { AbstractHeap::ShapeWords };

static constexpr OpcodeSet answeredByTextAloneForLoopInvariants { Opcode::GetStringCodeUnit, Opcode::StringEqualsLiteral, Opcode::ValueEqualsLiteral };

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

    if (isByKey && opcode != Opcode::ElemHas && walker.looksTakeFormsBehindLooksAsBuilt() && mayUseKeyedAddress(walker, operation, types(operation.objectOperand()), types(operation.propertyOperand())))
        return effectsOfKeyedAccess(walker, operation, types(operation.objectOperand()));
    if (isByKey && opcode != Opcode::ElemHas && walker.looksTakeFormsBehindLooksAsBuilt() && mayDispatchOverNames(walker, operation, types(operation.objectOperand()), types(operation.propertyOperand())))
        return effectsOfDispatchOverNames(walker, operation, types(operation.objectOperand()));
    if (!isByKey && walker.looksTakeFormsBehindLooksAsBuilt() && mayUseNamedAddress(walker, operation, types(operation.objectOperand())))
        return effectsOfKeyedAccess(walker, operation, types(operation.objectOperand()));
    walker.takeFormAskedAhead();
    TypedForm typedForm = typedFormOf(walker, operation, types);
    if (typedForm == TypedForm::None && walker.looksTakeFormsBehindLooksAsBuilt() && canLookUpCallee(walker, operation))
        return Effects::ofReadByNameThatRunsNoCode();
    if (typedForm == TypedForm::None && (opcode == Opcode::PropGet || opcode == Opcode::PropSet) && walker.looksTakeFormsBehindLooksAsBuilt() && mayUseGlobalBindingInIterations(walker, operation, types(operation.objectOperand()))) {
        Effects effects = declaredBoundOf(Opcode::GetGlobalPropertyAddress);
        Opcode access = opcode == Opcode::PropSet ? Opcode::PutGlobalBinding : Opcode::GetGlobalProperty;
        Effects ofAccess = declaredBoundOf(access);
        narrowGlobalAccessEffects(walker.program(), access, operation.key(), ofAccess);
        effects.merge(ofAccess);
        return effects;
    }
    if (typedForm == TypedForm::None && (opcode == Opcode::PropGet || opcode == Opcode::PropSet) && walker.looksTakeFormsBehindLooksAsBuilt()) {
        if (std::optional<Effects> ofStaticField = effectsOfStaticFieldAccess(walker, operation, types(operation.objectOperand())))
            return ofStaticField;
    }
    if (typedForm == TypedForm::None && sitesMayAnswer && walker.looksTakeFormsBehindLooksAsBuilt() && (siteMayAnswerReadWithoutOtherCode(walker, operation) || mayReadLengthInIterations(walker, operation) || canTestPresenceBeforeLoop(walker, operation)))
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
    bool isPrivate = opcode == Opcode::PrivateGet || opcode == Opcode::PrivateSet;
    TypeID typeOfReceiver = isPrivate || opcode == Opcode::PropGet || opcode == Opcode::PropSet ? types(operation.objectOperand()) : TypeID();
    if (LayoutID heir = typeOfReceiver ? layoutKnownByStructureOfClass(walker, walker.types().withoutNullish(typeOfReceiver)) : LayoutID(); heir && (isPrivate || findField(walker.program(), heir, operation.key()))) {
        bool readsField = (opcode == Opcode::PropGet || opcode == Opcode::PrivateGet) && typeFoundBehindLooks(walker, operation, walker.types().withoutNullish(typeOfReceiver));
        bool storesIntoField = (opcode == Opcode::PropSet || opcode == Opcode::PrivateSet) && typedForm == TypedForm::BuiltBehindLookAtCell;
        if (storesIntoField || readsField) {
            Opcode access = storesIntoField ? Opcode::PutPropertyOfHeir : Opcode::GetPropertyOfHeir;
            Effects ofAccess = declaredBoundOf(access);
            nameFieldInEffectsOfAccessToHeir(walker.program(), access, keyOfAccessToSlot(walker.program(), operation), ofAccess);
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
        bool isOptionalInteger = form == Representation::OptionalInt64 && checksProofs();
        if ((form != Representation::JSValue && form != Representation::Cell && form != Representation::Cell32 && !isOptionalInteger) || !m_types.at(m_code.at(parameter).type).isJavaScriptValue())
            continue;
        TypeID anyOfForm = isOptionalInteger ? widestTypeOf(form) : anyWord;
        Draft check(Opcode::CheckProof, origin);
        check.setValueOperand(Reference::value(parameter));
        check.setCheckedType(anyOfForm);
        check.setTypeWithoutProof(anyOfForm);
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
        TypeID anyOfForm = m_code.at(parameter).representation == Representation::OptionalInt64 ? check.typeWithoutProof() : TypeID(TypeTable::anything);
        if (!m_types.isSubset(anyOfForm, checked))
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
        if (versionedLoop.boundElement)
            restsOn.add(AbstractHeapSet { AbstractHeap::TypedArrayElements, AbstractHeap::TypedArrayStorage });
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
    return place - 1 < indexOfLoop || (operation.opcode != Opcode::Strcat && (operation.opcode != Opcode::FlattenString || operation.isReadOperands().empty()));
}

static bool mayLeaveSomeLoopForLoopInvariants(const TypeArena& types, const Draft& draft, const Effects& effects, bool proofsHoldEverywhere)
{
    Opcode opcode = draft.opcode;
    bool isMadeOnce = opcode == Opcode::NewTemplateObject || (opcode == Opcode::NewClosure && draft.isHeldByClosureTable());
    if (resultRuleOf(opcode) == ResultRule::None || (!isMadeOnce && (resultRuleOf(opcode) == ResultRule::Allocation || interpretedOf(opcode) == Interpretation::Allocation)))
        return false;
    if (opcode == Opcode::IsElementPresent || opcode == Opcode::HasStructureOfClass || opcode == Opcode::GetGlobalProperty || opcode == Opcode::GetKeyedProperty)
        return false;
    if (opcode == Opcode::Narrowed || opcode == Opcode::ResultAt || (!isMadeOnce && boundToOf(opcode) != BoundTo::Nothing))
        return false;
    if (!effects.writes.isEmpty() || effects.mayThrow || !effects.alwaysCompletes() || effects.reads.containsAny(bookkeepingHeaps))
        return false;
    bool readsStableField = opcode == Opcode::GetSlot && effects.reads.isEmpty();
    if (!proofsHoldEverywhere && !readsStableField && (reliesOf(opcode).contains(Proof::SlotFilled) || reliesOf(opcode).contains(Proof::InBounds)))
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

bool Walker::executesOnEveryIteration(uint32_t loopIndex, const HoistPoint& point, UnrolledIterations unrolledIterations) const
{
    if (point.block && m_code.at(point.block).frequency != BlockFrequency::Normal)
        return false;
    const OpenLoop& loop = m_openLoops[loopIndex];
    auto blockInFrame = [&](size_t frameIndex) {
        BlockID block = frameIndex == point.frameIndex ? point.inputBlock : m_frames[frameIndex].block;
        return unrolledIterations == UnrolledIterations::AsWritten ? block : blockAroundConstantTrips(frameIndex, block, frameIndex == loop.frame ? loop.loop : LoopID());
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
    return precedesReturnsOfInlinedCallees(loopIndex, point);
}

bool Walker::precedesReturnsOfInlinedCallees(uint32_t loopIndex, const HoistPoint& point) const
{
    auto blockInFrame = [&](size_t frameIndex) {
        return frameIndex == point.frameIndex ? point.inputBlock : m_frames[frameIndex].block;
    };
    for (size_t frameIndex = m_openLoops[loopIndex].frame + 1; frameIndex <= point.frameIndex; ++frameIndex) {
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

bool Walker::isAfterFewBranches(uint32_t loopIndex, const HoistPoint& point) const
{
    constexpr unsigned mostForks = 3;
    if (point.block && m_code.at(point.block).frequency != BlockFrequency::Normal)
        return false;
    const OpenLoop& loop = m_openLoops[loopIndex];
    unsigned forks = 0;
    for (size_t frameIndex = loop.frame; frameIndex <= point.frameIndex; ++frameIndex) {
        const BodyIndex& index = *m_frames[frameIndex].index;
        BlockID first = frameIndex == loop.frame ? index.loop(loop.loop).head : entryBlock;
        BlockID block = frameIndex == point.frameIndex ? point.inputBlock : m_frames[frameIndex].block;
        while (block != first) {
            BlockID from = index.immediateDominator(block);
            if (!from)
                return false;
            if (index.numberOfIncomingEdges(block) == 1) {
                LoopID inside = index.loopOf(block);
                unsigned others = 0;
                for (BodyTarget target : index.terminator(from).targets())
                    others += target.block() != block && (!inside || index.isInLoop(target.block(), inside));
                if (others > 1 || (others && ++forks > mostForks))
                    return false;
            }
            block = from;
        }
    }
    return true;
}

bool Walker::isOnMainPath(uint32_t loopIndex, const HoistPoint& point) const
{
    constexpr unsigned mostBlocksFollowed = 16;
    const OpenLoop& loop = m_openLoops[loopIndex];
    bool isOnEveryIterationPath = executesOnEveryIteration(loopIndex, point);
    BlockID block = point.block;
    for (unsigned step = 0; block && step < mostBlocksFollowed; ++step) {
        EdgeID entering = m_code.at(block).firstEdgeIn();
        if (!entering || m_code.at(entering).nextInto())
            return isOnEveryIterationPath;
        BlockID from = m_code.at(entering).from;
        const Terminator& end = m_code.at(from).terminator;
        if (end.numberOfEdges > 2)
            return false;
        bool hasOtherFrequentEdge = false;
        for (uint32_t index = 0; index < end.numberOfEdges; ++index) {
            const Edge& edge = m_code.at(end.edge(index));
            hasOtherFrequentEdge |= edge.to != block && (!edge.to || m_code.at(edge.to).frequency == BlockFrequency::Normal);
        }
        if (from == loop.head) {
            const BodyIndex& index = *m_frames[loop.frame].index;
            bool exits = false;
            index.block(index.loop(loop.loop).head).forEachOutgoingEdge([&](BodyTarget target, bool) {
                exits |= !index.isInLoop(target.block(), loop.loop);
            });
            return exits || !hasOtherFrequentEdge;
        }
        if (hasOtherFrequentEdge)
            return false;
        block = from;
    }
    return isOnEveryIterationPath;
}

std::optional<uint32_t> Walker::findLoopToBuildInFrontOf(const Draft& draft, const Effects& effects, TypeID proven) const
{
    if ((draft.opcode == Opcode::GetValueAtSiteOrAbsent || draft.opcode == Opcode::HasPropertyOrAbsent || draft.opcode == Opcode::GetGlobalPropertyAddress) && (effects.reads.containsAny(AbstractHeapSet { AbstractHeap::RuntimeState }) || effects.writes.containsAny(AbstractHeapSet { AbstractHeap::RuntimeState }))) {
        Effects besideItsSite = effects;
        besideItsSite.reads.remove(AbstractHeapSet { AbstractHeap::RuntimeState });
        besideItsSite.writes.remove(AbstractHeapSet { AbstractHeap::RuntimeState });
        return findLoopToBuildInFrontOf(draft, besideItsSite, proven);
    }
    if ((draft.opcode == Opcode::MapNext || draft.opcode == Opcode::SetNext) && effects.writes.containsAny(AbstractHeapSet { AbstractHeap::AllocatorState }) && !effects.writes.containsAny(AbstractHeapSet { AbstractHeap::CollectionStorage })) {
        Effects besideStrings = effects;
        besideStrings.reads.remove(AbstractHeapSet { AbstractHeap::AllocatorState });
        besideStrings.writes.remove(AbstractHeapSet { AbstractHeap::AllocatorState });
        return findLoopToBuildInFrontOf(draft, besideStrings, proven);
    }
    if (draft.opcode == Opcode::GetClassPrototype && effects.writes.containsAny(AbstractHeapSet { AbstractHeap::ClassObjects })) {
        Effects besideClassObjects = effects;
        besideClassObjects.writes.remove(AbstractHeapSet { AbstractHeap::ClassObjects });
        return findLoopToBuildInFrontOf(draft, besideClassObjects, proven);
    }
    if (draft.opcode == Opcode::NewClosure && draft.isHeldByClosureTable() && effects.writes.containsAny(AbstractHeapSet { AbstractHeap::AllocatorState })) {
        Effects behindFirst = effects;
        behindFirst.reads.remove(AbstractHeapSet { AbstractHeap::AllocatorState });
        behindFirst.writes.remove(AbstractHeapSet { AbstractHeap::AllocatorState, AbstractHeap::ClassObjects });
        return findLoopToBuildInFrontOf(draft, behindFirst, proven);
    }
    if (draft.opcode == Opcode::GetBuiltinValue && effects.mayThrow && !effects.mayRunOtherCode() && !effects.mayBeTerminated) {
        if (isUnderHandler())
            return std::nullopt;
        Effects behindFirstRead = Effects::none();
        behindFirstRead.reads.add(observableHeaps);
        behindFirstRead.mayCollect = effects.mayCollect;
        return findLoopToBuildInFrontOf(draft, behindFirstRead, proven);
    }
    if (draft.opcode == Opcode::NewTemplateObject && !effects.writes.isEmpty()) {
        if (isUnderHandler())
            return std::nullopt;
        Effects behindFirst = Effects::none();
        behindFirst.mayCollect = effects.mayCollect;
        return findLoopToBuildInFrontOf(draft, behindFirst, proven);
    }
    if (m_isBuildingInPlace)
        return std::nullopt;
    uint32_t numberOfLoops = std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft);
    if (!numberOfLoops || !mayLeaveSomeLoopForLoopInvariants(m_types, draft, effects, m_numberOfScopesWhereProofsHoldEverywhere))
        return std::nullopt;
    for (Reference operand : draft.operands()) {
        if (operand && !isInvariantInLoop(numberOfLoops - 1, operand))
            return std::nullopt;
    }
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
        bool readsWhatTurnsMayWrite = !reads.isEmpty() && (loop.effects.mayRunOtherCode() || reads.overlaps(loop.effects.writes)) && !iterationsSpareArray(index, draft, reads);
        if (!loop.preheaderBlock || m_code.at(loop.preheaderBlock).hasEnded() || (readsWhatTurnsMayWrite && (reads.containsAny(unobservableWriteHeaps) || reads.overlaps(loop.placesThatQuietTurnsWrite) || !mayReadInFrontOfQuietTurns(index))))
            return IterationStatus::Done;
        if (effects.mayCollect && (!loop.effects.mayCollect || m_code.at(loop.preheaderBlock).handler))
            return IterationStatus::Done;
        bool operandsStandInFront = std::ranges::all_of(draft.operands(), [&](Reference operand) {
            return !operand || isInvariantInLoop(index, operand);
        });
        if (!operandsStandInFront)
            return IterationStatus::Done;
        static constexpr OpcodeSet neverFailOfList { Opcode::GetObjectStorage, Opcode::GetObjectStorageLength, Opcode::GetArrayStorage, Opcode::GetArrayLength, Opcode::GetArrayCapacity, Opcode::HasArrayStorage };
        static constexpr OpcodeSet viewHeaderReads { Opcode::GetTypedArrayLength, Opcode::GetTypedArrayVector, Opcode::GetDataViewLimit };
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
        bool readsMirror = draft.opcode == Opcode::GetMirroredLength || draft.opcode == Opcode::GetMirroredVector || (draft.opcode == Opcode::GetModuleVariable && plan().variable(draft.variable()).hasViewMirror);
        bool mayStandWhereNotEveryTurnAsks = draft.opcode == Opcode::ResolveCallTarget || (readsMirror && m_code.at(m_block).frequency != BlockFrequency::Rare) || ((findsList || isSumWithWrittenNumber || neverFailOfList.contains(draft.opcode)) && m_code.at(m_block).frequency != BlockFrequency::Rare);
        static constexpr OpcodeSet preparedDivisors { Opcode::WholeDivisor, Opcode::ReciprocalOfDivisor };
        bool checksMainPath = preparedDivisors.contains(draft.opcode) || (draft.opcode == Opcode::GetSlot && reads.isEmpty());
        if (!mayStandWhereNotEveryTurnAsks && !(viewHeaderReads.contains(draft.opcode) && isAfterFewBranches(index, point)) && !(checksMainPath ? isOnMainPath(index, point) : executesOnEveryIteration(index, point)))
            return IterationStatus::Done;
        if (readsWhatTurnsMayWrite && isReadForCall(draft) && !executesOnEveryIteration(index, point))
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

bool Walker::iterationsSpareArray(uint32_t loopIndex, const Draft& draft, const LocationMasks& reads) const
{
    static constexpr OpcodeSet readsOfArrayHeader { Opcode::GetArrayStorage, Opcode::GetArrayLength, Opcode::HasArrayStorage };
    const OpenLoop& loop = m_openLoops[loopIndex];
    if (!readsOfArrayHeader.contains(draft.opcode) || !loop.effects.writesObjectsMadeHereAlone || loop.effects.mayRunOtherCode() || reads.without(perObjectHeaps).overlaps(loop.effects.writes))
        return false;
    for (const TrialVisit& trialVisit : m_trialVisits) {
        if (trialVisit.summaryIndex + 1 == loop.summaryIndexPlusOne && trialVisit.othersAreSparedAtHead)
            return classifyAgainstAllocations(*trialVisit.othersAreSparedAtHead, draft.arrayOperand()) == IsListedAllocation::No;
    }
    return false;
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

void Walker::checkHoistedRead(Reference found, TypeID held, ProofSource source)
{
    uint32_t numberOfLoops = std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft);
    for (uint32_t index = 0; found.isValue() && index < numberOfLoops && !isUnreachable(); ++index) {
        BlockID preheader = m_openLoops[index].preheaderBlock;
        if (!preheader || m_code.at(preheader).hasEnded() || m_code.at(found.id()).block() != preheader || !isInvariantInLoop(index, found))
            continue;
        SetForScope preheaderBlock(m_block, preheader);
        SetForScope loopsThatMayBeLeft(m_numberOfLoopsThatMayBeLeft, index);
        holdWhatIsFoundInPlace(found, held, source);
        return;
    }
    holdWhatIsFoundInPlace(found, held, source);
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
    if (std::optional<LoopEntryCondition> condition = findConditionOnEntry(indexOfLoop, asked))
        return condition;

    std::span<const ValueID> entering = open.recordsEntering();
    Reference left;
    Reference right;
    {
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

std::optional<Walker::LoopEntryCondition> Walker::findConditionOnEntry(uint32_t indexOfLoop, ValueID asked) const
{
    const OpenLoop& open = m_openLoops[indexOfLoop];
    const Frame& frame = m_frames[open.frame];
    const BodyIndex& index = *frame.index;
    BodyBlock head = index.block(index.loop(open.loop).head);
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
    if (!left || !right)
        return std::nullopt;
    return LoopEntryCondition { asked, left, right };
}

Reference Walker::buildConditionOnEntry(uint32_t indexOfLoop, const LoopEntryCondition& asked, uint32_t inFrontOf)
{
    const OpenLoop& open = m_openLoops[indexOfLoop];
    Origin origin = open.origin;
    Opcode opcode = m_frames[open.frame].index->opcodeOf(asked.written);
    BlockID inside = std::exchange(m_block, m_openLoops[inFrontOf].preheaderBlock);
    SetForScope loopsThatMayBeLeft(m_numberOfLoopsThatMayBeLeft, inFrontOf);
    Reference condition = buildOperatorForStrings(*this, opcode, asked.left, asked.right, origin);
    if (condition && asked.isNegated && !m_hasFailed)
        condition = buildNegationForStrings(*this, condition, origin);
    m_block = m_hasFailed ? BlockID() : inside;
    return condition;
}

Reference Walker::buildLoopEntryCondition(uint32_t indexOfLoop)
{
    std::optional<LoopEntryCondition> asked = findLoopEntryCondition(indexOfLoop);
    if (!asked)
        return { };
    Reference begins = buildConditionOnEntry(indexOfLoop, *asked, indexOfLoop);
    return begins && begins.isValue() && representationOf(begins) == Representation::Boolean ? begins : Reference();
}

static ValueID behindConversionsOfForm(const Code& code, ValueID value)
{
    while (conversionsOfForm.contains(code.at(value).opcode))
        value = code.at(value).operand(0);
    return value;
}

bool Walker::isFlattenedBeforeLoop(uint32_t loopIndex, Reference string, std::span<const Reference> conditions) const
{
    const OpenLoop& loop = m_openLoops[loopIndex];
    for (uint32_t place = 0; place < loop.builtInFront.size(); ++place) {
        ValueID id = ValueID::fromBits(loop.builtInFront[place]);
        const Operation& record = m_code.at(id);
        if (record.opcode != Opcode::FlattenString || record.mayStayRope() || (!record.isReadOperands().empty() && id.index() < loop.firstRecord) || behindConversionsOfForm(m_code, record.stringOperand()) != behindConversionsOfForm(m_code, string.id()))
            continue;
        bool hasSameConditions = std::ranges::equal(record.isReadOperands(), conditions, [](ValueID held, Reference taken) {
            return held == taken.id();
        });
        if (hasSameConditions)
            return true;
    }
    return false;
}

bool Walker::findFirstArrivalConditionsOf(uint32_t innerLoop, uint32_t loopIndex, const HoistPoint& point, Vector<ArrivalCondition, 4>& conditions) const
{
    bool isBetween = innerLoop != loopIndex;
    std::optional<LoopEntryCondition> begins = isBetween ? std::nullopt : findLoopEntryCondition(innerLoop);
    if (begins)
        conditions.append({ innerLoop, *begins });
    else if (headerMayExitBefore(innerLoop, point))
        return false;
    if (!executesOnEveryIteration(innerLoop, point) || !precedesReturnsOfInlinedCallees(innerLoop, point))
        return false;
    const OpenLoop& loop = m_openLoops[innerLoop];
    const BodyIndex& index = *m_frames[loop.frame].index;
    BlockID inputBlock = loop.frame == point.frameIndex ? point.inputBlock : m_frames[loop.frame].block;
    Vector<LoopEntryCondition, 2> found;
    for (BlockID member : index.membersOf(loop.loop)) {
        if (member == index.loop(loop.loop).head || !index.isReachable(member) || index.dominates(inputBlock, member))
            continue;
        BodyTerminator end = index.terminator(member);
        unsigned numberOfExits = 0;
        index.block(member).forEachOutgoingEdge([&](BodyTarget target, bool) {
            numberOfExits += !index.isInLoop(target.block(), loop.loop);
        });
        if (!numberOfExits)
            continue;
        ValueID asked = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
        if (numberOfExits != 1 || !asked || index.isParameter(asked) || !allRelations.contains(index.opcodeOf(asked)) || !index.dominates(member, inputBlock))
            return false;
        std::optional<LoopEntryCondition> condition = isBetween ? std::nullopt : findConditionOnEntry(innerLoop, asked);
        if (!condition)
            return false;
        condition->isNegated = !index.isInLoop((*end.targets().begin()).block(), loop.loop);
        found.append(*condition);
    }
    for (const LoopEntryCondition& condition : found)
        conditions.append({ innerLoop, condition });
    return true;
}

bool Walker::findFirstArrivalConditions(uint32_t loopIndex, Vector<ArrivalCondition, 4>& conditions) const
{
    bool areAllFound = true;
    HoistPoint point = currentHoistPoint();
    forEachOpenLoopAround(std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft), 0, [&](uint32_t index) {
        areAllFound &= findFirstArrivalConditionsOf(index, loopIndex, point, conditions);
        if (index <= loopIndex)
            return IterationStatus::Done;
        point = hoistPointInFrontOf(index);
        return IterationStatus::Continue;
    });
    return areAllFound;
}

bool Walker::isReachedByEachIteration(uint32_t loopIndex) const
{
    HoistPoint point = currentHoistPoint();
    return executesOnEveryIteration(loopIndex, point) && precedesReturnsOfInlinedCallees(loopIndex, point);
}

bool Walker::tryBuildFirstArrivalConditions(uint32_t loopIndex, Vector<Reference, 2>& conditions)
{
    Vector<ArrivalCondition, 4> found;
    bool areAllBuilt = findFirstArrivalConditions(loopIndex, found);
    for (const ArrivalCondition& condition : found) {
        Reference holds = buildConditionOnEntry(condition.loop, condition.entry, loopIndex);
        if (m_hasFailed || !holds)
            return false;
        if (std::optional<bool> outcome = outcomeKnownOf(*this, holds)) {
            areAllBuilt &= *outcome;
            continue;
        }
        if (!holds.isValue() || representationOf(holds) != Representation::Boolean) {
            areAllBuilt = false;
            continue;
        }
        if (!conditions.contains(holds))
            conditions.append(holds);
    }
    return areAllBuilt;
}

Reference Walker::flatStringBeforeLoop(uint32_t loopIndex, Reference string)
{
    Vector<ValueID, 4> flattened;
    const OpenLoop& loop = m_openLoops[loopIndex];
    for (uint32_t place = 0; place < loop.builtInFront.size(); ++place) {
        ValueID id = ValueID::fromBits(loop.builtInFront[place]);
        const Operation& record = m_code.at(id);
        if (record.opcode == Opcode::FlattenString && !record.mayStayRope() && (record.isReadOperands().empty() || id.index() >= loop.firstRecord) && behindConversionsOfForm(m_code, record.stringOperand()) == behindConversionsOfForm(m_code, string.id()))
            flattened.append(id);
    }
    if (flattened.isEmpty())
        return { };
    Vector<Reference, 2> conditions;
    tryBuildFirstArrivalConditions(loopIndex, conditions);
    if (m_hasFailed)
        return { };
    for (ValueID id : flattened) {
        bool isFlatHere = std::ranges::all_of(m_code.at(id).isReadOperands(), [&](ValueID held) {
            return conditions.contains(Reference::value(held));
        });
        if (isFlatHere && isInSightWhereWalkBuilds(Reference::value(id)))
            return Reference::value(id);
    }
    return { };
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
    return std::ranges::all_of(draft.operands(), [&](Reference operand) { return isOperandLoopInvariant(operand); });
}

bool Walker::isOperandLoopInvariant(Reference operand) const
{
    auto preheaderDepth = [&](BlockID block) {
        return block && block.index() < m_preheaderDepths.size() ? m_preheaderDepths[block.index()] : 0;
    };
    uint32_t place = m_hoistedDiamond ? m_hoistedDiamond->placeOfLoop + 1 : preheaderDepth(m_block);
    if (!place)
        return true;
    uint32_t firstDiamondRecord = m_hoistedDiamond ? m_hoistedDiamond->firstRecord : std::numeric_limits<uint32_t>::max();
    uint32_t firstRecordOfLoop = m_openLoops[place - 1].firstRecord;
    if (!operand || !operand.isValue() || operand.id().index() < firstRecordOfLoop || operand.id().index() >= firstDiamondRecord)
        return true;
    uint32_t placeOfOperand = preheaderDepth(m_code.blockOf(operand.id()));
    return placeOfOperand && placeOfOperand <= place;
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

bool Walker::dominatesLoopVersions(Reference reference) const
{
    if (!reference.isValue())
        return reference.isConstant();
    if (m_code.isConstant(reference.id()))
        return true;
    BlockID home = m_code.blockOf(reference.id());
    auto isHomeBefore = [&](BlockID block) {
        while (block && block != home) {
            const Block& passed = m_code.at(block);
            EdgeID entering = passed.firstEdgeIn();
            if (passed.isLoopHead || passed.isEntry || passed.isHandlerBlock() || !entering || m_code.at(entering).nextInto())
                return false;
            block = m_code.at(entering).from;
        }
        return !!block;
    };
    if (isHomeBefore(m_block) || (m_isVersioningDryRun && isHomeBefore(m_frontOfLoopLeftOpen.block)))
        return true;
    return std::ranges::any_of(m_openLoops, [&](const OpenLoop& loop) { return isHomeBefore(loop.preheaderBlock); });
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
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    for (uint32_t slot = 0; slot < loop->carriedResultCount; ++slot) {
        ValueID parameter = parameters[parameters.size() - loop->carriedResultCount + slot];
        Reference result;
        if (requests[slot].isField()) {
            size_t place = loop->carriedFields.findIf([&](const OpenLoop::CarriedField& field) { return field.slot == slot; });
            if (place != notFound && loop->carriedFields[place].object) {
                result = fieldCarriedAcrossBackEdge(requests[slot], loop->carriedFields[place]);
                if (m_hasFailed)
                    return;
                if (!result || !m_types.isSubset(ownTypeOf(result), m_code.at(parameter).type)) {
                    strikeCarriedElement(head, m_carriedResultRequests.find(loop->carriedResultsSummary)->value[slot], representationOf(Reference::value(parameter)));
                    result = { };
                }
            } else if (place == notFound) {
                OpenLoop& changed = m_openLoops.changeable(loop - m_openLoops.begin());
                changed.carriedFields.append({ slot, { }, { } });
                loop = &changed;
            }
        } else if (requests[slot].isElement) {
            size_t place = loop->carriedElements.findIf([&](const OpenLoop::CarriedElement& element) { return element.slot == slot; });
            if (place != notFound && loop->carriedElements[place].counterPosition == noCounter)
                result = findElementAtConstant(loop->carriedElements[place].array, loop->carriedElements[place].offset);
            if (!result && (place == notFound || loop->carriedElements[place].counterPosition != noCounter)) {
                std::optional<uint64_t> stored = place != notFound && passesNextCounter(*loop, loop->carriedElements[place].counterPosition, arguments.span()) ? findKnown(*this, KnownNote::CarriedElement, loop->carriedElements[place].array, Reference::value(parameter), 1) : std::nullopt;
                result = stored ? Reference::fromBits(static_cast<uint32_t>(*stored)) : Reference();
            }
            if (place != notFound && (!result || !m_types.isSubset(ownTypeOf(result), m_code.at(parameter).type))) {
                OpenLoop& changed = m_openLoops.changeable(loop - m_openLoops.begin());
                changed.carriedElements[place].isMissingOnBackEdge = true;
                loop = &changed;
                if (changed.carriedElements[place].isRead)
                    strikeCarriedElement(head, m_carriedResultRequests.find(loop->carriedResultsSummary)->value[slot], representationOf(Reference::value(parameter)));
            }
        } else
            result = carriedResultOf(m_state, Reference::value(parameter));
        bool fits = result && ownTypeOf(result) && m_types.isSubset(ownTypeOf(result), m_code.at(parameter).type);
        arguments.append(fits ? result : absentCarriedResult(requests[slot]));
    }
}

Reference Walker::absentCarriedResult(const CarriedResult& requested)
{
    if (!requested.isField())
        return absentResult(requested.form);
    Draft nothing(Opcode::Integer, m_origin);
    Range integers = m_types.at(requested.type).integers;
    nothing.setInteger(integers.contains(0) ? 0 : integers.minimum);
    return constant(WTF::move(nothing));
}

Reference Walker::fieldCarriedAcrossBackEdge(const CarriedResult& requested, const OpenLoop::CarriedField& field)
{
    if (requested.staticField) {
        std::optional<uint64_t> held = findKnown(*this, KnownNote::StaticFieldInVariable, { }, { }, requested.staticField.index());
        if (Reference known = held ? Reference::fromBits(static_cast<uint32_t>(*held)) : Reference(); known && known.isRecord())
            return known;
        return readInitializedStaticField(field.object, requested.staticField, requested.fieldKey, m_origin);
    }
    if (Reference held = m_state.slot(field.object, requested.field); held && held.isRecord())
        return held;
    Draft read(Opcode::GetSlot, m_origin);
    chooseConservativeFields(read);
    read.setObjectOperand(field.object);
    read.setKey(requested.fieldKey);
    read.setReceivers(field.receivers);
    read.setLevel(KnowledgeLevel::Proven);
    return build(WTF::move(read));
}

void Walker::requestCarriedField(Reference object, PropertyKeyID key, Reference before, VariableID staticField, Reference stored)
{
    if (!before || !before.isValue() || m_code.isConstant(before.id()) || m_isBuildingInPlace || isUnderHandler() || hasStringsInBuilders() || !has(Mechanism::LoopInvariants) || !has(Mechanism::Folding))
        return;
    ValueID read = underlyingCellOf(m_code, before.id());
    if (m_code.isConstant(read) || m_code.at(read).opcode != (staticField ? Opcode::GetStaticFieldOrAbsent : Opcode::GetSlot) || m_code.at(read).key() != key || (staticField && !m_code.at(read).isKnownInitialized()))
        return;
    std::optional<uint32_t> place = innermostOpenLoop();
    if (!place || read.index() < m_openLoops[*place].firstRecord)
        return;
    const OpenLoop& loop = m_openLoops[*place];
    TrialVisit& visit = m_trialVisits.last();
    if (visit.frameIndex != loop.frame || visit.inputHead != m_frames[loop.frame].index->loop(loop.loop).head || !isInvariantInLoop(*place, object) || !executesOnEveryIteration(*place, currentHoistPoint()))
        return;
    Representation form = representationOf(Reference::value(read));
    TypeID type = ownTypeOf(Reference::value(read));
    bool isNumber = form == Representation::Int32 || form == Representation::Int64 || form == Representation::Double;
    if ((!isNumber && form != Representation::JSValue) || !type || !(m_types.at(type).primitives & Primitive::Integer) || (isNumber && !m_types.at(type).isNumber()))
        return;
    CarriedResult wanted { ValueID(), type, false, false, form };
    wanted.fieldKey = key;
    wanted.staticField = staticField;
    if (!staticField) {
        std::optional<FilledSlot> filled = findFilledSlot(*this, typeOf(object), key);
        if (!filled && isSlotTestedBeforeLoop(*place, object, key))
            filled = findPermanentlyFilledSlot(*this, typeOf(object), key, true);
        if (!filled)
            return;
        wanted.field = filled->place;
        TypeID typeOfStored = stored ? ownTypeOf(stored) : TypeID();
        if (form == Representation::JSValue && typeOfStored && m_types.at(typeOfStored).isNumber()) {
            bool isInt32 = m_types.isSubset(typeOfStored, TypeTable::int32);
            wanted.type = isInt32 ? TypeTable::int32 : TypeTable::number;
            wanted.form = isInt32 ? Representation::Int32 : Representation::Double;
        }
    }
    Vector<CarriedResult>& requests = m_carriedResultRequests.add(visit.summaryIndex, Vector<CarriedResult> { }).iterator->value;
    if (requests.containsIf([&](const CarriedResult& requested) { return requested.fieldKey == key && requested.field == wanted.field && requested.staticField == staticField; }))
        return;
    requests.append(wanted);
    visit.requestsCarriedResult = true;
}

bool Walker::canTestStoredSlotBeforeLoop(Reference object, PropertyKeyID key, IterationPurity purity)
{
    std::optional<uint32_t> place = key && object.isValue() ? specializedOpenLoop(purity) : std::nullopt;
    if (!place || !isInvariantInLoop(*place, object) || typeOf(object) != ownTypeOf(object) || m_openLoops[*place].firstReloadBlock)
        return false;
    const Effects& ofIterations = m_openLoops[*place].effects;
    LocationMasks slot;
    slot.add(program(), MemoryLocation::objectField(key));
    if (ofIterations.mayRunOtherCode() || !ofIterations.writes.overlaps(slot) || !passesOnEveryIteration(m_versionedLoops.last()) || !executesOnEveryIteration(*place, currentHoistPoint()))
        return false;
    static constexpr AbstractHeapSet ownKeysHeaps { AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::NamedProperties };
    PropertyKeyKind kind = program().at(key).kind;
    bool isPrivate = kind == PropertyKeyKind::PrivateName || kind == PropertyKeyKind::ModulePrivateName;
    if (!isPrivate && ofIterations.writes.overlaps(LocationMasks(ownKeysHeaps)))
        return false;
    return !!findPermanentlyFilledSlot(*this, typeOf(object), key, true);
}

bool Walker::isSlotTestedBeforeLoop(uint32_t indexOfLoop, Reference object, PropertyKeyID key) const
{
    return m_openLoops[indexOfLoop].earlierPreheaderBlocks.containsIf([&](BlockID block) {
        const Terminator& end = m_code.at(block).terminator;
        if (!end.isConditionalBranch() || m_code.isConstant(end.conditionOperand()))
            return false;
        const Operation& test = m_code.at(end.conditionOperand());
        return test.opcode == Opcode::IsSlotFilled && test.key() == key && underlyingCellOf(m_code, test.objectOperand()) == underlyingCellOf(m_code, object.id());
    });
}

std::optional<uint32_t> Walker::findCarriedField(Reference object, PropertyKeyID key, FieldReference field, VariableID staticField)
{
    if (m_carriedResultRequests.isEmpty() || m_isBuildingInPlace || isUnreachable() || isUnderHandler() || hasStringsInBuilders() || m_trialVisits.isEmpty())
        return std::nullopt;
    std::optional<uint32_t> place = innermostOpenLoop();
    if (!place || !m_openLoops[*place].carriedResultCount || m_openLoops[*place].firstReloadBlock || m_code.at(m_block).frequency != BlockFrequency::Normal)
        return std::nullopt;
    const OpenLoop& loop = m_openLoops[*place];
    const Vector<CarriedResult>& requests = m_carriedResultRequests.find(loop.carriedResultsSummary)->value;
    size_t slot = requests.findIf([&](const CarriedResult& requested) { return requested.fieldKey == key && requested.field == field && requested.staticField == staticField && !requested.isStruck; });
    if (slot >= loop.carriedResultCount || loop.carriedSlotsBuilt.contains(static_cast<uint32_t>(slot)) || !loop.stateAtHeader)
        return std::nullopt;
    size_t placeOfParameter = m_code.at(loop.head).parameters().size() - loop.carriedResultCount + slot;
    Reference headerParameter = Reference::value(m_code.at(loop.head).parameters()[placeOfParameter]);
    if (!object || !object.isValue() || !findKnown(*this, KnownNote::CarriedField, { }, headerParameter) || !isInvariantInLoop(*place, object) || typeOf(object) != ownTypeOf(object))
        return std::nullopt;
    if (loop.carriedFields.containsIf([&](const OpenLoop::CarriedField& carried) { return carried.slot == slot; })) {
        strikeCarriedField(static_cast<uint32_t>(slot));
        return std::nullopt;
    }
    return static_cast<uint32_t>(slot);
}

Reference Walker::strikeCarriedField(uint32_t slot)
{
    m_carriedResultRequests.find(m_openLoops[*innermostOpenLoop()].carriedResultsSummary)->value[slot].isStruck = true;
    m_trialVisits.last().requestsCarriedResult = true;
    return { };
}

Reference Walker::readCarriedField(const Draft& read, FieldReference field)
{
    std::optional<uint32_t> slot = findCarriedField(read.objectOperand(), read.key(), field, { });
    if (!slot)
        return { };
    uint32_t place = *innermostOpenLoop();
    WalkState stateOfIteration = std::exchange(m_state, WalkState(*m_openLoops[place].stateAtHeader));
    Reference first = buildInFrontOfOpenLoop(place, Draft(read));
    m_state = WTF::move(stateOfIteration);
    if (hasFailed() || isUnreachable())
        return { };
    return enterWithCarriedField(*slot, read.objectOperand(), read.receivers(), first);
}

Reference Walker::readCarriedStaticField(Reference object, VariableID variable, PropertyKeyID key, Reference before, Reference isAbsentBefore, Origin origin)
{
    std::optional<uint32_t> slot = findCarriedField(object, key, { }, variable);
    if (!slot)
        return { };
    Reference first;
    {
        SpecializedPreheaderScope preheaderScope(*this, true, IterationPurity::KnownEffectsOnly);
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(before);
        proof.setTestOperand(isAbsentBefore);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(false);
        first = build(WTF::move(proof));
    }
    if (hasFailed() || isUnreachable())
        return { };
    return enterWithCarriedField(*slot, object, { }, first);
}

Reference Walker::readInitializedStaticField(Reference object, VariableID variable, PropertyKeyID key, Origin origin)
{
    Draft read(Opcode::GetStaticFieldOrAbsent, origin);
    read.setVariable(variable);
    read.setKey(key);
    read.setValueType(TypeTable::anything);
    read.setClassObjectOperand(object);
    read.setIsKnownInitialized(true);
    return build(WTF::move(read));
}

Reference Walker::enterWithCarriedField(uint32_t slot, Reference object, LayoutSetID receivers, Reference first)
{
    uint32_t place = *innermostOpenLoop();
    size_t placeOfParameter = m_code.at(m_openLoops[place].head).parameters().size() - m_openLoops[place].carriedResultCount + slot;
    Reference headerParameter = Reference::value(m_code.at(m_openLoops[place].head).parameters()[placeOfParameter]);
    Representation form = representationOf(headerParameter);
    if (first && first.isValue() && (form == Representation::Double || form == Representation::Int32) && representationOf(first) == Representation::JSValue) {
        first = unboxNumberBeforeLoop(first, form);
        if (hasFailed() || isUnreachable())
            return { };
        if (first)
            count(StatisticID::SlotsCarriedUnboxed);
    }
    if (!first || !first.isRecord() || representationOf(first) != representationOf(headerParameter) || !m_types.isSubset(typeOf(first), m_code.at(headerParameter.id()).type))
        return strikeCarriedField(slot);
    OpenLoop& changed = m_openLoops.changeable(place);
    changed.carriedSlotsBuilt.append(slot);
    changed.carriedFields.append({ slot, object, receivers });
    auto withField = std::make_shared<ArgumentsOfEdge>(*changed.entering);
    withField->m_records[placeOfParameter] = first.id();
    changed.entering = WTF::move(withField);
    count(StatisticID::SlotsCarriedAroundLoops);
    return headerParameter;
}

Reference Walker::unboxNumberBeforeLoop(Reference first, Representation form)
{
    constexpr auto purity = IterationPurity::KnownEffectsOnly;
    TypeID tested = form == Representation::Int32 ? TypeTable::int32 : TypeTable::number;
    Reference number = first;
    if (!m_types.isSubset(typeOf(first), tested)) {
        Reference test;
        {
            SpecializedPreheaderScope preheaderScope(*this, true, purity);
            Draft question(Opcode::TypeTest, m_origin);
            question.setValueOperand(first);
            question.setTestedType(tested);
            test = build(WTF::move(question));
        }
        if (!test || !test.isValue() || hasFailed() || isUnreachable() || !tryBranchInPreheader(test, true, purity))
            return { };
        recordSpeculation();
        SpecializedPreheaderScope preheaderScope(*this, true, purity);
        Draft proof(Opcode::Narrowed, m_origin);
        proof.setValueOperand(first);
        proof.setTestOperand(test);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(true);
        number = build(WTF::move(proof));
    }
    if (!number || !number.isValue() || hasFailed() || isUnreachable())
        return { };
    Draft unboxing(Opcode::Unbox, m_origin);
    unboxing.setValueOperand(number);
    unboxing.representation = form;
    return buildInFrontOfOpenLoop(*innermostOpenLoop(), WTF::move(unboxing));
}

Reference Walker::absentResult(Representation form)
{
    switch (form) {
    case Representation::Int32:
        return constant(Draft(Opcode::Integer, m_origin));
    case Representation::Double:
        return constant(Draft(Opcode::Double, m_origin));
    default:
        return constant(Draft(Opcode::Absent, m_origin));
    }
}

uint32_t Walker::numberOfCarriedResults(uint32_t summaryIndex)
{
    Vector<CarriedResult>& requests = m_carriedResultRequests.find(summaryIndex)->value;
    bool isHeldByOpenLoop = std::ranges::any_of(m_openLoops, [&](const OpenLoop& loop) { return loop.carriedResultCount && loop.carriedResultsSummary == summaryIndex; });
    if (isHeldByOpenLoop)
        return requests.size();
    return std::ranges::stable_partition(requests, [](const CarriedResult& requested) { return !requested.isStruck; }).begin() - requests.begin();
}

void Walker::strikeCarriedElement(BlockID head, CarriedResult& element, Representation form)
{
    element.isStruck |= element.form == form;
    for (size_t place = m_trialVisits.size(); place--;) {
        TrialVisit& trialVisit = m_trialVisits[place];
        if (trialVisit.frameIndex >= m_frames.size() || inputBlock(trialVisit.frameIndex, trialVisit.inputHead).head != head)
            continue;
        trialVisit.misfits = true;
        trialVisit.requestsCarriedResult = true;
        return;
    }
    fail("an element carried around a loop is not passed by an edge"_s);
}

static constexpr uint64_t noCarriedElementGuard = std::numeric_limits<uint64_t>::max();

Reference Walker::findElementAtConstant(Reference array, int64_t position) const
{
    for (const WalkState::ElementOfObject& element : m_state.listElementsOf(array)) {
        if (!element.index.isConstant())
            continue;
        const Operation& index = m_code.at(element.index.id());
        if (index.opcode == Opcode::Integer && index.integer() == position)
            return element.value;
    }
    return { };
}

Vector<Walker::CarriedElementNote, 2> Walker::listCarriedElementNotes(Reference array, int64_t storedPosition) const
{
    Vector<CarriedElementNote, 2> notes;
    std::optional<uint32_t> place = m_carriedResultRequests.isEmpty() ? std::nullopt : innermostOpenLoop();
    if (!place || !m_openLoops[*place].carriedResultCount)
        return notes;
    const OpenLoop& loop = m_openLoops[*place];
    const Vector<CarriedResult>& requests = m_carriedResultRequests.find(loop.carriedResultsSummary)->value;
    std::span<const ValueID> parameters = m_code.at(loop.head).parameters().last(loop.carriedResultCount);
    for (uint32_t slot = 0; slot < loop.carriedResultCount; ++slot) {
        if (!requests[slot].isElement || requests[slot].isStruck || !requests[slot].constantPosition || *requests[slot].constantPosition == storedPosition)
            continue;
        Reference parameter = Reference::value(parameters[slot]);
        if (std::optional<uint64_t> noted = findKnown(*this, KnownNote::CarriedElement, array, parameter, 0))
            notes.append({ array, parameter, *noted });
    }
    return notes;
}

void Walker::restoreCarriedElementNotes(std::span<const CarriedElementNote> notes)
{
    for (const CarriedElementNote& note : notes)
        noteKnown(*this, KnownNote::CarriedElement, note.array, note.parameter, 0, LocationMasks(elementGuardHeaps), note.noted);
}

static auto keyOfRecurrence(const Walker::RecurringAccess& access)
{
    return std::tuple { access.loop, access.counterPosition, access.offset };
}

static Walker::ElementRecurrences findElementRecurrences(Walker& walker, const BodyIndex& index)
{
    Walker::ElementRecurrences found;
    if (!index.holdsAnyOf(OpcodeSet { Opcode::ElemSet }) || !index.holdsAnyOf(OpcodeSet { Opcode::ElemGet }))
        return found;
    struct Position {
        ValueID counter;
        int64_t offset { 0 };
    };
    auto positionOf = [&](BlockID head, ValueID value) -> Position {
        static constexpr unsigned maxSteps = 4;
        constexpr int64_t maxOffset = std::numeric_limits<uint16_t>::max();
        Position position;
        for (unsigned step = 0; step < maxSteps && value; ++step) {
            value = index.stripCasts(value);
            if (!value)
                return { };
            if (index.isParameter(value)) {
                position.counter = index.blockOf(value) == head ? value : ValueID();
                return position;
            }
            BodyValue made = index.value(value);
            switch (made.opcode()) {
            case Opcode::ToNumeric:
                break;
            case Opcode::Increment:
                ++position.offset;
                break;
            case Opcode::Decrement:
                --position.offset;
                break;
            case Opcode::Add:
            case Opcode::Sub: {
                ValueID addend = index.stripCasts(made.operand(1));
                if (!addend || index.isParameter(addend) || index.opcodeOf(addend) != Opcode::Integer)
                    return { };
                int64_t distance = index.value(addend).as<Opcode::Integer>().integer();
                if (distance < -maxOffset || distance > maxOffset)
                    return { };
                position.offset += made.opcode() == Opcode::Add ? distance : -distance;
                break;
            }
            default:
                return { };
            }
            value = made.operand(0);
        }
        return { };
    };
    struct Access {
        Position position;
        ValueID id;
    };
    Vector<Access, 8> reads;
    Vector<Access, 4> stores;
    auto constantPositionOf = [&](ValueID key) -> std::optional<int64_t> {
        ValueID written = index.stripCasts(key);
        if (!written || index.isParameter(written) || index.opcodeOf(written) != Opcode::Integer)
            return std::nullopt;
        int64_t position = index.value(written).as<Opcode::Integer>().integer();
        return position >= 0 && position <= std::numeric_limits<uint16_t>::max() ? std::optional(position) : std::nullopt;
    };
    auto addAtConstant = [&](uint32_t loop, const BodyValue& operation) {
        std::optional<int64_t> constant = constantPositionOf(operation.propertyOperand());
        if (constant)
            (operation.opcode() == Opcode::ElemGet ? found.reads : found.stores).append({ loop, Walker::noCounter, static_cast<int32_t>(*constant), operation.id() });
        return constant.has_value();
    };
    for (uint32_t number = 0; number < index.numberOfLoops(); ++number) {
        LoopID loop(number);
        if (index.loop(loop).numberOfLoopsWithin != 1)
            continue;
        BlockID head = index.loop(loop).head;
        reads.shrink(0);
        stores.shrink(0);
        for (BlockID member : index.membersOf(loop)) {
            for (BodyValue operation : index.block(member).values()) {
                if (operation.opcode() != Opcode::ElemGet && operation.opcode() != Opcode::ElemSet)
                    continue;
                if (addAtConstant(number, operation))
                    continue;
                Position position = positionOf(head, operation.propertyOperand());
                if (!position.counter)
                    continue;
                if (operation.opcode() == Opcode::ElemGet)
                    reads.append({ position, operation.id() });
                else
                    stores.append({ position, operation.id() });
            }
        }
        if (reads.isEmpty() || stores.isEmpty())
            continue;
        auto placeOf = [&](ValueID counter) { return counter.index() - index.block(head).firstParameter().index(); };
        for (const Access& read : reads)
            found.reads.append({ number, placeOf(read.position.counter), static_cast<int32_t>(read.position.offset), read.id });
        for (const Access& store : stores) {
            ValueID counter = store.position.counter;
            uint32_t place = placeOf(counter);
            std::optional<int64_t> step;
            bool isStepped = std::ranges::all_of(walker.incomingEdgesOf(index, head), [&](const BodyIncomingEdge& edge) {
                if (!index.isInLoop(edge.from, loop))
                    return true;
                std::span<const ValueID> arguments = edge.target.arguments();
                Position next = place < arguments.size() ? positionOf(head, arguments[place]) : Position();
                return next.counter == counter && next.offset && std::exchange(step, next.offset).value_or(next.offset) == next.offset;
            });
            if (!isStepped || !step)
                continue;
            found.stores.append({ number, place, static_cast<int32_t>(store.position.offset - *step), store.id });
        }
    }
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (index.loopOf(BlockID(number)))
            continue;
        for (BodyValue operation : index.block(BlockID(number)).values()) {
            if (operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet)
                addAtConstant(index.numberOfLoops(), operation);
        }
    }
    std::ranges::sort(found.stores, { }, [](const Walker::RecurringAccess& store) { return store.id.index(); });
    std::ranges::sort(found.reads, { }, [](const Walker::RecurringAccess& read) { return std::pair { keyOfRecurrence(read), read.id.index() }; });
    return found;
}

const Walker::ElementRecurrences& Walker::elementRecurrences()
{
    const BodyIndex& index = currentBodyIndex();
    return m_elementRecurrences.ensure(&index, [&] {
        return findElementRecurrences(*this, index);
    }).iterator->value;
}

std::optional<uint32_t> Walker::openLoopOfElement(ValueID access) const
{
    std::optional<uint32_t> place = innermostOpenLoop();
    if (!place || m_isBuildingInPlace || !has(Mechanism::LoopInvariants) || isUnreachable() || isUnderHandler() || hasStringsInBuilders())
        return std::nullopt;
    const OpenLoop& loop = m_openLoops[*place];
    const BodyIndex& index = currentBodyIndex();
    LoopID loopOfAccess = index.loopOf(index.blockOf(access));
    if (loop.frame == m_frames.size() - 1)
        return loopOfAccess == loop.loop ? place : std::nullopt;
    return loop.frame < m_frames.size() && !loopOfAccess && !program().at(currentFunction()).isCoroutine() ? place : std::nullopt;
}

bool Walker::isCounterOfIteration(const OpenLoop& loop, uint32_t position) const
{
    const BodyIndex& index = currentBodyIndex();
    Reference counter = referenceOf(index.block(index.loop(loop.loop).head).parameter(position));
    return counter && counter.isValue() && m_types.at(typeOf(counter)).isInteger() && m_code.valueBehindNames<valuePreservingOpcodes>(counter.id()) == m_code.at(loop.head).parameters()[position];
}

bool Walker::passesNextCounter(const OpenLoop& loop, uint32_t position, std::span<const Reference> arguments) const
{
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
    if (position >= arguments.size() || !arguments[position] || !arguments[position].isValue())
        return false;
    const Operation& next = m_code.at(m_code.valueBehindNames<valuePreservingOpcodes>(arguments[position].id()));
    return steps.contains(next.opcode) && m_code.valueBehindNames<valuePreservingOpcodes>(next.operand(0)) == m_code.at(loop.head).parameters()[position];
}

Reference Walker::headerArrayOfVariable(FunctionID callee, ValueID read)
{
    const BodyIndex& body = facts().bodies().at(callee);
    ValueID array = body.stripCasts(body.value(read).objectOperand());
    if (array && !body.isParameter(array) && body.opcodeOf(array) == Opcode::CheckInitialized)
        array = body.stripCasts(body.value(array).operand(0));
    if (!array || body.isParameter(array) || body.opcodeOf(array) != Opcode::GetModuleVariable)
        return { };
    VariableID variable = body.value(array).variable();
    if (Reference made = objectOfVariablePutOff(*this, variable))
        return made;
    if (Reference held = m_state.variable(variable))
        return held;
    return valueOfStableVariable(*this, variable, positions().variable(TypeLevel::Proven, variable));
}

void Walker::noteCarriedElements(BlockID head)
{
    const OpenLoop* loop = openLoopOfHeader(head);
    if (!loop || !loop->carriedResultCount || loop->frame != m_frames.size() - 1)
        return;
    const BodyIndex& index = currentBodyIndex();
    const Vector<CarriedResult>& requests = m_carriedResultRequests.find(loop->carriedResultsSummary)->value;
    std::span<const ValueID> parameters = m_code.at(head).parameters().last(loop->carriedResultCount);
    bool hasElements = false;
    for (uint32_t slot = 0; slot < loop->carriedResultCount; ++slot) {
        if (requests[slot].isField() && !requests[slot].isStruck) {
            LocationMasks stores(requests[slot].staticField ? AbstractHeapSet { AbstractHeap::NamedProperties, AbstractHeap::ClassObjects } : AbstractHeapSet { });
            stores.add(program(), MemoryLocation::objectField(requests[slot].fieldKey));
            noteKnown(*this, KnownNote::CarriedField, { }, Reference::value(parameters[slot]), 0, stores, 1);
            hasElements = true;
        }
        if (!requests[slot].isElement || requests[slot].isStruck)
            continue;
        Reference array = requests[slot].copiedCallee ? headerArrayOfVariable(requests[slot].copiedCallee, requests[slot].site) : arrayNameAtHeader(*this, index.loop(loop->loop).head, index.value(requests[slot].site).objectOperand());
        std::optional<uint64_t> guard = array && array.isValue() ? findArrayGuard(*this, m_state, array) : std::nullopt;
        if (!array || !array.isValue())
            continue;
        noteKnown(*this, KnownNote::CarriedElement, array, Reference::value(parameters[slot]), 0, LocationMasks(elementGuardHeaps), guard.value_or(noCarriedElementGuard));
        hasElements = true;
    }
    if (hasElements)
        m_openLoops.changeable(loop - m_openLoops.begin()).stateAtHeader = std::make_shared<const WalkState>(m_state);
}

Reference Walker::buildCarriedElement(const BodyValue& read, Reference array, IterationPurity purity, bool mayBeInt32, NOESCAPE const ScopedLambda<Reference(Reference position, bool isInt32, bool isGuardedAtHeader)>& readInPreheader)
{
    std::optional<uint32_t> place = openLoopOfElement(read.id());
    if (!place || m_code.at(m_block).frequency != BlockFrequency::Normal || !m_openLoops[*place].carriedResultCount || specializedOpenLoop(purity) != place || !isInvariantInLoop(*place, array))
        return { };
    const OpenLoop& loop = m_openLoops[*place];
    Vector<CarriedResult>& requests = m_carriedResultRequests.find(loop.carriedResultsSummary)->value;
    FunctionID copiedCallee = loop.frame == m_frames.size() - 1 ? FunctionID() : currentFunction();
    size_t slot = requests.findIf([&](const CarriedResult& requested) { return requested.isElement && !requested.isStruck && requested.site == read.id() && requested.copiedCallee == copiedCallee; });
    if (slot >= loop.carriedResultCount || loop.carriedSlotsBuilt.contains(static_cast<uint32_t>(slot)))
        return { };
    const Vector<RecurringAccess>& reads = elementRecurrences().reads;
    auto recurrence = std::ranges::find(reads, read.id(), &RecurringAccess::id);
    bool isAtConstant = recurrence != reads.end() && recurrence->counterPosition == noCounter;
    if (recurrence == reads.end() || (!isAtConstant && !isCounterOfIteration(loop, recurrence->counterPosition)))
        return { };
    size_t placeOfParameter = m_code.at(loop.head).parameters().size() - loop.carriedResultCount + slot;
    Reference headerParameter = Reference::value(m_code.at(loop.head).parameters()[placeOfParameter]);
    std::optional<uint64_t> guardAtHeader = loop.stateAtHeader ? findKnown(*this, KnownNote::CarriedElement, array, headerParameter, 0) : std::nullopt;
    if (!guardAtHeader || loop.firstReloadBlock)
        return { };
    bool isGuardedAtHeader = guardAtHeader == findArrayGuard(*this, m_state, array);
    size_t element = loop.carriedElements.findIf([&](const OpenLoop::CarriedElement& each) { return each.slot == slot; });
    if (element != notFound && underlyingCellOf(m_code, loop.carriedElements[element].array.id()) != underlyingCellOf(m_code, array.id()))
        return { };
    auto strike = [&] {
        requests[slot].isStruck = true;
        m_trialVisits.last().requestsCarriedResult = true;
        return Reference();
    };
    if (element != notFound && loop.carriedElements[element].isMissingOnBackEdge)
        return strike();
    std::span<const ValueID> enters = loop.recordsEntering();
    if (enters.size() != m_code.at(loop.head).parameters().size())
        return { };
    Reference entering;
    if (!isAtConstant) {
        ValueID counter = enters[recurrence->counterPosition];
        entering = m_code.isConstant(counter) ? Reference::constant(counter) : Reference::value(counter);
        if (!m_types.at(typeOf(entering)).isInteger())
            return { };
    }

    Representation form = representationOf(headerParameter);
    if (form == Representation::Int32 && !mayBeInt32) {
        requests[slot].type = TypeTable::number;
        requests[slot].form = Representation::Double;
        m_trialVisits.last().requestsCarriedResult = true;
        return { };
    }
    Origin origin = originOf(read);
    origin.isTailCall = false;
    uint32_t counterPosition = recurrence->counterPosition;
    WalkState stateOfIteration = std::exchange(m_state, WalkState(*loop.stateAtHeader));
    Reference position = entering;
    int32_t offset = recurrence->offset;
    if (isAtConstant) {
        Draft constantPosition(Opcode::Integer, origin);
        constantPosition.setInteger(offset);
        position = constant(WTF::move(constantPosition));
    } else if (offset) {
        SpecializedPreheaderScope preheaderScope(*this, true, purity);
        Draft distance(Opcode::Integer, origin);
        distance.setInteger(std::abs(recurrence->offset));
        position = buildIntegerOperator(*this, recurrence->offset < 0 ? Opcode::Sub : Opcode::Add, entering, constant(WTF::move(distance)), origin);
    }
    Reference first = position && position.isRecord() ? readInPreheader(position, form == Representation::Int32, isGuardedAtHeader) : Reference();
    m_state = WTF::move(stateOfIteration);
    if (hasFailed() || isUnreachable())
        return { };
    if (!first || !first.isRecord() || representationOf(first) != form || !m_types.isSubset(typeOf(first), m_code.at(headerParameter.id()).type))
        return strike();
    OpenLoop& changed = m_openLoops.changeable(*place);
    changed.carriedSlotsBuilt.append(static_cast<uint32_t>(slot));
    if (element == notFound)
        changed.carriedElements.append({ static_cast<uint32_t>(slot), counterPosition, offset, array, true, false });
    else
        changed.carriedElements[element].isRead = true;
    auto withElement = std::make_shared<ArgumentsOfEdge>(*changed.entering);
    withElement->m_records[placeOfParameter] = first.id();
    changed.entering = WTF::move(withElement);
    recordSpeculation();
    count(StatisticID::ElementsCarriedAroundLoops);
    return headerParameter;
}

static Walker::ConstantElementReads findConstantElementReads(const Program& program, const BodyIndex& index)
{
    static constexpr OpcodeSet operationsWithoutElementReads = allNumericOperators | allRelations | allEqualities | inputIdentityOpcodes
        | OpcodeSet { Opcode::Integer, Opcode::Double, Opcode::Boolean, Opcode::Undefined, Opcode::Null, Opcode::ElemSet, Opcode::LoopHint, Opcode::GetModuleVariable, Opcode::PutModuleVariable, Opcode::GetEnvironmentField, Opcode::PutEnvironmentField, Opcode::CheckInitialized, Opcode::TypeTest };
    Walker::ConstantElementReads reads;
    for (uint32_t number = 0; number < index.numberOfLoops(); ++number) {
        LoopID loop(number);
        if (index.loop(loop).parent || index.loop(loop).numberOfLoopsWithin != 1)
            continue;
        BlockID head = index.loop(loop).head;
        for (BlockID member : index.membersOf(loop)) {
            for (BodyValue operation : index.block(member).values()) {
                if (operationsWithoutElementReads.contains(operation.opcode()))
                    continue;
                if (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() == program.keyOf(BuiltinNames::length))
                    continue;
                ValueID key = operation.opcode() == Opcode::ElemGet ? index.stripCasts(operation.propertyOperand()) : ValueID();
                if (key && !index.isParameter(key) && index.opcodeOf(key) == Opcode::Integer)
                    reads.atConstants.append({ number, index.value(key).as<Opcode::Integer>().integer(), operation.id().index() });
                else if (!key || !index.isParameter(key) || index.blockOf(key) != head)
                    reads.others.append({ number, operation.id().index() });
            }
        }
    }
    std::ranges::sort(reads.atConstants);
    std::ranges::sort(reads.others);
    return reads;
}

const Walker::ConstantElementReads& Walker::constantElementReads()
{
    const BodyIndex& index = currentBodyIndex();
    return m_constantElementReads.ensure(&index, [&] {
        return findConstantElementReads(program(), index);
    }).iterator->value;
}

std::optional<uint32_t> Walker::loopReadingConstantsAfter(const BodyValue& store)
{
    std::optional<uint32_t> place = m_frames.size() == 1 ? openLoopOfElement(store.id()) : std::nullopt;
    if (!place)
        return std::nullopt;
    LoopID loop = m_openLoops[*place].loop;
    const BodyLoop& record = currentBodyIndex().loop(loop);
    if (record.parent || record.numberOfLoopsWithin != 1)
        return std::nullopt;
    const ConstantElementReads& reads = constantElementReads();
    auto other = std::ranges::upper_bound(reads.others, std::pair { loop.index(), store.id().index() });
    return other != reads.others.end() && other->first == loop.index() ? std::nullopt : place;
}

bool Walker::readsElementAfter(uint32_t openLoop, const BodyValue& store, int64_t position)
{
    const OpenLoop& loop = m_openLoops[openLoop];
    const ConstantElementReads& reads = constantElementReads();
    auto read = std::ranges::upper_bound(reads.atConstants, ConstantElementReads::AtConstant { loop.loop.index(), position, store.id().index() });
    if (read != reads.atConstants.end() && read->loop == loop.loop.index() && read->position == position)
        return true;
    if (!loop.carriedResultCount)
        return false;
    const Vector<CarriedResult>& requests = m_carriedResultRequests.find(loop.carriedResultsSummary)->value;
    return std::ranges::any_of(requests.span().first(loop.carriedResultCount), [&](const CarriedResult& request) {
        return request.isElement && !request.isStruck && request.constantPosition && *request.constantPosition == position;
    });
}

void Walker::carryStoredElement(const BodyValue& store, Reference array, Reference stored)
{
    if (!stored || !stored.isValue() || !array || !array.isValue() || hasFailed() || isUnreachable())
        return;
    const ElementRecurrences& recurrences = elementRecurrences();
    auto recurrence = std::ranges::lower_bound(recurrences.stores, store.id().index(), { }, [](const RecurringAccess& each) { return each.id.index(); });
    if (recurrence == recurrences.stores.end() || recurrence->id != store.id())
        return;
    std::span<const RecurringAccess> reads = std::ranges::equal_range(recurrences.reads, keyOfRecurrence(*recurrence), { }, keyOfRecurrence);
    if (reads.empty())
        return;
    std::optional<uint32_t> place = openLoopOfElement(store.id());
    if (!place || !isInvariantInLoop(*place, array))
        return;
    const OpenLoop& loop = m_openLoops[*place];
    TypeID type = typeOf(stored);
    if (!type || !m_types.at(type).isNumber() || (recurrence->counterPosition != noCounter && !isCounterOfIteration(loop, recurrence->counterPosition)))
        return;
    const BodyIndex& index = currentBodyIndex();
    FunctionID copiedCallee = loop.frame == m_frames.size() - 1 ? FunctionID() : currentFunction();
    BlockID inputHead = m_frames[loop.frame].index->loop(loop.loop).head;
    ValueID root = underlyingCellOf(m_code, array.id());
    auto found = std::ranges::find_if(reads, [&](const RecurringAccess& each) {
        if (each.id.index() > store.id().index() && index.dominates(index.blockOf(store.id()), index.blockOf(each.id)))
            return false;
        ValueID object = index.value(each.id).objectOperand();
        Reference read = copiedCallee ? Reference() : arrayNameAtHeader(*this, inputHead, object);
        if (!read)
            read = referenceOf(object);
        return read && read.isValue() && underlyingCellOf(m_code, read.id()) == root;
    });
    if (found == reads.end())
        return;
    ValueID recurringRead = found->id;

    bool isInt32 = m_types.isSubset(type, TypeTable::int32);
    auto isForRead = [&](const CarriedResult& requested) { return requested.isElement && requested.site == recurringRead && requested.copiedCallee == copiedCallee; };
    Representation form = isInt32 ? Representation::Int32 : Representation::Double;
    auto request = [&] {
        if (m_trialVisits.isEmpty() || m_trialVisits.last().frameIndex != loop.frame || m_trialVisits.last().inputHead != inputHead)
            return;
        Vector<CarriedResult>& requests = m_carriedResultRequests.add(m_trialVisits.last().summaryIndex, Vector<CarriedResult> { }).iterator->value;
        CarriedResult requested { recurringRead, TypeID(isInt32 ? TypeTable::int32 : TypeTable::number), true, false, form };
        requested.copiedCallee = copiedCallee;
        if (recurrence->counterPosition == noCounter)
            requested.constantPosition = recurrence->offset;
        size_t held = requests.findIf(isForRead);
        if (held == notFound)
            requests.append(requested);
        else if (!requests[held].isStruck && !isInt32 && requests[held].form == Representation::Int32)
            requests[held] = requested;
        else
            return;
        m_trialVisits.last().requestsCarriedResult = true;
    };
    size_t slot = loop.carriedResultCount ? m_carriedResultRequests.find(loop.carriedResultsSummary)->value.findIf(isForRead) : notFound;
    if (slot >= loop.carriedResultCount)
        return request();
    ValueID parameter = m_code.at(loop.head).parameters().last(loop.carriedResultCount)[slot];
    if (!m_types.isSubset(type, m_code.at(parameter).type))
        return request();
    size_t element = loop.carriedElements.findIf([&](const OpenLoop::CarriedElement& each) { return each.slot == slot; });
    if (element == notFound) {
        element = loop.carriedElements.size();
        m_openLoops.changeable(*place).carriedElements.append({ static_cast<uint32_t>(slot), recurrence->counterPosition, recurrence->offset, array, false, false });
    }
    Reference known = m_openLoops[*place].carriedElements[element].array;
    if (underlyingCellOf(m_code, known.id()) == root)
        noteKnown(*this, KnownNote::CarriedElement, known, Reference::value(parameter), 1, LocationMasks(elementGuardHeaps), stored.bits());
}

Reference Walker::buildOnFirstArrival(ValueID site, const Draft& call, const Effects& effects)
{
    return buildOnFirstArrival(site, resultTypeOf(call), call.origin(), scopedLambda<Reference()>([&] {
        return build(Draft(call), &effects);
    }));
}

Reference Walker::buildOnFirstArrival(ValueID site, TypeID resultType, Origin origin, NOESCAPE const ScopedLambda<Reference()>& buildResult)
{
    std::optional<uint32_t> place = innermostOpenLoop();
    if (!place || m_isBuildingInPlace || !has(Mechanism::LoopInvariants) || isUnreachable() || isUnderHandler() || hasStringsInBuilders())
        return { };
    const OpenLoop& loop = m_openLoops[*place];
    if (loop.frame != m_frames.size() - 1 || m_code.at(m_block).frequency != BlockFrequency::Normal)
        return { };
    if (const BodyIndex& index = currentBodyIndex(); index.loopOf(index.blockOf(site)) != loop.loop)
        return { };
    if (!resultType || isNeverType(resultType) || !m_types.at(resultType).isJavaScriptValue() || (m_types.at(resultType).primitives & Primitive::Absent))
        return { };
    auto isForSite = [&](const CarriedResult& requested) { return requested.site == site && !requested.copiedCallee; };
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
    if (loop.carriedSlotsBuilt.contains(static_cast<uint32_t>(slot)))
        return { };
    m_openLoops.changeable(*place).carriedSlotsBuilt.append(static_cast<uint32_t>(slot));
    Reference headerParameter = Reference::value(parameters[parameters.size() - m_openLoops[*place].carriedResultCount + slot]);
    Reference carried = carriedResultOf(m_state, headerParameter);
    if (!(m_types.at(ownTypeOf(carried)).primitives & Primitive::Absent))
        return carried;

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
    Reference computed = buildResult();
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
    noteKnown(*this, KnownNote::CarriedResult, headerParameter, { }, 0, { }, result.bits());
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

static constexpr OpcodeSet opcodesOfUnseenWork {
    Opcode::LoopHint, Opcode::ReserveArrayCapacity, Opcode::ReserveTableCapacity, Opcode::InitializeBuiltin,
    Opcode::StackStringBuilderBegin, Opcode::StackStringBuilderToString, Opcode::StackStringBuilderFinish, Opcode::StackStringBuilderDiscard,
    Opcode::StackStringBuilderAppendConstant, Opcode::StackStringBuilderAppendInteger, Opcode::StackStringBuilderAppendNumber, Opcode::StackStringBuilderAppendString,
    Opcode::StackStringBuilderAppendStringRange, Opcode::StackStringBuilderAppendBoolean, Opcode::StackStringBuilderAppendCodeUnit, Opcode::StackStringBuilderAppendStringOrNullish,
    Opcode::StackStringBuilderAppendValue
};

static bool doesVisibleWork(const Operation& operation)
{
    EffectTraits traits = operation.traits();
    return !opcodesOfUnseenWork.contains(operation.opcode) && (traits.mayWriteObservableState() || traits.mayRunOtherCode() || traits.mayBeTerminated());
}

bool Walker::hasVisibleWork(uint32_t loopIndex)
{
    const OpenLoop& loop = m_openLoops[loopIndex];
    if (loop.hasVisibleWork)
        return true;
    uint32_t end = m_code.numberOfValues();
    bool isFound = false;
    for (uint32_t index = std::max(loop.firstRecord, loop.visibleWorkScanEnd); index < end && !isFound; ++index) {
        if (m_code.isConstant(ValueID(index)))
            continue;
        const Operation& operation = m_code.at(ValueID(index));
        if (!doesVisibleWork(operation))
            continue;
        BlockID block = m_code.blockOf(ValueID(index));
        uint32_t depth = block && block.index() < m_preheaderDepths.size() ? m_preheaderDepths[block.index()] : 0;
        isFound = !depth || depth - 1 > loopIndex;
    }
    OpenLoop& scanned = m_openLoops.changeable(loopIndex);
    scanned.hasVisibleWork = isFound;
    scanned.visibleWorkScanEnd = end;
    return isFound;
}

static bool mayDoVisibleWork(const Effects& effects)
{
    return !effects.writes.containsOnly(unobservableWriteHeaps) || effects.mayRunOtherCode();
}

std::optional<uint32_t> Walker::outermostLoopWithoutVisibleWork(uint32_t outermost)
{
    std::optional<uint32_t> found;
    forEachOpenLoopAround(std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft), 0, [&](uint32_t index) {
        Vector<ArrivalCondition, 4> conditions;
        if (index < outermost || hasVisibleWork(index) || (mayDoVisibleWork(m_openLoops[index].effects) && !findFirstArrivalConditions(index, conditions)))
            return IterationStatus::Done;
        found = index;
        return IterationStatus::Continue;
    });
    return found;
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
    if (isOwnVersion && exited->headAsksLesserBound) {
        BodyTerminator end = m_frames[loop.frame].index->block(exited->inputHead).terminator();
        Reference test = end.kind() == TerminatorKind::Branch ? referenceBoundIn(m_frames[loop.frame], end.as<TerminatorKind::Branch>().conditionOperand()) : Reference();
        if (test && test.isValue())
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

std::optional<uint32_t> Walker::outermostLoopForBuiltinPreheaderRead()
{
    static constexpr OpcodeSet visibleBeforeFirstRead { Opcode::InitializeBuiltin, Opcode::CallBuiltin };
    std::optional<uint32_t> specialized = specializedOpenLoop(IterationPurity::KnownEffectsOnly);
    if (!specialized || m_openLoops[*specialized].hasBuiltinRead)
        return std::nullopt;
    const OpenLoop& loop = m_openLoops[*specialized];
    uint32_t end = m_code.numberOfValues();
    bool isFound = false;
    for (uint32_t index = std::max(loop.firstRecord, loop.builtinReadScanEnd); index < end && !isFound; ++index) {
        if (m_code.isConstant(ValueID(index)))
            continue;
        const Operation& operation = m_code.at(ValueID(index));
        bool listsNames = operation.opcode == Opcode::CallInline && operation.inlineBuiltin() == InlineBuiltin::OwnPropertyKeys;
        if (!listsNames && !visibleBeforeFirstRead.contains(operation.opcode))
            continue;
        BlockID block = m_code.blockOf(ValueID(index));
        uint32_t depth = block && block.index() < m_preheaderDepths.size() ? m_preheaderDepths[block.index()] : 0;
        isFound = block && (!depth || depth - 1 > *specialized);
    }
    for (uint32_t number = loop.head.index(); number < m_code.numberOfBlocks() && !isFound; ++number) {
        const Block& block = m_code.at(BlockID(number));
        isFound = block.hasEnded() && block.terminator.kind == TerminatorKind::TryCallInline && block.terminator.inlineBuiltin() == InlineBuiltin::OwnPropertyKeys;
    }
    OpenLoop& scanned = m_openLoops.changeable(*specialized);
    scanned.hasBuiltinRead = isFound;
    scanned.builtinReadScanEnd = end;
    if (isFound)
        return std::nullopt;
    Effects ofFirstRead = Effects::none();
    ofFirstRead.mayCollect = true;
    std::optional<uint32_t> outermost = outermostLoopToLookUpInFrontOf({ }, ofFirstRead);
    return outermost == specialized ? outermost : std::nullopt;
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
    if (boundToOf(draft.opcode) == BoundTo::Site && (record.position != draft.position || code.inlineFrames()[record.inlineFrame].callee != code.inlineFrames()[draft.inlineFrame].callee))
        return false;
    if (record.opcode == Opcode::FlattenString && !record.isReadOperands().empty() && id.index() < firstRecordOfLoop)
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
