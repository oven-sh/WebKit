/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTShapeTestEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTPlacement.h"
#include "AOTShapeTest.h"
#include "MarkedBlock.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned bitsPerShapeSetWord = 32;
static constexpr unsigned logBitsPerShapeSetWord = 5;
static constexpr unsigned logBytesPerShapeSetWord = 2;

static_assert(offsetOfShapeWord + sizeof(uint32_t) <= MarkedBlock::atomSize);
static_assert(offsetOfOrderWord + sizeof(uint32_t) <= MarkedBlock::atomSize);
static_assert(1u << logBitsPerShapeSetWord == bitsPerShapeSetWord && 8u << logBytesPerShapeSetWord == bitsPerShapeSetWord);
static_assert(immediateBitsWindow <= 64);

ShapeTestEmitter::ShapeTestEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

const B3::AbstractHeap& ShapeTestEmitter::shapeWordHeapOf(LayoutSetID possibleLayouts)
{
    MemoryLocation location = possibleLayouts ? MemoryLocation::shapeWords(possibleLayouts) : MemoryLocation(AbstractHeap::ShapeWords);
    return m_heaps.at(m_program, location);
}

Word32 ShapeTestEmitter::shapeWordOf(CellPointer cell, LayoutSetID possibleLayouts)
{
    return Word32(m_out.load32(m_out.address(shapeWordHeapOf(possibleLayouts), cell.value(), offsetOfShapeWord)));
}

Word32 ShapeTestEmitter::orderWordOf(CellPointer cell)
{
    return loadField<Word32>(cell, m_heaps.AOTObject_orderWord);
}

// The object of a layout without compiled storage, such as a module namespace, has no shape word.
bool ShapeTestEmitter::hasOnlyCompiledObjects(const Type& known) const
{
    const SlotPlan& slots = m_placement.slots();
    return known.hasOnlyLayouts() && std::ranges::all_of(membersOf(m_program, known), [&](LayoutID member) {
        return slots.storageOf(member) == LayoutStorage::CompiledObject || m_program.at(member).flow.allocation == AllocationState::NeverAllocated;
    });
}

Condition ShapeTestEmitter::hasShapeIn(CellPointer cell, LayoutSetID tested, const Type& known)
{
    const ShapeTable& shapes = m_placement.shapes();
    bool isKnownCompiledObject = hasOnlyCompiledObjects(known);
    const ShapeTest& test = isKnownCompiledObject ? shapes.testOf(tested, known.layouts) : shapes.testOf(tested);

    switch (test.form) {
    case ShapeTestForm::NeverPasses:
        return boolean(false);
    case ShapeTestForm::AlwaysPasses:
        if (isKnownCompiledObject)
            return boolean(true);
        break;
    case ShapeTestForm::Block:
    case ShapeTestForm::BlockOrEarlier:
        return passesBlockByte(cell, known.layouts, test);
    case ShapeTestForm::OneShape:
    case ShapeTestForm::UpTo:
    case ShapeTestForm::From:
    case ShapeTestForm::Range:
    case ShapeTestForm::ImmediateBits:
    case ShapeTestForm::BitSet:
        break;
    }

    Word32 shapeWord = shapeWordOf(cell, known.layouts);
    Condition passesTest = passes(shapeWord, tested, test);
    if (isKnownCompiledObject || holdsForAnyCell(test.form))
        return passesTest;
    return logicalAnd(hasBits(shapeWord, compiledObjectShapeBit), passesTest);
}

void ShapeTestEmitter::branchOnShapeIn(CellPointer cell, LayoutSetID tested, const Type& known, BranchTargets targets)
{
    branch(hasShapeIn(cell, tested, known), targets);
}

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
        return logicalAnd(passesRange(shapeWord, test), hasBits(shiftedBits, 1));
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
    return below(add(shapeWord, word32(test.last())), word32(test.count));
}

