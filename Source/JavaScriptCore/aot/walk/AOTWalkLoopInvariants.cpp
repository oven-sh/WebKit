/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// LoopInvariants: the block in front of a loop stays open until the walk has passed the loop. What a turn builds out of what stands in front of the loop, and what gives the same in every turn, is appended there.
// At a suspension: nothing leaves a loop that may suspend, where a frame would save it at every turn. At a throw: what leaves a loop neither throws nor writes, so it may run where no turn follows.
// Withheld, or Folding withheld: the block in front of a loop ends where the loop is entered, and each turn builds anew.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include <algorithm>
#include <optional>
#include <wtf/IterationStatus.h>

namespace JSC { namespace AOT { namespace Typed {

// The shape word is written where the cell is made and never again, so a test of it answers the same for as long as the value lives.
static constexpr AbstractHeapSet heapsWrittenOnceForLoopInvariants { AbstractHeap::ShapeWords };

// ---- Reads at the head

// The head is walked whole behind what stands in front of it, so a turn is spared only a read whose result the state keeps.
static constexpr OpcodeSet readsKeptByStateInLoopInvariants { Opcode::GetModuleVariable, Opcode::GetEnvironmentField };

static bool findsSameInEveryTurnForLoopInvariants(const Program& program, const BodyValue& operation, const Effects& effectsOfLoop)
{
    Opcode opcode = operation.opcode();
    if (!readsKeptByStateInLoopInvariants.contains(opcode))
        return false;
    Effects effects = declaredEffectsOfOperation(program, opcode, operation);
    return effects.isUnobservable() && !effects.reads.overlaps(effectsOfLoop.writes);
}

// The first turn is sure to come as far as the first operation of the head that lets something else run, so what is read up to there is read where the program reads it.
// Each read goes through its chain in front of the head, which leaves what it found in the state. No turn ends that, so in the loop the read is answered from there.
void hoistInvariants(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering)
{
    if (!walker.has(Mechanism::LoopInvariants) || !walker.has(Mechanism::Folding) || preview.effects.mayRunOtherCode())
        return;

    // A guess picks the end of its own stretch, which may lie behind operations that take the parameters of the head. The head binds anew what is bound here.
    Walker::ClosedArm withoutGuesses(walker);
    BodyBlock head = walker.currentBodyIndex().block(inputHead);
    // What throws here throws in front of the first turn: its handler is handed what enters.
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

// ---- What a look at a loop counts on

static constexpr OpcodeSet readsByNameForLoopInvariants { Opcode::PropGet, Opcode::PropHas };
static constexpr OpcodeSet storesByNameForLoopInvariants { Opcode::PropSet, Opcode::PropDefine };
static constexpr OpcodeSet accessesByKeyForLoopInvariants { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas };

// In the place of a read by name stands a constant, a read of a slot or of a length, or the read of a method, which finds a value above the receiver: the run time may make the closure of a member then, and files what it finds.
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
    // The look that such a form wants stands in front of it, and where it fails the turn begins again in the closed copy.
    // Not the form on an object as made: `while (view.length === 4)`: with turns that run no other code the length would leave the loop, and another thread's grow() is no effect of a turn.
    bool isBuiltBehindLooks = !isByKey && walker.looksTakeFormsBehindLooksAsBuilt() && typedForm != TypedForm::None && typedForm != TypedForm::BuiltAsCall && typedForm != TypedForm::BuiltOnObjectAsMade;
    if (typedForm != TypedForm::Built && !isBuiltBehindLooks)
        return std::nullopt;
    Walker::FormAskedAhead form = walker.takeFormAskedAhead();
    // `gauge.level`, an accessor above an object that is out of sight: buildAccessToAccessorBehindLooksForTypedAccess().
    if (form.opcode == Opcode::CallDirect) {
        if (halfRunBehindLooks)
            *halfRunBehindLooks = form.function;
        return std::nullopt;
    }

    // At the level Expected the operation tests for itself, and does the closed operation where the test fails.
    if (isByKey)
        return form.level == KnowledgeLevel::Proven ? std::optional<Effects>(declaredBoundOf(form.opcode)) : std::nullopt;
    if (readsByNameForLoopInvariants.contains(opcode)) {
        Effects effects = effectsOfTypedReadByNameForLoopInvariants();
        // `process.nextTick`: its holder makes the member at the first read, which the program may see: buildReadOfBuiltinMember().
        // `tools.read`, a closure of a literal that is made at its first read as a value: what either read of such a slot declares.
        if (form.opcode == Opcode::GetDeferredClosureSlotOrUndefined) {
            for (Opcode read : { Opcode::GetDeferredClosureSlot, Opcode::GetDeferredClosureSlotOrUndefined }) {
                Draft built(read);
                built.setKey(operation.key());
                effects.merge(declaredEffectsOfOperation(walker.program(), read, built));
            }
        }
        // `process.env`: the instance's cell is filled at the first read, by the host's own code.
        if (form.opcode == Opcode::GetWatchedBuiltinValue)
            effects.merge(declaredBoundOf(Opcode::GetWatchedBuiltinValue));
        if (form.opcode == Opcode::InitializeBuiltin)
            effects.merge(declaredBoundOf(Opcode::InitializeBuiltin));
        return effects;
    }

    // PutSlot writes no more than PutSlotOrAdd.
    Draft store(Opcode::PutSlotOrAdd);
    store.setKey(operation.key());
    static_assert(declaredBoundOf(Opcode::PutSlot).writes.isSubsetOf(declaredBoundOf(Opcode::PutSlotOrAdd).writes));
    return declaredEffectsOfOperation(walker.program(), Opcode::PutSlotOrAdd, store);
}

// ---- The block in front of a loop

// The rows of the steps of an iteration say `suspends` for `for await`. A step of any other iteration calls and comes back.
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
    frame.placesOfOpenLoops[loop.loop.index()] = place;
    if (loop.blockInFront) {
        while (m_placesOfLoopsBehindBlocks.size() <= loop.blockInFront.index())
            m_placesOfLoopsBehindBlocks.append(0);
        RELEASE_ASSERT(!m_placesOfLoopsBehindBlocks[loop.blockInFront.index()]);
        m_placesOfLoopsBehindBlocks[loop.blockInFront.index()] = place;
    }
    m_openLoops.append(WTF::move(loop));
}

void Walker::closeNewestLoop()
{
    const OpenLoop& loop = m_openLoops.last();
    settleFateOfPoll(loop);
    // A failure may have taken the frame away.
    if (loop.frame < m_frames.size() && !m_frames[loop.frame].placesOfOpenLoops.isEmpty())
        m_frames[loop.frame].placesOfOpenLoops[loop.loop.index()] = 0;
    if (loop.blockInFront)
        m_placesOfLoopsBehindBlocks[loop.blockInFront.index()] = 0;
    for (BlockID earlier : loop.earlierBlocksInFront)
        m_placesOfLoopsBehindBlocks[earlier.index()] = 0;
    m_openLoops.removeLast();
}

// A loop is opened behind those around it, and the loops of a callee behind those around its call.
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

// As endWithJumpInto(), but for the jump: endBlocksInFrontOfLoops() builds that.
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
        endWithJumpInto(head, entering);
        return;
    }

