/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageTestSupport.h"
#include "AOTSectionLinker.h"
#include "AOTUnitPlacement.h"
#include "AOTVector.h"
#include "GPRInfo.h"
#include <algorithm>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace ImageTests {

static constexpr uint32_t sizeOfPlacedUnit = 10;
static constexpr size_t numberOfPlacedUnits = 30;
static constexpr int64_t placementReach = 100;
static_assert(sizeOfPlacedUnit * numberOfPlacedUnits == 3 * placementReach);
static constexpr uint32_t unitAlignment = 4;
static constexpr size_t sizeOfPlacementLine = 64;
static constexpr uint32_t symbolWithoutUnit = UINT32_MAX;
static constexpr int64_t largestFiller = 16 * MB;

namespace {

struct PlacementCase {
    UnitPlacement place() const;

    Vector<size_t> sizes { };
    Vector<Vector<CopyableUse>> usesByUnit { };
    Vector<CopyableUnit> copyables { };
    Vector<Vector<CopyableUse>> usesByCopyable { };
    CopyPolicy policy { CopyPolicy::BeforeUser };
    Vector<size_t> alignedOffsets { };
    size_t alignment { 1 };
    BitVector fallsIntoNext { };
    size_t shiftOfFirstUnit { 0 };
    Vector<Vector<RangeInUnit>> rangesByUnit { };
};

struct ShortJump {
    Vector<uint8_t> bytes;
    LinkedSite site;
};

struct ExpectedSection {
    explicit ExpectedSection(const LinkedSection& section)
        : bytes(FillWith { }, section.bytes.size(), 0)
    {
        size_t sizeOfBreakpoint = testTemplates().breakpoint.size();
        testTemplates().fillWithBreakpoints(bytes.mutableSpan().first(section.dataOffset - section.dataOffset % sizeOfBreakpoint));
    }

    void place(const LinkUnit&, size_t offset, std::initializer_list<size_t> targets = { });
    void placeVeneer(size_t offset, size_t target);

    Vector<uint8_t> bytes;
};

using LinkResult = std::expected<LinkedSection, SectionLinkFailure>;

} // anonymous namespace

static void appendRows(const Vector<Vector<CopyableUse>>& rows, size_t numberOfRows, Vector<CopyableUse>& uses, Vector<uint32_t>& firstUseByRow)
{
    for (size_t row = 0; row < numberOfRows; ++row) {
        firstUseByRow.append(static_cast<uint32_t>(uses.size()));
        if (row < rows.size())
            uses.appendVector(rows[row]);
    }
    firstUseByRow.append(static_cast<uint32_t>(uses.size()));
}

UnitPlacement PlacementCase::place() const
{
    Vector<CopyableUse> uses;
    Vector<CopyableUse> usesOfCopyables;
    Vector<uint32_t> firstUseByUnit;
    Vector<uint32_t> firstUseByCopyable;
    appendRows(usesByUnit, sizes.size(), uses, firstUseByUnit);
    appendRows(usesByCopyable, copyables.size(), usesOfCopyables, firstUseByCopyable);
    Vector<size_t> offsets = alignedOffsets.isEmpty() ? Vector<size_t>(FillWith { }, sizes.size(), 0) : alignedOffsets;
    Vector<RangeInUnit> ranges;
    Vector<uint32_t> firstRangeByUnit;
    for (size_t unit = 0; unit < sizes.size(); ++unit) {
        firstRangeByUnit.append(static_cast<uint32_t>(ranges.size()));
        if (unit < rangesByUnit.size())
            ranges.appendVector(rangesByUnit[unit]);
    }
    firstRangeByUnit.append(static_cast<uint32_t>(ranges.size()));
    return placeUnits({
        .sizes = sizes.span(),
        .alignedOffsets = offsets.span(),
        .alignments = singleElementSpan(alignment),
        .fallsIntoNext = fallsIntoNext,
        .copyables = copyables.span(),
        .uses = uses.span(),
        .firstUseByUnit = firstUseByUnit.span(),
        .usesOfCopyables = usesOfCopyables.span(),
        .firstUseByCopyable = firstUseByCopyable.span(),
        .alignmentAfterCopies = 1,
        .start = 0,
        .shiftOfFirstUnit = shiftOfFirstUnit,
        .policy = policy,
        .rangesKeptInLine = ranges.span(),
        .firstRangeByUnit = firstRangeByUnit.span(),
        .lineSize = sizeOfPlacementLine,
    });
}

