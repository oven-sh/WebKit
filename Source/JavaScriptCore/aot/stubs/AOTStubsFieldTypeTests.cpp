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
#include <utility>

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

// Reads bits 0 to 31 of type and preserves the register on every path.
void branchUnlessTypeAccepts(StubAssembler& jit, const TypeTestRegisters& r, JumpList& isRejected, JumpList& cannot)
{
    RELEASE_ASSERT(noOverlap(r.value, r.type, r.test, r.scratch, r.secondScratch, instanceGPR));
    const GPRReg record = r.test;
    const GPRReg cellTest = r.test;
    const GPRReg primitives = r.scratch;
    const GPRReg testKind = r.scratch;
    const GPRReg cellType = r.secondScratch;
    JumpList isAccepted;
    JumpList testFailed;

    auto loadTypeRecord = [&](GPRReg result) {
#if CPU(X86_64)
        jit.zeroExtend32ToWord(r.type, result);
        jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), result);
#else
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), result);
        jit.addZeroExtend64(result, r.type, result);
#endif
    };
    auto decideByPrimitive = [&](uint16_t primitive) {
        isRejected.append(jit.branchTest32(CCallHelpers::Zero, r.scratch, TrustedImm32(primitive)));
        isAccepted.append(jit.jump());
    };
    auto testCellType = [&] {
        const GPRReg bound = r.scratch;
        jit.load8(Address(r.value, JSCell::typeInfoTypeOffset()), cellType);
        jit.load8(Address(cellTest, RuntimeCellTest::offsetOfFirstCellType()), bound);
        testFailed.append(jit.branch32(CCallHelpers::Below, cellType, bound));
        jit.load8(Address(cellTest, RuntimeCellTest::offsetOfLastCellType()), bound);
        isAccepted.append(jit.branch32(CCallHelpers::BelowOrEqual, cellType, bound));
        testFailed.append(jit.jump());
    };
    auto loadShapeTest = [&](GPRReg result) {
        jit.load32(Address(r.test, RuntimeCellTest::offsetOfOperand()), result);
        jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), result);
    };
    auto testShape = [&] {
        const GPRReg shapeTest = r.scratch;
        const GPRReg location = r.scratch;
        const GPRReg words = r.scratch;
        const GPRReg bits = r.scratch;
        const GPRReg indexFromLast = r.secondScratch;
        const GPRReg page = r.secondScratch;
        const GPRReg word = r.secondScratch;

        loadShapeTest(shapeTest);
        loadShapeWord(jit, r.value, indexFromLast);
        jit.add32(Address(shapeTest, ShapeTestData::offsetOfLast()), indexFromLast);
        testFailed.append(jit.branch32(CCallHelpers::AboveOrEqual, indexFromLast, Address(shapeTest, ShapeTestData::offsetOfCount())));
        jit.load32(Address(shapeTest, ShapeTestData::offsetOfBitSetOffset()), location);
        isAccepted.append(jit.branch32(CCallHelpers::Equal, location, TrustedImm32(static_cast<int32_t>(noBitSet))));

        {
            const GPRReg head = r.scratch;
            const GPRReg slot = r.secondScratch;
            const GPRReg held = r.secondScratch;
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
            loadShapeTest(shapeTest);
            jit.load32(Address(shapeTest, ShapeTestData::offsetOfBitSetOffset()), location);
            jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(shapeSets)), words);
            static_assert(sizeof(PagedBitSet::Slot) == sizeof(uint16_t));
            jit.load16(BaseIndex(words, slot, CCallHelpers::TimesTwo, static_cast<int32_t>(PagedBitSet::offsetOfSlots) - mark), held);
            loadShapeTest(shapeTest);
            jit.load32(Address(shapeTest, ShapeTestData::offsetOfLast()), shapeTest);
            jit.add32(Address(r.value, offsetOfShapeWord), shapeTest);
            isAccepted.append(jit.branch32(CCallHelpers::Equal, shapeTest, held));
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
        loadShapeTest(indexFromLast);
        jit.load32(Address(indexFromLast, ShapeTestData::offsetOfLast()), indexFromLast);
        jit.add32(Address(r.value, offsetOfShapeWord), indexFromLast);
        jit.urshift64(indexFromLast, bits);
        isAccepted.append(jit.branchTest32(CCallHelpers::NonZero, bits, TrustedImm32(1)));
        testFailed.append(jit.jump());
    };
    auto emitCellTest = [&](CellTestKind kind) {
        auto branchOnKind = [&](CCallHelpers::RelationalCondition condition) {
            return jit.branch32(condition, testKind, TrustedImm32(static_cast<int32_t>(kind)));
        };
        switch (kind) {
        case CellTestKind::IsObject:
            isAccepted.append(branchOnKind(CCallHelpers::Equal));
            return;
        case CellTestKind::IsCallable:
        case CellTestKind::CellTypeIs:
        case CellTestKind::ShapeIn: {
            Jump hasOtherKind = branchOnKind(CCallHelpers::NotEqual);
            if (kind == CellTestKind::ShapeIn)
                testShape();
            else if (kind == CellTestKind::CellTypeIs)
                testCellType();
            else {
                cannot.append(branchIfNotFunctionType(jit, r.value, r.scratch));
                isAccepted.append(jit.jump());
            }
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
            cannot.append(branchOnKind(CCallHelpers::Equal));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    cannot.append(jit.branchTest32(CCallHelpers::Zero, r.type));
    loadTypeRecord(record);
    jit.load16(Address(record, RuntimeType::offsetOfPrimitives()), primitives);
    Jump isCell = jit.branchIfCell(r.value);
    Jump isNotInt32 = jit.branchIfNotInt32(r.value);
    const GPRReg range = r.scratch;
    const GPRReg numberOfLiteralStrings = r.secondScratch;
    const GPRReg integer = r.secondScratch;
    isRejected.append(jit.branchTest32(CCallHelpers::Zero, primitives, TrustedImm32(Primitive::Integer)));
    isAccepted.append(jit.branchTest8(CCallHelpers::Zero, Address(record, RuntimeType::offsetOfFlags()), TrustedImm32(RuntimeType::hasIntegerRange)));
    jit.load16(Address(record, RuntimeType::offsetOfNumberOfCellTests()), range);
    jit.load16(Address(record, RuntimeType::offsetOfNumberOfLiteralStrings()), numberOfLiteralStrings);
    jit.lshift32(TrustedImm32(1), range);
    jit.add32(numberOfLiteralStrings, range);
    jit.getEffectiveAddress(BaseIndex(record, range, CCallHelpers::TimesFour, static_cast<int32_t>(sizeof(RuntimeType))), range);
    jit.signExtend32ToPtr(r.value, integer);
    isRejected.append(jit.branch64(CCallHelpers::LessThan, integer, Address(range)));
    isRejected.append(jit.branch64(CCallHelpers::GreaterThan, integer, Address(range, sizeof(int64_t))));
    isAccepted.append(jit.jump());

    isNotInt32.link(&jit);
    Jump isNotNumber = jit.branchIfNotNumber(r.value);
    jit.and32(TrustedImm32(Primitive::Number), primitives);
    isRejected.append(jit.branchTest32(CCallHelpers::Zero, primitives));
    cannot.append(jit.branch32(CCallHelpers::NotEqual, primitives, TrustedImm32(Primitive::Number)));
    cannot.append(jit.branchTest8(CCallHelpers::NonZero, Address(record, RuntimeType::offsetOfFlags()), TrustedImm32(RuntimeType::hasIntegerRange)));
    isAccepted.append(jit.jump());

    isNotNumber.link(&jit);
    static constexpr std::pair<int32_t, uint16_t> primitivesOfImmediates[] = {
        { JSValue::ValueUndefined, Primitive::Undefined },
        { JSValue::ValueNull, Primitive::Null },
        { JSValue::ValueTrue, Primitive::True },
        { JSValue::ValueFalse, Primitive::False },
    };
    for (auto [encoded, primitive] : primitivesOfImmediates) {
        Jump isOtherValue = jit.branch64(CCallHelpers::NotEqual, r.value, TrustedImm32(encoded));
        decideByPrimitive(primitive);
        isOtherValue.link(&jit);
    }
    isRejected.append(jit.jump());

    isCell.link(&jit);
    jit.load8(Address(r.value, JSCell::typeInfoTypeOffset()), cellType);
    Jump isObject = jit.branch32(CCallHelpers::AboveOrEqual, cellType, TrustedImm32(ObjectType));
    Jump isNotString = jit.branch32(CCallHelpers::NotEqual, cellType, TrustedImm32(StringType));
    cannot.append(jit.branchTest16(CCallHelpers::NonZero, Address(record, RuntimeType::offsetOfNumberOfLiteralStrings())));
    decideByPrimitive(Primitive::String);

    isNotString.link(&jit);
    Jump isNotSymbol = jit.branch32(CCallHelpers::NotEqual, cellType, TrustedImm32(SymbolType));
    isAccepted.append(jit.branchTest32(CCallHelpers::NonZero, primitives, TrustedImm32(Primitive::Symbol)));
    isRejected.append(jit.branchTest16(CCallHelpers::Zero, Address(record, RuntimeType::offsetOfNumberOfCellTests())));
    cannot.append(jit.jump());

    isNotSymbol.link(&jit);
    isRejected.append(jit.branch32(CCallHelpers::NotEqual, cellType, TrustedImm32(HeapBigIntType)));
    decideByPrimitive(Primitive::BigInt);

    isObject.link(&jit);
    const GPRReg end = r.scratch;
    const GPRReg numberOfCellTests = r.secondScratch;
    isRejected.append(jit.branchTest16(CCallHelpers::Zero, Address(record, RuntimeType::offsetOfNumberOfCellTests())));
    jit.addPtr(TrustedImm32(RuntimeType::offsetOfCellTests()), cellTest);

    Label hasCellTest = jit.label();
    jit.load8(Address(cellTest, RuntimeCellTest::offsetOfKind()), testKind);
    for (CellTestKind kind : cellTestKindsInTestOrder)
        emitCellTest(kind);
    jit.breakpoint();

    testFailed.link(&jit);
    jit.addPtr(TrustedImm32(sizeof(RuntimeCellTest)), cellTest);
    loadTypeRecord(end);
    jit.load16(Address(end, RuntimeType::offsetOfNumberOfCellTests()), numberOfCellTests);
    jit.getEffectiveAddress(BaseIndex(end, numberOfCellTests, CCallHelpers::TimesEight, static_cast<int32_t>(RuntimeType::offsetOfCellTests())), end);
    jit.branchPtr(CCallHelpers::Below, cellTest, end).linkTo(hasCellTest, &jit);
    isRejected.append(jit.jump());

    isAccepted.link(&jit);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
