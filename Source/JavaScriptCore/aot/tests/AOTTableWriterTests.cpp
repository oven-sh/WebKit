/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageTestSupport.h"
#include "AOTShapeTest.h"
#include "AOTTableParts.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC::AOT {

using namespace ImageTests;

static constexpr size_t vectorBytes = defaultLimitValue(LimitID::VectorBytes, hostArchitecture());
static_assert(vectorBytes < defaultLimitValue(LimitID::TableBytes, hostArchitecture()), "A part tests VectorBytes first, so no part reports TableBytes.");

static Vector<TablePart> createAllTableParts(BuildContext& build)
{
    return Vector<TablePart>(numberOfTableParts, [&](size_t index) {
        return TablePart(static_cast<TablePartID>(index), build);
    });
}

static TablePart& tablePartWithID(Vector<TablePart>& parts, TablePartID id)
{
    return parts[static_cast<unsigned>(id)];
}

static void writeEmptyProgramTables(TablePart& part, std::optional<ProgramTableIndex> omittedTable = std::nullopt)
{
    uint32_t offset = part.allocate(sizeof(uint64_t));
    for (unsigned index = 0; index < numberOfProgramTables; ++index) {
        ProgramTableIndex table = static_cast<ProgramTableIndex>(index);
        if (table != omittedTable)
            part.setTableRange(table, offset, 0);
    }
}

static void testSharesAppendedBytes()
{
    BuildContext build(wireOptions());
    TablePart part(TablePartID::Data, build);
    uint32_t first = part.append(Vector<uint32_t> { 1, 2, 3 });
    uint32_t second = part.append(Vector<uint32_t> { 1, 2, 4 });
    size_t size = part.size();
    AOT_TEST_CHECK(second != first);
    AOT_TEST_CHECK(part.append(Vector<uint32_t> { 1, 2, 3 }) == first);
    AOT_TEST_CHECK(part.size() == size);
}

static void testSharesOnlyAppendedBytes()
{
    BuildContext build(wireOptions());
    TablePart part(TablePartID::Data, build);
    constexpr std::array<uint8_t, 8> zeros { };
    uint32_t allocated = part.allocate(zeros.size());
    uint32_t unshared = part.appendUnshared(zeros);
    uint32_t appended = part.append(std::span<const uint8_t> { zeros });
    AOT_TEST_CHECK(unshared == allocated + zeros.size());
    AOT_TEST_CHECK(appended == unshared + zeros.size());
}

// A linked word that is 0 means no offset, so a part puts nothing at its offset 0.
static void testKeepsOffsetZeroFree()
{
    BuildContext build(wireOptions());
    constexpr std::array<uint8_t, 1> byte { 1 };
    AOT_TEST_CHECK(TablePart(TablePartID::Data, build).allocate(1, 1));
    AOT_TEST_CHECK(TablePart(TablePartID::Data, build).append(std::span<const uint8_t> { byte }, 1));
    AOT_TEST_CHECK(TablePart(TablePartID::Data, build).appendUnshared(byte));
}

static void testFillsToLimit()
{
    BuildContext build(wireOptions());
    TablePart part(TablePartID::Data, build);
    uint32_t first = part.allocate(8);
    AOT_TEST_CHECK(first);
    AOT_TEST_CHECK(part.allocate(vectorBytes - part.size(), 1) == first + 8);
    AOT_TEST_CHECK(part.size() == vectorBytes);
    AOT_TEST_CHECK(!count(build, UserDiagnosticKind::LimitExceeded, LimitID::VectorBytes));
}

