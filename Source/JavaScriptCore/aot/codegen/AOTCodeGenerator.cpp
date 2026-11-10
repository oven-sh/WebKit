/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTAccessEmitter.h"
#include "AOTArrayStorageForms.h"
#include "AOTCodeRecords.h"
#include "AOTConstantEmitter.h"
#include "AOTControlEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTEffectsNet.h"
#include "AOTEffectsOfOperations.h"
#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTEntryEmitter.h"
#include "AOTHintEmitter.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSavedFrameEmitter.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTStringEmitter.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeIDs.h"
#include <algorithm>
#include <type_traits>
#include <wtf/BitVector.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned longestWayUpSinglePredecessors = 8;
static constexpr unsigned longestWayUpDominators = 32;
static constexpr BlockID enteredOtherwise = BlockID(BlockID::hashTableDeletedIndex);

template<typename T>
static std::span<T> allocateSpan(Machine::Arena& arena, size_t count)
{
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
    return unsafeMakeSpan(static_cast<T*>(arena.allocate(count * sizeof(T), alignof(T))), count);
}

template<typename T>
static std::span<T> allocateZeroedSpan(Machine::Arena& arena, size_t count)
{
    std::span<T> span = allocateSpan<T>(arena, count);
    zeroSpan(span);
    return span;
}

static_assert(BlockID().bits() == UINT32_MAX);
template<typename T>
static std::span<T> allocateSpanOfAllBitsSet(Machine::Arena& arena, size_t count)
{
    std::span<T> span = allocateSpan<T>(arena, count);
    memsetSpan(span, 0xFF);
    return span;
}

static Machine::EdgeClass edgeClassLeadingTo(BlockFrequency frequency)
{
    switch (frequency) {
    case BlockFrequency::Normal:
        return Machine::EdgeClass::Normal;
    case BlockFrequency::Unlikely:
        return Machine::EdgeClass::Unlikely;
    case BlockFrequency::Rare:
        return Machine::EdgeClass::Rare;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOfGeneratedOperation(const OperationContext& context)
{
    if (context.valueID())
        return nameOf(static_cast<const ValueContext&>(context).opcode());
    return nameOf(static_cast<const TerminatorContext&>(context).kind());
}

static String describeFormsOfGeneratedOperand(Representation found, Representation wanted)
{
    return makeString("an operand kept as "_s, nameOf(found), " is wanted as "_s, nameOf(wanted));
}

GenerationResult CodeGenerator::run()
{
    RELEASE_ASSERT(code().isSwept() && !code().entries().empty());
    if (!recordSitesWithoutCode())
        return GenerationResult::Failed;

    m_values = allocateZeroedSpan<LValue>(m_proc.arena(), code().numberOfValues());
    m_looksBehindEachOperation = m_checksAllocation || code().someBarrierIsCountedOn();
    m_storesBehindWhichOwnerIsRemembered.clearAll();
    orderBlocks();
    createBlocks();
    followNumbersOutOfBytes();
    createPhis();
    if (unit().role == CodeRole::CoroutineBody && code().hasSuspensions()) {
        m_keepsValuesInHeapFrame = emitter<SavedFrameEmitter>().layOutHeapFrame();
        m_looksBehindEachOperation |= m_keepsValuesInHeapFrame;
    }

    definePinnedValues(m_out, m_pinned);
    emitter<EntryEmitter>().generateEntries();
    for (size_t position = 0; position < m_order.size(); ++position) {
        if (m_hasFailed)
            return GenerationResult::Failed;
        BlockID id = m_order[position];
        m_positionInOrder = position;
        if (leavesOutBlockThatNothingLeadsTo(id))
            continue;
        const Block& block = code().at(id);
        if (block.isLoopHead && (!m_keepsValuesInHeapFrame || emitter<SavedFrameEmitter>().liesInLoopInsideStretch(id))) {
            std::optional<CountedLoop> loop = findCountedLoopAt(position);
            if (loop && m_keepsValuesInHeapFrame) {
                for (BlockID inTurn : loop->turn) {
                    if (!emitter<SavedFrameEmitter>().liesInLoopInsideStretch(inTurn)) {
                        loop = std::nullopt;
                        break;
                    }
                }
            }
            if (loop && loop->storesIntoSlots)
                build().statistics().count(StatisticID::UnrolledLoopsWithSlotStores);
            if (loop && !loop->viewsAskedForKind.isEmpty())
                layOutTurnsForEachKindOfView(*loop);
            else if (loop) {
                auto layOutForEveryKind = [&](CountedLoop& loop) {
                    loop.turnsForWideTextsFollow = asksTextsOfLoopForWidth(loop);
                    if (!loop.mayComeBack && !loop.secondTurnsGoByKindsRead && !loop.turnsForWideTextsFollow && loop.asksListsForAnotherKind())
                        m_layoutsInRow = LayoutsInRow { loop.asksListsReadAndListsStoredInto ? 4u : 2u, nullptr, { } };
                    else if (!loop.mayComeBack && !loop.secondTurnsGoByKindsRead && loop.turnsForWideTextsFollow && loop.listsAskedForKind.isEmpty())
                        m_layoutsInRow = LayoutsInRow { 2u, nullptr, { } };
                    layOutTurns(loop);
                    if (std::exchange(loop.turnsForWideTextsFollow, false) && !m_hasFailed) {
                        loop.asksTextsForWide = true;
                        layOutTurns(loop);
                    }
                    if (loop.asksListsForAnotherKind() && !m_hasFailed)
                        layOutTurnsForOtherKindOfStorage(loop);
                    if (loop.asksListsReadAndListsStoredInto && !m_hasFailed)
                        layOutTurnsForOtherKindOfStorage(loop, false);
                    if (loop.asksListsReadAndListsStoredInto && !m_hasFailed)
                        layOutTurnsForOtherKindOfStorage(loop);
                    RELEASE_ASSERT(m_hasFailed || !m_ringOfLoopLaidOut);
                    RELEASE_ASSERT(m_hasFailed || !m_layoutsInRow);
                };
                if (std::ranges::any_of(loop->listsAskedForKind, &ListAskedForKind::otherKindsAreNotAsked)) {
                    const Edge& back = code().at(code().at(loop->turn.last()).terminator.next());
                    auto findSumsAnew = [&](CountedLoop& changed) {
                        changed.sumsOfIntegers.shrink(0);
                        changed.sumsAreTakenOverBehindStretches = false;
                        findSumsOfIntegers(changed, back);
                    };
                    bool onlyTruthIsAsked = std::ranges::all_of(loop->turn, [&](BlockID inTurn) {
                        std::span<const ValueID> values = code().valuesOf(inTurn);
                        return std::ranges::all_of(values, [&](ValueID read) {
                            const Operation& operation = code().at(read);
                            if (operation.opcode != Opcode::GetArrayElement || !std::ranges::any_of(loop->listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.otherKindsAreNotAsked && asked.list == operation.arrayOperand(); }))
                                return true;
                            ValueID test = code().at(inTurn).terminator.kind == TerminatorKind::Branch ? testReadByBranchAlone(inTurn, code().at(inTurn).terminator) : ValueID();
                            return test && values.size() >= 2 && values.back() == test && values[values.size() - 2] == read && opcodeOf(test) == Opcode::ToBoolean && code().at(test).form() == OperatorForm::Generic && code().at(test).valueOperand() == read && code().useCountOf(read) == 1;
                        });
                    });
                    if (onlyTruthIsAsked) {
                        CountedLoop forWholeNumbers = *loop;
                        for (ListAskedForKind& asked : forWholeNumbers.listsAskedForKind) {
                            if (asked.otherKindsAreNotAsked)
                                asked.kind = ArrayStorage::Int32;
                        }
                        forWholeNumbers.mayComeBack = false;
                        forWholeNumbers.boundsAskedInFront.shrink(0);
                        findSumsAnew(forWholeNumbers);
                        build().statistics().count(StatisticID::UnrolledLoopsForInt32Truthiness);
                        layOutForEveryKind(forWholeNumbers);
                    }
                    CountedLoop forDoubles = *loop;
                    forDoubles.mayComeBack = loop->mayComeBack && loop->boundsAskedInFront.isEmpty();
                    forDoubles.layoutWithoutQuestionFollows = forDoubles.mayComeBack;
                    forDoubles.boundsAskedInFront.shrink(0);
                    findSumsAnew(forDoubles);
                    if (forDoubles.mayComeBack)
                        layOutTurns(forDoubles);
                    else
                        layOutForEveryKind(forDoubles);
                    loop->listsAskedForKind.removeAllMatching([](const ListAskedForKind& asked) {
                        return asked.otherKindsAreNotAsked;
                    });
                    askKindsReadWhereLoopIsComeBackTo(*loop);
                    findSumsAnew(*loop);
                }
                if (!m_hasFailed && !loop->boundsAskedInFront.isEmpty()) {
                    CountedLoop behindBounds = *loop;
                    behindBounds.mayComeBack = false;
                    build().statistics().count(StatisticID::UnrolledLoopsForMaskedIndexes);
                    layOutForEveryKind(behindBounds);
                    loop->boundsAskedInFront.shrink(0);
                }
                if (!m_hasFailed)
                    layOutForEveryKind(*loop);
            }
            RELEASE_ASSERT(m_hasFailed || !m_layoutsInRow);
            if (m_hasFailed)
                return GenerationResult::Failed;
        }
        [[clang::always_inline]] generateBlock(id, block);
        new (NotNull, &m_scopeAtEndOfBlock[id.index()]) ScopeFork(m_out.forkScope());
    }
    for (LBasicBlock leftOut : m_blocksLeftOut)
        RELEASE_ASSERT(m_hasFailed || !leftOut->numberOfIncomingEdges());
    RELEASE_ASSERT(m_hasFailed || !m_limitOfWrittenLoop);
    for (bool someWasMarked = true; someWasMarked;) {
        someWasMarked = false;
        for (auto [value, phi] : m_phisHandedOnWhileIncomplete)
            someWasMarked |= m_out.handOnUpperHalf(value, phi);
    }
    return m_hasFailed ? GenerationResult::Failed : GenerationResult::Generated;
}

ALWAYS_INLINE_INTO_OTHER_FILES bool CodeGenerator::leavesOutBlockThatNothingLeadsTo(BlockID id)
{
    if (m_nextBlockGoesOnHere)
        return false;
    LBasicBlock block = m_blocks[id.index()];
    if (!block)
        return true;
    if (block->numberOfIncomingEdges() || block->isEntry()) [[likely]]
        return false;
    m_blocksLeftOut.append(block);
    return true;
}

const Function& CodeGenerator::function() const
{
    return program().at(unit().function);
}

FormAtPlace CodeGenerator::sayFormNamedFor(const OperationContext& context, StateOfKnowledge state) const
{
    bool standsInLoop = context.block() && isInLoop(context.block());
    if (!context.valueID())
        return formByLoopAlone(static_cast<const TerminatorContext&>(context).kind(), state, standsInLoop);
    FormAtPlace named = formByLoopAlone(static_cast<const ValueContext&>(context).opcode(), state, standsInLoop);
    if (m_operation == &context)
        m_records.formsSaid.say(named.form, state, named.place);
    return named;
}

bool CodeGenerator::takesOpenForm(const OperationContext& context, StateOfKnowledge state) const
{
    return sayFormNamedFor(context, state).form == Form::Open;
}

bool CodeGenerator::takesFormOfLoops(const OperationContext& context, StateOfKnowledge state) const
{
    return sayFormNamedFor(context, state).place == Place::Loop;
}

const PlacedSlot* CodeGenerator::slotOfScopeReadInPlaceOfClosureSlow(ValueID id, const Operation& read) const
{
    if (code().useCountOf(id) != 1 || !code().onlyUserOf(id))
        return nullptr;
    const Operation& capture = code().at(code().onlyUserOf(id));
    if (capture.opcode != Opcode::GetCapture || capture.index() || capture.representation != Representation::Cell)
        return nullptr;
    const PlacedSlot* scope = nullptr;
    for (LayoutID member : membersOf(inputs().types, read.receivers())) {
        if (!plan().isInUse(member))
            continue;
        std::optional<FieldIndex> field = plan().slots().findField(program(), member, read.key());
        std::optional<FieldIndex> fieldOfScope = plan().layout(member).environmentOfDeferredClosures;
        if (!field || !fieldOfScope)
            return nullptr;
        const PlacedSlot& slot = plan().slot(member, *field);
        if (slot.representation != SlotRepresentation::DeferredClosure || slot.storage != SlotStorage::Inline || !slot.deferredClosureCapturesEnvironment || slot.deferredFunction != capture.function())
            return nullptr;
        const PlacedSlot& found = plan().slot(member, *fieldOfScope);
        if (found.representation != SlotRepresentation::Cell32 || (scope && !scope->hasLocationOf(found)))
            return nullptr;
        scope = &found;
    }
    return scope;
}

bool CodeGenerator::isDeferredClosureSlow(const Operation& named) const
{
    if (named.representation != Representation::Void)
        return false;
    const Operation* definition = &named;
    while (definition->opcode == Opcode::Narrowed)
        definition = &code().at(definition->valueOperand());
    return definition->opcode == Opcode::NewClosure && definition->closureForm() == ClosureForm::Deferred;
}

void CodeGenerator::computeEffectsOf(const OperationContext& context) const
{
    std::span<const ValueID> operands = context.instruction().operands();
    std::array<TypeID, 8> typesOfFewOperands;
    Vector<TypeID> typesOfManyOperands;
    std::span<TypeID> typesOfOperands(typesOfFewOperands);
    if (operands.size() > typesOfFewOperands.size()) [[unlikely]] {
        typesOfManyOperands.grow(operands.size());
        typesOfOperands = typesOfManyOperands.mutableSpan();
    }
    for (size_t index = 0; index < operands.size(); ++index) {
        ValueID operand = operands[index];
        typesOfOperands[index] = operand ? typeIDOf(operand) : TypeID();
    }
    OperandTypes operandTypes { typesOfOperands.first(operands.size()) };
    if (!context.valueID()) {
        new (NotNull, &context.m_effects) Effects(effectsOfTerminator(m_typing, static_cast<const TerminatorContext&>(context).terminator(), operandTypes));
        context.m_hasEffects = true;
        return;
    }

    const Operation& operation = static_cast<const ValueContext&>(context).value();
    Effects& effects = *new (NotNull, &context.m_effects) Effects(Typed::effectsOf(m_typing, operation, operandTypes, operation.type));
    context.m_hasEffects = true;
    EffectTraits traits = operation.traits();
    if (!traits.mayThrow())
        effects.mayThrow = false;
    else if (!traits.mayThrowByRule())
        effects.mayThrow &= ThrowEffect::onlyForLackOfMemory();
    if (!traits.mayCollect())
        effects.mayCollect = false;
    if (!traits.mayCallUnknownCode())
        effects.mayCallUnknownCode = false;
    if (!traits.maySuspend())
        effects.maySuspend = false;
    if (!traits.mayBeTerminated())
        effects.mayBeTerminated = false;
    if (m_understatedEffects) [[unlikely]]
        UnderstatedEffects::decode(m_understatedEffects).cut(KindOfEffectsRow::Operation, static_cast<unsigned>(operation.opcode), effects);
}

bool CodeGenerator::recordSitesWithoutCode()
{
    Subject subject { .function = unit().function };
    std::span<const InlineFrame> inlineFrames = code().inlineFrames();
    if (inlineFrames.size() > build().limit(LimitID::InlineFramesPerFunction)) {
        failAtLimit(LimitID::InlineFramesPerFunction, inlineFrames.size(), subject);
        return false;
    }

    SiteRecords& sites = m_records.sites;
    for (const InlineFrame& frame : inlineFrames) {
        bool isFunctionItself = sites.inlineCallSites.isEmpty();
        if (isFunctionItself) {
            sites.inlineCallSites.append(0);
            continue;
        }
        sites.inlineCallSites.append(sites.addSite({
            .origin = { .position = frame.callPosition, .inlineFrame = frame.parent, .isPartOfConstruction = frame.callIsPartOfConstruction || frame.isConstruction },
            .kind = frame.isConstruction ? CallSiteKind::Construct : CallSiteKind::Plain,
        }));
    }
    sites.numberOfSitesWithoutCode = sites.positions.size();

    size_t numberOfPositions = sites.positions.size() + 1;
    if (numberOfPositions > build().limit(LimitID::SitesPerFunction)) {
        failAtLimit(LimitID::SitesPerFunction, numberOfPositions, subject);
        return false;
    }
    return true;
}

void CodeGenerator::orderBlocks()
{
    static_assert(BlockFrequency::Normal < BlockFrequency::Unlikely && BlockFrequency::Unlikely < BlockFrequency::Rare);
    enum class StepKind : uint8_t { EdgeOfTerminator, OtherWayIn, EndOfBlock };
    struct Step {
        BlockID block;
        BlockID from;
        StepKind kind;
    };

    Machine::Arena& arena = m_proc.arena();
    uint32_t numberOfBlocks = code().numberOfBlocks();
    m_singlePredecessors = allocateSpanOfAllBitsSet<BlockID>(arena, numberOfBlocks);
    m_scopeAtEndOfBlock = allocateZeroedSpan<ScopeFork>(arena, numberOfBlocks);
    std::span<BlockID> order = allocateSpan<BlockID>(arena, numberOfBlocks);
    uint32_t numberOrdered = 0;
    Vector<Step, 64> steps;

    auto endBlock = [&](BlockID block) {
        order[numberOfBlocks - ++numberOrdered] = block;
    };

    static constexpr BlockFrequency fromLikeliest[] = { BlockFrequency::Normal, BlockFrequency::Unlikely, BlockFrequency::Rare };
    auto enter = [&](Step& step) -> bool {
        BlockID id = step.block;
        RELEASE_ASSERT(id);
        BlockID& predecessor = m_singlePredecessors[id.index()];
        if (predecessor) {
            m_firstBlockHasOtherWaysIn |= id == m_onlyEntryBlock;
            if (predecessor != enteredOtherwise)
                m_waysIntoJoins.append({ id, predecessor });
            m_waysIntoJoins.append({ id, step.from });
            predecessor = enteredOtherwise;
            return false;
        }
        if (!code().isReached(id))
            return false;
        if (step.kind == StepKind::EdgeOfTerminator)
            predecessor = step.from;
        else {
            predecessor = enteredOtherwise;
            m_waysIntoJoins.append({ id, step.from });
        }

        const Block& block = code().at(id);
        const Terminator& terminator = block.terminator;
        EdgeID handler = block.handler ? code().handlerOf(id) : EdgeID();
        uint32_t numberOfSuccessors = terminator.numberOfEdges + (handler ? 1 : 0);
        if (!numberOfSuccessors) {
            endBlock(id);
            return false;
        }
        steps.append({ id, BlockID(), StepKind::EndOfBlock });
        auto successorAt = [&](uint32_t index) -> Step {
            if (index < terminator.numberOfEdges)
                return { code().at(terminator.edge(index)).to, id, StepKind::EdgeOfTerminator };
            return { code().at(handler).to, id, StepKind::OtherWayIn };
        };
        if (numberOfSuccessors == 1) {
            step = successorAt(0);
            return true;
        }
        if (numberOfSuccessors == 2) {
            Step first = successorAt(0);
            Step second = successorAt(1);
            bool firstIsRarer = code().at(first.block).frequency > code().at(second.block).frequency;
            steps.append(firstIsRarer ? second : first);
            step = firstIsRarer ? first : second;
            return true;
        }

        struct Successor {
            Step step;
            BlockFrequency frequency;
        };
        Vector<Successor, 8> successors;
        for (uint32_t index = 0; index < numberOfSuccessors; ++index) {
            Step successor = successorAt(index);
            successors.append({ successor, code().at(successor.block).frequency });
        }
        for (BlockFrequency frequency : fromLikeliest) {
            for (const Successor& successor : successors) {
                if (successor.frequency == frequency)
                    steps.append(successor.step);
            }
        }
        return false;
    };
    auto takeStep = [&](Step& step) -> bool {
        while (!steps.isEmpty()) {
            step = steps.takeLast();
            if (step.kind != StepKind::EndOfBlock)
                return true;
            endBlock(step.block);
        }
        return false;
    };

    std::span<const EntryOfCode> entries = code().entries();
    if (entries.size() == 1)
        m_onlyEntryBlock = entries[0].block;
    for (size_t entry = entries.size(); entry--;) {
        Step step { entries[entry].block, BlockID(), StepKind::OtherWayIn };
        while (enter(step) || takeStep(step)) { }
    }
    m_order = order.last(numberOrdered);
    for (const WayIntoBlock& way : m_waysIntoJoins)
        m_singlePredecessors[way.to.index()] = BlockID();
}

void CodeGenerator::findDominators()
{
    constexpr uint32_t unknown = UINT32_MAX;
    constexpr uint32_t noWay = UINT32_MAX;
    constexpr uint32_t root = 0;
    Machine::Arena& arena = m_proc.arena();
    uint32_t numberOfPlaces = static_cast<uint32_t>(m_order.size() + 1);

    m_places = allocateZeroedSpan<uint32_t>(arena, code().numberOfBlocks());
    for (uint32_t place = 1; place < numberOfPlaces; ++place)
        m_places[m_order[place - 1].index()] = place;

    m_dominatorsByPlace = allocateSpanOfAllBitsSet<uint32_t>(arena, numberOfPlaces);
    m_dominatorsByPlace[root] = root;
    std::span<uint32_t> firstWaysOut = allocateSpanOfAllBitsSet<uint32_t>(arena, numberOfPlaces);
    std::span<uint32_t> nextWaysOut = allocateSpan<uint32_t>(arena, m_waysIntoJoins.size());
    for (uint32_t index = 0; index < m_waysIntoJoins.size(); ++index) {
        const WayIntoBlock& way = m_waysIntoJoins[index];
        if (!way.from) {
            m_dominatorsByPlace[m_places[way.to.index()]] = root;
            continue;
        }
        uint32_t& first = firstWaysOut[m_places[way.from.index()]];
        nextWaysOut[index] = first;
        first = index;
    }

    std::span<uint32_t> firstWaysIn = allocateSpanOfAllBitsSet<uint32_t>(arena, numberOfPlaces);
    std::span<uint32_t> nextWaysIn = allocateSpan<uint32_t>(arena, m_waysIntoJoins.size());
    Vector<uint32_t, 16> targetsOfWaysThatComeRound;
    for (uint32_t place = 1; place < numberOfPlaces; ++place) {
        for (uint32_t index = firstWaysOut[place]; index != noWay; index = nextWaysOut[index]) {
            uint32_t target = m_places[m_waysIntoJoins[index].to.index()];
            if (target <= place && (firstWaysIn[target] == noWay || m_places[m_waysIntoJoins[firstWaysIn[target]].from.index()] < target))
                targetsOfWaysThatComeRound.append(target);
            nextWaysIn[index] = firstWaysIn[target];
            firstWaysIn[target] = index;
        }
    }
    auto goThroughPlaces = [&]<bool climbsMayBeLong>() {
        constexpr int64_t stepsOfShortClimbsForEachPlace = 4;
        [[maybe_unused]] int64_t stepsLeftForShortClimbs = stepsOfShortClimbsForEachPlace * numberOfPlaces;
        [[maybe_unused]] std::span<uint32_t> depths;
        [[maybe_unused]] std::span<uint32_t> leaps;
        [[maybe_unused]] uint32_t numberOfPlacesHung = 1;
        [[maybe_unused]] uint32_t numberOfPlacesWithDominatorOfRound = 1;
        [[maybe_unused]] bool dominatorHasMovedBehindRound = false;
        auto lastOnEveryWayToBoth = [&](uint32_t found, uint32_t other) {
            if (!climbsMayBeLong || stepsLeftForShortClimbs >= 0 || dominatorHasMovedBehindRound) [[likely]] {
                while (found != other) {
                    if (found > other)
                        found = m_dominatorsByPlace[found];
                    else
                        other = m_dominatorsByPlace[other];
                    if constexpr (climbsMayBeLong)
                        --stepsLeftForShortClimbs;
                }
                return found;
            }
            if (leaps.empty()) {
                depths = allocateSpan<uint32_t>(arena, numberOfPlaces);
                leaps = allocateSpan<uint32_t>(arena, numberOfPlaces);
                depths[root] = 0;
                leaps[root] = root;
            }
            for (; numberOfPlacesHung < numberOfPlacesWithDominatorOfRound; ++numberOfPlacesHung) {
                uint32_t above = m_dominatorsByPlace[numberOfPlacesHung];
                uint32_t leap = leaps[above];
                depths[numberOfPlacesHung] = depths[above] + 1;
                leaps[numberOfPlacesHung] = depths[above] - depths[leap] == depths[leap] - depths[leaps[leap]] ? leaps[leap] : above;
            }
            if (depths[found] < depths[other])
                std::swap(found, other);
            uint32_t depth = depths[other];
            while (depths[found] > depth)
                found = depths[leaps[found]] >= depth ? leaps[found] : m_dominatorsByPlace[found];
            while (found != other) {
                bool leapsDiffer = leaps[found] != leaps[other];
                found = leapsDiffer ? leaps[found] : m_dominatorsByPlace[found];
                other = leapsDiffer ? leaps[other] : m_dominatorsByPlace[other];
            }
            return found;
        };
        auto takeWaysInto = [&](uint32_t target, bool thatComeRound) {
            uint32_t known = m_dominatorsByPlace[target];
            if (known == root)
                return false;
            uint32_t found = unknown;
            for (uint32_t index = firstWaysIn[target]; index != noWay; index = nextWaysIn[index]) {
                uint32_t source = m_places[m_waysIntoJoins[index].from.index()];
                if ((source >= target) != thatComeRound) {
                    if (thatComeRound)
                        break;
                    continue;
                }
                found = found == unknown ? source : lastOnEveryWayToBoth(found, source);
            }
            if (found == unknown)
                return false;
            if (known != unknown)
                found = lastOnEveryWayToBoth(found, known);
            m_dominatorsByPlace[target] = found;
            return found != known;
        };

        for (bool changed = true; changed;) {
            changed = false;
            numberOfPlacesHung = 1;
            dominatorHasMovedBehindRound = false;
            for (uint32_t place = 1; place < numberOfPlaces; ++place) {
                if (BlockID predecessor = m_singlePredecessors[m_order[place - 1].index()])
                    m_dominatorsByPlace[place] = m_places[predecessor.index()];
                else {
                    numberOfPlacesWithDominatorOfRound = place;
                    takeWaysInto(place, false);
                }
            }
            numberOfPlacesWithDominatorOfRound = numberOfPlaces;
            for (uint32_t target : targetsOfWaysThatComeRound) {
                if (takeWaysInto(target, true)) {
                    changed = true;
                    dominatorHasMovedBehindRound = true;
                }
            }
        }
    };
    constexpr uint32_t mostPlacesOfBodyWithoutLongClimbs = 512;
    if (numberOfPlaces > mostPlacesOfBodyWithoutLongClimbs)
        goThroughPlaces.template operator()<true>();
    else
        goThroughPlaces.template operator()<false>();
}

void CodeGenerator::numberDominatorTree()
{
    Machine::Arena& arena = m_proc.arena();
    size_t numberOfPlaces = m_dominatorsByPlace.size();
    m_subtreesByPlace = allocateZeroedSpan<SubtreeOfDominators>(arena, numberOfPlaces);
    for (size_t place = numberOfPlaces; --place;) {
        uint32_t size = ++m_subtreesByPlace[place].size;
        m_subtreesByPlace[m_dominatorsByPlace[place]].size += size;
    }
    std::span<uint32_t> nextNumbersBelow = allocateSpan<uint32_t>(arena, numberOfPlaces);
    nextNumbersBelow[0] = 0;
    for (size_t place = 1; place < numberOfPlaces; ++place) {
        uint32_t& nextNumberBelowDominator = nextNumbersBelow[m_dominatorsByPlace[place]];
        uint32_t first = nextNumberBelowDominator;
        nextNumberBelowDominator += m_subtreesByPlace[place].size;
        nextNumbersBelow[place] = first + 1;
        m_subtreesByPlace[place].first = first;
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES bool CodeGenerator::dominates(BlockID home, BlockID user)
{
    if (m_dominatorsByPlace.empty()) {
        BlockID above = user;
        for (unsigned step = 0; step < longestWayUpSinglePredecessors; ++step) {
            above = m_singlePredecessors[above.index()];
            if (!above)
                break;
            if (above == home)
                return true;
        }
        findDominators();
    }

    uint32_t placeOfHome = m_places[home.index()];
    uint32_t placeOfUser = m_places[user.index()];
    if (!placeOfHome || !placeOfUser)
        return false;
    if (m_subtreesByPlace.empty()) {
        uint32_t place = placeOfUser;
        for (unsigned step = 0; step < longestWayUpDominators; ++step) {
            if (place <= placeOfHome)
                return place == placeOfHome;
            place = m_dominatorsByPlace[place];
        }
        numberDominatorTree();
    }
    const SubtreeOfDominators& subtreeOfHome = m_subtreesByPlace[placeOfHome];
    return m_subtreesByPlace[placeOfUser].first - subtreeOfHome.first < subtreeOfHome.size;
}

ALWAYS_INLINE_INTO_OTHER_FILES bool CodeGenerator::goesOnBehindJump(size_t positionInOrder) const
{
    if (!positionInOrder)
        return false;
    BlockID id = m_order[positionInOrder];
    BlockID inFront = m_order[positionInOrder - 1];
    if (m_singlePredecessors[id.index()] != inFront)
        return false;
    const Block& previousBlock = code().at(inFront);
    const Block& block = code().at(id);
    if (previousBlock.terminator.kind != TerminatorKind::Jump || previousBlock.frequency != block.frequency)
        return false;
    return std::ranges::none_of(block.parameters(), [&](ValueID parameter) {
        return code().isNeeded(parameter) && hasRegister(representationOf(parameter));
    });
}

void CodeGenerator::createBlocks()
{
    m_blocks = allocateZeroedSpan<LBasicBlock>(m_proc.arena(), code().numberOfBlocks());
    for (size_t position = 0; position < m_order.size(); ++position) {
        if (!goesOnBehindJump(position) && !goesOnInBlockOfEntry(m_order[position]))
            m_blocks[m_order[position].index()] = m_out.newBlock();
    }
}

bool CodeGenerator::isReadByBoxAlone(BlockID home, ValueID parameter, unsigned stepsLeft) const
{
    if (code().useCountOf(parameter) != 1)
        return false;
    for (const auto& entry : code().entries()) {
        if (entry.block == home)
            return false;
    }
    if (ValueID user = code().onlyUserOf(parameter)) {
        const Operation& box = code().at(user);
        std::span<const ValueID> values = code().valuesOf(home);
        return box.opcode == Opcode::Box && box.representation == Representation::JSValue && !values.empty() && values.front() == user;
    }
    const Terminator& terminator = code().at(home).terminator;
    if (!stepsLeft || terminator.kind != TerminatorKind::Jump)
        return false;
    const Edge& edge = code().at(terminator.edge(0));
    std::span<const ValueID> arguments = edge.arguments();
    std::span<const ValueID> parameters = code().at(edge.to).parameters();
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] != parameter)
            continue;
        ValueID next = parameters[index];
        return representationOf(next) == Representation::Double && code().isNeeded(next) && isReadByBoxAlone(edge.to, next, stepsLeft - 1);
    }
    return false;
}

