/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTIR.h"
#include "AOTLayoutData.h"
#include "JSExportMacros.h"
#include <algorithm>
#include <compare>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr uint32_t shapesPerBlock = 256;
inline constexpr uint32_t maxTestableBlocks = 128;
inline constexpr uint32_t maxShapesForBlockTests = shapesPerBlock << 16;
inline constexpr uint32_t immediateBitsWindow = 64;
inline constexpr uint32_t firstShapeWithLongConstant = 4096;
static_assert(shapesPerBlock == 1 << 8);
static_assert((shapeWordOf(1) & shapeBlockMarker) == shapeBlockMarker && (shapeWordOf(maxTestableBlocks * shapesPerBlock) & shapeBlockMarker) == shapeBlockMarker);

enum class ShapeTestForm : uint8_t {
    NeverPasses,
    AlwaysPasses,
    OneShape,
    UpTo,
    From,
    Range,
    Block,
    BlockOrEarlier,
    ImmediateBits,
    BitSet,
};

struct ShapeTest {
    constexpr uint32_t last() const { return first + count - 1; }

    friend constexpr bool operator==(const ShapeTest&, const ShapeTest&) = default;

    ShapeTestForm form { ShapeTestForm::NeverPasses };
    uint8_t blockByte { 0 };
    uint32_t first { 0 };
    uint32_t count { 0 };
    uint32_t bitSetOffset { noBitSet };
    uint64_t bits { 0 };
};

constexpr bool holdsForAnyCell(ShapeTestForm form)
{
    switch (form) {
    case ShapeTestForm::NeverPasses:
    case ShapeTestForm::OneShape:
    case ShapeTestForm::UpTo:
    case ShapeTestForm::Range:
    case ShapeTestForm::Block:
    case ShapeTestForm::BlockOrEarlier:
    case ShapeTestForm::ImmediateBits:
    case ShapeTestForm::BitSet:
        return true;
    case ShapeTestForm::AlwaysPasses:
    case ShapeTestForm::From:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr bool hasBits(ShapeTestForm form)
{
    return form == ShapeTestForm::ImmediateBits || form == ShapeTestForm::BitSet;
}

constexpr unsigned numberOfNonzeroHalfwords(uint64_t bits)
{
    unsigned halfwords = 0;
    for (unsigned shift = 0; shift < 64; shift += 16)
        halfwords += !!(bits >> shift & 0xffff);
    return halfwords;
}

constexpr unsigned instructionsFor(const ShapeTest& test)
{
    unsigned longConstant = test.count && test.last() >= firstShapeWithLongConstant ? 1 : 0;
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
        return 5 + numberOfNonzeroHalfwords(test.bits) + longConstant;
    case ShapeTestForm::BitSet:
        return 9 + longConstant;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

inline bool passes(const ShapeTest& test, uint32_t shape, std::span<const uint32_t> ascendingTestedShapes)
{
    uint32_t distanceFromLast = test.last() - shape;
    uint32_t blockHalfword = shapeWordOf(shape) >> 8 & 0xffff;
    switch (test.form) {
    case ShapeTestForm::NeverPasses:
        return false;
    case ShapeTestForm::AlwaysPasses:
        return true;
    case ShapeTestForm::OneShape:
        return shape == test.first;
    case ShapeTestForm::UpTo:
        return shape <= test.last();
    case ShapeTestForm::From:
        return shape >= test.first;
    case ShapeTestForm::Range:
        return distanceFromLast < test.count;
    case ShapeTestForm::Block:
        return blockHalfword == blockTestHalfword(test.blockByte);
    case ShapeTestForm::BlockOrEarlier:
        return blockHalfword >= blockTestHalfword(test.blockByte);
    case ShapeTestForm::ImmediateBits:
        return distanceFromLast < test.count && (test.bits >> distanceFromLast & 1);
    case ShapeTestForm::BitSet:
        return distanceFromLast < test.count && std::ranges::binary_search(ascendingTestedShapes, shape);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct ShapeRange {
    bool isEmpty() const { return !count; }
    uint32_t end() const { return first + count; }

    uint32_t first { 0 };
    uint32_t count { 0 };
};

struct PlacedShape {
    LayoutID layout;
    bool keysAreNeverEnumerated { false };
    OptionSet<ObjectMutation> impossibleMutations;
};

struct ShapeTestWithinKnownSet {
    LayoutSetID tested;
    LayoutSetID known;
    ShapeTest test;
};

struct ShapeTableParts {
    Vector<PlacedShape> shapes;
    Vector<ShapeRange> shapesOfLayout;
    Vector<uint32_t> shapeOfAllocationClass;
    uint32_t firstEnvironmentShape { 0 };
    Vector<ShapeTest> testOfSet;
    BitVector setsWithTest;
    Vector<ShapeTestWithinKnownSet> testsWithinKnownSets;
    Vector<uint32_t> firstMemberShape;
    Vector<uint32_t> memberShapes;
    Vector<uint32_t> shapeCountPrefixSums;
    BitVector shapeSetBits;
};

class ShapeTable {
public:
    ShapeTable() = default;
    explicit ShapeTable(ShapeTableParts&& parts)
        : m_parts(WTF::move(parts))
    {
    }

    uint32_t numberOfShapes() const { return m_parts.shapes.isEmpty() ? 0 : m_parts.shapes.size() - 1; }
    uint32_t firstEnvironmentShape() const { return m_parts.firstEnvironmentShape; }
    const PlacedShape& shape(uint32_t number) const { return m_parts.shapes[number]; }
    bool isAssigned(uint32_t number) const { return number < m_parts.shapes.size() && m_parts.shapes[number].layout; }

    ShapeRange shapesOf(LayoutID id) const { return id.index() < m_parts.shapesOfLayout.size() ? m_parts.shapesOfLayout[id.index()] : ShapeRange { }; }
    uint32_t generalShapeOf(LayoutID id) const { return shapesOf(id).first; }
    uint32_t shapeOf(LayoutID id, AllocationClassID allocationClass) const
    {
        if (allocationClass && allocationClass.index() < m_parts.shapeOfAllocationClass.size() && m_parts.shapeOfAllocationClass[allocationClass.index()])
            return m_parts.shapeOfAllocationClass[allocationClass.index()];
        return generalShapeOf(id);
    }

    bool hasTestOf(LayoutSetID set) const { return set && m_parts.setsWithTest.get(set.index()); }
    JS_EXPORT_PRIVATE const ShapeTest& testOf(LayoutSetID tested) const;
    JS_EXPORT_PRIVATE const ShapeTest& testOf(LayoutSetID tested, LayoutSetID known) const;
    JS_EXPORT_PRIVATE std::span<const uint32_t> memberShapes(LayoutSetID) const;

    uint32_t shapeCountInRange(uint32_t first, uint32_t last) const { return first > last ? 0 : m_parts.shapeCountPrefixSums[last] - m_parts.shapeCountPrefixSums[first - 1]; }
    const BitVector& shapeSetBits() const { return m_parts.shapeSetBits; }
    std::span<const ShapeTestWithinKnownSet> testsWithinKnownSets() const { return m_parts.testsWithinKnownSets.span(); }

    JS_EXPORT_PRIVATE ShapeTestData dataOf(const ShapeTest&) const;

private:
    ShapeTableParts m_parts;
    ShapeTest m_neverPasses;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