Pointer ShapeTestEmitter::bitSetWords(LayoutSetID tested, const ShapeTest& test)
{
    uint32_t constant = m_bitSetConstants.ensure(tested, [&] {
        Vector<uint32_t> words(FillWith { }, (test.count + bitsPerShapeSetWord - 1) / bitsPerShapeSetWord, 0);
        for (uint32_t shape : m_placement.shapes().memberShapes(tested)) {
            uint32_t bit = test.last() - shape;
            words[bit / bitsPerShapeSetWord] |= 1u << bit % bitsPerShapeSetWord;
        }
        return m_out.addConstantData(asBytes(words.span()));
    }).iterator->value;
    return Pointer(m_out.relocatable(RelocationSpace::Constants, constant));
}

Condition ShapeTestEmitter::passesBitSet(Word32 shapeWord, LayoutSetID tested, const ShapeTest& test)
{
    Condition isWithinReach = passesRange(shapeWord, test);
    Word32 index = select(isWithinReach, add(shapeWord, word32(test.last())), word32(0));
    Pointer words = bitSetWords(tested, test);
    Integer wordIndex = zeroExtended(shiftRightUnsigned(index, word32(logBitsPerShapeSetWord)));
    Word32 word(m_out.load32(m_out.address(m_heaps.AOTImage_tables, offsetBy(words, wordIndex, logBytesPerShapeSetWord).value())));
    return logicalAnd(isWithinReach, hasBits(shiftRightUnsigned(word, index), 1));
}

Condition ShapeTestEmitter::passesBlockByte(CellPointer cell, LayoutSetID possibleLayouts, const ShapeTest& test)
{
    TypedPointer pointer = m_out.address(shapeWordHeapOf(possibleLayouts), cell.value(), offsetOfShapeWord + 1);
    uint16_t expectedHalfword = blockTestHalfword(test.blockByte);
    Word32 halfword(isX86_64() ? m_out.load16ZeroExt32(pointer) : m_out.load16SignExt32(pointer));
    Word32 expected = word32(isX86_64() ? expectedHalfword : static_cast<int16_t>(expectedHalfword));

    switch (test.form) {
    case ShapeTestForm::Block:
        return equal(halfword, expected);
    case ShapeTestForm::BlockOrEarlier:
        return aboveOrEqual(halfword, expected);
    case ShapeTestForm::NeverPasses:
    case ShapeTestForm::AlwaysPasses:
    case ShapeTestForm::OneShape:
    case ShapeTestForm::UpTo:
    case ShapeTestForm::From:
    case ShapeTestForm::Range:
    case ShapeTestForm::ImmediateBits:
    case ShapeTestForm::BitSet:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ShapeTestEmitter::switchOnShape(CellPointer cell, std::span<const LayoutSetID> cases, std::span<const B3::FrequentedBlock> targets, B3::FrequentedBlock otherwise)
{
    RELEASE_ASSERT(cases.size() == targets.size());
    const ShapeTable& shapes = m_placement.shapes();
    bool hasAtMostOneShapePerCase = std::ranges::all_of(cases, [&](LayoutSetID layouts) {
        ShapeTestForm form = shapes.testOf(layouts).form;
        return form == ShapeTestForm::OneShape || form == ShapeTestForm::NeverPasses;
    });

    if (hasAtMostOneShapePerCase) {
        Vector<SwitchTarget, 8> switchTargets;
        for (size_t index = 0; index < cases.size(); ++index) {
            const ShapeTest& test = shapes.testOf(cases[index]);
            if (test.form == ShapeTestForm::OneShape)
                switchTargets.append(SwitchTarget { static_cast<int32_t>(AOT::shapeWordOf(test.first)), targets[index] });
        }
        if (switchTargets.isEmpty()) {
            m_out.jump(otherwise.block());
            return;
        }
        m_out.switchOn(shapeWordOf(cell, LayoutSetID()).value(), switchTargets.span(), otherwise);
        return;
    }

    const Type anyObject = Type::object();
    for (size_t index = 0; index < cases.size(); ++index) {
        bool isLast = index + 1 == cases.size();
        B3::FrequentedBlock next = isLast ? otherwise : normalEdge(newBlock());
        branchOnShapeIn(cell, cases[index], anyObject, { targets[index], next });
        if (!isLast)
            m_out.appendTo(next.block());
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