static bool isSamePlacement(const UnitPlacement& first, const UnitPlacement& second)
{
    return first.unitStarts == second.unitStarts && first.copyOffsetByUse == second.copyOffsetByUse && first.end == second.end
        && std::ranges::equal(first.copies, second.copies, [](const PlacedCopy& a, const PlacedCopy& b) {
            return a.copyable == b.copyable && a.offset == b.offset && a.copyOffsetByUse == b.copyOffsetByUse;
        });
}

static size_t numberOfCopiesOf(const UnitPlacement& placement, uint32_t copyable)
{
    return std::ranges::count(placement.copies, copyable, &PlacedCopy::copyable);
}

static void checkUsesAreInReach(const PlacementCase& inputs, const UnitPlacement& placement)
{
    size_t useIndex = 0;
    for (size_t unit = 0; unit < inputs.sizes.size(); ++unit) {
        size_t start = placement.unitStarts[unit];
        for (const CopyableUse& use : inputs.usesByUnit[unit]) {
            size_t copy = placement.copyOffsetByUse[useIndex++];
            bool isPlaced = placement.copies.containsIf([&](const PlacedCopy& placed) {
                return placed.copyable == use.copyable && placed.offset == copy;
            });
            AOT_TEST_CHECK(isPlaced);
            size_t distance = copy < start ? start + use.lastSite - copy : copy + inputs.copyables[use.copyable].size - (start + use.firstSite);
            AOT_TEST_CHECK(static_cast<int64_t>(distance) < use.reach);
        }
    }
}

static void testRangeIsKeptInOneLine()
{
    // 60 up to 80 would straddle the line that ends at 64, and 94 + 10 up to 94 + 30 straddles none.
    PlacementCase inputs { .sizes = { 50, 40, 40 }, .rangesByUnit = { { }, { { 10, 30 } }, { { 10, 30 } } } };
    UnitPlacement placement = inputs.place();
    AOT_TEST_CHECK(placement.unitStarts == Vector<size_t>({ 0, 54, 94 }));
    AOT_TEST_CHECK(placement.end == 134);

    // The aligned offset stays aligned: 54 + 2 is a multiple of 4, and 64 up to 84 lies in one line.
    inputs.alignment = unitAlignment;
    inputs.alignedOffsets = { 0, 2, 2 };
    placement = inputs.place();
    AOT_TEST_CHECK(placement.unitStarts[1] == 54);
    AOT_TEST_CHECK(!((placement.unitStarts[1] + 2) % unitAlignment));

    // A range as long as a line begins with a line.
    PlacementCase whole { .sizes = { 10, 100 }, .rangesByUnit = { { }, { { 6, 70 } } } };
    AOT_TEST_CHECK(whole.place().unitStarts[1] == 58);
}

static void testFirstRangeGoesFirst()
{
    // 0 up to 40 and 40 up to 80 lie in no two lines of 64 bytes at once unless the unit begins 24 bytes into a line.
    PlacementCase both { .sizes = { 30, 80 }, .rangesByUnit = { { }, { { 0, 40 }, { 40, 80 } } } };
    AOT_TEST_CHECK(both.place().unitStarts[1] == 88);

    // 0 up to 60 and 30 up to 90 cannot both be kept. The first one is.
    PlacementCase one { .sizes = { 30, 90 }, .rangesByUnit = { { }, { { 0, 60 }, { 30, 90 } } } };
    AOT_TEST_CHECK(one.place().unitStarts[1] == 64);
}

