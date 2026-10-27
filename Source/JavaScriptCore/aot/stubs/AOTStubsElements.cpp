/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsElements.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTInstance.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsArrays.h"
#include "AOTStubsKeyed.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTStubsValues.h"
#include "ArrayStorage.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSArrayBufferView.h"
#include "JSCell.h"
#include "JSGlobalObject.h"
#include "JSObject.h"
#include "JSString.h"
#include "MarkedBlock.h"
#include "NumericStrings.h"
#include "SmallStrings.h"
#include "SparseArrayValueMap.h"
#include "Structure.h"
#include <initializer_list>
#include <optional>

namespace JSC { namespace AOT {

struct ElementRegisters {
    GPRReg base;
    GPRReg key;
    GPRReg storage;
    GPRReg index;
    GPRReg type;
    FPRReg number;
};

StubSignature signatureOfGetByVal(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A1) }); }
StubSignature signatureOfGetByValAtIndex(StubKey key) { return signatureOfGetByVal(key); }
StubSignature signatureOfGetByValExpecting(StubKey key) { return signatureOfGetByVal(key); }
StubSignature signatureOfGetByValAtIndexExpecting(StubKey key) { return signatureOfGetByVal(key); }

StubSignature signatureOfPutByVal(StubKey) { return StubSignature::withoutResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }
StubSignature signatureOfPutByValAtIndex(StubKey key) { return signatureOfPutByVal(key); }
StubSignature signatureOfPutByValExpecting(StubKey key) { return signatureOfPutByVal(key); }
StubSignature signatureOfPutByValAtIndexExpecting(StubKey key) { return signatureOfPutByVal(key); }

static ElementRegisters allocateElementRegisters(StubAssembler& jit)
{
    ElementRegisters registers;
    registers.base = jit.clobberOperand(0);
    registers.key = jit.clobberOperand(1);
    registers.storage = jit.allocateScratch(StubRegister::T11);
    registers.index = jit.allocateScratch(StubRegister::T12);
    registers.type = jit.allocateScratch(StubRegister::T13);
    registers.number = jit.allocateScratch(FPRInfo::fpRegT0);
    return registers;
}

static void checkTypedArrayAccess(StubAssembler& jit, const ElementRegisters& registers, JumpList& slow, JumpList* isOutside = nullptr)
{
    jit.load8(Address(registers.base, JSCell::typeInfoTypeOffset()), registers.type);
    jit.sub32(registers.type, TrustedImm32(FirstTypedArrayType), registers.storage);
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, registers.storage, TrustedImm32(NumberOfTypedArrayTypesExcludingDataView)));
    slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(registers.base, JSArrayBufferView::offsetOfMode()), TrustedImm32(isResizableOrGrowableSharedMode)));
    // A length may exceed 31 bits, so a negative key must not look like an index.
    jit.signExtend32ToPtr(registers.key, registers.index);
    jit.loadPtr(Address(registers.base, JSArrayBufferView::offsetOfLength()), registers.storage);
    (isOutside ? *isOutside : slow).append(jit.branchPtr(CCallHelpers::AboveOrEqual, registers.index, registers.storage));
    jit.loadPtr(Address(registers.base, JSArrayBufferView::offsetOfVector()), registers.storage);
}

static void returnBoxedInt32(StubAssembler& jit, const ElementRegisters& registers)
{
    boxInt32(jit, registers.storage, jit.result());
    jit.returnToCaller();
}

static void returnBoxedDouble(StubAssembler& jit, const ElementRegisters& registers, PureDouble number, FPRReg numberScratch)
{
    JumpList notInt32;
    branchIfNotExactInt32(jit, number.fpr(), registers.storage, notInt32, numberScratch);
    returnBoxedInt32(jit, registers);
    notInt32.link(&jit);
    boxDouble(jit, number, jit.result());
    jit.returnToCaller();
}

static void returnValueUnlessHole(StubAssembler& jit, const ElementRegisters& registers, BaseIndex element, JumpList& isHole)
{
    jit.load64(element, registers.storage);
    isHole.append(jit.branchTest64(CCallHelpers::Zero, registers.storage));
    jit.move(registers.storage, jit.result());
    jit.returnToCaller();
}

