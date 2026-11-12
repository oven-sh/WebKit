/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineAllocator.h"

#if ENABLE(AOT)

#include "AOTMachineCallsOnWays.h"
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
    uint32_t read = allocator.findReadFrom(value, loop.begin);
    return read ? allocator.m_liveness.use(read).position : noPosition;
}

static bool isReadInsideLoopOutsideRareRegions(const Allocator& allocator, const Operation* value, const Loop& loop, bool loopIsRare)
{
    const Liveness& liveness = allocator.m_liveness;
    unsigned stepsLeft = 64;
    for (uint32_t index = allocator.findReadFrom(value, loop.begin); index; index = liveness.use(index).next) {
        Position position = liveness.use(index).position;
        if (position >= loop.end)
            return false;
        if (loopIsRare || !liveness.isUseInRareRegion(index))
            return true;
        if (!--stepsLeft) [[unlikely]] {
            uint32_t read = allocator.findReadOutsideUnlikelyRegions(value, position);
            return read && liveness.use(read).position < loop.end;
        }
    }
    return false;
}

static RegisterMask registersLoopMayTake(const Allocator& allocator, const Block* head, Bank bank)
{
    RegisterMask registers = allocator.m_file.allocatableIn(bank);
    if (allocator.m_order.of(head).isRare())
        registers &= ~allocator.m_file.calleeSaved | allocator.m_calleeSavesTakenOutsideRareRegions;
    if (allocator.isOutsideFramedPart(head))
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

Reg Allocator::takeRegisterOfValueThatLoopReadsBehindPhi(const Loop& loop, const Operation* phi)
{
    const Block* head = m_order.blocksInOrder()[loop.firstBlock];
    const ValueFacts& factsOfPhi = m_liveness.of(phi);
    if (m_order.of(head).isRare() || !hasBackEdgeOutsideRareRegions(loop) || factsOfPhi.has(ValueFlag::LivesAcrossCall) || factsOfPhi.has(ValueFlag::LivesAcrossSuspension) || factsOfPhi.has(ValueFlag::LivesIntoLandingPad))
        return Reg();
    Position latestRead = findFirstReadFromHeadOf(head, phi);
    if (latestRead == noPosition || latestRead >= loop.end)
        return Reg();

    Reg chosen;
    forEachRegister(m_bound & registersLoopMayTake(*this, head, bankOf(phi->type())) & ~m_liveness.of(loop, m_order).registersTakenByPatches, [&](Reg reg) {
        const Operation* value = m_valueIn[reg.index()];
        if (!value || (value->opcode() == Phi && value->owner() == head) || !value->type().isNumeric() || isDefinedOnEdgeIntoLoop(m_block, value) || m_liveness.of(value).has(ValueFlag::IsRematerialized))
            return;
        Position read = firstReadAtOrBehindStartOfLoop(*this, value, loop);
        if (read == noPosition || read <= latestRead)
            return;
        latestRead = read;
        chosen = reg;
    });
    if (!chosen)
        return Reg();
    Operation* value = m_valueIn[chosen.index()];
    markSpilled(value);
    unbindAll(value);
    return chosen;
}

const Block* Allocator::headOfLoopThatBlockOnlyLeadsBackTo(const Block* block) const
{
    if (!block->last()->isUnconditionalJump() || m_order.of(block).isRare())
        return nullptr;
    const Block* next = block->successor(0).block();
    const BlockPlace& place = m_order.of(next);
    return place.isLoopHead && !place.isRare() && place.head < m_order.headOf(block) && m_heads[next->index()].isSet() ? next : nullptr;
}

void Allocator::dropWhatBlockThatLeadsBackNeitherReadsNorHandsOn(const Block* block, const Block* headOfLoop)
{
    const Position end = m_order.endOf(block);
    std::span<const RegisterBinding> held = m_heads[headOfLoop->index()].bindings();
    forEachRegister(m_bound, [&](Reg reg) {
        Operation* value = m_valueIn[reg.index()];
        if (!value || value->owner() == block || m_liveness.of(value).has(ValueFlag::IsRematerialized) || !value->type().isNumeric() || !spillHoldsValueAtHeadOf(block, value))
            return;
        if (findFirstReadFromHeadOf(block, value) < end)
            return;
        for (const RegisterBinding& binding : held) {
            if (binding.reg == reg && binding.value == value)
                return;
        }
        unbindAll(value);
    });
}

void Allocator::holdWhatHeadOfLoopHolds(const Block* block, const Block* headOfLoop, Position limitOfDefinitions)
{
    for (const RegisterBinding& binding : m_heads[headOfLoop->index()].bindings()) {
        Operation* value = binding.value;
        if (!value || value->owner() == headOfLoop || m_valueIn[binding.reg.index()] || stateOf(value).registers || !value->type().isNumeric())
            continue;
        const ValueFacts& facts = m_liveness.of(value);
        if (facts.has(ValueFlag::IsRematerialized) || facts.definition >= limitOfDefinitions || !spillHoldsValueAtHeadOf(block, value))
            continue;
        bind(binding.reg, value);
    }
}

void Allocator::chooseRegistersKeptThroughLoop(const Loop& loop, bool movesMayWriteRegisters)
{
    const Block* head = m_order.blocksInOrder()[loop.firstBlock];
    ASSERT(m_order.of(head).isLoopHead && m_order.headOf(head) == loop.begin);
    bool loopIsRare = m_order.of(head).isRare();
    bool hasHotBackEdge = hasBackEdgeOutsideRareRegions(loop);
    const LoopFacts& loopFacts = m_liveness.of(loop, m_order);
    const RegisterMask takenByPatches = loopFacts.registersTakenByPatches;

    RegisterMask registersOfLoop = 0;
    int numberOfSpareRegisters[numberOfBanks];
    int numberOfSpareRegistersThatPatchesLeave[numberOfBanks];
    for (Bank bank : { Bank::GP, Bank::FP }) {
        RegisterMask registers = registersLoopMayTake(*this, head, bank);
        numberOfSpareRegisters[static_cast<unsigned>(bank)] = std::popcount(registers) - static_cast<int>(loopFacts.pressure[static_cast<unsigned>(bank)]);
        numberOfSpareRegistersThatPatchesLeave[static_cast<unsigned>(bank)] = std::popcount(registers & ~takenByPatches) - static_cast<int>(loopFacts.pressureAcrossPatches[static_cast<unsigned>(bank)]);
        registersOfLoop |= registers;
    }
    auto takeSpareRegister = [&](Reg reg) {
        unsigned bank = static_cast<unsigned>(bankOf(reg));
        bool isSpare = numberOfSpareRegisters[bank]-- > 0;
        if (maskOf(reg) & takenByPatches)
            return isSpare;
        return numberOfSpareRegistersThatPatchesLeave[bank]-- > 0 && isSpare;
    };
    auto spillAheadOfLoop = [&](Operation* value) {
        markSpilled(value);
        unbindAll(value);
    };
    auto mayBeSpilledAheadOfLoop = [&](const Operation* value) {
        bool wouldBeStoredOutsideRareRegion = loopIsRare && isDefinedOutsideRareRegion(value) && !spillHoldsValueAtHeadOf(head, value);
        const ValueFacts& facts = m_liveness.of(value);
        bool spillWouldHoldNothing = hasImplicitReader(facts) && startsOutsideFramedPart(head) && isStoredAtHeadsOfFramedPart(value);
        return value->type().isNumeric() && !isDefinedOnEdgeIntoLoop(m_block, value) && !wouldBeStoredOutsideRareRegion && !spillWouldHoldNothing;
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
        if (!hasHotBackEdge)
            return;
        if (isReadInside(value, loop)) {
            ++numberOfSpareRegisters[static_cast<unsigned>(bankOf(reg))];
            ++numberOfSpareRegistersThatPatchesLeave[static_cast<unsigned>(bankOf(reg))];
        }

        if (!mayBeSpilledAheadOfLoop(value)) {
            takeSpareRegister(reg);
            return;
        }
        if (!(maskOf(reg) & registersWorthKeepingThroughLoop(m_file, facts, isOutsideFramedPart(head) && !loopIsRare) & ~takenByPatches)) {
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
        if (!takeSpareRegister(anyRegisterOf(value, stateOf(value).registers)))
            spillAheadOfLoop(value);
    }
    if (!takenByPatches || !hasHotBackEdge)
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
        else if (RegisterMask held = stateOf(value).registers; !(held & (held - 1)) && !isOutsideFramedPart(head) && mayBeSpilledAheadOfLoop(value) && firstCallBehindHeadReadsValueElsewhere(head, anyRegisterOf(value, held), value))
            spillAheadOfLoop(value);
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
    unsigned numberStillAskedForAcrossPatches[numberOfBanks];
    for (Bank bank : { Bank::GP, Bank::FP }) {
        unsigned index = static_cast<unsigned>(bank);
        registers[index] = registersLoopMayTake(*this, head, bank);
        numberFree[index] = static_cast<unsigned>(std::popcount(registers[index] & ~m_bound));
        numberStillAskedFor[index] = loopFacts.pressure[index];
        numberStillAskedForAcrossPatches[index] = loopFacts.pressureAcrossPatches[index];
    }
    forEachRegister(m_bound, [&](Reg reg) {
        const Operation* value = m_valueIn[reg.index()];
        unsigned& number = numberStillAskedFor[static_cast<unsigned>(bankOf(reg))];
        unsigned& numberAcrossPatches = numberStillAskedForAcrossPatches[static_cast<unsigned>(bankOf(reg))];
        bool isCounted = !m_liveness.of(value).has(ValueFlag::IsRematerialized) && ((value->opcode() == Phi && value->owner() == head) || isReadInside(value, loop));
        if (isCounted && number)
            --number;
        if (isCounted && numberAcrossPatches && !(maskOf(reg) & loopFacts.registersTakenByPatches))
            --numberAcrossPatches;
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
        if (!isRematerialized && static_cast<unsigned>(std::popcount(candidates)) + 1 <= numberStillAskedForAcrossPatches[index])
            continue;

        Reg hint = hintOf(value);
        Reg reg = takeRegister(bank, candidates, hint && (candidates & maskOf(hint)) ? hint : Reg());
        bind(reg, value);
        --numberFree[index];
        if (isRematerialized) {
            m_rematerializedValueKeptIn[reg.index()] = value;
            m_rematerializedValueIsKeptUntil[reg.index()] = loop.end;
            m_endOfLastLoopThatKeepsValueMadeAgain = std::max(m_endOfLastLoopThatKeepsValueMadeAgain, loop.end);
        } else {
            if (numberStillAskedFor[index])
                --numberStillAskedFor[index];
            if (numberStillAskedForAcrossPatches[index])
                --numberStillAskedForAcrossPatches[index];
        }
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

static ALWAYS_INLINE uint32_t passOverReadsAheadOfOnlyWayOn(const Allocator& allocator, const Operation* value, const Block* block, Position endOfBlock, uint32_t use)
{
    if (block->numSuccessors() != 1)
        return use;
    const Liveness& liveness = allocator.m_liveness;
    const Order& order = allocator.m_order;
    const BlockPlace& target = order.places()[block->successor(0).block()->index()];
    Position endOfWhatIsOutOfReach = target.head;
    if (!use || liveness.use(use).position >= endOfWhatIsOutOfReach)
        return use;
    const Loop* const loops = order.loops().data();
    for (uint32_t loop = target.loop; loop != noLoop && loops[loop].begin >= endOfBlock; loop = loops[loop].parent)
        endOfWhatIsOutOfReach = loops[loop].begin;
    return allocator.findReadFromAmongLaterReads(value, use, endOfWhatIsOutOfReach);
}

static void noteReadOfValue(Allocator& allocator, ArenaVector<uint32_t>& readsByValue, const Operation* value, uint32_t read)
{
    if (readsByValue.isEmpty()) {
        const uint32_t numberOfOperations = allocator.m_order.mostOperations();
        readsByValue.growWithoutInitializing(allocator.m_arena, numberOfOperations);
        memset(readsByValue.begin(), 0, static_cast<size_t>(numberOfOperations) * sizeof(uint32_t));
    }
    readsByValue[value->index()] = read;
}

enum class LoopsAroundBlock : uint8_t {
    AreAsked,
    AreAskedByCaller,
};

static bool isDeadOnEveryPathFromBlock(LoopsAroundBlock, Allocator&, const Operation* value, const Block*, const BlockPlace&, Position from, uint32_t firstReadFrom);

static bool isDeadOnWaysOnFromHeadOf(const Loop& loop, Allocator& allocator, const Operation* value)
{
    ASSERT(loop.hasSideEntrance && allocator.m_liveness.of(value).definition < loop.begin);
    const Block* head = allocator.m_order.blocksInOrder()[loop.firstBlock];
    const BlockPlace& place = allocator.m_order.of(head);
    return !place.isRare() && isDeadOnEveryPathFromBlock(LoopsAroundBlock::AreAskedByCaller, allocator, value, head, place, loop.begin, allocator.findReadFromInList(value, loop.begin));
}

NEVER_INLINE bool isDeadOnEveryPathFromBlockOutsideRareRegions(Allocator& allocator, const Operation* value, const Block* block, const BlockPlace& place, Position from, uint32_t use)
{
    ASSERT(use == allocator.findReadFrom(value, from));
    return isDeadOnEveryPathFromBlock(LoopsAroundBlock::AreAsked, allocator, value, block, place, from, use);
}

static bool isDeadOnEveryPathFromBlock(LoopsAroundBlock loopsAround, Allocator& allocator, const Operation* value, const Block* block, const BlockPlace& place, Position from, uint32_t use)
{
    const Liveness& liveness = allocator.m_liveness;
    const Order& order = allocator.m_order;
    const Position head = place.head;
    const ValueFacts& facts = liveness.of(value);
    const Position definition = facts.definition;
    ASSERT(definition != noPosition && definition <= from && !facts.has(ValueFlag::IsKeptForCollector));
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
    if (loopsAround == LoopsAroundBlock::AreAskedByCaller && loopAround != noLoop && definition < loops[loopAround].begin)
        loopAround = innermostLoopAroundBoth(loops, order.of(value->owner()).loop, head);
    while (loopAround != noLoop && definition < loops[loopAround].begin && loops[loopAround].hasSideEntrance && isDeadOnWaysOnFromHeadOf(loops[loopAround], allocator, value))
        loopAround = loops[loopAround].parent;
    if (loopAround != noLoop) {
        const Loop& loop = loops[loopAround];
        if (definition < loop.begin)
            return false;
        if (head >= liveness.of(loop, order).endOfLastExitingBlock)
            startOfReadsBehindLoop = loop.end;
    }
    const uint32_t firstReadBehindBlock = use;
    use = passOverReadsAheadOfOnlyWayOn(allocator, value, block, head + 2 * (block->size() + 1), use);
    const bool hasPassedOverReads = use != firstReadBehindBlock;
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
        if (!firstReadsNotKnownOutOfReach.isEmpty() && head >= allocator.m_headOfBlock) {
            uint32_t known = firstReadsNotKnownOutOfReach[value->index()];
            if (known == UINT32_MAX)
                return true;
            if (known && liveness.use(use).position < liveness.use(known).position)
                use = known;
        }
        uint32_t firstInDoubt = use;
        for (; use; use = liveness.use(use).next) {
            ++allocator.m_laterUsesScanned;
            const Use& read = liveness.use(use);
            if (read.position >= startOfReadsBehindLoop)
                break;
            if (read.headOfBlockPassedOnEveryPath >= head) {
                isDead = false;
                break;
            }
            if (use != firstInDoubt || allocator.m_lastReadsThatCounted.isEmpty())
                continue;
            if (uint32_t counted = allocator.m_lastReadsThatCounted[value->index()]) {
                const Use& kept = liveness.use(counted);
                if (kept.position > read.position && kept.position < startOfReadsBehindLoop && kept.headOfBlockPassedOnEveryPath >= head)
                    return false;
            }
        }
        if (use == firstInDoubt)
            return isDead;
        if (!isDead)
            noteReadOfValue(allocator, allocator.m_lastReadsThatCounted, value, use);
    }
    if (block == allocator.m_block && !hasPassedOverReads)
        noteReadOfValue(allocator, firstReadsNotKnownOutOfReach, value, use ? use : UINT32_MAX);
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
    return isDeadOnEveryPathFromBlockOutsideRareRegions(*this, value, block, place, from, findReadFrom(value, from));
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