bool CodeGenerator::isStoredIntoByName(ValueID made) const
{
    BlockID block = code().blockOf(made);
    for (unsigned stepsLeft = 8; stepsLeft--;) {
        ValueID boxed;
        for (ValueID id : code().valuesOf(block)) {
            const Operation& operation = code().at(id);
            if (operation.opcode == Opcode::Box && operation.operand(0) == made)
                boxed = id;
            if (operation.opcode == Opcode::PropSet && (operation.objectOperand() == made || (boxed && operation.objectOperand() == boxed)))
                return true;
        }
        const Terminator& terminator = code().at(block).terminator;
        if (terminator.kind != TerminatorKind::Jump)
            return false;
        const Edge& edge = code().at(terminator.next());
        std::span<const ValueID> arguments = edge.arguments();
        std::span<const ValueID> parameters = code().at(edge.to).parameters();
        size_t position = std::ranges::find_if(arguments, [&](ValueID argument) {
            return argument == made || (boxed && argument == boxed);
        }) - arguments.begin();
        if (position < std::min(arguments.size(), parameters.size()))
            made = parameters[position];
        block = edge.to;
    }
    return false;
}

void CodeGenerator::createPhis()
{
    constexpr unsigned mostBlocksThatHandParameterOn = 4;
    for (BlockID block : m_order) {
        if (goesOnInBlockOfEntry(block))
            continue;
        for (ValueID parameter : code().at(block).parameters()) {
            Representation representation = representationOf(parameter);
            RELEASE_ASSERT(representation != Representation::Unchosen);
            if (!code().isNeeded(parameter) || !hasRegister(representation))
                continue;
            if (representation == Representation::Double && !m_followsNumbersOutOfBytes && isReadByBoxAlone(block, parameter, mostBlocksThatHandParameterOn)) [[unlikely]] {
                if (!m_carriesParametersBoxed) {
                    m_carriesParametersBoxed = true;
                    m_parametersCarriedBoxed.ensureSize(code().numberOfValues());
                }
                m_parametersCarriedBoxed.quickSet(parameter.index());
                representation = Representation::JSValue;
            }
            m_out.setOrigin(machineOriginOf(parameter));
            defineValue(parameter, representation, Operand(representation, m_out.phiIn(blockFor(block), machineTypeOf(representation))));
        }
    }
    m_out.setOrigin(Machine::Origin());
}

