/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTCodeRecords.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTEffectsOfOperations.h"
#include "AOTEntryEmitter.h"
#include "AOTHintEmitter.h"
#include "AOTIRProgram.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTRepresentationRules.h"
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

// Whoever asks writes each element before it is read.
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
    orderBlocks();
    createBlocks();
    followNumbersOutOfBytes();
    createPhis();

    definePinnedValues(m_out, m_pinned);
    emitter<EntryEmitter>().generateEntries();
    for (size_t position = 0; position < m_order.size(); ++position) {
        if (m_hasFailed)
            return GenerationResult::Failed;
        BlockID block = m_order[position];
        if (code().at(block).isLoopHead) {
            if (std::optional<CountedLoop> loop = findCountedLoopAt(position))
                layOutTurns(*loop);
            if (m_hasFailed)
                return GenerationResult::Failed;
        }
        generateBlock(block);
        m_scopeAtEndOfBlock[block.index()] = m_out.forkScope();
    }
    return m_hasFailed ? GenerationResult::Failed : GenerationResult::Generated;
}

const Function& CodeGenerator::function() const
{
    return program().at(unit().function);
}

MentionList& CodeGenerator::mentions()
{
    return m_records.mentions;
}

FormAtPlace CodeGenerator::sayFormNamedFor(const OperationContext& context, StateOfKnowledge state) const
{
    bool standsInLoop = context.block() && isInLoop(context.block());
    // The code of the end of a block is counted by the kind of the end, and no site stands for it.
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
    // A deferred closure has no value, and a name has the form of what it names. Behind a row of tests of one value each name stands over the one in front of it: names x names for whoever goes down from each.
    if (named.representation != Representation::Void)
        return false;
    const Operation* definition = &named;
    while (definition->opcode == Opcode::Narrowed)
        definition = &code().at(definition->valueOperand());
    return definition->opcode == Opcode::NewClosure && definition->closureForm() == ClosureForm::Deferred;
}

