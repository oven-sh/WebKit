/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "Helpers/Test.h"
#include <algorithm>
#include <array>
#include <span>
#include <wtf/MultiArrayList.h>
#include <wtf/StdLibExtras.h>

namespace TestWebKitAPI {

namespace {

struct Row {
    uint8_t tag { 7 };
    uint64_t wide { 0 };
    uint16_t half { 0 };
    uint32_t word { 9 };

    friend bool operator==(const Row&, const Row&) = default;
};

using Rows = MultiArrayList<Row, &Row::tag, &Row::wide, &Row::half, &Row::word>;

struct alignas(16) Vector128 {
    uint64_t low { 0 };
    uint64_t high { 0 };
};

struct VectorRow {
    uint8_t tag { 0 };
    Vector128 vector;
    uint32_t word { 0 };
};

using VectorRows = MultiArrayList<VectorRow, &VectorRow::tag, &VectorRow::vector, &VectorRow::word>;

struct Single {
    uint64_t value;
};

using Singles = MultiArrayList<Single, &Single::value>;

Row createRow(uint32_t index)
{
    return { static_cast<uint8_t>(index * 3), index * 0x100000001ull, static_cast<uint16_t>(index * 5), index * 7 };
}

Rows createRows(uint32_t count)
{
    Rows list;
    for (uint32_t index = 0; index < count; ++index)
        list.append(createRow(index));
    return list;
}

void expectRows(const Rows& list, uint32_t count)
{
    ASSERT_EQ(list.size(), count);
    for (uint32_t index = 0; index < count; ++index)
        EXPECT_EQ(list.get(index), createRow(index));
}

struct ColumnRange {
    uintptr_t begin;
    uintptr_t end;
    size_t alignment;
};

template<typename Field>
ColumnRange columnRange(std::span<Field> items, uint32_t capacity)
{
    auto begin = reinterpret_cast<uintptr_t>(items.data());
    return { begin, begin + sizeof(Field) * capacity, alignof(Field) };
}

template<typename T, auto... fields>
void expectAlignedDisjointColumns(MultiArrayList<T, fields...>& list)
{
    std::array<ColumnRange, sizeof...(fields)> columns { columnRange(list.template items<fields>(), list.capacity())... };
    for (auto& column : columns)
        EXPECT_EQ(column.begin % column.alignment, 0U);

    std::ranges::sort(columns, { }, &ColumnRange::begin);
    for (size_t i = 1; i < columns.size(); ++i)
        EXPECT_LE(columns[i - 1].end, columns[i].begin);
    EXPECT_EQ(columns.back().end - columns.front().begin, list.sizeInBytes());
}

template<typename List>
void testColumnAlignment()
{
    for (uint32_t capacity : { 1U, 3U, 5U, 7U, 9U, 31U }) {
        List reserved;
        reserved.reserveCapacity(capacity);
        EXPECT_EQ(reserved.capacity(), capacity);
        expectAlignedDisjointColumns(reserved);

        List shrunk;
        shrunk.reserveCapacity(64);
        shrunk.resize(capacity);
        shrunk.shrinkToFit();
        EXPECT_EQ(shrunk.capacity(), capacity);
        expectAlignedDisjointColumns(shrunk);
    }

    List list;
    uint32_t capacity = 0;
    for (unsigned i = 0; i < 2000; ++i) {
        list.append({ });
        if (list.capacity() == capacity)
            continue;
        capacity = list.capacity();
        expectAlignedDisjointColumns(list);
    }
}

} // anonymous namespace

TEST(WTF_MultiArrayList, RowBytes)
{
    static_assert(Rows::rowBytes == 15);
    static_assert(VectorRows::rowBytes == 21);
    static_assert(Singles::rowBytes == 8);

    Rows list;
    EXPECT_EQ(list.sizeInBytes(), 0U);
    list.reserveCapacity(31);
    EXPECT_EQ(list.sizeInBytes(), 15U * 31U);
    for (uint32_t index = 0; index < 100; ++index) {
        list.append(createRow(index));
        EXPECT_EQ(list.sizeInBytes(), 15U * list.capacity());
    }
}

TEST(WTF_MultiArrayList, ColumnsAreAligned)
{
    testColumnAlignment<Rows>();
    testColumnAlignment<VectorRows>();
}

TEST(WTF_MultiArrayList, GrowthKeepsRows)
{
    constexpr uint32_t count = 10000;

    Rows list;
    EXPECT_TRUE(list.isEmpty());
    for (uint32_t index = 0; index < count; ++index)
        EXPECT_EQ(list.append(createRow(index)), index);
    EXPECT_FALSE(list.isEmpty());
    EXPECT_EQ(list.size(), count);
    EXPECT_GE(list.capacity(), count);

    const Rows& constList = list;
    auto tags = list.items<&Row::tag>();
    auto wides = list.items<&Row::wide>();
    auto halves = constList.items<&Row::half>();
    auto words = constList.items<&Row::word>();
    ASSERT_EQ(tags.size(), count);
    ASSERT_EQ(wides.size(), count);
    ASSERT_EQ(halves.size(), count);
    ASSERT_EQ(words.size(), count);

    for (uint32_t index = 0; index < count; ++index) {
        Row expected = createRow(index);
        EXPECT_EQ(list.get(index), expected);

        EXPECT_EQ(constList.at<&Row::tag>(index), expected.tag);
        EXPECT_EQ(constList.at<&Row::wide>(index), expected.wide);
        EXPECT_EQ(list.at<&Row::half>(index), expected.half);
        EXPECT_EQ(list.at<&Row::word>(index), expected.word);

        EXPECT_EQ(tags[index], expected.tag);
        EXPECT_EQ(wides[index], expected.wide);
        EXPECT_EQ(halves[index], expected.half);
        EXPECT_EQ(words[index], expected.word);
    }
}

TEST(WTF_MultiArrayList, AppendAliasedRow)
{
    constexpr uint64_t firstValue = 0x1122334455667788ull;

    Singles list;
    list.append({ firstValue });
    while (list.size() < list.capacity())
        list.append({ list.size() });
    uint32_t fullCapacity = list.capacity();

    uint32_t index = list.append(*reinterpret_cast<const Single*>(&list.at<&Single::value>(0)));
    EXPECT_EQ(index, fullCapacity);
    EXPECT_GT(list.capacity(), fullCapacity);
    EXPECT_EQ(list.at<&Single::value>(index), firstValue);
    EXPECT_EQ(list.at<&Single::value>(0), firstValue);
}

TEST(WTF_MultiArrayList, GrowFillsWithDefaults)
{
    Rows list;
    list.resize(20);
    ASSERT_EQ(list.size(), 20U);
    for (uint32_t index = 0; index < 20; ++index) {
        EXPECT_EQ(list.at<&Row::tag>(index), 7U);
        EXPECT_EQ(list.at<&Row::wide>(index), 0U);
        EXPECT_EQ(list.at<&Row::half>(index), 0U);
        EXPECT_EQ(list.at<&Row::word>(index), 9U);
    }

    for (uint32_t index = 0; index < 20; ++index)
        list.set(index, createRow(index));

    list.shrink(10);
    list.growWithoutInitializing(20);
    expectRows(list, 20);

    uint32_t reallocatedSize = list.capacity() + 1;
    list.growWithoutInitializing(reallocatedSize);
    EXPECT_EQ(list.size(), reallocatedSize);
    for (uint32_t index = 0; index < 20; ++index)
        EXPECT_EQ(list.get(index), createRow(index));

    list.shrink(10);
    list.grow(20);
    ASSERT_EQ(list.size(), 20U);
    for (uint32_t index = 0; index < 20; ++index)
        EXPECT_EQ(list.get(index), index < 10 ? createRow(index) : Row { });
}

TEST(WTF_MultiArrayList, SwapRemoveAndTakeLast)
{
    Rows list = createRows(5);

    list.swapRemove(1);
    ASSERT_EQ(list.size(), 4U);
    EXPECT_EQ(list.get(0), createRow(0));
    EXPECT_EQ(list.get(1), createRow(4));
    EXPECT_EQ(list.get(2), createRow(2));
    EXPECT_EQ(list.get(3), createRow(3));

    list.swapRemove(3);
    ASSERT_EQ(list.size(), 3U);
    EXPECT_EQ(list.get(0), createRow(0));
    EXPECT_EQ(list.get(1), createRow(4));
    EXPECT_EQ(list.get(2), createRow(2));

    EXPECT_EQ(list.takeLast(), createRow(2));
    EXPECT_EQ(list.takeLast(), createRow(4));
    EXPECT_EQ(list.size(), 1U);
    EXPECT_EQ(list.takeLast(), createRow(0));
    EXPECT_TRUE(list.isEmpty());
}

TEST(WTF_MultiArrayList, SetAndFill)
{
    Rows list;
    list.resize(8);
    list.set(3, createRow(300));
    for (uint32_t index = 0; index < 8; ++index)
        EXPECT_EQ(list.get(index), index == 3 ? createRow(300) : Row { });

    list.fill<&Row::half>(0xbeef);
    for (uint32_t index = 0; index < 8; ++index) {
        Row expected = index == 3 ? createRow(300) : Row { };
        expected.half = 0xbeef;
        EXPECT_EQ(list.get(index), expected);
    }

    list.at<&Row::word>(5) = 77;
    EXPECT_EQ(list.get(4).word, 9U);
    EXPECT_EQ(list.get(5).word, 77U);
    EXPECT_EQ(list.get(6).word, 9U);
}

TEST(WTF_MultiArrayList, CopyAndMove)
{
    Rows source = createRows(50);

    Rows copy(source);
    expectRows(copy, 50);
    copy.set(0, createRow(900));
    source.set(1, createRow(901));
    EXPECT_EQ(source.get(0), createRow(0));
    EXPECT_EQ(copy.get(1), createRow(1));
    source.set(1, createRow(1));

    Rows grownByAssignment = createRows(3);
    grownByAssignment = source;
    expectRows(grownByAssignment, 50);
    expectRows(source, 50);

    Rows shrunkByAssignment = createRows(200);
    uint32_t capacity = shrunkByAssignment.capacity();
    shrunkByAssignment = source;
    expectRows(shrunkByAssignment, 50);
    EXPECT_EQ(shrunkByAssignment.capacity(), capacity);

    Rows& alias = source;
    source = alias;
    expectRows(source, 50);
    source = WTF::move(alias);
    expectRows(source, 50);

    Rows moved(WTF::move(source));
    expectRows(moved, 50);
    SUPPRESS_USE_AFTER_MOVE EXPECT_TRUE(source.isEmpty());
    SUPPRESS_USE_AFTER_MOVE EXPECT_EQ(source.capacity(), 0U);

    Rows moveAssigned = createRows(3);
    moveAssigned = WTF::move(moved);
    expectRows(moveAssigned, 50);
    SUPPRESS_USE_AFTER_MOVE EXPECT_TRUE(moved.isEmpty());
    SUPPRESS_USE_AFTER_MOVE EXPECT_EQ(moved.capacity(), 0U);

    SUPPRESS_USE_AFTER_MOVE moved.append(createRow(0));
    expectRows(moved, 1);

    Rows empty;
    Rows emptyCopy(empty);
    EXPECT_TRUE(emptyCopy.isEmpty());
    EXPECT_EQ(emptyCopy.capacity(), 0U);
}

TEST(WTF_MultiArrayList, Swap)
{
    Rows first = createRows(5);
    Rows second = createRows(40);
    uint32_t firstCapacity = first.capacity();
    uint32_t secondCapacity = second.capacity();

    first.swap(second);
    expectRows(first, 40);
    expectRows(second, 5);
    EXPECT_EQ(first.capacity(), secondCapacity);
    EXPECT_EQ(second.capacity(), firstCapacity);

    Rows empty;
    empty.swap(first);
    expectRows(empty, 40);
    EXPECT_TRUE(first.isEmpty());
    EXPECT_EQ(first.capacity(), 0U);
}

TEST(WTF_MultiArrayList, ShrinkKeepsCapacity)
{
    Rows list = createRows(100);
    uint32_t capacity = list.capacity();
    const uint64_t* wideColumn = list.items<&Row::wide>().data();

    list.shrink(10);
    expectRows(list, 10);
    EXPECT_EQ(list.capacity(), capacity);
    EXPECT_EQ(list.items<&Row::wide>().data(), wideColumn);

    list.resize(5);
    expectRows(list, 5);
    EXPECT_EQ(list.capacity(), capacity);

    list.reserveCapacity(capacity - 1);
    EXPECT_EQ(list.capacity(), capacity);

    list.clear();
    EXPECT_TRUE(list.isEmpty());
    EXPECT_EQ(list.capacity(), capacity);
    EXPECT_EQ(list.sizeInBytes(), Rows::rowBytes * capacity);
    EXPECT_EQ(list.items<&Row::wide>().data(), wideColumn);
}

TEST(WTF_MultiArrayList, ClearAndFree)
{
    Rows list = createRows(100);
    list.clearAndFree();
    EXPECT_TRUE(list.isEmpty());
    EXPECT_EQ(list.capacity(), 0U);
    EXPECT_EQ(list.sizeInBytes(), 0U);

    list.clearAndFree();
    EXPECT_EQ(list.capacity(), 0U);

    for (uint32_t index = 0; index < 10; ++index)
        list.append(createRow(index));
    expectRows(list, 10);
}

TEST(WTF_MultiArrayList, ShrinkToFit)
{
    Rows list = createRows(33);
    EXPECT_GT(list.capacity(), 33U);
    list.shrinkToFit();
    EXPECT_EQ(list.capacity(), 33U);
    expectRows(list, 33);

    const uint64_t* wideColumn = list.items<&Row::wide>().data();
    list.shrinkToFit();
    EXPECT_EQ(list.capacity(), 33U);
    EXPECT_EQ(list.items<&Row::wide>().data(), wideColumn);

    list.clear();
    list.shrinkToFit();
    EXPECT_EQ(list.capacity(), 0U);
    EXPECT_EQ(list.sizeInBytes(), 0U);
}

} // namespace TestWebKitAPI