// The elements of a compiled object are those of its extra properties.
static void loadExtraPropertiesOfCompiledObject(StubAssembler& jit, const ElementRegisters& registers, JumpList& hasNone)
{
    const auto [base, key, extraProperties, index, hiddenWordOffset, number] = registers;

    hasNone.append(jit.branchTest32(CCallHelpers::Zero, Address(base, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)));
    loadStructure(jit, base, extraProperties);
    jit.load32(Address(extraProperties, CompiledStructure::offsetOfExtraPropertiesSlot()), hiddenWordOffset);
    hasNone.append(jit.branchTest32(CCallHelpers::Zero, hiddenWordOffset, TrustedImm32(SlotData::maxOffset)));
    jit.and32(TrustedImm32(SlotData::maxOffset), hiddenWordOffset);
    loadExtraProperties(jit, base, hiddenWordOffset, extraProperties, hasNone);
}

static void emitGetByValAtIndexEntry(StubAssembler& jit, const ElementRegisters& registers)
{
    const auto [base, index, storage, length, shape, number] = registers;
    const BaseIndex element(storage, index, CCallHelpers::TimesEight);
    JumpList notHandled;

    jit.bindEntry(getByVal(true));
    notHandled.append(jit.branchIfNotCell(base));
    jit.load8(Address(base, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    Jump isNotContiguous = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(ContiguousShape));
    Label hasContiguousShape = jit.label();
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.load32(Address(storage, Butterfly::offsetOfPublicLength()), length);
    notHandled.append(jit.branch64(CCallHelpers::AboveOrEqual, index, length));
    returnValueUnlessHole(jit, registers, element, notHandled);

    isNotContiguous.link(&jit);
    Jump hasIndexedStorage = jit.branchTest32(CCallHelpers::NonZero, shape);
    Jump isNotString = jit.branchIfNotString(base);
    jit.loadPtr(Address(base, JSString::offsetOfValue()), storage);
    notHandled.append(jit.branchIfRopeStringImpl(storage));
    jit.load32(Address(storage, StringImpl::lengthMemoryOffset()), length);
    notHandled.append(jit.branch64(CCallHelpers::AboveOrEqual, index, length));
    notHandled.append(jit.branchTest32(CCallHelpers::Zero, Address(storage, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(storage, StringImpl::dataOffset()), storage);
    jit.load8(BaseIndex(storage, index, CCallHelpers::TimesOne), storage);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(singleCharacterStrings)), length);
    jit.loadPtr(BaseIndex(length, storage, CCallHelpers::TimesEight), jit.result());
    jit.returnToCaller();

    // A table that a literal with numbers for keys has made.
    isNotString.link(&jit);
    notHandled.append(jit.branchIfNotType(base, CompiledObjectType));
    loadExtraPropertiesOfCompiledObject(jit, registers, notHandled);
    jit.load8(Address(storage, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    notHandled.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(ContiguousShape)));
    jit.loadPtr(Address(storage, JSObject::butterflyOffset()), storage);
    jit.load32(Address(storage, Butterfly::offsetOfPublicLength()), length);
    notHandled.append(jit.branch64(CCallHelpers::AboveOrEqual, index, length));
    returnValueUnlessHole(jit, registers, element, notHandled);

    hasIndexedStorage.link(&jit);
    jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(Int32Shape)).linkTo(hasContiguousShape, &jit);
    notHandled.link(&jit);
    jit.boxInt52(index, index, storage, number);
}