void CodeGenerator::computeEffectsOf(const OperationContext& context) const
{
    TypingContext typing { program(), m_inputs.types, PositionTypes::knowingNothing() };
    Vector<TypeID, 8> typesOfOperands;
    for (ValueID operand : context.instruction().operands())
        typesOfOperands.append(operand ? typeIDOf(operand) : TypeID());
    OperandTypes operandTypes { typesOfOperands.span() };
    if (!context.valueID()) {
        context.m_effects = effectsOfTerminator(typing, static_cast<const TerminatorContext&>(context).terminator(), operandTypes);
        return;
    }

    const Operation& operation = static_cast<const ValueContext&>(context).value();
    Effects& effects = context.m_effects.emplace(Typed::effectsOf(typing, operation, operandTypes, operation.type));
    EffectTraits traits = operation.traits();
    if (!traits.mayThrow())
        effects.mayThrow = false;
    else if (!traits.mayThrowByRule())
        effects.mayThrow &= ThrowEffect::onlyForLackOfMemory();
    effects.mayCollect &= traits.mayCollect();
    effects.mayCallUnknownCode &= traits.mayCallUnknownCode();
    effects.maySuspend &= traits.maySuspend();
    effects.mayBeTerminated &= traits.mayBeTerminated();
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
        uint32_t keptCalleePlusOne = frame.calleeIsKeptInFrame ? sites.addKeptCallee(inlineFrames, frame.callPosition, frame.parent) : 0;
        sites.inlineCallSites.append(sites.addSite({
            .origin = { .position = frame.callPosition, .inlineFrame = frame.parent, .isPartOfConstruction = frame.callIsPartOfConstruction || frame.isConstruction },
            .kind = frame.isConstruction ? CallSiteKind::Construct : CallSiteKind::Plain,
            .callsOwnFunctionObject = frame.callsOwnFunctionObject,
            .keptCalleePlusOne = keptCalleePlusOne,
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
    // `from` is null for an entry.
    struct Step {
        BlockID block;
        BlockID from;
        StepKind kind;
    };

    Machine::Arena& arena = m_proc.arena();
    uint32_t numberOfBlocks = code().numberOfBlocks();
    m_singlePredecessors = allocateSpanOfAllBitsSet<BlockID>(arena, numberOfBlocks);
    m_scopeAtEndOfBlock = allocateSpan<ScopeFork>(arena, numberOfBlocks);
    std::span<BlockID> order = allocateSpan<BlockID>(arena, numberOfBlocks);
    uint32_t numberOrdered = 0;
    Vector<Step, 64> steps;

    auto endBlock = [&](BlockID block) {
        order[numberOfBlocks - ++numberOrdered] = block;
    };

    // What is walked first comes last in the order: entry 0 leads, and the likeliest successor of a block stands right behind it. Among equals edge 0 leads and the handler comes last.
    // The clock of the back end's numbering follows this order, so what a rarer successor writes ends no load for a likelier one.
    // True: `step` has become the way into the successor that is walked first.
    static constexpr BlockFrequency fromLikeliest[] = { BlockFrequency::Normal, BlockFrequency::Unlikely, BlockFrequency::Rare };
    auto enter = [&](Step& step) -> bool {
        BlockID id = step.block;
        RELEASE_ASSERT(id);
        BlockID& predecessor = m_singlePredecessors[id.index()];
        if (predecessor) {
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
    for (BlockID block : m_order) {
        BlockID& predecessor = m_singlePredecessors[block.index()];
        if (predecessor == enteredOtherwise)
            predecessor = BlockID();
    }
}

// A block with a single predecessor has that for its dominator, so only the ways into the others are gone. Those into one block are taken from the last source to the first:
// a thousand breaks of one loop come from blocks that stand one above another, and so the way up from each ends at the next, not at the head of the loop.
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

    // The ways into each block, the last source first.
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
    auto lastOnEveryWayToBoth = [&](uint32_t found, uint32_t other) {
        while (found != other) {
            if (found > other)
                found = m_dominatorsByPlace[found];
            else
                other = m_dominatorsByPlace[other];
        }
        return found;
    };
    // True: the dominator of the target has changed.
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
        for (uint32_t place = 1; place < numberOfPlaces; ++place) {
            if (BlockID predecessor = m_singlePredecessors[m_order[place - 1].index()])
                m_dominatorsByPlace[place] = m_places[predecessor.index()];
            else
                takeWaysInto(place, false);
        }
        for (uint32_t target : targetsOfWaysThatComeRound)
            changed |= takeWaysInto(target, true);
    }
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

bool CodeGenerator::dominates(BlockID home, BlockID user)
{
    if (home == m_onlyEntryBlock)
        return true;
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

void CodeGenerator::createBlocks()
{
    m_blocks = allocateZeroedSpan<LBasicBlock>(m_proc.arena(), code().numberOfBlocks());
    for (size_t position = 0; position < m_order.size(); ++position) {
        if (!goesOnBehindJump(position))
            m_blocks[m_order[position].index()] = m_out.newBlock();
    }
}

bool CodeGenerator::goesOnBehindJump(size_t positionInOrder) const
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

bool CodeGenerator::isReadByBoxAlone(BlockID home, ValueID parameter, unsigned stepsLeft) const
{
    if (code().useCountOf(parameter) != 1)
        return false;
    // An entrypoint hands over what its convention delivers.
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
    for (unsigned stepsLeft = 8; stepsLeft--;) {
        BlockID home = code().blockOf(made);
        for (ValueID id : code().valuesOf(home)) {
            const Operation& operation = code().at(id);
            if (operation.opcode == Opcode::PropSet && operation.objectOperand() == made)
                return true;
        }
        const Terminator& terminator = code().at(home).terminator;
        if (terminator.kind != TerminatorKind::Jump)
            return false;
        const Edge& edge = code().at(terminator.next());
        std::span<const ValueID> arguments = edge.arguments();
        std::span<const ValueID> parameters = code().at(edge.to).parameters();
        size_t position = std::ranges::find(arguments, made) - arguments.begin();
        if (position >= std::min(arguments.size(), parameters.size()))
            return false;
        made = parameters[position];
    }
    return false;
}

void CodeGenerator::createPhis()
{
    constexpr unsigned mostBlocksThatHandParameterOn = 4;
    for (BlockID block : m_order) {
        for (ValueID parameter : code().at(block).parameters()) {
            Representation representation = representationOf(parameter);
            RELEASE_ASSERT(representation != Representation::Unchosen);
            // The sweep needs the arguments of needed parameters alone, so nothing may ask for the others.
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

// `for (..) sum += view[i]`: the test for NaN stands behind the loop, where the sum is boxed, and not in every turn.
// What this finds decides only where that test stands. That nobody is handed other bits than those of the one NaN rests on get(), which tests for whoever asks.
void CodeGenerator::followNumbersOutOfBytes()
{
    if (!code().readsNumbersOutOfBytes())
        return;

    m_followsNumbersOutOfBytes = true;
    uint32_t numberOfValues = code().numberOfValues();
    m_valuesHoldingAnyNaN.ensureSize(numberOfValues);
    m_valuesReadForBitsOfNaN.ensureSize(numberOfValues);
    m_blocksBegun.ensureSize(code().numberOfBlocks());

    // By the index of the value: a parameter of a loop head that the value comes to through arithmetic and edges alone.
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

    // Users stand behind what they use: where the way comes by an operation, or by an edge to a join, everyone who reads the value has been seen. Who reads a parameter of a loop head is known at the end.
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
                // What a throw brings is moved in a landing pad.
                if (edgeID == handler || representationOf(parameter) != Representation::Double) {
                    m_valuesReadForBitsOfNaN.quickSet(argument.index());
                    continue;
                }
                // A record stands behind what it is made of, so a parameter in front of its argument is one that the edge leads back to. The innermost such head goes before any other that the value is handed to.
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

    // The sum of an inner loop goes on into that of the loop around it.
    for (ValueID head : headsFedByBytes) {
        while (head && !isReadForBitsOfNaN(head) && !m_valuesHoldingAnyNaN.quickSet(head.index()))
            head = headsFed[head.index()];
    }
}

void CodeGenerator::generateBlock(BlockID id)
{
    const Block& block = code().at(id);
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
    std::span<const ReleaseBehindLastRead> releases = code().releasesBehindLastReadsIn(id);
    // By position, not by equality: those at the head of a handler block wait for what the block catches, and one behind a caught value stands behind them in the list.
    auto releaseBehind = [&](ValueID lastRead) {
        for (; !releases.empty() && (!releases.front().behind || (lastRead && releases.front().behind <= lastRead)); releases = releases.subspan(1))
            emitter<HintEmitter>().release(releases.front().released);
    };
    ValueID test = block.terminator.kind == TerminatorKind::Branch ? testReadByBranchAlone(id, block.terminator) : ValueID();
    bool testWaitsForBranch = false;
    for (size_t index = 0; index < values.size(); ++index) {
        ValueID value = values[index];
        const Operation& operation = code().at(value);
        bool isCatch = false;
        if (!releases.empty()) [[unlikely]] {
            // What a handler catches stands at its head, and by it the handler is known for a `catch` or a `finally`.
            isCatch = operation.opcode == Opcode::CatchValue || operation.opcode == Opcode::CatchException;
            if (!isCatch)
                releaseBehind(index ? values[index - 1] : ValueID());
        }
        if (value == test) {
            testWaitsForBranch = true;
            continue;
        }
        // The test moves behind what has no effects, and behind nothing else.
        if (testWaitsForBranch && (!operation.traits().isPure() || isNeverType(operation.type))) {
            testWaitsForBranch = false;
            generateValue(id, test, code().at(test), false, values.subspan(index));
            if (m_hasFailed)
                return;
        }
        // The list has no place for a test that is still to come.
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
    releaseBehind(values.empty() ? ValueID() : values.back());
    generateTerminator(id, block, testWaitsForBranch);
}

static constexpr unsigned turnsLaidOut = 8;
// The turns laid out poll once in so many turns.
static constexpr int32_t turnsBetweenPolls = 1 << 20;
static constexpr unsigned mostBlocksInTurnLaidOut = 4;
static constexpr unsigned placesLookedAtBehindHead = 8;
static constexpr int64_t largestStepOfCounter = 16;
static constexpr unsigned mostOperationsInTurnLaidOut = 16;
// None keeps anything by the number of its value but what it defines.
static constexpr OpcodeSet operationsOfTurnsLaidOut {
    Opcode::Narrowed, Opcode::Box, Opcode::Unbox, Opcode::IntegerToNumber, Opcode::NumberToInteger, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::ToInt32,
    Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Increment, Opcode::Decrement, Opcode::Negate,
    Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift, Opcode::URShift,
    Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::StrictEq, Opcode::Eq, Opcode::TypeTest,
    Opcode::GetStringCodeUnit, Opcode::GetTypedArrayElement, Opcode::GetArrayElement,
};
// One store into storage that is there: neither the storage nor its length is another behind it.
static constexpr OpcodeSet storesOfTurnsLaidOut { Opcode::PutContiguousIndex, Opcode::PutDoubleIndex, Opcode::PutTypedArrayElement };

std::optional<CodeGenerator::CountedLoop> CodeGenerator::findCountedLoopAt(size_t positionOfHead) const
{
    BlockID head = m_order[positionOfHead];
    const Block& blockOfHead = code().at(head);
    if (blockOfHead.frequency != BlockFrequency::Normal || blockOfHead.handler || blockOfHead.subject || blockOfHead.isEntry || blockOfHead.isHandlerBlock() || !code().releasesBehindLastReadsIn(head).empty())
        return std::nullopt;

    CountedLoop loop { .positionOfHead = positionOfHead };
    ValueID test = testReadByBranchAlone(head, blockOfHead.terminator);
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
    const Edge& intoTurn = code().at(blockOfHead.terminator.taken());
    if (!intoTurn.arguments().empty() || positionOfHead + 1 == m_order.size() || m_order[positionOfHead + 1] != intoTurn.to || m_singlePredecessors[intoTurn.to.index()] != head)
        return std::nullopt;

    // The order is one in which a block stands behind whatever leads to it. Looked at: a few places behind the head.
    unsigned numberOfPlaces = std::min<size_t>(placesLookedAtBehindHead, m_order.size() - positionOfHead - 1);
    auto blockAt = [&](unsigned place) {
        return m_order[positionOfHead + 1 + place];
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
        if (!block.isInLoop || block.isLoopHead || block.handler || block.subject || block.isEntry || block.isHandlerBlock() || block.frequency != blockOfHead.frequency)
            return false;
        if (!code().releasesBehindLastReadsIn(id).empty() || std::ranges::any_of(block.parameters(), [&](ValueID parameter) { return isCarriedBoxed(parameter); }))
            return false;
        for (ValueID value : code().valuesOf(id)) {
            const Operation& operation = code().at(value);
            EffectTraits traits = operation.traits();
            if (traits.mayRunOtherCode() || traits.mayBeTerminated() || isNeverType(operation.type))
                return false;
            // A turn that leaves is taken again by the written loop: nobody may see that it was begun twice. So behind a store no edge leaves the turn: it stands in the block that leads back to the head.
            // What is stored is no cell: the store is all that there is, and no barrier remembers anything from one turn to the next or on into the written loop.
            if (storesOfTurnsLaidOut.contains(operation.opcode)) {
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
    // Come to from the head's test through such blocks alone. Of those that lead back the last in the order ends the turn: whatever leads to another leaves it.
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
    // And leading on to the end of the turn.
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
        loop.positionsOfTurn.append(positionOfHead + 1 + place);
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
    // Each hands on the integer that it is handed.
    auto integerHandedOnBy = [&](ValueID value) {
        static constexpr OpcodeSet handingIntegerOn { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
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
    // By the types: whether there are ever as many turns as are laid out.
    Range counters = typeOf(loop.counter).integers;
    Range bounds = typeOf(loop.bound).integers;
    loop.mostTurns = (countsUp ? bounds.maximum - counters.minimum : counters.maximum - bounds.minimum) / std::abs(loop.step) + 1;
    if (loop.mostTurns < static_cast<int64_t>(turnsLaidOut))
        return std::nullopt;
    findSumsOfIntegers(loop, back);
    for (BlockID block : loop.turn) {
        ValueID test = testReadByBranchAlone(block, code().at(block).terminator);
        if (!test)
            continue;
        const Operation& operation = code().at(test);
        if (operation.opcode == Opcode::TypeTest && operation.testedType() == TypeTable::int32 && integerHandedOnBy(operation.valueOperand()) == loop.counter)
            loop.testsOfCounterForInt32.append(test);
    }
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
        // Nothing else in a turn reads the sum so far.
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
    // As passArguments() hands them to the head.
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

// In front of the loop as it is written stands one that takes several turns at a time, as long as that many are left: one test, one poll and one jump for all of them,
// and what a turn reads again of what no turn writes is read once. The loop as it is written takes the turns that are left over.
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

    // What createBlocks() and createPhis() have made for the turn is the written loop's.
    Vector<LBasicBlock, mostBlocksInTurnLaidOut> blocksOfWrittenTurn;
    Vector<LValue, 8> phisOfWrittenTurn;
    for (BlockID block : turn)
        blocksOfWrittenTurn.append(m_blocks[block.index()]);
    forEachParameterInTurn([&](BlockID, ValueID parameter) {
        phisOfWrittenTurn.append(m_values[parameter.index()]);
    });

    // What has been made for the head is entered from in front of the loop, and from there alone from here on: it asks what is asked once.
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
    // Each Phi is fed on every way in: none is made that is not.
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (phisOfEntry[index] && !phisOfTurns[index])
            phisOfTurns[index] = m_out.phiIn(turns, phisOfEntry[index]->type());
    }
    size_t indexOfCounter = std::ranges::find(parameters, loop.counter) - parameters.begin();
    // `valueAt` is asked in the block on the edge. The parameters of the entry are fed by nothing more.
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
            // What comes round to the counter of the turns laid out is made by an instruction of 32 bits, so what the entry has handed it settles what it says of its upper half.
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
    // Turns that are so few by the types poll in the written loop, which all of them come to.
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
    // The counter at which the turns laid out stop to poll, or for good. Where the sum wraps, within a stretch of the end of int32, it lies on the other side of the counter: the stretch ends behind eight turns, which is early and no more.
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
        // The tests of the turns hold one after the other as far as the last of them does: the counter is held against the bound less what the turns behind the first step it by.
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
        // An Int64 holds what a number holds exactly, so nothing wraps there. A bound of 32 bits so near the end of int32 that the difference wraps has too few turns left in any case.
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
        // What every turn would ask again and no turn changes. Whatever fails takes the loop as it is written, which asks.
        for (ValueID string : loop.flatStrings) {
            std::optional<Condition> isNarrow = emitter<StringEmitter>().findNarrow(string);
            if (!isNarrow || isNarrow->value()->hasInt())
                continue;
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(isNarrow->value(), normalEdge(holds), unlikelyEdge(written));
            m_out.appendTo(holds);
        }
        if (!loop.testsOfCounterForInt32.isEmpty()) {
            auto continueIf = [&](LValue holds) {
                LBasicBlock next = m_out.newBlock();
                m_out.branch(holds, normalEdge(next), unlikelyEdge(written));
                m_out.appendTo(next);
            };
            LValue least = m_out.constantOfType(bound->type(), std::numeric_limits<int32_t>::min());
            LValue greatest = m_out.constantOfType(bound->type(), std::numeric_limits<int32_t>::max());
            Range counters = typeOf(loop.counter).integers;
            Range bounds = typeOf(loop.bound).integers;
            if (countsUp ? bounds.maximum > std::numeric_limits<int32_t>::max() : bounds.minimum < std::numeric_limits<int32_t>::min())
                continueIf(countsUp ? m_out.lessThanOrEqual(bound, greatest) : m_out.greaterThanOrEqual(bound, least));
            if (countsUp ? counters.minimum < std::numeric_limits<int32_t>::min() : counters.maximum > std::numeric_limits<int32_t>::max())
                continueIf(countsUp ? m_out.greaterThanOrEqual(counter, least) : m_out.lessThanOrEqual(counter, greatest));
        }
        // A turn that leaves hands the written loop the sum as the entry brought it and what was added since. The first turn has added nothing yet, and -0 and nothing is -0, but -0 + 0 is 0.
        // Where no turn leaves, eight terms at the least have been added to whatever is handed on, and behind the first of them the language has 0 too.
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
            // Neither NaN nor an infinity is as small.
            LValue size = m_out.doubleAbs(phisOfEntry[sum.indexOfParameter]);
            LValue limit = m_out.constDouble(0x1p52);
            LBasicBlock isSmall = m_out.newBlock();
            m_out.branch(m_out.doubleLessThanOrEqual(size, limit), normalEdge(isSmall), unlikelyEdge(written));
            m_out.appendTo(isSmall);
            if (typeOf(parameters[sum.indexOfParameter]).isNumberWithoutFraction())
                continue;
            // From 2^52 on a number has no room for a fraction, so the sum with 2^52 is rounded to a whole number.
            LBasicBlock isWhole = m_out.newBlock();
            m_out.branch(m_out.doubleEqual(m_out.doubleSub(m_out.doubleAdd(size, limit), limit), size), normalEdge(isWhole), unlikelyEdge(written));
            m_out.appendTo(isWhole);
        }
        m_out.branch(holdsOf(counter, lastStart), normalEdge(laidOut), normalEdge(written));
    }

    m_out.appendTo(turns);
    m_headOfTurnsLaidOut = head;
    // The first turn reads the parameters of the head: from here on none of them is marked for good as taking any NaN that is not marked so yet.
    // In the turns laid out every number among them takes any NaN: get() tests for whoever asks for the one NaN, so a sum goes from turn to turn untested. The written loop is handed the one NaN.
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
    // The counter of each turn is that of the first and a constant. Whoever extends it for an address finds the constant outermost, where the address takes it in.
    LValue firstCounter = phisOfTurns[indexOfCounter];
    bool counterIsExtended = firstCounter->type() == Machine::Int32 && typeOf(loop.counter).integers.minimum >= 0 && countsUp;
    // What comes round is made by an instruction of 32 bits, so the register of the counter is the extended counter. Where an address takes an index and a distance, an extension with one reader goes into it
    // and is no instruction: each turn makes its own. Elsewhere the sum of base and index is an instruction anyway, and one extension serves all.
    LValue extensionForAllTurns = counterIsExtended && isARM64() ? m_out.extendWordThatIsNotNegative(firstCounter) : nullptr;
    auto counterBehind = [&](unsigned numberOfTurns) -> LValue {
        if (counterIsExtended && numberOfTurns < turnsLaidOut)
            return m_out.castToInt32(m_out.add(extensionForAllTurns ? extensionForAllTurns : m_out.extendWordThatIsNotNegative(firstCounter), m_out.constInt64(numberOfTurns * loop.step)));
        return m_out.add(firstCounter, m_out.constantOfType(firstCounter->type(), numberOfTurns * loop.step));
    };
    m_turnLaidOut = turn;
    m_testsThatHoldInTurnsLaidOut = loop.testsOfCounterForInt32.span();
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
    m_testsThatHoldInTurnsLaidOut = { };
    m_blockLeavingTurnLaidOut = nullptr;
    m_nextBlockGoesOnHere = false;
    emitter<StringEmitter>().forgetStringsFoundNarrow();
    m_out.setOrigin(Machine::Origin());
    Values valuesBehindTurns;
    for (ValueID parameter : parameters)
        valuesBehindTurns.append(m_values[parameter.index()]);
    LValue counterBehindTurns = valuesBehindTurns[indexOfCounter];
    // blockHandingOver() counts on it where it hands the Phi of the counter on before this has come round to it.
    RELEASE_ASSERT(counterBehindTurns->type() != Machine::Int32 || Machine::clearsUpperHalfOfRegister(counterBehindTurns->opcode()));
    // The next stretch is counted from a copy: folded into a sum with the counter of the first turn it would keep that alive beside the counter that comes round, and the two would not share a register.
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

// Such a test leaves its answer in no register: its code goes straight to the two targets of the branch.
// The sweep counts whoever else reads the answer: an operation that Known handed the test to, an edge. A Narrowed names its test and reads nothing of it.
ValueID CodeGenerator::testReadByBranchAlone(BlockID id, const Terminator& terminator) const
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

bool CodeGenerator::isAwaitedAtOnce(const Operation& call, std::span<const ValueID> valuesOfBlock, size_t index) const
{
    if ((call.opcode != Opcode::Call && call.opcode != Opcode::CallDirect) || index + 1 == valuesOfBlock.size())
        return false;
    const Operation& next = code().at(valuesOfBlock[index + 1]);
    return next.opcode == Opcode::AwaitBySavingFrame && next.awaitedOperand() == valuesOfBlock[index];
}

static constexpr OpcodeSet sumsThatMayStayBoxed { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };

ValueID CodeGenerator::sumBoxedBy(const Operation& box) const
{
    if (box.opcode != Opcode::Box || box.representation != Representation::JSValue || m_followsNumbersOutOfBytes)
        return { };
    ValueID sum = box.valueOperand();
    const Operation& operation = code().at(sum);
    BlockID block = box.block();
    if (!sumsThatMayStayBoxed.contains(operation.opcode) || operation.representation != Representation::Double || operation.form() != OperatorForm::Numbers)
        return { };
    // What is released behind its last read is read later here than the list says.
    if (operation.block() != block || code().useCountOf(sum) != 1 || !code().releasesBehindLastReadsIn(block).empty())
        return { };

    bool someTermLiesBoxed = false;
    auto isTerm = [&](ValueID id) {
        const Operation& term = code().at(id);
        if (code().isConstant(id))
            return term.opcode == Opcode::Integer && isRepresentableAs<int32_t>(term.integer());
        if (term.block() != block || code().useCountOf(id) != 1 || term.representation != Representation::Double)
            return false;
        if (term.opcode == Opcode::IntegerToNumber)
            return representationOf(term.valueOperand()) == Representation::Int32;
        if (term.opcode != Opcode::Unbox || representationOf(term.valueOperand()) != Representation::JSValue || !typeOf(term.valueOperand()).isNumber())
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

void CodeGenerator::generateValue(BlockID block, ValueID id, const Operation& operation, bool isAwaitedAtOnce, std::span<const ValueID> operationsBehind)
{
    if (!m_sumsOfIntegers.isEmpty() && addToSumOfIntegers(id)) [[unlikely]]
        return;
    if (isDeferredClosure(operation))
        return;
    if (operation.representation == Representation::Double && waitsForBoxOfSum(id, operation)) [[unlikely]]
        return;
    // The reader stands right behind it: the walk reads the scope beside the slot, so no suspension and no other code lies between.
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

void CodeGenerator::generate(const ValueContext& context)
{
    const OperationContext* user = std::exchange(m_operation, &context);
    Machine::Origin originOfUser = m_out.origin();
    m_out.setOrigin(machineOriginOf(context.id()));
    uint32_t siteOfUser = FormsSaid::noSite;
    if (m_notesForms) [[unlikely]]
        siteOfUser = m_records.formsSaid.enter(context.id(), context.opcode(), context.block(), m_proc.numberOfOperations());
    declareAccessesOf(context);

    dispatch(context);

    if (m_notesForms) [[unlikely]]
        m_records.formsSaid.leave(siteOfUser, m_proc.numberOfOperations());
    m_out.setOrigin(originOfUser);
    m_operation = user;
    if (m_checksAllocation) [[unlikely]] {
        if (user)
            declareAccessesSlow(*user);
        else
            m_out.clearDeclaredAccesses();
    }

    if (m_hasFailed || m_definitionTaken || context.isBranchCondition() || context.returnCall() || isNeverType(context->type))
        return;
    size_t index = context.id().index();
    RELEASE_ASSERT(!hasRegister(context.representation()) || m_values[index] || hasResults(index));
}

void CodeGenerator::generateTerminator(BlockID id, const Block& block, bool generatesCondition)
{
    RELEASE_ASSERT(block.hasEnded() && !m_operation);
    const Terminator& terminator = block.terminator;
    m_isAheadOfWhatMayThrow = false;
    TerminatorContext context(id, terminator, handlerOf(id));
    context.m_generatesCondition = generatesCondition;

    m_out.setOrigin(m_proc.traits().notesRangesOfOrigins ? machineOriginOf(terminator.kind) : Machine::Origin());

    m_operation = &context;
    declareAccessesOf(context);
    // The entry of the turns laid out has asked.
    if (generatesCondition && std::ranges::contains(m_testsThatHoldInTurnsLaidOut, terminator.conditionOperand())) [[unlikely]]
        jumpTo(context, 0);
    else
        dispatch(context);
    m_operation = nullptr;
    if (m_checksAllocation) [[unlikely]]
        m_out.clearDeclaredAccesses();
    RELEASE_ASSERT(m_hasFailed || currentBlockIsOpen() == m_nextBlockGoesOnHere);
}

void CodeGenerator::generateBranchCondition(const TerminatorContext& terminator, ValueID condition, BranchTargets targets)
{
    ValueContext context(terminator.block(), condition, code().at(condition), handlerOf(terminator.block()));
    context.m_branchTargets = targets;
    generate(context);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

void CodeGenerator::generateReturnedCall(const TerminatorContext& terminator, ValueID call)
{
    ValueContext context(terminator.block(), call, code().at(call), handlerOf(terminator.block()));
    context.m_returnCall = &terminator;
    generate(context);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

void CodeGenerator::declareAccessesSlow(const OperationContext& context)
{
    // Nobody else has the cell that is being filled, so what fills it is no effect of the operation.
    if (m_initializationDepth) {
        m_out.clearDeclaredAccesses();
        return;
    }

    Effects effects = effectsOf(context);
    HeapList reads;
    HeapList writes;
    heaps().appendHeaps(effects.reads, reads);
    heaps().appendHeaps(effects.writes, writes);
    // The IR's effects say what a program can observe. It cannot observe which form a cell is kept in: a type test reads those words and declares nothing.
    // Stores to them are still held to the declared writes.
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
        return BlockEdge(m_blockLeavingTurnLaidOut(), edgeClassLeadingTo(blockAtEnd.frequency));
    BlockEdge target(blockFor(edge.to), edgeClassLeadingTo(blockAtEnd.frequency));
    std::span<const ValueID> parameters = blockAtEnd.parameters();
    bool passesSomething = false;
    for (size_t index = 0; index < parameters.size() && !passesSomething; ++index)
        passesSomething = m_values[parameters[index].index()];
    if (!passesSomething)
        return target;
    // The code of any other terminator may take the edge from several machine blocks, however few targets it has.
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

void CodeGenerator::passArguments(const Edge& edge, std::span<const ValueID> parameters, std::span<const Operand> declaredResults)
{
    std::span<const ValueID> arguments = edge.arguments();
    RELEASE_ASSERT(parameters.size() == arguments.size() + declaredResults.size());
    if (parameters.empty())
        return;

    // A constant may take several machine blocks, and an Upsilon lies in the block that leads to its Phi: every operand is made ahead of the first Upsilon.
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

void CodeGenerator::passArgument(ValueID parameter, Operand argument)
{
    Representation representationOfParameter = isCarriedBoxed(parameter) ? Representation::JSValue : representationOf(parameter);
    if (!isSameWord(argument.representation, representationOfParameter)) {
        String forms = describeFormsOfGeneratedOperand(argument.representation, representationOfParameter);
        // An entrypoint passes what its convention delivers, outside any operation.
        if (m_operation)
            fail(*m_operation, InternalErrorKind::UnsupportedConversion, forms);
        else
            fail(ValueContext(code().blockOf(parameter), parameter, code().at(parameter), nullptr), InternalErrorKind::UnsupportedConversion, forms);
        return;
    }
    LValue phi = m_values[parameter.index()];
    RELEASE_ASSERT(phi && phi->opcode() == Machine::Phi && argument.value && argument.value->type() == phi->type());
    m_out.upsilon(argument.value, phi);
}

Operand CodeGenerator::get(ValueID id)
{
    return get(id, code().at(id));
}

Operand CodeGenerator::get(ValueID id, const Operation& operation)
{
    Representation representation = operation.representation;
    RELEASE_ASSERT(representation != Representation::Unchosen);
    if (representation == Representation::Void)
        return { };
    BlockID home = operation.block();
    if (!home)
        return makeConstant(id, operation);
    if (BlockID user = blockBeingGenerated(); user && user != home && home != m_onlyEntryBlock && !isProvenToDominate(home, user))
        verifyThatDefinitionDominates(id, user);
    LValue lowered = m_values[id.index()];
    // The build has failed, and what is generated until the next look at that is thrown away.
    if (!lowered && m_hasFailed) [[unlikely]]
        return emitter<ConstantEmitter>().zeroOf(representation);
    RELEASE_ASSERT(lowered);
    if (m_carriesParametersBoxed && representation == Representation::Double && m_parametersCarriedBoxed.quickGet(id.index())) [[unlikely]]
        return { Representation::JSValue, lowered };
    if (m_followsNumbersOutOfBytes && representation == Representation::Double) [[unlikely]] {
        if (m_valuesHoldingAnyNaN.quickGet(id.index()))
            return emitter<ConversionEmitter>().purified(Number(lowered));
    }
    return { representation, lowered };
}

void CodeGenerator::verifyThatDefinitionDominates(ValueID id, BlockID user)
{
    BlockID home = code().blockOf(id);
    if (home && dominates(home, user)) [[likely]] {
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
    // What is generated once more for one reader goes to that reader past get().
    if (!m_followsNumbersOutOfBytes || m_definitionTaken || m_valuesReadForBitsOfNaN.quickGet(value.id().index()))
        return emitter<ConversionEmitter>().purified(number);
    m_valuesHoldingAnyNaN.quickSet(value.id().index());
    return number;
}

// Whoever reads a parameter is generated behind the head of its block. Until then the parameter can still be marked, and behind that only what was marked takes such a word: an edge that leads back.
bool CodeGenerator::letsAnyNaNInto(ValueID parameter)
{
    if (holdsAnyNaN(parameter))
        return true;
    if (representationOf(parameter) != Representation::Double || m_valuesReadForBitsOfNaN.quickGet(parameter.index()) || m_blocksBegun.quickGet(code().blockOf(parameter).index()))
        return false;
    m_valuesHoldingAnyNaN.quickSet(parameter.index());
    return true;
}

Operand CodeGenerator::getIn(ValueID id, Representation wanted)
{
    // What is wanted without bits is taken in any form, as Walker::use() takes it: the place of a parameter that no caller hands over.
    if (wanted == Representation::Void)
        return { wanted, nullptr };

    const Operation& operation = code().at(id);
    if (isSameWord(operation.representation, wanted)) [[likely]]
        return { wanted, get(id, operation).value };

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

void CodeGenerator::defineValue(ValueID id, Representation representation, Operand operand)
{
    if (m_definitionTaken) [[unlikely]] {
        *m_definitionTaken = operand;
        return;
    }
    RELEASE_ASSERT(operand.representation == representation);
    if (operand.representation == Representation::Void)
        return;
    LValue& lowered = m_values[id.index()];
    RELEASE_ASSERT(!lowered && operand.value && operand.value->type() == machineTypeOf(operand.representation));
    lowered = operand.value;
}

void CodeGenerator::define(const ValueContext& value, Operand result)
{
    RELEASE_ASSERT(!value.isBranchCondition());
    defineValue(value.id(), value.representation(), result);
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

// In the entrypoint that is about to jump to the block of the parameter.
void CodeGenerator::defineParameter(ValueID parameter, Operand operand)
{
    RELEASE_ASSERT(opcodeOf(parameter) == Opcode::Parameter);
    if (m_values[parameter.index()])
        passArgument(parameter, operand);
}

Subject CodeGenerator::subjectOf(const OperationContext& context) const
{
    Subject subject { .function = unit().function, .location = code().locationOf(program(), context.origin()) };
    if (!hasModule(subject.location))
        subject.location = function().declarationLocation();
    // Without a location, a record's numbers are looked up in the body the front end wrote, where Code's numbers mean nothing.
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
    // A message prints the operation's name next to its number, and a record without a location carries no numbers.
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

ValueID valueOwnedByTerminator(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::TestAndBranch:
        return terminator.conditionOperand();
    case TerminatorKind::ReturnCall:
        return terminator.callOperand();
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Return:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
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

// Each block hands itself to its successors as it is come by, in reverse postorder: whatever leads forward to a block has been come by before the block.
// An edge that leads back changes nothing where the flow is made of loops with one head, and asks for another round elsewhere.
DominatorsOfCode::DominatorsOfCode(const Code& code, std::span<const BlockID> order)
{
    constexpr uint32_t unknown = UINT32_MAX;
    // By number.
    struct Node {
        uint32_t dominator;
        uint32_t sizeOfSubtree;
        uint32_t nextNumberBelow;
    };
    const uint32_t numberOfBlocks = order.size();
    // Above every entry stands a root, which has the number of no block.
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
            // The root is above everything, and has the greatest number.
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
