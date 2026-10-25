/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUseLists.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

ValueID usedValueOf(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return function.at(use.user).operand(use.operandIndex);
    case Use::Kind::TerminatorOperand:
        return function.at(use.block).terminator.operand(use.operandIndex);
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        return targetOf(function, use.edge()).arguments[use.operandIndex];
    case Use::Kind::BlockSubject:
        return function.at(use.block).subject;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<ValueID, 2> findNarrowedParameters(const Function& function, const UseLists& useLists, const Use& useByTest)
{
    Vector<ValueID, 2> parameters;
    if (useByTest.kind != Use::Kind::Operand || provesOf(function.at(useByTest.user).opcode) == Proof::None)
        return parameters;
    for (const Use& useOfTest : useLists.usesOf(useByTest.user)) {
        if (useOfTest.kind != Use::Kind::TerminatorOperand)
            continue;
        forEachEdgeResult(function, useOfTest.block, [&](const EdgeResult& result, ValueID parameter) {
            if (result.kind == EdgeResultKind::NarrowedOperand && result.test == useByTest.user && result.operandIndex == useByTest.operandIndex)
                parameters.append(parameter);
        });
    }
    return parameters;
}

static uint32_t rankOf(Use::Kind kind)
{
    switch (kind) {
    case Use::Kind::BlockSubject:
        return 0;
    case Use::Kind::HandlerArgument:
        return 1;
    case Use::Kind::Operand:
        return 2;
    case Use::Kind::TerminatorOperand:
        return 3;
    case Use::Kind::EdgeArgument:
        return 4;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE void forEachUseInReverseOrder(std::span<const Block> blocks, const Value* values, BlockID blockID, NOESCAPE const Invocable<void(ValueID, ValueLocation)> auto& placeValue, NOESCAPE const Invocable<void(ValueID, const Use&)> auto& addUse)
{
    const Block& block = blocks[blockID.index()];
    auto addUses = [&](std::span<const ValueID> used, Use::Kind kind, ValueID user, uint32_t targetIndex) ALWAYS_INLINE_LAMBDA {
        for (uint32_t index = static_cast<uint32_t>(used.size()); index--;) {
            if (used[index])
                addUse(used[index], Use { kind, blockID, user, index, targetIndex });
        }
    };

    std::span<const Target> targets = block.terminator.targets.span();
    for (uint32_t targetIndex = static_cast<uint32_t>(targets.size()); targetIndex--;)
        addUses(targets[targetIndex].arguments.span(), Use::Kind::EdgeArgument, { }, targetIndex);
    addUses(block.terminator.operands(), Use::Kind::TerminatorOperand, { }, 0);
    std::span<const ValueID> listed = block.values.span();
    for (uint32_t index = static_cast<uint32_t>(listed.size()); index--;) {
        ValueID user = listed[index];
        placeValue(user, ValueLocation { ValuePlacement::InBlock, blockID, index });
        addUses(values[user.index()].operands(), Use::Kind::Operand, user, 0);
    }
    if (block.handler)
        addUses(block.handler->arguments.span(), Use::Kind::HandlerArgument, { }, 0);
    if (block.subject)
        addUse(block.subject, Use { Use::Kind::BlockSubject, blockID, { }, 0, 0 });
    uint32_t index = 0;
    for (ValueID parameter : block.parameters)
        placeValue(parameter, ValueLocation { ValuePlacement::Parameter, blockID, index++ });
}

UseLists::UseLists(const Function& function, const CFG& cfg)
    : m_function(function)
    , m_cfg(&cfg)
{
    makeAnew();
}

auto UseLists::makeRoomForUses(size_t size) -> RoomForUses
{
    m_uses.reserveCapacity(2 * size);
    m_uses.grow(size);
    m_placeOfNextUse.grow(size);
    return { m_uses.mutableSpan().data(), m_placeOfNextUse.mutableSpan().data(), size };
}

void UseLists::makeAnew()
{
    size_t numberOfValues = m_function.values.size();
    m_locations.fill(ValueLocation { }, numberOfValues);
    m_ranges.fill(UseRange { }, numberOfValues);

    m_uses.shrink(0);
    m_placeOfNextUse.shrink(0);
    RoomForUses room = makeRoomForUses(std::max<size_t>(m_numberOfUses + m_numberOfUses / 8, numberOfValues + numberOfValues / 4) + 8);

    std::span<ValueLocation> locations = m_locations.mutableSpan();
    std::span<UseRange> ranges = m_ranges.mutableSpan();
    std::span<const Block> blocks = m_function.blocks.span();
    const Value* values = m_function.values.span().data();
    std::span<const BlockID> order = m_cfg->reversePostOrder();
    size_t place = 0;
    for (size_t orderIndex = order.size(); orderIndex--;) {
        forEachUseInReverseOrder(blocks, values, order[orderIndex], [&](ValueID value, ValueLocation location) ALWAYS_INLINE_LAMBDA {
            ValueLocation& entry = locations[value.index()];
            RELEASE_ASSERT(entry.placement == ValuePlacement::Detached);
            entry = location;
        }, [&](ValueID used, const Use& use) ALWAYS_INLINE_LAMBDA {
            if (place == room.size) [[unlikely]]
                room = makeRoomForUses(2 * room.size);
            UseRange& range = ranges[used.index()];
            room.placeOfNextUse[place] = range.first;
            range = { static_cast<uint32_t>(place), (range.end | isLinked) + 1 };
            new (NotNull, room.uses + place) Use(use);
            ++place;
        });
    }
    RELEASE_ASSERT(place < isLinked);
    m_uses.shrink(place);
    m_placeOfNextUse.shrink(place);
    m_numberOfUses = place;
    didBecomeUpToDate();
}

std::span<const Use> UseLists::layOut(ValueID id) const
{
    UseRange& range = m_ranges[id.index()];
    uint32_t size = range.end & ~isLinked;
    if (size == 1) {
        range.end = range.first + 1;
        return { uses() + range.first, 1 };
    }

    size_t first = m_uses.size();
    RELEASE_ASSERT(first + size <= m_uses.capacity());
    m_uses.grow(first + size);
    StoredUse* row = m_uses.mutableSpan().data();
    uint32_t place = range.first;
    for (uint32_t index = 0; index < size; ++index) {
        row[first + index] = row[place];
        place = m_placeOfNextUse[place];
    }
    range = { static_cast<uint32_t>(first), static_cast<uint32_t>(first + size) };
    return { uses() + first, size };
}

void UseLists::didBecomeUpToDate()
{
    m_hasChangedBlocks = false;
    m_isUpToDate = true;
    m_graphMayHaveChanged = false;
    m_mustBeMadeAnew = false;
}

void UseLists::didChangeOperandsOf(ValueID user)
{
    m_isUpToDate = false;
    if (user.index() < m_locations.size() && m_locations[user.index()].placement == ValuePlacement::InBlock)
        m_hasChangedBlocks = true;
}

void UseLists::didReplaceUses(ValueID from, ValueID)
{
    m_isUpToDate = false;
    if (!m_mustBeMadeAnew && from.index() < m_ranges.size() && useCount(from))
        m_hasChangedBlocks = true;
}

auto UseLists::bringUpToDate(const CFG& cfg) -> Update
{
    RELEASE_ASSERT(!m_isUpToDate);
    m_cfg = &cfg;
    size_t numberOfValues = m_function.values.size();
    if (m_mustBeMadeAnew || m_hasChangedBlocks || m_graphMayHaveChanged || numberOfValues < m_locations.size()) {
        makeAnew();
        return { true, numberOfValues };
    }
    m_locations.grow(numberOfValues);
    m_ranges.grow(numberOfValues);
    didBecomeUpToDate();
    return { };
}

ProgramPoint UseLists::pointOf(const Use& use) const
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return { use.block, locationOf(use.user).index };
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
        return pointBeforeTerminatorOf(m_function, use.block);
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return ProgramPoint::atHeadOf(use.block);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ProgramPoint UseLists::pointOf(ValueID valueInBlock) const
{
    ValueLocation location = locationOf(valueInBlock);
    RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
    return { location.block, location.index };
}

ProgramPoint UseLists::pointAfter(ValueID id) const
{
    ValueLocation location = locationOf(id);
    switch (location.placement) {
    case ValuePlacement::Parameter:
        return { location.block, indexAfterCaughtValues(m_function, m_function.at(location.block)) };
    case ValuePlacement::InBlock:
        return { location.block, std::max(location.index + 1, indexAfterCaughtValues(m_function, m_function.at(location.block))) };
    case ValuePlacement::Detached:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

UseOrder UseLists::orderOf(const Use& use) const
{
    return { m_cfg->reversePostOrderIndex(use.block), pointOf(use).index, rankOf(use.kind), use.targetIndex, use.operandIndex };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