static void testUnitThatIsFallenIntoStays()
{
    PlacementCase inputs { .sizes = { 50, 40 }, .rangesByUnit = { { }, { { 10, 30 } } } };
    inputs.fallsIntoNext.set(0);
    AOT_TEST_CHECK(inputs.place().unitStarts == Vector<size_t>({ 0, 50 }));
}

static ShortJump assembleShortJump()
{
    CCallHelpers jit;
    RelocationRecorder relocations(jit, testAssemblerPolicy());
    CCallHelpers::Jump jump = jit.branchTest32(CCallHelpers::NonZero, GPRInfo::regT0, CCallHelpers::TrustedImm32(1));
    jit.breakpoint();
    jump.link(&jit);
    jit.ret();
    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::Thunk);
    AOT_TEST_CHECK(!buffer.didFailToAllocate());
    buffer.finalize(false, "test"_s);
    AOT_TEST_CHECK(relocations.link(buffer, nullptr));
    const Vector<LinkedSite>& sites = buffer.linkBuffer().linkedSites();
    AOT_TEST_CHECK(sites.size() == 1);
    AOT_TEST_CHECK(transfersControl(sites[0].kind));
    return { Vector<uint8_t>(buffer.bytes()), sites[0] };
}

static RelocatableCode shortJumpsTo(std::initializer_list<uint32_t> units)
{
    ShortJump jump = assembleShortJump();
    RelocatableCode code;
    for (uint32_t unit : units) {
        code.relocations.append(Relocation { static_cast<uint32_t>(code.bytes.size()) + jump.site.offset, jump.site.kind, SymbolSpace::Function, primaryEntry, unit });
        code.bytes.appendVector(jump.bytes);
    }
    return code;
}

static RelocatableCode callsTo(std::initializer_list<uint32_t> units)
{
    RelocatableCode code;
    for (uint32_t unit : units) {
        code.relocations.append(Relocation { static_cast<uint32_t>(code.bytes.size()), RelocationKind::Call, SymbolSpace::Function, primaryEntry, unit });
        code.bytes.appendVector(testTemplates().nearCall);
    }
    code.bytes.appendVector(testTemplates().returnToCaller);
    return code;
}

static RelocatableCode addressOf(uint32_t unit, PCRelativeReach reach)
{
    CCallHelpers jit;
    RelocationRecorder recorder(jit, testAssemblerPolicy());
    recorder.moveAddress({ SymbolSpace::Constant, unit }, GPRInfo::returnValueGPR, reach);
    jit.ret();
    auto code = recorder.finalize(LinkBuffer::Profile::Thunk, nullptr);
    AOT_TEST_CHECK(code);
    AOT_TEST_CHECK(code->relocations.size() == 1);
    return WTF::move(*code);
}

static bool canFillReachOf(RelocationKind kind)
{
    return MacroAssembler::relocationReach(kind) <= largestFiller;
}

static Vector<uint8_t> tableBytes()
{
    return { 1, 2, 3, 4, 5, 6, 7, 8 };
}

static Vector<uint8_t> fillerAsLargeAsReachOf(RelocationKind kind)
{
    AOT_TEST_CHECK(canFillReachOf(kind));
    return Vector<uint8_t>(FillWith { }, static_cast<size_t>(MacroAssembler::relocationReach(kind)), 0);
}

static Vector<uint8_t> fillerOutOfShortReach()
{
    return fillerAsLargeAsReachOf(assembleShortJump().site.kind);
}

static LinkUnit unitOf(const RelocatableCode& code, UnitSharing sharing = UnitSharing::Unique)
{
    return { code.bytes.span(), code.relocations.span(), unitAlignment, sharing };
}