static void testVectorBytesOverflow()
{
    BuildContext build(wireOptions());
    Vector<TablePart> parts = createAllTableParts(build);
    TablePart& part = tablePartWithID(parts, TablePartID::Data);
    constexpr std::array<uint8_t, 4> bytes { 1, 2, 3, 4 };
    AOT_TEST_CHECK(part.allocate(8));
    size_t sizeOfLargeRecord = vectorBytes + 1 - part.size();
    AOT_TEST_CHECK(!(sizeOfLargeRecord % alignof(uint64_t)));

    AOT_TEST_CHECK(!part.allocate(sizeOfLargeRecord));
    AOT_TEST_CHECK(part.size() == sizeOfLargeRecord);
    part.at<uint64_t>(static_cast<uint32_t>(sizeOfLargeRecord - sizeof(uint64_t))) = 1;

    AOT_TEST_CHECK(!part.allocate(8));
    AOT_TEST_CHECK(!part.append(std::span<const uint8_t> { bytes }));
    AOT_TEST_CHECK(!part.appendUnshared(bytes));
    part.appendUnshared(std::span { bytes }.first(1));
    AOT_TEST_CHECK(part.size() == sizeOfLargeRecord);
    ProgramTables header { };
    AOT_TEST_CHECK(joinTableParts(parts.mutableSpan(), header, build).isEmpty());
    AOT_TEST_CHECK(count(build, UserDiagnosticKind::LimitExceeded, LimitID::VectorBytes) == 1);
    AOT_TEST_CHECK(build.diagnostics().userDiagnostics().size() == 1);
    AOT_TEST_CHECK(build.diagnostics().internalErrors().isEmpty());
}

// Whoever fills its room itself gets it where allocate() would have put it, and what alignment leaves empty in front of it is zero.
static void testUnfilledRoomLiesWhereZeroedRoomWould()
{
    BuildContext build(wireOptions());
    TablePart zeroed(TablePartID::Data, build);
    TablePart unfilled(TablePartID::Data, build);
    constexpr std::array<uint8_t, 3> bytesThatLeaveSizeUnaligned { 1, 2, 3 };
    constexpr std::array<std::pair<size_t, size_t>, 5> sizesAndAlignments { { { 0, 4 }, { 5, 1 }, { 12, 4 }, { 0, 8 }, { 100000, 8 } } };
    unfilled.reserveRoomFor(200000);
    for (auto [size, alignment] : sizesAndAlignments) {
        AOT_TEST_CHECK(zeroed.appendUnshared(bytesThatLeaveSizeUnaligned) == unfilled.appendUnshared(bytesThatLeaveSizeUnaligned));
        size_t end = unfilled.size();
        uint32_t offset = unfilled.allocateUnfilled(size, alignment);
        AOT_TEST_CHECK(offset == zeroed.allocate(size, alignment));
        AOT_TEST_CHECK(unfilled.size() == zeroed.size());
        AOT_TEST_CHECK(std::ranges::all_of(unfilled.bytesAt(static_cast<uint32_t>(end), offset - end), [](uint8_t byte) { return !byte; }));
        std::ranges::fill(unfilled.bytesAt(offset, size), 0xee);
    }
    AOT_TEST_CHECK(!build.diagnostics().hasErrors());
}

// The build has failed, and whoever asked still writes what it asked room for.
static void testUnfilledRoomOverVectorBytes()
{
    BuildContext build(wireOptions());
    TablePart part(TablePartID::Data, build);
    AOT_TEST_CHECK(part.allocate(8));
    part.reserveRoomFor(static_cast<uint64_t>(vectorBytes) + 1);
    size_t sizeOfLargeRecord = vectorBytes + 1 - part.size();
    AOT_TEST_CHECK(!part.allocateUnfilled(sizeOfLargeRecord, 1));
    AOT_TEST_CHECK(part.size() == sizeOfLargeRecord);
    part.bytesAt(0, sizeOfLargeRecord).back() = 1;
    AOT_TEST_CHECK(!part.allocateUnfilled(8));
    AOT_TEST_CHECK(!part.setTableWrittenInPlace<uint64_t>(ProgramTableIndex::types, 2, [](std::span<uint64_t> room) {
        std::ranges::fill(room, 1);
    }));
    AOT_TEST_CHECK(part.size() == sizeOfLargeRecord);
    AOT_TEST_CHECK(count(build, UserDiagnosticKind::LimitExceeded, LimitID::VectorBytes) == 1);
    AOT_TEST_CHECK(build.diagnostics().internalErrors().isEmpty());
}

