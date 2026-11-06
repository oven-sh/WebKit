/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUseLists.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
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

UseLists::UseLists(const Function& function, const CFG& cfg, const ChangeLog* changes)
    : m_function(function)
    , m_cfg(&cfg)
    , m_changes(changes)
{
    makeAnew();
}

auto UseLists::makeRoomForUses(size_t size, size_t capacity) -> RoomForUses
{
    m_uses.reserveCapacity(capacity);
    m_uses.grow(size);
    m_placeOfNextUse.grow(size);
    if (m_keepsWhatUpdatesRead)
        m_usedValues.grow(size);
    return { m_uses.mutableSpan().data(), m_placeOfNextUse.mutableSpan().data(), m_usedValues.mutableSpan().data(), size };
}

void UseLists::makeAnew()
{
    if (m_keepsWhatUpdatesRead)
        makeAnewFromBlocks<true>();
    else
        makeAnewFromBlocks<false>();
}

template<bool keepsWhatUpdatesRead>
void UseLists::makeAnewFromBlocks()
{
    size_t numberOfValues = m_function.values.size();
    m_locations.fill(ValueLocation { }, numberOfValues);
    m_ranges.fill(UseRange { }, numberOfValues);

    m_uses.shrink(0);
    m_placeOfNextUse.shrink(0);
    m_usedValues.shrink(0);
    size_t expectedNumberOfUses = std::max<size_t>(m_numberOfUses + m_numberOfUses / 8, numberOfValues + numberOfValues / 4) + 8;
    RoomForUses room = makeRoomForUses(expectedNumberOfUses, 2 * expectedNumberOfUses);

    std::span<ValueLocation> locations = m_locations.mutableSpan();
    std::span<UseRange> ranges = m_ranges.mutableSpan();
    std::span<const Block> blocks = m_function.blocks.span();
    const Value* values = m_function.values.span().data();
    std::span<const BlockID> order = m_cfg->reversePostOrder();
    ValueID* listedValues = nullptr;
    std::span<PartOfBlock> parts;
    if constexpr (keepsWhatUpdatesRead) {
        m_firstPlacesOfRows.resize(numberOfValues);
        m_listedValues.shrink(0);
        m_listedValues.grow(numberOfValues);
        listedValues = m_listedValues.mutableSpan().data();
        m_partsOfBlocks.fill(PartOfBlock { }, blocks.size());
        parts = m_partsOfBlocks.mutableSpan();
        m_order.shrink(0);
        m_order.append(order);
    }
    size_t place = 0;
    uint32_t numberOfListedValues = 0;
    for (size_t orderIndex = order.size(); orderIndex--;) {
        uint32_t firstUse = static_cast<uint32_t>(place);
        uint32_t firstListedValue = numberOfListedValues;
        forEachUseInReverseOrder(blocks, values, order[orderIndex], [&](ValueID value, ValueLocation location) ALWAYS_INLINE_LAMBDA {
            ValueLocation& entry = locations[value.index()];
            RELEASE_ASSERT(entry.placement == ValuePlacement::Detached);
            entry = location;
            if constexpr (keepsWhatUpdatesRead)
                listedValues[numberOfListedValues++] = value;
        }, [&](ValueID used, const Use& use) ALWAYS_INLINE_LAMBDA {
            if (place == room.size) [[unlikely]]
                room = makeRoomForUses(2 * room.size, 4 * room.size);
            UseRange& range = ranges[used.index()];
            room.placeOfNextUse[place] = range.first;
            range = { static_cast<uint32_t>(place), (range.end | isLinked) + 1 };
            new (NotNull, room.uses + place) Use(use);
            if constexpr (keepsWhatUpdatesRead)
                room.usedValues[place] = used.index();
            ++place;
        });
        if constexpr (keepsWhatUpdatesRead)
            parts[order[orderIndex].index()] = { firstUse, static_cast<uint32_t>(place), firstListedValue, numberOfListedValues };
    }
    RELEASE_ASSERT(place < mayBeOutOfOrder);
    m_uses.shrink(place);
    m_placeOfNextUse.shrink(place);
    if constexpr (keepsWhatUpdatesRead) {
        m_usedValues.shrink(place);
        m_listedValues.shrink(numberOfListedValues);
    }
    m_numberOfUses = place;
    m_wasUpdated = false;
    didBecomeUpToDate();
}

