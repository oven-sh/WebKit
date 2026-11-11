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
    AOT_TEST_CHECK(!part.setTableInPlace<uint64_t>(ProgramTableIndex::types, 2, [](std::span<uint64_t> room) {
        std::ranges::fill(room, 1);
    }));
    AOT_TEST_CHECK(part.size() == sizeOfLargeRecord);
    AOT_TEST_CHECK(count(build, UserDiagnosticKind::LimitExceeded, LimitID::VectorBytes) == 1);
    AOT_TEST_CHECK(build.diagnostics().internalErrors().isEmpty());
}

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

static void checkShapeSet(const Vector<uint32_t>& ascendingShapes)
{
    Typed::EncodedShapeSet encoded = Typed::encodeShapeSet(ascendingShapes.span());
    uint32_t last = ascendingShapes.last();
    uint32_t count = last - ascendingShapes.first() + 1;
    uint64_t numberOfPages = Typed::numberOfPagesInShapeSet(count);
    AOT_TEST_CHECK(encoded.entries == PagedBitSet::Entries::None ? encoded.words.size() == numberOfPages : encoded.words.size() <= numberOfPages);
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

std::span<const NativeTest> tableWriterTests()
{
    static constexpr NativeTest tests[] = {
        { "plan/shape-sets/holds-its-members"_s, testShapeSetHoldsItsMembers },
        { "image/table-part/vector-bytes-overflow"_s, testVectorBytesOverflow },
        { "image/table-part/unfilled-room-over-vector-bytes"_s, testUnfilledRoomOverVectorBytes },
        { "image/table-part/join-refuses-words-linked-by-one-of-two-owners"_s, testJoinRefusesWordsLinkedByOneOfTwoOwners },
        { "image/table-part/join-refuses-table-written-by-no-part"_s, testJoinRefusesTableWrittenByNoPart },
        { "image/table-part/join-refuses-table-written-by-two-parts"_s, testJoinRefusesTableWrittenByTwoParts },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