void CodeGenerator::followNumbersOutOfBytes()
{
    if (!code().readsNumbersOutOfBytes())
        return;

    m_followsNumbersOutOfBytes = true;
    uint32_t numberOfValues = code().numberOfValues();
    m_valuesHoldingAnyNaN.ensureSize(numberOfValues);
    m_valuesReadForBitsOfNaN.ensureSize(numberOfValues);
    m_blocksBegun.ensureSize(code().numberOfBlocks());

    Vector<ValueID> headsFed;
    headsFed.fill(ValueID(), numberOfValues);
    Vector<ValueID, 4> headsFedByBytes;
    auto isReadForBitsOfNaN = [&](ValueID value) {
        return m_valuesReadForBitsOfNaN.quickGet(value.index());
    };
    auto feed = [&](ValueID value, ValueID head) {
        if (head && !headsFed[value.index()])
            headsFed[value.index()] = head;
    };

    for (size_t position = m_order.size(); position--;) {
        BlockID id = m_order[position];
        const Block& block = code().at(id);
        block.terminator.forEachChild([&](ValueID operand) {
            m_valuesReadForBitsOfNaN.quickSet(operand.index());
        });
        if (block.subject)
            m_valuesReadForBitsOfNaN.quickSet(block.subject.index());
        EdgeID handler = code().handlerOf(id);
        code().forEachEdgeOf(id, [&](EdgeID edgeID, const Edge& edge) {
            std::span<const ValueID> parameters = code().at(edge.to).parameters();
            std::span<const ValueID> arguments = edge.arguments();
            size_t numberOfPairs = std::min(parameters.size(), arguments.size());
            for (size_t index = 0; index < numberOfPairs; ++index) {
                ValueID parameter = parameters[index];
                ValueID argument = arguments[index];
                if (!argument || !code().isNeeded(parameter))
                    continue;
                if (edgeID == handler || representationOf(parameter) != Representation::Double) {
                    m_valuesReadForBitsOfNaN.quickSet(argument.index());
                    continue;
                }
                if (code().at(edge.to).isLoopHead && parameter < argument) {
                    ValueID& fed = headsFed[argument.index()];
                    if (!fed || argument < fed || fed < parameter)
                        fed = parameter;
                } else if (code().at(edge.to).isLoopHead)
                    feed(argument, parameter);
                else if (!isReadForBitsOfNaN(parameter))
                    feed(argument, headsFed[parameter.index()]);
            }
        });

        std::span<const ValueID> values = code().valuesOf(id);
        for (size_t index = values.size(); index--;) {
            ValueID value = values[index];
            const Operation& operation = code().at(value);
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (!takesAnyNaN(operation, operandIndex))
                    m_valuesReadForBitsOfNaN.quickSet(operand.index());
            });
            ValueID head = headsFed[value.index()];
            if (!head || isReadForBitsOfNaN(value))
                continue;
            if (readsNumberOutOfBytes(operation) || readsNumberOrNothingOutOfBytes(operation))
                headsFedByBytes.append(head);
            else if (handsOnBitsOfNaN(operation)) {
                operation.forEachChild([&](ValueID operand) {
                    feed(operand, head);
                });
            }
        }
    }

    for (ValueID head : headsFedByBytes) {
        while (head && !isReadForBitsOfNaN(head) && !m_valuesHoldingAnyNaN.quickSet(head.index()))
            head = headsFed[head.index()];
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES ValueID CodeGenerator::testReadByBranchAlone(BlockID id, const Terminator& terminator) const
{
    if (terminator.kind != TerminatorKind::Branch)
        return { };
    ValueID condition = terminator.conditionOperand();
    const Operation& test = code().at(condition);
    if (test.block() != id || branchesOf(test.opcode) != Branches::May || code().useCountOf(condition) != 1)
        return { };
    if (test.representation != Representation::Boolean || isNeverType(test.type))
        return { };
    return condition;
}

ALWAYS_INLINE_INTO_OTHER_FILES ValueID CodeGenerator::testReadByBranchBehindJumpAlone(BlockID id, const Block& block) const
{
    if (block.terminator.kind != TerminatorKind::Jump)
        return { };
    BlockID behind = code().at(block.terminator.next()).to;
    if (m_blocks[behind.index()] || m_singlePredecessors[behind.index()] != id || !code().valuesOf(behind).empty() || code().handlerOf(behind) != code().handlerOf(id))
        return { };
    return testReadByBranchAlone(id, code().at(behind).terminator);
}

ALWAYS_INLINE_INTO_OTHER_FILES bool CodeGenerator::isAwaitedAtOnce(const Operation& call, std::span<const ValueID> valuesOfBlock, size_t index) const
{
    if ((call.opcode != Opcode::Call && call.opcode != Opcode::CallDirect) || index + 1 == valuesOfBlock.size())
        return false;
    const Operation& next = code().at(valuesOfBlock[index + 1]);
    return next.opcode == Opcode::AwaitBySavingFrame && next.awaitedOperand() == valuesOfBlock[index];
}

static constexpr OpcodeSet sumsThatMayStayBoxed { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };

ValueID CodeGenerator::sumBoxedBySlow(const Operation& box) const
{
    ValueID sum = box.valueOperand();
    const Operation& operation = code().at(sum);
    BlockID block = box.block();
    if (!sumsThatMayStayBoxed.contains(operation.opcode) || operation.form() != OperatorForm::Numbers)
        return { };
    if (operation.block() != block || code().useCountOf(sum) != 1)
        return { };

    bool someTermLiesBoxed = false;
    auto isTerm = [&](ValueID id) {
        const Operation& term = code().at(id);
        if (code().isConstant(id))
            return term.opcode == Opcode::Integer && isRepresentableAs<int32_t>(term.integer());
        if (term.block() != block || code().useCountOf(id) != 1)
            return false;
        if (term.opcode == Opcode::IntegerToNumber)
            return term.representation == Representation::Double && representationOf(term.valueOperand()) == Representation::Int32;
        if (term.opcode != Opcode::Unbox || representationOf(term.valueOperand()) != Representation::JSValue)
            return false;
        const Type& boxed = typeOf(term.valueOperand());
        if (!(term.representation == Representation::Double && boxed.isNumber()) && !(term.representation == Representation::OptionalDouble && boxed.isNumberOrNullish()))
            return false;
        someTermLiesBoxed = true;
        return true;
    };
    bool isStep = operation.opcode == Opcode::Increment || operation.opcode == Opcode::Decrement;
    bool hasTermsAlone = isStep ? isTerm(operation.valueOperand()) : isTerm(operation.leftOperand()) && isTerm(operation.rightOperand());
    return hasTermsAlone && someTermLiesBoxed ? sum : ValueID();
}

bool CodeGenerator::waitsForBoxOfSum(ValueID id, const Operation& operation) const
{
    ValueID sum = id;
    if (operation.opcode == Opcode::Unbox || operation.opcode == Opcode::IntegerToNumber)
        sum = code().onlyUserOf(id);
    else if (!sumsThatMayStayBoxed.contains(operation.opcode))
        return false;
    ValueID box = sum ? code().onlyUserOf(sum) : ValueID();
    return box && sumBoxedBy(code().at(box)) == sum;
}

NEVER_INLINE void CodeGenerator::dispatchWatched(const ValueContext& context)
{
    uint32_t siteOfUser = FormsSaid::noSite;
    if (m_notesForms)
        siteOfUser = m_records.formsSaid.enter(context.id(), context.opcode(), context.block(), m_proc.numberOfOperations());
    declareAccessesOf(context);

    if (m_checksEffects) {
        RuntimeCallEmitter::WatchOfEffects watch;
        RuntimeCallEmitter::DeedsInFrontOfQuietCall deedsInFrontOfQuietCall = emitter<RuntimeCallEmitter>().countDeedOf(context);
        emitter<RuntimeCallEmitter>().beginWatchOfOperation(context, watch);
        dispatch(context);
        if (deedsInFrontOfQuietCall.all && !m_hasFailed && currentBlockIsOpen())
            emitter<RuntimeCallEmitter>().putBackDeedsBehindQuietCall(deedsInFrontOfQuietCall);
        if (watch.isSet() && !m_hasFailed && currentBlockIsOpen() && !isNeverType(context->type))
            emitter<RuntimeCallEmitter>().endWatchOfEffects(context, watch, nullptr);
        else
            emitter<RuntimeCallEmitter>().abandonWatchOfOperation(watch);
    } else
        dispatch(context);

    if (m_notesForms)
        m_records.formsSaid.leave(siteOfUser, m_proc.numberOfOperations());
}

ALWAYS_INLINE_INTO_OTHER_FILES void CodeGenerator::generate(const ValueContext& context)
{
    const OperationContext* user = std::exchange(m_operation, &context);
    Machine::Origin originOfUser = m_out.origin();
    m_out.setOrigin(machineOriginOf(context.id()));
    bool userOwesFenceBehindStores = std::exchange(m_owesFenceBehindStores, false);
    if (m_looksBehindEachOperation) [[unlikely]]
        m_storesBehindWhichOwnerIsRemembered.clear(context.id().index());
    if (m_watchesOperations) [[unlikely]]
        dispatchWatched(context);
    else
        dispatch(context);
    if (m_owesFenceBehindStores) [[unlikely]]
        fail(context, InternalErrorKind::MissingFenceAfterLateStores);
    m_owesFenceBehindStores = userOwesFenceBehindStores;
    if (!m_scheduledLifetimeEnds.isEmpty()) [[unlikely]] {
        m_scheduledLifetimeEnds.removeAllMatching([&](const std::pair<ValueID, ValueID>& lent) {
            if (lent.second != context.id())
                return false;
            if (!m_hasFailed && currentBlockIsOpen())
                emitter<HintEmitter>().emitLifetimeEnd(context, lent.first);
            return true;
        });
    }
    m_out.setOrigin(originOfUser);
    m_operation = user;
    if (m_looksBehindEachOperation) [[unlikely]] {
        if (barrierIsCountedOn(context.id()) && !m_storesBehindWhichOwnerIsRemembered.get(context.id().index()) && !m_hasFailed)
            fail(context, InternalErrorKind::OpcodeNotInForm, "the barrier of an earlier store is left to this one, and the code of this one does not remember its owner"_s);
        if (m_checksAllocation && user)
            declareAccessesSlow(*user);
        else if (m_checksAllocation)
            m_out.clearDeclaredAccesses();
        if (m_keepsValuesInHeapFrame && !user && !m_hasFailed && currentBlockIsOpen() && !context.isBranchCondition() && !context.returnCall()) {
            SetForScope<const OperationContext*> whileKeeping(m_operation, &context);
            emitter<SavedFrameEmitter>().keepInHeapFrame(context);
        }
    }

    size_t index = context.id().index();
    if (m_values[index] || !hasRegister(context.representation())) [[likely]]
        return;
    if (m_hasFailed || m_definitionTaken || context.isBranchCondition() || context.returnCall() || isNeverType(context->type))
        return;
    RELEASE_ASSERT(hasResults(index));
}

ALWAYS_INLINE_INTO_OTHER_FILES void CodeGenerator::generateValue(BlockID block, ValueID id, const Operation& operation, bool isAwaitedAtOnce, std::span<const ValueID> operationsBehind)
{
    if (!m_sumsOfIntegers.isEmpty() && addToSumOfIntegers(id)) [[unlikely]]
        return;
    if (!m_valuesLeftOutOfTurnsLaidOut.isEmpty() && m_valuesLeftOutOfTurnsLaidOut.contains(id)) [[unlikely]]
        return;
    if (operation.opcode == Opcode::HasArrayStorage && leavesKindToTurnsLaidOut(block, id, operation)) [[unlikely]]
        return;
    if (isDeferredClosure(operation))
        return;
    if ((operation.representation == Representation::Double || operation.representation == Representation::OptionalDouble) && waitsForBoxOfSum(id, operation)) [[unlikely]]
        return;
    if (const PlacedSlot* scope = slotOfScopeReadInPlaceOfClosure(id, operation); scope && !operationsBehind.empty() && operationsBehind.front() == code().onlyUserOf(id)) {
        m_closureReadForItsScope = id;
        m_slotOfScopeOfClosureRead = scope;
        return;
    }
    if (operation.opcode == Opcode::NewArray && !operationsBehind.empty() && !m_keepsValuesInHeapFrame && ArrayAllocationEmitter(*this).isLeftToSpreadRightBehind(block, id, operation, operationsBehind)) [[unlikely]] {
        m_literalLeftToSpreadRightBehind = id;
        return;
    }
    if (operation.traits().mayThrow())
        m_isAheadOfWhatMayThrow = false;
    ValueContext context(block, id, operation, handlerOf(block));
    context.m_isAwaitedAtOnce = isAwaitedAtOnce;
    context.m_operationsBehind = operationsBehind;
    generate(context);
}

std::span<const std::pair<ValueID, ValueID>> CodeGenerator::usersOfLiteralInBlock(BlockID block, ValueID literal)
{
    auto indexOfLiteral = [](const std::pair<ValueID, ValueID>& use) {
        return use.first.index();
    };
    if (m_blockWhoseUsersOfLiteralsAreListed != block) {
        m_blockWhoseUsersOfLiteralsAreListed = block;
        m_usersOfLiteralsInBlock.shrink(0);
        for (ValueID user : code().valuesOf(block)) {
            for (ValueID operand : code().at(user).operands()) {
                if (operand && code().at(operand).opcode == Opcode::NewArray)
                    m_usersOfLiteralsInBlock.append({ operand, user });
            }
        }
        std::ranges::stable_sort(m_usersOfLiteralsInBlock, { }, indexOfLiteral);
    }
    auto users = std::ranges::equal_range(m_usersOfLiteralsInBlock, literal.index(), { }, indexOfLiteral);
    return { users.begin(), users.end() };
}

LValue CodeGenerator::bearLiteralAsWritten(BlockID block, ValueID literal)
{
    generateValue(block, literal, code().at(literal), false, { });
    return m_values[literal.index()];
}

ALWAYS_INLINE_INTO_OTHER_FILES void CodeGenerator::generateTerminator(BlockID id, const Block& block, bool generatesCondition)
{
    RELEASE_ASSERT(block.hasEnded() && !m_operation);
    const Terminator& terminator = block.terminator;
    m_isAheadOfWhatMayThrow = false;
    TerminatorContext context(id, terminator, handlerOf(id));
    context.m_generatesCondition = generatesCondition;

    m_out.setOrigin(m_proc.traits().notesRangesOfOrigins ? machineOriginOf(terminator.kind) : Machine::Origin());

    m_operation = &context;
    declareAccessesOf(context);
    if (m_limitOfWrittenLoop && m_limitOfWrittenLoop->asking == id) [[unlikely]]
        askLimitOfWrittenLoop(context);
    else
        dispatch(context);
    if (std::exchange(m_owesFenceBehindStores, false)) [[unlikely]]
        fail(context, InternalErrorKind::MissingFenceAfterLateStores);
    m_operation = nullptr;
    if (m_checksAllocation) [[unlikely]]
        m_out.clearDeclaredAccesses();
    RELEASE_ASSERT(m_hasFailed || currentBlockIsOpen() == m_nextBlockGoesOnHere);
}

void CodeGenerator::generateBranchCondition(const TerminatorContext& terminator, ValueID condition, BranchTargets targets)
{
    if (ValueID read = std::exchange(m_readLeftToBranch, ValueID()))
        condition = read;
    ValueContext context(terminator.block(), condition, code().at(condition), handlerOf(terminator.block()));
    context.m_branchTargets = targets;
    generate(context);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

NEVER_INLINE ValueID CodeGenerator::readLeftToBranchOnItsTruthSlow(BlockID id, std::span<const ValueID> values, ValueID test)
{
    if (code().handlerOf(id))
        return { };
    const Operation& asked = code().at(test);
    ValueID read = values[values.size() - 2];
    if (m_headOfTurnsLaidOut) {
        bool asksTruthOfRead = asked.opcode == Opcode::ToBoolean && asked.form() == OperatorForm::Generic && asked.valueOperand() == read && code().useCountOf(read) == 1;
        return asksTruthOfRead && emitter<ElementAccessEmitter>().asksTruthOfRawElement(read) ? read : ValueID();
    }
    if (code().at(id).isInLoop)
        return { };
    if (code().useCountOf(read) == 1 && code().onlyUserOf(read) == test && emitter<AccessEmitter>().integerThatLengthIsAskedFor(id, read, test))
        return read;
    if (asked.opcode != Opcode::ToBoolean || asked.form() != OperatorForm::Generic || asked.valueOperand() != read || code().useCountOf(read) != 1)
        return { };
    return code().at(read).opcode == Opcode::PropGet && emitter<AccessEmitter>().readsTruthAtSite(id, read) ? read : ValueID();
}

std::optional<CodeGenerator::WaysOfTruth> CodeGenerator::waysOfTruthOf(const ValueContext& read) const
{
    BlockID id = read.block();
    const Block& block = code().at(id);
    std::span<const ValueID> behind = read.operationsBehind();
    if (block.isInLoop || m_headOfTurnsLaidOut || behind.size() != 1)
        return std::nullopt;
    static constexpr OpcodeSet tests { Opcode::ToBoolean, Opcode::TypeTest };
    auto testOfEnd = [&](BlockID tested, const Block& ended) -> const Operation* {
        ValueID test = testReadByBranchAlone(tested, ended.terminator);
        return test && tests.contains(opcodeOf(test)) && code().at(test).valueOperand() == read.id() ? &code().at(test) : nullptr;
    };
    auto wayAlong = [&](EdgeID along) -> std::optional<BlockEdge> {
        const Edge& edge = code().at(along);
        const Block& target = code().at(edge.to);
        if (!edge.arguments().empty() || !target.parameters().empty() || target.isLoopHead)
            return std::nullopt;
        return BlockEdge(blockFor(edge.to), edgeClassLeadingTo(target.frequency));
    };
    auto asksFor = [&](const Operation& test, TypeID asked) {
        TypeArena& types = inputs().types;
        return test.opcode == Opcode::TypeTest && types.isSubset(test.testedType(), asked) && types.isSubset(asked, test.testedType());
    };
    if (block.terminator.kind != TerminatorKind::Branch || !tests.contains(opcodeOf(behind[0])))
        return std::nullopt;
    const Operation* first = testOfEnd(id, block);
    if (!first || block.terminator.conditionOperand() != behind[0])
        return std::nullopt;
    std::optional<BlockEdge> whereTrue = wayAlong(block.terminator.taken());
    if (!whereTrue)
        return std::nullopt;
    if (first->opcode == Opcode::ToBoolean) {
        std::optional<BlockEdge> whereFalse = wayAlong(block.terminator.notTaken());
        if (first->form() != OperatorForm::Generic || code().useCountOf(read.id()) != 1 || !whereFalse)
            return std::nullopt;
        return WaysOfTruth { { *whereTrue, *whereFalse }, false };
    }

    BlockID others = code().at(block.terminator.notTaken()).to;
    const Block& blockOfOthers = code().at(others);
    if (!asksFor(*first, TypeTable::object) || code().useCountOf(read.id()) != 3 || m_singlePredecessors[others.index()] != id || code().valuesOf(others).size() != 1 || blockOfOthers.terminator.kind != TerminatorKind::Branch)
        return std::nullopt;
    const Operation* second = testOfEnd(others, blockOfOthers);
    if (!second || !asksFor(*second, TypeTable::nullish))
        return std::nullopt;
    BlockID rest = code().at(blockOfOthers.terminator.notTaken()).to;
    std::span<const ValueID> valuesOfRest = code().valuesOf(rest);
    if (m_singlePredecessors[rest.index()] != others || valuesOfRest.empty() || opcodeOf(valuesOfRest[0]) != Opcode::ToBoolean || code().at(valuesOfRest[0]).valueOperand() != read.id())
        return std::nullopt;
    std::optional<BlockEdge> whereNothing = wayAlong(blockOfOthers.terminator.taken());
    if (!whereNothing)
        return std::nullopt;
    return WaysOfTruth { { *whereTrue, *whereNothing }, true };
}

bool CodeGenerator::isReadAgainBehindItsOwnTruthAlone(const ValueContext& read) const
{
    const Block& block = code().at(read.block());
    if (block.terminator.kind != TerminatorKind::Branch)
        return false;
    ValueID test = block.terminator.conditionOperand();
    if (!test || opcodeOf(test) != Opcode::ToBoolean || code().at(test).valueOperand() != read.id() || !std::ranges::contains(read.operationsBehind(), test))
        return false;
    uint32_t uses = 1;
    BlockID inFront = read.block();
    for (EdgeID way = block.terminator.taken(); m_singlePredecessors[code().at(way).to.index()] == inFront;) {
        inFront = code().at(way).to;
        for (ValueID behindYes : code().valuesOf(inFront)) {
            code().at(behindYes).forEachOperand([&](ValueID operand, uint32_t) {
                uses += operand == read.id();
            });
        }
        const Terminator& end = code().at(inFront).terminator;
        if (end.kind != TerminatorKind::Jump)
            break;
        way = end.next();
    }
    return code().useCountOf(read.id()) == uses;
}

void CodeGenerator::generateReturnedCall(const TerminatorContext& terminator, ValueID call)
{
    ValueContext context(terminator.block(), call, code().at(call), handlerOf(terminator.block()));
    context.m_returnCall = &terminator;
    generate(context);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

static constexpr OpcodeSet operationsThatThrowWhateverComes { Opcode::ThrowNewError, Opcode::ThrowModuleFailure, Opcode::ThrowNotIterable };

bool CodeGenerator::leavesHoleToTestBehind(const ValueContext& read)
{
    BlockID home = read.block();
    const Block& block = code().at(home);
    if (!isX86_64() || m_keepsValuesInHeapFrame || m_headOfTurnsLaidOut || !block.isInLoop || block.handler)
        return false;
    ValueID test = testReadByBranchAlone(home, block.terminator);
    if (!test || code().at(test).opcode != Opcode::TypeTest || !code().at(test).asksInFull() || code().at(test).valueOperand() != read.id() || inputs().types.at(code().at(test).testedType()).primitives)
        return false;
    std::span<const ValueID> values = code().valuesOf(block);
    size_t place = std::ranges::find(values, read.id()) - values.begin();
    for (ValueID behind : values.subspan(std::min(place + 1, values.size()))) {
        const Operation& operation = code().at(behind);
        if (behind != test && (operation.opcode != Opcode::CheckProof || !operation.isLeftToTestBehind() || operation.valueOperand() != read.id()))
            return false;
    }
    const Edge& failing = code().at(block.terminator.notTaken());
    const Block& whereItFails = code().at(failing.to);
    if (m_singlePredecessors[failing.to.index()] != home || !failing.arguments().empty() || whereItFails.handler || whereItFails.terminator.kind != TerminatorKind::Unreachable)
        return false;
    m_elementsWhoseHolesAreLeftToTestsBehind.set(read.id().index());
    m_elementsMadePresentAtHeadsOfBlocks.add(failing.to.index(), read.id());
    return true;
}

void CodeGenerator::generateBlock(BlockID id, const Block& block)
{
    std::span<const ValueID> values = code().valuesOf(block);
    LValue* elementMadePresent = nullptr;
    LValue elementAsRead = nullptr;
    auto takeElementAsReadAgain = makeScopeExit([&] {
        if (elementMadePresent)
            *elementMadePresent = elementAsRead;
    });
    m_blockOfCachedHandler = id;
    m_cachedHandler = handlerEdgeOf(id, block);
    if (m_followsNumbersOutOfBytes)
        m_blocksBegun.quickSet(id.index());
    if (std::exchange(m_nextBlockGoesOnHere, false))
        RELEASE_ASSERT(blockFor(id) == m_out.m_block && currentBlockIsOpen());
    else if (BlockID predecessor = m_singlePredecessors[id.index()])
        m_out.appendToSuccessor(blockFor(id), m_scopeAtEndOfBlock[predecessor.index()]);
    else
        m_out.appendTo(blockFor(id));
    m_out.setOrigin(Machine::Origin());
    m_isAheadOfWhatMayThrow = true;
    if (!m_elementsMadePresentAtHeadsOfBlocks.isEmpty()) [[unlikely]] {
        if (auto made = m_elementsMadePresentAtHeadsOfBlocks.find(id.index()); made != m_elementsMadePresentAtHeadsOfBlocks.end()) {
            elementMadePresent = &m_values[made->value.index()];
            elementAsRead = *elementMadePresent;
            *elementMadePresent = m_out.select(m_out.isZero64(elementAsRead), m_out.constInt64(JSValue::encode(jsUndefined())), elementAsRead);
        }
    }
    if (m_keepsValuesInHeapFrame) [[unlikely]]
        enterBlockOfBodyWithHeapFrame(id, block);

    if (ValueID owned = valueOwnedByTerminator(block.terminator)) {
        RELEASE_ASSERT(!values.empty() && values.back() == owned);
        values = values.first(values.size() - 1);
    }
    ValueID test = block.terminator.kind == TerminatorKind::Branch ? testReadByBranchAlone(id, block.terminator) : ValueID();
    bool testWaitsForBranch = false;
    m_readLeftToBranch = readLeftToBranchOnItsTruth(id, values, test);
    for (size_t index = 0; index < values.size(); ++index) {
        ValueID value = values[index];
        if (value == m_readLeftToBranch)
            continue;
        const Operation& operation = code().at(value);
        if (value == test) {
            testWaitsForBranch = true;
            continue;
        }
        if (testWaitsForBranch && (!operation.traits().isPure() || isNeverType(operation.type))) {
            testWaitsForBranch = false;
            generateValue(id, test, code().at(test), false, values.subspan(index));
            if (m_hasFailed)
                return;
        }
        generateValue(id, value, operation, isAwaitedAtOnce(operation, values, index), testWaitsForBranch ? std::span<const ValueID>() : values.subspan(index + 1));
        if (m_hasFailed)
            return;
        if (!isNeverType(operation.type)) [[likely]]
            continue;
        if (!currentBlockIsOpen())
            return;
        if (operationsThatThrowWhateverComes.contains(operation.opcode)) {
            m_out.unreachable();
            return;
        }
        ValueContext context(id, value, operation, nullptr);
        SetForScope<const OperationContext*> whileStopping(m_operation, &context);
        emitter<ThrowEmitter>().crashOnFalseProof(context, std::nullopt, ProofSource::OtherOperation);
        return;
    }
    generateTerminator(id, block, testWaitsForBranch);
}

void CodeGenerator::enterBlockOfBodyWithHeapFrame(BlockID id, const Block& block)
{
    SavedFrameEmitter& frames = emitter<SavedFrameEmitter>();
    if (frames.goesOnInStretchAroundLoop(id))
        return;
    frames.beginStretchOfHeapFrame(id);
    if (id == m_order[0])
        frames.clearWordsInMachineFrame();
    for (ValueID parameter : block.parameters()) {
        if (!m_values[parameter.index()])
            continue;
        ValueContext context(id, parameter, code().at(parameter), nullptr);
        SetForScope<const OperationContext*> whileKeeping(m_operation, &context);
        frames.keepInHeapFrame(context);
    }
    if (ValueID neverStored = frames.readBackOutOfHeapFrame(id, ValueID()))
        fail(ValueContext(id, neverStored, code().at(neverStored), nullptr), InternalErrorKind::InvalidSuspension);
}

bool CodeGenerator::blockDominates(BlockID above, BlockID below)
{
    return above == below || dominates(above, below);
}

BlockID CodeGenerator::nearestDominatorOf(BlockID block)
{
    if (m_dominatorsByPlace.empty())
        findDominators();
    uint32_t above = m_dominatorsByPlace[m_places[block.index()]];
    return above ? m_order[above - 1] : BlockID();
}

static constexpr unsigned turnsLaidOut = 8;
static constexpr unsigned turnsLaidOutForEachKindOfView = 4;
static constexpr int32_t turnsBetweenPolls = 1 << 20;
static constexpr unsigned mostBlocksInTurnLaidOut = 4;
static constexpr unsigned placesLookedAtBehindHead = 32;
static constexpr unsigned placesLookedAtFirst = 8;
static constexpr unsigned mostBlocksInTurnFoundWide = 8;
static constexpr int64_t fewestTurnsOfLoopComeBackTo = 64;
static constexpr int64_t turnsThatPayForComingBack = 12;
static constexpr int64_t turnsWrittenForEachTurnOwed = 2;
static constexpr int64_t mostTurnsOwed = 64;
static constexpr int64_t largestStepOfCounter = 16;
static constexpr unsigned mostOperationsInTurnLaidOut = 16;
static constexpr OpcodeSet operationsOfTurnsLaidOut {
    Opcode::Narrowed, Opcode::Box, Opcode::Unbox, Opcode::IntegerToNumber, Opcode::NumberToInteger, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::ToInt32,
    Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::RemainderByReciprocal, Opcode::Increment, Opcode::Decrement, Opcode::Negate,
    Opcode::ToNumber, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift, Opcode::URShift,
    Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::StrictEq, Opcode::Eq, Opcode::TypeTest,
    Opcode::GetStringCodeUnit, Opcode::GetTypedArrayElement, Opcode::GetArrayElement, Opcode::IsElementPresent,
    Opcode::ShapeTest, Opcode::GetSlot,
};
static constexpr AbstractHeapSet heapsThatTurnsLaidOutReadOffObjects { AbstractHeap::ObjectFields, AbstractHeap::ShapeWords, AbstractHeap::NamedProperties };
static_assert(declaredBoundOf(Opcode::ShapeTest).reads.containsOnly(heapsThatTurnsLaidOutReadOffObjects) && declaredBoundOf(Opcode::GetSlot).reads.containsOnly(heapsThatTurnsLaidOutReadOffObjects));
static_assert(!declaredBoundOf(Opcode::PutTypedArrayElement).writes.containsAny(heapsThatTurnsLaidOutReadOffObjects) && !declaredBoundOf(Opcode::PutContiguousIndex).writes.containsAny(heapsThatTurnsLaidOutReadOffObjects) && !declaredBoundOf(Opcode::PutDoubleIndex).writes.containsAny(heapsThatTurnsLaidOutReadOffObjects));
static_assert(!declaredBoundOf(Opcode::PutModuleVariable).writes.containsAny(heapsThatTurnsLaidOutReadOffObjects) && !declaredBoundOf(Opcode::PutEnvironmentField).writes.containsAny(heapsThatTurnsLaidOutReadOffObjects));
static_assert(!declaredBoundOf(Opcode::ExtendArrayLength).writes.containsAny(heapsThatTurnsLaidOutReadOffObjects));

bool CodeGenerator::leavesKindToTurnsLaidOut(BlockID block, ValueID id, const Operation& operation) const
{
    if (operation.arrayStorage() != ArrayStorage::Double || operation.admitsInt32Storage() || operation.isWritable() || operation.data() || m_keepsValuesInHeapFrame || m_headOfTurnsLaidOut)
        return false;
    if (ValueID user = code().onlyUserOf(id); m_order[m_positionInOrder] != block || (user && (opcodeOf(user) != Opcode::GetArrayElement || code().at(user).holdsDoublesOperand() != id)))
        return false;
    static constexpr size_t blocksLookedThroughForHead = 8;
    size_t next = m_positionInOrder + 1;
    size_t end = std::min(m_order.size(), next + blocksLookedThroughForHead);
    while (next < end && !code().at(m_order[next]).isLoopHead)
        ++next;
    if (next == end)
        return false;
    if (m_cachedLoopHeaderPosition != next || m_cachedLoopQueryPosition != m_positionInOrder) {
        m_cachedLoopHeaderPosition = next;
        m_cachedLoopQueryPosition = m_positionInOrder;
        m_cachedCountedLoop = findCountedLoopAt(next);
    }
    const std::optional<CountedLoop>& loop = m_cachedCountedLoop;
    if (!loop)
        return false;
    auto told = std::ranges::find(loop->kindsToldToReads, id, &KindToldToReads::holdsDoubles);
    if (told == loop->kindsToldToReads.end() || told->numberOfReads != code().useCountOf(id))
        return false;
    return loop->listsWhoseKindIsRead.contains(told->list) || std::ranges::any_of(loop->listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == told->list && !asked.isStoredInto; });
}

std::optional<CodeGenerator::CountedLoop> CodeGenerator::findCountedLoopAt(size_t positionOfHead) const
{
    BlockID head = m_order[positionOfHead];
    const Block& blockOfHead = code().at(head);
    if (blockOfHead.frequency != BlockFrequency::Normal || code().handlerOf(head) || blockOfHead.subject || blockOfHead.isEntry || blockOfHead.isHandlerBlock())
        return std::nullopt;

    CountedLoop loop { .positionOfHead = positionOfHead };
    BlockID asking = head;
    size_t positionOfAsking = positionOfHead;
    if (blockOfHead.terminator.kind == TerminatorKind::Jump) {
        const Edge& behindPoll = code().at(blockOfHead.terminator.next());
        if (!behindPoll.arguments().empty() || positionOfHead + 1 == m_order.size() || m_order[positionOfHead + 1] != behindPoll.to || m_singlePredecessors[behindPoll.to.index()] != head)
            return std::nullopt;
        asking = behindPoll.to;
        ++positionOfAsking;
        const Block& block = code().at(asking);
        if (!block.isInLoop || block.isLoopHead || code().handlerOf(asking) || block.subject || block.isEntry || block.isHandlerBlock() || block.frequency != blockOfHead.frequency)
            return std::nullopt;
        if (!block.parameters().empty() || code().valuesOf(asking).size() > 1)
            return std::nullopt;
    }
    const Block& blockThatAsks = code().at(asking);
    ValueID test = asking != head && code().valuesOf(asking).empty() ? testReadByBranchBehindJumpAlone(head, blockOfHead) : testReadByBranchAlone(asking, blockThatAsks.terminator);
    if (!test)
        return std::nullopt;
    for (ValueID value : code().valuesOf(head)) {
        if (value == test)
            continue;
        if (opcodeOf(value) != Opcode::LoopHint || loop.poll)
            return std::nullopt;
        loop.poll = value;
    }
    const Operation& comparison = code().at(test);
    static constexpr OpcodeSet relationsOfCounters { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };
    if (!relationsOfCounters.contains(comparison.opcode) || comparison.form() != OperatorForm::Integers)
        return std::nullopt;
    loop.relation = comparison.opcode;
    loop.counter = comparison.leftOperand();
    loop.bound = comparison.rightOperand();
    std::span<const ValueID> parameters = blockOfHead.parameters();
    auto counterAmongParameters = std::ranges::find(parameters, loop.counter);
    Representation representation = representationOf(loop.counter);
    if (counterAmongParameters == parameters.end() || (representation != Representation::Int32 && representation != Representation::Int64) || representationOf(loop.bound) != representation)
        return std::nullopt;
    if (code().blockOf(loop.bound) == head)
        return std::nullopt;
    if (std::ranges::any_of(parameters, [&](ValueID parameter) { return isCarriedBoxed(parameter); }))
        return std::nullopt;
    const Edge& intoTurn = code().at(blockThatAsks.terminator.taken());
    if (!intoTurn.arguments().empty() || positionOfAsking + 1 == m_order.size() || m_order[positionOfAsking + 1] != intoTurn.to || m_singlePredecessors[intoTurn.to.index()] != asking || code().valuesOf(intoTurn.to).size() > mostOperationsInTurnLaidOut)
        return std::nullopt;

    unsigned numberOfPlaces = 0;
    bool mayEndInSwitch = false;
    auto blockAt = [&](unsigned place) {
        return m_order[positionOfAsking + 1 + place];
    };
    auto placeOf = [&](BlockID block) -> std::optional<unsigned> {
        for (unsigned place = 0; place < numberOfPlaces; ++place) {
            if (blockAt(place) == block)
                return place;
        }
        return std::nullopt;
    };
    auto forEachPlaceLedTo = [&](unsigned place, auto&& function) {
        const Terminator& terminator = code().at(blockAt(place)).terminator;
        for (uint32_t edge = 0; edge < terminator.numberOfEdges; ++edge) {
            if (std::optional<unsigned> target = placeOf(code().at(terminator.edge(edge)).to); target && *target > place)
                function(*target);
        }
    };
    auto leadsBack = [&](const Block& block) {
        return block.terminator.kind == TerminatorKind::Jump && code().at(block.terminator.next()).to == head;
    };
    bool admitsStoresIntoSlots = true;
    auto mayBelongToTurn = [&](BlockID id) {
        const Block& block = code().at(id);
        if (!block.isInLoop || block.isLoopHead || code().handlerOf(id) || block.subject || block.isEntry || block.isHandlerBlock() || block.frequency != blockOfHead.frequency)
            return false;
        if (std::ranges::any_of(block.parameters(), [&](ValueID parameter) { return isCarriedBoxed(parameter); }))
            return false;
        for (ValueID value : code().valuesOf(id)) {
            const Operation& operation = code().at(value);
            EffectTraits traits = operation.traits();
            if (traits.mayRunOtherCode() || traits.mayBeTerminated() || isNeverType(operation.type))
                return false;
            if ((operation.opcode == Opcode::GetTypedArrayElement || operation.opcode == Opcode::PutTypedArrayElement) && operation.typedArrayType() == NotTypedArray && !operation.isInsideView())
                return false;
            if (operation.opcode == Opcode::PutTypedArrayElement) {
                const Type& stored = typeOf(operation.valueOperand());
                if (!leadsBack(block) || stored.hasObjects() || stored.primitives & cellPrimitives)
                    return false;
                continue;
            }
            if (operation.opcode == Opcode::PutContiguousIndex || operation.opcode == Opcode::PutDoubleIndex) {
                if (!leadsBack(block) || !typeOf(operation.valueOperand()).isNumber())
                    return false;
                continue;
            }
            if (operation.opcode == Opcode::ExtendArrayLength) {
                if (!leadsBack(block))
                    return false;
                continue;
            }
            if (operation.opcode == Opcode::PutModuleVariable || operation.opcode == Opcode::PutEnvironmentField) {
                if (!leadsBack(block) || opcodeOf(operation.valueOperand()) != Opcode::Box)
                    return false;
                continue;
            }
            if (operation.opcode == Opcode::PutSlot) {
                if (!admitsStoresIntoSlots || operation.level() != KnowledgeLevel::Proven || !typeOf(operation.valueOperand()).isNumber())
                    return false;
                continue;
            }
            if (operation.opcode == Opcode::IsSlotFilled && operation.level() == KnowledgeLevel::Proven)
                continue;
            if (operation.opcode == Opcode::ToNumber && (operation.form() == OperatorForm::Generic || !typeOf(operation.valueOperand()).isNumberOrNullish()))
                return false;
            if (operation.opcode == Opcode::CheckProof && operation.isLeftToTestBehind())
                continue;
            if (operation.opcode == Opcode::ToBoolean) {
                ValueID read = operation.valueOperand();
                const Operation& element = code().at(read);
                if (operation.form() != OperatorForm::Generic || code().isConstant(read) || code().blockOf(read) != id || code().useCountOf(read) != 1 || element.opcode != Opcode::GetArrayElement)
                    return false;
                if (element.arrayStorage() != ArrayStorage::Other || element.level() != KnowledgeLevel::Proven || !element.isInBounds() || element.asksCellWhereNothingIsFound() || representationOf(read) != Representation::JSValue)
                    return false;
                continue;
            }
            if (operation.opcode == Opcode::CallInline && reckonsInLine(operation.inlineBuiltin()) && traits.isPure())
                continue;
            if (!operationsOfTurnsLaidOut.contains(operation.opcode) || traits.mayWriteObservableState())
                return false;
        }
        const Terminator& terminator = block.terminator;
        if (terminator.kind == TerminatorKind::Jump)
            return true;
        bool endsInSmallSwitch = mayEndInSwitch && terminator.kind == TerminatorKind::Switch && terminator.numberOfCases() < Machine::minCasesForJumpTable && (representationOf(terminator.subjectOperand()) == Representation::Int32 || representationOf(terminator.subjectOperand()) == Representation::Int64);
        if (terminator.kind != TerminatorKind::Branch && !endsInSmallSwitch)
            return false;
        for (uint32_t edge = 0; edge < terminator.numberOfEdges; ++edge) {
            if (code().at(terminator.edge(edge)).to == head)
                return false;
        }
        return true;
    };
    Vector<ValueID, 4> listsReadInTurn;
    Vector<ValueID, 2> listsThatTakeRawDoubles;
    auto findTurn = [&](unsigned mostPlaces, bool switches) {
        numberOfPlaces = std::min<size_t>(mostPlaces, m_order.size() - positionOfAsking - 1);
        mayEndInSwitch = switches;
        loop.turn.shrink(0);
        loop.positionsOfTurn.shrink(0);
        loop.flatStrings.shrink(0);
        loop.listsAskedForKind.shrink(0);
        loop.listsWhoseKindIsRead.shrink(0);
        loop.kindsToldToReads.shrink(0);
        loop.viewsAskedForKind.shrink(0);
        loop.boundsAskedInFront.shrink(0);
        std::array<bool, placesLookedAtBehindHead> isComeTo { };
        std::array<bool, placesLookedAtBehindHead> isInTurn { };
        std::optional<unsigned> placeOfLast;
        isComeTo[0] = mayBelongToTurn(intoTurn.to);
        for (unsigned place = 0; place < numberOfPlaces; ++place) {
            if (!isComeTo[place])
                continue;
            if (leadsBack(code().at(blockAt(place)))) {
                placeOfLast = place;
                continue;
            }
            forEachPlaceLedTo(place, [&](unsigned target) {
                if (!isComeTo[target])
                    isComeTo[target] = mayBelongToTurn(blockAt(target));
            });
        }
        if (!placeOfLast)
            return false;
        isInTurn[*placeOfLast] = true;
        for (unsigned place = *placeOfLast; place--;) {
            if (!isComeTo[place] || leadsBack(code().at(blockAt(place))))
                continue;
            forEachPlaceLedTo(place, [&](unsigned target) {
                isInTurn[place] |= isInTurn[target];
            });
        }
        if (!isInTurn[0])
            return false;
        unsigned numberOfOperations = 0;
        loop.holesLeaveTurns = true;
        loop.storesIntoLists = false;
        loop.storesIntoSlots = false;
        listsReadInTurn.shrink(0);
        listsThatTakeRawDoubles.shrink(0);
        unsigned numberOfBlocksThatHoldOperations = 0;
        for (unsigned place = 0; place <= *placeOfLast; ++place) {
            if (!isInTurn[place])
                continue;
            numberOfBlocksThatHoldOperations += !code().valuesOf(blockAt(place)).empty();
            if (numberOfBlocksThatHoldOperations > (switches ? mostBlocksInTurnFoundWide : mostBlocksInTurnLaidOut))
                return false;
            loop.turn.append(blockAt(place));
            loop.positionsOfTurn.append(positionOfAsking + 1 + place);
            for (ValueID value : code().valuesOf(blockAt(place))) {
                const Operation& operation = code().at(value);
                if (++numberOfOperations > mostOperationsInTurnLaidOut)
                    return false;
                if (operation.opcode == Opcode::GetStringCodeUnit && opcodeOf(operation.stringOperand()) == Opcode::FlattenString && !loop.flatStrings.contains(operation.stringOperand()))
                    loop.flatStrings.append(operation.stringOperand());
                loop.holesLeaveTurns &= operation.opcode != Opcode::PutTypedArrayElement && operation.opcode != Opcode::PutSlot;
                loop.storesIntoSlots |= operation.opcode == Opcode::PutSlot;
                bool storesIntoList = operation.opcode == Opcode::PutContiguousIndex || operation.opcode == Opcode::PutDoubleIndex;
                loop.storesIntoLists |= storesIntoList;
                loop.holesLeaveTurns &= !storesIntoList;
                if (operation.opcode == Opcode::GetArrayElement || operation.opcode == Opcode::IsElementPresent)
                    listsReadInTurn.append(operation.arrayOperand());
                if (operation.opcode == Opcode::PutDoubleIndex)
                    listsThatTakeRawDoubles.append(operation.arrayOperand());
                if (operation.opcode == Opcode::PutContiguousIndex && operation.goesByKindOfStorage() && !code().isConstant(operation.arrayOperand())) {
                    ValueID list = operation.arrayOperand();
                    if (!std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == list && asked.isStoredInto; }))
                        loop.listsAskedForKind.append({ list, typeOf(operation.valueOperand()).isInt32() ? ArrayStorage::Contiguous : ArrayStorage::Double, true });
                }
                if ((operation.opcode == Opcode::GetTypedArrayElement || operation.opcode == Opcode::PutTypedArrayElement) && operation.typedArrayType() == NotTypedArray) {
                    if (!loop.viewsAskedForKind.contains(operation.viewOperand()))
                        loop.viewsAskedForKind.append(operation.viewOperand());
                }
                if (operation.opcode == Opcode::GetArrayElement && operation.level() == KnowledgeLevel::Proven && !operation.isInBounds() && !operation.isPresent() && !operation.lengthOperands().empty()) {
                    if (std::optional<int64_t> most = mostOfKeyCutByWrittenMask(operation.positionOperand()))
                        loop.boundsAskedInFront.append({ value, operation.lengthOperands()[0], *most });
                }
                if (operation.opcode == Opcode::GetArrayElement && operation.arrayStorage() == ArrayStorage::Other && operation.level() == KnowledgeLevel::Proven && operation.isInBounds() && !operation.asksCellWhereNothingIsFound() && representationOf(value) == Representation::JSValue && (typeOf(value).primitives & Primitive::Number) && !code().isConstant(operation.arrayOperand())) {
                    if (!std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == operation.arrayOperand(); }))
                        loop.listsAskedForKind.append({ operation.arrayOperand(), ArrayStorage::Double, false, true, true });
                    continue;
                }
                if (operation.opcode != Opcode::GetArrayElement || !operation.admitsInt32Storage() || code().isConstant(operation.arrayOperand()))
                    continue;
                ValueID list = operation.arrayOperand();
                if (ValueID holdsDoubles = operation.holdsDoublesOperand()) {
                    auto told = std::ranges::find(loop.kindsToldToReads, holdsDoubles, &KindToldToReads::holdsDoubles);
                    if (told == loop.kindsToldToReads.end())
                        loop.kindsToldToReads.append({ list, holdsDoubles, 1 });
                    else
                        ++told->numberOfReads;
                }
                if (!operation.isInBounds() && !operation.isPresent()) {
                    if (!loop.listsWhoseKindIsRead.contains(list))
                        loop.listsWhoseKindIsRead.append(list);
                    continue;
                }
                if (std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == list; }))
                    continue;
                bool mayHoldFraction = !!(typeOf(value).primitives & Primitive::Number & ~Primitive::NumberWithoutFraction);
                loop.listsAskedForKind.append({ list, mayHoldFraction ? ArrayStorage::Double : ArrayStorage::Int32 });
            }
            if (loop.storesIntoSlots && code().at(blockAt(place)).terminator.kind != TerminatorKind::Jump)
                return false;
        }
        for (unsigned place = 0; loop.storesIntoSlots && place < *placeOfLast; ++place) {
            unsigned numberOfBlocksOfTurnLedTo = 0;
            if (isInTurn[place]) {
                forEachPlaceLedTo(place, [&](unsigned target) {
                    numberOfBlocksOfTurnLedTo += isInTurn[target];
                });
            }
            if (isInTurn[place] && numberOfBlocksOfTurnLedTo != 1)
                return false;
        }
        return true;
    };
    if (!findTurn(placesLookedAtFirst, false) && !findTurn(placesLookedAtBehindHead, true)) {
        admitsStoresIntoSlots = false;
        if (!findTurn(placesLookedAtFirst, false) && !findTurn(placesLookedAtBehindHead, true))
            return std::nullopt;
    }
    auto isAmong = [&](ValueID list, const auto& lists) {
        return std::ranges::any_of(lists, [&](ValueID other) { return other == list || (wordOf(list) && wordOf(other) == wordOf(list)); });
    };
    if (std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.isStoredInto && (isAmong(asked.list, listsThatTakeRawDoubles) || isAmong(asked.list, listsReadInTurn)); }))
        return std::nullopt;
    for (ListAskedForKind& asked : loop.listsAskedForKind) {
        if (!isAmong(asked.list, listsThatTakeRawDoubles))
            continue;
        asked.kind = ArrayStorage::Double;
        asked.hasOneKind = true;
    }
    auto isMadeInLoop = [&](ValueID value) {
        BlockID home = code().blockOf(value);
        return home == head || loop.turn.contains(home);
    };
    if (!loop.viewsAskedForKind.isEmpty()) {
        if (std::ranges::any_of(loop.viewsAskedForKind, isMadeInLoop) || std::ranges::any_of(loop.listsAskedForKind, &ListAskedForKind::isStoredInto))
            return std::nullopt;
        for (const ListAskedForKind& asked : std::exchange(loop.listsAskedForKind, { })) {
            if (!loop.listsWhoseKindIsRead.contains(asked.list))
                loop.listsWhoseKindIsRead.append(asked.list);
        }
        loop.boundsAskedInFront.shrink(0);
    }
    loop.flatStrings.removeAllMatching(isMadeInLoop);
    loop.boundsAskedInFront.removeAllMatching([&](const BoundAskedInFront& bound) {
        return isMadeInLoop(bound.length);
    });
    loop.relationsAskedInFront.shrink(0);
    for (BlockID block : loop.turn) {
        const Terminator& end = code().at(block).terminator;
        if (end.kind != TerminatorKind::Branch || !loop.turn.contains(code().at(end.edge(0)).to) || loop.turn.contains(code().at(end.edge(1)).to) || code().at(end.edge(1)).to == head)
            continue;
        const Operation& relation = code().at(end.conditionOperand());
        if (relation.opcode != Opcode::Less || relation.form() != OperatorForm::Integers || isMadeInLoop(relation.operand(0)) || isMadeInLoop(relation.operand(1)))
            continue;
        loop.relationsAskedInFront.append(end.conditionOperand());
    }
    loop.listsWhoseKindIsRead.removeAllMatching([&](ValueID list) {
        return isMadeInLoop(list) || std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == list; });
    });
    loop.listsAskedForKind.removeAllMatching([&](const ListAskedForKind& asked) {
        return isMadeInLoop(asked.list);
    });
    loop.kindsToldToReads.removeAllMatching([&](const KindToldToReads& told) {
        return isMadeInLoop(told.holdsDoubles);
    });

    loop.asksListsReadAndListsStoredInto = std::ranges::any_of(loop.listsAskedForKind, &ListAskedForKind::isStoredInto) && std::ranges::any_of(loop.listsAskedForKind, [](const ListAskedForKind& asked) { return !asked.isStoredInto && !asked.hasOneKind; });
    const Edge& back = code().at(code().at(loop.turn.last()).terminator.next());
    auto integerHandedOnBy = [&](ValueID value) {
        static constexpr OpcodeSet handingIntegerOn { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
        while (handingIntegerOn.contains(opcodeOf(value)))
            value = code().at(value).valueOperand();
        return value;
    };
    auto broughtInTurn = [&](ValueID value) -> ValueID {
        while (true) {
            value = integerHandedOnBy(value);
            BlockID home = opcodeOf(value) == Opcode::Parameter ? code().blockOf(value) : BlockID();
            if (!home || home == head || !loop.turn.contains(home))
                return value;
            std::span<const ValueID> taken = code().at(home).parameters();
            size_t place = std::ranges::find(taken, value) - taken.begin();
            ValueID brought;
            for (BlockID from : loop.turn) {
                const Terminator& end = code().at(from).terminator;
                for (uint32_t edge = 0; edge < end.numberOfEdges; ++edge) {
                    const Edge& into = code().at(end.edge(edge));
                    if (into.to != home)
                        continue;
                    if (place >= into.arguments().size() || (brought && brought != into.arguments()[place]))
                        return { };
                    brought = into.arguments()[place];
                }
            }
            if (!brought)
                return { };
            value = brought;
        }
    };
    ValueID stepped = broughtInTurn(back.arguments()[counterAmongParameters - parameters.begin()]);
    if (!stepped)
        return std::nullopt;
    const Operation& step = code().at(stepped);
    static constexpr OpcodeSet stepsOfCounters { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
    if (!stepsOfCounters.contains(step.opcode) || step.form() != OperatorForm::Integers)
        return std::nullopt;
    ValueID steppedFrom;
    if (step.opcode == Opcode::Increment || step.opcode == Opcode::Decrement) {
        steppedFrom = step.valueOperand();
        loop.step = step.opcode == Opcode::Increment ? 1 : -1;
    } else {
        ValueID constant = step.rightOperand();
        if (!code().isConstant(constant) || opcodeOf(constant) != Opcode::Integer)
            return std::nullopt;
        steppedFrom = step.leftOperand();
        loop.step = step.opcode == Opcode::Add ? code().at(constant).integer() : -code().at(constant).integer();
    }
    bool countsUp = loop.relation == Opcode::Less || loop.relation == Opcode::LessEq;
    if (integerHandedOnBy(steppedFrom) != loop.counter || !loop.step || loop.step > 0 != countsUp || std::abs(loop.step) > largestStepOfCounter)
        return std::nullopt;
    Range counters = typeOf(loop.counter).integers;
    Range bounds = typeOf(loop.bound).integers;
    loop.mostTurns = (countsUp ? bounds.maximum - counters.minimum : counters.maximum - bounds.minimum) / std::abs(loop.step) + 1;
    if (loop.mostTurns < static_cast<int64_t>(turnsLaidOut))
        return std::nullopt;
    if (loop.step == 1 && loop.relation == Opcode::Less && counters.minimum >= 0) {
        auto holdAgainstBound = [&](ValueID length) {
            if (!loop.lengthsHeldAgainstBound.contains(length))
                loop.lengthsHeldAgainstBound.append(length);
        };
        auto isLengthFromInFront = [&](ValueID length) {
            return !isMadeInLoop(length) && typeOf(length).isInteger() && typeOf(length).integers.minimum >= 0 && (representationOf(length) == Representation::Int32 || representationOf(length) == Representation::Int64);
        };
        for (BlockID block : loop.turn) {
            for (ValueID value : code().valuesOf(block)) {
                const Operation& operation = code().at(value);
                bool testsItsPosition = operation.opcode == Opcode::IsElementPresent ? !operation.isInBounds() : operation.opcode == Opcode::GetArrayElement && operation.level() == KnowledgeLevel::Proven && !operation.isInBounds() && !operation.isPresent();
                if (!testsItsPosition || operation.lengthOperands().empty() || !isLengthFromInFront(operation.lengthOperands()[0]) || integerHandedOnBy(operation.positionOperand()) != loop.counter)
                    continue;
                loop.accessesUnderCounterBelowBound.append(value);
                holdAgainstBound(operation.lengthOperands()[0]);
            }
            const Terminator& end = code().at(block).terminator;
            if (end.kind != TerminatorKind::Branch || !loop.turn.contains(code().at(end.edge(0)).to) || loop.turn.contains(code().at(end.edge(1)).to) || code().at(end.edge(1)).to == head)
                continue;
            const Operation& relation = code().at(end.conditionOperand());
            if (relation.opcode != Opcode::Less || relation.form() != OperatorForm::Integers || integerHandedOnBy(relation.operand(0)) != loop.counter || !isLengthFromInFront(relation.operand(1)))
                continue;
            loop.relationsOfCounterBelowBound.append(end.conditionOperand());
            holdAgainstBound(relation.operand(1));
        }
    }
    if (!findStoresOfWhatComesRound(loop, back))
        return std::nullopt;
    loop.asking = asking;
    loop.mayComeBack = !m_keepsValuesInHeapFrame && loop.loopCarriedValueStores.isEmpty() && loop.mostTurns >= fewestTurnsOfLoopComeBackTo && (someUnusualTurnComesRound(loop) || someHoleLeavesTurn(loop)) && loop.viewsAskedForKind.isEmpty();
    loop.mayComeBack &= !loop.storesIntoLists;
    if (loop.mayComeBack && loop.listsAskedForKind.isEmpty() && loop.listsWhoseKindIsRead.isEmpty()) {
        BlockID head = m_order[positionOfHead];
        code().forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
            if (edge.to == head && edge.from != head && m_scopeAtEndOfBlock[edge.from.index()].block && code().at(edge.from).isInLoop)
                loop.mayComeBack = false;
        });
    }
    askKindsReadWhereLoopIsComeBackTo(loop);
    findSumsOfIntegers(loop, back);
    return loop;
}

