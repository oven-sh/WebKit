/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineAllocator.h"

#if ENABLE(AOT)

#include "AOTInliningAcrossFiles.h"
#include "AOTMachineCallsOnWays.h"
#include <bit>

namespace JSC { namespace AOT { namespace Machine {

static constexpr unsigned costOfCalleeSaveNotYetSaved = 2;
static constexpr unsigned mostReloadsThatCostNoMoreBytesThanCalleeSave = 4;

static NEVER_INLINE bool slotServesEveryReadAsCheaplyAsRegister(const Allocator& allocator, const Operation* argument, const Block* blockOfWalk)
{
    const Liveness& liveness = allocator.m_liveness;
    const uint32_t firstUse = allocator.findReadFrom(argument, allocator.m_position + 1);
    unsigned numberOfReads = 0;
    for (uint32_t use = firstUse; use; use = liveness.use(use).next) {
        if (++numberOfReads > mostReloadsThatCostNoMoreBytesThanCalleeSave)
            return false;
    }

    const Order& order = allocator.m_order;
    uint32_t place = order.of(blockOfWalk).orderIndex;
    for (uint32_t use = firstUse; use; use = liveness.use(use).next) {
        Position position = liveness.use(use).position;
        place = findPlaceOfBlockAtPosition(order, position, place);
        const Block* block = order.blocksInOrder()[place];
        unsigned numberInBlock = (position - order.of(block).head) / 2;
        if (!numberInBlock || numberInBlock > block->size())
            return false;
        const Operation* reader = block->at(numberInBlock - 1);
        if (reader->opcode() == Jump)
            continue;
        if (reader->opcode() == Return) {
            if (reader->numberOfInlineChildren() != 1 || reader->inlineChildren()[0] != argument)
                return false;
            continue;
        }
        const PatchOperation* patch = reader->as<PatchOperation>();
        if (!patch)
            return false;
        bool isOperand = false;
        std::span<Operation* const> operands = patch->operands();
        for (unsigned child = 0; child < operands.size(); ++child) {
            if (operands[child] != argument)
                continue;
            const ValueRep& constraint = patch->constraintOf(child);
            if (!constraint.isReg() && constraint.kind() != ValueRep::ColdAny && constraint.kind() != ValueRep::LateColdAny)
                return false;
            isOperand = true;
        }
        if (!isOperand)
            return false;
    }
    return numberOfReads;
}

static NEVER_INLINE bool isWorthCalleeSaveNotYetSavedThoughRareRegionsReadIt(const Allocator& allocator, const Operation* value, uint32_t firstReadInRareRegion, unsigned worth)
{
    enum : uint8_t { IsNotAskedYet, IsWorthIt, IsNotWorthIt };
    ArenaVector<uint8_t>& answers = allocator.m_whetherValuesThatRareRegionsReadAreWorthCalleeSaveNotYetSaved;
    if (answers.isEmpty())
        answers.fill(allocator.m_arena, allocator.m_order.mostOperations(), IsNotAskedYet);
    uint8_t& answer = answers[value->index()];
    if (answer != IsNotAskedYet)
        return answer == IsWorthIt;

    const Liveness& liveness = allocator.m_liveness;
    const ValueFacts& facts = liveness.of(value);
    Position lastRead = 0;
    for (uint32_t use = firstReadInRareRegion; use; use = liveness.use(use).next) {
        lastRead = liveness.use(use).position;
        if (!liveness.isUseInRareRegion(use) && ++worth > costOfCalleeSaveNotYetSaved) {
            answer = IsWorthIt;
            return true;
        }
    }
    answer = facts.lastUse != noPosition && facts.lastUse > lastRead ? IsWorthIt : IsNotWorthIt;
    return answer == IsWorthIt;
}

static ALWAYS_INLINE bool isWorthCalleeSaveNotYetSaved(const Allocator& allocator, const Operation* value, const Block* block)
{
    if (allocator.m_order.of(block).loop != noLoop)
        return true;

    const Liveness& liveness = allocator.m_liveness;
    const ValueFacts& facts = liveness.of(value);
    unsigned worth = 1;
    Position lastRead = 0;
    for (uint32_t use = facts.firstUse; use; use = liveness.use(use).next) {
        if (liveness.isUseInRareRegion(use)) [[unlikely]]
            return isWorthCalleeSaveNotYetSavedThoughRareRegionsReadIt(allocator, value, use, worth);
        lastRead = liveness.use(use).position;
        if (++worth > costOfCalleeSaveNotYetSaved)
            return value->opcode() != ArgumentReg || !slotServesEveryReadAsCheaplyAsRegister(allocator, value, block);
    }
    return facts.lastUse != noPosition && facts.lastUse > lastRead;
}

unsigned Allocator::numberOfReadsThatWordWouldReload(const Operation* value, Position end) const
{
    unsigned number = 0;
    for (uint32_t use = findReadFrom(value, m_position + 1); use && m_liveness.use(use).position < end; use = m_liveness.use(use).next) {
        if (!m_liveness.isUseInRareRegion(use))
            ++number;
    }
    return number;
}

ALWAYS_INLINE_INTO_OTHER_FILES RegisterMask Allocator::calleeSavesWorthTakingFor(const Operation* value, Bank bank, const Block* block) const
{
    RegisterMask calleeSaved = m_file.calleeSavedIn(bank);
    RegisterMask notYetSaved = calleeSaved & ~m_allocation.usedCalleeSaves();
    if (!notYetSaved || !m_proc.traits().chargesForCalleeSaves || m_proc.traits().savesRegistersOfCallers)
        return calleeSaved;
    if (isARM64() && bank == Bank::GP && (std::popcount(calleeSaved & ~notYetSaved) & 1))
        return calleeSaved;
    if (isWorthCalleeSaveNotYetSaved(*this, value, block))
        return calleeSaved;
    return calleeSaved & ~notYetSaved;
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
