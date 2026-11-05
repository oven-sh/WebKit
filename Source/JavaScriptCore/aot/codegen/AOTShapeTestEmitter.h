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
    Word32 orderWordOf(CellPointer);
    Word32 orderWordOfAnyCell(CellPointer);

    Condition hasShapeIn(CellPointer, LayoutSetID tested, const Type& known);
    Condition isShapeOf(Word32 shapeWord, LayoutID);
    void branchOnShapeIn(CellPointer, LayoutSetID tested, const Type& known, BranchTargets);
    void branchOnShapeAmongReceivers(const ValueContext& access, CellPointer, LayoutSetID tested, BranchTargets);
    void switchOnShape(CellPointer, std::span<const LayoutSetID> cases, std::span<const BlockEdge> targets, BlockEdge otherwise);
    bool hasOnlyCompiledObjects(const Type& known) const;
    bool mayBeOtherThanCompiledObject(const Type&) const;
    Condition isCompiledObject(CellPointer);

private:
    TypeReader typeReader() const;
    ShapeTest testOf(LayoutSetID tested) const;
    Condition passes(CellPointer, LayoutSetID tested, const ShapeTest&);
    Condition passes(Word32 shapeWord, LayoutSetID tested, const ShapeTest&);
    Condition passesRange(Word32 shapeWord, const ShapeTest&);
    Condition passesBlockByte(CellPointer, const ShapeTest&);
    Condition passesBitSet(Word32 shapeWord, LayoutSetID tested, const ShapeTest&);
    std::optional<Condition> passesBitSetThatHoldsEveryKnownShape(Word32 shapeWord, LayoutSetID tested, LayoutSetID known, std::span<const uint32_t> testedShapes, std::span<const uint32_t> knownShapes);
    struct BitSetConstant {
        uint32_t index;
        PagedBitSet::Entries entries;
        uint64_t headOfHashedSet;
    };
    BitSetConstant bitSetConstant(LayoutSetID tested);

    LayoutSetID m_setWhoseShapesAreKept;
    bool m_keptSetHoldsCompiledObjectsAlone { true };
    ShapeTable::ShapeList m_shapesKept;
    UncheckedKeyHashMap<LayoutSetID, BitSetConstant> m_bitSetConstants;
    UncheckedKeyHashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_bitSetsThatHoldEveryKnownShape;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
