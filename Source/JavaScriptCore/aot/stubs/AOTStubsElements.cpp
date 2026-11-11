/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsElements.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsArrays.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTStubsPropertyProbes.h"
#include "AOTStubsStrings.h"
#include "AOTStubsValues.h"
#include "Allocator.h"
#include "ArrayConventions.h"
#include "ArrayStorage.h"
#include "Butterfly.h"
#include "CompleteSubspace.h"
#include "DirectArguments.h"
#include "IndexingType.h"
#include "JSArrayBufferView.h"
#include "JSCell.h"
#include "JSGlobalObject.h"
#include "JSObject.h"
#include "JSString.h"
#include "LocalAllocator.h"
#include "MarkedBlock.h"
#include "MarkedSpace.h"
#include "NumericStrings.h"
#include "PureNaN.h"
#include "SmallStrings.h"
#include "SparseArrayValueMap.h"
#include "Structure.h"
#include <bit>
#include <initializer_list>
#include <optional>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

StubSignature signatureOfMoveElementsDown(StubKey)
{
    return StubSignature::withoutResult({ gprOf(StubRegister::A0), gprOf(StubRegister::A2) });
}

void generateMoveElementsDown(StubAssembler& jit)
{
    const GPRReg butterfly = jit.operand(0);
    const GPRReg count = jit.operand(1);
    const GPRReg second = jit.reuseScratch(StubRegister::A1);
    RELEASE_ASSERT(butterfly == leafArgumentGPR(0) && second == leafArgumentGPR(1) && count == leafArgumentGPR(2));
    jit.move(butterfly, second);
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), second);
    jit.lshift64(TrustedImm32(CCallHelpers::TimesEight), count);
    jit.callLeaf(Entry::operationAOTMoveBytes);
    jit.returnToCaller();
}

StubSignature signatureOfMoveFewBytes(StubKey)
{
    return StubSignature::withoutResult({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

void generateMoveFewBytes(StubAssembler& jit)
{
    static_assert(maxBytesMovedByStub == 2 * sizeof(uint64_t));
    const GPRReg target = jit.operand(0);
    const GPRReg source = jit.operand(1);
    const GPRReg count = jit.operand(2);
    const GPRReg first = jit.allocateScratch(StubRegister::T9);
    const GPRReg last = jit.allocateScratch(StubRegister::T10);
    auto endOf = [&](GPRReg range, int32_t size) {
        return BaseIndex(range, count, CCallHelpers::TimesOne, -size);
    };

    Jump isUnderEight = jit.branch64(CCallHelpers::Below, count, TrustedImm32(8));
    jit.load64(Address(source), first);
    jit.load64(endOf(source, 8), last);
    jit.store64(first, Address(target));
    jit.store64(last, endOf(target, 8));
    jit.returnToCaller();

    isUnderEight.link(&jit);
    Jump isUnderFour = jit.branch64(CCallHelpers::Below, count, TrustedImm32(4));
    jit.load32(Address(source), first);
    jit.load32(endOf(source, 4), last);
    jit.store32(first, Address(target));
    jit.store32(last, endOf(target, 4));
    jit.returnToCaller();

    isUnderFour.link(&jit);
    Jump isOne = jit.branch64(CCallHelpers::Below, count, TrustedImm32(2));
    jit.load16(Address(source), first);
    jit.load16(endOf(source, 2), last);
    jit.store16(first, Address(target));
    jit.store16(last, endOf(target, 2));
    jit.returnToCaller();

    isOne.link(&jit);
    jit.load8(Address(source), first);
    jit.store8(first, Address(target));
    jit.returnToCaller();
}

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
StubSignature signatureOfGetByValKeepingRegisters(StubKey key) { return signatureOfGetByVal(key); }
StubSignature signatureOfGetByValAtConstantIndex(StubKey key) { return signatureOfGetByVal(key); }
StubSignature signatureOfGetByValExpecting(StubKey key) { return signatureOfGetByVal(key); }
StubSignature signatureOfGetByValAtIndexExpecting(StubKey key) { return signatureOfGetByVal(key); }

StubSignature signatureOfGetByValAtKnownIndex(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::R0) }); }

StubSignature signatureOfPutByVal(StubKey) { return StubSignature::withoutResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }
StubSignature signatureOfPutByValAtIndex(StubKey key) { return signatureOfPutByVal(key); }
StubSignature signatureOfPutByValAtConstantIndex(StubKey key) { return signatureOfPutByVal(key); }
StubSignature signatureOfPutByValAtKnownIndex(StubKey) { return StubSignature::withoutResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A2) }); }
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

static Jump branchIfNoView(StubAssembler& jit, const ElementRegisters& registers)
{
    jit.load8(Address(registers.base, JSCell::typeInfoTypeOffset()), registers.type);
    jit.sub32(registers.type, TrustedImm32(FirstTypedArrayType), registers.storage);
    return jit.branch32(CCallHelpers::AboveOrEqual, registers.storage, TrustedImm32(NumberOfTypedArrayTypesExcludingDataView));
}

static void checkTypedArrayAccess(StubAssembler& jit, const ElementRegisters& registers, JumpList& slow, JumpList* isOutside = nullptr, JumpList* isNoTypedArray = nullptr)
{
    (isNoTypedArray ? *isNoTypedArray : slow).append(branchIfNoView(jit, registers));
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
    returnBoxedNumber(jit, number, registers.storage, numberScratch, jit.result());
}

static void returnDoubleUnlessHole(StubAssembler& jit, const ElementRegisters& registers, BaseIndex element, FPRReg numberScratch, JumpList& isHole)
{
    returnBoxOfDoubleElement(jit, element, registers.number, registers.storage, registers.type, numberScratch, isHole, jit.result());
}

static void returnValueUnlessHole(StubAssembler& jit, const ElementRegisters& registers, BaseIndex element, JumpList& isHole)
{
    jit.load64(element, registers.storage);
    isHole.append(jit.branchTest64(CCallHelpers::Zero, registers.storage));
    jit.move(registers.storage, jit.result());
    jit.returnToCaller();
}

static void loadExtraPropertiesOfCompiledObject(StubAssembler& jit, const ElementRegisters& registers, JumpList& hasNone, JumpList* noneWereMade = nullptr)
{
    const auto [base, key, extraProperties, index, hiddenWordOffset, number] = registers;

    (noneWereMade ? *noneWereMade : hasNone).append(jit.branchTest32(CCallHelpers::Zero, Address(base, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)));
    loadStructure(jit, base, extraProperties);
    jit.load32(Address(extraProperties, CompiledStructure::offsetOfExtraPropertiesSlot()), hiddenWordOffset);
    hasNone.append(jit.branchTest32(CCallHelpers::Zero, hiddenWordOffset, TrustedImm32(SlotData::maxOffset)));
    jit.and32(TrustedImm32(SlotData::maxOffset), hiddenWordOffset);
    loadExtraProperties(jit, base, hiddenWordOffset, extraProperties, hasNone);
}