static void loadArrayStorageElementAndReturn(StubAssembler& jit, const ElementRegisters& registers, GPRReg probe, JumpList& isAbsent, JumpList& slow)
{
    const auto [base, key, storage, index, shape, number] = registers;
    const GPRReg owner = shape;
    const GPRReg entry = base;
    JumpList isOutsideVector;
    JumpList isAbsentInMap;

    static_assert(ArrayStorageShape > DoubleShape && ArrayStorageShape > ContiguousShape && SlowPutArrayStorageShape > ArrayStorageShape && SlowPutArrayStorageShape == IndexingShapeMask - 2);
    slow.append(jit.branch32(CCallHelpers::Below, shape, TrustedImm32(ArrayStorageShape)));
    slow.append(jit.branch32(CCallHelpers::Above, shape, TrustedImm32(SlowPutArrayStorageShape)));
    isOutsideVector.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, ArrayStorage::vectorLengthOffset())));
    returnValueUnlessHole(jit, registers, BaseIndex(storage, index, CCallHelpers::TimesEight, ArrayStorage::vectorOffset()), isOutsideVector);

    isOutsideVector.link(&jit);
    slow.append(jit.branch32(CCallHelpers::LessThan, key, TrustedImm32(0)));
    jit.load8(Address(base, JSCell::typeInfoTypeOffset()), storage);
    Jump isArray = jit.branch32(CCallHelpers::Equal, storage, TrustedImm32(ArrayType));
    slow.append(jit.branch32(CCallHelpers::NotEqual, storage, TrustedImm32(FinalObjectType)));
    isArray.link(&jit);
    jit.move(base, owner);
    emitFindSparseElement(jit, { owner, index, entry, probe, storage }, isAbsentInMap);
    Jump isAccessor = jit.branchTest32(CCallHelpers::NonZero, Address(entry, SparseArrayEntry::offsetOfAttributes()), TrustedImm32(static_cast<int32_t>(PropertyAttribute::Accessor)));
    jit.load64(Address(entry, SparseArrayEntry::offsetOfValue()), jit.result());
    jit.returnToCaller();

    isAccessor.link(&jit);
    jit.move(owner, base);
    slow.append(jit.jump());

    isAbsentInMap.link(&jit);
    jit.move(owner, base);
    isAbsent.append(jit.jump());
}

static Label emitAbsentElement(StubAssembler& jit, const ElementRegisters& registers, JumpList& slow)
{
    const auto [base, key, structure, index, type, number] = registers;

    slow.append(jit.branchIfNotType(base, ArrayType));
    loadStructure(jit, base, structure);
    slow.append(jit.branchTest32(CCallHelpers::Zero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_hasPristineElementsBits)));
    Jump isName = jit.branch32(CCallHelpers::LessThan, key, TrustedImm32(0));
    Label isAbsent = jit.label();
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();

    isName.link(&jit);
    static_assert(sizeof(Structure::SeenProperties) == sizeof(uint64_t));
    slow.append(jit.branchTest64(CCallHelpers::NonZero, Address(structure, Structure::seenPropertiesOffset()), TrustedImm32(-1)));
    slow.append(branchIfNotOfThisRealm(jit, structure));
    jit.jump().linkTo(isAbsent, &jit);
    return isAbsent;
}

static void loadStringCharacterAndReturn(StubAssembler& jit, const ElementRegisters& registers, JumpList& slow, Label isAbsent)
{
    const auto [base, key, storage, index, flags, number] = registers;

    jit.loadPtr(Address(base, JSString::offsetOfValue()), storage);
    slow.append(jit.branchIfRopeStringImpl(storage));
    jit.zeroExtend32ToWord(key, index);
    jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, StringImpl::lengthMemoryOffset())).linkTo(isAbsent, &jit);
    jit.load32(Address(storage, StringImpl::flagsOffset()), flags);
    jit.loadPtr(Address(storage, StringImpl::dataOffset()), storage);
    Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(StringImpl::flagIs8Bit()));
    jit.load8(BaseIndex(storage, index, CCallHelpers::TimesOne), storage);
    Label characterReady = jit.label();
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(singleCharacterStrings)), index);
    jit.loadPtr(BaseIndex(index, storage, CCallHelpers::TimesEight), jit.result());
    jit.returnToCaller();

    is16Bit.link(&jit);
    jit.load16(BaseIndex(storage, index, CCallHelpers::TimesTwo), storage);
    jit.branch32(CCallHelpers::BelowOrEqual, storage, TrustedImm32(maxSingleCharacterString)).linkTo(characterReady, &jit);
    slow.append(jit.jump());
}