void CodeGenerator::askKindsReadWhereLoopIsComeBackTo(CountedLoop& loop)
{
    if (!loop.mayComeBack || !loop.listsAskedForKind.isEmpty() || loop.listsWhoseKindIsRead.isEmpty())
        return;
    loop.secondTurnsGoByKindsRead = loop.listsWhoseKindIsRead.size() > 1;
    for (ValueID list : std::exchange(loop.listsWhoseKindIsRead, { }))
        loop.listsAskedForKind.append({ list, ArrayStorage::Double });
}

static_assert(!operationsOfTurnsLaidOut.contains(Opcode::GetModuleVariable) && !operationsOfTurnsLaidOut.contains(Opcode::GetEnvironmentField));

bool CodeGenerator::findStoresOfWhatComesRound(CountedLoop& loop, const Edge& back) const
{
    BlockID last = loop.turn.last();
    std::span<const ValueID> parameters = code().at(m_order[loop.positionOfHead]).parameters();
    for (ValueID value : code().valuesOf(last)) {
        const Operation& store = code().at(value);
        if (store.opcode != Opcode::PutModuleVariable && store.opcode != Opcode::PutEnvironmentField)
            continue;
        if (code().barrierIsCountedOn(value))
            return false;
        bool storesIntoScope = store.opcode == Opcode::PutEnvironmentField;
        if (storesIntoScope) {
            BlockID home = code().blockOf(store.environmentOperand());
            if (home == m_order[loop.positionOfHead] || loop.turn.contains(home) || !plan().isInUse(store.layout()))
                return false;
        }
        ValueID box = store.valueOperand();
        const Operation& boxing = code().at(box);
        bool placeHoldsValues = storesIntoScope && plan().slot(store.layout(), store.field()).representation == SlotRepresentation::JSValue;
        if (boxing.opcode != Opcode::Box || boxing.block() != last || boxing.representation != (placeHoldsValues ? Representation::JSValue : Representation::OptionalDouble) || code().useCountOf(box) != 1)
            return false;
        ValueID loopCarriedValue = boxing.valueOperand();
        if (representationOf(loopCarriedValue) != Representation::Double || !loop.turn.contains(code().blockOf(loopCarriedValue)))
            return false;
        size_t index = std::ranges::find(back.arguments(), loopCarriedValue) - back.arguments().begin();
        if (index == parameters.size() || !m_values[parameters[index].index()] || representationOf(parameters[index]) != Representation::Double)
            return false;
        for (const LoopCarriedValueStore& other : loop.loopCarriedValueStores) {
            const Operation& stored = code().at(other.store);
            if (stored.opcode != store.opcode)
                continue;
            if (storesIntoScope ? stored.layout() == store.layout() && stored.field() == store.field() : stored.variable() == store.variable())
                return false;
        }
        loop.loopCarriedValueStores.append({ index, loopCarriedValue, box, value });
    }
    if (loop.loopCarriedValueStores.isEmpty())
        return true;
    auto mayLeaveByThrow = [&](const Operation& operation) {
        if (!operation.traits().mayThrow())
            return false;
        if (operation.opcode != Opcode::GetArrayElement || operation.asksCellWhereNothingIsFound() || !hasArrayStorageForm(operation.arrayStorage()))
            return true;
        const Type& position = typeOf(operation.positionOperand());
        return !position.isInteger() || !position.integers.isSubsetOf(Range { 0, std::numeric_limits<uint32_t>::max() - 1 });
    };
    return std::ranges::none_of(loop.turn, [&](BlockID block) {
        return std::ranges::any_of(code().valuesOf(block), [&](ValueID value) {
            return mayLeaveByThrow(code().at(value));
        });
    });
}

