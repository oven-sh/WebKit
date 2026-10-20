/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <initializer_list>
#include <ranges>
#include <tuple>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/UnionFind.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr unsigned numberOfWidths = 4;
constexpr uint32_t widestSlot = 1u << (numberOfWidths - 1);
constexpr unsigned overflowAreaLogSize = 3;
static_assert(widthOf(SlotRepresentation::JSValue) == widestSlot);
static_assert(sizeOfOverflowArea == 1u << overflowAreaLogSize && sizeOfOverflowArea <= widestSlot);
static_assert(!(offsetOfFirstField % widestSlot) && !(offsetOfFirstExtensionSlot % widestSlot) && extensionGranuleSize == widestSlot);
static_assert(defaultLimitValue(LimitID::MaxExtensionGranules, hostArchitecture()) == maxExtensionGranules);

using WidthCounts = std::array<uint32_t, numberOfWidths>;

enum class RowPart : uint8_t {
    Slot,
    DoubleArea,
};

struct MemberRow {
    LayoutID layout;
    SlotRow row { 0 };
};

enum class StepRank : uint8_t {
    FoundNoRoomInFirstRound,
    JoinsFamilies,
    OneJoinedFamily,
    OneFamily,
};

// The rows of one joined set with one storage and representation whose fields are declared at this depth or above it.
struct PlacementStep {
    uint32_t depth { 0 };
    StepRank rank { StepRank::JoinsFamilies };
    bool placesHiddenWords { false };
    bool placesRowByItself { false };
    SiteWeight weight { 0 };
    size_t firstSharedRow { 0 };
    uint32_t numberOfSharedRows { 0 };
};

struct JoinedGroup {
    uint32_t indexOfSet { 0 };
    size_t firstSharedRow { 0 };
    uint32_t numberOfSharedRows { 0 };
};

struct LocationRequest {
    PropertyKeyID key;
    LayoutSetID receivers;
    SiteWeight weight { 0 };
    size_t firstMemberRow { 0 };
    uint32_t numberOfMemberRows { 0 };
};

struct OffsetUse {
    uint32_t offset { 0 };
    uint32_t numberOfRows { 0 };
};

struct RowSet : WTF::UnionFind<RowSet> { };

auto rowsOf(const SlotRows& rows, LayoutID id)
{
    SlotRow first = rows.firstRowOf(id);
    return std::views::iota(first, first + rows.numberOfRowsOf(id));
}