static void emitGetByValAtIndexEntry(StubAssembler& jit, const ElementRegisters& registers, FPRReg numberScratch, JumpList& isNeitherStringNorCompiledObject, JumpList& isInsideView)
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

    isNotString.link(&jit);
    Jump isNoView = branchIfNoView(jit, registers);
    notHandled.append(jit.branchTest8(CCallHelpers::NonZero, Address(base, JSArrayBufferView::offsetOfMode()), TrustedImm32(isResizableOrGrowableSharedMode)));
    jit.loadPtr(Address(base, JSArrayBufferView::offsetOfLength()), storage);
    notHandled.append(jit.branchPtr(CCallHelpers::AboveOrEqual, index, storage));
    jit.move(index, registers.index);
    jit.loadPtr(Address(base, JSArrayBufferView::offsetOfVector()), storage);
    isInsideView.append(jit.jump());

    isNoView.link(&jit);
    JumpList isNotCompiledObject = branchIfNotCompiledObject(jit, base);
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
    notHandled.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(DoubleShape)));
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.load32(Address(storage, Butterfly::offsetOfPublicLength()), length);
    notHandled.append(jit.branch64(CCallHelpers::AboveOrEqual, index, length));
    returnDoubleUnlessHole(jit, registers, element, numberScratch, notHandled);

    isNotCompiledObject.link(&jit);
    jit.signExtend32ToPtr(index, storage);
    notHandled.append(jit.branch64(CCallHelpers::NotEqual, index, storage));
    boxInt32(jit, index, index);
    isNeitherStringNorCompiledObject.append(jit.jump());

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

    static_assert(ArrayStorageShape > DoubleShape && ArrayStorageShape > ContiguousShape && SlowPutArrayStorageShape > ArrayStorageShape && SlowPutArrayStorageShape == IndexingShapeMask);
    slow.append(jit.branch32(CCallHelpers::Below, shape, TrustedImm32(ArrayStorageShape)));
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
    branchIfHasNamesOrIsNotOfThisRealm(jit, structure, slow);
    jit.jump().linkTo(isAbsent, &jit);
    return isAbsent;
}

static void loadStringCharacterAndReturn(StubAssembler& jit, const ElementRegisters& registers, JumpList& slow, Label isAbsent)
{
    const auto [base, key, storage, index, flags, number] = registers;

    jit.loadPtr(Address(base, JSString::offsetOfValue()), storage);
    Jump isRope = jit.branchIfRopeStringImpl(storage);
    jit.zeroExtend32ToWord(key, index);
    jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, StringImpl::lengthMemoryOffset())).linkTo(isAbsent, &jit);
    jit.load32(Address(storage, StringImpl::flagsOffset()), flags);
    jit.loadPtr(Address(storage, StringImpl::dataOffset()), storage);
    Label charactersReady = jit.label();
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

    isRope.link(&jit);
    const GPRReg impl = storage;
    const GPRReg characters = index;
    const GPRReg length = flags;
    loadSubstringCharacters(jit, base, impl, characters, length, slow);
    jit.branch32(CCallHelpers::AboveOrEqual, key, length).linkTo(isAbsent, &jit);
    jit.load32(Address(impl, StringImpl::flagsOffset()), flags);
    jit.move(characters, storage);
    jit.zeroExtend32ToWord(key, index);
    jit.jump().linkTo(charactersReady, &jit);
}

static_assert(Int8ArrayType + 1 == Uint8ArrayType && Uint8ArrayType + 1 == Uint8ClampedArrayType && Uint8ClampedArrayType + 1 == Int16ArrayType && Int16ArrayType + 1 == Uint16ArrayType && Uint16ArrayType + 1 == Int32ArrayType && Int32ArrayType + 1 == Uint32ArrayType && Uint32ArrayType < Float32ArrayType && Float32ArrayType + 1 == Float64ArrayType);

static void loadTypedArrayElementAndReturn(StubAssembler& jit, const ElementRegisters& registers, FPRReg numberScratch, JumpList& slow, Label isAbsent, JumpList& isNoView, JumpList& isInsideView)
{
    const auto [base, key, storage, index, type, number] = registers;
    const BaseIndex in8Bits(storage, index, CCallHelpers::TimesOne);
    const BaseIndex in16Bits(storage, index, CCallHelpers::TimesTwo);
    const BaseIndex in32Bits(storage, index, CCallHelpers::TimesFour);
    const BaseIndex in64Bits(storage, index, CCallHelpers::TimesEight);
    JumpList isOutsideTypedArray;

    checkTypedArrayAccess(jit, registers, slow, &isOutsideTypedArray, &isNoView);
    isOutsideTypedArray.linkTo(isAbsent, &jit);
    isInsideView.link(&jit);
    auto returnDouble = [&] {
        returnBoxedDouble(jit, registers, PureDouble::purify(jit, number, number), numberScratch);
    };

    Jump hasNoBytes = jit.branch32(CCallHelpers::Above, type, TrustedImm32(Uint8ClampedArrayType));
    Jump hasSignedBytes = jit.branch32(CCallHelpers::Equal, type, TrustedImm32(Int8ArrayType));
    jit.load8(in8Bits, storage);
    returnBoxedInt32(jit, registers);

    hasSignedBytes.link(&jit);
    jit.load8SignedExtendTo32(in8Bits, storage);
    returnBoxedInt32(jit, registers);

    hasNoBytes.link(&jit);
    Jump hasHalfWords = jit.branch32(CCallHelpers::Below, type, TrustedImm32(Int32ArrayType));
    Jump hasOtherWords = jit.branch32(CCallHelpers::NotEqual, type, TrustedImm32(Int32ArrayType));
    jit.load32(in32Bits, storage);
    returnBoxedInt32(jit, registers);

    hasHalfWords.link(&jit);
    Jump hasSignedHalfWords = jit.branch32(CCallHelpers::Equal, type, TrustedImm32(Int16ArrayType));
    jit.load16(in16Bits, storage);
    returnBoxedInt32(jit, registers);

    hasSignedHalfWords.link(&jit);
    jit.load16SignedExtendTo32(in16Bits, storage);
    returnBoxedInt32(jit, registers);

    hasOtherWords.link(&jit);
    Jump hasNoDoubles = jit.branch32(CCallHelpers::NotEqual, type, TrustedImm32(Float64ArrayType));
    jit.loadDouble(in64Bits, number);
    returnDouble();

    hasNoDoubles.link(&jit);
    Jump hasNoFloats = jit.branch32(CCallHelpers::NotEqual, type, TrustedImm32(Float32ArrayType));
    jit.loadFloat(in32Bits, number);
    jit.convertFloatToDouble(number, number);
    returnDouble();

    hasNoFloats.link(&jit);
    Jump hasUint32s = jit.branch32(CCallHelpers::Equal, type, TrustedImm32(Uint32ArrayType));
    jit.boxInt52(index, key, storage, number);
    slow.append(jit.jump());

    hasUint32s.link(&jit);
    jit.load32(in32Bits, storage);
    Jump isInt32 = jit.branch32(CCallHelpers::GreaterThanOrEqual, storage, TrustedImm32(0));
    returnBoxedDouble(jit, registers, PureDouble::convertInt64(jit, storage, number), numberScratch);
    isInt32.link(&jit);
    returnBoxedInt32(jit, registers);
}

