/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTID.h"
#include "AOTLayoutData.h"
#include "AOTPagedBitSet.h"
#include "AOTSchemaEnums.h"
#include "AOTTypeIDs.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr uint32_t immediateBitsWindow = 64;

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

    ShapeTestForm form { ShapeTestForm::NeverPasses };
    uint8_t blockByte { 0 };
    uint32_t first { 0 };
    uint32_t count { 0 };
    uint64_t bits { 0 };
};

constexpr bool hasBits(ShapeTestForm form)
{
    return form == ShapeTestForm::ImmediateBits || form == ShapeTestForm::BitSet;
}

constexpr bool holdsForAnyCell(ShapeTestForm form)
{
    return form != ShapeTestForm::AlwaysPasses && form != ShapeTestForm::From;
}

constexpr uint64_t numberOfPagesInShapeSet(uint32_t count) { return PagedBitSet::numberOfPages(static_cast<uint64_t>(count) + 1); }
inline constexpr uint64_t maxPagesInShortShapeSet = 8;

struct EncodedShapeSet {
    Vector<uint64_t> words;
    PagedBitSet::Entries entries { PagedBitSet::Entries::None };
};

JS_EXPORT_PRIVATE EncodedShapeSet encodeShapeSet(std::span<const uint32_t> ascendingShapes);

struct ShapeRange {
    bool isEmpty() const { return !count; }
    uint32_t end() const { return first + count; }

    uint32_t first { 0 };
    uint32_t count { 0 };
};

struct DistanceOfShapes {
    ShapeRange shapes;
    uint32_t distance { 0 };
};

struct DistancesByComparison {
    Vector<DistanceOfShapes, 8> stretches;
    bool comparesWithFirstShapes { false };

    JS_EXPORT_PRIVATE size_t numberOfComparisons() const;
    JS_EXPORT_PRIVATE unsigned numberOfInstructions() const;
    JS_EXPORT_PRIVATE uint32_t distanceOf(uint32_t shape) const;
};
JS_EXPORT_PRIVATE DistancesByComparison distancesByComparison(std::span<const DistanceOfShapes> members);

enum class DistanceForm : uint8_t {
    Comparisons,
    FlatTable,
    HashedTable,
    PagedTable,
};

inline constexpr uint32_t maxShapesInFlatTable = 1024;

struct DistancesByShape {
    JS_EXPORT_PRIVATE unsigned numberOfInstructions() const;
    JS_EXPORT_PRIVATE uint32_t distanceOf(uint32_t shape) const;
    uint32_t distanceOfNoMember() const { return logBytesPerDistance ? std::numeric_limits<uint16_t>::max() : std::numeric_limits<uint8_t>::max(); }

    DistanceForm form { DistanceForm::Comparisons };
    uint8_t logBytesPerDistance { 0 };
    uint8_t logShapesPerPage { 0 };
    uint8_t logBytesPerPageEntry { 0 };
    uint8_t hashShift { 0 };
    uint32_t hashMultiplier { 0 };
    uint32_t lastShape { 0 };
    uint32_t numberOfShapes { 0 };
    DistancesByComparison comparisons;
    Vector<uint8_t> table;
};

JS_EXPORT_PRIVATE DistancesByShape encodeDistancesByShape(std::span<const DistanceOfShapes> members, DistanceForm);
JS_EXPORT_PRIVATE DistancesByShape distancesByShape(std::span<const DistanceOfShapes> members);

struct PlacedShape {
    LayoutID layout;
    OptionSet<ObjectMutation> impossibleMutations;
};

struct ShapeTableParts {
    Vector<PlacedShape> shapes;
    Vector<ShapeRange> shapesOfLayout;
    uint32_t firstEnvironmentShape { 0 };
};

struct ShapeTestsOfSets {
    struct OfSet {
        ShapeTest test;
        std::optional<EncodedShapeSet> encoded;
    };
    struct Part {
        Lock lock;
        HashMap<uint32_t, std::unique_ptr<OfSet>> sets;
    };
    std::array<Part, 64> parts;
};

class ShapeTable {
public:
    using ShapeList = Vector<uint32_t, 16>;

    ShapeTable() = default;
    explicit ShapeTable(ShapeTableParts&& parts)
        : m_parts(WTF::move(parts))
        , m_testsOfSets(makeUniqueWithoutFastMallocCheck<ShapeTestsOfSets>())
    {
    }

    uint32_t numberOfShapes() const { return m_parts.shapes.isEmpty() ? 0 : m_parts.shapes.size() - 1; }
    uint32_t firstEnvironmentShape() const { return m_parts.firstEnvironmentShape; }
    const PlacedShape& shape(uint32_t number) const { return m_parts.shapes[number]; }
    bool isAssigned(uint32_t number) const { return number < m_parts.shapes.size() && m_parts.shapes[number].layout; }

    ShapeRange shapesOf(LayoutID id) const { return id.index() < m_parts.shapesOfLayout.size() ? m_parts.shapesOfLayout[id.index()] : ShapeRange { }; }
    uint32_t generalShapeOf(LayoutID id) const { return shapesOf(id).first; }

    JS_EXPORT_PRIVATE void appendMemberShapes(std::span<const LayoutID>, ShapeList& ascendingShapes) const;
    JS_EXPORT_PRIVATE ShapeTest testOfShapes(std::span<const uint32_t> ascendingShapes) const;
    JS_EXPORT_PRIVATE ShapeTest testOfShapes(std::span<const uint32_t> ascendingTestedShapes, std::span<const uint32_t> ascendingKnownShapes) const;
    JS_EXPORT_PRIVATE ShapeTest testOf(std::span<const LayoutID>) const;
    JS_EXPORT_PRIVATE ShapeTest testOf(LayoutSetID, std::span<const LayoutID> members) const;
    JS_EXPORT_PRIVATE EncodedShapeSet encodedShapesOf(LayoutSetID, std::span<const LayoutID> members) const;

private:
    ShapeTestsOfSets::OfSet* testsKeptOf(LayoutSetID, std::span<const LayoutID> members, ShapeTestsOfSets::Part*&) const;

    ShapeTableParts m_parts;
    std::unique_ptr<ShapeTestsOfSets> m_testsOfSets;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
