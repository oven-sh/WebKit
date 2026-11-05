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
    // Block, BlockOrEarlier: the second byte of the shape words of the block.
    uint8_t blockByte { 0 };
    uint32_t first { 0 };
    uint32_t count { 0 };
    // ImmediateBits: bit n is set where the shape `last() - n` passes.
    uint64_t bits { 0 };
};

constexpr bool hasBits(ShapeTestForm form)
{
    return form == ShapeTestForm::ImmediateBits || form == ShapeTestForm::BitSet;
}

// False: the form passes words that cells without a shape hold, so only a test within known shapes has it.
constexpr bool holdsForAnyCell(ShapeTestForm form)
{
    return form != ShapeTestForm::AlwaysPasses && form != ShapeTestForm::From;
}

// The set of a test holds the distance of each shape from the last one, and a clear bit behind the greatest distance.
constexpr uint64_t numberOfPagesInShapeSet(uint32_t count) { return PagedBitSet::numberOfPages(static_cast<uint64_t>(count) + 1); }
// A set of so few pages has no entries.
inline constexpr uint64_t maxPagesInShortShapeSet = 8;

struct EncodedShapeSet {
    Vector<uint64_t> words;
    PagedBitSet::Entries entries { PagedBitSet::Entries::None };
};

// Equal sets of distances give equal words.
JS_EXPORT_PRIVATE EncodedShapeSet encodeShapeSet(std::span<const uint32_t> ascendingShapes);

struct ShapeRange {
    bool isEmpty() const { return !count; }
    uint32_t end() const { return first + count; }

    uint32_t first { 0 };
    uint32_t count { 0 };
};

// The layouts that hold a field keep it at different offsets, and code finds by the shape how far behind the lowest of them it lies.
struct DistanceOfShapes {
    ShapeRange shapes;
    uint32_t distance { 0 };
};

struct DistancesByComparison {
    // Ascending and apart. Neighbors differ in their distance.
    Vector<DistanceOfShapes, 8> stretches;
    // The distance is that of the last stretch whose first shape is not above the shape. Otherwise: that of the stretch in whose range the shape lies.
    bool comparesWithFirstShapes { false };

    JS_EXPORT_PRIVATE size_t numberOfComparisons() const;
    JS_EXPORT_PRIVATE unsigned numberOfInstructions() const;
    JS_EXPORT_PRIVATE uint32_t distanceOf(uint32_t shape) const;
};
// `members`: apart, none empty, in any order.
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

    DistanceForm form { DistanceForm::Comparisons };
    uint8_t logBytesPerDistance { 0 };
    uint8_t logShapesPerPage { 0 };
    uint8_t logBytesPerPageEntry { 0 };
    uint8_t hashShift { 0 };
    uint32_t hashMultiplier { 0 };
    uint32_t lastShape { 0 };
    DistancesByComparison comparisons;
    // FlatTable: distance[index], where index = lastShape - shape.
    // HashedTable: distance[shapeWord * hashMultiplier >> hashShift].
    // PagedTable: an entry for each page, then the rows: distance[entry[index >> logShapesPerPage] - index], counted from the start of the table.
    Vector<uint8_t> table;
};

// In place of a HashedTable a PagedTable, where none of the multipliers tried keeps different distances apart.
JS_EXPORT_PRIVATE DistancesByShape encodeDistancesByShape(std::span<const DistanceOfShapes> members, DistanceForm);
// The fewest instructions, then the fewest bytes. A function of the members alone, in any order.
JS_EXPORT_PRIVATE DistancesByShape distancesByShape(std::span<const DistanceOfShapes> members);

struct PlacedShape {
    LayoutID layout;
    OptionSet<ObjectMutation> impossibleMutations;
};

struct ShapeTableParts {
    // By shape, and number 0 is no shape. Milestone one: first one shape for each layout in use that holdsCompiledObjects(): a layout in front of what is written below it, otherwise in the order of the layouts.
    // From firstEnvironmentShape on, one for each layout in use with LayoutStorage::Environment and one for the layout of closures, in the order of the layouts.
    // LayoutKind::Closure is an environment kind: AOTLayoutTables.cpp refuses it among the objects. No flag, no mutation ruled out.
    Vector<PlacedShape> shapes;
    // By layout. Milestone one: the one shape, or none. A layout with isRecordAlone has none: its record is written under shape 0.
    Vector<ShapeRange> shapesOfLayout;
    // Milestone one: behind the last shape of a compiled object. Never 0.
    uint32_t firstEnvironmentShape { 0 };
};

// What ShapeTable says of the sets of the program's table that hold several layouts: each answer is kept where it is first asked for, by whichever thread.
struct ShapeTestsOfSets {
    struct OfSet {
        ShapeTest test;
        // Nothing: not asked for yet.
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

    // Ascending, each once. The layouts come in any order.
    JS_EXPORT_PRIVATE void appendMemberShapes(std::span<const LayoutID>, ShapeList& ascendingShapes) const;
    // It passes these shapes, refuses every other that isAssigned(), and holdsForAnyCell().
    JS_EXPORT_PRIVATE ShapeTest testOfShapes(std::span<const uint32_t> ascendingShapes) const;
    // For a cell that has one of the known shapes. It says nothing of any other cell.
    JS_EXPORT_PRIVATE ShapeTest testOfShapes(std::span<const uint32_t> ascendingTestedShapes, std::span<const uint32_t> ascendingKnownShapes) const;
    JS_EXPORT_PRIVATE ShapeTest testOf(std::span<const LayoutID>) const;
    // Of the members of the set. A set may hold thousands of layouts, and every test for it asks.
    JS_EXPORT_PRIVATE ShapeTest testOf(LayoutSetID, std::span<const LayoutID> members) const;
    // encodeShapeSet() of their shapes.
    JS_EXPORT_PRIVATE EncodedShapeSet encodedShapesOf(LayoutSetID, std::span<const LayoutID> members) const;

private:
    ShapeTestsOfSets::OfSet* testsKeptOf(LayoutSetID, std::span<const LayoutID> members, ShapeTestsOfSets::Part*&) const;

    ShapeTableParts m_parts;
    std::unique_ptr<ShapeTestsOfSets> m_testsOfSets;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
