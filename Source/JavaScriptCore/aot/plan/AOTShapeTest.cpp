/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTShapeTest.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTPagedBitSet.h"
#include "AOTSlotPlan.h"
#include "AOTSort.h"
#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <wtf/BitSet.h>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t shapesPerBlockOfShapeTest = 256;
static constexpr uint32_t maxBlocksOfShapeTest = 128;
static constexpr uint32_t maxShapesForBlockShapeTest = shapesPerBlockOfShapeTest << 16;
static constexpr uint32_t firstShapeWithLongConstantInShapeTest = 4096;

static_assert(shapesPerBlockOfShapeTest == 1 << 8);
static_assert((AOT::shapeWordOf(1) & shapeBlockMarker) == shapeBlockMarker && (AOT::shapeWordOf(maxBlocksOfShapeTest * shapesPerBlockOfShapeTest) & shapeBlockMarker) == shapeBlockMarker);
static_assert(immediateBitsWindow <= 8 * sizeof(ShapeTest::bits));
static_assert(numberOfPagesInShapeSet(0) == 1 && numberOfPagesInShapeSet(PagedBitSet::bitsPerPage - 1) == 1 && numberOfPagesInShapeSet(PagedBitSet::bitsPerPage) == 2);

static constexpr bool shapeTestRefusesCellsWithoutShape(uint32_t first, uint32_t last)
{
    return first && (AOT::shapeWordOf(last) & compiledObjectShapeBit);
}
static_assert(!canPassShapeTest(AOT::shapeWordOf(0)) && !shapeTestRefusesCellsWithoutShape(0, 1));
static_assert(canPassShapeTest(AOT::shapeWordOf(1)) && canPassShapeTest(AOT::shapeWordOf(compiledObjectShapeBit)));
static_assert(shapeTestRefusesCellsWithoutShape(1, compiledObjectShapeBit) && !shapeTestRefusesCellsWithoutShape(1, compiledObjectShapeBit + 1));

#if ASSERT_ENABLED
static bool shapesAscendWithoutRepeats(std::span<const uint32_t> shapes)
{
    return std::ranges::adjacent_find(shapes, std::ranges::greater_equal { }) == shapes.end();
}
#endif

static constexpr unsigned numberOfNonzeroHalfwordsInShapeTest(uint64_t bits)
{
    unsigned halfwords = 0;
    for (unsigned shift = 0; shift < 64; shift += 16)
        halfwords += !!(bits >> shift & 0xffff);
    return halfwords;
}

