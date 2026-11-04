/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineAllocator.h"

#if ENABLE(AOT)

#include <algorithm>
#include <bit>
#include <cstring>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

// Without initializers: chooseRegistersKeptThroughLoop() has room for sixty-four and writes each one that it reads.
struct ValueLiveThroughLoop {
    Operation* value;
    Position nextReadBehindLoop;
};

// Blocks of other ways into the loop may stand between the walk and the loop, and the walk has not passed their reads.
static Position firstReadAtOrBehindStartOfLoop(const Allocator& allocator, const Operation* value, const Loop& loop)
{
    const Liveness& liveness = allocator.m_liveness;
    for (uint32_t index = allocator.m_values[value->index()].nextUse; index; index = liveness.use(index).next) {
        Position position = liveness.use(index).position;
        if (position >= loop.begin)
            return position;
    }
    return noPosition;
}

// What a rare region reads it loads for itself. In a loop that stands in a rare region every read counts.
static bool isReadInsideLoopOutsideRareRegions(const Allocator& allocator, const Operation* value, const Loop& loop, bool loopIsRare)
{
    const Liveness& liveness = allocator.m_liveness;
    for (uint32_t index = allocator.m_values[value->index()].nextUse; index; index = liveness.use(index).next) {
        Position position = liveness.use(index).position;
        if (position >= loop.end)
            return false;
        if (position >= loop.begin && (loopIsRare || !liveness.isUseInRareRegion(index)))
            return true;
    }
    return false;
}

static RegisterMask registersLoopMayTake(const Allocator& allocator, const Block* head, Bank bank)
{
    RegisterMask registers = allocator.m_file.allocatableIn(bank);
    if (allocator.m_order.of(head).isRare())
        registers &= ~allocator.m_file.calleeSaved | allocator.m_calleeSavesTakenOutsideRareRegions;
    if (allocator.isAheadOfFramedPart(head))
        registers &= ~allocator.m_file.calleeSaved;
    return registers;
}

// The flags do not say where the call or the suspension stands. If it stands in the loop, any other register is lost there, and every turn would load the value again for the head.
// If it stands elsewhere, the value has its spill or gets it there, and giving the register up costs one load.
// Ahead of the framed part no call stands in the loop, and no spill is written.
static RegisterMask registersWorthKeepingThroughLoop(const RegisterFile& file, const ValueFacts& facts, bool loopIsAheadOfFramedPart)
{
    if (facts.has(ValueFlag::LivesAcrossSuspension))
        return file.calleeSaved & ~file.calleeSavedClobberedAtSuspensions;
    if (facts.has(ValueFlag::LivesAcrossCall) && !loopIsAheadOfFramedPart)
        return file.calleeSaved;
    return ~RegisterMask { 0 };
}

// Its store would stand at the head of the loop, which every later turn enters with something else in the register.
static bool isDefinedOnEdgeIntoLoop(const Block* from, const Operation* value)
{
    const Operation* last = from->last();
    if (last->opcode() != Patch)
        return false;
    return value == last || (value->opcode() == Extract && value->child(0) == last);
}