std::span<const Use> UseLists::layOut(ValueID id) const
{
    if (m_wasUpdated)
        return layOutBehindUpdates(id);

    UseRange& range = m_ranges[id.index()];
    uint32_t size = range.end & ~marksOfLinks;
    if (size == 1) {
        range.end = range.first + 1;
        return { uses() + range.first, 1 };
    }

    size_t first = m_uses.size();
    RELEASE_ASSERT(first + size <= m_uses.capacity());
    m_uses.grow(first + size);
    StoredUse* row = m_uses.mutableSpan().data();
    uint32_t place = range.first;
    if (m_keepsWhatUpdatesRead)
        m_firstPlacesOfRows[id.index()] = place;
    for (uint32_t index = 0; index < size; ++index) {
        row[first + index] = row[place];
        place = m_placeOfNextUse[place];
    }
    range = { static_cast<uint32_t>(first), static_cast<uint32_t>(first + size) };
    return { uses() + first, size };
}

// An update leaves the uses that are gone among the links, and puts those that it has read in front.
std::span<const Use> UseLists::layOutBehindUpdates(ValueID id) const
{
    UseRange& range = m_ranges[id.index()];
    uint32_t size = range.end & ~marksOfLinks;
    const uint32_t* usedValues = m_usedValues.span().data();
    uint32_t* placeOfNextUse = m_placeOfNextUse.mutableSpan().data();
    uint32_t* link = &range.first;
    auto takeNextPlace = [&]() ALWAYS_INLINE_LAMBDA {
        while (usedValues[*link] == useIsGone)
            *link = placeOfNextUse[*link];
        uint32_t place = *link;
        link = placeOfNextUse + place;
        return place;
    };
    if (size == 1) {
        uint32_t place = takeNextPlace();
        range = { place, place + 1 };
        return { uses() + place, 1 };
    }

    size_t first = m_uses.size();
    RELEASE_ASSERT(first + size <= m_uses.capacity());
    m_uses.grow(first + size);
    StoredUse* row = m_uses.mutableSpan().data();
    if (!(range.end & mayBeOutOfOrder)) {
        for (uint32_t index = 0; index < size; ++index)
            row[first + index] = row[takeNextPlace()];
    } else {
        struct PlaceInOrder {
            UseOrder order;
            uint32_t place;
        };
        Vector<PlaceInOrder, 8> places;
        places.reserveInitialCapacity(size);
        for (uint32_t index = 0; index < size; ++index) {
            uint32_t place = takeNextPlace();
            places.append(PlaceInOrder { orderOf(uses()[place]), place });
        }
        if (!std::ranges::is_sorted(places, { }, &PlaceInOrder::order))
            AOT::sortInLine(places, { }, &PlaceInOrder::order);
        link = &range.first;
        for (uint32_t index = 0; index < size; ++index) {
            uint32_t place = places[index].place;
            row[first + index] = row[place];
            *link = place;
            link = placeOfNextUse + place;
        }
    }
    m_firstPlacesOfRows[id.index()] = range.first;
    range = { static_cast<uint32_t>(first), static_cast<uint32_t>(first + size) };
    return { uses() + first, size };
}

void UseLists::didBecomeUpToDate()
{
    m_isUpToDate = true;
    m_mustBeMadeAnew = false;
    m_namedBlocks.shrink(0);
    if (m_changes)
        m_firstUnreadChange = m_changes->endOfShownEntries();
}

void UseLists::didChangeOperandsOf(ValueID user)
{
    m_isUpToDate = false;
    if (user.index() < m_locations.size() && m_locations[user.index()].placement == ValuePlacement::InBlock)
        m_namedBlocks.append(m_locations[user.index()].block);
}

ALWAYS_INLINE void UseLists::linkAgain(UseRange& range, ValueID id)
{
    if (range.end & isLinked)
        return;
    uint32_t size = range.end - range.first;
    if (!size)
        return;
    if (size > 1)
        range.first = m_firstPlacesOfRows[id.index()];
    range.end = size | isLinked;
}

auto UseLists::findReasonToMakeAnew() const -> ReasonToMakeAnew
{
    if (m_mustBeMadeAnew || !m_changes)
        return ReasonToMakeAnew::NoLog;
    if (!m_keepsWhatUpdatesRead)
        return ReasonToMakeAnew::NothingKeptForUpdates;
    if (m_changes->hasForgottenSince(m_firstUnreadChange) || m_function.values.size() < m_locations.size() || m_function.blocks.size() < m_partsOfBlocks.size())
        return ReasonToMakeAnew::ForgottenChanges;
    AOT_THRESHOLD(size_t, mostPlacesForEachUse, 4);
    AOT_THRESHOLD(size_t, placesThatAreAlwaysKept, 256);
    if (m_uses.size() > mostPlacesForEachUse * m_numberOfUses + placesThatAreAlwaysKept || m_listedValues.size() > mostPlacesForEachUse * m_locations.size() + placesThatAreAlwaysKept)
        return ReasonToMakeAnew::MuchIsGone;
    return ReasonToMakeAnew::None;
}

