/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsField.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTPagedBitSet.h"
#include "AOTPrimitives.h"
#include "AOTRuntimeType.h"
#include "JSCJSValue.h"
#include "JSCell.h"
#include "JSType.h"
#include <array>

namespace JSC { namespace AOT {

namespace Primitive = Typed::Primitive;

#if USE(BIGINT32)
#error "The BigInt test is a cell type test for HeapBigIntType and would reject a BigInt32."
#endif

static_assert(RuntimeType::offsetOfCellTests() == sizeof(RuntimeType) && sizeof(RuntimeCellTest) == 8);
static_assert(RuntimeType::sizeInBytes(1, 1, 0) == sizeof(RuntimeType) + 2 * sizeof(uint32_t) + sizeof(uint32_t));
static_assert(RuntimeType::sizeInBytes(0, 0, RuntimeType::hasIntegerRange) == sizeof(RuntimeType) + 2 * sizeof(int64_t));
static_assert(!(shapeWordOf(1) + 1));

namespace {

struct TypeTest {
    StubAssembler& jit;
    const TypeTestRegisters& registers;
    JumpList& isRejected;
    JumpList& cannot;
    JumpList isAccepted;
};

struct ImmediateClass {
    int32_t encoded;
    uint16_t primitive;
};

// Most reference fields hold the objects of one family of layouts. Such a type has one test, and where the shapes of the family have numbers in a row, that test is one comparison.
constexpr auto cellTestKindsInTestOrder = [] {
    std::array<CellTestKind, numberOfEnumerators<CellTestKind>> order { };
    size_t count = 0;
    order[count++] = CellTestKind::ShapeIn;
    for (CellTestKind kind : allMembersOf<CellTestKind>) {
        if (kind != CellTestKind::ShapeIn)
            order[count++] = kind;
    }
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(count == order.size());
    return order;
}();

} // namespace

static void loadTypeRecord(TypeTest& test, GPRReg result)
{
    StubAssembler& jit = test.jit;
#if CPU(X86_64)
    jit.zeroExtend32ToWord(test.registers.type, result);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), result);
#else
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), result);
    jit.addZeroExtend64(result, test.registers.type, result);
#endif
}

static void decideByPrimitive(TypeTest& test, uint16_t primitive)
{
    test.isRejected.append(test.jit.branchTest32(CCallHelpers::Zero, test.registers.scratch, TrustedImm32(primitive)));
    test.isAccepted.append(test.jit.jump());
}

static void testInt32(TypeTest& test)
{
    StubAssembler& jit = test.jit;
    const GPRReg record = test.registers.test;
    const GPRReg primitives = test.registers.scratch;
    const GPRReg range = test.registers.scratch;
    const GPRReg numberOfLiteralStrings = test.registers.secondScratch;
    const GPRReg integer = test.registers.secondScratch;

    test.isRejected.append(jit.branchTest32(CCallHelpers::Zero, primitives, TrustedImm32(Primitive::Integer)));
    test.isAccepted.append(jit.branchTest8(CCallHelpers::Zero, Address(record, RuntimeType::offsetOfFlags()), TrustedImm32(RuntimeType::hasIntegerRange)));
    jit.load16(Address(record, RuntimeType::offsetOfNumberOfCellTests()), range);
    jit.load16(Address(record, RuntimeType::offsetOfNumberOfLiteralStrings()), numberOfLiteralStrings);
    jit.lshift32(TrustedImm32(1), range);
    jit.add32(numberOfLiteralStrings, range);
    jit.getEffectiveAddress(BaseIndex(record, range, CCallHelpers::TimesFour, static_cast<int32_t>(sizeof(RuntimeType))), range);
    jit.signExtend32ToPtr(test.registers.value, integer);
    test.isRejected.append(jit.branch64(CCallHelpers::LessThan, integer, Address(range)));
    test.isRejected.append(jit.branch64(CCallHelpers::GreaterThan, integer, Address(range, sizeof(int64_t))));
    test.isAccepted.append(jit.jump());
}