void Allocator::chooseRegistersKeptThroughLoop(const Loop& loop, bool movesMayWriteRegisters)
{
    const Block* head = m_order.blocksInOrder()[loop.firstBlock];
    ASSERT(m_order.of(head).isLoopHead && m_order.headOf(head) == loop.begin);
    bool loopIsRare = m_order.of(head).isRare();
    bool comesRound = hasBackEdgeOutsideRareRegions(loop);
    const LoopFacts& loopFacts = m_liveness.of(loop, m_order);
    const RegisterMask takenByPatches = loopFacts.registersTakenByPatches;

    // The pressure counts what the loop defines, the Phis of its head among that, and what it reads, in rare regions too.
    RegisterMask registersOfLoop = 0;
    int numberOfSpareRegisters[numberOfBanks];
    for (Bank bank : { Bank::GP, Bank::FP }) {
        RegisterMask registers = registersLoopMayTake(*this, head, bank);
        numberOfSpareRegisters[static_cast<unsigned>(bank)] = std::popcount(registers) - static_cast<int>(loopFacts.pressure[static_cast<unsigned>(bank)]);
        registersOfLoop |= registers;
    }
    auto takeSpareRegister = [&](Bank bank) {
        return numberOfSpareRegisters[static_cast<unsigned>(bank)]-- > 0;
    };
    // No step can be appended here: the operation that ends the block has its step. A spill needs none.
    auto spillAheadOfLoop = [&](Operation* value) {
        markSpilled(value);
        unbindAll(value);
    };

    ValueLiveThroughLoop candidates[64];
    unsigned numberOfCandidates = 0;
    // Each turn would move such a value out of the way of the Patch, and the edge back to the head would move it back.
    Operation* valuesInRegistersPatchesTake[64];
    unsigned numberInRegistersPatchesTake = 0;
    forEachRegister(m_bound, [&](Reg reg) {
        Operation* value = m_valueIn[reg.index()];
        if (!value || (value->opcode() == Phi && value->owner() == head))
            return;

        // A range that ends where the loop starts is that of a loop in front of this one. No edge back to the head would bring such a value.
        const ValueFacts& facts = m_liveness.of(value);
        if (facts.has(ValueFlag::IsRematerialized)) {
            if (!isKeptThroughLoopAround(reg, value, loop.begin) || (maskOf(reg) & takenByPatches))
                unbindAll(value);
            return;
        }
        if (!isLiveBehind(value, loop.begin)) {
            unbindAll(value);
            return;
        }
        if (isReadInsideLoopOutsideRareRegions(*this, value, loop, loopIsRare)) {
            if (maskOf(reg) & takenByPatches)
                valuesInRegistersPatchesTake[numberInRegistersPatchesTake++] = value;
            return;
        }
        // A rare block alone leads back: a store ahead of the head would save no turn anything. What lives through stays where it is, as at any other join.
        if (!comesRound)
            return;
        // From here on it asks for a spare register like what only lives through, so it is not counted twice.
        if (isReadInside(value, loop))
            ++numberOfSpareRegisters[static_cast<unsigned>(bankOf(reg))];

        // The code outside a rare region does not pay for the region: what comes from there is stored by the region, where it takes the register.
        bool wouldBeStoredOutsideRareRegion = loopIsRare && isDefinedOutsideRareRegion(value) && !spillHoldsValueAtHeadOf(head, value);
        if (!value->type().isNumeric() || isDefinedOnEdgeIntoLoop(m_block, value) || wouldBeStoredOutsideRareRegion) {
            takeSpareRegister(bankOf(reg));
            return;
        }
        if (!(maskOf(reg) & registersWorthKeepingThroughLoop(m_file, facts, isAheadOfFramedPart(head) && !loopIsRare) & ~takenByPatches)) {
            spillAheadOfLoop(value);
            return;
        }
        // The loop has no use for the register.
        if (!(maskOf(reg) & registersOfLoop))
            return;

        // Without a read that is left, a turn of a loop around this one reads the value again.
        Position nextRead = firstReadAtOrBehindStartOfLoop(*this, value, loop);
        candidates[numberOfCandidates++] = { value, nextRead == noPosition ? facts.lastUse : nextRead };
    });

    std::sort(candidates, candidates + numberOfCandidates, [](const ValueLiveThroughLoop& left, const ValueLiveThroughLoop& right) {
        if (left.nextReadBehindLoop != right.nextReadBehindLoop)
            return left.nextReadBehindLoop < right.nextReadBehindLoop;
        return left.value->index() < right.value->index();
    });
    for (unsigned index = 0; index < numberOfCandidates; ++index) {
        Operation* value = candidates[index].value;
        if (!takeSpareRegister(bankOf(value->type())))
            spillAheadOfLoop(value);
    }
    if (!takenByPatches || !comesRound)
        return;

    // The registers that no Patch of the loop takes may be few. What the loop reads, and the Phis of the head that wait for a register, come before what only lives through:
    // that costs a store and a load outside the loop, these a move or a load on every turn.
    auto registersLeftAlone = [&](Bank bank) {
        return registersLoopMayTake(*this, head, bank) & ~takenByPatches & ~m_bound;
    };
    if (movesMayWriteRegisters) {
        unsigned numberWanted[numberOfBanks] { 0, 0 };
        for (unsigned index = 0; index < numberInRegistersPatchesTake; ++index)
            ++numberWanted[static_cast<unsigned>(bankOf(valuesInRegistersPatchesTake[index]->type()))];
        for (const Operation* phi : head->phis()) {
            if (!m_liveness.isDropped(phi) && !stateOf(phi).registers)
                ++numberWanted[static_cast<unsigned>(bankOf(phi->type()))];
        }
        for (unsigned index = numberOfCandidates; index--;) {
            Operation* value = candidates[index].value;
            Bank bank = bankOf(value->type());
            if (stateOf(value).registers && static_cast<unsigned>(std::popcount(registersLeftAlone(bank))) < numberWanted[static_cast<unsigned>(bank)])
                spillAheadOfLoop(value);
        }
    }
    for (unsigned index = 0; index < numberInRegistersPatchesTake; ++index) {
        Operation* value = valuesInRegistersPatchesTake[index];
        Bank bank = bankOf(value->type());
        RegisterMask leftAlone = registersLeftAlone(bank);
        if (movesMayWriteRegisters && leftAlone) {
            Reg target = takeRegister(bank, leftAlone);
            unbindAll(value);
            bind(target, value);
        } else if (spillHoldsValueAtHeadOf(head, value))
            unbindAll(value);
    }
}