    for (size_t index = 0; index < entering.size(); ++index) {
        Representation form = m_code.at(parameters[index]).representation;
        Reference argument = isSameWord(representationOf(entering[index]), form) ? entering[index] : convert(entering[index], form, m_origin);
        if (!argument || isUnreachable()) {
            fail("an edge hands on a value that nothing was built for"_s);
            return;
        }
        loop.entering.append(argument.id());
    }
    loop.blockInFront = m_block;
    openLoop(WTF::move(loop));
    m_block = { };
}

// `next`: the block of the body of the frame to which the walk comes. Null: it has passed the last one.
// A loop behind an exit of another may stand in the stream in front of the last block of that one. Then the block in front of the older loop ends with that of the younger.
void Walker::endBlocksInFrontOfLoops(size_t frameIndex, BlockID next)
{
    while (!m_openLoops.isEmpty()) {
        const OpenLoop& loop = m_openLoops.last();
        if (loop.frame < frameIndex)
            return;
        if (loop.frame == frameIndex && next && next.index() <= m_frames[frameIndex].index->loop(loop.loop).last.index())
            return;
        // A failure ends the block in which it finds the walk.
        if (loop.blockInFront && !m_code.at(loop.blockInFront).hasEnded()) {
            m_code.end(loop.blockInFront, TerminatorDraft(TerminatorKind::Jump, loop.origin), 1);
            m_code.closeEdge(m_code.at(loop.blockInFront).terminator.next(), loop.head, loop.entering.span());
        }
        closeNewestLoop();
    }
}

void Walker::endBlocksInFrontOfLoopsFrom(size_t numberOfLoopsKept)
{
    while (m_openLoops.size() > numberOfLoopsKept) {
        const OpenLoop& loop = m_openLoops.last();
        if (loop.blockInFront && !m_code.at(loop.blockInFront).hasEnded()) {
            m_code.end(loop.blockInFront, TerminatorDraft(TerminatorKind::Jump, loop.origin), 1);
            m_code.closeEdge(m_code.at(loop.blockInFront).terminator.next(), loop.head, loop.entering.span());
        }
        closeNewestLoop();
    }
}

