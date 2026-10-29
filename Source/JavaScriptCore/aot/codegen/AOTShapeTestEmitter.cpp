/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTShapeTestEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTLayout.h"
#include "AOTPlacement.h"
#include "AOTShapeTest.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "BrandedStructure.h"
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
    checkClaim(cell, [&] {
        return isCompiledObject(cell);
    });
    return orderWordOfAnyCell(cell);
}

Word32 ShapeTestEmitter::orderWordOfAnyCell(CellPointer cell)
{
    return loadField<Word32>(cell, m_heaps.AOTObject_orderWord);
}

// The object of a layout without compiled storage, such as a module namespace, has no shape word.
bool ShapeTestEmitter::hasOnlyCompiledObjects(const Type& known) const
{
    const SlotPlan& slots = m_placement.slots();
    return known.hasOnlyLayouts() && std::ranges::all_of(membersOf(m_program, known), [&](LayoutID member) {
        return slots.holdsCompiledObjects(member) || m_program.at(member).flow.allocation == AllocationState::NeverAllocated;
    });
}

Condition ShapeTestEmitter::hasNoExtraProperties(CellPointer cell)
{
    return hasNoBits(orderWordOf(cell), orderWordHasExtraProperties);
}

Condition ShapeTestEmitter::carriesBrandOfClass(CellPointer cell, LayoutID classLayout)
{
    static_assert(sizeof(CompactRefPtr<UniquedStringImpl>) == sizeof(void*));
    std::optional<uint32_t> rank = m_placement.rankOfClass(classLayout);
    if (!rank)
        return boolean(false);
    ptrdiff_t offsetOfBrand = static_cast<ptrdiff_t>(*rank) * sizeof(void*);
    Pointer structure = loadStructure(cell);
    Condition isBranded = equal(Word32(m_out.load8ZeroExt32(structure.value(), m_heaps.Structure_variant)), word32(static_cast<uint32_t>(Structure::StructureVariant::Branded)));
    return selectByBranch<Condition>(isBranded, [&] {
        Pointer brands = loadImmutablePointer(AOT_INSTANCE_OFFSET(classes.brandsOfInstances));
        LValue brandOfClass = m_out.loadPtr(m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), brands.value(), offsetOfBrand));

        // The class's brand is the first one the object got. Code evaluated at run time may have added others on top.
        LBasicBlock loop = newBlock();
        LBasicBlock earlierBrand = newBlock();
        LBasicBlock continuation = newBlock();
        ValueFromBlock latest = m_out.anchor(structure.value());
        m_out.jump(loop);

        m_out.appendTo(loop);
        LValue branded = m_out.phi(B3::pointerType(), latest);
        ValueFromBlock found = m_out.anchor(boolean(true).value());
        m_out.branch(m_out.equal(m_out.loadPtr(branded, m_heaps.BrandedStructure_brand), brandOfClass), normalEdge(continuation), normalEdge(earlierBrand));

        m_out.appendTo(earlierBrand);
        static_assert(sizeof(WriteBarrierStructureID) == sizeof(uint32_t));
        Word32 earlier(m_out.load32(branded, m_heaps.BrandedStructure_parentBrand));
        m_out.addIncomingToPhi(branded, m_out.anchor(decodeStructureID(earlier).value()));
        ValueFromBlock notFound = m_out.anchor(boolean(false).value());
        m_out.branch(m_out.isZero32(earlier.value()), normalEdge(continuation), unlikelyEdge(loop));

        m_out.appendTo(continuation);
        return Condition(m_out.phi(B3::Int32, found, notFound));
    }, [&] {
        return boolean(false);
    });
}

bool ShapeTestEmitter::hasOnlyCellsWithBrands(const Type& type) const
{
    AOT_THRESHOLD(size_t, maxClasses, 4);
    if (!type.hasOnlyLayouts())
        return false;
    std::span<const LayoutID> members = membersOf(m_program, type);
    return !members.empty() && members.size() <= maxClasses && std::ranges::all_of(members, [&](LayoutID member) {
        return Typed::carriesBrandOfClass(m_program, member) && m_placement.rankOfClass(member);
    });
}

bool ShapeTestEmitter::mayBeCellWithBrand(const Type& type) const
{
    return type.layouts && std::ranges::any_of(membersOf(m_program, type), [&](LayoutID member) {
        return Typed::carriesBrandOfClass(m_program, member);
    });
}