bool Allocator::isReadInside(const Operation* value, const Loop& loop) const
{
    return firstReadAtOrBehindStartOfLoop(*this, value, loop) < loop.end;
}

void Allocator::bringInWhatLoopReads(const Loop& loop, Position limitOfDefinitions)
{
    const LoopFacts& loopFacts = m_liveness.of(loop, m_order);
    if (loopFacts.valuesReadFromOutside.empty() || !hasBackEdgeOutsideRareRegions(loop))
        return;
    const Block* head = m_order.blocksInOrder()[loop.firstBlock];
    bool loopIsRare = m_order.of(head).isRare();

    // The pressure counts the Phis of the head and what the loop reads. What is left of it once those that have their registers are taken off is what the loop still asks for.
    RegisterMask registers[numberOfBanks];
    unsigned numberFree[numberOfBanks];
    unsigned numberStillAskedFor[numberOfBanks];
    for (Bank bank : { Bank::GP, Bank::FP }) {
        unsigned index = static_cast<unsigned>(bank);
        registers[index] = registersLoopMayTake(*this, head, bank);
        numberFree[index] = static_cast<unsigned>(std::popcount(registers[index] & ~m_bound));
        numberStillAskedFor[index] = loopFacts.pressure[index];
    }
    forEachRegister(m_bound, [&](Reg reg) {
        const Operation* value = m_valueIn[reg.index()];
        unsigned& number = numberStillAskedFor[static_cast<unsigned>(bankOf(reg))];
        bool isCounted = !m_liveness.of(value).has(ValueFlag::IsRematerialized) && ((value->opcode() == Phi && value->owner() == head) || isReadInside(value, loop));
        if (isCounted && number)
            --number;
    });

    for (Operation* value : loopFacts.valuesReadFromOutside) {
        if (stateOf(value).registers || !value->type().isNumeric() || !isReadInsideLoopOutsideRareRegions(*this, value, loop, loopIsRare))
            continue;
        const ValueFacts& facts = m_liveness.of(value);
        bool isRematerialized = facts.has(ValueFlag::IsRematerialized);
        if (!isRematerialized && (!spillHoldsValueAtHeadOf(head, value) || facts.definition >= limitOfDefinitions))
            continue;

        Bank bank = bankOf(value->type());
        unsigned index = static_cast<unsigned>(bank);
        RegisterMask candidates = registers[index] & ~m_bound & ~loopFacts.registersTakenByPatches & (~m_file.calleeSaved | m_allocation.usedCalleeSaves());
        // takeRegister() evicts what is made again before it takes a callee-saved register for the first time, and so does whatever gives way to a Patch.
        unsigned room = isRematerialized ? static_cast<unsigned>(std::popcount(candidates)) : numberFree[index] + 1;
        if (!candidates || room <= numberStillAskedFor[index])
            continue;

        Reg hint = hintOf(value);
        Reg reg = takeRegister(bank, candidates, hint && (candidates & maskOf(hint)) ? hint : Reg());
        bind(reg, value);
        --numberFree[index];
        if (isRematerialized) {
            m_rematerializedValueKeptIn[reg.index()] = value;
            m_rematerializedValueIsKeptUntil[reg.index()] = loop.end;
            m_endOfLastLoopThatKeepsValueMadeAgain = std::max(m_endOfLastLoopThatKeepsValueMadeAgain, loop.end);
        } else if (numberStillAskedFor[index])
            --numberStillAskedFor[index];
    }
}