static void testDouble(TypeTest& test)
{
    StubAssembler& jit = test.jit;
    const GPRReg record = test.registers.test;
    const GPRReg primitives = test.registers.scratch;

    jit.and32(TrustedImm32(Primitive::Number), primitives);
    test.isRejected.append(jit.branchTest32(CCallHelpers::Zero, primitives));
    test.cannot.append(jit.branch32(CCallHelpers::NotEqual, primitives, TrustedImm32(Primitive::Number)));
    test.cannot.append(jit.branchTest8(CCallHelpers::NonZero, Address(record, RuntimeType::offsetOfFlags()), TrustedImm32(RuntimeType::hasIntegerRange)));
    test.isAccepted.append(jit.jump());
}

static void testOtherImmediate(TypeTest& test)
{
    static constexpr ImmediateClass classes[] = {
        { JSValue::ValueUndefined, Primitive::Undefined },
        { JSValue::ValueNull, Primitive::Null },
        { JSValue::ValueTrue, Primitive::True },
        { JSValue::ValueFalse, Primitive::False },
    };
    StubAssembler& jit = test.jit;
    for (const ImmediateClass& immediate : classes) {
        Jump isOtherValue = jit.branch64(CCallHelpers::NotEqual, test.registers.value, TrustedImm32(immediate.encoded));
        decideByPrimitive(test, immediate.primitive);
        isOtherValue.link(&jit);
    }
    test.isRejected.append(jit.jump());
}

static void testNonObjectCell(TypeTest& test)
{
    StubAssembler& jit = test.jit;
    const GPRReg record = test.registers.test;
    const GPRReg primitives = test.registers.scratch;
    const GPRReg cellType = test.registers.secondScratch;

    Jump isNotString = jit.branch32(CCallHelpers::NotEqual, cellType, TrustedImm32(StringType));
    test.cannot.append(jit.branchTest16(CCallHelpers::NonZero, Address(record, RuntimeType::offsetOfNumberOfLiteralStrings())));
    decideByPrimitive(test, Primitive::String);

    isNotString.link(&jit);
    Jump isNotSymbol = jit.branch32(CCallHelpers::NotEqual, cellType, TrustedImm32(SymbolType));
    test.isAccepted.append(jit.branchTest32(CCallHelpers::NonZero, primitives, TrustedImm32(Primitive::Symbol)));
    test.isRejected.append(jit.branchTest16(CCallHelpers::Zero, Address(record, RuntimeType::offsetOfNumberOfCellTests())));
    test.cannot.append(jit.jump());

    isNotSymbol.link(&jit);
    test.isRejected.append(jit.branch32(CCallHelpers::NotEqual, cellType, TrustedImm32(HeapBigIntType)));
    decideByPrimitive(test, Primitive::BigInt);
}

static void testCellType(TypeTest& test, JumpList& testFailed)
{
    StubAssembler& jit = test.jit;
    const GPRReg cellTest = test.registers.test;
    const GPRReg bound = test.registers.scratch;
    const GPRReg cellType = test.registers.secondScratch;

    jit.load8(Address(test.registers.value, JSCell::typeInfoTypeOffset()), cellType);
    jit.load8(Address(cellTest, RuntimeCellTest::offsetOfFirstCellType()), bound);
    testFailed.append(jit.branch32(CCallHelpers::Below, cellType, bound));
    jit.load8(Address(cellTest, RuntimeCellTest::offsetOfLastCellType()), bound);
    test.isAccepted.append(jit.branch32(CCallHelpers::BelowOrEqual, cellType, bound));
    testFailed.append(jit.jump());
}

static void loadShapeTest(TypeTest& test, GPRReg result)
{
    test.jit.load32(Address(test.registers.test, RuntimeCellTest::offsetOfOperand()), result);
    test.jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), result);
}

