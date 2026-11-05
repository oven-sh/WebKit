/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTShapeTestEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCodeSites.h"
#include "AOTShapeTest.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestPlan.h"
#include "MarkedBlock.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(offsetOfShapeWord + sizeof(uint32_t) <= MarkedBlock::atomSize);
static_assert(offsetOfOrderWord + sizeof(uint32_t) <= MarkedBlock::atomSize);
static_assert(immediateBitsWindow <= 64);

ShapeTestEmitter::ShapeTestEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

TypeReader ShapeTestEmitter::typeReader() const
{
    return m_generator.inputs().types;
}

Word32 ShapeTestEmitter::shapeWordOf(CellPointer cell)
{
    return loadField<Word32>(cell, m_heaps.AOTObject_shapeWord);
}

Word32 ShapeTestEmitter::orderWordOf(CellPointer cell)
{
    return orderWordOfAnyCell(cell);
}

Word32 ShapeTestEmitter::orderWordOfAnyCell(CellPointer cell)
{
    return loadField<Word32>(cell, m_heaps.AOTObject_orderWord);
}

bool ShapeTestEmitter::hasOnlyCompiledObjects(const Type& known) const
{
    return holdsOnlyCompiledObjects(typeReader(), m_plan, known);
}

bool ShapeTestEmitter::mayBeOtherThanCompiledObject(const Type& type) const
{
    return !hasOnlyCompiledObjects(type);
}

Condition ShapeTestEmitter::isCompiledObject(CellPointer cell)
{
    return hasBits(shapeWordOf(cell), compiledObjectShapeBit);
}

ShapeTest ShapeTestEmitter::testOf(LayoutSetID tested) const
{
    ShapeTest test = m_plan.shapes().testOf(tested, membersOf(typeReader(), tested));
    RELEASE_ASSERT(holdsForAnyCell(test.form));
    return test;
}