bool Allocator::isKeptThroughLoopAround(Reg reg, const Operation* value, Position position) const
{
    return reg && m_rematerializedValueKeptIn[reg.index()] == value && position < m_rematerializedValueIsKeptUntil[reg.index()];
}

// By the place of a loop in Order::loops(), which rises, and one entry more: the first loop at or behind it whose head isDeadOnEveryPathFrom() says nothing of.
// That is a loop within a loop at or behind it, whose first child is the next one in the list, and a loop in a rare region.
static void findLoopsWhoseHeadsAreNotJudged(Allocator& allocator)
{
    const Order& order = allocator.m_order;
    std::span<const Loop> loops = order.loops();
    uint32_t numberOfLoops = static_cast<uint32_t>(loops.size());
    allocator.m_firstLoopsWhoseHeadsAreNotJudged.fill(allocator.m_arena, numberOfLoops + 1, numberOfLoops);
    for (uint32_t loop = numberOfLoops; loop--;) {
        uint32_t first = allocator.m_firstLoopsWhoseHeadsAreNotJudged[loop + 1];
        if (loop + 1 < numberOfLoops && loops[loop + 1].parent == loop)
            first = loop + 1;
        if (order.of(order.blocksInOrder()[loops[loop].firstBlock]).isRare())
            first = loop;
        allocator.m_firstLoopsWhoseHeadsAreNotJudged[loop] = first;
    }
}

