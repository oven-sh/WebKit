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

static_assert(offsetOfShapeWord + sizeof(uint32_t) <= MarkedBlock::atomSize);
static_assert(offsetOfOrderWord + sizeof(uint32_t) <= MarkedBlock::atomSize);
static_assert(immediateBitsWindow <= 64);

ShapeTestEmitter::ShapeTestEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Word32 ShapeTestEmitter::shapeWordOf(CellPointer cell)
{
    return loadField<Word32>(cell, m_heaps.AOTObject_shapeWord);
}

Word32 ShapeTestEmitter::orderWordOf(CellPointer cell)
{
    return loadField<Word32>(cell, m_heaps.AOTObject_orderWord);
}

// The object of a layout without compiled storage, such as a module namespace, has no shape word.
bool ShapeTestEmitter::hasOnlyCompiledObjects(const Type& known) const
{
    const SlotPlan& slots = m_placement.slots();
    return known.hasOnlyLayouts(m_program) && std::ranges::all_of(membersOf(m_program, known), [&](LayoutID member) {
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
        return passesBlockByte(cell, test);
    case ShapeTestForm::OneShape:
    case ShapeTestForm::UpTo:
    case ShapeTestForm::From:
    case ShapeTestForm::Range:
    case ShapeTestForm::ImmediateBits:
    case ShapeTestForm::BitSet:
        break;
    }

    Word32 shapeWord = shapeWordOf(cell);
    if (isKnownCompiledObject && test.form == ShapeTestForm::BitSet) {
        if (std::optional<Condition> passesTest = passesBitSetThatHoldsEveryKnownShape(shapeWord, tested, known.layouts))
            return *passesTest;
    }
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

ShapeTestEmitter::BitSetConstant ShapeTestEmitter::bitSetConstant(LayoutSetID tested)
{
    return m_bitSetConstants.ensure(tested, [&] {
        EncodedShapeSet encoded = encodeShapeSet(m_placement.shapes().memberShapes(tested));
        return BitSetConstant { m_out.addConstantData(asBytes(encoded.words.span())), encoded.entries, encoded.words[0] };
    }).iterator->value;
}

// A shape out of reach takes the bit behind the last one, which is clear. So the test is one bit test: no second exit, and no flag that is made into a value.
Condition ShapeTestEmitter::passesBitSet(Word32 shapeWord, LayoutSetID tested, const ShapeTest& test)
{
    Word32 distanceFromLast = add(shapeWord, word32(test.last()));
    BitSetConstant constant = bitSetConstant(tested);
    Pointer words(m_out.relocatable(RelocationSpace::Constants, constant.index));
    if (constant.entries == PagedBitSet::Entries::Hashed) {
        // A shape out of reach finds another number in its slot, like any shape that is no member.
        Word32 slot = shiftRightUnsigned(mul(distanceFromLast, word32(PagedBitSet::multiplierIn(constant.headOfHashedSet))), word32(PagedBitSet::shiftIn(constant.headOfHashedSet)));
        static_assert(sizeof(PagedBitSet::Slot) == sizeof(uint16_t));
        Word32 held(m_out.load16ZeroExt32(m_out.address(m_heaps.AOTImage_tables, offsetBy(words, zeroExtended(slot), PagedBitSet::logBytesPerSlot).value(), PagedBitSet::offsetOfSlots)));
        return equal(held, distanceFromLast);
    }
    Word32 index = select(below(distanceFromLast, word32(test.count)), distanceFromLast, word32(test.count));
    auto addressOf = [&](Word32 element, unsigned logSize) {
        return m_out.address(m_heaps.AOTImage_tables, offsetBy(words, zeroExtended(element), logSize).value());
    };
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

// No shape that reaches the test is out of reach of a set that has a bit for each of them, so the distance is the index as it is.
std::optional<Condition> ShapeTestEmitter::passesBitSetThatHoldsEveryKnownShape(Word32 shapeWord, LayoutSetID tested, LayoutSetID known)
{
    constexpr uint32_t maxNumberOfShapes = maxPagesInShortShapeSet * PagedBitSet::bitsPerPage;
    constexpr unsigned bitsPerShapeInKey = 20;
    constexpr unsigned logBitsPerHalf = PagedBitSet::logBitsPerPage - 1;
    constexpr unsigned logBytesPerHalf = PagedBitSet::logBytesPerWord - 1;

    const ShapeTable& shapes = m_placement.shapes();
    std::span<const uint32_t> knownShapes = shapes.memberShapes(known);
    if (knownShapes.empty())
        return std::nullopt;
    uint32_t first = knownShapes.front();
    uint32_t last = knownShapes.back();
    if (shapes.numberOfShapes() <= maxNumberOfShapes) {
        first = 1;
        last = shapes.numberOfShapes();
    } else if (last - first >= maxNumberOfShapes || last >> bitsPerShapeInKey)
        return std::nullopt;

    uint64_t key = (static_cast<uint64_t>(tested.index()) + 1) << (2 * bitsPerShapeInKey) | static_cast<uint64_t>(first) << bitsPerShapeInKey | last;
    uint32_t constant = m_bitSetsThatHoldEveryKnownShape.ensure(key, [&] {
        Vector<uint32_t, 16> halves(FillWith { }, ((last - first) >> logBitsPerHalf) + 1, 0);
        for (uint32_t shape : shapes.memberShapes(tested)) {
            if (shape < first || shape > last)
                continue;
            uint32_t distanceFromLast = last - shape;
            halves[distanceFromLast >> logBitsPerHalf] |= 1u << (distanceFromLast & ((1u << logBitsPerHalf) - 1));
        }
        return m_out.addConstantData(asBytes(halves.span()));
    }).iterator->value;

    Word32 distanceFromLast = add(shapeWord, word32(last));
    Pointer halves(m_out.relocatable(RelocationSpace::Constants, constant));
    Word32 half(m_out.load32(m_out.address(m_heaps.AOTImage_tables, offsetBy(halves, zeroExtended(shiftRightUnsigned(distanceFromLast, word32(logBitsPerHalf))), logBytesPerHalf).value())));
    return hasBits(shiftRightUnsigned(half, distanceFromLast), 1);
}

Condition ShapeTestEmitter::passesBlockByte(CellPointer cell, const ShapeTest& test)
{
    TypedPointer pointer = m_out.address(m_heaps.AOTObject_shapeWord, cell.value(), offsetOfShapeWord + 1);
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
        m_out.switchOn(shapeWordOf(cell).value(), switchTargets.span(), otherwise);
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
