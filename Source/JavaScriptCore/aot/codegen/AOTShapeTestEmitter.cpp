/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTShapeTestEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompileUnit.h"
#include "AOTEffectsNet.h"
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
    return bitOr(loadField<Word32>(cell, m_heaps.AOTObject_shapeNumber), word32(static_cast<int32_t>(shapeWordIsUnmarked)));
}

Word32 ShapeTestEmitter::shapeWordOf(CellPointer cell, LayoutSetID possible)
{
    return mayBearMark(possible) ? shapeWordOf(cell) : shapeWordWithMarkOf(cell);
}

bool ShapeTestEmitter::mayBearMark(LayoutSetID layouts)
{
    if (!layouts)
        return true;
    return (liesInThread(layouts) ? m_setsThatMayBearMark : testsKeptAcrossUnits().setsThatMayBearMark).ensure(layouts, [&] {
        return std::ranges::any_of(membersOf(typeReader(), layouts), [&](LayoutID member) {
            return m_plan.layout(member).mayBearMark;
        });
    }).iterator->value;
}

ShapeTestEmitter::IndexByShape ShapeTestEmitter::indexByShapeOf(CellPointer cell)
{
    if (isX86_64() && m_plan.shapes().numberOfShapes() <= 1u << 16)
        return { zeroExtended(Word32(m_out.load16ZeroExt32(m_out.address(m_heaps.AOTObject_shapeNumber, cell.value(), offsetOfShapeWord)))), -static_cast<int32_t>(sizeof(uint32_t) << 16) };
    return { signExtended(shapeWordOf(cell)), 0 };
}

bool ShapeTestEmitter::paysForMark(LayoutSetID tested, const Type& known)
{
    return mayBearMark(tested) && (!known.hasOnlyLayouts() || known.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow));
}

Word32 ShapeTestEmitter::shapeWordWithMarkOf(CellPointer cell)
{
    return loadField<Word32>(cell, m_wordIsAskedForMark ? m_heaps.AOTObject_shapeWord : m_heaps.AOTObject_shapeNumber);
}

Condition ShapeTestEmitter::bearsMark(CellPointer cell)
{
    return hasNoBits(Word32(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTObject_shapeWord, cell.value(), offsetOfByteWithMark))), byteWithMarkIsUnmarked);
}

Condition ShapeTestEmitter::bearsNoMark(CellPointer cell)
{
    return hasBits(Word32(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTObject_shapeWord, cell.value(), offsetOfByteWithMark))), byteWithMarkIsUnmarked);
}

void ShapeTestEmitter::setMark(CellPointer cell)
{
    CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap::No);
    storeField(cell, m_heaps.AOTObject_shapeWord, bitAnd(loadField<Word32>(cell, m_heaps.AOTObject_shapeWord), word32(static_cast<int32_t>(~shapeWordIsUnmarked))));
}

void ShapeTestEmitter::clearMark(CellPointer cell)
{
    CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap::No);
    storeField(cell, m_heaps.AOTObject_shapeWord, bitOr(loadField<Word32>(cell, m_heaps.AOTObject_shapeWord), word32(static_cast<int32_t>(shapeWordIsUnmarked))));
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
    return hasBits(shapeWordWithMarkOf(cell), compiledObjectShapeBit);
}

ShapeTest ShapeTestEmitter::testOf(LayoutSetID tested) const
{
    if (tested && tested == m_setTestedLast)
        return m_testOfSetTestedLast;
    ShapeTest test = m_plan.shapes().testOf(tested, membersOf(typeReader(), tested));
    RELEASE_ASSERT(holdsForAnyCell(test.form));
    m_setTestedLast = tested;
    m_testOfSetTestedLast = test;
    return test;
}

ShapeTestsKeptAcrossUnits& ShapeTestEmitter::testsKeptAcrossUnits()
{
    if (!m_testsKeptAcrossUnits) {
        m_testsKeptAcrossUnits = &generator().procedure().arena().objectKeptAcrossResets<ShapeTestsKeptAcrossUnits>();
        if (m_testsKeptAcrossUnits->table != &m_plan.shapes()) {
            m_testsKeptAcrossUnits->entries.fill(ShapeTestsKeptAcrossUnits::Entry { });
            m_testsKeptAcrossUnits->setsThatMayBearMark.clear();
            m_testsKeptAcrossUnits->table = &m_plan.shapes();
        }
    }
    return *m_testsKeptAcrossUnits;
}