bool hasPart(const PlacedSlot& slot, RowPart part)
{
    switch (part) {
    case RowPart::Slot:
        return slot.hasStorage();
    case RowPart::DoubleArea:
        return slot.storage == SlotStorage::Inline && slot.representation == SlotRepresentation::Int32OrDouble;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint64_t roomIn(uint64_t occupied, unsigned logSize)
{
    static constexpr uint64_t aligned[] = { UINT64_MAX, 0x5555555555555555ull, 0x1111111111111111ull, 0x0101010101010101ull };
    uint64_t freeBits = ~occupied;
    for (unsigned i = 0; i < logSize; ++i)
        freeBits &= freeBits >> (1u << i);
    return freeBits & aligned[logSize];
}

uint64_t occupiedBitsBelow(const OccupiedBytes& occupied, uint32_t word, uint32_t limit)
{
    uint64_t bits = word < occupied.size() ? occupied[word] : 0;
    uint32_t bytesBelowLimit = limit - word * 64;
    return bytesBelowLimit < 64 ? bits | UINT64_MAX << bytesBelowLimit : bits;
}

std::optional<uint32_t> findFreeOffset(const OccupiedBytes& occupied, unsigned logSize, uint32_t from, uint32_t limit)
{
    for (uint32_t word = from / 64; word * 64 < limit; ++word) {
        uint64_t freeBits = roomIn(occupiedBitsBelow(occupied, word, limit), logSize);
        if (word == from / 64)
            freeBits &= UINT64_MAX << (from % 64);
        if (freeBits)
            return word * 64 + WTF::ctz(freeBits);
    }
    return std::nullopt;
}

void occupy(OccupiedBytes& words, uint32_t offset, uint32_t size)
{
    uint32_t word = offset / 64;
    while (words.size() <= word)
        words.append(0);
    words[word] |= bytesAt(offset, size);
}

// Taking any free aligned block takes exactly two free blocks of half its width, so counts decide what packing widest first would find.
bool remainingRowsFit(const WidthCounts& room, const WidthCounts& unplaced)
{
    uint64_t blocksTaken = 0;
    for (unsigned logSize = numberOfWidths; logSize--;) {
        blocksTaken = 2 * blocksTaken + unplaced[logSize];
        if (blocksTaken > room[logSize])
            return false;
    }
    return true;
}

struct Area {
    void initialize(uint32_t headerSize, uint32_t sizeLimit);
    void growTo(uint32_t sizeLimit);
    bool isFree(uint32_t offset, uint32_t size) const;
    WidthCounts roomAfterPlacing(uint32_t offset, unsigned logSize) const;
    bool canPlace(uint32_t offset, unsigned logSize) const;
    void place(uint32_t offset, unsigned logSize);
    bool canReserve(uint32_t offset, unsigned logSize) const;
    void reserve(uint32_t offset, unsigned logSize);

    OccupiedBytes occupied;
    uint32_t limit { 0 };
    WidthCounts unplaced { };
    WidthCounts room { };
};

void Area::initialize(uint32_t headerSize, uint32_t sizeLimit)
{
    RELEASE_ASSERT(!(sizeLimit % widestSlot));
    occupied.append(bytesAt(0, headerSize));
    limit = sizeLimit;
    for (uint32_t word = 0; word * 64 < limit; ++word) {
        uint64_t bits = occupiedBitsBelow(occupied, word, limit);
        for (unsigned logSize = 0; logSize < numberOfWidths; ++logSize)
            room[logSize] += std::popcount(roomIn(bits, logSize));
    }
}

void Area::growTo(uint32_t sizeLimit)
{
    RELEASE_ASSERT(sizeLimit > limit && !(sizeLimit % widestSlot));
    for (unsigned logSize = 0; logSize < numberOfWidths; ++logSize)
        room[logSize] += (sizeLimit - limit) >> logSize;
    limit = sizeLimit;
}

bool Area::isFree(uint32_t offset, uint32_t size) const
{
    uint32_t word = offset / 64;
    return word >= occupied.size() || !(occupied[word] & bytesAt(offset, size));
}

WidthCounts Area::roomAfterPlacing(uint32_t offset, unsigned logSize) const
{
    WidthCounts result = room;
    for (unsigned other = 0; other < numberOfWidths; ++other) {
        if (other <= logSize)
            result[other] -= 1u << (logSize - other);
        else if (uint32_t size = 1u << other; isFree(offset & ~(size - 1), size))
            --result[other];
    }
    return result;
}

bool Area::canPlace(uint32_t offset, unsigned logSize) const
{
    ASSERT(unplaced[logSize]);
    uint32_t size = 1u << logSize;
    if (offset % size || offset + size > limit || !isFree(offset, size))
        return false;
    WidthCounts remaining = unplaced;
    --remaining[logSize];
    return remainingRowsFit(roomAfterPlacing(offset, logSize), remaining);
}

void Area::place(uint32_t offset, unsigned logSize)
{
    ASSERT(canPlace(offset, logSize));
    room = roomAfterPlacing(offset, logSize);
    --unplaced[logSize];
    occupy(occupied, offset, 1u << logSize);
}

bool Area::canReserve(uint32_t offset, unsigned logSize) const
{
    uint32_t size = 1u << logSize;
    if (offset % size || offset + size > limit || !isFree(offset, size))
        return false;
    return remainingRowsFit(roomAfterPlacing(offset, logSize), unplaced);
}

void Area::reserve(uint32_t offset, unsigned logSize)
{
    ASSERT(canReserve(offset, logSize));
    room = roomAfterPlacing(offset, logSize);
    occupy(occupied, offset, 1u << logSize);
}

uint32_t extensionEndOf(const SlotRows& rows, const Vector<PlacedSlot>& slots, LayoutID id)
{
    uint32_t end = 0;
    for (SlotRow row : rowsOf(rows, id)) {
        const PlacedSlot& slot = slots[row];
        if (slot.isOutOfLine())
            end = std::max(end, slot.end());
        if (slot.doubleOffsetInExtension)
            end = std::max(end, *slot.doubleOffsetInExtension + sizeOfOverflowArea);
    }
    return end;
}

Vector<OffsetUse, 8> countOffsetUses(Vector<uint32_t, 8>&& offsets)
{
    std::ranges::sort(offsets);
    Vector<OffsetUse, 8> uses;
    for (uint32_t offset : offsets) {
        if (uses.isEmpty() || uses.last().offset != offset)
            uses.append(OffsetUse { offset, 0 });
        ++uses.last().numberOfRows;
    }
    std::ranges::sort(uses, [](const OffsetUse& a, const OffsetUse& b) {
        return a.numberOfRows != b.numberOfRows ? a.numberOfRows > b.numberOfRows : a.offset < b.offset;
    });
    return uses;
}

void forEachAdjacentGroup(std::span<MemberRow> rows, const Invocable<bool(MemberRow, MemberRow)> auto& belongTogether, const Invocable<void(std::span<MemberRow>)> auto& functor)
{
    for (size_t first = 0; first < rows.size();) {
        size_t end = first + 1;
        while (end < rows.size() && belongTogether(rows[first], rows[end]))
            ++end;
        if (end - first >= 2)
            functor(rows.subspan(first, end - first));
        first = end;
    }
}

class ObjectPacker {
    WTF_MAKE_NONCOPYABLE(ObjectPacker);
public:
    ObjectPacker(const SlotPlanContext&, const SlotUses&, const JoinedRows&, std::span<const SlotClaim>);

    PackedLayouts run();

private:
    const ParallelLoops& loops() const { return m_context.build.parallelLoops(); }
    uint32_t cellSizeFor(uint32_t bytes) const { return static_cast<uint32_t>(m_context.sizeClasses.cellSizeFor(bytes)); }
    const PlacedSlot& slotOf(MemberRow member) const { return m_packed.slots[member.row]; }
    const PlacedSlot& extensionWordOf(MemberRow member) const { return m_packed.slots[m_context.rows.extensionWordOf(member.layout)]; }
    std::span<MemberRow> memberRowsOf(const LocationRequest& request) { return m_memberRows.mutableSpan().subspan(request.firstMemberRow, request.numberOfMemberRows); }
    std::span<const MemberRow> memberRowsOf(const LocationRequest& request) const { return m_memberRows.span().subspan(request.firstMemberRow, request.numberOfMemberRows); }

    bool isInExtension(MemberRow member, RowPart part) const { return part == RowPart::DoubleArea || slotOf(member).isOutOfLine(); }
    const Vector<Area>& areasOf(MemberRow member, RowPart part) const { return isInExtension(member, part) ? m_extension : m_inline; }
    const Area& areaOf(MemberRow member, RowPart part) const { return areasOf(member, part)[member.layout.index()]; }
    Area& areaOf(MemberRow member, RowPart part) { return (isInExtension(member, part) ? m_extension : m_inline)[member.layout.index()]; }
    unsigned logSizeOf(MemberRow, RowPart) const;
    std::optional<uint32_t> offsetOf(MemberRow, RowPart) const;

    void forEachCompiledObject(const Invocable<void(LayoutID)> auto& functor) const
    {
        forEachIndexInParallel(loops(), m_context.program.layouts.size(), [&](unsigned index) {
            if (LayoutID id(index); m_context.isCompiledObject(id))
                functor(id);
        });
    }

    void forEachSharedOffset(std::span<MemberRow> rows, RowPart part, const Invocable<void(std::span<MemberRow>)> auto& functor) const
    {
        auto unplaced = std::ranges::partition(rows, [&](MemberRow member) {
            return offsetOf(member, part).has_value();
        });
        std::span<MemberRow> placed = rows.first(unplaced.begin() - rows.begin());
        auto orderOf = [&](MemberRow member) {
            return std::pair(*offsetOf(member, part), member.row);
        };
        if (!std::ranges::is_sorted(placed, { }, orderOf))
            std::ranges::sort(placed, { }, orderOf);
        forEachAdjacentGroup(placed, [&](MemberRow a, MemberRow b) {
            return offsetOf(a, part) == offsetOf(b, part);
        }, functor);
    }

    bool isInherited(SlotRow row) const { return m_context.inheritedRows.declaringRowOf(row) != row; }
    bool areInOneFamily(MemberRow a, MemberRow b) const { return m_context.inheritedRows.declaringRowOf(a.row) == m_context.inheritedRows.declaringRowOf(b.row); }
    uint32_t depthOfDeclaration(MemberRow member) const { return m_context.inheritedRows.depthOf(m_context.rows.layoutOf(m_context.inheritedRows.declaringRowOf(member.row))); }
    void chooseStorage(LayoutID);
    uint32_t countGranulesOfInheritedRows(LayoutID) const;
    void chooseOutOfLineRows(LayoutID, uint32_t& granulesWanted);
    void widenSlotsWithoutOverflowArea(LayoutID, uint32_t& granulesWanted);
    void claimExtensionWord(LayoutID);
    void shareExtensionWords();
    void initializeAreas(LayoutID);
    void placeExtensionWords();
    void appendSteps(std::span<const MemberRow>, StepRank, SiteWeight, Vector<PlacementStep>&) const;
    void placeAtFirstFreeOffset(MemberRow);
    Vector<PlacementStep> collectSteps(const BitVector& setsToPlaceFirst);
    void placeSharedRows(const BitVector& setsToPlaceFirst);
    BitVector findSetsWithoutOneLocation() const;
    SiteWeight weightOf(const BitVector& sets) const;
    void forgetLocations();
    void placeSharedRowsInBestOrder();
    void reserveEmptySlotsBesideSharedRows();
    Vector<LocationRequest> collectRequests();
    void findMemberRows(LocationRequest&);
    bool canPlace(MemberRow, RowPart, uint32_t offset) const;
    void place(MemberRow, RowPart, uint32_t offset);
    void forEachFamily(std::span<const MemberRow>, const Invocable<void(std::span<const MemberRow>)> auto&) const;
    bool canPlaceFamily(std::span<const MemberRow>, RowPart, uint32_t offset) const;
    void tryPlaceRowsAt(Vector<MemberRow, 8>& unplaced, RowPart, uint32_t offset);
    std::optional<uint32_t> findMostSharedOffset(std::span<const MemberRow>, RowPart) const;
    void placeFamilyInGrownAreas(std::span<const MemberRow>, RowPart);
    void placePartTogether(std::span<const MemberRow>, RowPart);
    void unite(std::span<const MemberRow>);
    void placeTogether(std::span<MemberRow>);
    void placeOutOfLineRowsByWeight(LayoutID);
    void placeRemainingRows(LayoutID);
    void finishLayout(LayoutID);
    void collectSharedLocations();
    bool hasOneLocation(std::span<const MemberRow>) const;
    void countShares(std::span<const LocationRequest>) const;

    const SlotPlanContext& m_context;
    const SlotUses& m_uses;
    const JoinedRows& m_joinedRows;
    std::span<const SlotClaim> m_claims;
    PackedLayouts m_packed;
    Vector<Area> m_inline;
    Vector<Area> m_extension;
    Vector<bool> m_isPlaced;
    Vector<RowSet> m_rowSets;
    Vector<MemberRow> m_memberRows;
    uint32_t m_maxGranules;
    Vector<MemberRow> m_sharedRows;
    Vector<JoinedGroup> m_groupsInCells;
};

ObjectPacker::ObjectPacker(const SlotPlanContext& context, const SlotUses& uses, const JoinedRows& joinedRows, std::span<const SlotClaim> claims)
    : m_context(context)
    , m_uses(uses)
    , m_joinedRows(joinedRows)
    , m_claims(claims)
    , m_inline(context.program.layouts.size())
    , m_extension(context.program.layouts.size())
    , m_isPlaced(FillWith { }, context.rows.count(), false)
    , m_rowSets(context.rows.count())
    , m_maxGranules(static_cast<uint32_t>(std::min<uint64_t>(context.build.limit(LimitID::MaxExtensionGranules), maxExtensionGranules)))
{
    size_t numberOfLayouts = context.program.layouts.size();
    m_packed.slots.grow(context.rows.count());
    m_packed.sizes.fill(0, numberOfLayouts);
    m_packed.inlineLimits.fill(0, numberOfLayouts);
    m_packed.extensionSizes.fill(0, numberOfLayouts);
    m_packed.isEscapedOffsets.grow(numberOfLayouts);
    m_packed.bytesKeptZero.grow(numberOfLayouts);
    m_packed.sharedLocationOfRow.fill(noSharedLocation, context.rows.count());
}

PackedLayouts ObjectPacker::run()
{
    for (const Vector<LayoutID>& layouts : m_context.inheritedRows.layoutsByDepth()) {
        forEachIndexInParallel(loops(), layouts.size(), [&](unsigned index) {
            chooseStorage(layouts[index]);
        });
    }
    shareExtensionWords();
    forEachCompiledObject([&](LayoutID id) {
        initializeAreas(id);
    });
    placeExtensionWords();
    placeSharedRowsInBestOrder();
    reserveEmptySlotsBesideSharedRows();
    forEachCompiledObject([&](LayoutID id) {
        placeOutOfLineRowsByWeight(id);
        placeRemainingRows(id);
        finishLayout(id);
    });
    collectSharedLocations();
    countShares(collectRequests().span());
    return WTF::move(m_packed);
}

unsigned ObjectPacker::logSizeOf(MemberRow member, RowPart part) const
{
    switch (part) {
    case RowPart::Slot:
        return WTF::ctz(slotOf(member).width());
    case RowPart::DoubleArea:
        return overflowAreaLogSize;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<uint32_t> ObjectPacker::offsetOf(MemberRow member, RowPart part) const
{
    switch (part) {
    case RowPart::Slot:
        return m_isPlaced[member.row] ? std::optional(slotOf(member).offset) : std::nullopt;
    case RowPart::DoubleArea:
        return slotOf(member).doubleOffsetInExtension;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The layout that declares a field has chosen before the layouts below it, which follow.
void ObjectPacker::chooseStorage(LayoutID id)
{
    for (SlotRow row : rowsOf(m_context.rows, id)) {
        const SlotClaim& claim = m_claims[row];
        if (!claim.hasStorage)
            continue;
        PlacedSlot& slot = m_packed.slots[row];
        if (isInherited(row)) {
            slot = m_packed.slots[m_context.inheritedRows.declaringRowOf(row)];
            continue;
        }
        slot.representation = claim.representation;
        slot.storage = SlotStorage::Inline;
        slot.deferredFunction = claim.deferredFunction;
        slot.deferredClosureCapturesObject = claim.deferredClosureCapturesObject;
    }
    uint32_t granulesWanted = countGranulesOfInheritedRows(id);
    chooseOutOfLineRows(id, granulesWanted);
    widenSlotsWithoutOverflowArea(id, granulesWanted);
    m_context.build.exceeds(LimitID::MaxExtensionGranules, granulesWanted, { .layout = id });
    claimExtensionWord(id);
}

// Wherever the rows come to lie, an Extension with a granule for each slot and each double has room for them.
uint32_t ObjectPacker::countGranulesOfInheritedRows(LayoutID id) const
{
    uint32_t count = 0;
    for (SlotRow row : rowsOf(m_context.rows, id)) {
        const PlacedSlot& slot = m_packed.slots[row];
        if (isInherited(row))
            count += slot.isOutOfLine() || hasPart(slot, RowPart::DoubleArea);
    }
    return count;
}

void ObjectPacker::widenSlotsWithoutOverflowArea(LayoutID id, uint32_t& granulesWanted)
{
    for (SlotRow row : rowsOf(m_context.rows, id)) {
        PlacedSlot& slot = m_packed.slots[row];
        if (isInherited(row) || !hasPart(slot, RowPart::DoubleArea))
            continue;
        if (granulesWanted++ >= m_maxGranules)
            slot.representation = m_claims[row].representationWithoutOverflowArea;
    }
}

void ObjectPacker::chooseOutOfLineRows(LayoutID id, uint32_t& granulesWanted)
{
    uint32_t hiddenWordWidth = widthOf(hiddenReferenceRepresentation(m_context.usesWideReferences));
    uint32_t fixedBytes = 0;
    uint32_t optionalBytes = 0;
    uint32_t bytesSetAtCreation = 0;
    bool hasDoubles = false;
    Vector<SlotRow, 16> optionalRows;
    for (SlotRow row : rowsOf(m_context.rows, id)) {
        const SlotClaim& claim = m_claims[row];
        if (!claim.hasStorage)
            continue;
        uint32_t width = widthOf(claim.representation);
        hasDoubles |= claim.representation == SlotRepresentation::Int32OrDouble;
        if (isInherited(row)) {
            if (m_packed.slots[row].isOutOfLine())
                continue;
            width = m_packed.slots[row].width();
        }
        if ((claim.isSetAtCreation && (claim.isInitializedWithValue || !claim.existsAtCreation)) || m_uses.rows[row].isLikelyFilled())
            bytesSetAtCreation += width;
        if (!claim.mayBeOutOfLine || isInherited(row)) {
            fixedBytes += width;
            continue;
        }
        optionalBytes += width;
        optionalRows.append(row);
    }
    if (optionalRows.isEmpty())
        return;

    uint32_t everything = offsetOfFirstField + fixedBytes + optionalBytes + (hasDoubles ? hiddenWordWidth : 0);
    uint32_t required = offsetOfFirstField + hiddenWordWidth + fixedBytes;
    uint32_t atCreation = cellSizeFor(offsetOfFirstField + hiddenWordWidth + bytesSetAtCreation);
    uint32_t bytesBeyondCreation = referencesBeyondCreation * widthOf(SlotRepresentation::Cell32);
    uint32_t cap = std::max({ cellSizeFor(required), std::min(cellSizeFor(std::max(atCreation, required) + bytesBeyondCreation), maxInlineSize), std::min(atCreation, maxInlineSizeAtCreation) });
    if (cellSizeFor(everything) <= cap)
        return;

    auto scoreOf = [&](SlotRow row) {
        const SlotClaim& claim = m_claims[row];
        return std::tuple(claim.isSetAtCreation, claim.isInitializedWithValue, m_uses.rows[row].isLikelyFilled(), m_uses.rows[row].accessWeight);
    };
    std::ranges::sort(optionalRows, [&](SlotRow a, SlotRow b) {
        auto scoreOfA = scoreOf(a);
        auto scoreOfB = scoreOf(b);
        return scoreOfA != scoreOfB ? scoreOfA > scoreOfB : a < b;
    });
    uint32_t used = required;
    optionalRows.removeAllMatching([&](SlotRow row) {
        uint32_t width = widthOf(m_claims[row].representation);
        if (used + width > cap)
            return false;
        used += width;
        return true;
    });
    // Where the Extension has too few granules, the rows that are worth the most stay in the cell.
    for (SlotRow row : optionalRows | std::views::reverse) {
        const SlotClaim& claim = m_claims[row];
        if (granulesWanted++ >= m_maxGranules)
            continue;
        PlacedSlot& slot = m_packed.slots[row];
        slot.storage = SlotStorage::OutOfLine;
        slot.representation = claim.outOfLineRepresentation;
        slot.mayExistWithoutStorage = claim.existsAtCreation;
    }
}

void ObjectPacker::claimExtensionWord(LayoutID id)
{
    bool hasExtension = std::ranges::any_of(rowsOf(m_context.rows, id), [&](SlotRow row) {
        const PlacedSlot& slot = m_packed.slots[row];
        return slot.isOutOfLine() || hasPart(slot, RowPart::DoubleArea);
    });
    if (!hasExtension)
        return;
    PlacedSlot& word = m_packed.slots[m_context.rows.extensionWordOf(id)];
    word.representation = hiddenReferenceRepresentation(m_context.usesWideReferences);
    word.storage = SlotStorage::Inline;
}

// The word lies in front of the fields, so either every member of a family has it or none.
void ObjectPacker::shareExtensionWords()
{
    const InheritedRows& inheritedRows = m_context.inheritedRows;
    for (const InheritedRows::Family& family : inheritedRows.families()) {
        if (family.declaringRow != m_context.rows.extensionWordOf(m_context.rows.layoutOf(family.declaringRow)))
            continue;
        std::span<const SlotRow> words = inheritedRows.rowsOf(family);
        auto claimed = std::ranges::find_if(words, [&](SlotRow word) {
            return m_packed.slots[word].hasStorage();
        });
        if (claimed == words.end())
            continue;
        for (SlotRow word : words)
            m_packed.slots[word] = m_packed.slots[*claimed];
    }
}

void ObjectPacker::initializeAreas(LayoutID id)
{
    Area& inlineArea = m_inline[id.index()];
    Area& extensionArea = m_extension[id.index()];
    uint32_t inlineEnd = offsetOfFirstField;
    for (SlotRow row : rowsOf(m_context.rows, id)) {
        const PlacedSlot& slot = m_packed.slots[row];
        if (!slot.hasStorage())
            continue;
        ++(slot.isOutOfLine() ? extensionArea : inlineArea).unplaced[WTF::ctz(slot.width())];
        if (!slot.isOutOfLine())
            inlineEnd += slot.width();
        if (hasPart(slot, RowPart::DoubleArea))
            ++extensionArea.unplaced[overflowAreaLogSize];
    }
    inlineArea.initialize(offsetOfFirstField, cellSizeFor(inlineEnd));
    m_packed.inlineLimits[id.index()] = inlineArea.limit;
    // A granule that an object does not hold takes no room, so a row may lie wherever the layouts that share its location agree.
    if (m_packed.slots[m_context.rows.extensionWordOf(id)].hasStorage())
        extensionArea.initialize(offsetOfFirstExtensionSlot, offsetOfFirstExtensionSlot + m_maxGranules * extensionGranuleSize);
}

void ObjectPacker::placeExtensionWords()
{
    forEachCompiledObject([&](LayoutID id) {
        MemberRow word { id, m_context.rows.extensionWordOf(id) };
        if (!slotOf(word).hasStorage())
            return;
        RELEASE_ASSERT(canPlace(word, RowPart::Slot, offsetOfFirstField));
        place(word, RowPart::Slot, offsetOfFirstField);
    });
}

// A granule is found by counting the granules before it, which takes the least code for the first few, so there the fields that are used most come first, shared or not.
void ObjectPacker::appendSteps(std::span<const MemberRow> rows, StepRank rank, SiteWeight weight, Vector<PlacementStep>& steps) const
{
    PlacementStep step { .rank = rank, .weight = weight, .firstSharedRow = static_cast<size_t>(rows.data() - m_sharedRows.span().data()) };
    if (slotOf(rows[0]).isOutOfLine()) {
        step.rank = StepRank::JoinsFamilies;
        step.weight = 0;
        for (MemberRow member : rows)
            step.weight = std::max(step.weight, m_uses.rows[member.row].accessWeight);
    }
    for (size_t end = 1; end <= rows.size(); ++end) {
        step.depth = depthOfDeclaration(rows[end - 1]);
        step.numberOfSharedRows = static_cast<uint32_t>(end);
        if (end == rows.size() || depthOfDeclaration(rows[end]) != step.depth)
            steps.append(step);
    }
}

// A field lies where it lies in the class that declares it, so a base is placed before the layouts below it: then what is free in one member of a family is free in all.
// At one depth come first the rows that several families or layouts have to agree on, the heaviest set first. A family by itself finds a place wherever its members have room.
Vector<PlacementStep> ObjectPacker::collectSteps(const BitVector& setsToPlaceFirst)
{
    const SlotRows& slotRows = m_context.rows;
    const InheritedRows& inheritedRows = m_context.inheritedRows;
    Vector<PlacementStep> steps;
    BitVector isInStep(slotRows.count());
    m_sharedRows.reserveInitialCapacity(slotRows.count());
    auto appendRow = [&](SlotRow row) {
        m_sharedRows.append(MemberRow { slotRows.layoutOf(row), row });
    };
    auto markRows = [&](std::span<const MemberRow> rows) {
        for (MemberRow member : rows)
            isInStep.quickSet(member.row);
    };

    std::span<const JoinedRows::Set> sets = m_joinedRows.sets();
    for (uint32_t indexOfSet = 0; indexOfSet < sets.size(); ++indexOfSet) {
        const JoinedRows::Set& joined = sets[indexOfSet];
        size_t first = m_sharedRows.size();
        for (SlotRow row : m_joinedRows.rowsOf(joined)) {
            if (m_packed.slots[row].hasStorage())
                appendRow(row);
        }
        std::span<MemberRow> rows = m_sharedRows.mutableSpan().subspan(first);
        std::ranges::sort(rows, { }, [&](MemberRow member) {
            return std::tuple(slotOf(member).storage, slotOf(member).representation, depthOfDeclaration(member), inheritedRows.declaringRowOf(member.row), member.row);
        });
        forEachAdjacentGroup(rows, [&](MemberRow a, MemberRow b) {
            return slotOf(a).storage == slotOf(b).storage && slotOf(a).representation == slotOf(b).representation;
        }, [&](std::span<MemberRow> group) {
            markRows(group);
            if (areInOneFamily(group.front(), group.back())) {
                appendSteps(group, StepRank::OneJoinedFamily, joined.weight, steps);
                return;
            }
            appendSteps(group, setsToPlaceFirst.get(indexOfSet) ? StepRank::FoundNoRoomInFirstRound : StepRank::JoinsFamilies, joined.weight, steps);
            if (!slotOf(group[0]).isOutOfLine())
                m_groupsInCells.append(JoinedGroup { indexOfSet, static_cast<size_t>(group.data() - m_sharedRows.span().data()), static_cast<uint32_t>(group.size()) });
        });
    }

    for (const InheritedRows::Family& family : inheritedRows.families()) {
        std::span<const SlotRow> familyRows = inheritedRows.rowsOf(family);
        if (familyRows.size() < 2 || isInStep.quickGet(familyRows[0]) || !m_packed.slots[familyRows[0]].hasStorage() || m_isPlaced[familyRows[0]])
            continue;
        size_t first = m_sharedRows.size();
        for (SlotRow row : familyRows)
            appendRow(row);
        markRows(m_sharedRows.span().subspan(first));
        appendSteps(m_sharedRows.span().subspan(first), StepRank::OneFamily, 0, steps);
        steps.last().placesHiddenWords = slotRows.isHiddenWord(family.declaringRow);
    }

    for (SlotRow row = 0; row < slotRows.count(); ++row) {
        if (!m_packed.slots[row].isOutOfLine() || isInStep.quickGet(row) || !m_context.isCompiledObject(slotRows.layoutOf(row)))
            continue;
        appendRow(row);
        appendSteps(m_sharedRows.span().last(1), StepRank::OneFamily, 0, steps);
        steps.last().placesRowByItself = true;
    }

    std::ranges::sort(steps, [](const PlacementStep& a, const PlacementStep& b) {
        return std::tuple(a.depth, a.rank, b.weight, a.firstSharedRow) < std::tuple(b.depth, b.rank, a.weight, b.firstSharedRow);
    });
    return steps;
}

void ObjectPacker::placeAtFirstFreeOffset(MemberRow member)
{
    uint32_t width = slotOf(member).width();
    uint32_t offset = 0;
    while (!canPlace(member, RowPart::Slot, offset)) {
        offset += width;
        RELEASE_ASSERT(offset < areaOf(member, RowPart::Slot).limit);
    }
    place(member, RowPart::Slot, offset);
}

void ObjectPacker::placeSharedRows(const BitVector& setsToPlaceFirst)
{
    for (const PlacementStep& step : collectSteps(setsToPlaceFirst)) {
        std::span<MemberRow> rows = m_sharedRows.mutableSpan().subspan(step.firstSharedRow, step.numberOfSharedRows);
        // A shared location is a unit of fields, which a later step may widen or move. A hidden word stays as it is.
        if (step.placesRowByItself)
            placeAtFirstFreeOffset(rows[0]);
        else if (step.placesHiddenWords)
            placePartTogether(rows, RowPart::Slot);
        else
            placeTogether(rows);
    }
}

BitVector ObjectPacker::findSetsWithoutOneLocation() const
{
    BitVector sets;
    for (const JoinedGroup& group : m_groupsInCells) {
        std::span<const MemberRow> rows = m_sharedRows.span().subspan(group.firstSharedRow, group.numberOfSharedRows);
        bool hasOneLocation = std::ranges::all_of(rows, [&](MemberRow member) {
            return m_isPlaced[member.row] && slotOf(member).hasLocationOf(slotOf(rows[0]));
        });
        if (!hasOneLocation)
            sets.set(group.indexOfSet);
    }
    return sets;
}

SiteWeight ObjectPacker::weightOf(const BitVector& sets) const
{
    SiteWeight weight = 0;
    for (unsigned index : sets)
        weight = saturatedSum(weight, m_joinedRows.sets()[index].weight);
    return weight;
}

void ObjectPacker::forgetLocations()
{
    m_isPlaced.fill(false);
    m_rowSets = Vector<RowSet>(m_context.rows.count());
    m_sharedRows.shrink(0);
    m_groupsInCells.shrink(0);
    for (PlacedSlot& slot : m_packed.slots) {
        slot.offset = 0;
        slot.doubleOffsetInExtension = std::nullopt;
    }
    forEachCompiledObject([&](LayoutID id) {
        m_inline[id.index()] = { };
        m_extension[id.index()] = { };
        initializeAreas(id);
    });
    placeExtensionWords();
}

// The heaviest set takes the lowest offset that its members have free, which may be the only one that a small member of a lighter set has.
// So the sets that found no room go first in a second round, which counts if it leaves less weight without one location.
void ObjectPacker::placeSharedRowsInBestOrder()
{
    placeSharedRows({ });
    BitVector setsWithoutRoom = findSetsWithoutOneLocation();
    if (setsWithoutRoom.isEmpty())
        return;

    auto slots = m_packed.slots;
    auto inlineAreas = WTF::move(m_inline);
    auto extensionAreas = WTF::move(m_extension);
    auto isPlaced = m_isPlaced;
    auto rowSets = WTF::move(m_rowSets);
    m_inline = Vector<Area>(inlineAreas.size());
    m_extension = Vector<Area>(extensionAreas.size());
    forgetLocations();
    placeSharedRows(setsWithoutRoom);
    if (weightOf(findSetsWithoutOneLocation()) < weightOf(setsWithoutRoom))
        return;

    m_packed.slots = WTF::move(slots);
    m_inline = WTF::move(inlineAreas);
    m_extension = WTF::move(extensionAreas);
    m_isPlaced = WTF::move(isPlaced);
    m_rowSets = WTF::move(rowSets);
}

// The rows that no access shares can lie anywhere, so the bytes that a layout keeps zero for the slot of others are taken before them.
void ObjectPacker::reserveEmptySlotsBesideSharedRows()
{
    auto hasLocation = [&](SlotRow row) {
        return m_isPlaced[row];
    };
    auto isFree = [&](ReservedBytes bytes, LayoutID layout) {
        return m_inline[layout.index()].canReserve(bytes.offset, WTF::ctz(bytes.width));
    };
    auto take = [&](ReservedBytes bytes, LayoutID layout) {
        m_inline[layout.index()].reserve(bytes.offset, WTF::ctz(bytes.width));
    };
    reserveEmptySlots(m_context, m_uses, m_packed, { hasLocation, isFree, take });
}

Vector<LocationRequest> ObjectPacker::collectRequests()
{
    Vector<LocationRequest> requests;
    size_t numberOfMembers = 0;
    for (const SlotAccessUse& use : m_uses.accesses) {
        const SlotAccessKey& access = use.access;
        if (!requests.isEmpty() && requests.last().key == access.key && requests.last().receivers == access.receivers) {
            requests.last().weight = saturatedSum(requests.last().weight, use.weight);
            continue;
        }
        size_t numberOfReceivers = m_context.program.at(access.receivers).members.size();
        if (numberOfReceivers < 2)
            continue;
        requests.append(LocationRequest { access.key, access.receivers, use.weight, numberOfMembers, 0 });
        numberOfMembers += numberOfReceivers;
    }
    m_memberRows.grow(numberOfMembers);
    forEachIndexInParallel(loops(), requests.size(), [&](unsigned index) {
        findMemberRows(requests[index]);
    });
    requests.removeAllMatching([](const LocationRequest& request) {
        return request.numberOfMemberRows < 2;
    });
    parallelSort(loops(), requests.mutableSpan(), [](const LocationRequest& a, const LocationRequest& b) {
        if (a.weight != b.weight)
            return a.weight > b.weight;
        return std::tuple(a.key, a.receivers) < std::tuple(b.key, b.receivers);
    });
    return requests;
}

void ObjectPacker::findMemberRows(LocationRequest& request)
{
    const Program& program = m_context.program;
    std::span<MemberRow> rows = m_memberRows.mutableSpan().subspan(request.firstMemberRow);
    for (LayoutID member : program.at(request.receivers).members) {
        if (!m_context.isCompiledObject(member))
            continue;
        auto field = program.at(member).findField(request.key);
        if (!field)
            continue;
        SlotRow row = m_context.rows.rowOf(member, *field);
        if (m_packed.slots[row].hasStorage())
            rows[request.numberOfMemberRows++] = { member, row };
    }
    std::ranges::sort(memberRowsOf(request), { }, [&](MemberRow member) {
        return std::tuple(slotOf(member).storage, slotOf(member).representation, member.row);
    });
}

bool ObjectPacker::canPlace(MemberRow member, RowPart part, uint32_t offset) const
{
    return areaOf(member, part).canPlace(offset, logSizeOf(member, part));
}

void ObjectPacker::place(MemberRow member, RowPart part, uint32_t offset)
{
    areaOf(member, part).place(offset, logSizeOf(member, part));
    PlacedSlot& slot = m_packed.slots[member.row];
    switch (part) {
    case RowPart::Slot:
        slot.offset = offset;
        m_isPlaced[member.row] = true;
        return;
    case RowPart::DoubleArea:
        slot.doubleOffsetInExtension = offset;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ObjectPacker::forEachFamily(std::span<const MemberRow> rowsByFamily, const Invocable<void(std::span<const MemberRow>)> auto& functor) const
{
    for (size_t first = 0; first < rowsByFamily.size();) {
        size_t end = first + 1;
        while (end < rowsByFamily.size() && areInOneFamily(rowsByFamily[first], rowsByFamily[end]))
            ++end;
        functor(rowsByFamily.subspan(first, end - first));
        first = end;
    }
}

bool ObjectPacker::canPlaceFamily(std::span<const MemberRow> family, RowPart part, uint32_t offset) const
{
    return std::ranges::all_of(family, [&](MemberRow member) {
        return canPlace(member, part, offset);
    });
}

void ObjectPacker::tryPlaceRowsAt(Vector<MemberRow, 8>& unplacedByFamily, RowPart part, uint32_t offset)
{
    Vector<MemberRow, 8> stillUnplaced;
    forEachFamily(unplacedByFamily.span(), [&](std::span<const MemberRow> family) {
        if (!canPlaceFamily(family, part, offset)) {
            stillUnplaced.append(family);
            return;
        }
        for (MemberRow member : family)
            place(member, part, offset);
    });
    unplacedByFamily = WTF::move(stillUnplaced);
}

std::optional<uint32_t> ObjectPacker::findMostSharedOffset(std::span<const MemberRow> rowsByFamily, RowPart part) const
{
    const Vector<Area>& areas = areasOf(rowsByFamily[0], part);
    unsigned logSize = logSizeOf(rowsByFamily[0], part);
    uint32_t first = UINT32_MAX;
    Vector<uint32_t, 8> limitsDescending;
    for (MemberRow member : rowsByFamily) {
        const Area& area = areas[member.layout.index()];
        first = std::min(first, findFreeOffset(area.occupied, logSize, 0, area.limit).value_or(UINT32_MAX));
        limitsDescending.append(area.limit);
    }
    std::ranges::sort(limitsDescending, std::ranges::greater { });
    std::optional<uint32_t> best;
    size_t mostRows = 1;
    for (uint32_t offset = first; mostRows < rowsByFamily.size() && offset < limitsDescending[mostRows]; offset += 1u << logSize) {
        size_t numberOfRows = 0;
        forEachFamily(rowsByFamily, [&](std::span<const MemberRow> family) {
            if (canPlaceFamily(family, part, offset))
                numberOfRows += family.size();
        });
        if (numberOfRows <= mostRows)
            continue;
        mostRows = numberOfRows;
        best = offset;
    }
    return best;
}

// The rows that are left to place fit each area by their number, which does not say that one offset suits every member.
void ObjectPacker::placeFamilyInGrownAreas(std::span<const MemberRow> family, RowPart part)
{
    unsigned logSize = logSizeOf(family[0], part);
    uint32_t size = 1u << logSize;
    uint32_t offset = 0;
    auto isFreeInEveryMember = [&] {
        return std::ranges::all_of(family, [&](MemberRow member) {
            return areaOf(member, part).isFree(offset, size);
        });
    };
    while (!isFreeInEveryMember())
        offset += size;
    for (MemberRow member : family) {
        Area& area = areaOf(member, part);
        while (!area.canPlace(offset, logSize))
            area.growTo(cellSizeFor(area.limit + 1));
        if (!isInExtension(member, part))
            m_packed.inlineLimits[member.layout.index()] = area.limit;
        place(member, part, offset);
    }
}

void ObjectPacker::placePartTogether(std::span<const MemberRow> rows, RowPart part)
{
    Vector<MemberRow, 8> unplaced;
    Vector<uint32_t, 8> offsets;
    for (MemberRow member : rows) {
        if (auto offset = offsetOf(member, part))
            offsets.append(*offset);
        else
            unplaced.append(member);
    }
    if (unplaced.isEmpty())
        return;
    std::ranges::sort(unplaced, { }, [&](MemberRow member) {
        return std::pair(m_context.inheritedRows.declaringRowOf(member.row), member.row);
    });
    for (OffsetUse use : countOffsetUses(WTF::move(offsets)))
        tryPlaceRowsAt(unplaced, part, use.offset);
    while (unplaced.size() >= 2) {
        auto offset = findMostSharedOffset(unplaced.span(), part);
        if (!offset)
            break;
        tryPlaceRowsAt(unplaced, part, *offset);
    }
    forEachFamily(unplaced.span(), [&](std::span<const MemberRow> family) {
        if (family.size() >= 2)
            placeFamilyInGrownAreas(family, part);
    });
}

void ObjectPacker::unite(std::span<const MemberRow> rows)
{
    for (MemberRow member : rows.subspan(1))
        m_rowSets[member.row].unify(&m_rowSets[rows[0].row]);
}

void ObjectPacker::placeTogether(std::span<MemberRow> rows)
{
    bool hasDoubleAreas = hasPart(slotOf(rows[0]), RowPart::DoubleArea);
    placePartTogether(rows, RowPart::Slot);
    forEachSharedOffset(rows, RowPart::Slot, [&](std::span<MemberRow> rowsAtOffset) {
        if (!hasDoubleAreas) {
            unite(rowsAtOffset);
            return;
        }
        placePartTogether(rowsAtOffset, RowPart::DoubleArea);
        forEachSharedOffset(rowsAtOffset, RowPart::DoubleArea, [&](std::span<MemberRow> rowsAtLocation) {
            unite(rowsAtLocation);
        });
    });
}

void ObjectPacker::placeOutOfLineRowsByWeight(LayoutID id)
{
    Vector<SlotRow, 16> rows;
    for (SlotRow row : rowsOf(m_context.rows, id)) {
        if (m_packed.slots[row].isOutOfLine() && !m_isPlaced[row])
            rows.append(row);
    }
    std::ranges::sort(rows, [&](SlotRow a, SlotRow b) {
        return std::tuple(m_uses.rows[b].accessWeight, a) < std::tuple(m_uses.rows[a].accessWeight, b);
    });
    for (SlotRow row : rows)
        placeAtFirstFreeOffset({ id, row });
}

void ObjectPacker::placeRemainingRows(LayoutID id)
{
    for (unsigned logSize = numberOfWidths; logSize--;) {
        if (!m_inline[id.index()].unplaced[logSize] && !m_extension[id.index()].unplaced[logSize])
            continue;
        for (SlotRow row : rowsOf(m_context.rows, id)) {
            MemberRow member { id, row };
            for (RowPart part : { RowPart::Slot, RowPart::DoubleArea }) {
                if (!hasPart(slotOf(member), part) || logSizeOf(member, part) != logSize || offsetOf(member, part))
                    continue;
                const Area& area = areaOf(member, part);
                auto offset = findFreeOffset(area.occupied, logSize, 0, area.limit);
                RELEASE_ASSERT(offset);
                place(member, part, *offset);
            }
        }
    }
}

void ObjectPacker::finishLayout(LayoutID id)
{
    uint32_t inlineEnd = offsetOfFirstField;
    for (SlotRow row : rowsOf(m_context.rows, id)) {
        const PlacedSlot& slot = m_packed.slots[row];
        if (slot.storage == SlotStorage::Inline)
            inlineEnd = std::max(inlineEnd, slot.end());
    }
    uint32_t extensionSize = extensionEndOf(m_context.rows, m_packed.slots, id);
    m_packed.sizes[id.index()] = std::max(m_packed.sizes[id.index()], WTF::roundUpToMultipleOf(16, inlineEnd));
    m_packed.inlineLimits[id.index()] = m_inline[id.index()].limit;
    m_packed.extensionSizes[id.index()] = extensionSize;
    uint32_t farthestEnd = std::max(inlineEnd, extensionSize);
    if (farthestEnd > m_context.build.limit(LimitID::MaxFieldOffset))
        m_context.build.reportLimitExceeded(LimitID::MaxFieldOffset, farthestEnd, { .layout = id });
}

void ObjectPacker::collectSharedLocations()
{
    auto rootOf = [&](SlotRow row) {
        return static_cast<SlotRow>(m_rowSets[row].find() - m_rowSets.begin());
    };
    BitVector isSharedRoot;
    isSharedRoot.ensureSize(m_rowSets.size());
    for (SlotRow row = 0; row < m_rowSets.size(); ++row) {
        if (!m_rowSets[row].isRoot())
            isSharedRoot.quickSet(rootOf(row));
    }
    for (SlotRow row = 0; row < m_rowSets.size(); ++row) {
        SlotRow root = rootOf(row);
        if (!isSharedRoot.quickGet(root))
            continue;
        uint32_t& locationOfRoot = m_packed.sharedLocationOfRow[root];
        if (locationOfRoot == noSharedLocation) {
            locationOfRoot = m_packed.sharedLocations.size();
            m_packed.sharedLocations.append(SharedLocation { });
        }
        SharedLocation& location = m_packed.sharedLocations[locationOfRoot];
        location.rows.append(row);
        location.weight = std::max(location.weight, m_uses.rows[row].accessWeight);
        m_packed.sharedLocationOfRow[row] = locationOfRoot;
    }
}

bool ObjectPacker::hasOneLocation(std::span<const MemberRow> rows) const
{
    const PlacedSlot& first = slotOf(rows[0]);
    return std::ranges::all_of(rows.subspan(1), [&](MemberRow member) {
        if (!slotOf(member).hasLocationOf(first))
            return false;
        return !first.needsExtension() || extensionWordOf(member).hasLocationOf(extensionWordOf(rows[0]));
    });
}

void ObjectPacker::countShares(std::span<const LocationRequest> requests) const
{
    Statistics& statistics = m_context.build.statistics();
    forEachIndexInParallel(loops(), requests.size(), [&](unsigned index) {
        statistics.count(hasOneLocation(memberRowsOf(requests[index])) ? StatisticID::SharedSlots : StatisticID::SharesRefused);
    });
}

} // anonymous namespace

PackedLayouts packObjects(const SlotPlanContext& context, const SlotUses& uses, const JoinedRows& joinedRows, std::span<const SlotClaim> claims)
{
    return ObjectPacker(context, uses, joinedRows, claims).run();
}

OccupiedBytes occupiedBytesOf(const SlotPlanContext& context, const PackedLayouts& packed, LayoutID id)
{
    OccupiedBytes words;
    words.fill(0, packed.inlineLimits[id.index()] / 64 + 1);
    words[0] = bytesAt(0, context.isEnvironment(id) ? offsetOfFirstEnvironmentVariable : offsetOfFirstField);
    for (SlotRow row : rowsOf(context.rows, id)) {
        const PlacedSlot& slot = packed.slots[row];
        if (slot.storage == SlotStorage::Inline)
            words[slot.offset / 64] |= bytesAt(slot.offset, slot.width());
    }
    for (ReservedBytes reserved : packed.bytesKeptZero[id.index()])
        words[reserved.offset / 64] |= bytesAt(reserved.offset, reserved.width);
    if (auto offset = packed.isEscapedOffsets[id.index()])
        words[*offset / 64] |= bytesAt(*offset, sizeof(bool));
    return words;
}

void updateExtensionSizes(const SlotPlanContext& context, PackedLayouts& packed)
{
    forEachIndexInParallel(context.build.parallelLoops(), context.program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        if (!context.isCompiledObject(id))
            return;
        packed.extensionSizes[index] = extensionEndOf(context.rows, packed.slots, id);
        if (!packed.extensionSizes[index])
            packed.slots[context.rows.extensionWordOf(id)] = PlacedSlot { };
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