template<typename T>
static void checkTableWrittenInPlace(TablePart& handedItems, TablePart& writtenInPlace, const Vector<T>& items)
{
    uint32_t expected = handedItems.setTable(ProgramTableIndex::types, items);
    uint32_t offset = writtenInPlace.setTableWrittenInPlace<T>(ProgramTableIndex::types, items.size(), [&](std::span<T> room) {
        AOT_TEST_CHECK(room.size() == items.size());
        memcpySpan(room, items.span());
    });
    AOT_TEST_CHECK(offset == expected);
    AOT_TEST_CHECK(writtenInPlace.size() == handedItems.size());
    AOT_TEST_CHECK(equalSpans(writtenInPlace.bytesAt(0, writtenInPlace.size()), handedItems.bytesAt(0, handedItems.size())));
}

// Tables that repeat another, in the same words or in words of another size. Empty ones. One whose bytes the part has at a place that is not aligned enough for it, and who gets which copy from then on.
static void testTableWrittenInPlaceLiesWhereSetTableWouldPutIt()
{
    BuildContext build(wireOptions());
    TablePart handedItems(TablePartID::Data, build);
    TablePart writtenInPlace(TablePartID::Data, build);
    auto check = [&](const auto& items) {
        checkTableWrittenInPlace(handedItems, writtenInPlace, items);
    };
    check(Vector<uint32_t> { 1, 2, 3 });
    check(Vector<uint16_t> { 7, 8, 9 });
    check(Vector<uint64_t> { 5 });
    size_t sizeInFrontOfRepeatedTables = writtenInPlace.size();
    check(Vector<uint32_t> { 1, 2, 3 });
    check(Vector<uint16_t> { 1, 0, 2, 0, 3, 0 });
    AOT_TEST_CHECK(writtenInPlace.size() == sizeInFrontOfRepeatedTables);
    check(Vector<uint16_t> { 11 });
    check(Vector<uint16_t> { });
    check(Vector<uint64_t> { });
    check(Vector<uint32_t> { });
    check(Vector<uint8_t> { 1, 2, 3 });
    check(Vector<uint32_t> { 4, 0 });
    size_t sizeInFrontOfAlignedCopy = writtenInPlace.size();
    check(Vector<uint64_t> { 4 });
    AOT_TEST_CHECK(writtenInPlace.size() > sizeInFrontOfAlignedCopy);
    check(Vector<uint32_t> { 4, 0 });
    check(Vector<uint64_t> { 4 });
    check(Vector<uint32_t>(100000, [](size_t index) {
        return static_cast<uint32_t>(index * 2654435761u);
    }));
    AOT_TEST_CHECK(!build.diagnostics().hasErrors());
}

static void testBytesWrittenInPlaceAreSharedWithAppendedBytes()
{
    BuildContext build(wireOptions());
    TablePart part(TablePartID::Data, build);
    auto writeInPlace = [&](const Vector<uint32_t>& items) {
        return part.setTableWrittenInPlace<uint32_t>(ProgramTableIndex::types, items.size(), [&](std::span<uint32_t> room) {
            memcpySpan(room, items.span());
        });
    };
    uint32_t appended = part.append(Vector<uint32_t> { 1, 2, 3 });
    constexpr std::array<uint8_t, 1> byteThatLeavesSizeUnaligned { 0xff };
    part.appendUnshared(byteThatLeavesSizeUnaligned);
    size_t size = part.size();
    AOT_TEST_CHECK(writeInPlace({ 1, 2, 3 }) == appended);
    AOT_TEST_CHECK(part.size() == size);

    uint32_t writtenInPlace = writeInPlace({ 4, 5, 6 });
    AOT_TEST_CHECK(writtenInPlace > appended && !(writtenInPlace % alignof(uint32_t)));
    size = part.size();
    AOT_TEST_CHECK(part.append(Vector<uint32_t> { 4, 5, 6 }) == writtenInPlace);
    AOT_TEST_CHECK(writeInPlace({ 4, 5, 6 }) == writtenInPlace);
    AOT_TEST_CHECK(part.size() == size);
    // Offsets never share bytes with what append() was handed, however it came into the part.
    AOT_TEST_CHECK(part.appendOffsets(Vector<uint32_t> { 4, 5, 6 }.span()) != writtenInPlace);
}