ShapeTestsKeptAcrossUnits::Entry& ShapeTestEmitter::entryKeptAcrossUnits(uint64_t testedAndKnown)
{
    return testsKeptAcrossUnits().entries[(testedAndKnown * 0x9E3779B97F4A7C15ull) >> (64 - ShapeTestsKeptAcrossUnits::numberOfBits)];
}

Condition ShapeTestEmitter::hasShapeIn(CellPointer cell, LayoutSetID tested, const Type& known, MarkedObjects marked)
{
    SetForScope wordIsAskedForMark(m_wordIsAskedForMark, marked != MarkedObjects::Pass && mayBearMark(tested));
    m_lastTestRejectsMarks = marked != MarkedObjects::Pass && marked != MarkedObjects::PassAlone;
    m_lastTestIsHeldAgainstEveryShape = true;
    bool shouldIgnoreMark = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::IgnoreMarksInShapeChecks);
    WordOfTest word = (m_lastTestRejectsMarks && !shouldIgnoreMark) || !mayBearMark(tested) ? WordOfTest::AsItLies : WordOfTest::WithoutMark;
    if (isX86_64() && marked == MarkedObjects::PassAlone && word == WordOfTest::WithoutMark && !shouldIgnoreMark)
        word = WordOfTest::WithMark;
    if (marked == MarkedObjects::AreRefused || marked == MarkedObjects::AreAskedAgain || marked == MarkedObjects::PassAlone || !known.hasOnlyLayouts() || known.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return passes(cell, tested, testOf(tested), word);
    const ShapeTable& shapes = m_plan.shapes();
    if (m_setWhoseShapesAreKept != known.layouts) {
        m_setWhoseShapesAreKept = known.layouts;
        m_keptSetHoldsCompiledObjectsAlone = hasOnlyCompiledObjects(known);
        m_shapesKept.shrink(0);
        m_shapesOfKeptSetAreListed = false;
    }
    if (!m_keptSetHoldsCompiledObjectsAlone)
        return passes(cell, tested, testOf(tested), word);

    m_lastTestRejectsMarks = false;
    m_lastTestIsHeldAgainstEveryShape = false;
    if (uint32_t word = build().options().understatedEffects) [[unlikely]] {
        UnderstatedEffects row = UnderstatedEffects::decode(word);
        if (row.part == PartOfEffects::ShapesKnown && generator().unit().function.index() == row.row) {
            ShapeTable::ShapeList testedShapes;
            ShapeTable::ShapeList knownShapes;
            shapes.appendMemberShapes(membersOf(typeReader(), tested), testedShapes);
            shapes.appendMemberShapes(membersOf(typeReader(), known), knownShapes);
            knownShapes.removeLast();
            ShapeTest test = shapes.testOfShapes(testedShapes.span(), knownShapes.span());
            if (test.form == ShapeTestForm::BitSet) {
                if (std::optional<Condition> passesTest = passesBitSetThatHoldsEveryKnownShape(shapeWordOf(cell, known.layouts), tested, LayoutSetID(), testedShapes.span(), knownShapes.span()))
                    return *passesTest;
            }
            return passes(cell, tested, test, mayBearMark(known.layouts) ? WordOfTest::WithoutMark : WordOfTest::AsItLies);
        }
    }
    uint64_t testedAndKnown = static_cast<uint64_t>(tested.bits()) << 32 | known.layouts.bits();
    if (tested) {
        if (auto remembered = m_testsAmongKnownShapes.find(testedAndKnown); remembered != m_testsAmongKnownShapes.end())
            return passes(cell, tested, remembered->value, mayBearMark(known.layouts) ? WordOfTest::WithoutMark : WordOfTest::AsItLies);
    }
    ShapeTestsKeptAcrossUnits::Entry* kept = tested && !liesInThread(tested) && !liesInThread(known.layouts) ? &entryKeptAcrossUnits(testedAndKnown) : nullptr;
    if (kept && kept->testedAndKnown == testedAndKnown)
        return passes(cell, tested, kept->test, mayBearMark(known.layouts) ? WordOfTest::WithoutMark : WordOfTest::AsItLies);
    if (!m_shapesOfKeptSetAreListed) {
        m_shapesOfKeptSetAreListed = true;
        shapes.appendMemberShapes(membersOf(typeReader(), known), m_shapesKept);
    }
    ShapeTable::ShapeList testedShapes;
    shapes.appendMemberShapes(membersOf(typeReader(), tested), testedShapes);
    const ShapeTable::ShapeList& knownShapes = m_shapesKept;
    ShapeTest test = shapes.testOfShapes(testedShapes.span(), knownShapes.span());

    if (test.form == ShapeTestForm::BitSet) {
        if (std::optional<Condition> passesTest = passesBitSetThatHoldsEveryKnownShape(shapeWordOf(cell, known.layouts), tested, known.layouts, testedShapes.span(), knownShapes.span()))
            return *passesTest;
    } else if (kept)
        *kept = ShapeTestsKeptAcrossUnits::Entry { testedAndKnown, test };
    else if (tested)
        m_testsAmongKnownShapes.add(testedAndKnown, test);
    return passes(cell, tested, test, mayBearMark(known.layouts) ? WordOfTest::WithoutMark : WordOfTest::AsItLies);
}