static void answerWhereNoObjectOfChainHasElements(StubAssembler& jit, const ElementRegisters& registers, JumpList& slow, Label isAbsent, JumpList& compiledObjectHasNone)
{
    const auto [base, key, structure, object, type, number] = registers;

    slow.append(jit.branchIfNotObject(base));
    slow.append(jit.branch32(CCallHelpers::LessThan, key, TrustedImm32(0)));
    jit.move(base, object);
    Label climbs = jit.label();
    slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(object, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetPrototype)));
    Jump overridesLookup = jit.branchTest8(CCallHelpers::NonZero, Address(object, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetOwnPropertySlot));
    Label asksStorage = jit.label();
    slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(object, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask | MayHaveIndexedAccessors)));
    Label hasNoElements = jit.label();
    loadStructure(jit, object, structure);
    jit.load64(Address(structure, Structure::prototypeOffset()), object);
    slow.append(jit.branchTest64(CCallHelpers::Zero, object));
    jit.branchIfCell(object).linkTo(climbs, &jit);
    jit.branch64(CCallHelpers::Equal, object, TrustedImm64(JSValue::ValueNull)).linkTo(isAbsent, &jit);
    slow.append(jit.jump());

    compiledObjectHasNone.link(&jit);
    slow.append(jit.branch32(CCallHelpers::LessThan, key, TrustedImm32(0)));
    jit.move(base, object);
    jit.jump().linkTo(hasNoElements, &jit);

    overridesLookup.link(&jit);
    Jump isNoCompiledObject = jit.branchIfNotType(object, CompiledObjectType);
    jit.branchTest32(CCallHelpers::Zero, Address(object, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)).linkTo(hasNoElements, &jit);
    slow.append(jit.jump());

    isNoCompiledObject.link(&jit);
    loadStructure(jit, object, structure);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.infoOfClassPrototypes)), type);
    slow.append(jit.branchPtr(CCallHelpers::NotEqual, Address(structure, Structure::classInfoOffset()), type));
    jit.jump().linkTo(asksStorage, &jit);
}

static void loadCompiledObjectElementAndReturn(StubAssembler& jit, const ElementRegisters& registers, FPRReg numberScratch, GPRReg probe, JumpList& slow, JumpList& isAbsent)
{
    const auto [base, key, storage, index, shape, number] = registers;
    const BaseIndex element(storage, index, CCallHelpers::TimesEight);

    loadExtraPropertiesOfCompiledObject(jit, registers, slow, &isAbsent);
    jit.load8(Address(storage, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    jit.loadPtr(Address(storage, JSObject::butterflyOffset()), storage);
    Jump isDouble = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
    Jump isArrayStorage = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(Int32Shape));
    isContiguous.link(&jit);
    isAbsent.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    returnValueUnlessHole(jit, registers, element, isAbsent);

    isDouble.link(&jit);
    isAbsent.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    returnDoubleUnlessHole(jit, registers, element, numberScratch, isAbsent);

    isArrayStorage.link(&jit);
    static_assert(ArrayStorageShape > DoubleShape && ArrayStorageShape > ContiguousShape && SlowPutArrayStorageShape > ArrayStorageShape);
    slow.append(jit.branch32(CCallHelpers::Below, shape, TrustedImm32(ArrayStorageShape)));
    JumpList isOutsideVector;
    isOutsideVector.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, ArrayStorage::vectorLengthOffset())));
    returnValueUnlessHole(jit, registers, BaseIndex(storage, index, CCallHelpers::TimesEight, ArrayStorage::vectorOffset()), isOutsideVector);

    isOutsideVector.link(&jit);
    slow.append(jit.branch32(CCallHelpers::LessThan, key, TrustedImm32(0)));
    loadExtraPropertiesOfCompiledObject(jit, registers, slow);
    const GPRReg owner = shape;
    const GPRReg entry = base;
    JumpList isAbsentInMap;
    jit.move64ToDouble(base, number);
    jit.move(storage, owner);
    emitFindSparseElement(jit, { owner, index, entry, probe, storage }, isAbsentInMap);
    Jump isAccessor = jit.branchTest32(CCallHelpers::NonZero, Address(entry, SparseArrayEntry::offsetOfAttributes()), TrustedImm32(static_cast<int32_t>(PropertyAttribute::Accessor)));
    jit.load64(Address(entry, SparseArrayEntry::offsetOfValue()), jit.result());
    jit.returnToCaller();

    isAccessor.link(&jit);
    jit.moveDoubleTo64(number, base);
    slow.append(jit.jump());

    isAbsentInMap.link(&jit);
    jit.moveDoubleTo64(number, base);
    isAbsent.append(jit.jump());
}

void loadTextOfIndexForTrap(StubAssembler& jit, GPRReg base, GPRReg key, GPRReg storage, GPRReg index, GPRReg text, JumpList& hasNoText)
{
    hasNoText.append(jit.branchIfNotCell(base));
    hasNoText.append(jit.branchIfNotType(base, ProxyObjectType));
    hasNoText.append(jit.branch64(CCallHelpers::Below, key, numberTag));
    loadKeptTextOfIndex(jit, key, storage, index, text, hasNoText);
}

void loadKeptTextOfIndex(StubAssembler& jit, GPRReg key, GPRReg storage, GPRReg index, GPRReg text, JumpList& hasNoText)
{
    static_assert(sizeof(NumericStrings::StringWithJSString) == 2 * sizeof(void*));
    jit.zeroExtend32ToWord(key, index);
    hasNoText.append(jit.branch32(CCallHelpers::AboveOrEqual, index, TrustedImm32(NumericStrings::cacheSize)));
    jit.lshift64(TrustedImm32(1), index);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.smallIntStrings)), storage);
    jit.loadPtr(BaseIndex(storage, index, CCallHelpers::TimesEight, NumericStrings::StringWithJSString::offsetOfJSString()), text);
    hasNoText.append(jit.branchTestPtr(CCallHelpers::Zero, text));
}