static LinkUnit unitOf(const Vector<uint8_t>& bytes, UnitSharing sharing = UnitSharing::Unique)
{
    return { bytes.span(), { }, unitAlignment, sharing };
}

static size_t alignUnit(size_t offset)
{
    return WTF::roundUpToMultipleOf<unitAlignment>(offset);
}

static LinkResult link(const Vector<LinkUnit>& units, uint64_t maxSize = UINT32_MAX)
{
    auto findUnit = [](uint32_t, const Relocation& relocation) -> std::optional<UnitTarget> {
        if (relocation.symbol == symbolWithoutUnit)
            return std::nullopt;
        return UnitTarget { relocation.symbol };
    };
    UnitResolver resolve = findUnit;
    return linkSection({ units.span(), resolve, testTemplates(), maxSize });
}

static bool hasFailed(const LinkResult& result, SectionLinkError error, uint32_t unit, uint32_t relocation)
{
    return !result && result.error().error == error && result.error().unit == unit && result.error().relocation == relocation;
}

void ExpectedSection::place(const LinkUnit& unit, size_t offset, std::initializer_list<size_t> targets)
{
    AOT_TEST_CHECK(targets.size() == unit.relocations.size());
    memcpySpan(bytes.mutableSpan().subspan(offset), unit.bytes);
    size_t index = 0;
    for (size_t target : targets)
        AOT_TEST_CHECK(applyRelocation(bytes.mutableSpan(), offset, unit.relocations[index++], target));
}

void ExpectedSection::placeVeneer(size_t offset, size_t target)
{
    const RelocatableCode& veneer = testTemplates().veneer;
    memcpySpan(bytes.mutableSpan().subspan(offset), veneer.bytes.span());
    AOT_TEST_CHECK(applyRelocation(bytes.mutableSpan(), offset, veneer.relocations[0], target));
}

static PlacementCase unitsUsingCopyablesWithReaches(std::initializer_list<int64_t> reaches)
{
    PlacementCase result { .policy = CopyPolicy::AfterUsers };
    Vector<CopyableUse> uses;
    for (int64_t reach : reaches) {
        uses.append(CopyableUse { static_cast<uint32_t>(result.copyables.size()), 0, sizeOfPlacedUnit, reach });
        result.copyables.append(CopyableUnit { 4, 1 });
    }
    result.sizes.fill(sizeOfPlacedUnit, numberOfPlacedUnits);
    result.usesByUnit.fill(uses, numberOfPlacedUnits);
    return result;
}

static void testCopyBeforeFirstUser()
{
    CopyableUse use { 0, 0, 16, 1000 };
    PlacementCase inputs { .sizes = { 16, 16, 16 }, .usesByUnit = { { }, { use }, { use } }, .copyables = { { 8, 1 } } };
    AOT_TEST_CHECK(isSamePlacement(inputs.place(), { .unitStarts = { 0, 24, 40 }, .copyOffsetByUse = { 16, 16 }, .copies = { { 0, 16, { } } }, .end = 56 }));
}

static void testSecondCopyBeforeUnitOutOfReach()
{
    CopyableUse use { 0, 0, 16, 64 };
    PlacementCase inputs { .sizes = { 16, 100, 16 }, .usesByUnit = { { use }, { }, { use } }, .copyables = { { 8, 1 } } };
    AOT_TEST_CHECK(isSamePlacement(inputs.place(), { .unitStarts = { 8, 24, 132 }, .copyOffsetByUse = { 0, 124 }, .copies = { { 0, 0, { } }, { 0, 124, { } } }, .end = 148 }));
}

static void testRoutineBringsTable()
{
    PlacementCase inputs {
        .sizes = { 16 },
        .usesByUnit = { { CopyableUse { 1, 0, 16, 1000 } } },
        .copyables = { { 8, 1 }, { 16, 1 } },
        .usesByCopyable = { { }, { CopyableUse { 0, 0, 16, 1000 } } },
    };
    AOT_TEST_CHECK(isSamePlacement(inputs.place(), { .unitStarts = { 24 }, .copyOffsetByUse = { 8 }, .copies = { { 0, 0, { } }, { 1, 8, { 0 } } }, .end = 40 }));
}

