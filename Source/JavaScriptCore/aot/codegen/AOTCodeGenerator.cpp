/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTCodeRecords.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTEffectsNet.h"
#include "AOTEffectsOfOperations.h"
#include "AOTEntryEmitter.h"
#include "AOTHintEmitter.h"
#include "AOTIRProgram.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTTypeIDs.h"
#include <algorithm>
#include <type_traits>
#include <wtf/BitVector.h>
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
    m_mayReleaseBehindLastReads = code().opcodesOfValues().contains(Opcode::ReleaseHint);
    orderBlocks();
    createBlocks();
    followNumbersOutOfBytes();
    createPhis();

    definePinnedValues(m_out, m_pinned);
    emitter<EntryEmitter>().generateEntries();
    for (size_t position = 0; position < m_order.size(); ++position) {
        if (m_hasFailed)
            return GenerationResult::Failed;
        BlockID id = m_order[position];
        if (leavesOutBlockThatNothingLeadsTo(id))
            continue;
        const Block& block = code().at(id);
        if (block.isLoopHead) {
            if (std::optional<CountedLoop> loop = findCountedLoopAt(position))
                layOutTurns(*loop);
            if (m_hasFailed)
                return GenerationResult::Failed;
        }
        generateBlock(id, block);
        new (NotNull, &m_scopeAtEndOfBlock[id.index()]) ScopeFork(m_out.forkScope());
    }
    for (LBasicBlock leftOut : m_blocksLeftOut)
        RELEASE_ASSERT(m_hasFailed || !leftOut->numberOfIncomingEdges());
    return m_hasFailed ? GenerationResult::Failed : GenerationResult::Generated;
}

ALWAYS_INLINE bool CodeGenerator::leavesOutBlockThatNothingLeadsTo(BlockID id)
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
    if (m_rowThatSaysLess) [[unlikely]]
        RowThatSaysLess::decode(m_rowThatSaysLess).cut(KindOfEffectsRow::Operation, static_cast<unsigned>(operation.opcode), effects);
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

ALWAYS_INLINE bool CodeGenerator::dominates(BlockID home, BlockID user)
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

ALWAYS_INLINE bool CodeGenerator::goesOnBehindJump(size_t positionInOrder) const
{
    if (!positionInOrder)
        return false;
    BlockID id = m_order[positionInOrder];
    BlockID inFront = m_order[positionInOrder - 1];
    if (m_singlePredecessors[id.index()] != inFront)
        return false;
    const Block& blockInFront = code().at(inFront);
    const Block& block = code().at(id);
    if (blockInFront.terminator.kind != TerminatorKind::Jump || blockInFront.frequency != block.frequency)
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
            if (readsNumberOutOfBytes(operation))
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

ALWAYS_INLINE ValueID CodeGenerator::testReadByBranchAlone(BlockID id, const Terminator& terminator) const
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

ALWAYS_INLINE bool CodeGenerator::isAwaitedAtOnce(const Operation& call, std::span<const ValueID> valuesOfBlock, size_t index) const
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
    if (operation.block() != block || code().useCountOf(sum) != 1 || !code().releasesBehindLastReadsIn(block).empty())
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
        emitter<RuntimeCallEmitter>().beginWatchOfOperation(context, watch);
        dispatch(context);
        if (watch.isSet() && !m_hasFailed && currentBlockIsOpen() && !isNeverType(context->type))
            emitter<RuntimeCallEmitter>().endWatchOfEffects(context, watch, nullptr);
        else
            emitter<RuntimeCallEmitter>().abandonWatchOfOperation(watch);
    } else
        dispatch(context);

    if (m_notesForms)
        m_records.formsSaid.leave(siteOfUser, m_proc.numberOfOperations());
}