static void loadStorageOfPlainArray(StubAssembler& jit, const ElementRegisters& registers, JumpList& notHandled)
{
    const auto [base, index, storage, length, type, number] = registers;

    notHandled.append(jit.branchIfNotCell(base));
    notHandled.append(jit.branch8(CCallHelpers::NotEqual, Address(base, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(ArrayWithContiguous)));
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    notHandled.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
}

static Jump emitGetByValAtConstantIndexEntry(StubAssembler& jit, const ElementRegisters& registers)
{
    const auto [base, index, storage, length, type, number] = registers;
    JumpList notHandled;

    jit.bindEntry(getByValAtConstantIndex());
    loadStorageOfPlainArray(jit, registers, notHandled);
    returnValueUnlessHole(jit, registers, BaseIndex(storage, index, CCallHelpers::TimesEight), notHandled);

    notHandled.link(&jit);
    boxInt32(jit, index, index);
    return jit.jump();
}

static Address loadPlainArrayElementAddress(StubAssembler& jit, GPRReg base, GPRReg storage, unsigned index, JumpList& notHandled)
{
    notHandled.append(jit.branchIfNotCell(base));
    notHandled.append(jit.branch8(CCallHelpers::NotEqual, Address(base, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(ArrayWithContiguous)));
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    notHandled.append(jit.branch32(CCallHelpers::BelowOrEqual, Address(storage, Butterfly::offsetOfPublicLength()), TrustedImm32(static_cast<int32_t>(index))));
    return Address(storage, static_cast<int32_t>(index * sizeof(EncodedJSValue)));
}

void generateGetByValAtKnownIndex(StubAssembler& jit)
{
    const unsigned index = jit.key().count();
    const GPRReg base = jit.operand(0);
    const GPRReg key = jit.allocateScratch(StubRegister::A1);
    const GPRReg storage = jit.allocateScratch(StubRegister::T11);
    JumpList notHandled;
    Address element = loadPlainArrayElementAddress(jit, base, storage, index, notHandled);
    jit.load64(element, storage);
    notHandled.append(jit.branchTest64(CCallHelpers::Zero, storage));
    jit.move(storage, jit.result());
    jit.returnToCaller();

    notHandled.link(&jit);
    jit.move(TrustedImm32(static_cast<int32_t>(index)), key);
    checkStubOperands(getByValAtConstantIndex(), { base, key });
    jit.jumpToStub(getByValAtConstantIndex());
}

static void generateGetByVal(StubAssembler& jit, RegistersOfCaller registersOfCaller)
{
    const ElementRegisters registers = allocateElementRegisters(jit);
    const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT1);
    const GPRReg probe = jit.allocateScratch(StubRegister::A2);
    const auto [base, key, storage, index, shape, number] = registers;
    const BaseIndex element(storage, index, CCallHelpers::TimesEight);
    JumpList slow;
    JumpList absent;

    JumpList isNeitherStringNorCompiledObject;
    JumpList isInsideView;
    Jump isConstantIndex;
    if (registersOfCaller == RegistersOfCaller::AreLost)
        emitGetByValAtIndexEntry(jit, registers, numberScratch, isNeitherStringNorCompiledObject, isInsideView);
    else
        isConstantIndex = emitGetByValAtConstantIndexEntry(jit, registers);
    jit.bindEntry(jit.key());
    Jump notInt32 = jit.branch64(CCallHelpers::Below, key, numberTag);
    Label indexReady = jit.label();
    if (isConstantIndex.isSet())
        isConstantIndex.linkTo(indexReady, &jit);
    slow.append(jit.branchIfNotCell(base));
    static_assert(JSObject::butterflyOffset() + sizeof(void*) <= MarkedBlock::atomSize);
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.zeroExtend32ToWord(key, index);
    Jump isNoPlainList = jit.branch8(CCallHelpers::NotEqual, Address(base, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(ArrayWithContiguous));
    Label hasContiguousShape = jit.label();
    absent.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    returnValueUnlessHole(jit, registers, element, absent);

    isNoPlainList.link(&jit);
    jit.load8(Address(base, JSCell::indexingTypeAndMiscOffset()), shape);
    Jump noButterfly = jit.branchTest32(CCallHelpers::Zero, shape, TrustedImm32(IndexingShapeMask));
    jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ArrayWithInt32)).linkTo(hasContiguousShape, &jit);
    Jump isNoListOfDoubles = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(ArrayWithDouble));
    Label hasDoubleShape = jit.label();
    absent.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    returnDoubleUnlessHole(jit, registers, element, numberScratch, absent);

    isNoListOfDoubles.link(&jit);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape)).linkTo(hasContiguousShape, &jit);
    jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(Int32Shape)).linkTo(hasContiguousShape, &jit);
    absent.append(jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(UndecidedShape)));
    jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape)).linkTo(hasDoubleShape, &jit);
    loadArrayStorageElementAndReturn(jit, registers, probe, absent, slow);

    absent.link(&jit);
    Label isAbsent = emitAbsentElement(jit, registers, slow);

    noButterfly.link(&jit);
    Jump notString = jit.branchIfNotString(base);
    loadStringCharacterAndReturn(jit, registers, slow, isAbsent);

    notString.link(&jit);
    JumpList isNoView;
    loadTypedArrayElementAndReturn(jit, registers, numberScratch, slow, isAbsent, isNoView, isInsideView);

    isNoView.link(&jit);
    JumpList isNotCompiledObject = branchIfNotCompiledObject(jit, base);
    JumpList compiledObjectHasNone;
    loadCompiledObjectElementAndReturn(jit, registers, numberScratch, probe, slow, compiledObjectHasNone);

    isNotCompiledObject.link(&jit);
    isNeitherStringNorCompiledObject.link(&jit);
    Jump isNotMapped = jit.branch32(CCallHelpers::NotEqual, registers.type, TrustedImm32(DirectArgumentsType));
    Jump isOverridden = jit.branchTestPtr(CCallHelpers::NonZero, Address(base, DirectArguments::offsetOfMappedArguments()));
    jit.zeroExtend32ToWord(key, index);
    Jump isPastEnd = jit.branch32(CCallHelpers::AboveOrEqual, index, Address(base, DirectArguments::offsetOfLength()));
    jit.load64(BaseIndex(base, index, CCallHelpers::TimesEight, DirectArguments::storageOffset()), jit.result());
    jit.returnToCaller();

    isNotMapped.link(&jit);
    isOverridden.link(&jit);
    isPastEnd.link(&jit);
    answerWhereNoObjectOfChainHasElements(jit, registers, slow, isAbsent, compiledObjectHasNone);

    notInt32.link(&jit);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, key, numberTag);
    unboxDouble(jit, key, storage, number);
    jit.branchConvertDoubleToInt32(number, storage, slow, numberScratch, false);
    boxInt32(jit, storage, key);
    jit.jump().linkTo(indexReady, &jit);

    notNumber.link(&jit);
    slow.append(jit.branchIfNotCell(base));
    slow.append(jit.branchIfNotCell(key));
    checkStubOperands(getByValCachedCells(registersOfCaller), { base, key });
    jit.jumpToStub(getByValCachedCells(registersOfCaller));

    slow.link(&jit);
    {
        const GPRReg text = registers.type;
        const GPRReg noHeader = index;
        static_assert(areDistinctOnEveryISA(StubRegister::R0, StubRegister::A1, slotPayloadRegister, slotHeaderRegister, StubRegister::T11));
        checkStubOperands(throughProxy(TrapOfProxy::Get), { base, text, noHeader });
        JumpList operationReads;
        loadTextOfIndexForTrap(jit, base, key, storage, index, text, operationReads);
        jit.move(TrustedImm32(0), noHeader);
        jit.jumpToStub(throughProxy(TrapOfProxy::Get));
        operationReads.link(&jit);
    }
    if (registersOfCaller == RegistersOfCaller::AreLost) {
        callBinaryOperation(jit, Entry::operationAOTGetByVal);
        return;
    }
    jit.callOperationAndReturn(Entry::operationAOTGetByVal, { .saves = OperationCall::Saves::CallerSavedGPRsAndFewFPRs, .secondArgumentIn = base, .thirdArgumentIn = key });
}

