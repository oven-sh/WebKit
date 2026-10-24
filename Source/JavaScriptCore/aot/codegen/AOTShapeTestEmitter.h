/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
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

    Condition hasShapeIn(CellPointer, LayoutSetID tested, const Type& known);
    void branchOnShapeIn(CellPointer, LayoutSetID tested, const Type& known, BranchTargets);
    void switchOnShape(CellPointer, std::span<const LayoutSetID> cases, std::span<const B3::FrequentedBlock> targets, B3::FrequentedBlock otherwise);
    bool hasOnlyCompiledObjects(const Type& known) const;
    // The constructor of a built-in class makes the objects of a class below it. Their Structures say which class each was made for, and what the run time found when it looked at one.
    bool mayBeMadeByBuiltinBase(const Type&) const;
    Condition hasNoExtraProperties(CellPointer, const Type& known);
    Condition hasLayoutPrototype(CellPointer, const Type& known);

private:
    Condition isBranded(Pointer structure);
    Condition isMadeByBuiltinBaseFor(CellPointer, LayoutSetID tested);
    Condition hasStateBelowBuiltinBase(CellPointer, uint32_t state);
    Condition hasState(CellPointer, const Type& known, uint32_t stateBelowBuiltinBase, NOESCAPE const Invocable<Condition()> auto& ofCompiledObject);
    Condition hasShapeOfCompiledObjectIn(CellPointer, LayoutSetID tested, const Type& known);
    Condition passes(Word32 shapeWord, LayoutSetID tested, const ShapeTest&);
    Condition passesRange(Word32 shapeWord, const ShapeTest&);
    Condition passesBitSet(Word32 shapeWord, LayoutSetID tested, const ShapeTest&);
    std::optional<Condition> passesBitSetThatHoldsEveryKnownShape(Word32 shapeWord, LayoutSetID tested, LayoutSetID known);
    Condition passesBlockByte(CellPointer, const ShapeTest&);
    struct BitSetConstant {
        uint32_t index;
        PagedBitSet::Entries entries;
        uint64_t headOfHashedSet;
    };

    BitSetConstant bitSetConstant(LayoutSetID tested);

    UncheckedKeyHashMap<LayoutSetID, BitSetConstant> m_bitSetConstants;
    UncheckedKeyHashMap<uint64_t, uint32_t> m_bitSetsThatHoldEveryKnownShape;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