static void testCycleOfCopyablesEnds()
{
    PlacementCase inputs {
        .sizes = { 16 },
        .usesByUnit = { { CopyableUse { 0, 0, 16, 1000 } } },
        .copyables = { { 8, 1 }, { 8, 1 } },
        .usesByCopyable = { { CopyableUse { 1, 0, 8, 1000 } }, { CopyableUse { 0, 0, 8, 1000 } } },
    };
    AOT_TEST_CHECK(isSamePlacement(inputs.place(), { .unitStarts = { 16 }, .copyOffsetByUse = { 8 }, .copies = { { 1, 0, { 8 } }, { 0, 8, { 0 } } }, .end = 32 }));
}

static void testTwoCopiesForThreeTimesReach()
{
    PlacementCase inputs = unitsUsingCopyablesWithReaches({ placementReach });
    UnitPlacement placement = inputs.place();
    AOT_TEST_CHECK(placement.copies.size() == 2);
    AOT_TEST_CHECK(placement.copies[0].offset > placement.unitStarts[0]);
    AOT_TEST_CHECK(placement.copies[0].offset < placement.unitStarts.last());
    checkUsesAreInReach(inputs, placement);
}

static void testCopyablesWithDifferentReaches()
{
    PlacementCase inputs = unitsUsingCopyablesWithReaches({ placementReach, 10 * placementReach });
    UnitPlacement placement = inputs.place();
    AOT_TEST_CHECK(numberOfCopiesOf(placement, 0) == 2);
    AOT_TEST_CHECK(numberOfCopiesOf(placement, 1) == 1);
    checkUsesAreInReach(inputs, placement);
}

static void testRunIsNotSplit()
{
    constexpr size_t firstOfRun = 6;
    constexpr size_t lastOfRun = 12;
    PlacementCase inputs = unitsUsingCopyablesWithReaches({ placementReach });
    AOT_TEST_CHECK(inputs.place().copies[0].offset == 9 * sizeOfPlacedUnit);
    for (size_t unit = firstOfRun; unit < lastOfRun; ++unit)
        inputs.fallsIntoNext.set(unit);
    UnitPlacement placement = inputs.place();
    AOT_TEST_CHECK(placement.copies[0].offset == firstOfRun * sizeOfPlacedUnit);
    for (size_t unit = firstOfRun; unit < lastOfRun; ++unit)
        AOT_TEST_CHECK(placement.unitStarts[unit + 1] == placement.unitStarts[unit] + sizeOfPlacedUnit);
    checkUsesAreInReach(inputs, placement);
}

static void checkAlignedOffsets(CopyPolicy policy)
{
    CopyableUse use { 0, 0, 5, 1000 };
    PlacementCase inputs {
        .sizes = { 5, 7, 9 },
        .usesByUnit = { { use }, { }, { use } },
        .copyables = { { 3, 2 } },
        .policy = policy,
        .alignedOffsets = { 3, 1, 0 },
        .alignment = 8,
        .shiftOfFirstUnit = 16,
    };
    UnitPlacement placement = inputs.place();
    AOT_TEST_CHECK(placement.unitStarts[0] >= inputs.shiftOfFirstUnit);
    for (size_t unit = 0; unit < inputs.sizes.size(); ++unit) {
        AOT_TEST_CHECK(!((placement.unitStarts[unit] + inputs.alignedOffsets[unit]) % inputs.alignment));
        AOT_TEST_CHECK(!unit || placement.unitStarts[unit] >= placement.unitStarts[unit - 1] + inputs.sizes[unit - 1]);
    }
    AOT_TEST_CHECK(placement.copies.size() == 1);
    AOT_TEST_CHECK(!(placement.copies[0].offset % 2));
    checkUsesAreInReach(inputs, placement);
}