void generateGetByVal(StubAssembler& jit)
{
    generateGetByVal(jit, RegistersOfCaller::AreLost);
}

void generateGetByValKeepingRegisters(StubAssembler& jit)
{
    generateGetByVal(jit, RegistersOfCaller::AreKept);
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

static Jump emitPutByValAtConstantIndexEntry(StubAssembler& jit, const ElementRegisters& registers, GPRReg value, ECMAMode mode)
{
    const auto [base, index, storage, length, type, number] = registers;
    JumpList notHandled;

    jit.bindEntry(putByValAtConstantIndex(mode));
    loadStorageOfPlainArray(jit, registers, notHandled);
    jit.store64(value, BaseIndex(storage, index, CCallHelpers::TimesEight));
    returnAfterStoring(jit, base, value);

    notHandled.link(&jit);
    boxInt32(jit, index, index);
    return jit.jump();
}

void generatePutByValAtKnownIndex(StubAssembler& jit)
{
    const unsigned index = jit.key().count();
    const ECMAMode mode = ecmaModeOf(static_cast<ElementStoreMode>(jit.key().form()));
    const GPRReg base = jit.clobberOperand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg key = jit.allocateScratch(StubRegister::A1);
    const GPRReg storage = jit.allocateScratch(StubRegister::T11);
    JumpList notHandled;
    Address element = loadPlainArrayElementAddress(jit, base, storage, index, notHandled);
    jit.store64(value, element);
    returnAfterStoring(jit, base, value);

    notHandled.link(&jit);
    jit.move(TrustedImm32(static_cast<int32_t>(index)), key);
    checkStubOperands(putByValAtConstantIndex(mode), { base, key, value });
    jit.jumpToStub(putByValAtConstantIndex(mode));
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

static void storeTypedArrayElementAndReturn(StubAssembler& jit, const ElementRegisters& registers, GPRReg value, GPRReg doubleBits, JumpList& slow, JumpList& isNoTypedArray)
{
    const auto [base, key, storage, index, type, number] = registers;

    JumpList isOutside;
    checkTypedArrayAccess(jit, registers, slow, &isOutside, &isNoTypedArray);
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
    jit.sub32(type, TrustedImm32(Int32ArrayType), doubleBits);
    Jump hasNoWords = jit.branch32(CCallHelpers::Above, doubleBits, TrustedImm32(Uint32ArrayType - Int32ArrayType));
    store32();
    jit.returnToCaller();

    hasNoWords.link(&jit);
    Jump hasNoBytes = jit.branch32(CCallHelpers::Above, type, TrustedImm32(Uint8ArrayType));
    store8();
    jit.returnToCaller();

    hasNoBytes.link(&jit);
    Jump clampsInt32 = jit.branch32(CCallHelpers::Equal, type, TrustedImm32(Uint8ClampedArrayType));
    Jump hasNoHalfWords = jit.branch32(CCallHelpers::Above, type, TrustedImm32(Uint16ArrayType));
    store16();
    jit.returnToCaller();

    hasNoHalfWords.link(&jit);
    jit.convertInt32ToDouble(value, number);
    forJSType(Float64ArrayType, storeDouble);
    forJSType(Float32ArrayType, storeFloat);
    slow.append(jit.jump());

    clampsInt32.link(&jit);
    jit.zeroExtend32ToWord(value, doubleBits);
    Jump int32FitsByte = jit.branch32(CCallHelpers::BelowOrEqual, doubleBits, TrustedImm32(UINT8_MAX));
    jit.rshift32(TrustedImm32(31), doubleBits);
    jit.not32(doubleBits);
    int32FitsByte.link(&jit);
    jit.store8(doubleBits, BaseIndex(storage, index, CCallHelpers::TimesOne));
    jit.returnToCaller();

    valueIsNotInt32.link(&jit);
    Jump valueIsNoNumber = jit.branchTest64(CCallHelpers::Zero, value, numberTag);
    unboxDouble(jit, value, doubleBits, number);
    forJSType(Float64ArrayType, storeDouble);
    forJSType(Float32ArrayType, storeFloat);
    Jump isClamped = jit.branch32(CCallHelpers::Equal, type, TrustedImm32(Uint8ClampedArrayType));
    slow.append(jit.branch32(CCallHelpers::Above, type, TrustedImm32(Uint32ArrayType)));
#if CPU(ARM64)
    if (jit.supportsDoubleToInt32ConversionUsingJavaScriptSemantics())
        jit.convertDoubleToInt32UsingJavaScriptSemantics(number, doubleBits);
    else
#endif
        slow.append(jit.branchTruncateDoubleToInt32ViaInt64(number, doubleBits));
    Jump hasWords = jit.branch32(CCallHelpers::AboveOrEqual, type, TrustedImm32(Int32ArrayType));
    Jump hasHalfWords = jit.branch32(CCallHelpers::AboveOrEqual, type, TrustedImm32(Int16ArrayType));
    jit.store8(doubleBits, BaseIndex(storage, index, CCallHelpers::TimesOne));
    jit.returnToCaller();
    hasHalfWords.link(&jit);
    jit.store16(doubleBits, BaseIndex(storage, index, CCallHelpers::TimesTwo));
    jit.returnToCaller();
    hasWords.link(&jit);
    jit.store32(doubleBits, BaseIndex(storage, index, CCallHelpers::TimesFour));
    jit.returnToCaller();

    isClamped.link(&jit);
    jit.roundTowardNearestIntDouble(number, number);
    jit.truncateDoubleToInt64(number, doubleBits);
    Jump fitsByte = jit.branch64(CCallHelpers::BelowOrEqual, doubleBits, TrustedImm32(UINT8_MAX));
    slow.append(jit.branchSub64(CCallHelpers::Overflow, TrustedImm32(1), doubleBits));
    jit.rshift64(TrustedImm32(63), doubleBits);
    jit.not32(doubleBits);
    fitsByte.link(&jit);
    jit.store8(doubleBits, BaseIndex(storage, index, CCallHelpers::TimesOne));
    jit.returnToCaller();

    valueIsNoNumber.link(&jit);
    const GPRReg converted = doubleBits;
    slow.append(jit.branchIfCell(value));
    jit.compare64(CCallHelpers::Equal, value, TrustedImm32(JSValue::ValueTrue), converted);
    Jump holdsNoIntegers = jit.branch32(CCallHelpers::Above, type, TrustedImm32(Uint32ArrayType));
    Jump holdsNoBytes = jit.branch32(CCallHelpers::Above, type, TrustedImm32(Uint8ClampedArrayType));
    jit.store8(converted, BaseIndex(storage, index, CCallHelpers::TimesOne));
    jit.returnToCaller();

    holdsNoBytes.link(&jit);
    Jump holds32Bits = jit.branch32(CCallHelpers::Above, type, TrustedImm32(Uint16ArrayType));
    jit.store16(converted, BaseIndex(storage, index, CCallHelpers::TimesTwo));
    jit.returnToCaller();

    holds32Bits.link(&jit);
    jit.store32(converted, BaseIndex(storage, index, CCallHelpers::TimesFour));
    jit.returnToCaller();

    holdsNoIntegers.link(&jit);
    slow.append(jit.branch32(CCallHelpers::Below, type, TrustedImm32(Float32ArrayType)));
    slow.append(jit.branch32(CCallHelpers::Above, type, TrustedImm32(Float64ArrayType)));
    jit.convertInt32ToDouble(converted, number);
    Jump isDefined = jit.branch64(CCallHelpers::NotEqual, value, TrustedImm32(JSValue::ValueUndefined));
    jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), converted);
    jit.move64ToDouble(converted, number);
    isDefined.link(&jit);
    forJSType(Float64ArrayType, storeDouble);
    storeFloat();
    jit.returnToCaller();

    isOutside.link(&jit);
    Jump isNumber = jit.branchTest64(CCallHelpers::NonZero, value, numberTag);
    slow.append(jit.branchIfCell(value));
    isNumber.link(&jit);
    slow.append(jit.branch32(CCallHelpers::Equal, type, TrustedImm32(BigInt64ArrayType)));
    slow.append(jit.branch32(CCallHelpers::Equal, type, TrustedImm32(BigUint64ArrayType)));
    jit.returnToCaller();
}