static constexpr unsigned instructionsForShapeTest(const ShapeTest& test)
{
    unsigned longConstant = test.count && test.last() >= firstShapeWithLongConstantInShapeTest ? 1 : 0;
    switch (test.form) {
    case ShapeTestForm::NeverPasses:
    case ShapeTestForm::AlwaysPasses:
        return 0;
    case ShapeTestForm::OneShape:
    case ShapeTestForm::UpTo:
    case ShapeTestForm::From:
        return 2 + longConstant;
    case ShapeTestForm::Block:
    case ShapeTestForm::BlockOrEarlier:
        return 2;
    case ShapeTestForm::Range:
        return 3 + longConstant;
    case ShapeTestForm::ImmediateBits:
        return 5 + numberOfNonzeroHalfwordsInShapeTest(test.bits) + longConstant;
    case ShapeTestForm::BitSet:
        return 9 + (numberOfPagesInShapeSet(test.count) > maxPagesInShortShapeSet ? 1 : 0) + longConstant;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static ShapeTest makeShapeTest(ShapeTestForm form, uint32_t first, uint32_t last)
{
    return { .form = form, .first = first, .count = last - first + 1 };
}

static ShapeTest makeShapeTestWithImmediateBits(std::span<const uint32_t> ascendingShapes)
{
    ShapeTest test = makeShapeTest(ShapeTestForm::ImmediateBits, ascendingShapes.front(), ascendingShapes.back());
    for (uint32_t shape : ascendingShapes)
        test.bits |= 1ull << (test.last() - shape);
    return test;
}

void ShapeTable::appendMemberShapes(std::span<const LayoutID> layouts, ShapeList& ascendingShapes) const
{
    ASSERT(shapesAscendWithoutRepeats(ascendingShapes.span()));
    size_t numberOfShapes = ascendingShapes.size();
    for (LayoutID layout : layouts)
        numberOfShapes += shapesOf(layout).count;
    ascendingShapes.reserveCapacity(numberOfShapes);

    bool isAscending = true;
    for (LayoutID layout : layouts) {
        ShapeRange range = shapesOf(layout);
        if (range.isEmpty())
            continue;
        isAscending &= ascendingShapes.isEmpty() || ascendingShapes.last() < range.first;
        for (uint32_t shape = range.first; shape < range.end(); ++shape)
            ascendingShapes.append(shape);
    }
    if (isAscending)
        return;
    AOT::sort(ascendingShapes);
    ascendingShapes.shrink(std::ranges::unique(ascendingShapes).begin() - ascendingShapes.begin());
}

static bool someShapeIsAssignedBetween(const ShapeTable& table, uint32_t first, uint32_t last)
{
    for (uint64_t shape = first; shape <= last; ++shape) {
        if (table.isAssigned(static_cast<uint32_t>(shape)))
            return true;
    }
    return false;
}

static bool listsEveryAssignedShapeItReachesOver(const ShapeTable& table, std::span<const uint32_t> ascendingShapes)
{
    if (ascendingShapes.back() - ascendingShapes.front() + 1 == ascendingShapes.size())
        return true;
    for (size_t index = 1; index < ascendingShapes.size(); ++index) {
        if (someShapeIsAssignedBetween(table, ascendingShapes[index - 1] + 1, ascendingShapes[index] - 1))
            return false;
    }
    return true;
}

static std::optional<ShapeTest> tryMakeBlockShapeTest(const ShapeTable& table, std::span<const uint32_t> ascendingShapes)
{
    uint32_t first = ascendingShapes.front();
    uint32_t last = ascendingShapes.back();
    uint32_t block = (last - 1) / shapesPerBlockOfShapeTest;
    if (block >= maxBlocksOfShapeTest || ascendingShapes.size() < 2 || table.numberOfShapes() > maxShapesForBlockShapeTest)
        return std::nullopt;
    uint32_t lastBeforeBlock = block * shapesPerBlockOfShapeTest;
    uint32_t lastOfBlock = std::min(lastBeforeBlock + shapesPerBlockOfShapeTest, table.numberOfShapes());
    if (someShapeIsAssignedBetween(table, last + 1, lastOfBlock))
        return std::nullopt;
    ShapeTestForm form = ShapeTestForm::BlockOrEarlier;
    if (someShapeIsAssignedBetween(table, 1, first - 1)) {
        if (first <= lastBeforeBlock || someShapeIsAssignedBetween(table, lastBeforeBlock + 1, first - 1))
            return std::nullopt;
        form = ShapeTestForm::Block;
    }
    ShapeTest test = makeShapeTest(form, first, last);
    test.blockByte = static_cast<uint8_t>(AOT::shapeWordOf(last) >> 8);
    return test;
}

ShapeTest ShapeTable::testOfShapes(std::span<const uint32_t> ascendingShapes) const
{
    if (ascendingShapes.empty())
        return { };
    ASSERT(shapesAscendWithoutRepeats(ascendingShapes));
    ASSERT(std::ranges::all_of(ascendingShapes, [&](uint32_t shape) {
        return isAssigned(shape);
    }));
    uint32_t first = ascendingShapes.front();
    uint32_t last = ascendingShapes.back();
    RELEASE_ASSERT(first <= last && last <= numberOfShapes() && shapeTestRefusesCellsWithoutShape(first, last));

    if (!listsEveryAssignedShapeItReachesOver(*this, ascendingShapes)) {
        if (last - first < immediateBitsWindow)
            return makeShapeTestWithImmediateBits(ascendingShapes);
        return makeShapeTest(ShapeTestForm::BitSet, first, last);
    }

    Vector<ShapeTest, 4> candidates;
    if (ascendingShapes.size() == 1)
        candidates.append(makeShapeTest(ShapeTestForm::OneShape, first, last));
    if (std::optional<ShapeTest> blockTest = tryMakeBlockShapeTest(*this, ascendingShapes))
        candidates.append(*blockTest);
    if (!someShapeIsAssignedBetween(*this, 1, first - 1))
        candidates.append(makeShapeTest(ShapeTestForm::UpTo, first, last));
    candidates.append(makeShapeTest(ShapeTestForm::Range, first, last));
    return *std::ranges::min_element(candidates, { }, [](const ShapeTest& test) {
        return instructionsForShapeTest(test);
    });
}

static ShapeTest shortenShapeTestForKnownShapes(const ShapeTest& general, std::span<const uint32_t> tested, std::span<const uint32_t> known)
{
    ShapeTable::ShapeList passing;
    size_t firstPosition = 0;
    size_t lastPosition = 0;
    bool testedAreFew = tested.size() < known.size() / 16;
    size_t position = 0;
    for (uint32_t shape : tested) {
        if (testedAreFew) {
            std::span<const uint32_t> rest = known.subspan(position);
            position += std::ranges::lower_bound(rest, shape) - rest.begin();
        } else {
            while (position < known.size() && known[position] < shape)
                ++position;
        }
        if (position == known.size())
            break;
        if (known[position] != shape)
            continue;
        if (passing.isEmpty())
            firstPosition = position;
        lastPosition = position;
        passing.append(shape);
    }
    if (passing.isEmpty())
        return { };
    if (passing.size() == known.size())
        return { .form = ShapeTestForm::AlwaysPasses };

    std::optional<ShapeTest> shortened;
    if (passing.size() == lastPosition - firstPosition + 1) {
        ShapeTestForm form = !firstPosition ? ShapeTestForm::UpTo
            : lastPosition + 1 == known.size() ? ShapeTestForm::From
            : passing.size() == 1 ? ShapeTestForm::OneShape : ShapeTestForm::Range;
        shortened = makeShapeTest(form, passing.first(), passing.last());
    } else if (passing.last() - passing.first() < immediateBitsWindow)
        shortened = makeShapeTestWithImmediateBits(passing.span());
    return shortened && instructionsForShapeTest(*shortened) < instructionsForShapeTest(general) ? *shortened : general;
}

ShapeTest ShapeTable::testOfShapes(std::span<const uint32_t> ascendingTestedShapes, std::span<const uint32_t> ascendingKnownShapes) const
{
    ASSERT(shapesAscendWithoutRepeats(ascendingKnownShapes));
    return shortenShapeTestForKnownShapes(testOfShapes(ascendingTestedShapes), ascendingTestedShapes, ascendingKnownShapes);
}

ShapeTest ShapeTable::testOf(std::span<const LayoutID> layouts) const
{
    ShapeList shapes;
    appendMemberShapes(layouts, shapes);
    return testOfShapes(shapes.span());
}

ShapeTestsOfSets::OfSet* ShapeTable::testsKeptOf(LayoutSetID set, std::span<const LayoutID> members, ShapeTestsOfSets::Part*& part) const
{
    if (!m_testsOfSets || !set || holdsOneMember(set) || liesInThread(set))
        return nullptr;
    part = &m_testsOfSets->parts[WTF::intHash(set.bits()) % m_testsOfSets->parts.size()];
    {
        Locker locker { part->lock };
        if (auto known = part->sets.find(set.bits()); known != part->sets.end())
            return known->value.get();
    }
    auto ofSet = makeUniqueWithoutFastMallocCheck<ShapeTestsOfSets::OfSet>();
    ofSet->test = testOf(members);
    Locker locker { part->lock };
    return part->sets.add(set.bits(), WTF::move(ofSet)).iterator->value.get();
}

ShapeTest ShapeTable::testOf(LayoutSetID set, std::span<const LayoutID> members) const
{
    ShapeTestsOfSets::Part* part = nullptr;
    ShapeTestsOfSets::OfSet* kept = testsKeptOf(set, members, part);
    return kept ? kept->test : testOf(members);
}

EncodedShapeSet ShapeTable::encodedShapesOf(LayoutSetID set, std::span<const LayoutID> members) const
{
    auto encode = [&] {
        ShapeList shapes;
        appendMemberShapes(members, shapes);
        return encodeShapeSet(shapes.span());
    };
    ShapeTestsOfSets::Part* part = nullptr;
    ShapeTestsOfSets::OfSet* kept = testsKeptOf(set, members, part);
    if (!kept)
        return encode();
    {
        Locker locker { part->lock };
        if (kept->encoded)
            return *kept->encoded;
    }
    EncodedShapeSet encoded = encode();
    Locker locker { part->lock };
    if (!kept->encoded)
        kept->encoded = encoded;
    return encoded;
}

static std::optional<EncodedShapeSet> tryHashShapeSet(std::span<const uint32_t> ascendingShapes, size_t maxNumberOfWords)
{
    constexpr unsigned maxNumberOfTrials = 256;
    constexpr unsigned maxLogNumberOfSlots = 10;
    size_t numberOfMembers = ascendingShapes.size();
    if (numberOfMembers < 2 || ascendingShapes.back() - ascendingShapes.front() > PagedBitSet::maxNumberInSlot)
        return std::nullopt;
    uint32_t last = ascendingShapes.back();
    WTF::BitSet<1u << maxLogNumberOfSlots> taken;
    for (unsigned logNumberOfSlots = std::max(1u, WTF::fastLog2(static_cast<unsigned>(numberOfMembers))); logNumberOfSlots <= maxLogNumberOfSlots; ++logNumberOfSlots) {
        unsigned shift = 32 - logNumberOfSlots;
        if (PagedBitSet::numberOfWordsOfHashedSet(shift) > maxNumberOfWords)
            return std::nullopt;
        uint32_t multiplier = 0x9e3779b1u;
        for (unsigned trial = 0; trial < maxNumberOfTrials; ++trial, multiplier = multiplier * 0x2c1b3c6du + 0x297a2d3au) {
            ASSERT(multiplier & 1);
            taken.clearAll();
            bool isPerfect = std::ranges::none_of(ascendingShapes, [&](uint32_t shape) {
                return taken.testAndSet(PagedBitSet::slotOf(last - shape, multiplier, shift));
            });
            if (!isPerfect)
                continue;
            EncodedShapeSet result { Vector<uint64_t>(FillWith { }, PagedBitSet::numberOfWordsOfHashedSet(shift), 0), PagedBitSet::Entries::Hashed };
            result.words[0] = PagedBitSet::headOfHashedSet(multiplier, shift);
            std::span<PagedBitSet::Slot> slots = spanReinterpretCast<PagedBitSet::Slot>(result.words.mutableSpan().subspan(1)).first(PagedBitSet::numberOfSlots(shift));
            for (uint32_t slot = 0; slot < slots.size(); ++slot) {
                uint32_t numberOfAnotherSlot = PagedBitSet::maxNumberInSlot;
                while (PagedBitSet::slotOf(numberOfAnotherSlot, multiplier, shift) == slot)
                    --numberOfAnotherSlot;
                slots[slot] = static_cast<PagedBitSet::Slot>(numberOfAnotherSlot);
            }
            for (uint32_t shape : ascendingShapes)
                slots[PagedBitSet::slotOf(last - shape, multiplier, shift)] = static_cast<PagedBitSet::Slot>(last - shape);
            return result;
        }
    }
    return std::nullopt;
}

static EncodedShapeSet encodeShapeSetAsBits(std::span<const uint32_t> ascendingShapes)
{
    using enum PagedBitSet::Entries;
    uint32_t count = ascendingShapes.empty() ? 0 : ascendingShapes.back() - ascendingShapes.front() + 1;
    Vector<uint64_t> pages(FillWith { }, numberOfPagesInShapeSet(count), 0);
    for (uint32_t shape : ascendingShapes) {
        uint32_t distanceFromLast = ascendingShapes.back() - shape;
        pages[distanceFromLast >> PagedBitSet::logBitsPerPage] |= 1ull << distanceFromLast % PagedBitSet::bitsPerPage;
    }
    if (pages.size() <= maxPagesInShortShapeSet)
        return { WTF::move(pages), None };

    Vector<uint64_t> distinctPages = pages;
    AOT::sort(distinctPages);
    distinctPages.shrink(std::ranges::unique(distinctPages).begin() - distinctPages.begin());

    auto numberOfWordsWith = [&](PagedBitSet::Entries entries) {
        return PagedBitSet::numberOfEntryWords(pages.size(), entries) + distinctPages.size();
    };
    PagedBitSet::Entries entries = numberOfWordsWith(Narrow) <= PagedBitSet::maxNumberOfWords(Narrow) ? Narrow : Wide;
    if (pages.size() <= numberOfWordsWith(entries) || numberOfWordsWith(entries) > PagedBitSet::maxNumberOfWords(entries))
        return { WTF::move(pages), None };

    size_t numberOfEntryWords = PagedBitSet::numberOfEntryWords(pages.size(), entries);
    EncodedShapeSet result { Vector<uint64_t>(FillWith { }, numberOfEntryWords, 0), entries };
    result.words.appendVector(distinctPages);
    std::span<uint8_t> narrowEntries = asMutableByteSpan(result.words.mutableSpan().first(numberOfEntryWords));
    std::span<uint16_t> wideEntries = spanReinterpretCast<uint16_t>(narrowEntries);
    for (size_t page = 0; page < pages.size(); ++page) {
        size_t word = numberOfEntryWords + (std::ranges::lower_bound(distinctPages, pages[page]) - distinctPages.begin());
        if (entries == Wide)
            wideEntries[page] = static_cast<uint16_t>(word);
        else
            narrowEntries[page] = static_cast<uint8_t>(word);
    }
    return result;
}

EncodedShapeSet encodeShapeSet(std::span<const uint32_t> ascendingShapes)
{
    ASSERT(shapesAscendWithoutRepeats(ascendingShapes));
    EncodedShapeSet bits = encodeShapeSetAsBits(ascendingShapes);
    if (std::optional<EncodedShapeSet> hashed = tryHashShapeSet(ascendingShapes, bits.words.size()))
        return WTF::move(*hashed);
    return bits;
}

size_t DistancesByComparison::numberOfComparisons() const
{
    if (comparesWithFirstShapes)
        return stretches.size() - 1;
    return std::ranges::count_if(stretches, [](const DistanceOfShapes& stretch) {
        return !!stretch.distance;
    });
}

uint32_t DistancesByComparison::distanceOf(uint32_t shape) const
{
    uint32_t distance = comparesWithFirstShapes ? stretches[0].distance : 0;
    for (const DistanceOfShapes& stretch : stretches) {
        if (comparesWithFirstShapes ? shape >= stretch.shapes.first : shape >= stretch.shapes.first && shape < stretch.shapes.end())
            distance = stretch.distance;
    }
    return distance;
}

DistancesByComparison distancesByComparison(std::span<const DistanceOfShapes> members)
{
    RELEASE_ASSERT(!members.empty());
    Vector<DistanceOfShapes, 8> ascending(members);
    std::ranges::sort(ascending, { }, [](const DistanceOfShapes& member) {
        return member.shapes.first;
    });

    DistancesByComparison result;
    for (const DistanceOfShapes& member : ascending) {
        RELEASE_ASSERT(!member.shapes.isEmpty() && (result.stretches.isEmpty() || result.stretches.last().shapes.end() <= member.shapes.first));
        if (!result.stretches.isEmpty() && result.stretches.last().distance == member.distance)
            result.stretches.last().shapes.count = member.shapes.end() - result.stretches.last().shapes.first;
        else
            result.stretches.append(member);
    }
    result.comparesWithFirstShapes = result.stretches.size() - 1 <= result.numberOfComparisons();
    return result;
}

unsigned DistancesByComparison::numberOfInstructions() const
{
    return 1 + static_cast<unsigned>(numberOfComparisons()) * (comparesWithFirstShapes ? 3 : 4);
}

static constexpr unsigned distanceTableInstructionCount(DistanceForm form)
{
    switch (form) {
    case DistanceForm::Comparisons:
        break;
    case DistanceForm::FlatTable:
        return 3;
    case DistanceForm::HashedTable:
        return 4;
    case DistanceForm::PagedTable:
        return 7;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

unsigned DistancesByShape::numberOfInstructions() const
{
    return form == DistanceForm::Comparisons ? comparisons.numberOfInstructions() : distanceTableInstructionCount(form);
}

static uint32_t readDistanceTableElement(std::span<const uint8_t> table, size_t element, unsigned logBytes)
{
    uint32_t value = 0;
    for (size_t byte = 0; byte < size_t { 1 } << logBytes; ++byte)
        value |= static_cast<uint32_t>(table[(element << logBytes) + byte]) << (8 * byte);
    return value;
}

static void writeDistanceTableElement(std::span<uint8_t> table, size_t element, unsigned logBytes, uint32_t value)
{
    for (size_t byte = 0; byte < size_t { 1 } << logBytes; ++byte)
        table[(element << logBytes) + byte] = static_cast<uint8_t>(value >> (8 * byte));
}

static constexpr uint32_t hashedDistanceTableIndex(uint32_t shape, uint32_t multiplier, unsigned shift)
{
    return AOT::shapeWordOf(shape) * multiplier >> shift;
}

uint32_t DistancesByShape::distanceOf(uint32_t shape) const
{
    uint32_t index = lastShape - shape;
    switch (form) {
    case DistanceForm::Comparisons:
        return comparisons.distanceOf(shape);
    case DistanceForm::FlatTable:
        return readDistanceTableElement(table.span(), index, logBytesPerDistance);
    case DistanceForm::HashedTable:
        return readDistanceTableElement(table.span(), hashedDistanceTableIndex(shape, hashMultiplier, hashShift), logBytesPerDistance);
    case DistanceForm::PagedTable:
        return readDistanceTableElement(table.span(), readDistanceTableElement(table.span(), index >> logShapesPerPage, logBytesPerPageEntry) - index, logBytesPerDistance);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool tryEncodeHashedDistanceTable(DistancesByShape& result, std::span<const DistanceOfShapes> members, std::span<const DistanceOfShapes> stretches)
{
    constexpr unsigned trialsPerLength = 64;
    constexpr unsigned maxLogLength = std::bit_width(maxShapesInFlatTable) - 1;
    constexpr uint32_t unused = std::numeric_limits<uint32_t>::max();
    static_assert(1u << maxLogLength == maxShapesInFlatTable);

    unsigned spanWidth = std::bit_width((result.lastShape - stretches.front().shapes.first) | 1);
    std::array<uint32_t, maxShapesInFlatTable> distances;
    for (unsigned logLength = 1; logLength <= maxLogLength; ++logLength) {
        unsigned shift = 32 - logLength;
        std::span<uint32_t> elements = std::span(distances).first(size_t { 1 } << logLength);
        uint32_t seed = 0x9e3779b1u;
        for (unsigned trial = 0; trial < trialsPerLength; ++trial, seed = seed * 0x2c1b3c6du + 0x297a2d3au) {
            uint32_t multiplier = trial % 2 ? seed >> (trial / 2 % spanWidth + 1) : seed;
            std::ranges::fill(elements, unused);
            auto place = [&](uint32_t shape, uint32_t distance) {
                uint32_t& element = elements[hashedDistanceTableIndex(shape, multiplier, shift)];
                if (element != unused && element != distance)
                    return false;
                element = distance;
                return true;
            };
            bool keepsDistancesApart = std::ranges::all_of(stretches, [&](const DistanceOfShapes& stretch) {
                return place(stretch.shapes.first, stretch.distance);
            }) && std::ranges::all_of(members, [&](const DistanceOfShapes& member) {
                for (uint32_t shape = member.shapes.first; shape < member.shapes.end(); ++shape) {
                    if (!place(shape, member.distance))
                        return false;
                }
                return true;
            });
            if (!keepsDistancesApart)
                continue;

            result.hashMultiplier = multiplier;
            result.hashShift = static_cast<uint8_t>(shift);
            result.table = Vector<uint8_t>(FillWith { }, elements.size() << result.logBytesPerDistance, static_cast<uint8_t>(0));
            for (size_t element = 0; element < elements.size(); ++element) {
                if (elements[element] != unused)
                    writeDistanceTableElement(result.table.mutableSpan(), element, result.logBytesPerDistance, elements[element]);
            }
            return true;
        }
    }
    return false;
}

namespace {

struct DistancePages {
    size_t numberOfBytes(unsigned logBytesPerDistance) const
    {
        return WTF::roundUpToMultipleOf(size_t { 1 } << logBytesPerEntry, (rowOfPage.size() << logBytesPerEntry) + (rows.size() << logBytesPerDistance));
    }

    unsigned logShapesPerPage { 0 };
    unsigned logBytesPerEntry { 0 };
    Vector<uint32_t> rowOfPage;
    Vector<uint16_t> rows;
};

} // anonymous namespace

static uint64_t hashMixedRowOfDistances(std::span<const uint16_t> row)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint16_t distance : row)
        hash = (hash ^ distance) * 1099511628211ull;
    return hash >> 2 | 1ull << 62;
}

static DistancePages buildDistancePages(std::span<const DistanceOfShapes> stretches, unsigned logShapesPerPage)
{
    int64_t last = stretches.back().shapes.end() - 1;
    size_t shapesPerPage = size_t { 1 } << logShapesPerPage;
    size_t numberOfPages = (static_cast<size_t>(last - stretches.front().shapes.first) >> logShapesPerPage) + 1;

    DistancePages pages { .logShapesPerPage = logShapesPerPage };
    pages.rowOfPage.reserveInitialCapacity(numberOfPages);
    UncheckedKeyHashMap<uint64_t, uint32_t> rowsByKey;
    Vector<uint16_t, 64> row(FillWith { }, shapesPerPage, static_cast<uint16_t>(0));
    size_t highest = stretches.size() - 1;
    for (size_t page = 0; page < numberOfPages; ++page) {
        int64_t top = last - static_cast<int64_t>(page << logShapesPerPage);
        int64_t bottom = top - static_cast<int64_t>(shapesPerPage - 1);
        auto reachesPage = [&](size_t stretch) {
            return stretches[stretch].shapes.end() > bottom;
        };
        while (stretches[highest].shapes.first > top)
            --highest;
        if (!reachesPage(highest)) {
            pages.rowOfPage.append(0);
            continue;
        }

        bool hasOneDistance = !highest || !reachesPage(highest - 1);
        if (!hasOneDistance) {
            size_t end = shapesPerPage;
            for (size_t stretch = highest; end; --stretch) {
                size_t begin = stretch && reachesPage(stretch - 1) ? static_cast<size_t>(stretches[stretch].shapes.first - bottom) : 0;
                std::ranges::fill(row.mutableSpan().subspan(begin, end - begin), static_cast<uint16_t>(stretches[stretch].distance));
                end = begin;
            }
        }
        uint64_t key = hasOneDistance ? uint64_t { stretches[highest].distance } + 1 : hashMixedRowOfDistances(row.span());
        auto known = rowsByKey.find(key);
        if (known != rowsByKey.end() && (hasOneDistance || std::ranges::equal(pages.rows.span().subspan(known->value, shapesPerPage), row.span()))) {
            pages.rowOfPage.append(known->value);
            continue;
        }
        if (hasOneDistance)
            std::ranges::fill(row, static_cast<uint16_t>(stretches[highest].distance));
        uint32_t begin = safeCast<uint32_t>(pages.rows.size());
        if (known == rowsByKey.end())
            rowsByKey.add(key, begin);
        pages.rows.append(row.span());
        pages.rowOfPage.append(begin);
    }
    return pages;
}

static uint64_t firstRowIndexOfDistancePages(const DistancePages& pages, unsigned logBytesPerEntry, unsigned logBytesPerDistance)
{
    return static_cast<uint64_t>(pages.rowOfPage.size()) << logBytesPerEntry >> logBytesPerDistance;
}

static void encodePagedDistanceTable(DistancesByShape& result, std::span<const DistanceOfShapes> stretches)
{
    constexpr unsigned logBytesPerNarrowEntry = 1;
    constexpr unsigned logBytesPerWideEntry = 2;
    static_assert(logBytesPerNarrowEntry >= 1, "the rows begin at a whole distance");

    uint32_t numberOfShapes = result.lastShape - stretches.front().shapes.first + 1;
    unsigned maxLogPageSize = std::bit_width(numberOfShapes - 1);
    auto minBytesForPageSize = [&](unsigned logShapesPerPage) {
        uint64_t numberOfPages = (static_cast<uint64_t>(numberOfShapes - 1) >> logShapesPerPage) + 1;
        return (numberOfPages << logBytesPerNarrowEntry) + (uint64_t { 1 } << logShapesPerPage << result.logBytesPerDistance);
    };

    std::optional<DistancePages> best;
    auto tryPageSize = [&](unsigned logShapesPerPage) {
        if (best && minBytesForPageSize(logShapesPerPage) >= best->numberOfBytes(result.logBytesPerDistance))
            return;
        DistancePages pages = buildDistancePages(stretches, logShapesPerPage);
        auto maxEntryForWidth = [&](unsigned logBytesPerEntry) {
            return firstRowIndexOfDistancePages(pages, logBytesPerEntry, result.logBytesPerDistance) + pages.rows.size() - 1 + (static_cast<uint64_t>(pages.rowOfPage.size() - 1) << logShapesPerPage);
        };
        pages.logBytesPerEntry = maxEntryForWidth(logBytesPerNarrowEntry) <= std::numeric_limits<uint16_t>::max() ? logBytesPerNarrowEntry : logBytesPerWideEntry;
        RELEASE_ASSERT(maxEntryForWidth(pages.logBytesPerEntry) <= std::numeric_limits<uint32_t>::max());
        if (!best || pages.numberOfBytes(result.logBytesPerDistance) < best->numberOfBytes(result.logBytesPerDistance))
            best = WTF::move(pages);
    };
    unsigned bestGuess = 0;
    for (unsigned logShapesPerPage = 1; logShapesPerPage <= maxLogPageSize; ++logShapesPerPage) {
        if (minBytesForPageSize(logShapesPerPage) < minBytesForPageSize(bestGuess))
            bestGuess = logShapesPerPage;
    }
    tryPageSize(bestGuess);
    for (unsigned logShapesPerPage = 0; logShapesPerPage <= maxLogPageSize; ++logShapesPerPage) {
        if (logShapesPerPage != bestGuess)
            tryPageSize(logShapesPerPage);
    }

    result.logShapesPerPage = static_cast<uint8_t>(best->logShapesPerPage);
    result.logBytesPerPageEntry = static_cast<uint8_t>(best->logBytesPerEntry);
    result.table = Vector<uint8_t>(FillWith { }, best->numberOfBytes(result.logBytesPerDistance), static_cast<uint8_t>(0));
    std::span<uint8_t> table = result.table.mutableSpan();
    uint64_t firstRow = firstRowIndexOfDistancePages(*best, best->logBytesPerEntry, result.logBytesPerDistance);
    uint64_t lastPlaceInRow = (uint64_t { 1 } << best->logShapesPerPage) - 1;
    for (size_t page = 0; page < best->rowOfPage.size(); ++page)
        writeDistanceTableElement(table, page, best->logBytesPerEntry, static_cast<uint32_t>(firstRow + best->rowOfPage[page] + lastPlaceInRow + (static_cast<uint64_t>(page) << best->logShapesPerPage)));
    for (size_t element = 0; element < best->rows.size(); ++element)
        writeDistanceTableElement(table, firstRow + element, result.logBytesPerDistance, best->rows[element]);
}

static void encodeFlatDistanceTable(DistancesByShape& result, std::span<const DistanceOfShapes> members, uint32_t firstShape)
{
    result.table = Vector<uint8_t>(FillWith { }, static_cast<size_t>(result.lastShape - firstShape + 1) << result.logBytesPerDistance, std::numeric_limits<uint8_t>::max());
    for (const DistanceOfShapes& member : members) {
        for (uint32_t shape = member.shapes.first; shape < member.shapes.end(); ++shape)
            writeDistanceTableElement(result.table.mutableSpan(), result.lastShape - shape, result.logBytesPerDistance, member.distance);
    }
}

static DistancesByShape encodeDistancesOfStretches(std::span<const DistanceOfShapes> members, DistancesByComparison&& byComparison, DistanceForm form)
{
    std::span<const DistanceOfShapes> stretches = byComparison.stretches.span();
    DistancesByShape result { .form = form, .lastShape = stretches.back().shapes.end() - 1, .numberOfShapes = stretches.back().shapes.end() - stretches.front().shapes.first };
    if (form == DistanceForm::Comparisons) {
        result.comparisons = WTF::move(byComparison);
        return result;
    }

    uint32_t greatestDistance = std::ranges::max(stretches, { }, &DistanceOfShapes::distance).distance;
    RELEASE_ASSERT(greatestDistance < std::numeric_limits<uint16_t>::max());
    result.logBytesPerDistance = greatestDistance >= std::numeric_limits<uint8_t>::max();
    if (form == DistanceForm::FlatTable) {
        encodeFlatDistanceTable(result, members, stretches.front().shapes.first);
        return result;
    }
    if (form == DistanceForm::HashedTable && tryEncodeHashedDistanceTable(result, members, stretches))
        return result;
    result.form = DistanceForm::PagedTable;
    encodePagedDistanceTable(result, stretches);
    return result;
}

DistancesByShape encodeDistancesByShape(std::span<const DistanceOfShapes> members, DistanceForm form)
{
    return encodeDistancesOfStretches(members, distancesByComparison(members), form);
}

DistancesByShape distancesByShape(std::span<const DistanceOfShapes> members)
{
    DistancesByShape byComparison = encodeDistancesByShape(members, DistanceForm::Comparisons);
    const Vector<DistanceOfShapes, 8>& stretches = byComparison.comparisons.stretches;
    DistanceForm form = stretches.last().shapes.end() - stretches.first().shapes.first <= maxShapesInFlatTable ? DistanceForm::FlatTable : DistanceForm::HashedTable;
    if (byComparison.numberOfInstructions() <= distanceTableInstructionCount(form))
        return byComparison;

    DistancesByShape byTable = encodeDistancesOfStretches(members, DistancesByComparison(byComparison.comparisons), form);
    if (byComparison.numberOfInstructions() <= byTable.numberOfInstructions())
        return byComparison;
    return byTable;
}

static Vector<uint32_t> layoutsEachInFrontOfThoseBelowForShapeTest(const Program& program)
{
    constexpr uint32_t none = UINT32_MAX;
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    Vector<uint32_t> firstBelow(FillWith { }, numberOfLayouts, none);
    Vector<uint32_t> nextBeside(FillWith { }, numberOfLayouts, none);
    for (uint32_t index = numberOfLayouts; index--;) {
        LayoutID base = writtenBaseOf(program.layouts[index]);
        if (!base)
            continue;
        nextBeside[index] = firstBelow[base.index()];
        firstBelow[base.index()] = index;
    }

    Vector<uint32_t> order;
    order.reserveInitialCapacity(numberOfLayouts);
    BitVector isListed(numberOfLayouts);
    Vector<uint32_t, 32> pending;
    auto listWithAllBelow = [&](uint32_t top) {
        if (isListed.quickSet(top))
            return;
        order.append(top);
        pending.append(firstBelow[top]);
        while (!pending.isEmpty()) {
            uint32_t current = pending.takeLast();
            if (current == none)
                continue;
            pending.append(nextBeside[current]);
            if (isListed.quickSet(current))
                continue;
            order.append(current);
            pending.append(firstBelow[current]);
        }
    };
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!writtenBaseOf(program.layouts[index]))
            listWithAllBelow(index);
    }
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        listWithAllBelow(index);
    return order;
}

static OptionSet<ObjectMutation> mutationsRuledOutForShapeTest(const Program& program, const LinkTables& linked, LayoutID id)
{
    const LinkedLayout& layout = linked.layout(id);
    if (layout.reachesUnseenCode)
        return { };
    OptionSet<ObjectMutation> ruledOut;
    constexpr std::pair<LayoutChange, ObjectMutation> mutationsOfChanges[] {
        { LayoutChange::IsFrozen, ObjectMutation::ChangeIntegrity },
        { LayoutChange::GainsProperty, ObjectMutation::AddProperty },
        { LayoutChange::LosesProperty, ObjectMutation::DeleteField },
        { LayoutChange::ChangesPrototype, ObjectMutation::ChangePrototype },
        { LayoutChange::GetsAccessor, ObjectMutation::RedefineProperty },
    };
    for (auto [change, mutation] : mutationsOfChanges) {
        if (!layout.possibleChanges.contains(change))
            ruledOut.add(mutation);
    }
    if (!layout.hasDirectInstances)
        ruledOut.add(ObjectMutation::ConstructDirectly);
    uint32_t numberDeclared = static_cast<uint32_t>(program.at(id).fields.size());
    for (uint32_t field = 0; field < numberDeclared; ++field) {
        if (!fieldAt(program, declarationOf(program, { id, field })).form.isEmpty())
            ruledOut.remove(ObjectMutation::RedefineProperty);
    }
    return ruledOut;
}

ShapeTable computeShapeTable(const Program&, const LinkTables&, const StorageOfLayouts&, LayoutID closureLayout, std::span<const uint32_t> orderOfLayouts);
ShapeTable computeShapeTable(const Program& program, const LinkTables& linked, const StorageOfLayouts& storage, LayoutID closureLayout, std::span<const uint32_t> orderOfLayouts)
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    ShapeTableParts parts;
    parts.shapes.append(PlacedShape { });
    parts.shapesOfLayout.fill(ShapeRange { }, numberOfLayouts);
    auto giveShapeTo = [&](LayoutID id) {
        parts.shapesOfLayout[id.index()] = ShapeRange { static_cast<uint32_t>(parts.shapes.size()), 1 };
        parts.shapes.append(PlacedShape { .layout = id });
    };

    Vector<uint32_t> orderByWrittenBases;
    if (orderOfLayouts.empty()) {
        orderByWrittenBases = layoutsEachInFrontOfThoseBelowForShapeTest(program);
        orderOfLayouts = orderByWrittenBases.span();
    }
    for (uint32_t index : orderOfLayouts) {
        LayoutID id(index);
        if (storage.holdsCompiledObjectsInUse(id)) {
            giveShapeTo(id);
            parts.shapes.last().impossibleMutations = mutationsRuledOutForShapeTest(program, linked, id);
        }
    }
    parts.firstEnvironmentShape = static_cast<uint32_t>(parts.shapes.size());
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID id(index);
        bool isEnvironmentInUse = storage.of(id).storage == LayoutStorage::Environment && !storage.isNeverAllocated(id);
        if (isEnvironmentInUse || id == closureLayout)
            giveShapeTo(id);
    }

    RELEASE_ASSERT(parts.shapes.size() - 1 <= compiledObjectShapeBit);
    RELEASE_ASSERT(parts.shapes.size() - 1 <= maxNumberOfShapes);
    return ShapeTable(WTF::move(parts));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