static void loadTypedArrayElementAndReturn(StubAssembler& jit, const ElementRegisters& registers, FPRReg numberScratch, JumpList& slow, Label isAbsent)
{
    const auto [base, key, storage, index, type, number] = registers;
    const BaseIndex in8Bits(storage, index, CCallHelpers::TimesOne);
    const BaseIndex in16Bits(storage, index, CCallHelpers::TimesTwo);
    const BaseIndex in32Bits(storage, index, CCallHelpers::TimesFour);
    const BaseIndex in64Bits(storage, index, CCallHelpers::TimesEight);
    JumpList isOutsideTypedArray;

    checkTypedArrayAccess(jit, registers, slow, &isOutsideTypedArray);
    isOutsideTypedArray.linkTo(isAbsent, &jit);
    auto returnInt32Of = [&](JSType viewType, const auto& load) {
        Jump other = jit.branch32(CCallHelpers::NotEqual, type, TrustedImm32(viewType));
        load();
        returnBoxedInt32(jit, registers);
        other.link(&jit);
    };
    auto returnDoubleOf = [&](JSType viewType, const auto& load) {
        Jump other = jit.branch32(CCallHelpers::NotEqual, type, TrustedImm32(viewType));
        load();
        returnBoxedDouble(jit, registers, PureDouble::purify(jit, number, number), numberScratch);
        other.link(&jit);
    };
    returnInt32Of(Uint8ArrayType, [&] { jit.load8(in8Bits, storage); });
    returnInt32Of(Int32ArrayType, [&] { jit.load32(in32Bits, storage); });
    returnDoubleOf(Float64ArrayType, [&] { jit.loadDouble(in64Bits, number); });
    returnInt32Of(Uint8ClampedArrayType, [&] { jit.load8(in8Bits, storage); });
    returnInt32Of(Uint16ArrayType, [&] { jit.load16(in16Bits, storage); });
    returnInt32Of(Int16ArrayType, [&] { jit.load16SignedExtendTo32(in16Bits, storage); });
    returnInt32Of(Int8ArrayType, [&] { jit.load8SignedExtendTo32(in8Bits, storage); });
    returnInt32Of(Uint32ArrayType, [&] {
        jit.load32(in32Bits, storage);
        Jump isInt32 = jit.branch32(CCallHelpers::GreaterThanOrEqual, storage, TrustedImm32(0));
        returnBoxedDouble(jit, registers, PureDouble::convertInt64(jit, storage, number), numberScratch);
        isInt32.link(&jit);
    });
    returnDoubleOf(Float32ArrayType, [&] {
        jit.loadFloat(in32Bits, number);
        jit.convertFloatToDouble(number, number);
    });
    slow.append(jit.jump());
}

static void loadCompiledObjectElementAndReturn(StubAssembler& jit, const ElementRegisters& registers, FPRReg numberScratch, JumpList& slow)
{
    const auto [base, key, storage, index, shape, number] = registers;
    const BaseIndex element(storage, index, CCallHelpers::TimesEight);
    JumpList isAbsent;

    loadExtraPropertiesOfCompiledObject(jit, registers, slow);
    jit.load8(Address(storage, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    jit.loadPtr(Address(storage, JSObject::butterflyOffset()), storage);
    Jump isDouble = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
    slow.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(Int32Shape)));
    isContiguous.link(&jit);
    isAbsent.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    returnValueUnlessHole(jit, registers, element, isAbsent);

    isDouble.link(&jit);
    isAbsent.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    jit.loadDouble(element, number);
    returnBoxedDouble(jit, registers, PureDouble::branchIfNaN(jit, number, isAbsent), numberScratch);

    // Right above the object is the Object.prototype of its realm, which has no elements and takes none. A negative key is a name.
    isAbsent.link(&jit);
    slow.append(jit.branch32(CCallHelpers::LessThan, key, TrustedImm32(0)));
    loadStructure(jit, base, storage);
    jit.load64(Address(storage, Structure::prototypeOffset()), storage);
    slow.append(jit.branch64(CCallHelpers::NotEqual, storage, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.objectPrototype))));
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();
}