void CodeGenerator::storeLoopCarriedValue(const CountedLoop& loop, const LoopCarriedValueStore& store, LValue value)
{
    Vector<ValueID, 4> leftOut = std::exchange(m_valuesLeftOutOfTurnsLaidOut, { });
    Vector<SumOfIntegers, 2> sums = std::exchange(m_sumsOfIntegers, { });
    LValue loopCarriedValue = std::exchange(m_values[store.loopCarriedValue.index()], emitter<ConversionEmitter>().purified(Number(value)).value());
    LValue box = std::exchange(m_values[store.box.index()], nullptr);
    generateValue(loop.turn.last(), store.box, code().at(store.box), false, { });
    generateValue(loop.turn.last(), store.store, code().at(store.store), false, { });
    m_values[store.loopCarriedValue.index()] = loopCarriedValue;
    m_values[store.box.index()] = box;
    m_sumsOfIntegers = WTF::move(sums);
    m_valuesLeftOutOfTurnsLaidOut = WTF::move(leftOut);
}

void CodeGenerator::findSumsOfIntegers(CountedLoop& loop, const Edge& back) const
{
    constexpr int64_t mostAddedInAll = int64_t { 1 } << 52;
    BlockID head = m_order[loop.positionOfHead];
    std::span<const ValueID> parameters = code().at(head).parameters();
    std::span<const BlockID> turn = loop.turn.span();
    int64_t mostTurns = loop.mostTurns;
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        ValueID sum = back.arguments()[index];
        const Operation& addition = code().at(sum);
        if (!m_values[parameter.index()] || representationOf(parameter) != Representation::Double)
            continue;
        bool isStoredToo = loop.loopCarriedValueStores.containsIf([&](const LoopCarriedValueStore& store) { return store.loopCarriedValue == sum; });
        if (addition.opcode != Opcode::Add || addition.form() != OperatorForm::Numbers || addition.representation != Representation::Double || code().useCountOf(sum) != 1u + isStoredToo)
            continue;
        if (addition.leftOperand() != parameter && addition.rightOperand() != parameter)
            continue;
        ValueID termAsNumber = addition.leftOperand() == parameter ? addition.rightOperand() : addition.leftOperand();
        const Operation& conversion = code().at(termAsNumber);
        bool termIsElementOfList = loop.holesLeaveTurns && conversion.opcode == Opcode::GetArrayElement && conversion.readsHoleAsNaN() && conversion.representation == Representation::Double
            && conversion.level() == KnowledgeLevel::Proven && (conversion.isInBounds() || conversion.isPresent())
            && (conversion.arrayStorage() == ArrayStorage::Int32 || std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == conversion.arrayOperand() && asked.kind == ArrayStorage::Int32; }));
        if (code().useCountOf(termAsNumber) != 1 || (termIsElementOfList ? !std::ranges::contains(turn, conversion.block()) : conversion.block() != addition.block()))
            continue;
        if (!termIsElementOfList && (conversion.opcode != Opcode::IntegerToNumber || representationOf(conversion.valueOperand()) != Representation::Int32))
            continue;
        Range terms = termIsElementOfList ? Range { std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() } : typeOf(conversion.valueOperand()).integers;
        int64_t largestTerm = std::max(-terms.minimum, terms.maximum);
        bool isTakenOverBehindStretches = largestTerm > mostAddedInAll / mostTurns;
        if (isTakenOverBehindStretches && (!loop.poll || mostTurns <= turnsBetweenPolls || largestTerm > mostAddedInAll / (turnsBetweenPolls + static_cast<int64_t>(turnsLaidOut))))
            continue;
        unsigned numberOfReads = 0;
        auto count = [&](ValueID read) {
            numberOfReads += read == parameter;
        };
        for (BlockID block : turn) {
            for (ValueID value : code().valuesOf(block))
                code().at(value).forEachChild(count);
            code().at(block).terminator.forEachChild(count);
            code().forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
                for (ValueID argument : edge.arguments())
                    count(argument);
            });
        }
        if (numberOfReads != 1)
            continue;
        loop.sumsOfIntegers.append({ index, sum, termAsNumber, termIsElementOfList ? termAsNumber : conversion.valueOperand(), nullptr, termIsElementOfList, nullptr, nullptr });
        loop.sumsAreTakenOverBehindStretches |= isTakenOverBehindStretches;
    }
}

bool CodeGenerator::addToSumOfIntegers(ValueID id)
{
    for (SumOfIntegers& sum : m_sumsOfIntegers) {
        if (id == sum.termAsNumber)
            return !sum.termIsElementOfList;
        if (id != sum.sum)
            continue;
        if (sum.termIsElementOfList) {
            auto noted = std::ranges::find(m_wordsOfElementsInTurnLaidOut, sum.term, [](const auto& word) { return word.first; });
            RELEASE_ASSERT(noted != m_wordsOfElementsInTurnLaidOut.end());
            sum.added = m_out.add(sum.added, m_out.signExt32To64(noted->second));
            return true;
        }
        LValue term = get(sum.term).value;
        sum.added = m_out.add(sum.added, typeOf(sum.term).integers.minimum >= 0 ? m_out.zeroExtPtr(term) : m_out.signExt32To64(term));
        return true;
    }
    return false;
}

bool CodeGenerator::liesInsideByQuestionInFrontOfTurnsLaidOut(ValueID read) const
{
    return std::ranges::contains(m_boundsAskedInFrontOfTurnsLaidOut, read, &BoundAskedInFront::read) || std::ranges::contains(m_accessesUnderCounterBelowBoundOfTurnsLaidOut, read);
}

std::optional<int64_t> CodeGenerator::mostOfKeyCutByWrittenMask(ValueID key) const
{
    Type type = typeOf(key);
    if (!type.isInteger() || type.integers.minimum < 0)
        return std::nullopt;
    while (conversionsOfForm.contains(opcodeOf(key)))
        key = code().at(key).operand(0);
    const Operation& cut = code().at(key);
    if (cut.opcode != Opcode::BitAnd || (!code().isConstant(cut.operand(0)) && !code().isConstant(cut.operand(1))))
        return std::nullopt;
    return type.integers.maximum;
}

std::optional<ArrayStorage> CodeGenerator::kindOfListFoundInFrontOfTurnsLaidOut(ValueID list) const
{
    for (const ListAskedForKind& asked : m_listsAskedForKindInFrontOfTurnsLaidOut) {
        if (asked.list == list)
            return asked.kind;
    }
    return std::nullopt;
}

std::optional<TypedArrayType> CodeGenerator::kindOfViewFoundInFrontOfTurnsLaidOut(ValueID view) const
{
    return std::ranges::contains(m_viewsAskedForKindInFrontOfTurnsLaidOut, view) ? std::optional(m_kindOfViewFoundInFrontOfTurnsLaidOut) : std::nullopt;
}

LValue CodeGenerator::holdsDoublesAsReadInFrontOfTurnsLaidOut(ValueID list) const
{
    for (auto& [read, holdsDoubles] : m_kindsReadInFrontOfTurnsLaidOut) {
        if (read == list)
            return holdsDoubles;
    }
    return nullptr;
}

LBasicBlock CodeGenerator::blockThatHoleLeavesTurnLaidOutBy()
{
    return m_holesLeaveTurnsLaidOut && m_headOfTurnsLaidOut ? m_blockLeavingTurnLaidOut() : nullptr;
}

bool CodeGenerator::asksTextsOfLoopForWidth(const CountedLoop& loop)
{
    if (loop.flatStrings.isEmpty() || !loop.listsAskedForKind.isEmpty() || !loop.listsWhoseKindIsRead.isEmpty() || loop.asksListsReadAndListsStoredInto)
        return false;
    BlockID head = m_order[loop.positionOfHead];
    TerminatorContext context(head, code().at(head).terminator, nullptr);
    SetForScope<const OperationContext*> whileAsking(m_operation, &context);
    return std::ranges::any_of(loop.flatStrings, [&](ValueID string) {
        return emitter<StringEmitter>().widthIsFoundAtRun(string);
    });
}

void CodeGenerator::layOutTurnsForOtherKindOfStorage(CountedLoop& loop, bool ofListsStoredInto)
{
    if (loop.secondTurnsGoByKindsRead) {
        for (const ListAskedForKind& asked : std::exchange(loop.listsAskedForKind, { }))
            loop.listsWhoseKindIsRead.append(asked.list);
    }
    for (ListAskedForKind& asked : loop.listsAskedForKind) {
        if (!asked.hasOneKind && (ofListsStoredInto || !asked.isStoredInto))
            asked.kind = asked.kind != ArrayStorage::Double ? ArrayStorage::Double : asked.isStoredInto ? ArrayStorage::Contiguous : ArrayStorage::Int32;
    }
    loop.sumsOfIntegers.shrink(0);
    loop.sumsAreTakenOverBehindStretches = false;
    findSumsOfIntegers(loop, code().at(code().at(loop.turn.last()).terminator.next()));
    layOutTurns(loop);
}

void CodeGenerator::layOutTurnsForEachKindOfView(CountedLoop& loop)
{
    uint32_t kinds = UINT32_MAX;
    for (ValueID view : loop.viewsAskedForKind)
        kinds &= kindsOfViewsOfNumbersIn(program(), m_inputs.types, typeIDOf(view));
    if (!kinds)
        return;
    build().statistics().count(StatisticID::UnrolledLoopsForTypedArrays);
    m_layoutsInRow = LayoutsInRow { static_cast<unsigned>(std::popcount(kinds)), nullptr, { } };
    for (uint32_t kind = 32; kind-- && !m_hasFailed;) {
        if (!(kinds >> kind & 1))
            continue;
        loop.kindAskedOfView = static_cast<TypedArrayType>(kind);
        layOutTurns(loop);
    }
}

