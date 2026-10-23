/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUseLists.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include <algorithm>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

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
    RoomForUses made = makeRoomForUses(std::max<size_t>(m_numberOfUses + m_numberOfUses / 8, numberOfValues + numberOfValues / 4) + 8);
    StoredUse* uses = made.uses;
    uint32_t* placeOfNextUse = made.placeOfNextUse;
    size_t room = made.size;

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
            if (place == room) [[unlikely]] {
                RoomForUses more = makeRoomForUses(2 * room);
                uses = more.uses;
                placeOfNextUse = more.placeOfNextUse;
                room = more.size;
            }
            UseRange& range = ranges[used.index()];
            placeOfNextUse[place] = range.first;
            range = { static_cast<uint32_t>(place), (range.end | isLinked) + 1 };
            new (NotNull, uses + place) Use(use);
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
    RELEASE_ASSERT(!m_hasSilentChanges);
    m_hasChangedBlocks = false;
    m_isUpToDate = true;
    m_graphMayHaveChanged = false;
    m_mustBeMadeAnew = false;
    m_takesSilentChangesAtOnce = false;
}

void UseLists::fallBehind()
{
    m_isUpToDate = false;
    if (!m_hasSilentChanges) [[likely]]
        return;
    m_hasChangedBlocks = true;
    m_hasSilentChanges = false;
}

void UseLists::didChangeBlock(BlockID)
{
    fallBehind();
    m_hasChangedBlocks = true;
}

void UseLists::didChangeBlockSilently(BlockID block)
{
    if (!m_isUpToDate || m_takesSilentChangesAtOnce) {
        didChangeBlock(block);
        return;
    }
    m_hasSilentChanges = true;
}

void UseLists::endSilence()
{
    if (m_hasSilentChanges)
        fallBehind();
}

void UseLists::didOutliveDrop()
{
    endSilence();
    m_takesSilentChangesAtOnce = true;
}

// A value that the lists find in no block has no use listed under its name as a user. Whoever has put it into a block since has named that block.
void UseLists::didChangeOperandsOf(ValueID user)
{
    fallBehind();
    if (user.index() >= m_locations.size())
        return;
    const ValueLocation& location = m_locations[user.index()];
    if (location.placement == ValuePlacement::InBlock)
        didChangeBlock(location.block);
}

// A use that has come about since the lists were brought up to date stands in a block that has been named.
void UseLists::didReplaceUses(ValueID from, ValueID)
{
    fallBehind();
    if (m_mustBeMadeAnew || from.index() >= m_ranges.size())
        return;
    if (useCount(from))
        m_hasChangedBlocks = true;
}

void UseLists::didChangeControlFlow()
{
    fallBehind();
    m_graphMayHaveChanged = true;
}

void UseLists::didChangeEverything()
{
    fallBehind();
    m_mustBeMadeAnew = true;
}

void UseLists::didRenumber(std::span<const BlockID> blocks, std::span<const ValueID> values)
{
    if (!m_isUpToDate || m_hasSilentChanges || values.size() != m_locations.size()) {
        didChangeEverything();
        return;
    }

    auto renumbered = [&](BlockID block) {
        if (blocks.empty())
            return block;
        BlockID moved = blocks[block.index()];
        RELEASE_ASSERT(moved);
        return moved;
    };

    size_t numberOfValues = m_function.values.size();
    Vector<UseRange> ranges(numberOfValues);
    Vector<ValueLocation> locations(numberOfValues);
    for (size_t index = 0; index < values.size(); ++index) {
        UseRange range = m_ranges[index];
        ValueID value = values[index];
        if (!value) {
            RELEASE_ASSERT(range.first == range.end);
            continue;
        }
        ValueLocation location = m_locations[index];
        RELEASE_ASSERT(location.placement != ValuePlacement::Detached);
        location.block = renumbered(location.block);
        locations[value.index()] = location;
        ranges[value.index()] = range;
    }
    m_ranges = WTF::move(ranges);
    m_locations = WTF::move(locations);

    for (Use& use : std::span<Use> { reinterpret_cast<Use*>(m_uses.mutableSpan().data()), m_uses.size() }) {
        use.block = renumbered(use.block);
        if (!use.user)
            continue;
        use.user = values[use.user.index()];
        RELEASE_ASSERT(use.user);
    }
    // The graph that orderOf() reads has the former numbers.
    m_isUpToDate = false;
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
