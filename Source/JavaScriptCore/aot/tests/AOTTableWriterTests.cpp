/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageTestSupport.h"
#include "AOTProgramTableEncoder.h"
#include "AOTShapeTest.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>

namespace JSC::AOT {

using namespace ImageTests;
using Typed::TableWriter;

static constexpr size_t vectorBytes = defaultLimitValue(LimitID::VectorBytes, hostArchitecture());
static_assert(vectorBytes < defaultLimitValue(LimitID::TableBytes, hostArchitecture()), "A writer tests VectorBytes first, so no writer reports TableBytes.");

static uint64_t hitCount(BuildContext& build, LimitID limit)
{
    return build.statistics().values()[static_cast<unsigned>(hitsOf(limit))];
}

static void testSharesAppendedBytes()
{
    BuildContext build(wireOptions());
    TableWriter writer(build);
    uint32_t first = writer.append(Vector<uint32_t> { 1, 2, 3 });
    uint32_t second = writer.append(Vector<uint32_t> { 1, 2, 4 });
    size_t size = writer.size();
    AOT_TEST_CHECK(second != first);
    AOT_TEST_CHECK(writer.append(Vector<uint32_t> { 1, 2, 3 }) == first);
    AOT_TEST_CHECK(writer.size() == size);
}

static void testSharesOnlyAppendedBytes()
{
    BuildContext build(wireOptions());
    TableWriter writer(build);
    constexpr std::array<uint8_t, 8> zeros { };
    uint32_t allocated = writer.allocate(zeros.size());
    uint32_t unshared = writer.appendUnshared(zeros);
    uint32_t appended = writer.append(std::span<const uint8_t> { zeros });
    AOT_TEST_CHECK(unshared == allocated + zeros.size());
    AOT_TEST_CHECK(appended == unshared + zeros.size());
}

static void testFillsToLimit()
{
    BuildContext build(wireOptions());
    TableWriter writer(build);
    AOT_TEST_CHECK(!writer.allocate(8));
    AOT_TEST_CHECK(writer.allocate(vectorBytes - 8, 1) == 8);
    AOT_TEST_CHECK(writer.take().size() == vectorBytes);
    AOT_TEST_CHECK(!hitCount(build, LimitID::VectorBytes));
    AOT_TEST_CHECK(!count(build, UserDiagnosticKind::LimitExceeded, LimitID::VectorBytes));
}

static void testVectorBytesOverflow()
{
    BuildContext build(wireOptions());
    TableWriter writer(build);
    constexpr std::array<uint8_t, 4> bytes { 1, 2, 3, 4 };
    constexpr size_t sizeOfFirstRecord = 8;
    constexpr size_t sizeOfLargeRecord = vectorBytes + 1 - sizeOfFirstRecord;
    static_assert(!(sizeOfLargeRecord % alignof(uint64_t)));
    AOT_TEST_CHECK(!writer.allocate(sizeOfFirstRecord));

    AOT_TEST_CHECK(!writer.allocate(sizeOfLargeRecord));
    AOT_TEST_CHECK(writer.size() == sizeOfLargeRecord);
    writer.at<uint64_t>(sizeOfLargeRecord - sizeof(uint64_t)) = 1;

    AOT_TEST_CHECK(!writer.allocate(8));
    AOT_TEST_CHECK(!writer.append(std::span<const uint8_t> { bytes }));
    AOT_TEST_CHECK(!writer.appendUnshared(bytes));
    writer.appendUnshared(std::span { bytes }.first(1));
    AOT_TEST_CHECK(writer.size() == sizeOfLargeRecord);
    AOT_TEST_CHECK(writer.take().isEmpty());
    AOT_TEST_CHECK(hitCount(build, LimitID::VectorBytes) == 1);
    AOT_TEST_CHECK(count(build, UserDiagnosticKind::LimitExceeded, LimitID::VectorBytes) == 1);
    AOT_TEST_CHECK(build.diagnostics().userDiagnostics().size() == 1);
    AOT_TEST_CHECK(build.diagnostics().internalErrors().isEmpty());
}

static Vector<uint32_t> shapesAtDistances(uint32_t last, const Vector<uint32_t>& distances)
{
    Vector<uint32_t> shapes = distances.map([&](uint32_t distance) {
        return last - distance;
    });
    std::ranges::sort(shapes);
    return shapes;
}

static void checkShapeSet(const Vector<uint32_t>& ascendingShapes, PagedBitSet::Entries entries)
{
    AOT_TEST_CHECK(Typed::canEncodeShapeSet(ascendingShapes.span()));
    Typed::EncodedShapeSet encoded = Typed::encodeShapeSet(ascendingShapes.span());
    AOT_TEST_CHECK(encoded.entries == entries);
    uint32_t last = ascendingShapes.last();
    uint32_t count = last - ascendingShapes.first() + 1;
    for (uint32_t distance = 0; distance <= count; ++distance) {
        bool isMember = distance < count && std::ranges::binary_search(ascendingShapes, last - distance);
        AOT_TEST_CHECK(PagedBitSet::contains(encoded.words.span().data(), encoded.entries, distance) == isMember);
    }
    if (entries != PagedBitSet::Entries::Hashed)
        return;
    // Compiled code asks a hashed set for the distance of any shape, without looking whether it is in reach.
    AOT_TEST_CHECK(encoded.words.size() == PagedBitSet::numberOfWordsOfHashedSet(PagedBitSet::shiftIn(encoded.words[0])));
    for (uint32_t step = 1; step <= 100000; ++step) {
        AOT_TEST_CHECK(!PagedBitSet::contains(encoded.words.span().data(), encoded.entries, count + step));
        AOT_TEST_CHECK(!PagedBitSet::contains(encoded.words.span().data(), encoded.entries, 0u - step));
        AOT_TEST_CHECK(!PagedBitSet::contains(encoded.words.span().data(), encoded.entries, count + step * 42949u));
    }
    for (uint32_t held : spanReinterpretCast<const PagedBitSet::Slot>(encoded.words.span().subspan(1)).first(PagedBitSet::numberOfSlots(PagedBitSet::shiftIn(encoded.words[0]))))
        AOT_TEST_CHECK(PagedBitSet::contains(encoded.words.span().data(), encoded.entries, held) == (held < count && std::ranges::binary_search(ascendingShapes, last - held)));
}

static void testShapeSetHoldsItsMembers()
{
    using enum PagedBitSet::Entries;
    checkShapeSet({ 7 }, None);
    checkShapeSet({ 1, 64 }, Hashed);
    checkShapeSet({ 1, 65 }, Hashed);
    checkShapeSet({ 1, 511 }, Hashed);
    checkShapeSet({ 1, 512 }, Hashed);
    checkShapeSet({ 3, 4, 5, 70, 71, 500, 1000, 1001, 1063, 1064 }, Hashed);
    Vector<uint32_t> everySecond(1000, [](size_t index) {
        return static_cast<uint32_t>(10 + 2 * index);
    });
    checkShapeSet(everySecond, Narrow);
    Vector<uint32_t> neighbors(70, [](size_t index) {
        return static_cast<uint32_t>(10 + index + index / 60);
    });
    checkShapeSet(neighbors, None);
}

// A slot for each member takes less room than an entry for each page between them.
static void testFewFarMembersAreHashed()
{
    Vector<uint32_t> members;
    uint32_t shape = 5;
    for (unsigned numberOfMembers = 2; numberOfMembers <= 12; ++numberOfMembers) {
        members.shrink(0);
        for (unsigned index = 0; index < numberOfMembers; ++index)
            members.append(shape += 611 + 41 * index * numberOfMembers);
        checkShapeSet(members, PagedBitSet::Entries::Hashed);
        Typed::EncodedShapeSet encoded = Typed::encodeShapeSet(members.span());
        AOT_TEST_CHECK(PagedBitSet::numberOfSlots(PagedBitSet::shiftIn(encoded.words[0])) <= 4 * numberOfMembers);
        AOT_TEST_CHECK(PagedBitSet::multiplierIn(encoded.words[0]) & 1);
    }
}

// Entries save nothing where every page differs from every other.
static void testDenseShapeSetHasNoEntries()
{
    Vector<uint32_t> distances;
    for (uint32_t page = 0; page < 100; ++page) {
        for (uint32_t bit = 0; bit < 16; ++bit) {
            if ((page + 1) >> bit & 1)
                distances.append(page * PagedBitSet::bitsPerPage + bit);
        }
    }
    checkShapeSet(shapesAtDistances(30000, distances), PagedBitSet::Entries::None);
}

static void testShapeSetWithFarMembersIsSmall()
{
    constexpr uint32_t span = PagedBitSet::maxNumberInSlot + 1;
    Typed::EncodedShapeSet encoded = Typed::encodeShapeSet(Vector<uint32_t> { 1, span }.span());
    AOT_TEST_CHECK(encoded.entries == PagedBitSet::Entries::Hashed);
    AOT_TEST_CHECK(encoded.words.size() == 2);
    checkShapeSet({ 1, span }, PagedBitSet::Entries::Hashed);
    checkShapeSet({ 1, 2, 3, 4, span }, PagedBitSet::Entries::Hashed);
    AOT_TEST_CHECK(Typed::encodeShapeSet(Vector<uint32_t> { 1, 2, 3, 4, span }.span()).words.size() == 3);
    // A slot cannot say a distance beyond that.
    checkShapeSet({ 1, span + 1 }, PagedBitSet::Entries::Narrow);

    // Too many members for a slot each: 40 clusters of 40 neighbors.
    Vector<uint32_t> clusters(1600, [](size_t index) {
        return static_cast<uint32_t>(1 + index / 40 * 2500 + index % 40);
    });
    Typed::EncodedShapeSet paged = Typed::encodeShapeSet(clusters.span());
    AOT_TEST_CHECK(paged.entries == PagedBitSet::Entries::Narrow);
    AOT_TEST_CHECK(paged.words.sizeInBytes() * 7 < clusters.last() / 8);
    checkShapeSet(clusters, PagedBitSet::Entries::Narrow);
}

// Every fourth page holds shapes, and no two of them the same.
static void testShapeSetWithManyDistinctPagesHasWideEntries()
{
    Vector<uint32_t> distances;
    for (uint32_t page = 0; page < 400; ++page) {
        for (uint32_t bit = 0; bit < 16; ++bit) {
            if ((page + 1) >> bit & 1)
                distances.append(4 * page * PagedBitSet::bitsPerPage + bit);
        }
    }
    checkShapeSet(shapesAtDistances(200000, distances), PagedBitSet::Entries::Wide);
}

static void testShapeSetsAtEqualDistancesAreEqual()
{
    Vector<uint32_t> distances { 0, 3, 90, 4000 };
    Typed::EncodedShapeSet first = Typed::encodeShapeSet(shapesAtDistances(5000, distances).span());
    Typed::EncodedShapeSet second = Typed::encodeShapeSet(shapesAtDistances(77777, distances).span());
    AOT_TEST_CHECK(first.words == second.words && first.entries == second.entries);
}

static void testShapeSetTooWideToEncode()
{
    AOT_TEST_CHECK(Typed::canEncodeShapeSet(Vector<uint32_t> { 1, 4000000 }.span()));
    AOT_TEST_CHECK(!Typed::canEncodeShapeSet(Vector<uint32_t> { 1, 2000000000 }.span()));
}

static FunctionRecordData functionRecordWithBits(uint32_t bits) { return std::bit_cast<FunctionRecordData>(bits); }

// Program tables that hold nothing but the three tables behind ProgramTables::recordOfFunction().
class TablesWithFunctionRecords {
public:
    explicit TablesWithFunctionRecords(const Typed::NumberedFunctionRecords& numbered)
    {
        m_storage.fill(0, sizeof(ProgramTables) / sizeof(uint64_t) + 1);
        append(ProgramTableIndex::functionRecordNumbers, numbered.numbers);
        append(ProgramTableIndex::functionRecords, numbered.records);
        append(ProgramTableIndex::functionRecordsByFunction, numbered.recordsByFunction);
    }

