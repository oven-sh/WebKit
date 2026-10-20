/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include <span>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

struct ShapeTest;

class ShapeTestEmitter final : public OperationEmitter {
public:
    explicit ShapeTestEmitter(CodeGenerator&);

    Word32 shapeWordOf(CellPointer, LayoutSetID possibleLayouts);
    Word32 orderWordOf(CellPointer);

    Condition hasShapeIn(CellPointer, LayoutSetID tested, const Type& known);
    void branchOnShapeIn(CellPointer, LayoutSetID tested, const Type& known, BranchTargets);
    void switchOnShape(CellPointer, std::span<const LayoutSetID> cases, std::span<const B3::FrequentedBlock> targets, B3::FrequentedBlock otherwise);

private:
    bool hasOnlyCompiledObjects(const Type& known) const;
    const B3::AbstractHeap& shapeWordHeapOf(LayoutSetID possibleLayouts);
    Condition passes(Word32 shapeWord, LayoutSetID tested, const ShapeTest&);
    Condition passesRange(Word32 shapeWord, const ShapeTest&);
    Condition passesBitSet(Word32 shapeWord, LayoutSetID tested, const ShapeTest&);
    Condition passesBlockByte(CellPointer, LayoutSetID possibleLayouts, const ShapeTest&);
    Pointer bitSetWords(LayoutSetID tested, const ShapeTest&);

    UncheckedKeyHashMap<LayoutSetID, uint32_t> m_bitSetConstants;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
