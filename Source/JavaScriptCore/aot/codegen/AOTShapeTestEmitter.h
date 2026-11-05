/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTPagedBitSet.h"
#include "AOTShapeTest.h"
#include <optional>
#include <span>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

struct ShapeTest;

class ShapeTestEmitter final : public OperationEmitter {
public:
    explicit ShapeTestEmitter(CodeGenerator&);

    Word32 shapeWordOf(CellPointer);
    // hasShapeIn() of a value that nobody has asked whether it is a cell. What is none has no shape: readsOffNoCellFault. For whoever expects a cell there: each value that is none costs a fault.
    // Nothing: the test would be one within known shapes, or it does not read the whole word. The cell test is then the caller's.
    std::optional<Condition> hasShapeInThoughNoCell(Boxed, LayoutSetID tested, const Type& known);
    Word32 orderWordOf(CellPointer);
    // Other cells have other things there. The caller masks the word, or compares it with one that only a compiled object has.
    Word32 orderWordOfAnyCell(CellPointer);

    // Passes for compiled objects alone: what stands behind it may read and write the words and the slots of one.
    // `known` is proven of the cell. Where it lists the shapes of all its cells, the test tells the tested shapes from those alone: ShapeTable::testOfShapes().
    Condition hasShapeIn(CellPointer, LayoutSetID tested, const Type& known);
    // Of the shape word of a compiled object.
    Condition isShapeOf(Word32 shapeWord, LayoutID);
    void branchOnShapeIn(CellPointer, LayoutSetID tested, const Type& known, BranchTargets);
    // Of an operation on a slot, whose object has one of the receivers that it lists: FieldAccess::membersWithField among them.
    void branchOnShapeAmongReceivers(const ValueContext& access, CellPointer, LayoutSetID tested, BranchTargets);
    // A shape that several cases hold goes to the first of them.
    void switchOnShape(CellPointer, std::span<const LayoutSetID> cases, std::span<const BlockEdge> targets, BlockEdge otherwise);
    bool hasOnlyCompiledObjects(const Type& known) const;
    bool mayBeOtherThanCompiledObject(const Type&) const;
    Condition isCompiledObject(CellPointer);

private:
    // Also reads the sets that the walk made for this function.
    TypeReader typeReader() const;
    ShapeTest testOf(LayoutSetID tested) const;
    Condition passes(CellPointer, LayoutSetID tested, const ShapeTest&);
    // Of every form but Block and BlockOrEarlier.
    Condition passes(Word32 shapeWord, LayoutSetID tested, const ShapeTest&);
    Condition passesRange(Word32 shapeWord, const ShapeTest&);
    Condition passesBlockByte(CellPointer, const ShapeTest&);
    Condition passesBitSet(Word32 shapeWord, LayoutSetID tested, const ShapeTest&);
    // Nothing: the known shapes lie too far apart for a set that reaches from the first to the last.
    std::optional<Condition> passesBitSetThatHoldsEveryKnownShape(Word32 shapeWord, LayoutSetID tested, LayoutSetID known, std::span<const uint32_t> testedShapes, std::span<const uint32_t> knownShapes);
    struct BitSetConstant {
        // SymbolSpace::Constant.
        uint32_t index;
        PagedBitSet::Entries entries;
        uint64_t headOfHashedSet;
    };
    BitSetConstant bitSetConstant(LayoutSetID tested);

    // The shapes of the set that was last known of a cell: each case of a switch over a union tests one layout against the same thousand.
    LayoutSetID m_setWhoseShapesAreKept;
    bool m_keptSetHoldsCompiledObjectsAlone { true };
    ShapeTable::ShapeList m_shapesKept;
    UncheckedKeyHashMap<LayoutSetID, BitSetConstant> m_bitSetConstants;
    // By LayoutSetID::bits() of the tested set and of the known one.
    UncheckedKeyHashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_bitSetsThatHoldEveryKnownShape;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