// ---- What leaves a loop

// Whatever is older than the parameters of the head and is named in the loop dominates the loop, so it dominates the one block that enters it.
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

// It gives the same wherever it stands behind its operands, as long as nothing writes what it reads, and nobody can tell whether it has run.
static bool mayLeaveSomeLoopForLoopInvariants(const TypeArena& types, const Draft& draft, const Effects& effects, bool proofsHoldEverywhere)
{
    Opcode opcode = draft.opcode;
    if (resultRuleOf(opcode) == ResultRule::None || resultRuleOf(opcode) == ResultRule::Allocation || interpretedOf(opcode) == Interpretation::Allocation)
        return false;
    // A Narrowed stands where its test has passed. A part of several results says something only beside the others, and makes no code.
    // It loads the element in the form of the kind that it names, and the test that has found the kind may stand in the loop.
    if (opcode == Opcode::IsElementPresent)
        return false;
    if (opcode == Opcode::Narrowed || opcode == Opcode::ResultAt || boundToOf(opcode) != BoundTo::Nothing)
        return false;
    if (!effects.writes.isEmpty() || effects.mayThrow || !effects.alwaysCompletes() || effects.reads.containsAny(bookkeepingHeaps))
        return false;
    // A test of a type or of a shape that has passed has a name, the Narrowed, and whatever leans on it takes that name. A look at a slot that has found it filled has none.
    if (!proofsHoldEverywhere && (reliesOf(opcode).contains(Proof::SlotFilled) || reliesOf(opcode).contains(Proof::InBounds)))
        return false;

    // A conversion that makes no code is another name of its operand. It leaves with it, or whatever takes it would stay: the length of a view, the storage of a list.
    if (conversionsOfForm.contains(opcode))
        return true;
    // What looks at no more than the branch on its answer would only holds a register all round the loop.
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

// Hoisted code runs once per entry into the loop, and most loops run few iterations: off a conditional path it costs more than it saves.
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

// The outermost of the loops around the place where the draft is built that it may stand in front of. The loops from `m_numberOfLoopsThatMayBeLeft` on do not stand around that place.
std::optional<uint32_t> Walker::findLoopToBuildInFrontOf(const Draft& draft, const Effects& effects) const
{
    uint32_t numberOfLoops = std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft);
    if (!numberOfLoops || !mayLeaveSomeLoopForLoopInvariants(m_types, draft, effects, m_numberOfScopesWhereProofsHoldEverywhere))
        return std::nullopt;
    // `for (..) { total += flag ? text.length : text.length + 1; total += text.length; }`: the third read follows its store because a check further up on its way has passed: the state says so, and the check stands in the loop.
    // The read has no check of its own and is typed without the empty word. In front of the loop nothing has been asked yet, and a loop may have no turn. As a look at a slot, such a proof has no name that the read could take.
    if (draft.opcode == Opcode::GetModuleVariable && draft.followsItsStore())
        return std::nullopt;
    // What Link says of a slot of a scope holds wherever the function runs, and whoever has the scope at hand in front of the loop got it behind the store.
    if (draft.opcode == Opcode::GetEnvironmentField && draft.followsItsStore()
        && (unit().role == CodeRole::SlowBody || !facts().linked().orderOfScopes.isFilledWhenever(currentFunction(), FieldReference { draft.layout(), draft.field() })))
        return std::nullopt;
    if (draft.opcode == Opcode::GetModuleVariable && isVariableFilledAtFirstRead(plan(), draft.variable()))
        return std::nullopt;
    // What the state knows of an operand beyond its own type may have been learned in the loop.
    for (Reference operand : draft.operands()) {
        if (operand && (!operand.isRecord() || typeOf(operand) != ownTypeOf(operand)))
            return std::nullopt;
    }

    LocationMasks reads = effects.reads.without(heapsWrittenOnceForLoopInvariants);
    HoistPoint point = currentHoistPoint();
    std::optional<uint32_t> outermost;
    forEachOpenLoopAround(numberOfLoops, 0, [&](uint32_t index) {
        const OpenLoop& loop = m_openLoops[index];
        if (!loop.blockInFront || (!reads.isEmpty() && (loop.effects.mayRunOtherCode() || reads.overlaps(loop.effects.writes))))
            return IterationStatus::Done;
        // Under a handler whatever may collect may throw, and the state of the handler is met with what holds where the walk stands.
        if (effects.mayCollect && (!loop.effects.mayCollect || m_code.at(loop.blockInFront).handler))
            return IterationStatus::Done;
        bool operandsStandInFront = std::ranges::all_of(draft.operands(), [&](Reference operand) {
            return !operand || standsInFrontOfLoop(index, operand);
        });
        if (!operandsStandInFront)
            return IterationStatus::Done;
        // One call through a resolved target saves more than the resolution costs.
        if (draft.opcode != Opcode::ResolveCallTarget && !executesOnEveryIteration(index, point))
            return IterationStatus::Done;
        outermost = index;
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
    SetForScope blockInFront(m_block, inFront);
    SetForScope loopsThatMayBeLeft(m_numberOfLoopsThatMayBeLeft, indexOfLoop);
    return build(WTF::move(draft), foundByCaller);
}

// The head of a loop has struck from the state what the loop writes, so what lies within the effects of the loop ends nothing that holds in it.
std::optional<uint32_t> Walker::outermostLoopToMakeTextInFrontOf(std::span<const Reference> operands, const Effects& ofText) const
{
    if (isUnreachable() || isUnderHandler() || m_numberOfCopiesBuiltForSize)
        return std::nullopt;
    // What the state knows of an operand beyond its own type may have been learned in the loop.
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

// The state at the head of a loop has kept, and the block in front of it holds, whatever the look at the loop has seen no turn end.
// A look that counts on no other code has been promised the typed form of every access of the loop. What a callee does it has from Link, which goes by rules of its own.
bool Walker::looksAtLoopsAroundHaveSeen(const Effects& effects) const
{
    LocationMasks writes = effects.writes.without(heapsWhoseWritesNobodySees);
    if (writes.isEmpty() && !effects.mayRunOtherCode())
        return true;
    bool haveSeen = true;
    forEachOpenLoopAround(std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft), 0, [&](uint32_t index) {
        const OpenLoop& loop = m_openLoops[index];
        if (loop.effects.mayRunOtherCode())
            return IterationStatus::Continue;
        if (effects.mayRunOtherCode() || !writes.isSubsetOf(loop.effects.writes)) {
            // The walk stands in the copy of a callee, and the loop around the call. The look has taken what the call does from Link.
            const LinkTables& linked = facts().linked();
            if (loop.frame + 1 < m_frames.size() && m_frames[loop.frame + 1].function.index() < linked.functions.size()) {
                const LinkedFunction& copied = linked.function(m_frames[loop.frame + 1].function);
                Effects ofCopied = copied.effectsOnReturn;
                ofCopied.merge(copied.effectsOnThrow);
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

// The operands of the draft are in the forms in which it takes them, and its own form is chosen.
ValueID Walker::findSameInFrontOfLoop(uint32_t indexOfLoop, const Draft& draft) const
{
    auto isSame = [&](ValueID id) {
        const Operation& record = m_code.at(id);
        if (record.opcode != draft.opcode || record.representation != draft.representation || record.flags != draft.flags || !std::ranges::equal(record.payload(), draft.payload()))
            return false;
        return std::ranges::equal(record.operands(), draft.operands(), [](ValueID held, Reference taken) {
            return taken ? held == taken.id() : !held;
        });
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

// What hashOfRecordInFrontOfLoop() reads of a draft of one: it has no flag and no word of its own.
struct ConversionSoughtInFrontOfLoop {
    std::span<const uint32_t> payload() const { return { }; }
    std::span<const Reference> operands() const { return { &operand, 1 }; }

    Opcode opcode;
    Representation representation;
    uint8_t flags { 0 };
    Reference operand;
};

// It has no effect, so the looks at the loops around have nothing to count, and of all that findLoopToBuildInFrontOf() asks there is left: what is known of the operand, and where it stands.
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
    // What the state knows of the operand beyond its own type may have been learned in the loop.
    if (numberOfLoopsAround && proven == ownTypeOf(operand)) {
        HoistPoint point = currentHoistPoint();
        forEachOpenLoopAround(numberOfLoopsAround, 0, [&](uint32_t index) {
            const OpenLoop& loop = m_openLoops[index];
            if (!loop.blockInFront || !standsInFrontOfLoop(index, operand) || !executesOnEveryIteration(index, point))
                return IterationStatus::Done;
            point = hoistPointInFrontOf(index);
            block = loop.blockInFront;
            numberOfLoopsAround = index;
            return IterationStatus::Continue;
        });
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
    OpenLoop& loop = m_openLoops[indexOfLoop];
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