Condition ShapeTestEmitter::isShapeOf(Word32 shapeWord, LayoutID layout)
{
    ShapeRange shapes = m_plan.shapes().shapesOf(layout);
    if (shapes.isEmpty())
        return boolean(false);
    return passesRange(shapeWord, ShapeTest { .form = ShapeTestForm::Range, .first = shapes.first, .count = shapes.count });
}

void ShapeTestEmitter::branchOnShapeIn(CellPointer cell, LayoutSetID tested, const Type& known, BranchTargets targets, MarkedObjects marked)
{
    if (marked == MarkedObjects::Pass && paysForMark(tested, known)) {
        LBasicBlock failsAsItLies = newBlock();
        branch(hasShapeIn(cell, tested, known, MarkedObjects::AreAskedAgain), { targets.taken, unlikelyEdge(failsAsItLies) });
        m_out.appendTo(failsAsItLies);
        marked = MarkedObjects::PassAlone;
    }
    branch(hasShapeIn(cell, tested, known, marked), targets);
}

Vector<ShapeTestEmitter::ShapesOfMember, 8> ShapeTestEmitter::shapesOfMembersOf(LayoutSetID layouts)
{
    Vector<ShapesOfMember, 8> listed;
    for (LayoutID member : membersOf(typeReader(), layouts)) {
        ShapesOfMember shapesOfMember { member, { } };
        m_plan.shapes().appendMemberShapes(std::span<const LayoutID>(&member, 1), shapesOfMember.shapes);
        listed.append(WTF::move(shapesOfMember));
    }
    return listed;
}

Word32 ShapeTestEmitter::distanceOfUnmarkedObjectFromShape(CellPointer cell, LayoutSetID tested, uint32_t shape)
{
    SetForScope wordIsAskedForMark(m_wordIsAskedForMark, mayBearMark(tested));
    m_lastTestRejectsMarks = true;
    bool shouldIgnoreMark = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::IgnoreMarksInShapeChecks);
    return add(shouldIgnoreMark && mayBearMark(tested) ? shapeWordOf(cell) : shapeWordWithMarkOf(cell), shape);
}

void ShapeTestEmitter::branchOnShapeAmongReceivers(const ValueContext& access, CellPointer cell, LayoutSetID tested, BranchTargets targets)
{
    TypeID known = types().intersectionOf(typeIDOf(access->objectOperand()), types().forLayoutSet(access->receivers()));
    branchOnShapeIn(cell, tested, types().at(known), targets);
}