void generateGetByVal(StubAssembler& jit)
{
    const ElementRegisters registers = allocateElementRegisters(jit);
    const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT1);
    const GPRReg probe = jit.allocateScratch(StubRegister::A2);
    const auto [base, key, storage, index, shape, number] = registers;
    const BaseIndex element(storage, index, CCallHelpers::TimesEight);
    JumpList slow;
    JumpList absent;

    emitGetByValAtIndexEntry(jit, registers);
    jit.bindEntry(jit.key());
    Jump notInt32 = jit.branch64(CCallHelpers::Below, key, numberTag);
    Label indexReady = jit.label();
    slow.append(jit.branchIfNotCell(base));
    jit.load8(Address(base, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    static_assert(JSObject::butterflyOffset() + sizeof(void*) <= MarkedBlock::atomSize);
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.zeroExtend32ToWord(key, index);
    Jump isNotContiguous = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(ContiguousShape));
    Label hasContiguousShape = jit.label();
    absent.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    returnValueUnlessHole(jit, registers, element, absent);

    isNotContiguous.link(&jit);
    Jump noButterfly = jit.branchTest32(CCallHelpers::Zero, shape);
    jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(Int32Shape)).linkTo(hasContiguousShape, &jit);
    absent.append(jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(UndecidedShape)));
    Jump isArrayStorage = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(DoubleShape));
    absent.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    jit.loadDouble(element, number);
    returnBoxedDouble(jit, registers, PureDouble::branchIfNaN(jit, number, absent), numberScratch);

    isArrayStorage.link(&jit);
    loadArrayStorageElementAndReturn(jit, registers, probe, absent, slow);

    absent.link(&jit);
    Label isAbsent = emitAbsentElement(jit, registers, slow);

    noButterfly.link(&jit);
    Jump notString = jit.branchIfNotString(base);
    loadStringCharacterAndReturn(jit, registers, slow, isAbsent);

    notString.link(&jit);
    Jump isNotCompiledObject = jit.branchIfNotType(base, CompiledObjectType);
    loadCompiledObjectElementAndReturn(jit, registers, numberScratch, slow);

    isNotCompiledObject.link(&jit);
    loadTypedArrayElementAndReturn(jit, registers, numberScratch, slow, isAbsent);

    notInt32.link(&jit);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, key, numberTag);
    unboxDouble(jit, key, storage, number);
    jit.branchConvertDoubleToInt32(number, storage, slow, numberScratch, false);
    boxInt32(jit, storage, key);
    jit.jump().linkTo(indexReady, &jit);

    notNumber.link(&jit);
    slow.append(jit.branchIfNotCell(base));
    slow.append(jit.branchIfNotCell(key));
    checkStubOperands(getByValCachedCells(), { base, key });
    jit.jumpToStub(getByValCachedCells());

    slow.link(&jit);
    if (jit.options().callsTrapsOfProxies) {
        // A trap is handed an index as its text. The texts of the small ones are kept once they have been asked for.
        static_assert(sizeof(NumericStrings::StringWithJSString) == 2 * sizeof(void*));
        const StubSignature throughProxySignature = signatureOf(throughProxy(TrapOfProxy::Get));
        const GPRReg text = registers.type;
        const GPRReg noHeader = index;
        RELEASE_ASSERT(throughProxySignature.operands[0] == Reg(base) && throughProxySignature.operands[1] == Reg(text) && throughProxySignature.operands[2] == Reg(noHeader));
        JumpList operationReads;
        operationReads.append(jit.branchIfNotCell(base));
        operationReads.append(jit.branchIfNotType(base, ProxyObjectType));
        operationReads.append(jit.branch64(CCallHelpers::Below, key, numberTag));
        jit.zeroExtend32ToWord(key, index);
        operationReads.append(jit.branch32(CCallHelpers::AboveOrEqual, index, TrustedImm32(NumericStrings::cacheSize)));
        jit.lshift64(TrustedImm32(1), index);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.smallIntStrings)), storage);
        jit.loadPtr(BaseIndex(storage, index, CCallHelpers::TimesEight, NumericStrings::StringWithJSString::offsetOfJSString()), text);
        operationReads.append(jit.branchTestPtr(CCallHelpers::Zero, text));
        jit.move(TrustedImm32(0), noHeader);
        jit.jumpToStub(throughProxy(TrapOfProxy::Get));
        operationReads.link(&jit);
    }
    callBinaryOperation(jit, Entry::operationAOTGetByVal);
}