ALWAYS_INLINE void CodeGenerator::generate(const ValueContext& context)
{
    const OperationContext* user = std::exchange(m_operation, &context);
    Machine::Origin originOfUser = m_out.origin();
    m_out.setOrigin(machineOriginOf(context.id()));
    if (m_watchesOperations) [[unlikely]]
        dispatchWatched(context);
    else
        dispatch(context);
    if (!m_closuresWhoseLivesEndBehindCalls.isEmpty()) [[unlikely]] {
        m_closuresWhoseLivesEndBehindCalls.removeAllMatching([&](const std::pair<ValueID, ValueID>& lent) {
            if (lent.second != context.id())
                return false;
            if (!m_hasFailed && currentBlockIsOpen())
                emitter<HintEmitter>().endLifeOf(context, lent.first);
            return true;
        });
    }
    m_out.setOrigin(originOfUser);
    m_operation = user;
    if (m_checksAllocation) [[unlikely]] {
        if (user)
            declareAccessesSlow(*user);
        else
            m_out.clearDeclaredAccesses();
    }

    size_t index = context.id().index();
    if (m_values[index] || !hasRegister(context.representation())) [[likely]]
        return;
    if (m_hasFailed || m_definitionTaken || context.isBranchCondition() || context.returnCall() || isNeverType(context->type))
        return;
    RELEASE_ASSERT(hasResults(index));
}

ALWAYS_INLINE void CodeGenerator::generateValue(BlockID block, ValueID id, const Operation& operation, bool isAwaitedAtOnce, std::span<const ValueID> operationsBehind)
{
    if (!m_sumsOfIntegers.isEmpty() && addToSumOfIntegers(id)) [[unlikely]]
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
    if (operation.traits().mayThrow())
        m_isAheadOfWhatMayThrow = false;
    ValueContext context(block, id, operation, handlerOf(block));
    context.m_isAwaitedAtOnce = isAwaitedAtOnce;
    context.m_operationsBehind = operationsBehind;
    generate(context);
}