Condition ShapeTestEmitter::hasShapeIn(CellPointer cell, LayoutSetID tested, const Type& known)
{
    // `new.target` may give an object a layout that the type does not list.
    if (!known.hasOnlyLayouts() || known.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return passes(cell, tested, testOf(tested));
    // Of a type that has layouts alone both go by the set.
    const ShapeTable& shapes = m_plan.shapes();
    if (m_setWhoseShapesAreKept != known.layouts) {
        m_setWhoseShapesAreKept = known.layouts;
        m_keptSetHoldsCompiledObjectsAlone = hasOnlyCompiledObjects(known);
        m_shapesKept.shrink(0);
        shapes.appendMemberShapes(membersOf(typeReader(), known), m_shapesKept);
    }
    if (!m_keptSetHoldsCompiledObjectsAlone)
        return passes(cell, tested, testOf(tested));

    // The test tells the tested shapes from the known ones alone. A known layout that is never allocated has no shape, so it counts for nothing.
    ShapeTable::ShapeList testedShapes;
    shapes.appendMemberShapes(membersOf(typeReader(), tested), testedShapes);
    const ShapeTable::ShapeList& knownShapes = m_shapesKept;
    ShapeTest test = shapes.testOfShapes(testedShapes.span(), knownShapes.span());

    if (test.form == ShapeTestForm::BitSet) {
        if (std::optional<Condition> passesTest = passesBitSetThatHoldsEveryKnownShape(shapeWordOf(cell), tested, known.layouts, testedShapes.span(), knownShapes.span()))
            return *passesTest;
    }
    return passes(cell, tested, test);
}

std::optional<Condition> ShapeTestEmitter::hasShapeInThoughNoCell(Boxed value, LayoutSetID tested, const Type& known)
{
    if (!readsOffNoCellFault)
        return std::nullopt;
    // As hasShapeIn().
    if (known.hasOnlyLayouts() && !known.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return std::nullopt;
    ShapeTest test = testOf(tested);
    switch (test.form) {
    case ShapeTestForm::OneShape:
    case ShapeTestForm::UpTo:
    case ShapeTestForm::Range:
    case ShapeTestForm::ImmediateBits:
    case ShapeTestForm::BitSet:
        break;
    case ShapeTestForm::NeverPasses:
    case ShapeTestForm::AlwaysPasses:
    case ShapeTestForm::From:
    case ShapeTestForm::Block:
    case ShapeTestForm::BlockOrEarlier:
        return std::nullopt;
    }
    return passes(Word32(m_out.load32OffWhatMayBeNoCell(value.value(), m_heaps.AOTObject_shapeWord)), tested, test);
}

Condition ShapeTestEmitter::isShapeOf(Word32 shapeWord, LayoutID layout)
{
    ShapeRange shapes = m_plan.shapes().shapesOf(layout);
    if (shapes.isEmpty())
        return boolean(false);
    return passesRange(shapeWord, ShapeTest { .form = ShapeTestForm::Range, .first = shapes.first, .count = shapes.count });
}

void ShapeTestEmitter::branchOnShapeIn(CellPointer cell, LayoutSetID tested, const Type& known, BranchTargets targets)
{
    branch(hasShapeIn(cell, tested, known), targets);
}

// The list alone does not say what hasShapeIn() asks of the objects that the type of the operand names.
void ShapeTestEmitter::branchOnShapeAmongReceivers(const ValueContext& access, CellPointer cell, LayoutSetID tested, BranchTargets targets)
{
    TypeID known = types().intersectionOf(typeIDOf(access->objectOperand()), types().forLayoutSet(access->receivers()));
    branchOnShapeIn(cell, tested, types().at(known), targets);
}

// A test of a block reads two bytes of the shape word, and a test that is decided reads none.
Condition ShapeTestEmitter::passes(CellPointer cell, LayoutSetID tested, const ShapeTest& test)
{
    switch (test.form) {
    case ShapeTestForm::NeverPasses:
        return boolean(false);
    case ShapeTestForm::AlwaysPasses:
        return boolean(true);
    case ShapeTestForm::Block:
    case ShapeTestForm::BlockOrEarlier:
        return passesBlockByte(cell, test);
    case ShapeTestForm::OneShape:
    case ShapeTestForm::UpTo:
    case ShapeTestForm::From:
    case ShapeTestForm::Range:
    case ShapeTestForm::ImmediateBits:
    case ShapeTestForm::BitSet:
        break;
    }
    return passes(shapeWordOf(cell), tested, test);
}

// The word of a cell that is no compiled object lacks compiledObjectShapeBit, which the word of every shape has. It is below each of those words, and the sum of it and a shape does not wrap: every form that holdsForAnyCell() refuses it.
Condition ShapeTestEmitter::passes(Word32 shapeWord, LayoutSetID tested, const ShapeTest& test)
{
    switch (test.form) {
    case ShapeTestForm::NeverPasses:
        return boolean(false);
    case ShapeTestForm::AlwaysPasses:
        return boolean(true);
    case ShapeTestForm::OneShape:
        return equal(shapeWord, word32(AOT::shapeWordOf(test.first)));
    case ShapeTestForm::UpTo:
        return aboveOrEqual(shapeWord, word32(AOT::shapeWordOf(test.last())));
    case ShapeTestForm::From:
        return belowOrEqual(shapeWord, word32(AOT::shapeWordOf(test.first)));
    case ShapeTestForm::Range:
        return passesRange(shapeWord, test);
    case ShapeTestForm::ImmediateBits: {
        Word32 distanceFromLast = add(shapeWord, word32(test.last()));
        Word32 shiftedBits = test.count <= 32
            ? shiftRightUnsigned(word32(static_cast<uint32_t>(test.bits)), distanceFromLast)
            : low32(Integer(m_out.lShr(m_out.constInt64(static_cast<int64_t>(test.bits)), distanceFromLast.value())));
        Condition isInRange = passesRange(shapeWord, test);
        return logicalAnd(isInRange, hasBits(shiftedBits, 1));
    }
    case ShapeTestForm::BitSet:
        return passesBitSet(shapeWord, tested, test);
    case ShapeTestForm::Block:
    case ShapeTestForm::BlockOrEarlier:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Condition ShapeTestEmitter::passesRange(Word32 shapeWord, const ShapeTest& test)
{
    return below(add(shapeWord, test.last()), test.count);
}

// The halfword of a cell that is no compiled object lacks shapeBlockMarker, so it is below that of every block.
Condition ShapeTestEmitter::passesBlockByte(CellPointer cell, const ShapeTest& test)
{
    TypedPointer pointer = m_out.address(m_heaps.AOTObject_shapeWord, cell.value(), offsetOfShapeWord + 1);
    uint16_t expectedHalfword = blockTestHalfword(test.blockByte);
    // Extended by its sign, the halfword of a block that can be tested fits the immediate of a comparison on arm64.
    Word32 halfword(isX86_64() ? m_out.load16ZeroExt32(pointer) : m_out.load16SignExt32(pointer));
    Word32 expected = word32(isX86_64() ? expectedHalfword : static_cast<int16_t>(expectedHalfword));
    if (test.form == ShapeTestForm::Block)
        return equal(halfword, expected);
    RELEASE_ASSERT(test.form == ShapeTestForm::BlockOrEarlier);
    return aboveOrEqual(halfword, expected);
}

ShapeTestEmitter::BitSetConstant ShapeTestEmitter::bitSetConstant(LayoutSetID tested)
{
    return m_bitSetConstants.ensure(tested, [&] {
        EncodedShapeSet encoded = m_plan.shapes().encodedShapesOf(tested, membersOf(typeReader(), tested));
        return BitSetConstant { m_out.addConstantData(asBytes(encoded.words.span()), ConstantKind::ShapeSet), encoded.entries, encoded.words[0] };
    }).iterator->value;
}

// A shape out of reach takes the bit behind the last one, which is clear. So the test is one bit test: no second exit, and no flag that is made into a value.
Condition ShapeTestEmitter::passesBitSet(Word32 shapeWord, LayoutSetID tested, const ShapeTest& test)
{
    Word32 distanceFromLast = add(shapeWord, word32(test.last()));
    BitSetConstant constant = bitSetConstant(tested);
    Pointer words(m_out.relocatable(SymbolSpace::Constant, constant.index));
    auto addressOf = [&](Word32 element, unsigned logSize, ptrdiff_t offset = 0) {
        return m_out.address(m_heaps.AOTImage_tables, offsetBy(words, zeroExtended(element), logSize).value(), offset);
    };
    if (constant.entries == PagedBitSet::Entries::Hashed) {
        // A shape out of reach finds another number in its slot, like any shape that is no member.
        static_assert(sizeof(PagedBitSet::Slot) == sizeof(uint16_t));
        Word32 slot = shiftRightUnsigned(mul(distanceFromLast, word32(PagedBitSet::multiplierIn(constant.headOfHashedSet))), static_cast<int32_t>(PagedBitSet::shiftIn(constant.headOfHashedSet)));
        Word32 held(m_out.load16ZeroExt32(addressOf(slot, PagedBitSet::logBytesPerSlot, PagedBitSet::offsetOfSlots)));
        return equal(held, distanceFromLast);
    }
    Word32 index = select(below(distanceFromLast, test.count), distanceFromLast, test.count);
    if (constant.entries == PagedBitSet::Entries::None) {
        // Both targets are little-endian, so the pages are read by halves: the instructions are shorter.
        constexpr unsigned logBitsPerHalf = PagedBitSet::logBitsPerPage - 1;
        constexpr unsigned logBytesPerHalf = PagedBitSet::logBytesPerWord - 1;
        Word32 half(m_out.load32(addressOf(shiftRightUnsigned(index, word32(logBitsPerHalf)), logBytesPerHalf)));
        return hasBits(shiftRightUnsigned(half, index), 1);
    }
    Word32 page = shiftRightUnsigned(index, word32(PagedBitSet::logBitsPerPage));
    Word32 word(constant.entries == PagedBitSet::Entries::Wide ? m_out.load16ZeroExt32(addressOf(page, PagedBitSet::logBytesPerWideEntry)) : m_out.load8ZeroExt32(addressOf(page, PagedBitSet::logBytesPerNarrowEntry)));
    Integer bits(m_out.load64(addressOf(word, PagedBitSet::logBytesPerWord)));
    return hasBits(Integer(m_out.lShr(bits.value(), index.value())), 1);
}

// No shape that reaches the test is out of reach of a set that has a bit for each known shape, so the distance is the index as it is.
std::optional<Condition> ShapeTestEmitter::passesBitSetThatHoldsEveryKnownShape(Word32 shapeWord, LayoutSetID tested, LayoutSetID known, std::span<const uint32_t> testedShapes, std::span<const uint32_t> knownShapes)
{
    constexpr uint32_t maxNumberOfBits = maxPagesInShortShapeSet * PagedBitSet::bitsPerPage;
    constexpr unsigned logBitsPerHalf = PagedBitSet::logBitsPerPage - 1;
    constexpr unsigned logBytesPerHalf = PagedBitSet::logBytesPerWord - 1;

    uint32_t first = knownShapes.front();
    uint32_t last = knownShapes.back();
    if (last - first >= maxNumberOfBits)
        return std::nullopt;

    uint64_t key = static_cast<uint64_t>(tested.bits()) << 32 | known.bits();
    uint32_t constant = m_bitSetsThatHoldEveryKnownShape.ensure(key, [&] {
        Vector<uint32_t, 16> halves(FillWith { }, ((last - first) >> logBitsPerHalf) + 1, 0u);
        for (uint32_t shape : testedShapes) {
            if (shape < first || shape > last)
                continue;
            uint32_t distanceFromLast = last - shape;
            halves[distanceFromLast >> logBitsPerHalf] |= 1u << (distanceFromLast & ((1u << logBitsPerHalf) - 1));
        }
        return m_out.addConstantData(asBytes(halves.span()), ConstantKind::BitSetOverKnownShapes);
    }).iterator->value;

    Word32 distanceFromLast = add(shapeWord, word32(last));
    Pointer halves(m_out.relocatable(SymbolSpace::Constant, constant));
    Pointer half = offsetBy(halves, zeroExtended(shiftRightUnsigned(distanceFromLast, word32(logBitsPerHalf))), logBytesPerHalf);
    Word32 bits(m_out.load32(m_out.address(m_heaps.AOTImage_tables, half.value())));
    return hasBits(shiftRightUnsigned(bits, distanceFromLast), 1);
}

void ShapeTestEmitter::switchOnShape(CellPointer cell, std::span<const LayoutSetID> cases, std::span<const BlockEdge> targets, BlockEdge otherwise)
{
    RELEASE_ASSERT(cases.size() == targets.size());
    Vector<ShapeTest, 8> tests;
    tests.reserveInitialCapacity(cases.size());
    for (LayoutSetID layouts : cases)
        tests.append(testOf(layouts));
    bool hasAtMostOneShapePerCase = std::ranges::all_of(tests, [](const ShapeTest& test) {
        return test.form == ShapeTestForm::OneShape || test.form == ShapeTestForm::NeverPasses;
    });

    if (hasAtMostOneShapePerCase) {
        Vector<SwitchTarget, 8> switchTargets;
        switchTargets.reserveInitialCapacity(cases.size());
        for (size_t index = 0; index < cases.size(); ++index) {
            if (tests[index].form == ShapeTestForm::OneShape)
                switchTargets.append(SwitchTarget { static_cast<int32_t>(AOT::shapeWordOf(tests[index].first)), targets[index] });
        }
        // switchOn() takes each value once. Equal values stay in the order of the cases, so the first case keeps its shape.
        AOT::stableSort(switchTargets, { }, &SwitchTarget::caseValue);
        switchTargets.shrink(std::ranges::unique(switchTargets, { }, &SwitchTarget::caseValue).begin() - switchTargets.begin());
        if (switchTargets.isEmpty()) {
            m_out.jump(otherwise.block());
            return;
        }
        m_out.switchOn(shapeWordOf(cell).value(), switchTargets.span(), otherwise);
        return;
    }

    for (size_t index = 0; index < cases.size(); ++index) {
        bool isLast = index + 1 == cases.size();
        BlockEdge next = isLast ? otherwise : normalEdge(newBlock());
        branch(passes(cell, cases[index], tests[index]), { targets[index], next });
        if (!isLast)
            m_out.appendToContinuation(next.block());
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