static void testShape(TypeTest& test, JumpList& testFailed)
{
    StubAssembler& jit = test.jit;
    const GPRReg shapeTest = test.registers.scratch;
    const GPRReg location = test.registers.scratch;
    const GPRReg words = test.registers.scratch;
    const GPRReg bits = test.registers.scratch;
    const GPRReg indexFromLast = test.registers.secondScratch;
    const GPRReg page = test.registers.secondScratch;
    const GPRReg word = test.registers.secondScratch;

    loadShapeTest(test, shapeTest);
    loadShapeWord(jit, test.registers.value, indexFromLast);
    jit.add32(Address(shapeTest, ShapeTestData::offsetOfLast()), indexFromLast);
    testFailed.append(jit.branch32(CCallHelpers::AboveOrEqual, indexFromLast, Address(shapeTest, ShapeTestData::offsetOfCount())));
    jit.load32(Address(shapeTest, ShapeTestData::offsetOfBitSetOffset()), location);
    test.isAccepted.append(jit.branch32(CCallHelpers::Equal, location, TrustedImm32(static_cast<int32_t>(noBitSet))));

    {
        const GPRReg head = test.registers.scratch;
        const GPRReg slot = test.registers.secondScratch;
        const GPRReg held = test.registers.secondScratch;
        constexpr int32_t mark = static_cast<int32_t>(PagedBitSet::hashedInLocation);
        JumpList isNotHashed;
        isNotHashed.append(jit.branchTest32(CCallHelpers::Zero, location, TrustedImm32(PagedBitSet::narrowEntriesInLocation)));
        isNotHashed.append(jit.branchTest32(CCallHelpers::Zero, location, TrustedImm32(PagedBitSet::wideEntriesInLocation)));
        jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(shapeSets)), words);
        jit.load64(Address(words, -mark), head);
        jit.mul32(head, slot);
        jit.urshift64(TrustedImm32(32), head);
        jit.urshift32(head, slot);
        // No third register keeps the words or the index.
        loadShapeTest(test, shapeTest);
        jit.load32(Address(shapeTest, ShapeTestData::offsetOfBitSetOffset()), location);
        jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(shapeSets)), words);
        static_assert(sizeof(PagedBitSet::Slot) == sizeof(uint16_t));
        jit.load16(BaseIndex(words, slot, CCallHelpers::TimesTwo, static_cast<int32_t>(PagedBitSet::offsetOfSlots) - mark), held);
        loadShapeTest(test, shapeTest);
        jit.load32(Address(shapeTest, ShapeTestData::offsetOfLast()), shapeTest);
        jit.add32(Address(test.registers.value, offsetOfShapeWord), shapeTest);
        test.isAccepted.append(jit.branch32(CCallHelpers::Equal, shapeTest, held));
        testFailed.append(jit.jump());
        isNotHashed.link(&jit);
    }

    jit.urshift32(TrustedImm32(PagedBitSet::logBitsPerPage), page);
    RELEASE_ASSERT(page == word);
    JumpList hasWord;
    Jump hasNarrowEntries = jit.branchTest32(CCallHelpers::NonZero, location, TrustedImm32(PagedBitSet::narrowEntriesInLocation));
    Jump hasWideEntries = jit.branchTest32(CCallHelpers::NonZero, location, TrustedImm32(PagedBitSet::wideEntriesInLocation));
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(shapeSets)), words);
    hasWord.append(jit.jump());
    hasNarrowEntries.link(&jit);
    jit.sub32(TrustedImm32(PagedBitSet::narrowEntriesInLocation), location);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(shapeSets)), words);
    jit.load8(BaseIndex(words, page, CCallHelpers::TimesOne), word);
    hasWord.append(jit.jump());
    hasWideEntries.link(&jit);
    jit.sub32(TrustedImm32(PagedBitSet::wideEntriesInLocation), location);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(shapeSets)), words);
    jit.load16(BaseIndex(words, page, CCallHelpers::TimesTwo), word);
    hasWord.link(&jit);
    jit.load64(BaseIndex(words, word, CCallHelpers::TimesEight), bits);

    // No third register keeps the index. A shift looks at the last six bits of it only.
    loadShapeTest(test, indexFromLast);
    jit.load32(Address(indexFromLast, ShapeTestData::offsetOfLast()), indexFromLast);
    jit.add32(Address(test.registers.value, offsetOfShapeWord), indexFromLast);
    jit.urshift64(indexFromLast, bits);
    test.isAccepted.append(jit.branchTest32(CCallHelpers::NonZero, bits, TrustedImm32(1)));
    testFailed.append(jit.jump());
}