static void testEqualInputsEqualPlacements()
{
    PlacementCase inputs = unitsUsingCopyablesWithReaches({ placementReach, 10 * placementReach });
    AOT_TEST_CHECK(isSamePlacement(inputs.place(), inputs.place()));
    UnitPlacement afterUsers = inputs.place();
    inputs.policy = CopyPolicy::BeforeUser;
    AOT_TEST_CHECK(isSamePlacement(inputs.place(), inputs.place()));
    AOT_TEST_CHECK(!isSamePlacement(inputs.place(), afterUsers));
}

static void testUnitsCallEachOther()
{
    RelocatableCode first = callsTo({ 1 });
    RelocatableCode second = callsTo({ 0 });
    Vector<LinkUnit> units { unitOf(first), unitOf(second) };
    LinkResult section = link(units);
    AOT_TEST_CHECK(section);
    uint32_t secondStart = static_cast<uint32_t>(alignUnit(first.bytes.size()));
    AOT_TEST_CHECK(section->uniqueUnitOffsets == Vector<uint32_t>({ 0, secondStart }));
    AOT_TEST_CHECK(section->bytes.size() == secondStart + second.bytes.size());
    AOT_TEST_CHECK(section->copies.isEmpty());
    AOT_TEST_CHECK(!section->numberOfVeneers);

    ExpectedSection expected(*section);
    expected.place(units[0], 0, { secondStart });
    expected.place(units[1], secondStart, { 0 });
    AOT_TEST_CHECK(section->bytes == expected.bytes);
}

static void testOneVeneerPerUnitAndTarget()
{
    RelocatableCode first = shortJumpsTo({ 3, 3 });
    RelocatableCode second = shortJumpsTo({ 3 });
    Vector<uint8_t> filler = fillerOutOfShortReach();
    Vector<LinkUnit> units { unitOf(first), unitOf(second), unitOf(filler), unitOf(testTemplates().returnToCaller) };
    LinkResult section = link(units);
    AOT_TEST_CHECK(section);
    AOT_TEST_CHECK(section->numberOfVeneers == 2);
    AOT_TEST_CHECK(section->copies.isEmpty());

    const Vector<uint32_t>& offsets = section->uniqueUnitOffsets;
    size_t firstVeneer = offsets[0] + alignUnit(first.bytes.size());
    size_t secondVeneer = offsets[1] + alignUnit(second.bytes.size());
    AOT_TEST_CHECK(offsets[1] == alignUnit(firstVeneer + testTemplates().sizeOfVeneer()));
    AOT_TEST_CHECK(offsets[2] == alignUnit(secondVeneer + testTemplates().sizeOfVeneer()));
    ExpectedSection expected(*section);
    expected.place(units[0], offsets[0], { firstVeneer, firstVeneer });
    expected.placeVeneer(firstVeneer, offsets[3]);
    expected.place(units[1], offsets[1], { secondVeneer });
    expected.placeVeneer(secondVeneer, offsets[3]);
    expected.place(units[2], offsets[2]);
    expected.place(units[3], offsets[3]);
    AOT_TEST_CHECK(section->bytes == expected.bytes);
}