static ECMAMode ecmaModeOf(ElementStoreMode mode)
{
    switch (mode) {
    case ElementStoreMode::Strict:
        return ECMAMode::strict();
    case ElementStoreMode::Sloppy:
        return ECMAMode::sloppy();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitPutByValAtIndexEntry(StubAssembler& jit, const ElementRegisters& registers, GPRReg value, ECMAMode mode)
{
    const auto [base, index, storage, length, type, number] = registers;
    JumpList notHandled;

    jit.bindEntry(putByVal(true, mode));
    notHandled.append(jit.branchIfNotCell(base));
    loadPristineArrayKind(jit, base, storage, IndexingShapeMask | CopyOnWrite | MayHaveIndexedAccessors);
    notHandled.append(jit.branch32(CCallHelpers::NotEqual, storage, TrustedImm32(ContiguousShape)));
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.load32(Address(storage, Butterfly::offsetOfPublicLength()), length);
    notHandled.append(jit.branch64(CCallHelpers::AboveOrEqual, index, length));
    jit.store64(value, BaseIndex(storage, index, CCallHelpers::TimesEight));
    returnAfterStoring(jit, base, value);

    notHandled.link(&jit);
    jit.boxInt52(index, index, storage, number);
}

static void storeTypedArrayElementAndReturn(StubAssembler& jit, const ElementRegisters& registers, GPRReg value, GPRReg doubleBits, JumpList& slow)
{
    const auto [base, key, storage, index, type, number] = registers;

    checkTypedArrayAccess(jit, registers, slow);
    Jump valueIsNotInt32 = jit.branch64(CCallHelpers::Below, value, numberTag);
    auto forJSType = [&](JSType viewType, const auto& store) {
        Jump other = jit.branch32(CCallHelpers::NotEqual, type, TrustedImm32(viewType));
        store();
        jit.returnToCaller();
        other.link(&jit);
    };
    auto store32 = [&] { jit.store32(value, BaseIndex(storage, index, CCallHelpers::TimesFour)); };
    auto store16 = [&] { jit.store16(value, BaseIndex(storage, index, CCallHelpers::TimesTwo)); };
    auto store8 = [&] { jit.store8(value, BaseIndex(storage, index, CCallHelpers::TimesOne)); };
    auto storeDouble = [&] { jit.storeDouble(number, BaseIndex(storage, index, CCallHelpers::TimesEight)); };
    auto storeFloat = [&] {
        jit.convertDoubleToFloat(number, number);
        jit.storeFloat(number, BaseIndex(storage, index, CCallHelpers::TimesFour));
    };
    forJSType(Int32ArrayType, store32);
    forJSType(Uint8ArrayType, store8);
    forJSType(Uint32ArrayType, store32);
    forJSType(Uint16ArrayType, store16);
    forJSType(Int16ArrayType, store16);
    forJSType(Int8ArrayType, store8);
    jit.convertInt32ToDouble(value, number);
    forJSType(Float64ArrayType, storeDouble);
    forJSType(Float32ArrayType, storeFloat);
    slow.append(jit.jump());

    valueIsNotInt32.link(&jit);
    slow.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
    unboxDouble(jit, value, doubleBits, number);
    forJSType(Float64ArrayType, storeDouble);
    forJSType(Float32ArrayType, storeFloat);
    slow.append(jit.jump());
}

// A store at the public length appends. Where the index is not below the vector length, the vector is then full and grows first.
template<typename Functor>
static void storeOrAppendAndReturn(StubAssembler& jit, const ElementRegisters& registers, GPRReg allocator, GPRReg scratch, JumpList& slow, const Functor& storeAndReturn)
{
    const auto [base, key, storage, index, newLength, number] = registers;

    Label loadStorage = jit.label();
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.zeroExtend32ToWord(key, index);
    Jump isWithinLength = jit.branch32(CCallHelpers::Below, index, Address(storage, Butterfly::offsetOfPublicLength()));
    Jump isBeyondVector = jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfVectorLength()));
    jit.add32(TrustedImm32(1), index, newLength);
    jit.store32(newLength, Address(storage, Butterfly::offsetOfPublicLength()));
    isWithinLength.link(&jit);
    storeAndReturn(BaseIndex(storage, index, CCallHelpers::TimesEight));

    isBeyondVector.link(&jit);
    slow.append(jit.branch32(CCallHelpers::NotEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    growFullVector(jit, { base, storage, index, newLength, allocator, scratch }, slow);
    jit.jump().linkTo(loadStorage, &jit);
}

static void storeDoubleElementAndReturn(StubAssembler& jit, const ElementRegisters& registers, GPRReg value, GPRReg allocator, GPRReg doubleBits, JumpList& slow)
{
    const FPRReg number = registers.number;

    Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, value, numberTag);
    slow.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
    unboxDouble(jit, value, doubleBits, number);
    slow.append(jit.branchIfNaN(number));
    Jump isReady = jit.jump();
    isInt32.link(&jit);
    jit.convertInt32ToDouble(value, number);
    isReady.link(&jit);
    storeOrAppendAndReturn(jit, registers, allocator, doubleBits, slow, [&](BaseIndex element) {
        jit.storeDouble(number, element);
        jit.returnToCaller();
    });
}

// Only an element that exists is replaced here: creating one asks the prototype chain and tells whoever depends on the object's own names.
static void replaceCompiledObjectElementAndReturn(StubAssembler& jit, const ElementRegisters& registers, GPRReg value, JumpList& slow)
{
    const auto [base, key, extraProperties, index, shape, number] = registers;
    const GPRReg butterfly = shape;

    loadExtraPropertiesOfCompiledObject(jit, registers, slow);
    jit.load8(Address(extraProperties, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), shape);
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
    slow.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(Int32Shape)));
    slow.append(jit.branch64(CCallHelpers::Below, value, numberTag));
    isContiguous.link(&jit);
    jit.loadPtr(Address(extraProperties, JSObject::butterflyOffset()), butterfly);
    jit.zeroExtend32ToWord(key, index);
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(butterfly, Butterfly::offsetOfPublicLength())));
    slow.append(jit.branchTest64(CCallHelpers::Zero, BaseIndex(butterfly, index, CCallHelpers::TimesEight)));
    jit.store64(value, BaseIndex(butterfly, index, CCallHelpers::TimesEight));
    jit.move(extraProperties, base);
    returnAfterStoring(jit, base, value);
}