void CodeGenerator::keepArgumentsForNextTurn(const Edge& edge)
{
    std::span<const ValueID> parameters = code().at(edge.to).parameters();
    std::span<const ValueID> arguments = edge.arguments();
    RELEASE_ASSERT(parameters.size() == arguments.size());
    m_argumentsForNextTurn.shrink(0);
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        if (!m_values[parameter.index()])
            m_argumentsForNextTurn.append(Operand());
        else if (holdsAnyNaN(arguments[index]) && letsAnyNaNInto(parameter))
            m_argumentsForNextTurn.append(getWithAnyNaN(arguments[index]));
        else
            m_argumentsForNextTurn.append(getIn(arguments[index], representationOf(parameter)));
    }
}

bool CodeGenerator::someHoleLeavesTurn(const CountedLoop& loop) const
{
    return loop.holesLeaveTurns && std::ranges::any_of(loop.turn, [&](BlockID block) {
        return std::ranges::any_of(code().valuesOf(block), [&](ValueID read) {
            return opcodeOf(read) == Opcode::GetArrayElement && code().at(read).readsHoleAsNaN() && (code().at(read).arrayStorage() == ArrayStorage::Int32 || code().at(read).admitsInt32Storage());
        });
    });
}

bool CodeGenerator::someUnusualTurnComesRound(const CountedLoop& loop) const
{
    BlockID head = m_order[loop.positionOfHead];
    auto isLedToByUnusualTurn = [&](BlockID target) {
        bool found = false;
        code().forEachIncomingEdgeUnsealed(target, [&](const Edge& edge) {
            BlockID from = edge.from;
            found |= edge.to == target && from != head && from != loop.asking && code().isReached(from) && code().at(from).frequency != BlockFrequency::Rare && !loop.turn.contains(from) && !m_scopeAtEndOfBlock[from.index()].block;
        });
        return found;
    };
    return isLedToByUnusualTurn(head) || std::ranges::any_of(loop.turn, isLedToByUnusualTurn);
}

LValue CodeGenerator::askCounter(Opcode relation, LValue counter, LValue limit)
{
    switch (relation) {
    case Opcode::Less:
        return m_out.lessThan(counter, limit);
    case Opcode::LessEq:
        return m_out.lessThanOrEqual(counter, limit);
    case Opcode::Greater:
        return m_out.greaterThan(counter, limit);
    default:
        return m_out.greaterThanOrEqual(counter, limit);
    }
}

void CodeGenerator::askLimitOfWrittenLoop(const TerminatorContext& terminator)
{
    LimitOfWrittenLoop loop = *std::exchange(m_limitOfWrittenLoop, std::nullopt);
    BranchTargets targets { edgeTo(terminator, 0), edgeTo(terminator, 1) };
    LValue counter = loop.phisOfWrittenHead[loop.indexOfCounter];
    LBasicBlock here = m_out.m_block;
    LBasicBlock limitIsReached = m_out.newBlock();
    LBasicBlock wayBack = m_out.newBlock();
    m_out.appendTo(limitIsReached);
    m_out.branch(askCounter(loop.relation, counter, loop.bound), normalEdge(wayBack), targets.notTaken);
    m_out.appendTo(wayBack);
    LValue price = m_out.constantOfType(counter->type(), turnsThatPayForComingBack * loop.step);
    LValue paid = m_out.add(loop.paidUpTo, price);
    LValue least = m_out.sub(counter, price);
    paid = m_out.select(loop.step > 0 ? m_out.lessThan(paid, least) : m_out.greaterThan(paid, least), least, paid);
    for (size_t index = 0; index < loop.phisOfEntry.size(); ++index) {
        if (!loop.phisOfEntry[index])
            continue;
        m_out.upsilonOfCompletePhi(loop.phisOfWrittenHead[index], loop.phisOfEntry[index]);
        m_phisHandedOnWhileIncomplete.append({ loop.phisOfWrittenHead[index], loop.phisOfEntry[index] });
    }
    m_out.upsilon(paid, loop.paidAtEntry);
    m_out.jump(normalEdge(loop.entry));
    m_out.appendTo(here);
    m_out.branch(askCounter(loop.relation, counter, loop.limit), targets.taken, normalEdge(limitIsReached));
}