static void checkSecondCopyOutOfReach(const RelocatableCode& user, const Vector<uint8_t>& copyable)
{
    Vector<uint8_t> filler = fillerAsLargeAsReachOf(user.relocations[0].kind);
    Vector<LinkUnit> units { unitOf(user), unitOf(filler), unitOf(user), unitOf(copyable, UnitSharing::Copyable) };
    LinkResult section = link(units);
    AOT_TEST_CHECK(section);
    AOT_TEST_CHECK(!section->numberOfVeneers);
    AOT_TEST_CHECK(section->copies.size() == 2);
    AOT_TEST_CHECK(section->copies[0].unit == 3 && section->copies[1].unit == 3);

    const Vector<uint32_t>& offsets = section->uniqueUnitOffsets;
    size_t sizeOfCopy = units[3].bytes.size();
    size_t secondCopy = section->copies[1].offset;
    AOT_TEST_CHECK(!offsets[3]);
    AOT_TEST_CHECK(!section->copies[0].offset);
    AOT_TEST_CHECK(offsets[0] == alignUnit(sizeOfCopy));
    AOT_TEST_CHECK(secondCopy == alignUnit(offsets[1] + filler.size()));
    AOT_TEST_CHECK(offsets[2] == alignUnit(secondCopy + sizeOfCopy));
    ExpectedSection expected(*section);
    expected.place(units[3], 0);
    expected.place(units[0], offsets[0], { 0 });
    expected.place(units[1], offsets[1]);
    expected.place(units[3], secondCopy);
    expected.place(units[2], offsets[2], { secondCopy });
    AOT_TEST_CHECK(section->bytes == expected.bytes);
}

static void testSecondCopyOfTableOutOfReach()
{
    if (canFillReachOf(RelocationKind::AddressNear))
        checkSecondCopyOutOfReach(addressOf(3, PCRelativeReach::Near), tableBytes());
}

static void testEveryCopyNamesUniqueTable()
{
    Vector<uint8_t> table = tableBytes();
    RelocatableCode user = shortJumpsTo({ 4 });
    Vector<uint8_t> filler = fillerOutOfShortReach();
    RelocatableCode routine = addressOf(0, PCRelativeReach::Far);
    Vector<LinkUnit> units { unitOf(table), unitOf(user), unitOf(filler), unitOf(user), unitOf(routine, UnitSharing::Copyable) };
    LinkResult section = link(units);
    AOT_TEST_CHECK(section);
    AOT_TEST_CHECK(section->copies.size() == 2);

    const Vector<uint32_t>& offsets = section->uniqueUnitOffsets;
    size_t firstCopy = section->copies[0].offset;
    size_t secondCopy = section->copies[1].offset;
    AOT_TEST_CHECK(firstCopy < offsets[1] && offsets[2] < secondCopy && secondCopy < offsets[3]);
    ExpectedSection expected(*section);
    expected.place(units[0], offsets[0]);
    expected.place(units[4], firstCopy, { offsets[0] });
    expected.place(units[1], offsets[1], { firstCopy });
    expected.place(units[2], offsets[2]);
    expected.place(units[4], secondCopy, { offsets[0] });
    expected.place(units[3], offsets[3], { secondCopy });
    AOT_TEST_CHECK(section->bytes == expected.bytes);
}

static void testCopyOutOfReachOfUniqueUnit()
{
    RelocatableCode user = shortJumpsTo({ 3 });
    Vector<uint8_t> filler = fillerOutOfShortReach();
    RelocatableCode routine = shortJumpsTo({ 2 });
    Vector<LinkUnit> units { unitOf(user), unitOf(filler), unitOf(testTemplates().returnToCaller), unitOf(routine, UnitSharing::Copyable) };
    AOT_TEST_CHECK(hasFailed(link(units), SectionLinkError::OutOfPCRelativeReach, 3, 0));
}

static void testAddressOutOfReach()
{
    if (!canFillReachOf(RelocationKind::AddressNear))
        return;
    Vector<uint8_t> table = tableBytes();
    RelocatableCode nearUser = addressOf(0, PCRelativeReach::Near);
    RelocatableCode farUser = addressOf(0, PCRelativeReach::Near);
    Vector<uint8_t> filler = fillerAsLargeAsReachOf(RelocationKind::AddressNear);
    Vector<LinkUnit> units { unitOf(table), unitOf(nearUser), unitOf(filler), unitOf(farUser) };
    AOT_TEST_CHECK(hasFailed(link(units), SectionLinkError::OutOfPCRelativeReach, 3, 0));
}