void generatePutByVal(StubAssembler& jit)
{
    const ElementRegisters registers = allocateElementRegisters(jit);
    const GPRReg value = jit.operand(2);
    const GPRReg valueArgument = jit.allocateScratch(StubRegister::A3);
    const GPRReg doubleBits = jit.allocateScratch(StubRegister::A4);
    const GPRReg isStrictArgument = doubleBits;
    const ECMAMode mode = ecmaModeOf(static_cast<ElementStoreMode>(jit.key().form()));
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    const GPRReg shape = registers.storage;
    JumpList slow;

    emitPutByValAtIndexEntry(jit, registers, value, mode);
    jit.bindEntry(jit.key());
    slow.append(jit.branch64(CCallHelpers::Below, key, numberTag));
    slow.append(jit.branchIfNotCell(base));
    jit.load8(Address(base, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), shape);
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
    Jump isNotInt32Shape = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(Int32Shape));
    slow.append(jit.branch64(CCallHelpers::Below, value, numberTag));
    Jump staysInt32Shape = jit.jump();
    isNotInt32Shape.link(&jit);
    Jump isDoubleShape = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
    slow.append(jit.branchTest32(CCallHelpers::NonZero, shape));
    Jump isNotCompiledObject = jit.branchIfNotType(base, CompiledObjectType);
    replaceCompiledObjectElementAndReturn(jit, registers, value, slow);

    isNotCompiledObject.link(&jit);
    storeTypedArrayElementAndReturn(jit, registers, value, doubleBits, slow);

    isDoubleShape.link(&jit);
    storeDoubleElementAndReturn(jit, registers, value, valueArgument, doubleBits, slow);

    isContiguous.link(&jit);
    staysInt32Shape.link(&jit);
    storeOrAppendAndReturn(jit, registers, valueArgument, doubleBits, slow, [&](BaseIndex element) {
        jit.store64(value, element);
        returnAfterStoring(jit, base, value);
    });

    slow.link(&jit);
    jit.move(value, valueArgument);
    jit.move(TrustedImm32(mode.isStrict() ? 1 : 0), isStrictArgument);
    jit.callOperationAndReturn(Entry::operationAOTPutByVal, {
        .returns = OperationCall::Returns::Void,
        .secondArgumentIn = base,
        .thirdArgumentIn = key,
    });
}

