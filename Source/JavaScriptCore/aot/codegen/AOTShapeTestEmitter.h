/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTPagedBitSet.h"
#include "AOTShapeTest.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

struct ShapeTest;

enum class MarkedObjects : uint8_t {
    Pass,
    MayBeRefused,
    AreRefused,
    AreAskedAgain,
    PassAlone,
};

struct ShapeTestsKeptAcrossUnits {
    static constexpr unsigned numberOfBits = 10;
    struct Entry {
        uint64_t testedAndKnown { UINT64_MAX };
        ShapeTest test;
    };

    const ShapeTable* table { nullptr };
    std::array<Entry, 1 << numberOfBits> entries;
    UncheckedKeyHashMap<LayoutSetID, bool> setsThatMayBearMark;
    LayoutSetID setListedLast;
    ShapeTable::ShapeList shapesOfSetListedLast;
};

class ShapeTestEmitter final : public OperationEmitter {
public:
    explicit ShapeTestEmitter(CodeGenerator&);

    Word32 shapeWordOf(CellPointer);
    Word32 shapeWordWithMarkOf(CellPointer);
    Word32 shapeWordOf(CellPointer, LayoutSetID possible);
    bool mayBearMark(LayoutSetID);
    struct IndexByShape {
        Integer index;
        int32_t offset { 0 };
    };
    IndexByShape indexByShapeOf(CellPointer);
    bool paysForMark(LayoutSetID tested, const Type& known);
    Condition bearsMark(CellPointer);
    Condition bearsNoMark(CellPointer);
    void setMark(CellPointer);
    void clearMark(CellPointer);
    Word32 orderWordOf(CellPointer);
    Word32 orderWordOfAnyCell(CellPointer);

    Condition hasShapeIn(CellPointer, LayoutSetID tested, const Type& known, MarkedObjects = MarkedObjects::Pass);
    bool lastTestRejectsMarks() const { return m_lastTestRejectsMarks; }
    bool lastTestIsHeldAgainstEveryShape() const { return m_lastTestIsHeldAgainstEveryShape; }
    Condition isShapeOf(Word32 shapeWord, LayoutID);
    void branchOnShapeIn(CellPointer, LayoutSetID tested, const Type& known, BranchTargets, MarkedObjects = MarkedObjects::Pass);
    struct ShapesOfMember {
        LayoutID member;
        ShapeTable::ShapeList shapes;
    };
    Vector<ShapesOfMember, 8> shapesOfMembersOf(LayoutSetID);
    Word32 distanceOfUnmarkedObjectFromShape(CellPointer, LayoutSetID tested, uint32_t shape);
    void branchOnShapeAmongReceivers(const ValueContext& access, CellPointer, LayoutSetID tested, BranchTargets);
    void switchOnShape(CellPointer, std::span<const LayoutSetID> cases, std::span<const BlockEdge> targets, BlockEdge otherwise);
    bool hasOnlyCompiledObjects(const Type& known) const;
    bool mayBeOtherThanCompiledObject(const Type&) const;
    Condition isCompiledObject(CellPointer);

private:
    TypeReader typeReader() const;
    ShapeTest testOf(LayoutSetID tested) const;
    enum class WordOfTest : uint8_t { WithoutMark, AsItLies, WithMark };
    Condition passes(CellPointer, LayoutSetID tested, const ShapeTest&, WordOfTest = WordOfTest::WithoutMark);
    Condition passes(Word32 shapeWord, LayoutSetID tested, const ShapeTest&);
    Condition passesRange(Word32 shapeWord, const ShapeTest&);
    Condition passesBlockByte(CellPointer, const ShapeTest&, WordOfTest);
    Condition passesBitSet(Word32 shapeWord, LayoutSetID tested, const ShapeTest&);
    std::optional<Condition> passesBitSetThatHoldsEveryKnownShape(Word32 shapeWord, LayoutSetID tested, LayoutSetID known, std::span<const uint32_t> testedShapes, std::span<const uint32_t> knownShapes);
    struct BitSetConstant {
        uint32_t index;
        PagedBitSet::Entries entries;
        uint64_t headOfHashedSet;
    };
    BitSetConstant bitSetConstant(LayoutSetID tested);
    ShapeTestsKeptAcrossUnits& testsKeptAcrossUnits();
    ShapeTestsKeptAcrossUnits::Entry& entryKeptAcrossUnits(uint64_t testedAndKnown);
    std::span<const uint32_t> shapesOf(LayoutSetID);

    LayoutSetID m_setKnownLast;
    bool m_setKnownLastHoldsCompiledObjectsAlone { true };
    bool m_lastTestRejectsMarks { false };
    bool m_lastTestIsHeldAgainstEveryShape { false };
    bool m_wordIsAskedForMark { true };
    UncheckedKeyHashMap<LayoutSetID, bool> m_setsThatMayBearMark;
    UncheckedKeyHashMap<LayoutSetID, BitSetConstant> m_bitSetConstants;
    UncheckedKeyHashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_bitSetsThatHoldEveryKnownShape;
    UncheckedKeyHashMap<uint64_t, ShapeTest, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_testsAmongKnownShapes;
    ShapeTestsKeptAcrossUnits* m_testsKeptAcrossUnits { nullptr };
    mutable LayoutSetID m_setTestedLast;
    mutable ShapeTest m_testOfSetTestedLast;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