void CodeGenerator::layOutTurns(const CountedLoop& loop)
{
    build().statistics().count(StatisticID::UnrolledLoopsEmitted);
    for (size_t stores = loop.loopCarriedValueStores.size(); stores--;)
        build().statistics().count(StatisticID::UnrolledLoopStoresSunk);
    for (size_t sums = loop.sumsOfIntegers.size(); sums--;)
        build().statistics().count(StatisticID::UnrolledLoopIntegerSums);
    BlockID head = m_order[loop.positionOfHead];
    const Block& blockOfHead = code().at(head);
    std::span<const ValueID> parameters = blockOfHead.parameters();
    std::span<const BlockID> turn = loop.turn.span();
    unsigned turnsInRow = loop.viewsAskedForKind.isEmpty() && !loop.asksListsReadAndListsStoredInto ? turnsLaidOut : turnsLaidOutForEachKindOfView;
    if (!loop.viewsAskedForKind.isEmpty())
        build().statistics().count(StatisticID::UnrolledIterationsForTypedArrays, turnsInRow);

    auto forEachParameterInTurn = [&](auto&& function) {
        for (BlockID block : turn) {
            for (ValueID parameter : code().at(block).parameters())
                function(block, parameter);
        }
    };
    auto forgetTurn = [&] {
        for (BlockID block : turn) {
            for (ValueID parameter : code().at(block).parameters())
                m_values[parameter.index()] = nullptr;
            for (ValueID value : code().valuesOf(block))
                m_values[value.index()] = nullptr;
        }
    };

    Vector<LBasicBlock, mostBlocksInTurnLaidOut> blocksOfWrittenTurn;
    Vector<LValue, 8> phisOfWrittenTurn;
    for (BlockID block : turn)
        blocksOfWrittenTurn.append(m_blocks[block.index()]);
    forEachParameterInTurn([&](BlockID, ValueID parameter) {
        phisOfWrittenTurn.append(m_values[parameter.index()]);
    });

    bool comesBack = loop.mayComeBack;
    RELEASE_ASSERT(!comesBack || (loop.loopCarriedValueStores.isEmpty() && !m_keepsValuesInHeapFrame));
    std::optional<RingOfLoopLaidOut> ring = std::exchange(m_ringOfLoopLaidOut, std::nullopt);
    RELEASE_ASSERT(!ring || (comesBack && ring->phisOfEntry.size() == parameters.size()));
    if (comesBack)
        build().statistics().count(ring ? StatisticID::UnrolledLoopsWithSharedReentry : StatisticID::UnrolledLoopsWithReentry);
    LBasicBlock entry = ring ? ring->entry : comesBack ? m_out.newBlock() : blockFor(head);
    LBasicBlock turns = m_out.newBlock();
    RELEASE_ASSERT(!m_layoutsInRow || !comesBack);
    bool isFirstInRow = m_layoutsInRow && !m_layoutsInRow->headBehindAll;
    bool isLastInRow = m_layoutsInRow && !--m_layoutsInRow->left;
    if (isFirstInRow)
        m_layoutsInRow->headBehindAll = m_out.newBlock();
    LBasicBlock writtenHead = ring ? ring->writtenHead : isLastInRow ? m_layoutsInRow->headBehindAll : m_out.newBlock();
    LBasicBlock leaving = comesBack ? m_out.newBlock() : nullptr;
    LBasicBlock intoWritten = ring ? ring->intoWritten : comesBack ? m_out.newBlock() : writtenHead;
    LBasicBlock otherKind = comesBack && (loop.layoutWithoutQuestionFollows || ((!ring || ring->nextMayOpenAnother) && (loop.asksListsForAnotherKind() || loop.turnsForWideTextsFollow))) ? m_out.newBlock() : nullptr;
    using Values = Vector<LValue, 8>;
    Values phisOfEntry;
    Values phisOfTurns;
    Values phisOfWrittenHead;
    Values phisOfHead;
    Values phisOfLeaving;
    if (ring) {
        phisOfHead = ring->phisOfHead;
        phisOfEntry = ring->phisOfEntry;
        phisOfWrittenHead = ring->phisOfWrittenHead;
        phisOfTurns.fill(nullptr, parameters.size());
    } else {
        for (ValueID parameter : parameters) {
            LValue phi = m_values[parameter.index()];
            phisOfHead.append(phi);
            phisOfEntry.append(phi && comesBack ? m_out.phiIn(entry, phi->type()) : phi);
            phisOfTurns.append(nullptr);
            if (isFirstInRow)
                m_layoutsInRow->phisBehindAll.append(phi ? m_out.phiIn(m_layoutsInRow->headBehindAll, phi->type()) : nullptr);
            phisOfWrittenHead.append(isLastInRow ? m_layoutsInRow->phisBehindAll[phisOfWrittenHead.size()] : phi ? m_out.phiIn(writtenHead, phi->type()) : nullptr);
        }
    }
    LBasicBlock headOfRest = m_layoutsInRow ? m_layoutsInRow->headBehindAll : writtenHead;
    Values phisOfRest = m_layoutsInRow ? m_layoutsInRow->phisBehindAll : phisOfWrittenHead;
    if (headOfRest != writtenHead)
        build().statistics().count(StatisticID::UnrolledLoopRemaindersShared);
    bool noTurnComesToNextFront = m_layoutsInRow && !loop.viewsAskedForKind.isEmpty();
    bool countIsAsked = noTurnComesToNextFront && !isFirstInRow;
    if (countIsAsked)
        build().statistics().count(StatisticID::UnrolledLoopTripChecksReused);
    if (isLastInRow)
        m_layoutsInRow = std::nullopt;
    m_sumsOfIntegers = loop.sumsOfIntegers;
    for (SumOfIntegers& sum : m_sumsOfIntegers) {
        sum.added = m_out.phiIn(turns, Machine::Int64);
        phisOfTurns[sum.indexOfParameter] = sum.added;
        sum.base = loop.sumsAreTakenOverBehindStretches ? m_out.phiIn(turns, Machine::Double) : phisOfEntry[sum.indexOfParameter];
    }
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (phisOfEntry[index] && !phisOfTurns[index])
            phisOfTurns[index] = m_out.phiIn(turns, phisOfEntry[index]->type());
    }
    size_t indexOfCounter = std::ranges::find(parameters, loop.counter) - parameters.begin();
    Values phisIntoWritten = phisOfWrittenHead;
    LType typeOfCounter = phisOfEntry[indexOfCounter]->type();
    LValue bound = nullptr;
    LValue limit = nullptr;
    LValue limitOfWay = nullptr;
    LValue paidUpTo = nullptr;
    LValue paidIntoWritten = nullptr;
    LValue paidOfWay = nullptr;
    if (comesBack) {
        for (size_t index = 0; index < parameters.size(); ++index) {
            phisOfLeaving.append(phisOfTurns[index] ? m_out.phiIn(leaving, phisOfTurns[index]->type()) : nullptr);
            phisIntoWritten[index] = ring ? ring->phisIntoWritten[index] : phisOfWrittenHead[index] ? m_out.phiIn(intoWritten, phisOfWrittenHead[index]->type()) : nullptr;
        }
        limit = ring ? ring->limit : m_out.phiIn(intoWritten, typeOfCounter);
        paidIntoWritten = ring ? ring->paidIntoWritten : m_out.phiIn(intoWritten, typeOfCounter);
        paidUpTo = ring ? ring->paidUpTo : m_out.phiIn(entry, typeOfCounter);
        paidOfWay = phisOfHead[indexOfCounter];
    }
    auto handOn = [&](LValue value, LValue phi) {
        m_out.upsilonOfCompletePhi(value, phi);
        if (comesBack)
            m_phisHandedOnWhileIncomplete.append({ value, phi });
    };
    bool leavesTurnsLaidOut = false;
    auto blockHandingOver = [&](const Values& phis, LBasicBlock target, auto&& valueAt) {
        LBasicBlock here = m_out.m_block;
        LBasicBlock blockOnEdge = m_out.newBlock();
        m_out.appendTo(blockOnEdge);
        Values values;
        for (size_t index = 0; index < phis.size(); ++index)
            values.append(phis[index] ? valueAt(index) : nullptr);
        if (leavesTurnsLaidOut && target != turns && !loop.loopCarriedValueStores.isEmpty()) {
            LBasicBlock behindStores = nullptr;
            if (values[indexOfCounter] == phisOfTurns[indexOfCounter]) {
                LBasicBlock someTurnHasRun = m_out.newBlock();
                behindStores = m_out.newBlock();
                m_out.branch(m_out.equal(values[indexOfCounter], phisOfEntry[indexOfCounter]), normalEdge(behindStores), normalEdge(someTurnHasRun));
                m_out.appendTo(someTurnHasRun);
            }
            for (const LoopCarriedValueStore& store : loop.loopCarriedValueStores)
                storeLoopCarriedValue(loop, store, values[store.indexOfParameter]);
            if (behindStores) {
                m_out.jump(normalEdge(behindStores));
                m_out.appendTo(behindStores);
            }
        }
        for (size_t index = 0; index < phis.size(); ++index) {
            if (!phis[index])
                continue;
            bool isCounterHandedOn = index == indexOfCounter && (values[index] == phisOfTurns[index] || (comesBack && values[index] == phisOfLeaving[index]));
            if (values[index] == phisOfHead[index] || values[index] == phisOfEntry[index] || isCounterHandedOn)
                handOn(values[index], phis[index]);
            else
                m_out.upsilon(values[index], phis[index]);
        }
        RELEASE_ASSERT(!comesBack || target != writtenHead);
        if (comesBack && target == intoWritten) {
            m_out.upsilon(limitOfWay ? limitOfWay : bound, limit);
            m_out.upsilon(paidOfWay, paidIntoWritten);
        }
        m_out.jump(normalEdge(target));
        m_out.appendTo(here);
        return blockOnEdge;
    };
    auto sumAt = [&](size_t indexOfParameter) -> const SumOfIntegers* {
        for (const SumOfIntegers& sum : m_sumsOfIntegers) {
            if (sum.indexOfParameter == indexOfParameter)
                return &sum;
        }
        return nullptr;
    };
    bool pollsBetweenStretches = loop.poll && loop.mostTurns > turnsBetweenPolls;
    bool countsUp = loop.step > 0;
    auto holdsOf = [&](LValue counter, LValue limit) {
        return askCounter(loop.relation, counter, limit);
    };
    LValue endOfStretch = pollsBetweenStretches ? m_out.phiIn(turns, phisOfEntry[indexOfCounter]->type()) : nullptr;
    auto endOfStretchFrom = [&](LValue counter, LValue lastStart) {
        LValue behindStretch = m_out.add(counter, m_out.constantOfType(counter->type(), turnsBetweenPolls * loop.step));
        return m_out.select(countsUp ? m_out.lessThan(behindStretch, lastStart) : m_out.greaterThan(behindStretch, lastStart), behindStretch, lastStart);
    };

    m_out.appendTo(ring ? ring->otherKind : comesBack ? blockFor(head) : entry);
    m_out.setOrigin(Machine::Origin());
    LValue lastStart = nullptr;
    {
        TerminatorContext context(head, blockOfHead.terminator, nullptr);
        SetForScope<const OperationContext*> whileTesting(m_operation, &context);
        bound = ring ? ring->bound : get(loop.bound).value;
        LValue counter = phisOfEntry[indexOfCounter];
        RELEASE_ASSERT(bound && counter && bound->type() == counter->type());
        int64_t reach = (turnsInRow - 1) * loop.step;
        lastStart = ring ? ring->lastStart : m_out.sub(bound, m_out.constantOfType(bound->type(), reach));
        auto handToWritten = [&](const Values& phis) {
            return blockHandingOver(phisIntoWritten, intoWritten, [&](size_t index) {
                return phis[index];
            });
        };
        auto handToTurns = [&] {
            return blockHandingOver(phisOfTurns, turns, [&](size_t index) {
                if (index == indexOfCounter && endOfStretch)
                    m_out.upsilon(endOfStretchFrom(counter, lastStart), endOfStretch);
                if (const SumOfIntegers* sum = sumAt(index); sum && loop.sumsAreTakenOverBehindStretches)
                    m_out.upsilon(phisOfEntry[index], sum->base);
                return sumAt(index) ? m_out.constInt64(0) : phisOfEntry[index];
            });
        };
        LBasicBlock written = comesBack ? nullptr : handToWritten(phisOfHead);
        LBasicBlock laidOut = comesBack ? nullptr : handToTurns();
        bool shouldSkipEntryChecks = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::SkipUnrollEntryChecks);
        auto answerTo = [&](LValue answer) {
            return shouldSkipEntryChecks ? m_out.opaque(m_out.booleanTrue) : answer;
        };
        auto tooFewForAnyLayout = [&] {
            return headOfRest == writtenHead ? written : blockHandingOver(phisOfRest, headOfRest, [&](size_t index) {
                return phisOfHead[index];
            });
        };
        if (bound->type() == Machine::Int32 && !ring && !countIsAsked) {
            Range bounds = typeOf(loop.bound).integers;
            int64_t nearest = (countsUp ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int32_t>::max()) + reach;
            if (countsUp ? bounds.minimum < nearest : bounds.maximum > nearest) {
                if (comesBack)
                    written = handToWritten(phisOfHead);
                LValue limit = m_out.constInt32(static_cast<int32_t>(nearest));
                LBasicBlock doesNotWrap = m_out.newBlock();
                m_out.branch(countsUp ? m_out.lessThan(bound, limit) : m_out.greaterThan(bound, limit), unlikelyEdge(tooFewForAnyLayout()), normalEdge(doesNotWrap));
                m_out.appendTo(doesNotWrap);
            }
        }
        if (comesBack) {
            if (!ring) {
                for (size_t index = 0; index < parameters.size(); ++index) {
                    if (phisOfEntry[index])
                        handOn(phisOfHead[index], phisOfEntry[index]);
                }
                m_out.upsilon(phisOfHead[indexOfCounter], paidUpTo);
                m_out.jump(normalEdge(entry));
                m_out.appendTo(entry);
            }
            paidOfWay = paidUpTo;
            written = handToWritten(phisOfEntry);
            laidOut = handToTurns();
        }
        if (!ring && !countIsAsked) {
            LBasicBlock enoughAreLeft = m_out.newBlock();
            bool shouldSkipTripCountCheck = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::SkipUnrollTripCountCheck);
            m_out.branch(shouldSkipTripCountCheck ? m_out.opaque(m_out.booleanTrue) : holdsOf(counter, lastStart), normalEdge(enoughAreLeft), normalEdge(tooFewForAnyLayout()));
            m_out.appendTo(enoughAreLeft);
        }
        for (size_t place = 0; place < loop.boundsAskedInFront.size() && !ring; ++place) {
            const BoundAskedInFront& bound = loop.boundsAskedInFront[place];
            bool isAskedAlready = std::ranges::any_of(loop.boundsAskedInFront.span().first(place), [&](const BoundAskedInFront& earlier) {
                return earlier.length == bound.length && earlier.most >= bound.most;
            });
            if (isAskedAlready)
                continue;
            LValue length = get(bound.length).value;
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(answerTo(m_out.lessThan(m_out.constantOfType(length->type(), bound.most), length)), normalEdge(holds), normalEdge(written));
            m_out.appendTo(holds);
        }
        for (ValueID relation : loop.relationsAskedInFront) {
            if (ring || countIsAsked)
                break;
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(answerTo(emitter<ControlEmitter>().isLess(code().at(relation).operand(0), code().at(relation).operand(1)).value()), normalEdge(holds), normalEdge(tooFewForAnyLayout()));
            m_out.appendTo(holds);
            build().statistics().count(StatisticID::UnrolledLoopComparisonsHoisted);
        }
        if (ring || countIsAsked) {
            RELEASE_ASSERT(std::ranges::all_of(loop.lengthsHeldAgainstBound, [&](ValueID length) {
                return m_lengthsHeldAgainstBoundByLayoutThatAsked.contains(length);
            }));
        } else
            m_lengthsHeldAgainstBoundByLayoutThatAsked = loop.lengthsHeldAgainstBound;
        for (ValueID asked : loop.lengthsHeldAgainstBound) {
            if (ring || countIsAsked)
                break;
            LBasicBlock holds = m_out.newBlock();
            LValue fallsShort = m_out.lessThan(m_out.zeroExt(get(asked).value, Machine::Int64), bound->type() == Machine::Int32 ? m_out.signExt32To64(bound) : bound);
            m_out.branch(shouldSkipEntryChecks ? m_out.opaque(m_out.booleanFalse) : fallsShort, normalEdge(tooFewForAnyLayout()), normalEdge(holds));
            m_out.appendTo(holds);
            build().statistics().count(StatisticID::UnrolledLoopLengthChecksHoisted);
        }
        for (ValueID string : loop.flatStrings) {
            std::optional<Condition> hasWidth = emitter<StringEmitter>().findWidth(string, !loop.asksTextsForWide);
            if (!hasWidth || hasWidth->value()->hasInt())
                continue;
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(answerTo(hasWidth->value()), normalEdge(holds), otherKind ? normalEdge(otherKind) : unlikelyEdge(written));
            m_out.appendTo(holds);
        }
        for (const ListAskedForKind& asked : loop.listsAskedForKind) {
            LBasicBlock holds = m_out.newBlock();
            Condition hasKind = asked.isStoredInto ? emitter<ElementAccessEmitter>().listTakesStoresInArrayStorage(asked.list, asked.kind) : emitter<ElementAccessEmitter>().listHasArrayStorage(asked.list, asked.kind);
            m_out.branch(answerTo(hasKind.value()), normalEdge(holds), normalEdge(otherKind ? otherKind : written));
            m_out.appendTo(holds);
        }
        for (ValueID view : loop.viewsAskedForKind) {
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(answerTo(emitter<ElementAccessEmitter>().viewIsOfKind(view, loop.kindAskedOfView).value()), normalEdge(holds), normalEdge(written));
            m_out.appendTo(holds);
        }
        bool someTurnMayLeave = std::ranges::any_of(loop.turn, [&](BlockID id) {
            const Terminator& terminator = code().at(id).terminator;
            for (uint32_t edge = 0; edge < terminator.numberOfEdges; ++edge) {
                BlockID target = code().at(terminator.edge(edge)).to;
                if (target != head && !loop.turn.contains(target))
                    return true;
            }
            return false;
        });
        for (const SumOfIntegers& sum : m_sumsOfIntegers) {
            if (someTurnMayLeave) {
                LBasicBlock isNotMinusZero = m_out.newBlock();
                m_out.branch(m_out.equal(m_out.bitCast(phisOfEntry[sum.indexOfParameter], Machine::Int64), m_out.constInt64(std::numeric_limits<int64_t>::min())), unlikelyEdge(written), normalEdge(isNotMinusZero));
                m_out.appendTo(isNotMinusZero);
            }
            LValue size = m_out.doubleAbs(phisOfEntry[sum.indexOfParameter]);
            LValue limit = m_out.constDouble(0x1p52);
            LBasicBlock isSmall = m_out.newBlock();
            m_out.branch(m_out.doubleLessThanOrEqual(size, limit), normalEdge(isSmall), unlikelyEdge(written));
            m_out.appendTo(isSmall);
            if (typeOf(parameters[sum.indexOfParameter]).isNumberWithoutFraction())
                continue;
            LBasicBlock isWhole = m_out.newBlock();
            m_out.branch(m_out.doubleEqual(m_out.doubleSub(m_out.doubleAdd(size, limit), limit), size), normalEdge(isWhole), unlikelyEdge(written));
            m_out.appendTo(isWhole);
        }
        for (ValueID list : loop.listsWhoseKindIsRead) {
            LValue holdsDoubles = emitter<ElementAccessEmitter>().listHasArrayStorage(list, ArrayStorage::Double).value();
            m_kindsReadInFrontOfTurnsLaidOut.append({ list, shouldSkipEntryChecks ? m_out.bitXor(holdsDoubles, m_out.booleanTrue) : holdsDoubles });
            build().statistics().count(StatisticID::UnrolledLoopStorageChecksHoisted);
        }
        m_out.jump(normalEdge(laidOut));
    }

    m_out.appendTo(turns);
    m_headOfTurnsLaidOut = head;
    leavesTurnsLaidOut = true;
    for (const LoopCarriedValueStore& store : loop.loopCarriedValueStores) {
        m_valuesLeftOutOfTurnsLaidOut.append(store.box);
        m_valuesLeftOutOfTurnsLaidOut.append(store.store);
    }
    Vector<size_t, 4> takingAnyNaNInTurnsAlone;
    if (m_followsNumbersOutOfBytes) {
        m_blocksBegun.quickSet(head.index());
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (!phisOfTurns[index] || sumAt(index) || representationOf(parameters[index]) != Representation::Double || holdsAnyNaN(parameters[index]))
                continue;
            m_valuesHoldingAnyNaN.quickSet(parameters[index].index());
            takingAnyNaNInTurnsAlone.append(index);
        }
    }
    auto forWrittenLoop = [&](size_t index, LValue value) {
        return takingAnyNaNInTurnsAlone.contains(index) ? emitter<ConversionEmitter>().purified(Number(value)).value() : value;
    };
    for (size_t index = 0; index < parameters.size(); ++index)
        m_values[parameters[index].index()] = sumAt(index) ? nullptr : phisOfTurns[index];
    LValue firstCounter = phisOfTurns[indexOfCounter];
    bool counterIsExtended = firstCounter->type() == Machine::Int32 && typeOf(loop.counter).integers.minimum >= 0 && countsUp;
    LValue extensionForAllTurns = counterIsExtended && isARM64() ? m_out.extendWordThatIsNotNegative(firstCounter) : nullptr;
    auto counterBehind = [&](unsigned numberOfTurns) -> LValue {
        if (counterIsExtended && numberOfTurns < turnsInRow)
            return m_out.castToInt32(m_out.add(extensionForAllTurns ? extensionForAllTurns : m_out.extendWordThatIsNotNegative(firstCounter), m_out.constInt64(numberOfTurns * loop.step)));
        return m_out.add(firstCounter, m_out.constantOfType(firstCounter->type(), numberOfTurns * loop.step));
    };
    m_turnLaidOut = turn;
    m_listsAskedForKindInFrontOfTurnsLaidOut = loop.listsAskedForKind.span();
    m_boundsAskedInFrontOfTurnsLaidOut = loop.boundsAskedInFront.span();
    m_relationsAskedInFrontOfTurnsLaidOut = loop.relationsAskedInFront;
    m_relationsAskedInFrontOfTurnsLaidOut.appendVector(loop.relationsOfCounterBelowBound);
    m_accessesUnderCounterBelowBoundOfTurnsLaidOut = loop.accessesUnderCounterBelowBound.span();
    m_viewsAskedForKindInFrontOfTurnsLaidOut = loop.viewsAskedForKind.span();
    m_kindOfViewFoundInFrontOfTurnsLaidOut = loop.kindAskedOfView;
    m_holesLeaveTurnsLaidOut = loop.holesLeaveTurns;
    m_asksElementsBehindTurns = loop.holesLeaveTurns && std::ranges::all_of(turn, [&](BlockID block) {
        return code().at(block).terminator.kind == TerminatorKind::Jump && std::ranges::all_of(code().valuesOf(block), [&](ValueID read) {
            return opcodeOf(read) != Opcode::GetArrayElement || !code().at(read).readsHoleAsNaN() || isTermOfSumOfElements(read);
        });
    });
    if (!loop.listsAskedForKind.isEmpty())
        build().statistics().count(StatisticID::UnrolledLoopsForIndexingType);
    build().statistics().count(StatisticID::UnrolledLoopIntegerElementSums, std::ranges::count_if(m_sumsOfIntegers, [](const SumOfIntegers& sum) { return sum.termIsElementOfList; }));
    for (unsigned turnLaidOut = 0; turnLaidOut < turnsInRow; ++turnLaidOut) {
        forgetTurn();
        m_wordsOfElementsInTurnLaidOut.shrink(0);
        Values valuesAheadOfTurn;
        for (size_t index = 0; index < parameters.size(); ++index) {
            const SumOfIntegers* sum = sumAt(index);
            valuesAheadOfTurn.append(sum ? sum->added : m_values[parameters[index].index()]);
        }
        LBasicBlock blockLeavingTurn = nullptr;
        m_blockLeavingTurnLaidOut = [&] {
            if (!blockLeavingTurn) {
                blockLeavingTurn = blockHandingOver(comesBack ? phisOfLeaving : noTurnComesToNextFront ? phisOfRest : phisOfWrittenHead, comesBack ? leaving : noTurnComesToNextFront ? headOfRest : writtenHead, [&](size_t index) -> LValue {
                    if (const SumOfIntegers* sum = sumAt(index))
                        return comesBack ? valuesAheadOfTurn[index] : m_out.doubleAdd(sum->base, m_out.intToDouble(valuesAheadOfTurn[index]));
                    if (index == indexOfCounter && turnLaidOut)
                        return m_out.add(firstCounter, m_out.constantOfType(firstCounter->type(), turnLaidOut * loop.step));
                    return comesBack ? valuesAheadOfTurn[index] : forWrittenLoop(index, valuesAheadOfTurn[index]);
                });
            }
            return blockLeavingTurn;
        };
        for (unsigned index = 0; index < turn.size(); ++index) {
            LBasicBlock& block = m_blocks[turn[index].index()];
            if (!index) {
                block = m_out.m_block;
                m_nextBlockGoesOnHere = true;
            } else
                block = goesOnBehindJump(loop.positionsOfTurn[index]) ? nullptr : m_out.newBlock();
        }
        size_t indexOfPhi = 0;
        forEachParameterInTurn([&](BlockID block, ValueID parameter) {
            if (LValue phi = phisOfWrittenTurn[indexOfPhi++])
                m_values[parameter.index()] = m_out.phiIn(blockFor(block), phi->type());
        });
        for (BlockID block : turn) {
            generateBlock(block);
            if (m_hasFailed)
                return;
            m_scopeAtEndOfBlock[block.index()] = m_out.forkScope();
        }
        RELEASE_ASSERT(m_nextBlockGoesOnHere && currentBlockIsOpen() && m_argumentsForNextTurn.size() == parameters.size());
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (phisOfTurns[index] && !sumAt(index))
                m_values[parameters[index].index()] = index == indexOfCounter ? counterBehind(turnLaidOut + 1) : m_argumentsForNextTurn[index].value;
        }
    }
    m_asksElementsBehindTurns = false;
    if (LValue words = std::exchange(m_boxedWordsOfElementsInTurns, nullptr)) {
        LBasicBlock someIsAbsent = blockHandingOver(comesBack ? phisOfLeaving : noTurnComesToNextFront ? phisOfRest : phisOfWrittenHead, comesBack ? leaving : noTurnComesToNextFront ? headOfRest : writtenHead, [&](size_t index) -> LValue {
            if (comesBack)
                return phisOfTurns[index];
            if (const SumOfIntegers* sum = sumAt(index))
                return m_out.doubleAdd(sum->base, m_out.intToDouble(phisOfTurns[index]));
            return forWrittenLoop(index, phisOfTurns[index]);
        });
        LBasicBlock everyOneIsThere = m_out.newBlock();
        m_out.branch(emitter<ElementAccessEmitter>().areInt32Elements(words).value(), normalEdge(everyOneIsThere), rareEdge(someIsAbsent));
        m_out.appendTo(everyOneIsThere);
    }
    for (size_t index : takingAnyNaNInTurnsAlone)
        m_valuesHoldingAnyNaN.quickClear(parameters[index].index());
    m_headOfTurnsLaidOut = BlockID();
    m_turnLaidOut = { };
    m_listsAskedForKindInFrontOfTurnsLaidOut = { };
    m_boundsAskedInFrontOfTurnsLaidOut = { };
    m_relationsAskedInFrontOfTurnsLaidOut.shrink(0);
    m_accessesUnderCounterBelowBoundOfTurnsLaidOut = { };
    m_viewsAskedForKindInFrontOfTurnsLaidOut = { };
    m_kindsReadInFrontOfTurnsLaidOut.shrink(0);
    m_holesLeaveTurnsLaidOut = false;
    m_wordsOfElementsInTurnLaidOut.shrink(0);
    m_blockLeavingTurnLaidOut = nullptr;
    m_nextBlockGoesOnHere = false;
    emitter<StringEmitter>().forgetWidthsFound();
    m_out.setOrigin(Machine::Origin());
    Values valuesBehindTurns;
    for (ValueID parameter : parameters)
        valuesBehindTurns.append(m_values[parameter.index()]);
    LValue counterBehindTurns = valuesBehindTurns[indexOfCounter];
    RELEASE_ASSERT(counterBehindTurns->type() != Machine::Int32 || Machine::clearsUpperHalf(counterBehindTurns->opcode()));
    auto goingRound = [&](LValue nextEndOfStretch) {
        return blockHandingOver(phisOfTurns, turns, [&](size_t index) {
            if (index == indexOfCounter && endOfStretch)
                m_out.upsilon(nextEndOfStretch ? nextEndOfStretch : endOfStretchFrom(m_out.opaque(counterBehindTurns), lastStart), endOfStretch);
            const SumOfIntegers* sum = sumAt(index);
            if (sum && loop.sumsAreTakenOverBehindStretches) {
                m_out.upsilon(nextEndOfStretch ? sum->base : sum->takenOver, sum->base);
                if (!nextEndOfStretch)
                    return m_out.constInt64(0);
            }
            return sum ? sum->added : valuesBehindTurns[index];
        });
    };
    if (pollsBetweenStretches) {
        LBasicBlock stretchHasEnded = m_out.newBlock();
        m_out.branch(holdsOf(counterBehindTurns, endOfStretch), normalEdge(goingRound(endOfStretch)), unlikelyEdge(stretchHasEnded));
        m_out.appendTo(stretchHasEnded);
        for (const LoopCarriedValueStore& store : loop.loopCarriedValueStores) {
            const SumOfIntegers* sum = sumAt(store.indexOfParameter);
            storeLoopCarriedValue(loop, store, sum ? m_out.doubleAdd(sum->base, m_out.intToDouble(sum->added)) : forWrittenLoop(store.indexOfParameter, valuesBehindTurns[store.indexOfParameter]));
        }
        generateValue(head, loop.poll, code().at(loop.poll), false, { });
        m_out.setOrigin(Machine::Origin());
    }
    RELEASE_ASSERT(pollsBetweenStretches || !loop.sumsAreTakenOverBehindStretches);
    if (loop.sumsAreTakenOverBehindStretches) {
        for (SumOfIntegers& sum : m_sumsOfIntegers)
            sum.takenOver = m_out.doubleAdd(sum.base, m_out.intToDouble(sum.added));
    }
    LBasicBlock again = goingRound(nullptr);
    LBasicBlock tooFewAreLeft = blockHandingOver(comesBack ? phisIntoWritten : phisOfRest, comesBack ? intoWritten : headOfRest, [&](size_t index) {
        const SumOfIntegers* sum = sumAt(index);
        if (sum)
            return sum->takenOver ? sum->takenOver : m_out.doubleAdd(sum->base, m_out.intToDouble(sum->added));
        return forWrittenLoop(index, valuesBehindTurns[index]);
    });
    if (loop.sumsAreTakenOverBehindStretches) {
        LBasicBlock enoughAreLeft = m_out.newBlock();
        m_out.branch(holdsOf(counterBehindTurns, lastStart), normalEdge(enoughAreLeft), normalEdge(tooFewAreLeft));
        m_out.appendTo(enoughAreLeft);
        for (const SumOfIntegers& sum : m_sumsOfIntegers) {
            LBasicBlock isSmall = m_out.newBlock();
            m_out.branch(m_out.doubleLessThanOrEqual(m_out.doubleAbs(sum.takenOver), m_out.constDouble(0x1p52)), normalEdge(isSmall), unlikelyEdge(tooFewAreLeft));
            m_out.appendTo(isSmall);
        }
        m_out.jump(normalEdge(again));
    } else
        m_out.branch(holdsOf(counterBehindTurns, lastStart), normalEdge(again), normalEdge(tooFewAreLeft));
    if (comesBack && leaving->numberOfIncomingEdges()) {
        m_out.appendTo(leaving);
        LValue left = phisOfLeaving[indexOfCounter];
        auto steps = [&](int64_t turns) {
            return m_out.constantOfType(typeOfCounter, turns * loop.step);
        };
        int64_t oneTurn = loop.relation == Opcode::Less ? 1 : loop.relation == Opcode::Greater ? -1 : 0;
        LValue paid = m_out.add(paidUpTo, steps(turnsThatPayForComingBack));
        LValue isPaid = countsUp ? m_out.greaterThanOrEqual(left, paid) : m_out.lessThanOrEqual(left, paid);
        LBasicBlock nothingIsPaid = m_out.newBlock();
        LBasicBlock handsOver = m_out.newBlock();
        limitOfWay = m_out.phiIn(handsOver, typeOfCounter);
        paidOfWay = m_out.phiIn(handsOver, typeOfCounter);
        m_out.upsilon(m_out.add(left, m_out.constantOfType(typeOfCounter, oneTurn)), limitOfWay);
        m_out.upsilon(paidUpTo, paidOfWay);
        m_out.branch(isPaid, normalEdge(handsOver), normalEdge(nothingIsPaid));
        m_out.appendTo(nothingIsPaid);
        auto onward = [&](LValue from, LValue distance) {
            return countsUp ? m_out.add(from, distance) : m_out.sub(from, distance);
        };
        LValue most = m_out.constantOfType(typeOfCounter, mostTurnsOwed * std::abs(loop.step));
        LValue owed = countsUp ? m_out.sub(paid, left) : m_out.sub(left, paid);
        owed = m_out.select(m_out.below(owed, most), owed, most);
        LValue stay = m_out.mul(owed, m_out.constantOfType(typeOfCounter, turnsWrittenForEachTurnOwed));
        LValue end = onward(left, stay);
        LValue room = countsUp ? m_out.sub(bound, left) : m_out.sub(left, bound);
        m_out.upsilon(m_out.select(m_out.below(room, stay), bound, end), limitOfWay);
        m_out.upsilon(m_out.sub(onward(end, owed), steps(turnsThatPayForComingBack)), paidOfWay);
        m_out.jump(normalEdge(handsOver));
        m_out.appendTo(handsOver);
        m_out.jump(normalEdge(blockHandingOver(phisIntoWritten, intoWritten, [&](size_t index) -> LValue {
            return sumAt(index) ? m_out.doubleAdd(sumAt(index)->base, m_out.intToDouble(phisOfLeaving[index])) : forWrittenLoop(index, phisOfLeaving[index]);
        })));
        limitOfWay = nullptr;
        paidOfWay = paidUpTo;
    }
    m_sumsOfIntegers.shrink(0);
    m_valuesLeftOutOfTurnsLaidOut.shrink(0);

    forgetTurn();
    for (unsigned index = 0; index < turn.size(); ++index)
        m_blocks[turn[index].index()] = blocksOfWrittenTurn[index];
    size_t indexOfPhi = 0;
    forEachParameterInTurn([&](BlockID, ValueID parameter) {
        m_values[parameter.index()] = phisOfWrittenTurn[indexOfPhi++];
    });
    m_blocks[head.index()] = writtenHead;
    for (size_t index = 0; index < parameters.size(); ++index)
        m_values[parameters[index].index()] = phisOfWrittenHead[index];
    if (comesBack && !ring) {
        m_out.appendTo(intoWritten);
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (phisOfWrittenHead[index])
                handOn(phisIntoWritten[index], phisOfWrittenHead[index]);
        }
        m_out.jump(normalEdge(writtenHead));
        m_limitOfWrittenLoop = LimitOfWrittenLoop { loop.asking, loop.relation, loop.step, indexOfCounter, bound, limit, paidIntoWritten, paidUpTo, entry, phisOfEntry, phisOfWrittenHead };
    }
    if (otherKind)
        m_ringOfLoopLaidOut = RingOfLoopLaidOut { entry, otherKind, intoWritten, writtenHead, bound, lastStart, limit, paidIntoWritten, paidUpTo, phisOfHead, phisOfEntry, phisIntoWritten, phisOfWrittenHead, loop.layoutWithoutQuestionFollows };
}

void CodeGenerator::declareAccessesSlow(const OperationContext& context)
{
    if (m_initializationDepth) {
        m_out.clearDeclaredAccesses();
        return;
    }

    Effects effects = effectsOf(context);
    HeapList reads;
    HeapList writes;
    heaps().appendHeaps(effects.reads, reads);
    heaps().appendHeaps(effects.writes, writes);
    heaps().appendHeapsOfConvertedCells(reads);
    if (effects.mayCollect)
        heaps().appendHeapsWrittenByCollection(writes);
    if (AOTHeaps::mayConvertCells(effects))
        heaps().appendHeapsOfConvertedCells(writes);
    m_out.setDeclaredAccesses(m_proc.heapSet(reads.span()), m_proc.heapSet(writes.span()));
}

LBasicBlock CodeGenerator::blockFor(BlockID id) const
{
    LBasicBlock block = m_blocks[id.index()];
    RELEASE_ASSERT(block);
    return block;
}