template<typename Functor>
static void storeOrAppendAndReturn(StubAssembler& jit, const ElementRegisters& registers, GPRReg allocator, GPRReg scratch, JumpList& slow, const Functor& storeAndReturn)
{
    const auto [base, key, storage, index, newLength, number] = registers;

    Label loadStorage = jit.label();
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.zeroExtend32ToWord(key, index);
    Jump isPastLength = jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength()));
    Label stores = jit.label();
    storeAndReturn(BaseIndex(storage, index, CCallHelpers::TimesEight));

    isPastLength.link(&jit);
    Jump isBeyondVector = jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfVectorLength()));
    jit.add32(TrustedImm32(1), index, newLength);
    jit.store32(newLength, Address(storage, Butterfly::offsetOfPublicLength()));
    jit.jump().linkTo(stores, &jit);

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
    Label isReady = jit.label();
    storeOrAppendAndReturn(jit, registers, allocator, doubleBits, slow, [&](BaseIndex element) {
        jit.storeDouble(number, element);
        jit.returnToCaller();
    });

    isInt32.link(&jit);
    jit.convertInt32ToDouble(value, number);
    jit.jump().linkTo(isReady, &jit);
}

StubSignature signatureOfStoreElementOfCompiledObject(StubKey key) { return signatureOfPutByVal(key); }