// `place` is that of the block, which stands outside rare regions. The value is defined at `from` or ahead of it, and is not kept for the collector.
static NEVER_INLINE bool isDeadOnEveryPathFromBlockOutsideRareRegions(Allocator& allocator, const Operation* value, const Block* block, const BlockPlace& place, Position from)
{
    const Liveness& liveness = allocator.m_liveness;
    const Order& order = allocator.m_order;
    const Position head = place.head;
    const ValueFacts& facts = liveness.of(value);
    const Position definition = facts.definition;
    ASSERT(definition != noPosition && definition <= from && !facts.has(ValueFlag::IsKeptForCollector));
    uint32_t use = allocator.findReadFrom(value, from);
    // Order::endOf().
    if (use && liveness.use(use).position < head + 2 * (block->size() + 1))
        return false;

    // No way leads on from the block to the read, nor back to a turn of a loop that would read the value again.
    if (Position end = liveness.endOfWhatWaysLeadToFrom(block); end != noPosition && (!use || liveness.use(use).position >= end))
        return true;

    // An edge that leads back leads to the start of a loop. From a block of this loop that is the start of this loop, of a loop further on in it, or of a loop around it, which is a way out.
    // At the head of a loop nothing of the loop has run, and the blocks ahead of the head are those ahead of the loop. So what is defined ahead of the loop is judged there by the loop around.
    const Loop* const loops = order.loops().data();
    const uint32_t numberOfLoops = static_cast<uint32_t>(order.loops().size());
    Position startOfReadsBehindLoop = noPosition;
    uint32_t loopAround = place.loop;
    if (place.isLoopHead && from == head && definition < loops[loopAround].begin)
        loopAround = loops[loopAround].parent;
    if (loopAround != noLoop) {
        const Loop& loop = loops[loopAround];
        if (definition < loop.begin)
            return false;
        if (head >= liveness.of(loop, order).endOfLastExitingBlock)
            startOfReadsBehindLoop = loop.end;
    }
    // Every read counts, and one of them is not known to be out of reach.
    if (use && startOfReadsBehindLoop == noPosition && liveness.use(use).greatestHeadOfBlockPassedOnEveryPath >= head)
        return false;

    // What is dead here has to be dead at every head that a way from here leads to: the edge owes the head whatever the head holds.
    // The head of a loop that starts further on is judged as this block is: whatever stands ahead of this block stands ahead of it. That holds of no loop within such a loop and of none in a rare region:
    // its head holds what its first edge brings and is live by its range, and a way may lead from here into the loop beside its head, and round it to the head. Order::loops() rises.
    if (numberOfLoops) {
        if (allocator.m_firstLoopsWhoseHeadsAreNotJudged.isEmpty())
            findLoopsWhoseHeadsAreNotJudged(allocator);
        if (allocator.m_positionOfLastQuestionForLoopNotJudged != from) {
            allocator.m_positionOfLastQuestionForLoopNotJudged = from;
            const Loop* firstLoopBehind = std::upper_bound(loops, loops + numberOfLoops, from, [](Position position, const Loop& loop) {
                return position < loop.begin;
            });
            uint32_t notJudged = allocator.m_firstLoopsWhoseHeadsAreNotJudged[static_cast<uint32_t>(firstLoopBehind - loops)];
            allocator.m_beginOfFirstLoopNotJudged = notJudged < numberOfLoops ? loops[notJudged].begin : noPosition;
        }
        if (allocator.m_beginOfFirstLoopNotJudged < std::min(facts.lastUse, startOfReadsBehindLoop))
            return false;
    }
    // A range that goes on behind the last read was extended over a loop within this one, which the walk has left.
    // Without a loop Use::greatestHeadOfBlockPassedOnEveryPath has spoken for every read that is left, and nobody asks for what is noted below: only a block with a loop around it does.
    if (!use || !numberOfLoops)
        return true;

    // A way from the block to a read that every path reaches through a block ahead of this one leads back over this one, so over the start of a loop around it.
    ArenaVector<uint32_t>& firstReadsNotKnownOutOfReach = allocator.m_firstReadsNotKnownOutOfReach;
    bool isDead = true;
    if (startOfReadsBehindLoop == noPosition) {
        // Use::greatestHeadOfBlockPassedOnEveryPath has spoken for this read and for all behind it: none is gone through.
        use = 0;
    } else {
        if (!firstReadsNotKnownOutOfReach.isEmpty()) {
            uint32_t known = firstReadsNotKnownOutOfReach[value->index()];
            if (known == UINT32_MAX)
                return true;
            if (known && liveness.use(use).position < liveness.use(known).position)
                use = known;
        }
        uint32_t firstInDoubt = use;
        for (; use; use = liveness.use(use).next) {
            const Use& read = liveness.use(use);
            if (read.position >= startOfReadsBehindLoop)
                break;
            if (read.headOfBlockPassedOnEveryPath >= head) {
                isDead = false;
                break;
            }
        }
        if (use == firstInDoubt)
            return isDead;
    }
    // The walk only goes on in the order. A head that an edge fixes stands further on than the walk, and what is out of its reach may be within reach of a block in between.
    if (block == allocator.m_block) {
        if (firstReadsNotKnownOutOfReach.isEmpty()) {
            const uint32_t numberOfOperations = allocator.m_proc.numberOfOperations();
            firstReadsNotKnownOutOfReach.growWithoutInitializing(allocator.m_arena, numberOfOperations);
            memset(firstReadsNotKnownOutOfReach.begin(), 0, static_cast<size_t>(numberOfOperations) * sizeof(uint32_t));
        }
        firstReadsNotKnownOutOfReach[value->index()] = use ? use : UINT32_MAX;
    }
    return isDead;
}

bool Allocator::isDeadOnEveryPathFrom(const Operation* value, const Block* block, Position from)
{
    const ValueFacts& facts = m_valueFacts[value->index()];
    if (facts.definition == noPosition || facts.definition > from || facts.has(ValueFlag::IsKeptForCollector))
        return false;
    const BlockPlace& place = m_order.of(block);
    // A value that has lost its register in a rare region was saved on the way: forEachValueHeldOnlyInRareRegionSave() and appendEdgeMoves() load it from ValueState::saveInRareRegion, which another arm may have set.
    if (place.isRare())
        return false;
    return isDeadOnEveryPathFromBlockOutsideRareRegions(*this, value, block, place, from);
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