    uint32_t bitsOfRecord(uint32_t function) const { return std::bit_cast<uint32_t>(reinterpret_cast<const ProgramTables*>(m_storage.span().data())->recordOfFunction(function)); }

private:
    template<typename T>
    void append(ProgramTableIndex index, const Vector<T>& table)
    {
        size_t offset = m_storage.size() * sizeof(uint64_t);
        m_storage.grow(m_storage.size() + table.size() * sizeof(T) / sizeof(uint64_t) + 1);
        memcpySpan(asMutableByteSpan(m_storage.mutableSpan()).subspan(offset), asByteSpan(table.span()));
        reinterpret_cast<ProgramTables*>(m_storage.mutableSpan().data())->ranges[static_cast<unsigned>(index)] = { static_cast<uint32_t>(offset), static_cast<uint32_t>(table.size()) };
    }

    Vector<uint64_t> m_storage;
};

static void testCommonFunctionRecordsGetNumbers()
{
    Vector<FunctionRecordData> records { functionRecordWithBits(7), functionRecordWithBits(9), functionRecordWithBits(9), functionRecordWithBits(3), functionRecordWithBits(9), functionRecordWithBits(3) };
    Typed::NumberedFunctionRecords numbered = Typed::numberFunctionRecords(records.span());
    AOT_TEST_CHECK(numbered.records.size() == 3 && numbered.recordsByFunction.isEmpty());
    AOT_TEST_CHECK(numbered.numbers == (Vector<uint8_t> { 2, 0, 0, 1, 0, 1 }));
    TablesWithFunctionRecords tables(numbered);
    for (uint32_t function = 0; function < records.size(); ++function)
        AOT_TEST_CHECK(tables.bitsOfRecord(function) == std::bit_cast<uint32_t>(records[function]));
}

// 300 pairs, the pair n for n + 1 functions. The 45 rarest have no number.
static void testMoreFunctionRecordsThanNumbers()
{
    constexpr uint32_t numberOfPairs = 300;
    Vector<FunctionRecordData> records;
    for (uint32_t round = 0; round < numberOfPairs; ++round) {
        for (uint32_t pair = round; pair < numberOfPairs; ++pair)
            records.append(functionRecordWithBits(1000 + pair));
    }
    Typed::NumberedFunctionRecords numbered = Typed::numberFunctionRecords(records.span());
    constexpr uint32_t numberWithoutNumber = numberOfPairs - functionRecordIsListedByFunction;
    AOT_TEST_CHECK(numbered.records.size() == functionRecordIsListedByFunction);
    AOT_TEST_CHECK(numbered.recordsByFunction.size() == numberWithoutNumber * (numberWithoutNumber + 1) / 2);
    AOT_TEST_CHECK(std::ranges::is_sorted(numbered.recordsByFunction, { }, &FunctionRecordByFunctionData::function));
    AOT_TEST_CHECK(std::bit_cast<uint32_t>(numbered.records[0]) == 1000 + numberOfPairs - 1);
    TablesWithFunctionRecords tables(numbered);
    for (uint32_t function = 0; function < records.size(); ++function)
        AOT_TEST_CHECK(tables.bitsOfRecord(function) == std::bit_cast<uint32_t>(records[function]));
}

std::span<const NativeTest> tableWriterTests()
{
    static constexpr NativeTest tests[] = {
        { "place/function-records/common-pairs-get-numbers"_s, testCommonFunctionRecordsGetNumbers },
        { "place/function-records/more-pairs-than-numbers"_s, testMoreFunctionRecordsThanNumbers },
        { "place/shape-sets/holds-its-members"_s, testShapeSetHoldsItsMembers },
        { "place/shape-sets/few-far-members-are-hashed"_s, testFewFarMembersAreHashed },
        { "place/shape-sets/dense-set-has-no-entries"_s, testDenseShapeSetHasNoEntries },
        { "place/shape-sets/far-members-are-small"_s, testShapeSetWithFarMembersIsSmall },
        { "place/shape-sets/many-distinct-pages-have-wide-entries"_s, testShapeSetWithManyDistinctPagesHasWideEntries },
        { "place/shape-sets/equal-distances-are-equal"_s, testShapeSetsAtEqualDistancesAreEqual },
        { "place/shape-sets/too-wide-to-encode"_s, testShapeSetTooWideToEncode },
        { "place/table-writer/shares-appended-bytes"_s, testSharesAppendedBytes },
        { "place/table-writer/shares-only-appended-bytes"_s, testSharesOnlyAppendedBytes },
        { "place/table-writer/fills-to-limit"_s, testFillsToLimit },
        { "place/table-writer/vector-bytes-overflow"_s, testVectorBytesOverflow },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