void generateStoreElementOfCompiledObject(StubAssembler& jit)
{
    static_assert(areDistinctOnEveryISA(StubRegister::R0, StubRegister::A1, StubRegister::A2, StubRegister::T9, StubRegister::T11, StubRegister::T12, StubRegister::T13, StubRegister::A3, StubRegister::A4));
    static_assert(sizeof(PrototypeTransition) == 16 && hasOneBitSet(numberOfPrototypeTransitions) && MarkedSpace::sizeStep == 2 * sizeof(Allocator) && sizeof(IndexingHeader) == sizeof(EncodedJSValue));
    constexpr size_t sizeOfExtraProperties = JSObject::offsetOfInlineStorage() + extraPropertiesInlineCapacity * sizeof(EncodedJSValue);
    static_assert(!(sizeOfExtraProperties % MarkedSpace::sizeStep));
    const PutMode mode = static_cast<PutMode>(jit.key().form());
    const GPRReg base = jit.clobberOperand(0);
    const GPRReg key = jit.clobberOperand(1);
    const GPRReg value = jit.operand(2);
    const GPRReg extraProperties = jit.allocateScratch(StubRegister::T11);
    const GPRReg index = jit.allocateScratch(StubRegister::T12);
    const GPRReg butterfly = jit.allocateScratch(StubRegister::T13);
    const GPRReg structure = jit.allocateScratch(StubRegister::A3);
    const GPRReg scratch = jit.allocateScratch(StubRegister::A4);
    const GPRReg transition = jit.allocateScratch(StubRegister::T9);
    const GPRReg hiddenWordOffset = butterfly;
    const GPRReg shape = butterfly;
    const BaseIndex element(butterfly, index, CCallHelpers::TimesEight);
    const Address orderWord(base, offsetOfOrderWord);
    const Address publicLength(butterfly, Butterfly::offsetOfPublicLength());
    JumpList slow;

    Jump hasNoExtraProperties = jit.branchTest32(CCallHelpers::Zero, orderWord, TrustedImm32(orderWordHasExtraProperties));
    loadStructure(jit, base, structure);
    jit.load32(Address(structure, CompiledStructure::offsetOfExtraPropertiesSlot()), hiddenWordOffset);
    slow.append(jit.branchTest32(CCallHelpers::Zero, hiddenWordOffset, TrustedImm32(SlotData::maxOffset)));
    jit.and32(TrustedImm32(SlotData::maxOffset), hiddenWordOffset);
    loadExtraProperties(jit, base, hiddenWordOffset, extraProperties, slow);
    jit.load8(Address(extraProperties, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), shape);
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
    slow.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(Int32Shape)));
    slow.append(jit.branch64(CCallHelpers::Below, value, numberTag));
    isContiguous.link(&jit);
    jit.loadPtr(Address(extraProperties, JSObject::butterflyOffset()), butterfly);
    jit.zeroExtend32ToWord(key, index);
    Jump isBeyondLength = jit.branch32(CCallHelpers::AboveOrEqual, index, publicLength);
    Jump isHole = jit.branchTest64(CCallHelpers::Zero, element);
    Label storeValue = jit.label();
    jit.store64(value, element);
    jit.move(extraProperties, base);
    returnAfterStoring(jit, base, value);

    isHole.link(&jit);
    isBeyondLength.link(&jit);
    slow.append(jit.branchIfNotType(extraProperties, FinalObjectType));
    slow.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
    slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(structure, CompiledStructure::offsetOfFlags()), TrustedImm32(static_cast<uint8_t>(CompiledStructure::Flag::HasOtherPrototype))));
    jit.load64(Address(structure, Structure::prototypeOffset()), structure);
    slow.append(jit.branch64(CCallHelpers::NotEqual, structure, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.objectPrototype))));
    slow.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(~(orderAndCountMask | orderWordHasExtraProperties))));
    jit.branch32(CCallHelpers::Below, index, publicLength).linkTo(storeValue, &jit);
    Jump isBeyondVector = jit.branch32(CCallHelpers::AboveOrEqual, index, Address(butterfly, Butterfly::offsetOfVectorLength()));
    Label lengthen = jit.label();
    jit.add32(TrustedImm32(1), index, structure);
    jit.store32(structure, publicLength);
    jit.jump().linkTo(storeValue, &jit);

    isBeyondVector.link(&jit);
    const GPRReg neededLength = index;
    JumpList cannotGrow;
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, index, TrustedImm32(maxLengthForElementwiseCopy)));
    jit.load32(publicLength, transition);
    jit.add32(TrustedImm32(1), neededLength);
    growVectorFor(jit, { .array = extraProperties, .butterfly = butterfly, .length = transition, .storage = structure, .allocator = scratch, .scratch = key, .isOfExtraProperties = true }, neededLength, cannotGrow);
    jit.sub32(TrustedImm32(1), index);
    jit.loadPtr(Address(extraProperties, JSObject::butterflyOffset()), butterfly);
    jit.jump().linkTo(lengthen, &jit);

    cannotGrow.link(&jit);
    jit.sub32(TrustedImm32(1), index);
    boxInt32(jit, index, key);
    slow.append(jit.jump());

    hasNoExtraProperties.link(&jit);
    const GPRReg structureID = structure;
    const GPRReg allocator = structure;
    const GPRReg vectorLength = scratch;
    const GPRReg sizeStep = extraProperties;
    slow.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(~orderAndCountMask)));
    jit.zeroExtend32ToWord(key, index);
    slow.append(jit.branch32(CCallHelpers::Above, index, TrustedImm32(maxIndexOfFirstElementStoredContiguously)));
    jit.load32(Address(base, JSCell::structureIDOffset()), structureID);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.extraPropertiesWithElementsStructure)), scratch);
    jit.xor32(scratch, structureID, transition);
    jit.urshift32(TrustedImm32(prototypeTransitionHashShift), transition);
    jit.and32(TrustedImm32(numberOfPrototypeTransitions - 1), transition);
    jit.lshift32(TrustedImm32(4), transition);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(objects.prototypeTransitions)), transition);
    slow.append(jit.branch64(CCallHelpers::NotEqual, Address(transition, OBJECT_OFFSETOF(PrototypeTransition, prototype)), scratch));
    slow.append(jit.branch32(CCallHelpers::NotEqual, Address(transition, OBJECT_OFFSETOF(PrototypeTransition, before)), structureID));
    slow.append(branchIfBarrierIsNeeded(jit, base, scratch));
    slow.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(frameCellState)));

    jit.add32(TrustedImm32(1), index, sizeStep);
    jit.urshift32(sizeStep, TrustedImm32(1), scratch);
    jit.add32(scratch, sizeStep);
    Jump isLongEnough = jit.branch32(CCallHelpers::AboveOrEqual, sizeStep, TrustedImm32(BASE_CONTIGUOUS_VECTOR_LEN));
    jit.move(TrustedImm32(BASE_CONTIGUOUS_VECTOR_LEN), sizeStep);
    isLongEnough.link(&jit);
    jit.add32(TrustedImm32(2), sizeStep);
    jit.urshift32(TrustedImm32(1), sizeStep);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, sizeStep, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, butterfly, allocator, scratch, slow);
    jit.load32(Address(allocator, LocalAllocator::offsetOfCellSize()), vectorLength);
    jit.sub32(TrustedImm32(sizeof(IndexingHeader)), vectorLength);
    jit.urshift32(TrustedImm32(3), vectorLength);
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), butterfly);
    jit.store32(vectorLength, Address(butterfly, Butterfly::offsetOfVectorLength()));
    jit.add32(TrustedImm32(1), index, structure);
    jit.store32(structure, publicLength);
    Label clearPrevious = jit.label();
    jit.sub32(TrustedImm32(1), vectorLength);
    jit.store64(TrustedImm32(0), BaseIndex(butterfly, vectorLength, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, vectorLength).linkTo(clearPrevious, &jit);
    jit.store64(value, element);

    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
    jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + static_cast<int32_t>(sizeOfExtraProperties / MarkedSpace::sizeStep * sizeof(Allocator))), allocator);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, extraProperties, allocator, scratch, slow);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.headerOfExtraPropertiesWithElements)), scratch);
    jit.store64(scratch, Address(extraProperties, JSCell::structureIDOffset()));
    jit.storePtr(butterfly, Address(extraProperties, JSObject::butterflyOffset()));
    for (unsigned word = 0; word < extraPropertiesInlineCapacity; ++word)
        jit.store64(TrustedImm32(0), Address(extraProperties, JSObject::offsetOfInlineStorage() + static_cast<int32_t>(word * sizeof(EncodedJSValue))));
    fenceAfterAllocation(jit);

    jit.load32(Address(transition, OBJECT_OFFSETOF(PrototypeTransition, after)), structureID);
    jit.move(structureID, scratch);
    decodeStructureID(jit, scratch);
    jit.load32(Address(scratch, CompiledStructure::offsetOfExtraPropertiesSlot()), scratch);
    jit.and32(TrustedImm32(SlotData::maxOffset), scratch);
    if (jit.options().useWideReferences)
        jit.store64(extraProperties, BaseIndex(base, scratch, CCallHelpers::TimesOne));
    else {
        encodeCell32(jit, extraProperties, index);
        jit.store32(index, BaseIndex(base, scratch, CCallHelpers::TimesOne));
    }
    jit.or32(TrustedImm32(orderWordHasExtraProperties), orderWord);
    jit.store32(structureID, Address(base, JSCell::structureIDOffset()));
    jit.returnToCaller();

    slow.link(&jit);
    if (mode == PutMode::Define) {
        jit.callOperationAndReturn(Entry::operationAOTDefineField, { .returns = OperationCall::Returns::Void, .secondArgumentIn = base, .thirdArgumentIn = key, .fourthArgumentIn = value });
        return;
    }
    jit.move(value, structure);
    jit.move(TrustedImm32(mode == PutMode::Strict ? 1 : 0), scratch);
    jit.callOperationAndReturn(Entry::operationAOTPutByVal, { .returns = OperationCall::Returns::Void, .secondArgumentIn = base, .thirdArgumentIn = key });
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

    Jump isConstantIndex = emitPutByValAtConstantIndexEntry(jit, registers, value, mode);
    emitPutByValAtIndexEntry(jit, registers, value, mode);
    jit.bindEntry(jit.key());
    slow.append(jit.branch64(CCallHelpers::Below, key, numberTag));
    isConstantIndex.link(&jit);
    slow.append(jit.branchIfNotCell(base));
    jit.load8(Address(base, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), shape);
    Jump isNotContiguous = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(ContiguousShape));
    Label storesValue = jit.label();
    storeOrAppendAndReturn(jit, registers, valueArgument, doubleBits, slow, [&](BaseIndex element) {
        jit.store64(value, element);
        returnAfterStoring(jit, base, value);
    });

    isNotContiguous.link(&jit);
    Jump isNotInt32Shape = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(Int32Shape));
    slow.append(jit.branch64(CCallHelpers::Below, value, numberTag));
    jit.jump().linkTo(storesValue, &jit);

    isNotInt32Shape.link(&jit);
    Jump isDoubleShape = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
    slow.append(jit.branchTest32(CCallHelpers::NonZero, shape));
    JumpList isNoTypedArray;
    storeTypedArrayElementAndReturn(jit, registers, value, doubleBits, slow, isNoTypedArray);

    isNoTypedArray.link(&jit);
    const StubKey storeInCompiledObject = storeElementOfCompiledObject(mode.isStrict() ? PutMode::Strict : PutMode::Sloppy);
    checkStubOperands(storeInCompiledObject, { base, key, value });
    jit.linkToStub(jit.branch32(CCallHelpers::Equal, registers.type, TrustedImm32(CompiledObjectType)), storeInCompiledObject);
    slow.append(jit.jump());

    isDoubleShape.link(&jit);
    storeDoubleElementAndReturn(jit, registers, value, valueArgument, doubleBits, slow);

    slow.link(&jit);
    {
        const GPRReg text = registers.type;
        const GPRReg storedValue = jit.reuseScratch(valueStoredThroughProxyRegister);
        const GPRReg isStrict = jit.reuseScratch(StubRegister::T14);
        static_assert(areDistinctOnEveryISA(StubRegister::R0, StubRegister::A1, StubRegister::A2, slotPayloadRegister, valueStoredThroughProxyRegister, StubRegister::T14, StubRegister::T11, StubRegister::T12));
        checkStubOperands(throughProxy(TrapOfProxy::Set), { base, text, storedValue, isStrict });
        JumpList operationStores;
        loadTextOfIndexForTrap(jit, base, key, registers.storage, registers.index, text, operationStores);
        jit.move(value, storedValue);
        jit.move(TrustedImm32(mode.isStrict() ? 1 : 0), isStrict);
        jit.jumpToStub(throughProxy(TrapOfProxy::Set));
        operationStores.link(&jit);
    }
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
    case ExpectedElements::Float64Array:
        return Float64ArrayType;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void accessExpectedElementAndReturn(StubAssembler& jit, const ElementRegisters& registers, ExpectedElements expected, GPRReg index, std::optional<GPRReg> value, FPRReg numberScratch, JumpList& miss)
{
    const GPRReg base = registers.base;
    const GPRReg storage = registers.storage;
    const FPRReg number = registers.number;

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
        case ExpectedElements::Float64Array:
            jit.loadDouble(BaseIndex(storage, index, CCallHelpers::TimesEight), number);
            returnBoxedDouble(jit, registers, PureDouble::purify(jit, number, number), numberScratch);
            return;
        }
        returnBoxedInt32(jit, registers);
        return;
    }
    if (expected == ExpectedElements::Float64Array) {
        Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, *value, numberTag);
        miss.append(jit.branchTest64(CCallHelpers::Zero, *value, numberTag));
        unboxDouble(jit, *value, registers.type, number);
        jit.storeDouble(number, BaseIndex(storage, index, CCallHelpers::TimesEight));
        jit.returnToCaller();
        isInt32.link(&jit);
        jit.convertInt32ToDouble(*value, number);
        jit.storeDouble(number, BaseIndex(storage, index, CCallHelpers::TimesEight));
        jit.returnToCaller();
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
    case ExpectedElements::Float64Array:
        RELEASE_ASSERT_NOT_REACHED();
    }
    jit.returnToCaller();
}

static void emitEntriesExpecting(StubAssembler& jit, ExpectedElements expected, std::optional<GPRReg> value, StubKey atIndex, StubKey takesEverythingAtIndex, StubKey takesEverything)
{
    const ElementRegisters registers = allocateElementRegisters(jit);
    const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT1);
    auto goOnToStub = [&](JumpList& miss, StubKey stub) {
        if (value)
            checkStubOperands(stub, { registers.base, registers.key, *value });
        else
            checkStubOperands(stub, { registers.base, registers.key });
        miss.link(&jit);
        jit.jumpToStub(stub);
    };

    JumpList missAtIndex;
    jit.bindEntry(atIndex);
    accessExpectedElementAndReturn(jit, registers, expected, registers.key, value, numberScratch, missAtIndex);
    goOnToStub(missAtIndex, takesEverythingAtIndex);

    JumpList miss;
    jit.bindEntry(jit.key());
    miss.append(jit.branch64(CCallHelpers::Below, registers.key, numberTag));
    jit.signExtend32ToPtr(registers.key, registers.index);
    accessExpectedElementAndReturn(jit, registers, expected, registers.index, value, numberScratch, miss);
    goOnToStub(miss, takesEverything);
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