static JSType typeOfExpectedView(ExpectedElements expected)
{
    switch (expected) {
    case ExpectedElements::Int32Array:
        return Int32ArrayType;
    case ExpectedElements::Uint8Array:
        return Uint8ArrayType;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Leaves the operands as they came, so that a miss goes on to the stub that takes everything. The index is a whole register: a negative one lies above every length.
// With a value, the element is replaced: only one that is there, and only by an int32.
static void accessExpectedElementAndReturn(StubAssembler& jit, const ElementRegisters& registers, ExpectedElements expected, GPRReg index, std::optional<GPRReg> value, JumpList& miss)
{
    const GPRReg base = registers.base;
    const GPRReg storage = registers.storage;

    miss.append(jit.branchIfNotCell(base));
    miss.append(jit.branch8(CCallHelpers::NotEqual, Address(base, JSCell::typeInfoTypeOffset()), TrustedImm32(typeOfExpectedView(expected))));
    miss.append(jit.branchTest8(CCallHelpers::NonZero, Address(base, JSArrayBufferView::offsetOfMode()), TrustedImm32(isResizableOrGrowableSharedMode)));
    miss.append(jit.branchPtr(CCallHelpers::AboveOrEqual, index, Address(base, JSArrayBufferView::offsetOfLength())));
    jit.loadPtr(Address(base, JSArrayBufferView::offsetOfVector()), storage);
    if (!value) {
        switch (expected) {
        case ExpectedElements::Int32Array:
            jit.load32(BaseIndex(storage, index, CCallHelpers::TimesFour), storage);
            break;
        case ExpectedElements::Uint8Array:
            jit.load8(BaseIndex(storage, index, CCallHelpers::TimesOne), storage);
            break;
        }
        returnBoxedInt32(jit, registers);
        return;
    }
    miss.append(jit.branch64(CCallHelpers::Below, *value, numberTag));
    switch (expected) {
    case ExpectedElements::Int32Array:
        jit.store32(*value, BaseIndex(storage, index, CCallHelpers::TimesFour));
        break;
    case ExpectedElements::Uint8Array:
        jit.store8(*value, BaseIndex(storage, index, CCallHelpers::TimesOne));
        break;
    }
    jit.returnToCaller();
}

static void emitEntriesExpecting(StubAssembler& jit, ExpectedElements expected, std::optional<GPRReg> value, StubKey atIndex, StubKey takesEverythingAtIndex, StubKey takesEverything)
{
    const ElementRegisters registers = allocateElementRegisters(jit);
    RELEASE_ASSERT(signatureOf(takesEverything).operands == jit.signature().operands && signatureOf(takesEverythingAtIndex).operands == jit.signature().operands);

    JumpList missAtIndex;
    jit.bindEntry(atIndex);
    accessExpectedElementAndReturn(jit, registers, expected, registers.key, value, missAtIndex);
    jit.linkToStub(missAtIndex, takesEverythingAtIndex);

    JumpList miss;
    jit.bindEntry(jit.key());
    miss.append(jit.branch64(CCallHelpers::Below, registers.key, numberTag));
    jit.signExtend32ToPtr(registers.key, registers.index);
    accessExpectedElementAndReturn(jit, registers, expected, registers.index, value, miss);
    jit.linkToStub(miss, takesEverything);
}

void generateGetByValExpecting(StubAssembler& jit)
{
    const ExpectedElements expected = static_cast<ExpectedElements>(jit.key().form());
    emitEntriesExpecting(jit, expected, std::nullopt, getByValExpecting(true, expected), getByVal(true), getByVal(false));
}

void generatePutByValExpecting(StubAssembler& jit)
{
    const ECMAMode mode = jit.key().form() < numberOfExpectedElements ? ECMAMode::strict() : ECMAMode::sloppy();
    const ExpectedElements expected = static_cast<ExpectedElements>(jit.key().form() % numberOfExpectedElements);
    emitEntriesExpecting(jit, expected, jit.operand(2), putByValExpecting(true, mode, expected), putByVal(true, mode), putByVal(false, mode));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