BlockEdge CodeGenerator::edgeTo(const TerminatorContext& terminator, uint32_t targetIndex, std::span<const Operand> declaredResults)
{
    const Edge& edge = code().at(terminator->edge(targetIndex));
    const Block& blockAtEnd = code().at(edge.to);
    if (leavesTurnLaidOut(edge.to)) [[unlikely]]
        return edgeLeavingTurnLaidOut(blockAtEnd);
    BlockEdge target(blockFor(edge.to), edgeClassLeadingTo(blockAtEnd.frequency));
    for (ValueID parameter : blockAtEnd.parameters()) {
        if (m_values[parameter.index()])
            return edgeThatPassesArguments(terminator, edge, declaredResults);
    }
    if (m_keepsValuesInHeapFrame && emitter<SavedFrameEmitter>().clearsWordsOnWayBackAlong(edge)) [[unlikely]]
        return edgeThatPassesArguments(terminator, edge, declaredResults);
    return target;
}

std::optional<BlockEdge> CodeGenerator::edgeThatBringsNothing(EdgeID id)
{
    const Edge& edge = code().at(id);
    const Block& blockAtEnd = code().at(edge.to);
    LBasicBlock block = m_blocks[edge.to.index()];
    if (!block || isLayingOutTurns() || m_keepsValuesInHeapFrame || !edge.arguments().empty() || !blockAtEnd.parameters().empty())
        return std::nullopt;
    return BlockEdge(block, edgeClassLeadingTo(blockAtEnd.frequency));
}

NEVER_INLINE BlockEdge CodeGenerator::edgeLeavingTurnLaidOut(const Block& blockAtEnd)
{
    return BlockEdge(m_blockLeavingTurnLaidOut(), edgeClassLeadingTo(blockAtEnd.frequency));
}

NEVER_INLINE BlockEdge CodeGenerator::edgeThatPassesArguments(const TerminatorContext& terminator, const Edge& edge, std::span<const Operand> declaredResults)
{
    const Block& blockAtEnd = code().at(edge.to);
    BlockEdge target(blockFor(edge.to), edgeClassLeadingTo(blockAtEnd.frequency));
    std::span<const ValueID> parameters = blockAtEnd.parameters();
    if (terminator.kind() == TerminatorKind::Jump) {
        passArguments(edge, parameters, declaredResults);
        return target;
    }

    LBasicBlock here = m_out.m_block;
    LBasicBlock blockOnEdge = m_out.newBlock();
    m_out.appendTo(blockOnEdge);
    passArguments(edge, parameters, declaredResults);
    m_out.jump(target);
    m_out.appendTo(here);
    return BlockEdge(blockOnEdge, target.edgeClass());
}

void CodeGenerator::jumpTo(const TerminatorContext& terminator, uint32_t targetIndex, std::span<const Operand> declaredResults)
{
    const Edge& edge = code().at(terminator->edge(targetIndex));
    if (edge.to == m_headOfTurnsLaidOut) [[unlikely]] {
        keepArgumentsForNextTurn(edge);
        m_nextBlockGoesOnHere = true;
        return;
    }
    if (leavesTurnLaidOut(edge.to)) [[unlikely]] {
        m_out.jump(BlockEdge(m_blockLeavingTurnLaidOut(), edgeClassLeadingTo(code().at(edge.to).frequency)));
        return;
    }
    LBasicBlock& machineBlock = m_blocks[edge.to.index()];
    if (!machineBlock) {
        machineBlock = m_out.m_block;
        m_nextBlockGoesOnHere = true;
        return;
    }
    const Block& blockAtEnd = code().at(edge.to);
    passArguments(edge, blockAtEnd.parameters(), declaredResults);
    m_out.jump(BlockEdge(machineBlock, edgeClassLeadingTo(blockAtEnd.frequency)));
}

void CodeGenerator::passHandlerArguments(const Edge& handler)
{
    SetForScope<bool> whilePassing(m_isPassingHandlerArguments, true);
    passArguments(handler, code().at(handler.to).parameters(), { });
}

NEVER_INLINE void CodeGenerator::failToPassArgument(ValueID parameter, Representation found, Representation wanted)
{
    String forms = describeFormsOfGeneratedOperand(found, wanted);
    if (m_operation)
        fail(*m_operation, InternalErrorKind::UnsupportedConversion, forms);
    else
        fail(ValueContext(code().blockOf(parameter), parameter, code().at(parameter), nullptr), InternalErrorKind::UnsupportedConversion, forms);
}

ALWAYS_INLINE_INTO_OTHER_FILES void CodeGenerator::passArgument(ValueID parameter, Operand argument)
{
    Representation representationOfParameter = isCarriedBoxed(parameter) ? Representation::JSValue : representationOf(parameter);
    if (!isSameWord(argument.representation, representationOfParameter)) [[unlikely]] {
        failToPassArgument(parameter, argument.representation, representationOfParameter);
        return;
    }
    LValue phi = m_values[parameter.index()];
    if (phi && phi->opcode() != Machine::Phi) [[unlikely]]
        phi = m_keepsValuesInHeapFrame ? emitter<SavedFrameEmitter>().phiOfKeptParameter(parameter) : nullptr;
    RELEASE_ASSERT(phi && phi->opcode() == Machine::Phi && argument.value && argument.value->type() == phi->type());
    m_out.upsilon(argument.value, phi);
}

void CodeGenerator::passArguments(const Edge& edge, std::span<const ValueID> parameters, std::span<const Operand> declaredResults)
{
    std::span<const ValueID> arguments = edge.arguments();
    RELEASE_ASSERT(parameters.size() == arguments.size() + declaredResults.size());
    if (m_keepsValuesInHeapFrame && !m_isPassingHandlerArguments) [[unlikely]]
        emitter<SavedFrameEmitter>().clearWordsOnWayBackAlong(edge);
    if (parameters.empty())
        return;

    Vector<Operand, 8> passed;
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        if (!m_values[parameter.index()])
            passed.append(Operand());
        else if (isCarriedBoxed(parameter)) [[unlikely]] {
            RELEASE_ASSERT(m_operation);
            bool isArgument = index < arguments.size();
            Operand operand = isArgument ? get(arguments[index]) : declaredResults[index - arguments.size()];
            passed.append(emitter<ConversionEmitter>().box(*m_operation, operand, typeOf(isArgument ? arguments[index] : parameter)));
        } else if (index < arguments.size() && holdsAnyNaN(arguments[index]) && letsAnyNaNInto(parameter))
            passed.append(getWithAnyNaN(arguments[index]));
        else if (index < arguments.size())
            passed.append(getIn(arguments[index], representationOf(parameter)));
        else
            passed.append(declaredResults[index - arguments.size()]);
    }
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (m_values[parameters[index].index()])
            passArgument(parameters[index], passed[index]);
    }
}

NEVER_INLINE Operand CodeGenerator::getSlow(ValueID id, const Operation& operation)
{
    Representation representation = operation.representation;
    RELEASE_ASSERT(representation != Representation::Unchosen);
    if (representation == Representation::Void)
        return { };
    BlockID home = operation.block();
    if (!home)
        return makeConstant(id, operation);
    if (BlockID user = blockBeingGenerated(); user && user != home && home != m_onlyEntryBlock && !isProvenToDominate(home, user))
        verifyThatDefinitionDominates(id, home, user);
    if (m_keepsValuesInHeapFrame) [[unlikely]]
        verifyThatKeptValueIsNamedInThisStretch(id);
    LValue lowered = m_values[id.index()];
    if (!lowered) [[unlikely]]
        return zeroInPlaceOfValueOfFailedCode(representation);
    if (representation == Representation::Double && (m_carriesParametersBoxed || m_followsNumbersOutOfBytes)) [[unlikely]]
        return getNumberInFormKept(id, lowered);
    if (representation == Representation::OptionalDouble && holdsAnyNaN(id)) [[unlikely]]
        failAtNumberOrNothingReadForItsBits(id);
    return { representation, lowered };
}

Operand CodeGenerator::zeroInPlaceOfValueOfFailedCode(Representation representation)
{
    RELEASE_ASSERT(m_hasFailed);
    return emitter<ConstantEmitter>().zeroOf(representation);
}

Operand CodeGenerator::getNumberInFormKept(ValueID id, LValue lowered)
{
    if (m_carriesParametersBoxed && m_parametersCarriedBoxed.quickGet(id.index()))
        return { Representation::JSValue, lowered };
    if (m_followsNumbersOutOfBytes && m_valuesHoldingAnyNaN.quickGet(id.index()))
        return emitter<ConversionEmitter>().purified(Number(lowered));
    return { Representation::Double, lowered };
}

void CodeGenerator::failAtNumberOrNothingReadForItsBits(ValueID id)
{
    fail(*m_operation, InternalErrorKind::UnsupportedConversion, makeString('v', id.index(), " ("_s, nameOf(opcodeOf(id)), ") is kept as its reader left it, for users that take it so, and is read by one that does not"_s));
}

void CodeGenerator::verifyThatKeptValueIsNamedInThisStretch(ValueID id)
{
    if (emitter<SavedFrameEmitter>().isReadAsNamedInAnotherStretch(id))
        fail(*m_operation, InternalErrorKind::InvalidSuspension, makeString('v', id.index(), " ("_s, nameOf(opcodeOf(id)), ") has a word in the machine frame and is read as another stretch named it"_s));
}

ALWAYS_INLINE_INTO_OTHER_FILES void CodeGenerator::verifyThatDefinitionDominates(ValueID id, BlockID home, BlockID user)
{
    if (dominates(home, user)) [[likely]] {
        if (m_userOfHomesProven != user) {
            m_userOfHomesProven = user;
            m_homesProven.fill(BlockID());
        }
        m_homesProven[home.index() % m_homesProven.size()] = home;
        return;
    }
    if (!m_dominators)
        m_dominators.emplace(code(), m_order);
    if (code().isReached(home) && m_dominators->dominates(home, user))
        return;
    fail(*m_operation, InternalErrorKind::UseNotDominated, makeString('v', id.index(), " ("_s, nameOf(opcodeOf(id)), ") is used in b"_s, user.index(), ", which b"_s, home.index(), ", where it is defined, does not dominate"_s));
}

Operand CodeGenerator::getWithAnyNaN(ValueID id)
{
    if (!holdsAnyNaN(id))
        return get(id);
    return { Representation::Double, m_values[id.index()] };
}

Number CodeGenerator::settleNaNOf(const ValueContext& value, Number number)
{
    if (!m_followsNumbersOutOfBytes || m_definitionTaken || m_valuesReadForBitsOfNaN.quickGet(value.id().index()))
        return emitter<ConversionEmitter>().purified(number);
    m_valuesHoldingAnyNaN.quickSet(value.id().index());
    return number;
}

void CodeGenerator::noteThatNumberIsNoNaN(const ValueContext& value)
{
    RELEASE_ASSERT(!m_values[value.id().index()]);
    if (m_followsNumbersOutOfBytes)
        m_valuesHoldingAnyNaN.quickClear(value.id().index());
}

bool CodeGenerator::takesNothingForNaN(const ValueContext& value)
{
    if (!readsNumberOrNothingOutOfBytes(value.value()) || !m_followsNumbersOutOfBytes || m_definitionTaken || m_valuesReadForBitsOfNaN.quickGet(value.id().index()))
        return false;
    m_valuesHoldingAnyNaN.quickSet(value.id().index());
    return true;
}

bool CodeGenerator::letsAnyNaNInto(ValueID parameter)
{
    if (holdsAnyNaN(parameter))
        return true;
    if (representationOf(parameter) != Representation::Double || m_valuesReadForBitsOfNaN.quickGet(parameter.index()) || m_blocksBegun.quickGet(code().blockOf(parameter).index()))
        return false;
    m_valuesHoldingAnyNaN.quickSet(parameter.index());
    return true;
}

Operand CodeGenerator::makeInWantedForm(ValueID id, const Operation& operation, Representation wanted)
{
    RELEASE_ASSERT(m_operation);
    bool canBeMadeInWantedForm = code().isConstant(id) && constantsInFormOfUses.contains(operation.opcode);
    if (canBeMadeInWantedForm) {
        Type type = typeOf(id);
        canBeMadeInWantedForm = isLegalRepresentation(type, wanted) && !(wanted == Representation::Cell32 && type.hasCells());
    }
    if (!canBeMadeInWantedForm) {
        fail(*m_operation, InternalErrorKind::UnsupportedConversion, describeFormsOfGeneratedOperand(operation.representation, wanted));
        return emitter<ConstantEmitter>().zeroOf(wanted);
    }

    Operation inWantedForm = operation;
    inWantedForm.representation = wanted;
    BlockID block = m_operation->block();
    Operand made;
    {
        SetForScope<Operand*> definitionTaken(m_definitionTaken, &made);
        generate(ValueContext(block, id, inWantedForm, handlerOf(block)));
    }
    if (made.representation == wanted)
        return made;
    RELEASE_ASSERT(m_hasFailed);
    return emitter<ConstantEmitter>().zeroOf(wanted);
}

ALWAYS_INLINE_INTO_OTHER_FILES Operand CodeGenerator::makeConstant(ValueID id, const Operation& constant)
{
    static constexpr OpcodeSet constantsWithoutCode { Opcode::Undefined, Opcode::Null, Opcode::Absent, Opcode::Boolean, Opcode::Integer, Opcode::Double };

    RELEASE_ASSERT(m_operation);
    bool isKeptForLaterUses = !m_notesForms && !constant.block() && constantsWithoutCode.contains(constant.opcode);
    if (isKeptForLaterUses) {
        if (LValue kept = m_values[id.index()])
            return { constant.representation, kept };
    }
    BlockID block = m_operation->block();
    Operand made;
    {
        SetForScope<Operand*> definitionTaken(m_definitionTaken, &made);
        generate(ValueContext(block, id, constant, handlerOf(block)));
    }
    if (made.representation == constant.representation) {
        if (isKeptForLaterUses && made.value && made.value->isConstant())
            m_values[id.index()] = made.value;
        return made;
    }
    RELEASE_ASSERT(m_hasFailed);
    return emitter<ConstantEmitter>().zeroOf(constant.representation);
}

void CodeGenerator::defineTest(const ValueContext& value, Condition result)
{
    if (!value.isBranchCondition()) {
        define(value, result);
        return;
    }
    LValue& lowered = m_values[value.id().index()];
    RELEASE_ASSERT(!lowered);
    lowered = result.value();
    m_out.branch(result.value(), value.branchTargets());
}

void CodeGenerator::defineResults(const ValueContext& value, std::span<const Operand> results)
{
    if (m_resultRanges.empty())
        m_resultRanges = m_proc.arena().createSpan<ResultRange>(code().numberOfValues());
    ResultRange& range = m_resultRanges[value.id().index()];
    RELEASE_ASSERT(!range);
    range = ResultRange { static_cast<uint32_t>(m_results.size()), static_cast<uint32_t>(results.size()) };
    m_results.append(results);
}

Operand CodeGenerator::resultAt(ValueID producer, uint32_t index) const
{
    RELEASE_ASSERT(hasResults(producer.index()));
    const ResultRange& range = m_resultRanges[producer.index()];
    RELEASE_ASSERT(index < range.count);
    return m_results[range.first + index];
}

void CodeGenerator::defineParameter(ValueID parameter, Operand operand)
{
    RELEASE_ASSERT(opcodeOf(parameter) == Opcode::Parameter);
    if (m_values[parameter.index()])
        passArgument(parameter, operand);
}

ALWAYS_INLINE_INTO_OTHER_FILES void CodeGenerator::defineParameterAsPassed(ValueID parameter, Operand passed)
{
    RELEASE_ASSERT(opcodeOf(parameter) == Opcode::Parameter);
    Representation representation = representationOf(parameter);
    RELEASE_ASSERT(representation != Representation::Unchosen);
    if (!code().isNeeded(parameter) || !hasRegister(representation))
        return;
    if (!isSameWord(passed.representation, representation)) [[unlikely]] {
        failToPassArgument(parameter, passed.representation, representation);
        return;
    }
    defineValue(parameter, representation, Operand(representation, passed.value));
}

void CodeGenerator::goOnInBlockOfEntry(BlockID block)
{
    RELEASE_ASSERT(goesOnInBlockOfEntry(block) && !m_blocks[block.index()] && currentBlockIsOpen());
    m_blocks[block.index()] = m_out.m_block;
    m_nextBlockGoesOnHere = true;
}

Subject CodeGenerator::subjectOf(const OperationContext& context) const
{
    Subject subject { .function = unit().function, .location = code().locationOf(program(), context.origin()) };
    if (!hasModule(subject.location))
        subject.location = function().declarationLocation();
    if (hasModule(subject.location)) {
        subject.block = context.block();
        subject.value = context.valueID();
    }
    return subject;
}

void CodeGenerator::fail(const OperationContext& context, InternalErrorKind kind, const String& detail)
{
    if (m_hasFailed)
        return;
    m_hasFailed = true;
    Subject subject = subjectOf(context);
    ASCIILiteral operation = nameOfGeneratedOperation(context);
    String ending = detail;
    if (!hasModule(subject.location))
        ending = detail.isNull() ? String(operation) : makeString(operation, ": "_s, detail);
    build().diagnostics().add(InternalError { .kind = kind, .function = subject.function, .block = subject.block, .value = subject.value, .location = subject.location, .detail = ending, .operation = operation });
}

void CodeGenerator::failAtLimit(const OperationContext& context, LimitID limit, uint64_t needed)
{
    failAtLimit(limit, needed, subjectOf(context));
}

void CodeGenerator::failAtLimit(LimitID limit, uint64_t needed, const Subject& subject)
{
    if (m_hasFailed)
        return;
    m_hasFailed = true;
    build().reportLimitExceeded(limit, needed, subject);
}

bool CodeGenerator::currentBlockIsOpen() const
{
    return !m_out.m_block->isTerminated();
}

CodeGenerator::InitializationScope::InitializationScope(CodeGenerator& generator, FillsCellOfHeap fillsCellOfHeap)
    : m_generator(generator)
{
    if (generator.m_initializationDepth++)
        return;
    generator.m_owesFenceBehindStores |= fillsCellOfHeap == FillsCellOfHeap::Yes;
    if (generator.m_proc.traits().checksAllocation)
        generator.m_out.clearDeclaredAccesses();
}

CodeGenerator::InitializationScope::~InitializationScope()
{
    if (!--m_generator.m_initializationDepth && m_generator.m_operation)
        m_generator.declareAccessesOf(*m_generator.m_operation);
}

DominatorsOfCode::DominatorsOfCode(const Code& code, std::span<const BlockID> order)
{
    constexpr uint32_t unknown = UINT32_MAX;
    struct Node {
        uint32_t dominator;
        uint32_t sizeOfSubtree;
        uint32_t nextNumberBelow;
    };
    const uint32_t numberOfBlocks = order.size();
    const uint32_t root = numberOfBlocks;
    m_sizeOfSubtree.fill(0, code.numberOfBlocks());
    m_firstNumberOfSubtree.fill(unknown, code.numberOfBlocks());
    Vector<uint32_t>& numbers = m_firstNumberOfSubtree;
    for (uint32_t number = 0; number < numberOfBlocks; ++number)
        numbers[order[number].index()] = number;
    Vector<Node, 64> nodes;
    nodes.fill(Node { unknown, 1, 0 }, numberOfBlocks + 1);
    nodes[root].dominator = root;
    for (const EntryOfCode& entry : code.entries())
        nodes[numbers[entry.block.index()]].dominator = root;

    auto handTo = [&](uint32_t number, EdgeID edge) -> bool {
        uint32_t target = numbers[code.at(edge).to.index()];
        uint32_t known = nodes[target].dominator;
        if (known == root)
            return false;
        uint32_t found = known;
        uint32_t other = number;
        if (found != unknown) {
            while (found != other) {
                if (other == root || (found != root && found > other))
                    found = nodes[found].dominator;
                else
                    other = nodes[other].dominator;
            }
        }
        if (known == other)
            return false;
        nodes[target].dominator = other;
        return target <= number;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t number = 0; number < numberOfBlocks; ++number) {
            BlockID block = order[number];
            const Terminator& end = code.at(block).terminator;
            for (uint32_t index = 0; index < end.numberOfEdges; ++index)
                changed |= handTo(number, end.edge(index));
            if (EdgeID handler = code.handlerOf(block))
                changed |= handTo(number, handler);
        }
    }

    for (uint32_t number = numberOfBlocks; number--;)
        nodes[nodes[number].dominator].sizeOfSubtree += nodes[number].sizeOfSubtree;
    for (uint32_t number = 0; number < numberOfBlocks; ++number) {
        Node& node = nodes[number];
        uint32_t& nextNumberBelowDominator = nodes[node.dominator].nextNumberBelow;
        uint32_t first = nextNumberBelowDominator;
        nextNumberBelowDominator += node.sizeOfSubtree;
        node.nextNumberBelow = first + 1;
        m_firstNumberOfSubtree[order[number].index()] = first;
        m_sizeOfSubtree[order[number].index()] = node.sizeOfSubtree;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