static Vector<uint8_t> joinPartsOfSeveralMegabytesOnThreads(unsigned threadCount)
{
    constexpr size_t sizeOfLongPart = 3 * MB + 5;
    BuildContext build(wireOptions({ .threadCount = threadCount }));
    Vector<TablePart> parts = createAllTableParts(build);
    TablePart& texts = tablePartWithID(parts, TablePartID::FunctionTexts);
    TablePart& data = tablePartWithID(parts, TablePartID::Data);
    uint32_t offsetInTexts = texts.allocateUnfilled(sizeOfLongPart, 1);
    std::span<uint8_t> bytes = texts.bytesAt(offsetInTexts, sizeOfLongPart);
    for (size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>(index * 31 + index / 251);
    uint32_t word = data.append(Vector<uint32_t> { offsetInTexts });
    data.link(word, TablePartID::FunctionTexts);
    data.setTableRange(ProgramTableIndex::types, word, 1);
    writeEmptyProgramTables(data, ProgramTableIndex::types);

    ProgramTables header { };
    header.magic = ProgramTables::expectedMagic;
    Vector<uint8_t> joined = joinTableParts(parts.mutableSpan(), header, build);
    AOT_TEST_CHECK(!build.diagnostics().hasErrors());
    const ProgramTables& tables = reinterpretCastSpanStartTo<ProgramTables>(joined.span());
    AOT_TEST_CHECK(equalSpans(joined.span().subspan(tables.types()[0], sizeOfLongPart), std::span<const uint8_t>(bytes)));
    return joined;
}

// The threads share the bytes of the parts and not the parts.
static void testJoinCopiesPartOfSeveralMegabytes()
{
    AOT_TEST_CHECK(joinPartsOfSeveralMegabytesOnThreads(1) == joinPartsOfSeveralMegabytesOnThreads(8));
}

static void testJoinRelocatesLinkedWords()
{
    BuildContext build(wireOptions());
    Vector<TablePart> parts = createAllTableParts(build);
    TablePart& types = tablePartWithID(parts, TablePartID::Types);
    TablePart& data = tablePartWithID(parts, TablePartID::Data);
    constexpr uint64_t recordInTypes = 0x1111222233334444ull;
    constexpr uint64_t recordInData = 0x5555666677778888ull;
    constexpr std::array<uint8_t, 1> byteThatLeavesSizeUnaligned { 0xff };
    constexpr uint32_t wordSize = sizeof(uint32_t);

    uint32_t offsetInTypes = types.append(Vector<uint64_t> { recordInTypes });
    types.appendUnshared(byteThatLeavesSizeUnaligned);
    uint32_t offsetInData = data.append(Vector<uint64_t> { recordInData });

    uint32_t words = 0;
    for (unsigned owner = 0; owner < 2; ++owner) {
        words = data.append(Vector<uint32_t> { offsetInTypes, offsetInData, 0, offsetInTypes });
        data.link(words, TablePartID::Types);
        data.link(words + wordSize);
        data.link(words + 2 * wordSize, TablePartID::Types);
    }
    data.setTableRange(ProgramTableIndex::types, words, 4);
    writeEmptyProgramTables(data, ProgramTableIndex::types);

    ProgramTables header { };
    header.magic = ProgramTables::expectedMagic;
    Vector<uint8_t> joined = joinTableParts(parts.mutableSpan(), header, build);
    AOT_TEST_CHECK(!build.diagnostics().hasErrors());

    const ProgramTables& tables = reinterpretCastSpanStartTo<ProgramTables>(joined.span());
    AOT_TEST_CHECK(tables.magic == ProgramTables::expectedMagic);
    std::span<const uint32_t> joinedWords = tables.types();
    AOT_TEST_CHECK(joinedWords.size() == 4);
    AOT_TEST_CHECK(*tables.at<uint64_t>(joinedWords[0]) == recordInTypes);
    AOT_TEST_CHECK(!(joinedWords[1] % alignof(uint64_t)));
    AOT_TEST_CHECK(*tables.at<uint64_t>(joinedWords[1]) == recordInData);
    AOT_TEST_CHECK(!joinedWords[2]);
    AOT_TEST_CHECK(joinedWords[3] == offsetInTypes);
}

// One owner means the word as an offset, the other as a number, and the part holds one copy for both.
static void testJoinRefusesWordsLinkedByOneOfTwoOwners()
{
    BuildContext build(wireOptions());
    Vector<TablePart> parts = createAllTableParts(build);
    TablePart& data = tablePartWithID(parts, TablePartID::Data);
    uint32_t offsetInTypes = tablePartWithID(parts, TablePartID::Types).append(Vector<uint64_t> { 1 });
    uint32_t word = data.append(Vector<uint32_t> { offsetInTypes });
    data.link(word, TablePartID::Types);
    AOT_TEST_CHECK(data.append(Vector<uint32_t> { offsetInTypes }) == word);
    writeEmptyProgramTables(data);

    ProgramTables header { };
    joinTableParts(parts.mutableSpan(), header, build);
    AOT_TEST_CHECK(count(build, InternalErrorKind::ProgramTableCheckFailed) == 1);
}

static void testJoinRefusesTableWrittenByNoPart()
{
    BuildContext build(wireOptions());
    Vector<TablePart> parts = createAllTableParts(build);
    writeEmptyProgramTables(tablePartWithID(parts, TablePartID::Data), ProgramTableIndex::types);

    ProgramTables header { };
    joinTableParts(parts.mutableSpan(), header, build);
    AOT_TEST_CHECK(count(build, InternalErrorKind::ProgramTableCheckFailed) == 1);
}

static void testJoinRefusesTableWrittenByTwoParts()
{
    BuildContext build(wireOptions());
    Vector<TablePart> parts = createAllTableParts(build);
    TablePart& types = tablePartWithID(parts, TablePartID::Types);
    types.setTableRange(ProgramTableIndex::types, types.allocate(sizeof(uint64_t)), 0);
    writeEmptyProgramTables(tablePartWithID(parts, TablePartID::Data));

    ProgramTables header { };
    joinTableParts(parts.mutableSpan(), header, build);
    AOT_TEST_CHECK(count(build, InternalErrorKind::ProgramTableCheckFailed) == 1);
}

static Vector<uint32_t> shapesAtDistances(uint32_t last, const Vector<uint32_t>& distances)
{
    Vector<uint32_t> shapes = distances.map([&](uint32_t distance) {
        return last - distance;
    });
    std::ranges::sort(shapes);
    return shapes;
}

static void checkShapeSet(const Vector<uint32_t>& ascendingShapes)
{
    Typed::EncodedShapeSet encoded = Typed::encodeShapeSet(ascendingShapes.span());
    uint32_t last = ascendingShapes.last();
    uint32_t count = last - ascendingShapes.first() + 1;
    uint64_t numberOfPages = Typed::numberOfPagesInShapeSet(count);
    AOT_TEST_CHECK(encoded.entries == PagedBitSet::Entries::None ? encoded.words.size() == numberOfPages : encoded.words.size() <= numberOfPages);
    // Compiled code asks a hashed set for a distance as it is, however far out of reach.
    uint32_t greatestDistanceAsked = encoded.entries == PagedBitSet::Entries::Hashed ? PagedBitSet::maxNumberInSlot : count;
    for (uint32_t distance = 0; distance <= greatestDistanceAsked; ++distance) {
        bool isMember = distance < count && std::ranges::binary_search(ascendingShapes, last - distance);
        AOT_TEST_CHECK(PagedBitSet::contains(encoded.words.span().data(), encoded.entries, distance) == isMember);
    }
}

static void testShapeSetHoldsItsMembers()
{
    checkShapeSet({ 7 });
    checkShapeSet({ 1, 64 });
    checkShapeSet({ 1, 65 });
    checkShapeSet({ 1, 511 });
    checkShapeSet({ 1, 512 });
    checkShapeSet({ 3, 4, 5, 70, 71, 500, 1000, 1001, 1063, 1064 });
    Vector<uint32_t> everySecond(1000, [](size_t index) {
        return static_cast<uint32_t>(10 + 2 * index);
    });
    checkShapeSet(everySecond);
    Vector<uint32_t> neighbors(70, [](size_t index) {
        return static_cast<uint32_t>(10 + index + index / 60);
    });
    checkShapeSet(neighbors);
}

static void testShapeSetsAtEqualDistancesAreEqual()
{
    Vector<uint32_t> distances { 0, 3, 90, 4000 };
    Typed::EncodedShapeSet first = Typed::encodeShapeSet(shapesAtDistances(5000, distances).span());
    Typed::EncodedShapeSet second = Typed::encodeShapeSet(shapesAtDistances(77777, distances).span());
    AOT_TEST_CHECK(first.entries == second.entries);
    AOT_TEST_CHECK(first.words == second.words);
}

std::span<const NativeTest> tableWriterTests()
{
    static constexpr NativeTest tests[] = {
        { "plan/shape-sets/holds-its-members"_s, testShapeSetHoldsItsMembers },
        { "plan/shape-sets/equal-distances-are-equal"_s, testShapeSetsAtEqualDistancesAreEqual },
        { "image/table-part/shares-appended-bytes"_s, testSharesAppendedBytes },
        { "image/table-part/shares-only-appended-bytes"_s, testSharesOnlyAppendedBytes },
        { "image/table-part/keeps-offset-zero-free"_s, testKeepsOffsetZeroFree },
        { "image/table-part/fills-to-limit"_s, testFillsToLimit },
        { "image/table-part/vector-bytes-overflow"_s, testVectorBytesOverflow },
        { "image/table-part/unfilled-room-lies-where-zeroed-room-would"_s, testUnfilledRoomLiesWhereZeroedRoomWould },
        { "image/table-part/unfilled-room-over-vector-bytes"_s, testUnfilledRoomOverVectorBytes },
        { "image/table-part/table-written-in-place-lies-where-set-table-would-put-it"_s, testTableWrittenInPlaceLiesWhereSetTableWouldPutIt },
        { "image/table-part/bytes-written-in-place-are-shared-with-appended-bytes"_s, testBytesWrittenInPlaceAreSharedWithAppendedBytes },
        { "image/table-part/join-copies-part-of-several-megabytes"_s, testJoinCopiesPartOfSeveralMegabytes },
        { "image/table-part/join-relocates-linked-words"_s, testJoinRelocatesLinkedWords },
        { "image/table-part/join-refuses-words-linked-by-one-of-two-owners"_s, testJoinRefusesWordsLinkedByOneOfTwoOwners },
        { "image/table-part/join-refuses-table-written-by-no-part"_s, testJoinRefusesTableWrittenByNoPart },
        { "image/table-part/join-refuses-table-written-by-two-parts"_s, testJoinRefusesTableWrittenByTwoParts },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