Condition ShapeTestEmitter::isCompiledObject(CellPointer cell)
{
    return hasBits(shapeWordOf(cell), compiledObjectShapeBit);
}

Condition ShapeTestEmitter::hasPrototypeOfItsClass(CellPointer cell, const Type& cellsWithBrands)
{
    Pointer structure = loadStructure(cell);
    LValue prototype = m_out.load64(structure.value(), m_heaps.Structure_prototype);
    Pointer prototypes = loadImmutablePointer(AOT_INSTANCE_OFFSET(classes.classPrototypes));
    std::span<const LayoutID> members = membersOf(m_program, cellsWithBrands);
    if (members.size() == 1) {
        ptrdiff_t offset = static_cast<ptrdiff_t>(*m_placement.rankOfClass(members[0])) * sizeof(void*);
        return Condition(m_out.equal(prototype, m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), prototypes.value(), offset))));
    }
    Condition result;
    for (LayoutID member : members) {
        ptrdiff_t offset = static_cast<ptrdiff_t>(*m_placement.rankOfClass(member)) * sizeof(void*);
        Condition hasPrototype(m_out.equal(prototype, m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), prototypes.value(), offset))));
        result = logicalOr(result, logicalAnd(carriesBrandOfClass(cell, member), hasPrototype));
    }
    return result;
}

Condition ShapeTestEmitter::hasNoPropertyOfItsOwn(CellPointer cell)
{
    Pointer structure = loadStructure(cell);
    return equal(Word32(m_out.load16ZeroExt32(structure.value(), m_heaps.Structure_maxOffset)), word32(static_cast<uint32_t>(Structure::greatestMaxOffsetOutsideRareData + 1)));
}

bool ShapeTestEmitter::hasOnlyCellsCreatedWithoutProperties(const Type& cellsWithBrands) const
{
    return std::ranges::none_of(membersOf(m_program, cellsWithBrands), [&](LayoutID member) {
        return isCreatedWithProperties(m_program, member, m_program.at(member).writtenBuiltinBase);
    });
}

// The runtime examines each Structure once between two garbage collections.
Condition ShapeTestEmitter::hasOnlyClassFieldNames(CellPointer cell)
{
    Word32 structureID = loadStructureID(cell);
    Pointer known = loadImmutablePointer(AOT_INSTANCE_OFFSET(classes.structureNameCache));
    Word32 index = bitAnd(shiftRightUnsigned(structureID, static_cast<int32_t>(structureNameCacheShift)), word32(structureNameCacheSize - 1));
    LValue place = m_out.add(known.value(), m_out.shl(m_out.zeroExtPtr(index.value()), m_out.constIntPtr(2)));
    Word32 entry(m_out.load32(m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), place, 0)));
    return either<Condition>(equal(entry, structureID), [&] {
        return boolean(true);
    }, [&] {
        return either<Condition>(equal(entry, bitOr(structureID, word32(structureHasOtherNames))), [&] {
            return boolean(false);
        }, [&] {
            return callWithoutSite<Condition>(RuntimeOperation::HasOnlyClassFieldNames, { cell }, { .isOnRarePath = true });
        });
    });
}

Condition ShapeTestEmitter::hasShapeIn(CellPointer cell, LayoutSetID tested, const Type& known)
{
    const ShapeTable& shapes = m_placement.shapes();
    bool isKnownCompiledObject = hasOnlyCompiledObjects(known);
    const ShapeTest& test = isKnownCompiledObject ? shapes.testOf(tested, known.layouts) : shapes.testOf(tested);
    // The test tells the tested shapes from the known ones alone.
    if (isKnownCompiledObject)
        checkIsCompiledObjectOf(cell, known.layouts);

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
        Word32 slot = shiftRightUnsigned(mul(distanceFromLast, PagedBitSet::multiplierIn(constant.headOfHashedSet)), PagedBitSet::shiftIn(constant.headOfHashedSet));
        static_assert(sizeof(PagedBitSet::Slot) == sizeof(uint16_t));
        Word32 held(m_out.load16ZeroExt32(m_out.address(m_heaps.AOTImage_tables, offsetBy(words, zeroExtended(slot), PagedBitSet::logBytesPerSlot).value(), PagedBitSet::offsetOfSlots)));
        return equal(held, distanceFromLast);
    }
    Word32 index = select(below(distanceFromLast, test.count), distanceFromLast, test.count);
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