static void emitCellTest(TypeTest& test, CellTestKind kind, JumpList& testFailed)
{
    StubAssembler& jit = test.jit;
    const GPRReg testKind = test.registers.scratch;
    auto branchOnKind = [&](CCallHelpers::RelationalCondition condition) {
        return jit.branch32(condition, testKind, TrustedImm32(static_cast<int32_t>(kind)));
    };
    switch (kind) {
    case CellTestKind::IsObject:
        test.isAccepted.append(branchOnKind(CCallHelpers::Equal));
        return;
    case CellTestKind::IsCallable: {
        Jump hasOtherKind = branchOnKind(CCallHelpers::NotEqual);
        test.cannot.append(branchIfNotFunctionType(jit, test.registers.value, test.registers.scratch));
        test.isAccepted.append(jit.jump());
        hasOtherKind.link(&jit);
        return;
    }
    case CellTestKind::CellTypeIs: {
        Jump hasOtherKind = branchOnKind(CCallHelpers::NotEqual);
        testCellType(test, testFailed);
        hasOtherKind.link(&jit);
        return;
    }
    case CellTestKind::ShapeIn: {
        Jump hasOtherKind = branchOnKind(CCallHelpers::NotEqual);
        testShape(test, testFailed);
        hasOtherKind.link(&jit);
        return;
    }
    case CellTestKind::InheritsClassInfo:
    case CellTestKind::IsPristine:
    case CellTestKind::IsBuiltinCell:
    case CellTestKind::IsClosureOf:
    case CellTestKind::IsSymbol:
    case CellTestKind::SharingModeIs:
    case CellTestKind::IsNamespaceOf:
        test.cannot.append(branchOnKind(CCallHelpers::Equal));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void testObject(TypeTest& test)
{
    StubAssembler& jit = test.jit;
    const GPRReg record = test.registers.test;
    const GPRReg cellTest = test.registers.test;
    const GPRReg testKind = test.registers.scratch;
    const GPRReg end = test.registers.scratch;
    const GPRReg numberOfCellTests = test.registers.secondScratch;
    JumpList testFailed;

    test.isRejected.append(jit.branchTest16(CCallHelpers::Zero, Address(record, RuntimeType::offsetOfNumberOfCellTests())));
    jit.addPtr(TrustedImm32(RuntimeType::offsetOfCellTests()), cellTest);

    Label hasCellTest = jit.label();
    jit.load8(Address(cellTest, RuntimeCellTest::offsetOfKind()), testKind);
    for (CellTestKind kind : cellTestKindsInTestOrder)
        emitCellTest(test, kind, testFailed);
    jit.breakpoint();

    testFailed.link(&jit);
    jit.addPtr(TrustedImm32(sizeof(RuntimeCellTest)), cellTest);
    loadTypeRecord(test, end);
    jit.load16(Address(end, RuntimeType::offsetOfNumberOfCellTests()), numberOfCellTests);
    jit.getEffectiveAddress(BaseIndex(end, numberOfCellTests, CCallHelpers::TimesEight, static_cast<int32_t>(RuntimeType::offsetOfCellTests())), end);
    jit.branchPtr(CCallHelpers::Below, cellTest, end).linkTo(hasCellTest, &jit);
    test.isRejected.append(jit.jump());
}

// Reads bits 0 to 31 of type and preserves the register on every path.
void branchUnlessTypeAccepts(StubAssembler& jit, const TypeTestRegisters& r, JumpList& isRejected, JumpList& cannot)
{
    RELEASE_ASSERT(noOverlap(r.value, r.type, r.test, r.scratch, r.secondScratch, instanceGPR));
    TypeTest test { jit, r, isRejected, cannot, { } };

    cannot.append(jit.branchTest32(CCallHelpers::Zero, r.type));
    loadTypeRecord(test, r.test);
    jit.load16(Address(r.test, RuntimeType::offsetOfPrimitives()), r.scratch);
    Jump isCell = jit.branchIfCell(r.value);
    Jump isNotInt32 = jit.branchIfNotInt32(r.value);
    testInt32(test);

    isNotInt32.link(&jit);
    Jump isNotNumber = jit.branchIfNotNumber(r.value);
    testDouble(test);

    isNotNumber.link(&jit);
    testOtherImmediate(test);

    isCell.link(&jit);
    jit.load8(Address(r.value, JSCell::typeInfoTypeOffset()), r.secondScratch);
    Jump isObject = jit.branch32(CCallHelpers::AboveOrEqual, r.secondScratch, TrustedImm32(ObjectType));
    testNonObjectCell(test);

    isObject.link(&jit);
    testObject(test);

    test.isAccepted.link(&jit);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