static void testSymbolWithoutUnit()
{
    RelocatableCode unresolved = callsTo({ 0, symbolWithoutUnit });
    AOT_TEST_CHECK(hasFailed(link({ unitOf(unresolved) }), SectionLinkError::SymbolWithoutUnit, 0, 1));
    RelocatableCode resolved = callsTo({ 1 });
    RelocatableCode pastLastUnit = callsTo({ 2 });
    AOT_TEST_CHECK(hasFailed(link({ unitOf(resolved), unitOf(pastLastUnit) }), SectionLinkError::SymbolWithoutUnit, 1, 0));
}

static void testSectionTooLarge()
{
    RelocatableCode first = callsTo({ 1 });
    RelocatableCode second = callsTo({ 0 });
    Vector<LinkUnit> units { unitOf(first), unitOf(second) };
    LinkResult section = link(units);
    AOT_TEST_CHECK(section);
    AOT_TEST_CHECK(link(units, section->bytes.size()));
    AOT_TEST_CHECK(hasFailed(link(units, section->bytes.size() - 1), SectionLinkError::SectionTooLarge, 0, 0));
}

std::span<const NativeTest> unitPlacementTests()
{
    static constexpr NativeTest tests[] = {
        { "image/unit-placement/copy-before-first-user"_s, testCopyBeforeFirstUser },
        { "image/unit-placement/second-copy-before-unit-out-of-reach"_s, testSecondCopyBeforeUnitOutOfReach },
        { "image/unit-placement/routine-brings-table"_s, testRoutineBringsTable },
        { "image/unit-placement/cycle-of-copyables-ends"_s, testCycleOfCopyablesEnds },
        { "image/unit-placement/two-copies-for-three-times-reach"_s, testTwoCopiesForThreeTimesReach },
        { "image/unit-placement/copyables-with-different-reaches"_s, testCopyablesWithDifferentReaches },
        { "image/unit-placement/run-is-not-split"_s, testRunIsNotSplit },
        { "image/unit-placement/aligned-offsets-before-user"_s, [] { checkAlignedOffsets(CopyPolicy::BeforeUser); } },
        { "image/unit-placement/aligned-offsets-after-users"_s, [] { checkAlignedOffsets(CopyPolicy::AfterUsers); } },
        { "image/unit-placement/equal-inputs-equal-placements"_s, testEqualInputsEqualPlacements },
        { "image/unit-placement/range-is-kept-in-one-line"_s, testRangeIsKeptInOneLine },
        { "image/unit-placement/first-range-goes-first"_s, testFirstRangeGoesFirst },
        { "image/unit-placement/unit-that-is-fallen-into-stays"_s, testUnitThatIsFallenIntoStays },
        { "image/section-linker/units-call-each-other"_s, testUnitsCallEachOther },
        { "image/section-linker/one-veneer-per-unit-and-target"_s, testOneVeneerPerUnitAndTarget },
        { "image/section-linker/second-copy-of-routine-out-of-reach"_s, [] { checkSecondCopyOutOfReach(shortJumpsTo({ 3 }), testTemplates().returnToCaller); } },
        { "image/section-linker/second-copy-of-table-out-of-reach"_s, testSecondCopyOfTableOutOfReach },
        { "image/section-linker/every-copy-names-unique-table"_s, testEveryCopyNamesUniqueTable },
        { "image/section-linker/copy-out-of-reach-of-unique-unit"_s, testCopyOutOfReachOfUniqueUnit },
        { "image/section-linker/address-out-of-reach"_s, testAddressOutOfReach },
        { "image/section-linker/symbol-without-unit"_s, testSymbolWithoutUnit },
        { "image/section-linker/section-too-large"_s, testSectionTooLarge },
    };
    return tests;
}

} } } // namespace JSC::AOT::ImageTests

#endif // ENABLE(AOT)
