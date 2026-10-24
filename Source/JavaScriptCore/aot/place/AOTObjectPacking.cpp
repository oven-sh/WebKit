/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
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
static_assert(widthOf(SlotRepresentation::JSValue) == widestSlot);
static_assert(!(offsetOfFirstField % widestSlot) && !(offsetOfFirstExtensionSlot % widestSlot) && extensionGranuleSize == widestSlot);
static_assert(defaultLimitValue(LimitID::MaxExtensionGranules, hostArchitecture()) == maxExtensionGranules);

using WidthCounts = std::array<uint32_t, numberOfWidths>;

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
    bool placesTwoRowsInOneGranule { false };
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

struct PackedRowSet : WTF::UnionFind<PackedRowSet> { };

auto rowsOf(const SlotRows& rows, LayoutID id)
{
    SlotRow first = rows.firstRowOf(id);
    return std::views::iota(first, first + rows.numberOfRowsOf(id));
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

Vector<OffsetUse, 8> countOffsetUses(Vector<uint32_t, 8>&& offsets)
{
    AOT::sort(offsets);
    Vector<OffsetUse, 8> uses;
    for (uint32_t offset : offsets) {
        if (uses.isEmpty() || uses.last().offset != offset)
            uses.append(OffsetUse { offset, 0 });
        ++uses.last().numberOfRows;
    }
    AOT::sort(uses, [](const OffsetUse& a, const OffsetUse& b) {
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

    const Vector<Area>& areasOf(MemberRow member) const { return slotOf(member).isOutOfLine() ? m_extension : m_inline; }
    const Area& areaOf(MemberRow member) const { return areasOf(member)[member.layout.index()]; }
    Area& areaOf(MemberRow member) { return (slotOf(member).isOutOfLine() ? m_extension : m_inline)[member.layout.index()]; }
    unsigned logSizeOf(MemberRow member) const { return WTF::ctz(slotOf(member).width()); }

    void forEachCompiledObject(const Invocable<void(LayoutID)> auto& functor) const
    {
        forEachIndexInParallel(loops(), m_context.program.layouts.size(), [&](unsigned index) {
            if (LayoutID id(index); m_context.isCompiledObject(id))
                functor(id);
        });
    }

    void forEachSharedOffset(std::span<MemberRow> rows, const Invocable<void(std::span<MemberRow>)> auto& functor) const
    {
        auto unplaced = std::ranges::partition(rows, [&](MemberRow member) {
            return m_isPlaced[member.row];
        });
        std::span<MemberRow> placed = rows.first(unplaced.begin() - rows.begin());
        auto orderOf = [&](MemberRow member) {
            return std::pair(slotOf(member).offset, member.row);
        };
        if (!std::ranges::is_sorted(placed, { }, orderOf))
            AOT::sort(placed, { }, orderOf);
        forEachAdjacentGroup(placed, [&](MemberRow a, MemberRow b) {
            return slotOf(a).offset == slotOf(b).offset;
        }, functor);
    }

    bool isInherited(SlotRow row) const { return m_context.inheritedRows.declaringRowOf(row) != row; }
    bool areInOneFamily(MemberRow a, MemberRow b) const { return m_context.inheritedRows.declaringRowOf(a.row) == m_context.inheritedRows.declaringRowOf(b.row); }
    LayoutID layoutOf(SlotRow row) const { return m_rowLayouts[row]; }
    uint32_t depthOfDeclaration(MemberRow member) const { return m_declarationDepths[member.row]; }
    void computeRowProperties();
    const Field& declarationAt(LayoutID, SlotRow) const;
    bool hasVisibleStore(LayoutID, SlotRow) const;
    bool isOnlyDeclaredByMakers(SlotRow) const;
    void chooseStorage(LayoutID);
    struct RowsOutOfLine {
        WidthCounts counts { };
        uint32_t numberOfRows { 0 };
        uint32_t numberOfRowsWithoutRoom { 0 };
    };

    RowsOutOfLine countInheritedRowsOutOfLine(LayoutID) const;
    bool hasRoomInExtension(LayoutID, const RowsOutOfLine&, unsigned logSize) const;
    bool tryMoveOutOfLine(LayoutID, SlotRow, RowsOutOfLine&);
    bool mayExistWithoutStorage(LayoutID, SlotRow) const;
    void moveOutOfLine(LayoutID, SlotRow);
    void chooseOutOfLineRows(LayoutID, RowsOutOfLine&);
    void claimExtensionWord(LayoutID);
    uint32_t bytesInCell(LayoutID) const;
    void moveIntoCell(SlotRow);
    void agreeOnStorage();
    void shareExtensionWords();
    void initializeAreas(LayoutID);
    void placeExtensionWords();
    void appendSteps(std::span<const MemberRow>, StepRank, SiteWeight, Vector<PlacementStep>&) const;
    void placeAtFirstFreeOffset(MemberRow);
    void placeInOneGranule(MemberRow, MemberRow);
    Vector<SlotRow> matchRowsStoredTogether(const BitVector& isInStep) const;
    Vector<PlacementStep> collectSteps(const BitVector& setsToPlaceFirst);
    void placeSharedRows(const BitVector& setsToPlaceFirst);
    BitVector findSetsWithoutOneLocation() const;
    SiteWeight weightOf(const BitVector& sets) const;
    void forgetLocations();
    void placeSharedRowsInBestOrder();
    void reserveEmptySlotsBesideSharedRows();
    Vector<LocationRequest> collectRequests();
    void findMemberRows(LocationRequest&);
    bool canPlace(MemberRow, uint32_t offset) const;
    void place(MemberRow, uint32_t offset);
    void forEachFamily(std::span<const MemberRow>, const Invocable<void(std::span<const MemberRow>)> auto&) const;
    bool canPlaceFamily(std::span<const MemberRow>, uint32_t offset) const;
    void tryPlaceRowsAt(Vector<MemberRow, 8>& unplaced, uint32_t offset);
    std::optional<uint32_t> findMostSharedOffset(std::span<const MemberRow>) const;
    void placeFamilyInGrownAreas(std::span<const MemberRow>);
    uint32_t maxLimit(MemberRow) const;
    std::optional<uint32_t> limitThatTakes(MemberRow, uint32_t offset) const;
    std::optional<uint32_t> growthByPlacingAt(std::span<const MemberRow>, uint32_t offset) const;
    bool tryPlaceAtOneOffset(std::span<const MemberRow> unplaced, std::span<const uint32_t> offsetsInUse);
    void placeAtSharedOffsets(std::span<const MemberRow>, bool mayGrowCells = false);
    void unite(std::span<const MemberRow>);
    void placeTogether(std::span<MemberRow>, bool mayGrowCells);
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
    Vector<LayoutID> m_rowLayouts;
    Vector<uint32_t> m_declarationDepths;
    BitVector m_rowsInUnorderedLayouts;
    BitVector m_rowsTreatedAsStored;
    BitVector m_rowsWithHeirs;
    Vector<uint32_t> m_limitsBeforeGrowth;
    Vector<PackedRowSet> m_rowSets;
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
    , m_rowsInUnorderedLayouts(context.rows.count())
    , m_rowsTreatedAsStored(context.rows.count())
    , m_rowSets(context.rows.count())
    , m_maxGranules(static_cast<uint32_t>(std::min<uint64_t>(context.build.limit(LimitID::MaxExtensionGranules), maxExtensionGranules)))
{
    size_t numberOfLayouts = context.program.layouts.size();
    m_packed.slots.grow(context.rows.count());
    m_packed.sizes.fill(0, numberOfLayouts);
    m_packed.inlineLimits.fill(0, numberOfLayouts);
    m_packed.extensionSizes.fill(0, numberOfLayouts);
    m_packed.bytesKeptZero.grow(numberOfLayouts);
    m_packed.sharedLocationOfRow.fill(noSharedLocation, context.rows.count());
    m_limitsBeforeGrowth.fill(0, numberOfLayouts);
}

PackedLayouts ObjectPacker::run()
{
    computeRowProperties();
    for (const Vector<LayoutID>& layouts : m_context.inheritedRows.layoutsByDepth()) {
        forEachIndexInParallel(loops(), layouts.size(), [&](unsigned index) {
            chooseStorage(layouts[index]);
        });
    }
    agreeOnStorage();
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

void ObjectPacker::computeRowProperties()
{
    const Program& program = m_context.program;
    const InheritedRows& inheritedRows = m_context.inheritedRows;
    m_rowLayouts.grow(m_context.rows.count());
    forEachIndexInParallel(loops(), program.layouts.size(), [&](unsigned index) {
        for (SlotRow row : rowsOf(m_context.rows, LayoutID(index)))
            m_rowLayouts[row] = LayoutID(index);
    });
    m_declarationDepths.grow(m_context.rows.count());
    forEachIndexInParallel(loops(), program.layouts.size(), [&](unsigned index) {
        for (SlotRow row : rowsOf(m_context.rows, LayoutID(index)))
            m_declarationDepths[row] = inheritedRows.depthOf(layoutOf(inheritedRows.declaringRowOf(row)));
    });
    m_rowsWithHeirs = BitVector(m_claims.size());
    for (const InheritedRows::Family& family : m_context.inheritedRows.families()) {
        for (SlotRow row : m_context.inheritedRows.rowsOf(family)) {
            if (row != family.declaringRow)
                m_rowsWithHeirs.quickSet(family.declaringRow);
        }
    }
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        if (!m_context.isCompiledObject(id))
            continue;
        bool mayBeUnordered = mayHaveUnorderedShape(program, m_context.inputs.flow, id);
        bool isFilledByNativeCode = program.at(id).flow.observedUses.contains(ObservedUse::ReachesTypedParse);
        if (!mayBeUnordered && !isFilledByNativeCode)
            continue;
        for (SlotRow row : rowsOf(m_context.rows, id)) {
            SlotRow declaringRow = m_context.inheritedRows.declaringRowOf(row);
            if (mayBeUnordered)
                m_rowsInUnorderedLayouts.quickSet(declaringRow);
            if (isFilledByNativeCode)
                m_rowsTreatedAsStored.quickSet(declaringRow);
        }
    }
    for (const JoinedRows::Set& joined : m_joinedRows.sets()) {
        std::span<const SlotRow> rows = m_joinedRows.rowsOf(joined);
        bool isStored = std::ranges::any_of(rows, [&](SlotRow row) {
            return m_uses.rows[row].isStoredSomewhere();
        });
        if (!isStored)
            continue;
        for (SlotRow row : rows)
            m_rowsTreatedAsStored.quickSet(m_context.inheritedRows.declaringRowOf(row));
    }
}

const Field& ObjectPacker::declarationAt(LayoutID id, SlotRow row) const
{
    const Program& program = m_context.program;
    return fieldAt(program, declarationOf(program, { id, row - m_context.rows.firstRowOf(id) }));
}

bool ObjectPacker::hasVisibleStore(LayoutID id, SlotRow row) const
{
    constexpr OptionSet<FieldFlags> privateNameFlags { FieldFlags::IsPrivate, FieldFlags::IsBrand };
    const SlotRowUse& use = m_uses.rows[row];
    if (use.isStoredSomewhere() || m_claims[row].existsAtCreation || !use.holderHeat || m_rowsTreatedAsStored.quickGet(row))
        return true;
    return declarationAt(id, row).flags.containsAny(privateNameFlags);
}

// `this.original = undefined` in the constructor of every node of a tree gives the objects one shape and fills nothing.
bool ObjectPacker::isOnlyDeclaredByMakers(SlotRow row) const
{
    constexpr uint64_t placesGivingUndefinedForEachGivingValue = 16;
    const SlotRowUse& use = m_uses.rows[row];
    return use.isOptionalInTypeOfThis && !m_claims[row].isInitializedWithValue && use.storesOfConstructorWithValue * placesGivingUndefinedForEachGivingValue < use.storesOfUndefined;
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
        slot.deferredClosureCapturesEnvironment = claim.deferredClosureCapturesEnvironment;
    }
    RowsOutOfLine outOfLine = countInheritedRowsOutOfLine(id);
    chooseOutOfLineRows(id, outOfLine);
    m_context.build.exceeds(LimitID::MaxExtensionGranules, outOfLine.numberOfRowsWithoutRoom ? m_maxGranules + outOfLine.numberOfRowsWithoutRoom : std::min(outOfLine.numberOfRows, m_maxGranules), { .layout = id });
    claimExtensionWord(id);
}

ObjectPacker::RowsOutOfLine ObjectPacker::countInheritedRowsOutOfLine(LayoutID id) const
{
    RowsOutOfLine outOfLine;
    for (SlotRow row : rowsOf(m_context.rows, id)) {
        if (!isInherited(row) || !m_packed.slots[row].isOutOfLine())
            continue;
        ++outOfLine.counts[WTF::ctz(m_packed.slots[row].width())];
        ++outOfLine.numberOfRows;
    }
    return outOfLine;
}

// The members of a family agree on one offset for a row, and with a granule for each row there is one that all of them have free.
bool ObjectPacker::hasRoomInExtension(LayoutID id, const RowsOutOfLine& outOfLine, unsigned logSize) const
{
    if (m_context.inheritedRows.isInFamily(id))
        return outOfLine.numberOfRows < m_maxGranules;
    WidthCounts room;
    for (unsigned width = 0; width < numberOfWidths; ++width)
        room[width] = (m_maxGranules * extensionGranuleSize) >> width;
    WidthCounts counts = outOfLine.counts;
    ++counts[logSize];
    return remainingRowsFit(room, counts);
}

bool ObjectPacker::tryMoveOutOfLine(LayoutID id, SlotRow row, RowsOutOfLine& outOfLine)
{
    unsigned logSize = WTF::ctz(widthOf(m_claims[row].outOfLineRepresentation));
    if (!hasRoomInExtension(id, outOfLine, logSize))
        return false;
    ++outOfLine.counts[logSize];
    ++outOfLine.numberOfRows;
    moveOutOfLine(id, row);
    return true;
}

bool ObjectPacker::mayExistWithoutStorage(LayoutID id, SlotRow row) const
{
    if (m_claims[row].existsAtCreation)
        return true;
    const SlotRowUse& use = m_uses.rows[row];
    if (!use.storesOfUndefined || m_rowsInUnorderedLayouts.quickGet(row))
        return false;
    const DeclaredType& declared = declarationAt(id, row).declared;
    return declared.flags.contains(PositionFlags::IsExpected) || (m_context.program.at(declared.type).primitives & Primitive::Undefined);
}

void ObjectPacker::moveOutOfLine(LayoutID id, SlotRow row)
{
    PlacedSlot& slot = m_packed.slots[row];
    slot.storage = SlotStorage::OutOfLine;
    slot.representation = m_claims[row].outOfLineRepresentation;
    slot.mayExistWithoutStorage = mayExistWithoutStorage(id, row);
    slot.isFilledByConstructor = m_uses.rows[row].storesOfConstructorWithValue;
}

void ObjectPacker::chooseOutOfLineRows(LayoutID id, RowsOutOfLine& outOfLine)
{
    uint32_t hiddenWordWidth = widthOf(hiddenReferenceRepresentation(m_context.usesWideReferences));
    uint32_t fixedBytes = 0;
    uint32_t optionalBytes = 0;
    uint32_t bytesSetAtCreation = 0;
    bool hasRowsOutOfLine = false;
    Vector<SlotRow, 16> optionalRows;
    for (SlotRow row : rowsOf(m_context.rows, id)) {
        const SlotClaim& claim = m_claims[row];
        if (!claim.hasStorage)
            continue;
        uint32_t width = widthOf(claim.representation);
        if (isInherited(row)) {
            if (m_packed.slots[row].isOutOfLine()) {
                hasRowsOutOfLine = true;
                continue;
            }
            width = m_packed.slots[row].width();
        } else if (claim.mayBeOutOfLine && !hasVisibleStore(id, row) && tryMoveOutOfLine(id, row, outOfLine)) {
            hasRowsOutOfLine = true;
            continue;
        } else if (claim.mayBeOutOfLine && m_rowsWithHeirs.quickGet(row) && isOnlyDeclaredByMakers(row) && tryMoveOutOfLine(id, row, outOfLine)) {
            // Every heir has the row where this layout puts it, and rows of its own to find room for.
            hasRowsOutOfLine = true;
            continue;
        }
        // A field that is added late is stored by a function of its own, which may store into one object again and again: how often it runs ranks such fields and does not say how many objects get one.
        bool isAddedLate = row < m_context.rows.extensionWordOf(id) && declarationAt(id, row).flags.contains(FieldFlags::IsAddedLate);
        bool isLikelyFilled = isAddedLate ? m_uses.rows[row].isFilledBesideCreation() : m_uses.rows[row].isLikelyFilled();
        if ((claim.isSetAtCreation && (claim.isInitializedWithValue || !claim.existsAtCreation)) || isLikelyFilled)
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

    uint32_t everything = offsetOfFirstField + fixedBytes + optionalBytes + (hasRowsOutOfLine ? hiddenWordWidth : 0);
    uint32_t required = offsetOfFirstField + hiddenWordWidth + fixedBytes;
    uint32_t atCreation = cellSizeFor(offsetOfFirstField + hiddenWordWidth + bytesSetAtCreation);
    uint32_t bytesBeyondCreation = referencesBeyondCreation * widthOf(SlotRepresentation::Cell32);
    uint32_t cap = std::max({ cellSizeFor(required), std::min(cellSizeFor(std::max(atCreation, required) + bytesBeyondCreation), maxInlineSize), std::min(atCreation, maxInlineSizeAtCreation) });
    if (cellSizeFor(everything) <= cap)
        return;
    // An Extension saves room where many objects are made and few of them get the field. Where few are made it saves next to nothing, and every access of the field pays for it.
    if (cellSizeFor(everything) <= maxInlineSizeAtCreation && m_context.isMadeSeldom(id))
        return;

    // Neither estimate says how many objects get a field.
    SlotRow firstRow = m_context.rows.firstRowOf(id);
    Vector<uint32_t, 32> betterRanks(FillWith { }, optionalRows.last() - firstRow + 1, std::numeric_limits<uint32_t>::max());
    auto rankBy = [&](auto&& estimateOf) {
        Vector<SlotRow, 16> ranked = optionalRows;
        AOT::sort(ranked, [&](SlotRow a, SlotRow b) {
            return std::tuple(estimateOf(m_uses.rows[a]), b) > std::tuple(estimateOf(m_uses.rows[b]), a);
        });
        for (uint32_t rank = 0; rank < ranked.size(); ++rank)
            betterRanks[ranked[rank] - firstRow] = std::min(betterRanks[ranked[rank] - firstRow], rank);
    };
    rankBy([](const SlotRowUse& use) { return use.fillHeat; });
    rankBy([](const SlotRowUse& use) { return use.accessWeight; });
    auto scoreOf = [&](SlotRow row) {
        const SlotClaim& claim = m_claims[row];
        const SlotRowUse& use = m_uses.rows[row];
        return std::tuple(claim.isSetAtCreation, claim.isInitializedWithValue, use.isFilledBesideCreation(), std::numeric_limits<uint32_t>::max() - betterRanks[row - firstRow], hasVisibleStore(id, row), use.accessWeight);
    };
    AOT::sort(optionalRows, [&](SlotRow a, SlotRow b) {
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
        if (!tryMoveOutOfLine(id, row, outOfLine))
            ++outOfLine.numberOfRowsWithoutRoom;
    }
}

void ObjectPacker::claimExtensionWord(LayoutID id)
{
    bool hasExtension = std::ranges::any_of(rowsOf(m_context.rows, id), [&](SlotRow row) {
        return m_packed.slots[row].isOutOfLine();
    });
    PlacedSlot& word = m_packed.slots[m_context.rows.extensionWordOf(id)];
    if (!hasExtension) {
        word = PlacedSlot { };
        return;
    }
    word.representation = hiddenReferenceRepresentation(m_context.usesWideReferences);
    word.storage = SlotStorage::Inline;
}

uint32_t ObjectPacker::bytesInCell(LayoutID id) const
{
    uint32_t bytes = offsetOfFirstField;
    for (SlotRow row : rowsOf(m_context.rows, id)) {
        const PlacedSlot& slot = m_packed.slots[row];
        if (slot.hasStorage() && !slot.isOutOfLine())
            bytes += slot.width();
    }
    return bytes;
}

void ObjectPacker::moveIntoCell(SlotRow row)
{
    PlacedSlot& slot = m_packed.slots[row];
    slot.storage = SlotStorage::Inline;
    slot.representation = m_claims[row].representation;
    slot.mayExistWithoutStorage = false;
    slot.isFilledByConstructor = false;
}

// One load reads a field that several layouts share only where it lies in the cell of all of them or in the Extension of all of them, and each layout has chosen by itself.
void ObjectPacker::agreeOnStorage()
{
    constexpr double accessesForEachByte = 256;
    Vector<std::optional<uint32_t>> bytesInCells(m_context.program.layouts.size());
    auto bytesOf = [&](LayoutID id) -> uint32_t& {
        std::optional<uint32_t>& bytes = bytesInCells[id.index()];
        if (!bytes)
            bytes = bytesInCell(id);
        return *bytes;
    };
    BitVector layoutsChanged(m_context.program.layouts.size());
    Vector<SlotRow, 16> rowsOutOfLine;
    for (const JoinedRows::Set& joined : m_joinedRows.sets()) {
        rowsOutOfLine.shrink(0);
        bool hasRowInCell = false;
        for (SlotRow row : m_joinedRows.rowsOf(joined)) {
            const PlacedSlot& slot = m_packed.slots[row];
            if (!slot.hasStorage())
                continue;
            if (slot.isOutOfLine())
                rowsOutOfLine.append(row);
            else
                hasRowInCell |= !!m_uses.rows[row].holderHeat;
        }
        if (!hasRowInCell || rowsOutOfLine.isEmpty())
            continue;
        // Hardly an object has the field, so an access of it mostly finds that there is no Extension to look into.
        bool isOnlyDeclared = std::ranges::any_of(rowsOutOfLine, [&](SlotRow row) {
            return isOnlyDeclaredByMakers(m_context.inheritedRows.declaringRowOf(row));
        });
        if (isOnlyDeclared)
            continue;
        double bytesAllocated = 0;
        for (SlotRow row : rowsOutOfLine) {
            uint32_t bytes = bytesOf(layoutOf(row));
            bytesAllocated += m_uses.rows[row].holderHeat * (cellSizeFor(bytes + widthOf(m_claims[row].representation)) - cellSizeFor(bytes));
        }
        if (bytesAllocated * accessesForEachByte > static_cast<double>(joined.weight >> siteWeightFractionBits))
            continue;
        for (SlotRow row : rowsOutOfLine) {
            LayoutID layout = layoutOf(row);
            moveIntoCell(row);
            layoutsChanged.quickSet(layout.index());
            bytesOf(layout) += m_packed.slots[row].width();
        }
    }
    for (unsigned index : layoutsChanged)
        claimExtensionWord(LayoutID(index));
}

// The word lies in front of the fields, so either every member of a family has it or none.
void ObjectPacker::shareExtensionWords()
{
    const InheritedRows& inheritedRows = m_context.inheritedRows;
    for (const InheritedRows::Family& family : inheritedRows.families()) {
        if (family.declaringRow != m_context.rows.extensionWordOf(layoutOf(family.declaringRow)))
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
    }
    inlineArea.initialize(offsetOfFirstField, cellSizeFor(inlineEnd));
    m_packed.inlineLimits[id.index()] = inlineArea.limit;
    m_limitsBeforeGrowth[id.index()] = inlineArea.limit;
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
        RELEASE_ASSERT(canPlace(word, offsetOfFirstField));
        place(word, offsetOfFirstField);
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

static constexpr SlotRow noRow = std::numeric_limits<SlotRow>::max();

// Two rows of four bytes share a granule of the Extension, and an object has the granule as soon as it has one of the two.
Vector<SlotRow> ObjectPacker::matchRowsStoredTogether(const BitVector& isInStep) const
{
    const SlotRows& slotRows = m_context.rows;
    Vector<SlotRow> partners(FillWith { }, slotRows.count(), noRow);
    auto isFree = [&](SlotRow row) {
        const PlacedSlot& slot = m_packed.slots[row];
        return partners[row] == noRow && slot.isOutOfLine() && slot.width() * 2 == extensionGranuleSize && !isInStep.quickGet(row) && m_context.isCompiledObject(layoutOf(row));
    };
    for (const RowsStoredTogether& stored : m_uses.rowsStoredTogether) {
        if (!isFree(stored.first) || !isFree(stored.second))
            continue;
        LayoutID layout = layoutOf(stored.first);
        if (depthOfDeclaration({ layout, stored.first }) != depthOfDeclaration({ layout, stored.second }))
            continue;
        partners[stored.first] = stored.second;
        partners[stored.second] = stored.first;
    }
    return partners;
}

// A field lies where it lies in the class that declares it, so a base is placed before the layouts below it: then what is free in one member of a family is free in all.
Vector<PlacementStep> ObjectPacker::collectSteps(const BitVector& setsToPlaceFirst)
{
    const SlotRows& slotRows = m_context.rows;
    const InheritedRows& inheritedRows = m_context.inheritedRows;
    Vector<PlacementStep> steps;
    BitVector isInStep(slotRows.count());
    m_sharedRows.reserveCapacity(slotRows.count());
    auto appendRow = [&](SlotRow row) {
        m_sharedRows.append(MemberRow { layoutOf(row), row });
    };
    auto markRows = [&](std::span<const MemberRow> rows) {
        for (MemberRow member : rows)
            isInStep.quickSet(member.row);
    };

    std::span<const JoinedRows::Set> sets = m_joinedRows.sets();
    Vector<size_t> setOffsets;
    setOffsets.reserveInitialCapacity(sets.size() + 1);
    for (const JoinedRows::Set& joined : sets) {
        setOffsets.append(m_sharedRows.size());
        for (SlotRow row : m_joinedRows.rowsOf(joined)) {
            if (m_packed.slots[row].hasStorage())
                appendRow(row);
        }
    }
    setOffsets.append(m_sharedRows.size());
    auto setRows = [&](uint32_t indexOfSet) {
        return m_sharedRows.mutableSpan().subspan(setOffsets[indexOfSet], setOffsets[indexOfSet + 1] - setOffsets[indexOfSet]);
    };
    forEachIndexInParallel(loops(), sets.size(), [&](unsigned indexOfSet) {
        AOT::sort(setRows(indexOfSet), { }, [&](MemberRow member) {
            return std::tuple(slotOf(member).storage, slotOf(member).representation, depthOfDeclaration(member), inheritedRows.declaringRowOf(member.row), member.row);
        });
    });
    for (uint32_t indexOfSet = 0; indexOfSet < sets.size(); ++indexOfSet) {
        const JoinedRows::Set& joined = sets[indexOfSet];
        forEachAdjacentGroup(setRows(indexOfSet), [&](MemberRow a, MemberRow b) {
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
        steps.last().placesHiddenWords = family.declaringRow >= slotRows.extensionWordOf(layoutOf(family.declaringRow));
    }

    Vector<SlotRow> partners = matchRowsStoredTogether(isInStep);
    for (SlotRow row = 0; row < slotRows.count(); ++row) {
        if (!m_packed.slots[row].isOutOfLine() || isInStep.quickGet(row) || !m_context.isCompiledObject(layoutOf(row)))
            continue;
        if (partners[row] == noRow) {
            appendRow(row);
            appendSteps(m_sharedRows.span().last(1), StepRank::OneFamily, 0, steps);
            steps.last().placesRowByItself = true;
            continue;
        }
        if (partners[row] < row)
            continue;
        appendRow(row);
        appendRow(partners[row]);
        appendSteps(m_sharedRows.span().last(2), StepRank::OneFamily, 0, steps);
        steps.last().placesTwoRowsInOneGranule = true;
    }

    AOT::sort(steps, [](const PlacementStep& a, const PlacementStep& b) {
        return std::tuple(a.depth, a.rank, b.weight, a.firstSharedRow) < std::tuple(b.depth, b.rank, a.weight, b.firstSharedRow);
    });
    return steps;
}

void ObjectPacker::placeAtFirstFreeOffset(MemberRow member)
{
    uint32_t width = slotOf(member).width();
    uint32_t offset = 0;
    while (!canPlace(member, offset)) {
        offset += width;
        RELEASE_ASSERT(offset < areaOf(member).limit);
    }
    place(member, offset);
}

void ObjectPacker::placeInOneGranule(MemberRow first, MemberRow second)
{
    const Area& area = areaOf(first);
    for (uint32_t offset = 0; offset + extensionGranuleSize <= area.limit; offset += extensionGranuleSize) {
        if (!area.isFree(offset, extensionGranuleSize) || !canPlace(first, offset))
            continue;
        place(first, offset);
        uint32_t besideFirst = offset + slotOf(first).width();
        if (canPlace(second, besideFirst))
            place(second, besideFirst);
        else
            placeAtFirstFreeOffset(second);
        return;
    }
    placeAtFirstFreeOffset(first);
    placeAtFirstFreeOffset(second);
}

void ObjectPacker::placeSharedRows(const BitVector& setsToPlaceFirst)
{
    for (const PlacementStep& step : collectSteps(setsToPlaceFirst)) {
        std::span<MemberRow> rows = m_sharedRows.mutableSpan().subspan(step.firstSharedRow, step.numberOfSharedRows);
        // A shared location is a unit of fields, which a later step may widen or move. A hidden word stays as it is.
        if (step.placesTwoRowsInOneGranule)
            placeInOneGranule(rows[0], rows[1]);
        else if (step.placesRowByItself)
            placeAtFirstFreeOffset(rows[0]);
        else if (step.placesHiddenWords)
            placeAtSharedOffsets(rows);
        else
            placeTogether(rows, step.weight);
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
    m_rowSets = Vector<PackedRowSet>(m_context.rows.count());
    m_sharedRows.shrink(0);
    m_groupsInCells.shrink(0);
    for (PlacedSlot& slot : m_packed.slots)
        slot.offset = 0;
    forEachCompiledObject([&](LayoutID id) {
        m_inline[id.index()] = { };
        m_extension[id.index()] = { };
        initializeAreas(id);
    });
    placeExtensionWords();
}

// The heaviest set takes the lowest offset that its members have free, which may be the only one that a small member of a lighter set has.
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
    forEachCompiledObject([&](LayoutID id) {
        m_packed.inlineLimits[id.index()] = m_inline[id.index()].limit;
    });
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
    AOT::sort(memberRowsOf(request), { }, [&](MemberRow member) {
        return std::tuple(slotOf(member).storage, slotOf(member).representation, member.row);
    });
}

bool ObjectPacker::canPlace(MemberRow member, uint32_t offset) const
{
    return areaOf(member).canPlace(offset, logSizeOf(member));
}

void ObjectPacker::place(MemberRow member, uint32_t offset)
{
    areaOf(member).place(offset, logSizeOf(member));
    m_packed.slots[member.row].offset = offset;
    m_isPlaced[member.row] = true;
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

bool ObjectPacker::canPlaceFamily(std::span<const MemberRow> family, uint32_t offset) const
{
    return std::ranges::all_of(family, [&](MemberRow member) {
        return canPlace(member, offset);
    });
}

void ObjectPacker::tryPlaceRowsAt(Vector<MemberRow, 8>& unplacedByFamily, uint32_t offset)
{
    Vector<MemberRow, 8> stillUnplaced;
    forEachFamily(unplacedByFamily.span(), [&](std::span<const MemberRow> family) {
        if (!canPlaceFamily(family, offset)) {
            stillUnplaced.append(family);
            return;
        }
        for (MemberRow member : family)
            place(member, offset);
    });
    unplacedByFamily = WTF::move(stillUnplaced);
}

std::optional<uint32_t> ObjectPacker::findMostSharedOffset(std::span<const MemberRow> rowsByFamily) const
{
    const Vector<Area>& areas = areasOf(rowsByFamily[0]);
    unsigned logSize = logSizeOf(rowsByFamily[0]);
    uint32_t first = UINT32_MAX;
    Vector<uint32_t, 8> limitsDescending;
    for (MemberRow member : rowsByFamily) {
        const Area& area = areas[member.layout.index()];
        first = std::min(first, findFreeOffset(area.occupied, logSize, 0, area.limit).value_or(UINT32_MAX));
        limitsDescending.append(area.limit);
    }
    AOT::sort(limitsDescending, std::ranges::greater { });
    std::optional<uint32_t> best;
    size_t mostRows = 1;
    for (uint32_t offset = first; mostRows < rowsByFamily.size() && offset < limitsDescending[mostRows]; offset += 1u << logSize) {
        size_t numberOfRows = 0;
        forEachFamily(rowsByFamily, [&](std::span<const MemberRow> family) {
            if (canPlaceFamily(family, offset))
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
void ObjectPacker::placeFamilyInGrownAreas(std::span<const MemberRow> family)
{
    unsigned logSize = logSizeOf(family[0]);
    uint32_t size = 1u << logSize;
    uint32_t offset = 0;
    auto isFreeInEveryMember = [&] {
        return std::ranges::all_of(family, [&](MemberRow member) {
            return areaOf(member).isFree(offset, size);
        });
    };
    while (!isFreeInEveryMember())
        offset += size;
    for (MemberRow member : family) {
        Area& area = areaOf(member);
        while (!area.canPlace(offset, logSize))
            area.growTo(cellSizeFor(area.limit + 1));
        if (!slotOf(member).isOutOfLine())
            m_packed.inlineLimits[member.layout.index()] = area.limit;
        place(member, offset);
    }
}

// Only the cells of layouts that are made seldom grow, by half of their size at the most.
uint32_t ObjectPacker::maxLimit(MemberRow member) const
{
    uint32_t limit = areaOf(member).limit;
    if (!m_context.isMadeSeldom(member.layout))
        return limit;
    uint32_t limitBeforeGrowth = m_limitsBeforeGrowth[member.layout.index()];
    return std::max(limit, limitBeforeGrowth + limitBeforeGrowth / 2);
}

std::optional<uint32_t> ObjectPacker::limitThatTakes(MemberRow member, uint32_t offset) const
{
    const Area& area = areaOf(member);
    unsigned logSize = logSizeOf(member);
    if (area.canPlace(offset, logSize))
        return area.limit;
    uint32_t cap = maxLimit(member);
    if (area.limit == cap)
        return std::nullopt;
    Area grown = area;
    while (!grown.canPlace(offset, logSize)) {
        uint32_t limit = cellSizeFor(grown.limit + 1);
        if (limit > cap)
            return std::nullopt;
        grown.growTo(limit);
    }
    return grown.limit;
}

std::optional<uint32_t> ObjectPacker::growthByPlacingAt(std::span<const MemberRow> rows, uint32_t offset) const
{
    uint32_t growth = 0;
    for (MemberRow member : rows) {
        std::optional<uint32_t> limit = limitThatTakes(member, offset);
        if (!limit)
            return std::nullopt;
        growth += *limit - areaOf(member).limit;
    }
    return growth;
}

// One load reads the field in every layout of the set, without a dispatch on the shape. The offset is the one for which the cells grow least.
bool ObjectPacker::tryPlaceAtOneOffset(std::span<const MemberRow> unplaced, std::span<const uint32_t> offsetsInUse)
{
    unsigned logSize = logSizeOf(unplaced[0]);
    uint32_t size = 1u << logSize;
    std::optional<uint32_t> best;
    uint32_t leastGrowth = 0;
    auto consider = [&](uint32_t offset) {
        bool isFreeInEveryMember = std::ranges::all_of(unplaced, [&](MemberRow member) {
            return areaOf(member).isFree(offset, size);
        });
        if (!isFreeInEveryMember)
            return;
        auto growth = growthByPlacingAt(unplaced, offset);
        if (!growth || (best && *growth >= leastGrowth))
            return;
        best = offset;
        leastGrowth = *growth;
    };
    if (!offsetsInUse.empty())
        consider(offsetsInUse[0]);
    else {
        uint32_t end = maxInlineSizeAtCreation;
        for (MemberRow member : unplaced)
            end = std::min(end, maxLimit(member));
        for (uint32_t offset = 0; offset + size <= end && (!best || leastGrowth); offset += size)
            consider(offset);
    }
    if (!best)
        return false;
    for (MemberRow member : unplaced) {
        Area& area = areaOf(member);
        while (!area.canPlace(*best, logSize))
            area.growTo(cellSizeFor(area.limit + 1));
        m_packed.inlineLimits[member.layout.index()] = area.limit;
        place(member, *best);
    }
    return true;
}

void ObjectPacker::placeAtSharedOffsets(std::span<const MemberRow> rows, bool mayGrowCells)
{
    Vector<MemberRow, 8> unplaced;
    Vector<uint32_t, 8> offsets;
    for (MemberRow member : rows) {
        if (m_isPlaced[member.row])
            offsets.append(slotOf(member).offset);
        else
            unplaced.append(member);
    }
    if (unplaced.isEmpty())
        return;
    AOT::sort(unplaced, { }, [&](MemberRow member) {
        return std::pair(m_context.inheritedRows.declaringRowOf(member.row), member.row);
    });
    if (mayGrowCells && !slotOf(unplaced[0]).isOutOfLine()) {
        AOT::sort(offsets);
        offsets.shrink(std::ranges::unique(offsets).begin() - offsets.begin());
        if (offsets.size() <= 1 && tryPlaceAtOneOffset(unplaced.span(), offsets.span()))
            return;
    }
    for (OffsetUse use : countOffsetUses(WTF::move(offsets)))
        tryPlaceRowsAt(unplaced, use.offset);
    while (unplaced.size() >= 2) {
        auto offset = findMostSharedOffset(unplaced.span());
        if (!offset)
            break;
        tryPlaceRowsAt(unplaced, *offset);
    }
    forEachFamily(unplaced.span(), [&](std::span<const MemberRow> family) {
        if (family.size() >= 2)
            placeFamilyInGrownAreas(family);
    });
}

void ObjectPacker::unite(std::span<const MemberRow> rows)
{
    for (MemberRow member : rows.subspan(1))
        m_rowSets[member.row].unify(&m_rowSets[rows[0].row]);
}

void ObjectPacker::placeTogether(std::span<MemberRow> rows, bool mayGrowCells)
{
    placeAtSharedOffsets(rows, mayGrowCells);
    forEachSharedOffset(rows, [&](std::span<MemberRow> rowsAtOffset) {
        unite(rowsAtOffset);
    });
}

void ObjectPacker::placeOutOfLineRowsByWeight(LayoutID id)
{
    Vector<SlotRow, 16> rows;
    for (SlotRow row : rowsOf(m_context.rows, id)) {
        if (m_packed.slots[row].isOutOfLine() && !m_isPlaced[row])
            rows.append(row);
    }
    AOT::sort(rows, [&](SlotRow a, SlotRow b) {
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
            if (!slotOf(member).hasStorage() || logSizeOf(member) != logSize || m_isPlaced[row])
                continue;
            const Area& area = areaOf(member);
            auto offset = findFreeOffset(area.occupied, logSize, 0, area.limit);
            RELEASE_ASSERT(offset);
            place(member, *offset);
        }
    }
}

void ObjectPacker::finishLayout(LayoutID id)
{
    uint32_t inlineEnd = offsetOfFirstField;
    uint32_t extensionSize = 0;
    for (SlotRow row : rowsOf(m_context.rows, id)) {
        const PlacedSlot& slot = m_packed.slots[row];
        if (slot.storage == SlotStorage::Inline)
            inlineEnd = std::max(inlineEnd, slot.end());
        else if (slot.isOutOfLine())
            extensionSize = std::max(extensionSize, slot.end());
    }
    if (!extensionSize)
        m_packed.slots[m_context.rows.extensionWordOf(id)] = PlacedSlot { };
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
        return !first.isOutOfLine() || extensionWordOf(member).hasLocationOf(extensionWordOf(rows[0]));
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
    return words;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