// It stops where the changed blocks hold more values than that.
auto UseLists::findChangedBlocks(bool isGraphThatWasRead, size_t mostValuesRead) -> ChangedBlocks
{
    size_t numberOfBlocks = m_function.blocks.size();
    if (m_keepsWhatUpdatesRead)
        m_partsOfBlocks.grow(numberOfBlocks);
    m_isChangedBlock.ensureSize(numberOfBlocks);
    ChangedBlocks result;
    auto note = [&](BlockID block) {
        if (m_isChangedBlock.quickSet(block.index()))
            return;
        m_changedBlocks.append(block);
        if (m_keepsWhatUpdatesRead) {
            const PartOfBlock& part = m_partsOfBlocks[block.index()];
            result.numberOfValuesRead += part.endOfListedValues - part.firstListedValue;
        }
        if (m_cfg->isReachable(block))
            result.numberOfValuesRead += m_function.at(block).parameters.size() + m_function.at(block).values.size();
    };

    for (BlockID block : m_namedBlocks) {
        note(block);
        if (result.numberOfValuesRead > mostValuesRead)
            return result;
    }
    for (const BlockChange& change : m_changes->blocksSinceWithoutShowing(m_firstUnreadChange)) {
        if (change.edits == BlockEdit::Incoming)
            continue;
        note(change.block);
        if (result.numberOfValuesRead > mostValuesRead)
            return result;
    }
    for (const ValueChange& change : m_changes->valuesSinceWithoutShowing(m_firstUnreadChange)) {
        if (change.value.index() >= m_locations.size() || !change.edits.containsAny({ ValueEdit::Added, ValueEdit::Definition, ValueEdit::OperandsOfSameTypes }))
            continue;
        const ValueLocation& location = m_locations[change.value.index()];
        if (location.placement != ValuePlacement::InBlock)
            continue;
        note(location.block);
        if (result.numberOfValuesRead > mostValuesRead)
            return result;
    }

    if (isGraphThatWasRead)
        return result;

    // Blocks that are reached no more, or only now, are changed blocks. The others keep their order unless one has passed another.
    std::span<const BlockID> formerOrder = m_order.span();
    std::span<const BlockID> order = m_cfg->reversePostOrder();
    size_t formerIndex = 0;
    size_t index = 0;
    while (formerIndex < formerOrder.size() || index < order.size()) {
        if (formerIndex < formerOrder.size() && index < order.size() && formerOrder[formerIndex] == order[index]) {
            ++formerIndex;
            ++index;
        } else if (formerIndex < formerOrder.size() && !m_cfg->isReachable(formerOrder[formerIndex]))
            note(formerOrder[formerIndex++]);
        else if (index < order.size() && !m_partsOfBlocks[order[index].index()].isListed())
            note(order[index++]);
        else {
            result.keepTheirOrder = false;
            break;
        }
    }
    if (!result.keepTheirOrder) {
        for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
            if (m_partsOfBlocks[blockIndex].isListed() != m_cfg->isReachable(BlockID(blockIndex)))
                note(BlockID(blockIndex));
        }
    }
    return result;
}

void UseLists::forgetChangedBlocks()
{
    for (BlockID block : m_changedBlocks)
        m_isChangedBlock.quickClear(block.index());
    m_changedBlocks.shrink(0);
}

// What updates read costs a tenth of a build, and an update pays where few blocks have changed. A function goes on as it began.
bool UseLists::hasChangedLittle()
{
    AOT_THRESHOLD(size_t, fewestValuesOfListsThatAreUpdated, 64);
    AOT_THRESHOLD(size_t, valuesForEachValueOfChangedBlock, 4);
    size_t numberOfValues = m_function.values.size();
    if (numberOfValues < fewestValuesOfListsThatAreUpdated || numberOfValues < m_locations.size() || m_changes->hasForgottenSince(m_firstUnreadChange))
        return false;
    size_t mostValuesRead = numberOfValues / valuesForEachValueOfChangedBlock;
    bool hasChangedLittle = findChangedBlocks(true, mostValuesRead).numberOfValuesRead <= mostValuesRead;
    forgetChangedBlocks();
    return hasChangedLittle;
}