ALWAYS_INLINE void CodeGenerator::generateTerminator(BlockID id, const Block& block, bool generatesCondition)
{
    RELEASE_ASSERT(block.hasEnded() && !m_operation);
    const Terminator& terminator = block.terminator;
    m_isAheadOfWhatMayThrow = false;
    TerminatorContext context(id, terminator, handlerOf(id));
    context.m_generatesCondition = generatesCondition;

    m_out.setOrigin(m_proc.traits().notesRangesOfOrigins ? machineOriginOf(terminator.kind) : Machine::Origin());

    m_operation = &context;
    declareAccessesOf(context);
    dispatch(context);
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

ValueID CodeGenerator::readLeftToBranchOnItsTruth(BlockID id, std::span<const ValueID> values, ValueID test)
{
    if (!test || values.size() < 2 || values.back() != test || code().at(id).isInLoop || m_headOfTurnsLaidOut || code().handlerOf(id))
        return { };
    const Operation& asked = code().at(test);
    ValueID read = values[values.size() - 2];
    if (asked.opcode != Opcode::ToBoolean || asked.form() != OperatorForm::Generic || asked.valueOperand() != read || code().useCountOf(read) != 1)
        return { };
    return code().at(read).opcode == Opcode::PropGet && emitter<AccessEmitter>().readsTruthAtSite(id, read) ? read : ValueID();
}

std::optional<CodeGenerator::WaysOfTruth> CodeGenerator::waysOfTruthOf(const ValueContext& read) const
{
    BlockID id = read.block();
    const Block& block = code().at(id);
    std::span<const ValueID> behind = read.operationsBehind();
    if (block.isInLoop || m_headOfTurnsLaidOut || behind.size() != 1 || !code().releasesBehindLastReadsIn(id).empty())
        return std::nullopt;
    auto testOfEnd = [&](BlockID tested, const Block& ended) -> const Operation* {
        ValueID test = testReadByBranchAlone(tested, ended.terminator);
        return test && code().at(test).valueOperand() == read.id() ? &code().at(test) : nullptr;
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
    static constexpr OpcodeSet tests { Opcode::ToBoolean, Opcode::TypeTest };
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

void CodeGenerator::generateReturnedCall(const TerminatorContext& terminator, ValueID call)
{
    ValueContext context(terminator.block(), call, code().at(call), handlerOf(terminator.block()));
    context.m_returnCall = &terminator;
    generate(context);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

void CodeGenerator::generateBlock(BlockID id, const Block& block)
{
    std::span<const ValueID> values = code().valuesOf(id);
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

    if (ValueID owned = valueOwnedByTerminator(block.terminator)) {
        RELEASE_ASSERT(!values.empty() && values.back() == owned);
        values = values.first(values.size() - 1);
    }
    std::span<const ReleaseBehindLastRead> releases;
    if (m_mayReleaseBehindLastReads) [[unlikely]]
        releases = code().releasesBehindLastReadsIn(id);
    auto releaseBehind = [&](ValueID lastRead) {
        for (; !releases.empty() && (!releases.front().behind || (lastRead && releases.front().behind <= lastRead)); releases = releases.subspan(1))
            emitter<HintEmitter>().release(releases.front().released);
    };
    ValueID test = block.terminator.kind == TerminatorKind::Branch ? testReadByBranchAlone(id, block.terminator) : ValueID();
    bool testWaitsForBranch = false;
    m_readLeftToBranch = releases.empty() ? readLeftToBranchOnItsTruth(id, values, test) : ValueID();
    for (size_t index = 0; index < values.size(); ++index) {
        ValueID value = values[index];
        if (value == m_readLeftToBranch)
            continue;
        const Operation& operation = code().at(value);
        bool isCatch = false;
        if (!releases.empty()) [[unlikely]] {
            isCatch = operation.opcode == Opcode::CatchValue || operation.opcode == Opcode::CatchException;
            if (!isCatch)
                releaseBehind(index ? values[index - 1] : ValueID());
        }
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
        if (!isNeverType(operation.type)) [[likely]] {
            if (!releases.empty() && !isCatch) [[unlikely]]
                releaseBehind(value);
            continue;
        }
        if (currentBlockIsOpen())
            m_out.unreachable();
        return;
    }
    if (!releases.empty()) [[unlikely]]
        releaseBehind(values.empty() ? ValueID() : values.back());
    generateTerminator(id, block, testWaitsForBranch);
}

static constexpr unsigned turnsLaidOut = 8;
static constexpr int32_t turnsBetweenPolls = 1 << 20;
static constexpr unsigned mostBlocksInTurnLaidOut = 4;
static constexpr unsigned placesLookedAtBehindHead = 8;
static constexpr int64_t largestStepOfCounter = 16;
static constexpr unsigned mostOperationsInTurnLaidOut = 16;
static constexpr OpcodeSet operationsOfTurnsLaidOut {
    Opcode::Narrowed, Opcode::AssumeType, Opcode::Box, Opcode::Unbox, Opcode::IntegerToNumber, Opcode::NumberToInteger, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::ToInt32,
    Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Increment, Opcode::Decrement, Opcode::Negate,
    Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift, Opcode::URShift,
    Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::StrictEq, Opcode::Eq, Opcode::TypeTest,
    Opcode::GetStringCodeUnit, Opcode::GetTypedArrayElement, Opcode::GetArrayElement,
};

std::optional<CodeGenerator::CountedLoop> CodeGenerator::findCountedLoopAt(size_t positionOfHead) const
{
    BlockID head = m_order[positionOfHead];
    const Block& blockOfHead = code().at(head);
    if (blockOfHead.frequency != BlockFrequency::Normal || code().handlerOf(head) || blockOfHead.subject || blockOfHead.isEntry || blockOfHead.isHandlerBlock() || !code().releasesBehindLastReadsIn(head).empty())
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
        if (!block.parameters().empty() || code().valuesOf(asking).size() != 1 || !code().releasesBehindLastReadsIn(asking).empty())
            return std::nullopt;
    }
    const Block& blockThatAsks = code().at(asking);
    ValueID test = testReadByBranchAlone(asking, blockThatAsks.terminator);
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
    if (!intoTurn.arguments().empty() || positionOfAsking + 1 == m_order.size() || m_order[positionOfAsking + 1] != intoTurn.to || m_singlePredecessors[intoTurn.to.index()] != asking)
        return std::nullopt;

    unsigned numberOfPlaces = std::min<size_t>(placesLookedAtBehindHead, m_order.size() - positionOfAsking - 1);
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
    auto mayBelongToTurn = [&](BlockID id) {
        const Block& block = code().at(id);
        if (!block.isInLoop || block.isLoopHead || code().handlerOf(id) || block.subject || block.isEntry || block.isHandlerBlock() || block.frequency != blockOfHead.frequency)
            return false;
        if (!code().releasesBehindLastReadsIn(id).empty() || std::ranges::any_of(block.parameters(), [&](ValueID parameter) { return isCarriedBoxed(parameter); }))
            return false;
        for (ValueID value : code().valuesOf(id)) {
            const Operation& operation = code().at(value);
            EffectTraits traits = operation.traits();
            if (traits.mayRunOtherCode() || traits.mayBeTerminated() || isNeverType(operation.type))
                return false;
            if (operation.opcode == Opcode::PutTypedArrayElement) {
                const Type& stored = typeOf(operation.valueOperand());
                if (!leadsBack(block) || stored.hasObjects() || stored.primitives & cellPrimitives)
                    return false;
                continue;
            }
            if (!operationsOfTurnsLaidOut.contains(operation.opcode) || traits.mayWriteWhatIsSeen())
                return false;
        }
        const Terminator& terminator = block.terminator;
        if (terminator.kind == TerminatorKind::Jump)
            return true;
        return terminator.kind == TerminatorKind::Branch && code().at(terminator.taken()).to != head && code().at(terminator.notTaken()).to != head;
    };
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
        return std::nullopt;
    isInTurn[*placeOfLast] = true;
    for (unsigned place = *placeOfLast; place--;) {
        if (!isComeTo[place] || leadsBack(code().at(blockAt(place))))
            continue;
        forEachPlaceLedTo(place, [&](unsigned target) {
            isInTurn[place] |= isInTurn[target];
        });
    }
    if (!isInTurn[0])
        return std::nullopt;
    unsigned numberOfOperations = 0;
    for (unsigned place = 0; place <= *placeOfLast; ++place) {
        if (!isInTurn[place])
            continue;
        if (loop.turn.size() == mostBlocksInTurnLaidOut)
            return std::nullopt;
        loop.turn.append(blockAt(place));
        loop.positionsOfTurn.append(positionOfAsking + 1 + place);
        for (ValueID value : code().valuesOf(blockAt(place))) {
            const Operation& operation = code().at(value);
            if (++numberOfOperations > mostOperationsInTurnLaidOut)
                return std::nullopt;
            if (operation.opcode == Opcode::GetStringCodeUnit && opcodeOf(operation.stringOperand()) == Opcode::FlattenString && !loop.flatStrings.contains(operation.stringOperand()))
                loop.flatStrings.append(operation.stringOperand());
        }
    }
    loop.flatStrings.removeAllMatching([&](ValueID string) {
        BlockID home = code().blockOf(string);
        return home == head || loop.turn.contains(home);
    });

    const Edge& back = code().at(code().at(loop.turn.last()).terminator.next());
    auto integerHandedOnBy = [&](ValueID value) {
        static constexpr OpcodeSet handingIntegerOn { Opcode::Narrowed, Opcode::AssumeType, Opcode::WidenInteger, Opcode::NarrowInteger };
        while (handingIntegerOn.contains(opcodeOf(value)))
            value = code().at(value).valueOperand();
        return value;
    };
    const Operation& step = code().at(integerHandedOnBy(back.arguments()[counterAmongParameters - parameters.begin()]));
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
    findSumsOfIntegers(loop, back);
    return loop;
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
        if (addition.opcode != Opcode::Add || addition.form() != OperatorForm::Numbers || addition.representation != Representation::Double || code().useCountOf(sum) != 1)
            continue;
        if (addition.leftOperand() != parameter && addition.rightOperand() != parameter)
            continue;
        ValueID termAsNumber = addition.leftOperand() == parameter ? addition.rightOperand() : addition.leftOperand();
        const Operation& conversion = code().at(termAsNumber);
        if (conversion.opcode != Opcode::IntegerToNumber || conversion.block() != addition.block() || code().useCountOf(termAsNumber) != 1 || representationOf(conversion.valueOperand()) != Representation::Int32)
            continue;
        Range terms = typeOf(conversion.valueOperand()).integers;
        int64_t largestTerm = std::max(-terms.minimum, terms.maximum);
        if (largestTerm > mostAddedInAll / mostTurns)
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
        if (numberOfReads == 1)
            loop.sumsOfIntegers.append({ index, sum, termAsNumber, conversion.valueOperand(), nullptr });
    }
}

bool CodeGenerator::addToSumOfIntegers(ValueID id)
{
    for (SumOfIntegers& sum : m_sumsOfIntegers) {
        if (id == sum.termAsNumber)
            return true;
        if (id != sum.sum)
            continue;
        LValue term = get(sum.term).value;
        sum.added = m_out.add(sum.added, typeOf(sum.term).integers.minimum >= 0 ? m_out.zeroExtPtr(term) : m_out.signExt32To64(term));
        return true;
    }
    return false;
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

void CodeGenerator::layOutTurns(const CountedLoop& loop)
{
    BlockID head = m_order[loop.positionOfHead];
    const Block& blockOfHead = code().at(head);
    std::span<const ValueID> parameters = blockOfHead.parameters();
    std::span<const BlockID> turn = loop.turn.span();

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

    LBasicBlock entry = blockFor(head);
    LBasicBlock turns = m_out.newBlock();
    LBasicBlock writtenHead = m_out.newBlock();
    using Values = Vector<LValue, 8>;
    Values phisOfEntry;
    Values phisOfTurns;
    Values phisOfWrittenHead;
    for (ValueID parameter : parameters) {
        LValue phi = m_values[parameter.index()];
        phisOfEntry.append(phi);
        phisOfTurns.append(nullptr);
        phisOfWrittenHead.append(phi ? m_out.phiIn(writtenHead, phi->type()) : nullptr);
    }
    m_sumsOfIntegers = loop.sumsOfIntegers;
    for (SumOfIntegers& sum : m_sumsOfIntegers) {
        sum.added = m_out.phiIn(turns, Machine::Int64);
        phisOfTurns[sum.indexOfParameter] = sum.added;
    }
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (phisOfEntry[index] && !phisOfTurns[index])
            phisOfTurns[index] = m_out.phiIn(turns, phisOfEntry[index]->type());
    }
    size_t indexOfCounter = std::ranges::find(parameters, loop.counter) - parameters.begin();
    auto blockHandingOver = [&](const Values& phis, LBasicBlock target, auto&& valueAt) {
        LBasicBlock here = m_out.m_block;
        LBasicBlock blockOnEdge = m_out.newBlock();
        m_out.appendTo(blockOnEdge);
        Values values;
        for (size_t index = 0; index < phis.size(); ++index)
            values.append(phis[index] ? valueAt(index) : nullptr);
        for (size_t index = 0; index < phis.size(); ++index) {
            if (!phis[index])
                continue;
            if (values[index] == phisOfEntry[index] || (index == indexOfCounter && values[index] == phisOfTurns[index]))
                m_out.upsilonOfCompletePhi(values[index], phis[index]);
            else
                m_out.upsilon(values[index], phis[index]);
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
        switch (loop.relation) {
        case Opcode::Less:
            return m_out.lessThan(counter, limit);
        case Opcode::LessEq:
            return m_out.lessThanOrEqual(counter, limit);
        case Opcode::Greater:
            return m_out.greaterThan(counter, limit);
        default:
            return m_out.greaterThanOrEqual(counter, limit);
        }
    };
    LValue endOfStretch = pollsBetweenStretches ? m_out.phiIn(turns, phisOfEntry[indexOfCounter]->type()) : nullptr;
    auto endOfStretchFrom = [&](LValue counter, LValue lastStart) {
        LValue behindStretch = m_out.add(counter, m_out.constantOfType(counter->type(), turnsBetweenPolls * loop.step));
        return m_out.select(countsUp ? m_out.lessThan(behindStretch, lastStart) : m_out.greaterThan(behindStretch, lastStart), behindStretch, lastStart);
    };

    m_out.appendTo(entry);
    m_out.setOrigin(Machine::Origin());
    LValue lastStart = nullptr;
    {
        TerminatorContext context(head, blockOfHead.terminator, nullptr);
        SetForScope<const OperationContext*> whileTesting(m_operation, &context);
        LValue bound = get(loop.bound).value;
        LValue counter = phisOfEntry[indexOfCounter];
        RELEASE_ASSERT(bound && counter && bound->type() == counter->type());
        int64_t reach = (turnsLaidOut - 1) * loop.step;
        lastStart = m_out.sub(bound, m_out.constantOfType(bound->type(), reach));
        LBasicBlock written = blockHandingOver(phisOfWrittenHead, writtenHead, [&](size_t index) {
            return phisOfEntry[index];
        });
        LBasicBlock laidOut = blockHandingOver(phisOfTurns, turns, [&](size_t index) {
            if (index == indexOfCounter && endOfStretch)
                m_out.upsilon(endOfStretchFrom(counter, lastStart), endOfStretch);
            return sumAt(index) ? m_out.constInt64(0) : phisOfEntry[index];
        });
        if (bound->type() == Machine::Int32) {
            Range bounds = typeOf(loop.bound).integers;
            int64_t nearest = (countsUp ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int32_t>::max()) + reach;
            if (countsUp ? bounds.minimum < nearest : bounds.maximum > nearest) {
                LValue limit = m_out.constInt32(static_cast<int32_t>(nearest));
                LBasicBlock doesNotWrap = m_out.newBlock();
                m_out.branch(countsUp ? m_out.lessThan(bound, limit) : m_out.greaterThan(bound, limit), unlikelyEdge(written), normalEdge(doesNotWrap));
                m_out.appendTo(doesNotWrap);
            }
        }
        for (ValueID string : loop.flatStrings) {
            std::optional<Condition> isNarrow = emitter<StringEmitter>().findNarrow(string);
            if (!isNarrow || isNarrow->value()->hasInt())
                continue;
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(isNarrow->value(), normalEdge(holds), unlikelyEdge(written));
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
        m_out.branch(holdsOf(counter, lastStart), normalEdge(laidOut), normalEdge(written));
    }

    m_out.appendTo(turns);
    m_headOfTurnsLaidOut = head;
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
        if (counterIsExtended && numberOfTurns < turnsLaidOut)
            return m_out.castToInt32(m_out.add(extensionForAllTurns ? extensionForAllTurns : m_out.extendWordThatIsNotNegative(firstCounter), m_out.constInt64(numberOfTurns * loop.step)));
        return m_out.add(firstCounter, m_out.constantOfType(firstCounter->type(), numberOfTurns * loop.step));
    };
    m_turnLaidOut = turn;
    for (unsigned turnLaidOut = 0; turnLaidOut < turnsLaidOut; ++turnLaidOut) {
        forgetTurn();
        Values valuesAheadOfTurn;
        for (size_t index = 0; index < parameters.size(); ++index) {
            const SumOfIntegers* sum = sumAt(index);
            valuesAheadOfTurn.append(sum ? sum->added : m_values[parameters[index].index()]);
        }
        LBasicBlock blockLeavingTurn = nullptr;
        m_blockLeavingTurnLaidOut = [&] {
            if (!blockLeavingTurn) {
                blockLeavingTurn = blockHandingOver(phisOfWrittenHead, writtenHead, [&](size_t index) -> LValue {
                    if (sumAt(index))
                        return m_out.doubleAdd(phisOfEntry[index], m_out.intToDouble(valuesAheadOfTurn[index]));
                    if (index == indexOfCounter && turnLaidOut)
                        return m_out.add(firstCounter, m_out.constantOfType(firstCounter->type(), turnLaidOut * loop.step));
                    return forWrittenLoop(index, valuesAheadOfTurn[index]);
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
    for (size_t index : takingAnyNaNInTurnsAlone)
        m_valuesHoldingAnyNaN.quickClear(parameters[index].index());
    m_headOfTurnsLaidOut = BlockID();
    m_turnLaidOut = { };
    m_blockLeavingTurnLaidOut = nullptr;
    m_nextBlockGoesOnHere = false;
    emitter<StringEmitter>().forgetStringsFoundNarrow();
    m_out.setOrigin(Machine::Origin());
    Values valuesBehindTurns;
    for (ValueID parameter : parameters)
        valuesBehindTurns.append(m_values[parameter.index()]);
    LValue counterBehindTurns = valuesBehindTurns[indexOfCounter];
    RELEASE_ASSERT(counterBehindTurns->type() != Machine::Int32 || Machine::clearsUpperHalfOfRegister(counterBehindTurns->opcode()));
    auto goingRound = [&](LValue nextEndOfStretch) {
        return blockHandingOver(phisOfTurns, turns, [&](size_t index) {
            if (index == indexOfCounter && endOfStretch)
                m_out.upsilon(nextEndOfStretch ? nextEndOfStretch : endOfStretchFrom(m_out.opaque(counterBehindTurns), lastStart), endOfStretch);
            const SumOfIntegers* sum = sumAt(index);
            return sum ? sum->added : valuesBehindTurns[index];
        });
    };
    if (pollsBetweenStretches) {
        LBasicBlock stretchHasEnded = m_out.newBlock();
        m_out.branch(holdsOf(counterBehindTurns, endOfStretch), normalEdge(goingRound(endOfStretch)), unlikelyEdge(stretchHasEnded));
        m_out.appendTo(stretchHasEnded);
        generateValue(head, loop.poll, code().at(loop.poll), false, { });
        m_out.setOrigin(Machine::Origin());
    }
    LBasicBlock again = goingRound(nullptr);
    LBasicBlock tooFewAreLeft = blockHandingOver(phisOfWrittenHead, writtenHead, [&](size_t index) {
        const SumOfIntegers* sum = sumAt(index);
        return sum ? m_out.doubleAdd(phisOfEntry[index], m_out.intToDouble(sum->added)) : forWrittenLoop(index, valuesBehindTurns[index]);
    });
    m_out.branch(holdsOf(counterBehindTurns, lastStart), normalEdge(again), normalEdge(tooFewAreLeft));
    m_sumsOfIntegers.shrink(0);

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
    return target;
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

ALWAYS_INLINE void CodeGenerator::passArgument(ValueID parameter, Operand argument)
{
    Representation representationOfParameter = isCarriedBoxed(parameter) ? Representation::JSValue : representationOf(parameter);
    if (!isSameWord(argument.representation, representationOfParameter)) [[unlikely]] {
        failToPassArgument(parameter, argument.representation, representationOfParameter);
        return;
    }
    LValue phi = m_values[parameter.index()];
    RELEASE_ASSERT(phi && phi->opcode() == Machine::Phi && argument.value && argument.value->type() == phi->type());
    m_out.upsilon(argument.value, phi);
}

void CodeGenerator::passArguments(const Edge& edge, std::span<const ValueID> parameters, std::span<const Operand> declaredResults)
{
    std::span<const ValueID> arguments = edge.arguments();
    RELEASE_ASSERT(parameters.size() == arguments.size() + declaredResults.size());
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

void CodeGenerator::verifyThatDefinitionDominates(ValueID id, BlockID home, BlockID user)
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

Operand CodeGenerator::makeConstant(ValueID id, const Operation& constant)
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

void CodeGenerator::defineParameterAsPassed(ValueID parameter, Operand passed)
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

CodeGenerator::InitializationScope::InitializationScope(CodeGenerator& generator)
    : m_generator(generator)
{
    if (!generator.m_initializationDepth++ && generator.m_proc.traits().checksAllocation)
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
