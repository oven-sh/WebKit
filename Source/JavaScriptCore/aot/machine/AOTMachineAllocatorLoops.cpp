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

struct ValueLiveThroughLoop {
    Operation* value;
    Position nextReadBehindLoop;
};

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

static RegisterMask registersWorthKeepingThroughLoop(const RegisterFile& file, const ValueFacts& facts, bool loopIsAheadOfFramedPart)
{
    if (facts.has(ValueFlag::LivesAcrossSuspension))
        return file.calleeSaved & ~file.calleeSavedClobberedAtSuspensions;
    if (facts.has(ValueFlag::LivesAcrossCall) && !loopIsAheadOfFramedPart)
        return file.calleeSaved;
    return ~RegisterMask { 0 };
}

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
    auto spillAheadOfLoop = [&](Operation* value) {
        markSpilled(value);
        unbindAll(value);
    };

    ValueLiveThroughLoop candidates[64];
    unsigned numberOfCandidates = 0;
    Operation* valuesInRegistersPatchesTake[64];
    unsigned numberInRegistersPatchesTake = 0;
    forEachRegister(m_bound, [&](Reg reg) {
        Operation* value = m_valueIn[reg.index()];
        if (!value || (value->opcode() == Phi && value->owner() == head))
            return;

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
        if (!comesRound)
            return;
        if (isReadInside(value, loop))
            ++numberOfSpareRegisters[static_cast<unsigned>(bankOf(reg))];

        bool wouldBeStoredOutsideRareRegion = loopIsRare && isDefinedOutsideRareRegion(value) && !spillHoldsValueAtHeadOf(head, value);
        if (!value->type().isNumeric() || isDefinedOnEdgeIntoLoop(m_block, value) || wouldBeStoredOutsideRareRegion) {
            takeSpareRegister(bankOf(reg));
            return;
        }
        if (!(maskOf(reg) & registersWorthKeepingThroughLoop(m_file, facts, isAheadOfFramedPart(head) && !loopIsRare) & ~takenByPatches)) {
            spillAheadOfLoop(value);
            return;
        }
        if (!(maskOf(reg) & registersOfLoop))
            return;

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

static NEVER_INLINE bool isDeadOnEveryPathFromBlockOutsideRareRegions(Allocator& allocator, const Operation* value, const Block* block, const BlockPlace& place, Position from)
{
    const Liveness& liveness = allocator.m_liveness;
    const Order& order = allocator.m_order;
    const Position head = place.head;
    const ValueFacts& facts = liveness.of(value);
    const Position definition = facts.definition;
    ASSERT(definition != noPosition && definition <= from && !facts.has(ValueFlag::IsKeptForCollector));
    uint32_t use = allocator.findReadFrom(value, from);
    if (use && liveness.use(use).position < head + 2 * (block->size() + 1))
        return false;

    if (Position end = liveness.endOfWhatWaysLeadToFrom(block); end != noPosition && (!use || liveness.use(use).position >= end))
        return true;

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
    if (use && startOfReadsBehindLoop == noPosition && liveness.use(use).greatestHeadOfBlockPassedOnEveryPath >= head)
        return false;

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
    if (!use || !numberOfLoops)
        return true;

    ArenaVector<uint32_t>& firstReadsNotKnownOutOfReach = allocator.m_firstReadsNotKnownOutOfReach;
    bool isDead = true;
    if (startOfReadsBehindLoop == noPosition) {
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
    if (block == allocator.m_block) {
        if (firstReadsNotKnownOutOfReach.isEmpty()) {
            const uint32_t numberOfOperations = allocator.m_order.mostOperations();
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
    if (place.isRare())
        return false;
    return isDeadOnEveryPathFromBlockOutsideRareRegions(*this, value, block, place, from);
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