Condition ShapeTestEmitter::passes(CellPointer cell, LayoutSetID tested, const ShapeTest& test, WordOfTest word)
{
    switch (test.form) {
    case ShapeTestForm::NeverPasses:
        return boolean(false);
    case ShapeTestForm::AlwaysPasses:
        return boolean(true);
    case ShapeTestForm::Block:
    case ShapeTestForm::BlockOrEarlier:
        return passesBlockByte(cell, test, word);
    case ShapeTestForm::OneShape:
        if (word == WordOfTest::WithMark)
            return equal(shapeWordWithMarkOf(cell), word32(AOT::shapeWordOf(test.first) & ~shapeWordIsUnmarked));
        break;
    case ShapeTestForm::UpTo:
    case ShapeTestForm::From:
    case ShapeTestForm::Range:
    case ShapeTestForm::ImmediateBits:
    case ShapeTestForm::BitSet:
        break;
    }
    return passes(word == WordOfTest::AsItLies ? shapeWordWithMarkOf(cell) : shapeWordOf(cell), tested, test);
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

Condition ShapeTestEmitter::passesBlockByte(CellPointer cell, const ShapeTest& test, WordOfTest word)
{
    TypedPointer pointer = m_out.address(m_wordIsAskedForMark ? m_heaps.AOTObject_shapeWord : m_heaps.AOTObject_shapeNumber, cell.value(), offsetOfShapeWord + 1);
    uint16_t expectedHalfword = blockTestHalfword(test.blockByte);
    Word32 halfword(isX86_64() ? m_out.load16ZeroExt32(pointer) : m_out.load16SignExt32(pointer));
    Word32 expected = word32(isX86_64() ? expectedHalfword : static_cast<int16_t>(expectedHalfword));
    if (word != WordOfTest::AsItLies) {
        constexpr uint16_t halfwordIsUnmarked = shapeWordIsUnmarked >> 8;
        halfword = bitOr(halfword, word32(isX86_64() ? halfwordIsUnmarked : static_cast<int16_t>(halfwordIsUnmarked)));
    }
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

Condition ShapeTestEmitter::passesBitSet(Word32 shapeWord, LayoutSetID tested, const ShapeTest& test)
{
    Word32 distanceFromLast = add(shapeWord, word32(test.last()));
    BitSetConstant constant = bitSetConstant(tested);
    Pointer words(m_out.relocatable(SymbolSpace::Constant, constant.index));
    auto addressOf = [&](Word32 element, unsigned logSize, ptrdiff_t offset = 0) {
        return m_out.address(m_heaps.AOTImage_tables, offsetBy(words, zeroExtended(element), logSize).value(), offset);
    };
    if (constant.entries == PagedBitSet::Entries::Hashed) {
        static_assert(sizeof(PagedBitSet::Slot) == sizeof(uint16_t));
        Word32 slot = shiftRightUnsigned(mul(distanceFromLast, word32(PagedBitSet::multiplierIn(constant.headOfHashedSet))), static_cast<int32_t>(PagedBitSet::shiftIn(constant.headOfHashedSet)));
        Word32 held(m_out.load16ZeroExt32(addressOf(slot, PagedBitSet::logBytesPerSlot, PagedBitSet::offsetOfSlots)));
        return equal(held, distanceFromLast);
    }
    Word32 index = select(below(distanceFromLast, test.count), distanceFromLast, test.count);
    if (constant.entries == PagedBitSet::Entries::None) {
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
        Vector<uint32_t, 16> halves(FillWith { }, ((last - first + 1) >> logBitsPerHalf) + 1, 0u);
        for (uint32_t shape : testedShapes) {
            if (shape < first || shape > last)
                continue;
            uint32_t distanceFromLast = last - shape;
            halves[distanceFromLast >> logBitsPerHalf] |= 1u << (distanceFromLast & ((1u << logBitsPerHalf) - 1));
        }
        return m_out.addConstantData(asBytes(halves.span()), ConstantKind::ShapeBitmap);
    }).iterator->value;

    uint32_t count = last - first + 1;
    Word32 distanceFromLast = add(shapeWord, word32(last));
    Word32 index = select(below(distanceFromLast, count), distanceFromLast, count);
    Pointer halves(m_out.relocatable(SymbolSpace::Constant, constant));
    Pointer half = offsetBy(halves, zeroExtended(shiftRightUnsigned(index, word32(logBitsPerHalf))), logBytesPerHalf);
    Word32 bits(m_out.load32(m_out.address(m_heaps.AOTImage_tables, half.value())));
    return hasBits(shiftRightUnsigned(bits, index), 1);
}

void ShapeTestEmitter::switchOnShape(CellPointer cell, std::span<const LayoutSetID> cases, std::span<const BlockEdge> targets, BlockEdge otherwise)
{
    RELEASE_ASSERT(cases.size() == targets.size());
    Vector<ShapeTest, 8> tests;
    tests.reserveInitialCapacity(cases.size());
    for (LayoutSetID layouts : cases)
        tests.append(testOf(layouts));
    bool someCaseMayBearMark = std::ranges::any_of(cases, [&](LayoutSetID layouts) {
        return mayBearMark(layouts);
    });
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
        AOT::stableSort(switchTargets, { }, &SwitchTarget::caseValue);
        switchTargets.shrink(std::ranges::unique(switchTargets, { }, &SwitchTarget::caseValue).begin() - switchTargets.begin());
        if (switchTargets.isEmpty()) {
            m_out.jump(otherwise.block());
            return;
        }
        m_out.switchOn((someCaseMayBearMark ? shapeWordOf(cell) : shapeWordWithMarkOf(cell)).value(), switchTargets.span(), otherwise);
        return;
    }

    for (size_t index = 0; index < cases.size(); ++index) {
        bool isLast = index + 1 == cases.size();
        BlockEdge next = isLast ? otherwise : normalEdge(newBlock());
        branch(passes(cell, cases[index], tests[index], someCaseMayBearMark ? WordOfTest::WithoutMark : WordOfTest::AsItLies), { targets[index], next });
        if (!isLast)
            m_out.appendToContinuation(next.block());
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