// False: a block that is not among the changed ones lists a value of theirs.
bool UseLists::readChangedBlocks()
{
    size_t numberOfValues = m_function.values.size();
    m_locations.grow(numberOfValues);
    m_ranges.grow(numberOfValues);
    m_firstPlacesOfRows.grow(numberOfValues);
    std::span<ValueLocation> locations = m_locations.mutableSpan();
    std::span<UseRange> ranges = m_ranges.mutableSpan();

    for (BlockID block : m_changedBlocks) {
        PartOfBlock& part = m_partsOfBlocks[block.index()];
        if (!part.isListed())
            continue;
        for (uint32_t place = part.firstUse; place < part.endOfUses; ++place) {
            ValueID used(std::exchange(m_usedValues[place], useIsGone));
            UseRange& range = ranges[used.index()];
            linkAgain(range, used);
            if (!(--range.end & ~marksOfLinks))
                range = { };
        }
        m_numberOfUses -= part.endOfUses - part.firstUse;
        for (uint32_t index = part.firstListedValue; index < part.endOfListedValues; ++index)
            locations[m_listedValues[index].index()] = { };
        part = { };
    }

    size_t place = m_uses.size();
    size_t firstNewPlace = place;
    RoomForUses room = makeRoomForUses(m_uses.capacity(), m_uses.capacity());
    std::span<const Block> blocks = m_function.blocks.span();
    const Value* values = m_function.values.span().data();
    bool isListedTwice = false;
    for (BlockID block : m_changedBlocks) {
        if (!m_cfg->isReachable(block))
            continue;
        uint32_t firstUse = static_cast<uint32_t>(place);
        uint32_t firstListedValue = m_listedValues.size();
        forEachUseInReverseOrder(blocks, values, block, [&](ValueID value, ValueLocation location) ALWAYS_INLINE_LAMBDA {
            ValueLocation& entry = locations[value.index()];
            isListedTwice |= entry.placement != ValuePlacement::Detached;
            entry = location;
            m_listedValues.append(value);
        }, [&](ValueID used, const Use& use) ALWAYS_INLINE_LAMBDA {
            if (place == room.size) [[unlikely]]
                room = makeRoomForUses(room.size + room.size / 2 + 8, room.size + room.size / 2 + 8);
            UseRange& range = ranges[used.index()];
            linkAgain(range, used);
            room.placeOfNextUse[place] = range.first;
            range = { static_cast<uint32_t>(place), (range.end | marksOfLinks) + 1 };
            new (NotNull, room.uses + place) Use(use);
            room.usedValues[place] = used.index();
            ++place;
        });
        m_partsOfBlocks[block.index()] = { firstUse, static_cast<uint32_t>(place), firstListedValue, static_cast<uint32_t>(m_listedValues.size()) };
    }
    RELEASE_ASSERT(place < mayBeOutOfOrder);
    m_uses.shrink(place);
    m_placeOfNextUse.shrink(place);
    m_usedValues.shrink(place);
    m_numberOfUses += place - firstNewPlace;
    if (m_uses.capacity() < place + m_numberOfUses)
        m_uses.reserveCapacity(place + m_numberOfUses + m_numberOfUses / 2);
    return !isListedTwice;
}

auto UseLists::bringUpToDate(const CFG& cfg, bool isGraphThatWasRead) -> Update
{
    RELEASE_ASSERT(!m_isUpToDate);
    m_cfg = &cfg;
    size_t numberOfValues = m_function.values.size();
    auto makeAnewFor = [&](ReasonToMakeAnew reason) -> Update {
        makeAnew();
        return { reason, numberOfValues };
    };
    if (ReasonToMakeAnew reason = findReasonToMakeAnew(); reason != ReasonToMakeAnew::None) {
        if (reason == ReasonToMakeAnew::NothingKeptForUpdates)
            m_keepsWhatUpdatesRead = hasChangedLittle();
        return makeAnewFor(reason);
    }

    ChangedBlocks changed = findChangedBlocks(isGraphThatWasRead, numberOfValues);
    bool isRead = changed.numberOfValuesRead <= numberOfValues && readChangedBlocks();
    bool hasChangedBlocks = !m_changedBlocks.isEmpty();
    forgetChangedBlocks();
    if (!isRead)
        return makeAnewFor(changed.numberOfValuesRead > numberOfValues ? ReasonToMakeAnew::MuchHasChanged : ReasonToMakeAnew::UnknownChange);

    if (!changed.keepTheirOrder) {
        uint32_t index = 0;
        for (UseRange& range : m_ranges) {
            linkAgain(range, ValueID(index++));
            if (range.end & isLinked)
                range.end |= mayBeOutOfOrder;
        }
    }
    if (!isGraphThatWasRead) {
        m_order.shrink(0);
        m_order.append(cfg.reversePostOrder());
    }
    m_wasUpdated |= hasChangedBlocks || !changed.keepTheirOrder;
    didBecomeUpToDate();
    return { ReasonToMakeAnew::None, changed.numberOfValuesRead };
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
