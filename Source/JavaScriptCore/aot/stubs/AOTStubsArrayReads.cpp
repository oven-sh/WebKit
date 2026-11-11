/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArrays.h"

#if ENABLE(AOT)

#include "AOTAtomListTable.h"
#include "AOTImageText.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsHashTables.h"
#include "AOTStubsStrings.h"
#include "ArrayConventions.h"
#include "ArrayStorage.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSCell.h"
#include "JSCellButterfly.h"
#include "JSObject.h"
#include "JSString.h"
#include "SparseArrayValueMap.h"
#include "Structure.h"
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2));

static ArrayRead readOf(StubAssembler& jit) { return static_cast<ArrayRead>(jit.key().form()); }

static void callOperationAndReturn(StubAssembler& jit)
{
    jit.callOperationAndReturn(readOf(jit) == ArrayRead::OfPlainList ? Entry::operationAOTGetByValOfPlainList : Entry::operationAOTGetByVal, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters });
}

void emitFindSparseElement(StubAssembler& jit, const SparseElementSearch& search, JumpList& isAbsent)
{
    const auto [owner, index, entry, probe, table] = search;
    const GPRReg mask = probe;
    const GPRReg statusBytes = table;
    constexpr unsigned entrySizeShift = 4;
    static_assert(sizeof(SparseArrayEntry) == 1 << entrySizeShift);
    static_assert(!WTF::StatusByte::empty && WTF::StatusByte::isFullBit == 0x80);

    emitHashOfBits(jit, index, entry, probe, table);
    jit.loadPtr(Address(owner, JSObject::butterflyOffset()), table);
    jit.loadPtr(Address(table, ArrayStorage::sparseMapOffset()), table);
    isAbsent.append(jit.branchTestPtr(CCallHelpers::Zero, table));
    jit.loadPtr(Address(table, SparseArrayValueMap::offsetOfTable()), table);
    isAbsent.append(jit.branchTestPtr(CCallHelpers::Zero, table));
    jit.load32(Address(table, SparseArrayValueMap::offsetOfTableSize()), mask);
    jit.lshift64(TrustedImm32(entrySizeShift), mask);
    jit.add64(mask, statusBytes);
    jit.urshift64(TrustedImm32(entrySizeShift), mask);
    jit.sub32(TrustedImm32(1), mask);

    Label hasSlotNumber = jit.label();
    jit.and32(mask, entry);
    Jump wasNeverFilled = jit.branchTest8(CCallHelpers::Zero, BaseIndex(statusBytes, entry, CCallHelpers::TimesOne));
    Jump isNotFull = jit.branchTest8(CCallHelpers::Zero, BaseIndex(statusBytes, entry, CCallHelpers::TimesOne), TrustedImm32(WTF::StatusByte::isFullBit));
    jit.sub64(mask, entry);
    jit.sub64(TrustedImm32(1), entry);
    jit.lshift64(TrustedImm32(entrySizeShift), entry);
    jit.add64(statusBytes, entry);
    Jump isFound = jit.branch32(CCallHelpers::Equal, Address(entry, SparseArrayEntry::offsetOfIndex()), index);
    jit.sub64(statusBytes, entry);
    jit.rshift64(TrustedImm32(entrySizeShift), entry);
    jit.add64(mask, entry);
    jit.add64(TrustedImm32(1), entry);

    isNotFull.link(&jit);
    jit.rotateRight64(TrustedImm32(32), index);
    jit.add64(TrustedImm32(1), index);
    jit.add32(index, entry);
    jit.rotateRight64(TrustedImm32(32), index);
    jit.jump().linkTo(hasSlotNumber, &jit);

    wasNeverFilled.link(&jit);
    jit.zeroExtend32ToWord(index, index);
    isAbsent.append(jit.jump());

    isFound.link(&jit);
    jit.zeroExtend32ToWord(index, index);
}

void generateArrayGetAtIndex(StubAssembler& jit)
{
    bool listIsAsMade = readOf(jit) == ArrayRead::OfListAsMade;
    const GPRReg array = jit.operand(0);
    const GPRReg position = jit.clobberOperand(1);
    const GPRReg result = jit.result();
    const GPRReg shape = jit.allocateScratch(StubRegister::T9);
    const GPRReg butterfly = jit.allocateScratch(StubRegister::T10);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT1);
    JumpList isAbsent;
    JumpList isAbsentAtIndex;
    JumpList callsOperation;

    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), butterfly);
    Jump isDoubleShape = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
    Jump isContiguousShape = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
    Jump isInt32Shape = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(Int32Shape));
    isAbsent.append(jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(UndecidedShape)));
    static_assert(SlowPutArrayStorageShape > ArrayStorageShape && SlowPutArrayStorageShape == IndexingShapeMask);
    callsOperation.append(jit.branch32(CCallHelpers::Below, shape, TrustedImm32(ArrayStorageShape)));
    jit.move(TrustedImm64(MAX_ARRAY_INDEX), result);
    callsOperation.append(jit.branch64(CCallHelpers::Above, position, result));
    Jump isOutsideVector = jit.branch32(CCallHelpers::AboveOrEqual, position, Address(butterfly, ArrayStorage::vectorLengthOffset()));
    jit.load64(BaseIndex(butterfly, position, CCallHelpers::TimesEight, ArrayStorage::vectorOffset()), result);
    Jump isHoleInVector = jit.branchTest64(CCallHelpers::Zero, result);
    jit.returnToCaller();

    isOutsideVector.link(&jit);
    isHoleInVector.link(&jit);
    const GPRReg index = shape;
    const GPRReg entry = result;
    jit.move(position, index);
    JumpList isAbsentFromMap;
    emitFindSparseElement(jit, { array, index, entry, position, butterfly }, isAbsentFromMap);
    Jump isAccessor = jit.branchTest32(CCallHelpers::NonZero, Address(entry, SparseArrayEntry::offsetOfAttributes()), TrustedImm32(static_cast<int32_t>(PropertyAttribute::Accessor)));
    jit.load64(Address(entry, SparseArrayEntry::offsetOfValue()), result);
    jit.returnToCaller();

    isAccessor.link(&jit);
    jit.move(index, position);
    callsOperation.append(jit.jump());

    isAbsentFromMap.link(&jit);
    jit.move(index, position);
    isAbsentAtIndex.append(jit.jump());

    isContiguousShape.link(&jit);
    isInt32Shape.link(&jit);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), result);
    isAbsent.append(jit.branch64(CCallHelpers::AboveOrEqual, position, result));
    jit.load64(BaseIndex(butterfly, position, CCallHelpers::TimesEight), result);
    isAbsent.append(jit.branchTest64(CCallHelpers::Zero, result));
    jit.returnToCaller();

    isDoubleShape.link(&jit);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), result);
    isAbsent.append(jit.branch64(CCallHelpers::AboveOrEqual, position, result));
    jit.loadDouble(BaseIndex(butterfly, position, CCallHelpers::TimesEight), number);
    PureDouble element = PureDouble::branchIfNaN(jit, number, isAbsent);
    JumpList isNotInt32;
    branchIfNotExactInt32(jit, number, result, isNotInt32, numberScratch);
    boxExactInt32(jit, result, result);
    jit.returnToCaller();
    isNotInt32.link(&jit);
    boxDouble(jit, element, result);
    jit.returnToCaller();

    isAbsent.link(&jit);
    jit.move(TrustedImm64(MAX_ARRAY_INDEX), shape);
    Jump isName = jit.branch64(CCallHelpers::Above, position, shape);
    isAbsentAtIndex.link(&jit);
    if (!listIsAsMade)
        callsOperation.append(branchIfNotPristine(jit, array));
    Label isFoundNowhere = jit.label();
    jit.move(TrustedImm64(JSValue::ValueUndefined), result);
    jit.returnToCaller();

    isName.link(&jit);
    const GPRReg structure = butterfly;
    callsOperation.append(jit.branchIfNotType(array, ArrayType));
    loadStructure(jit, array, structure);
    callsOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_hasPristineElementsBits)));
    branchIfHasNamesOrIsNotOfThisRealm(jit, structure, callsOperation);
    jit.jump().linkTo(isFoundNowhere, &jit);

    callsOperation.link(&jit);
    jit.boxInt52(position, position, shape, number);
    callOperationAndReturn(jit);
}

void generateArrayGetAtNumber(StubAssembler& jit)
{
    const FPRReg position = jit.operandFPR(1);
    const GPRReg index = jit.allocateScratch(StubRegister::A2);
    const FPRReg converted = jit.allocateScratch(FPRInfo::argumentFPR1);

    jit.truncateDoubleToInt64(position, index);
    jit.convertInt64ToDouble(index, converted);
    Jump isNotInteger = jit.branchDouble(CCallHelpers::DoubleNotEqualOrUnordered, position, converted);
    checkStubOperands(arrayGetAtIndex(readOf(jit)), { jit.operand(0), index });
    jit.jumpToStub(arrayGetAtIndex(readOf(jit)));

    isNotInteger.link(&jit);
    boxDouble(jit, PureDouble::purify(jit, position, converted), index);
    callOperationAndReturn(jit);
}

StubSignature signatureOfArrayGetAtIndex(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }
StubSignature signatureOfArrayGetAtNumber(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), FPRInfo::argumentFPR0 }); }

static bool searchesArrayFromStart(StubKey key)
{
    return static_cast<SearchedElements>(key.form()) == SearchedElements::ValuesFromStart;
}

StubSignature signatureOfArrayIndexOfString(StubKey key)
{
    if (searchesArrayFromStart(key))
        return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) });
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

static void loadSubstringCharactersAndFlags(StubAssembler& jit, GPRReg string, GPRReg characters, GPRReg offset, GPRReg flags, JumpList& notSubstring)
{
    notSubstring.append(jit.branchTestPtr(CCallHelpers::Zero, characters, TrustedImm32(JSRopeString::isSubstringInPointer)));
    loadSubstringBaseAndOffset(jit, string, characters, offset);
    jit.loadPtr(Address(characters, JSString::offsetOfValue()), characters);
    jit.load32(Address(characters, StringImpl::flagsOffset()), flags);
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
    Jump is8Bit = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(StringImpl::flagIs8Bit()));
    jit.lshift64(TrustedImm32(1), offset);
    is8Bit.link(&jit);
    jit.addPtr(offset, characters);
}

void generateArrayIndexOfString(StubAssembler& jit)
{
    static_assert(areDistinctOnEveryISA(StubRegister::R0, StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A4, StubRegister::A5, StubRegister::T9, StubRegister::T12, StubRegister::T13));
    static_assert(gprOf(StubRegister::A3) == operationArgumentGPR(3) && areDistinctOnEveryISA(StubRegister::Operation, StubRegister::A1, StubRegister::A2, StubRegister::A3));
    constexpr int32_t wordSize = sizeof(uint64_t);
    constexpr int32_t is8BitFlag = StringImpl::flagIs8Bit();
    static_assert(sizeof(EncodedJSValue) == 8 && is8BitFlag == 4);
    const bool hasStart = searchesArrayFromStart(jit.key());
    const GPRReg array = jit.operand(0);
    const GPRReg searchString = jit.operand(1);
    const GPRReg cursor = hasStart ? jit.clobberOperand(2) : jit.allocateScratch(StubRegister::A3);
    const GPRReg end = jit.allocateScratch(StubRegister::A4);
    const GPRReg lengthAndByteSize = jit.allocateScratch(StubRegister::A5);
    const GPRReg searchCharacters = jit.allocateScratch(StubRegister::T12);
    const GPRReg element = jit.allocateScratch(StubRegister::T9);
    const GPRReg characters = jit.allocateScratch(StubRegister::T13);
    const GPRReg byteSize = jit.result();
    const GPRReg flags = element;
    const GPRReg word = element;
    const FPRReg savedEnd = jit.allocateScratch(FPRInfo::fpRegT0);

    JumpList slowPath;
    JumpList notFound;
    JumpList found;
    JumpList nextElement;

    const GPRReg butterfly = hasStart ? characters : cursor;
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), butterfly);
    if (hasStart)
        jit.getEffectiveAddress(BaseIndex(butterfly, cursor, CCallHelpers::TimesEight), cursor);
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), end);
    jit.and32(TrustedImm32(IndexingShapeMask), end);
    Jump isNotContiguous = jit.branch32(CCallHelpers::NotEqual, end, TrustedImm32(ContiguousShape));
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), end);
    jit.getEffectiveAddress(BaseIndex(butterfly, end, CCallHelpers::TimesEight, 1 - static_cast<int32_t>(sizeof(EncodedJSValue))), end);
    notFound.append(jit.branchPtr(CCallHelpers::AboveOrEqual, cursor, end));

    auto loadCharactersAndFlagsOfFlatSearchString = [&] {
        jit.load32(Address(searchCharacters, StringImpl::flagsOffset()), flags);
        jit.loadPtr(Address(searchCharacters, StringImpl::dataOffset()), searchCharacters);
    };
    auto packWidthAndByteSize = [&] {
        jit.and32(TrustedImm32(is8BitFlag), flags);
        jit.or64(flags, end);
        jit.move(lengthAndByteSize, byteSize);
        Jump searchStringIs8Bit = jit.branchTest32(CCallHelpers::NonZero, flags);
        jit.lshift64(TrustedImm32(1), byteSize);
        searchStringIs8Bit.link(&jit);
        jit.lshift64(TrustedImm32(32), byteSize);
        jit.or64(byteSize, lengthAndByteSize);
    };

    Jump isOwnStorage = jit.branchTest8(CCallHelpers::Zero, Address(array, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(CopyOnWrite));
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), characters);
    jit.load32(Address(characters, JSCell::structureIDOffset() - static_cast<ptrdiff_t>(JSCellButterfly::offsetOfData())), characters);
    Jump holdsOtherValues = jit.branch32(CCallHelpers::NotEqual, characters, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.atomListStructureID)));
    Jump hasTable;
    if (!hasStart)
        hasTable = jit.branchTest8(CCallHelpers::NonZero, Address(cursor, JSCell::typeInfoFlagsOffset() - static_cast<ptrdiff_t>(JSCellButterfly::offsetOfData())), TrustedImm32(TypeInfoPerCellBit));
    jit.loadPtr(Address(searchString, JSString::offsetOfValue()), searchCharacters);
    slowPath.append(jit.branchIfRopeStringImpl(searchCharacters));
    slowPath.append(jit.branchTest32(CCallHelpers::Zero, Address(searchCharacters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsAtom())));
    isOwnStorage.link(&jit);
    holdsOtherValues.link(&jit);
    jit.loadPtr(Address(searchString, JSString::offsetOfValue()), searchCharacters);
    Jump searchStringIsRope = jit.branchIfRopeStringImpl(searchCharacters);
    jit.load32(Address(searchCharacters, StringImpl::flagsOffset()), flags);
    Jump searchStringIsNoAtom = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(StringImpl::flagIsAtom()));

    static_assert(JSCell::typeInfoFlagsOffset() == JSCell::typeInfoTypeOffset() + 1);
    constexpr int32_t flagInTypeAndFlags = ConstantCellWords::flagOfStringConstant << 8;
    constexpr int32_t typeAndFlagsOfAtom = StringType | (TypeInfo(StringType, JSString::StructureFlags).inlineTypeFlags() | TypeInfoPerCellBit) << 8 | flagInTypeAndFlags;
    const GPRReg typeAndFlags = byteSize;
    JumpList isNoCell;
    Label seekAtom = jit.label();
    jit.load64(Address(cursor), element);
    isNoCell.append(jit.branchIfNotCell(element));
    isNoCell.append(jit.branchTest64(CCallHelpers::Zero, element));
    jit.load16(Address(element, JSCell::typeInfoTypeOffset()), typeAndFlags);
    jit.or32(TrustedImm32(flagInTypeAndFlags), typeAndFlags);
    Jump cellDoesNotSay = jit.branch32(CCallHelpers::NotEqual, typeAndFlags, TrustedImm32(typeAndFlagsOfAtom));
    found.append(jit.branchPtr(CCallHelpers::Equal, Address(element, JSString::offsetOfValue()), searchCharacters));
    isNoCell.link(&jit);
    Label passOver = jit.label();
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), cursor);
    jit.branchPtr(CCallHelpers::Below, cursor, end).linkTo(seekAtom, &jit);
    returnResult(jit, TrustedImm64(-1));

    cellDoesNotSay.link(&jit);
    jit.branchIfNotType(element, StringType).linkTo(passOver, &jit);
    jit.loadPtr(Address(element, JSString::offsetOfValue()), characters);
    found.append(jit.branchPtr(CCallHelpers::Equal, characters, searchCharacters));
    Jump elementSoughtIsRope = jit.branchIfRopeStringImpl(characters);
    jit.branchTest32(CCallHelpers::NonZero, Address(characters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsAtom())).linkTo(passOver, &jit);
    JumpList seeksByLength;
    seeksByLength.append(jit.jump());

    searchStringIsNoAtom.link(&jit);
    seeksByLength.append(jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(flagIsInImage)));
    JumpList isAnother;
    Label seekTextOfImage = jit.label();
    jit.load64(Address(cursor), element);
    isAnother.append(jit.branchIfNotCell(element));
    isAnother.append(jit.branchTest64(CCallHelpers::Zero, element));
    isAnother.append(jit.branchIfNotType(element, StringType));
    jit.loadPtr(Address(element, JSString::offsetOfValue()), characters);
    found.append(jit.branchPtr(CCallHelpers::Equal, characters, searchCharacters));
    seeksByLength.append(jit.branchIfRopeStringImpl(characters));
    seeksByLength.append(jit.branchTest32(CCallHelpers::Zero, Address(characters, StringImpl::flagsOffset()), TrustedImm32(flagIsInImage)));
    isAnother.link(&jit);
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), cursor);
    jit.branchPtr(CCallHelpers::Below, cursor, end).linkTo(seekTextOfImage, &jit);
    returnResult(jit, TrustedImm64(-1));

    elementSoughtIsRope.link(&jit);
    seeksByLength.link(&jit);
    jit.load32(Address(searchCharacters, StringImpl::lengthMemoryOffset()), lengthAndByteSize);
    slowPath.append(jit.branchTest32(CCallHelpers::Zero, lengthAndByteSize));

    JumpList hasOtherLength;
    Label compareLengths = jit.label();
    jit.load64(Address(cursor), element);
    hasOtherLength.append(jit.branchIfNotCell(element));
    hasOtherLength.append(jit.branchTest64(CCallHelpers::Zero, element));
    hasOtherLength.append(jit.branchIfNotType(element, StringType));
    jit.loadPtr(Address(element, JSString::offsetOfValue()), characters);
    Jump firstRope = jit.branchIfRopeStringImpl(characters);
    Jump firstOfSameLength = jit.branch32(CCallHelpers::Equal, Address(characters, StringImpl::lengthMemoryOffset()), lengthAndByteSize);
    hasOtherLength.link(&jit);
    Label skipElement = jit.label();
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), cursor);
    jit.branchPtr(CCallHelpers::Below, cursor, end).linkTo(compareLengths, &jit);

    notFound.link(&jit);
    Label returnNotFound = jit.label();
    returnResult(jit, TrustedImm64(-1));

    isNotContiguous.link(&jit);
    static_assert(UndecidedShape < ContiguousShape && Int32Shape < ContiguousShape && DoubleShape < ContiguousShape && ArrayStorageShape > ContiguousShape);
    jit.branch32(CCallHelpers::Below, end, TrustedImm32(ContiguousShape)).linkTo(returnNotFound, &jit);
    slowPath.append(jit.jump());

    firstOfSameLength.link(&jit);
    found.append(jit.branch64(CCallHelpers::Equal, element, searchString));
    slowPath.append(jit.branchIfRopeStringImpl(searchCharacters));
    loadCharactersAndFlagsOfFlatSearchString();
    packWidthAndByteSize();
    Jump compareFirstOfSameLength = jit.jump();

    searchStringIsRope.link(&jit);
    jit.load32(Address(searchString, JSRopeString::offsetOfLength()), lengthAndByteSize);
    JumpList searchStringIsNoSubstring;
    loadSubstringCharactersAndFlags(jit, searchString, searchCharacters, byteSize, flags, searchStringIsNoSubstring);
    Jump hasSearchCharacters = jit.jump();
    searchStringIsNoSubstring.linkTo(compareLengths, &jit);

    firstRope.link(&jit);
    jit.branch32(CCallHelpers::NotEqual, Address(element, JSRopeString::offsetOfLength()), lengthAndByteSize).linkTo(skipElement, &jit);
    found.append(jit.branch64(CCallHelpers::Equal, element, searchString));
    slowPath.append(jit.branchIfRopeStringImpl(searchCharacters));
    loadCharactersAndFlagsOfFlatSearchString();
    hasSearchCharacters.link(&jit);
    packWidthAndByteSize();

    Label loop = jit.label();
    jit.load64(Address(cursor), element);
    nextElement.append(jit.branchIfNotCell(element));
    nextElement.append(jit.branchTest64(CCallHelpers::Zero, element));
    nextElement.append(jit.branchIfNotType(element, StringType));
    jit.loadPtr(Address(element, JSString::offsetOfValue()), characters);
    Jump elementIsRope = jit.branchIfRopeStringImpl(characters);
    nextElement.append(jit.branch32(CCallHelpers::NotEqual, Address(characters, StringImpl::lengthMemoryOffset()), lengthAndByteSize));
    found.append(jit.branch64(CCallHelpers::Equal, element, searchString));
    compareFirstOfSameLength.link(&jit);
    jit.load32(Address(characters, StringImpl::flagsOffset()), flags);
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);

    Label compareCharacters = jit.label();
    jit.xor32(end, flags);
    Jump widthsDiffer = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(is8BitFlag));
    jit.load8(Address(characters), word);
    jit.load8(Address(searchCharacters), byteSize);
    nextElement.append(jit.branch32(CCallHelpers::NotEqual, word, byteSize));
    jit.urshift64(lengthAndByteSize, TrustedImm32(32), byteSize);
    Jump shorterThanWord = jit.branch32(CCallHelpers::Below, byteSize, TrustedImm32(wordSize));
    Label wordLoop = jit.label();
    jit.load64(BaseIndex(characters, byteSize, CCallHelpers::TimesOne, -wordSize), word);
    nextElement.append(jit.branch64(CCallHelpers::NotEqual, BaseIndex(searchCharacters, byteSize, CCallHelpers::TimesOne, -wordSize), word));
    jit.sub32(TrustedImm32(wordSize), byteSize);
    jit.branch32(CCallHelpers::AboveOrEqual, byteSize, TrustedImm32(wordSize)).linkTo(wordLoop, &jit);
    found.append(jit.branchTest32(CCallHelpers::Zero, byteSize));
    jit.load64(Address(characters), word);
    found.append(jit.branch64(CCallHelpers::Equal, Address(searchCharacters), word));

    nextElement.link(&jit);
    Label advance = jit.label();
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), cursor);
    jit.branchPtr(CCallHelpers::Below, cursor, end).linkTo(loop, &jit);
    returnResult(jit, TrustedImm64(-1));

    auto convertCursorToIndex = [&] {
        jit.loadPtr(Address(array, JSObject::butterflyOffset()), characters);
        jit.subPtr(characters, cursor);
        jit.urshiftPtr(TrustedImm32(3), cursor);
    };
    found.link(&jit);
    Label returnIndex = jit.label();
    convertCursorToIndex();
    returnResult(jit, cursor);

    elementIsRope.link(&jit);
    jit.branch32(CCallHelpers::NotEqual, Address(element, JSRopeString::offsetOfLength()), lengthAndByteSize).linkTo(advance, &jit);
    loadSubstringCharactersAndFlags(jit, element, characters, byteSize, flags, slowPath);
    jit.jump().linkTo(compareCharacters, &jit);

    shorterThanWord.link(&jit);
    for (int32_t width : { 4, 2 }) {
        Jump shorter = jit.branch32(CCallHelpers::Below, byteSize, TrustedImm32(width));
        auto load = [&](auto address, GPRReg destination) {
            if (width == 4)
                jit.load32(address, destination);
            else
                jit.load16Unaligned(address, destination);
        };
        load(BaseIndex(characters, byteSize, CCallHelpers::TimesOne, -width), word);
        load(Address(characters), characters);
        load(BaseIndex(searchCharacters, byteSize, CCallHelpers::TimesOne, -width), byteSize);
        jit.branch32(CCallHelpers::NotEqual, word, byteSize).linkTo(advance, &jit);
        load(Address(searchCharacters), byteSize);
        jit.branch32(CCallHelpers::NotEqual, characters, byteSize).linkTo(advance, &jit);
        jit.jump().linkTo(returnIndex, &jit);
        shorter.link(&jit);
    }
    jit.branchTest32(CCallHelpers::Zero, byteSize).linkTo(returnIndex, &jit);
    jit.load8(Address(characters), word);
    jit.load8(Address(searchCharacters), byteSize);
    jit.branch32(CCallHelpers::NotEqual, word, byteSize).linkTo(advance, &jit);
    jit.jump().linkTo(returnIndex, &jit);

    widthsDiffer.link(&jit);
    const GPRReg length = byteSize;
    jit.zeroExtend32ToWord(lengthAndByteSize, length);
    jit.branchTest32(CCallHelpers::Zero, length).linkTo(returnIndex, &jit);
    jit.move64ToDouble(end, savedEnd);
    JumpList mismatch;
    auto compareLatin1WithUTF16 = [&](GPRReg latin1Characters, GPRReg utf16Characters) {
        Label characterLoop = jit.label();
        jit.load8(BaseIndex(latin1Characters, length, CCallHelpers::TimesOne, -1), word);
        jit.load16(BaseIndex(utf16Characters, length, CCallHelpers::TimesTwo, -2), end);
        mismatch.append(jit.branch32(CCallHelpers::NotEqual, word, end));
        jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(1), length).linkTo(characterLoop, &jit);
        jit.jump().linkTo(returnIndex, &jit);
    };
    Jump searchStringIsLatin1 = jit.branchTest32(CCallHelpers::NonZero, end, TrustedImm32(is8BitFlag));
    compareLatin1WithUTF16(characters, searchCharacters);
    searchStringIsLatin1.link(&jit);
    compareLatin1WithUTF16(searchCharacters, characters);
    mismatch.link(&jit);
    jit.moveDoubleTo64(savedEnd, end);
    jit.jump().linkTo(advance, &jit);

    if (!hasStart) {
        const GPRReg slot = end;
        constexpr int32_t offsetOfSlots = sizeof(EncodedJSValue) - 1;
        const GPRReg searchLength = lengthAndByteSize;
        const GPRReg hash = byteSize;
        hasTable.link(&jit);
        jit.loadPtr(Address(searchString, JSString::offsetOfValue()), searchCharacters);
        Jump searchesForRope = jit.branchIfRopeStringImpl(searchCharacters);
        jit.load32(Address(searchCharacters, StringImpl::lengthMemoryOffset()), searchLength);
        loadCharactersAndFlagsOfFlatSearchString();
        Label hasCharacters = jit.label();
        slowPath.append(jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(is8BitFlag)));
        jit.branchTest32(CCallHelpers::Zero, searchLength).linkTo(returnNotFound, &jit);

        static_assert(AtomListTable::factorOfFirst == 5 && AtomListTable::factorOfMiddle == 3 && AtomListTable::factorOfLast == 9 && AtomListTable::numberOfHashes == 256);
        jit.load8(Address(searchCharacters), word);
        jit.getEffectiveAddress(BaseIndex(word, word, CCallHelpers::TimesFour), hash);
        jit.load8(BaseIndex(searchCharacters, searchLength, CCallHelpers::TimesOne, -1), word);
        jit.getEffectiveAddress(BaseIndex(word, word, CCallHelpers::TimesEight), word);
        jit.add32(word, hash);
        jit.move(searchLength, word);
        jit.urshift32(TrustedImm32(1), word);
        jit.load8(BaseIndex(searchCharacters, word, CCallHelpers::TimesOne), word);
        jit.getEffectiveAddress(BaseIndex(word, word, CCallHelpers::TimesTwo), word);
        jit.add32(word, hash);
        jit.add32(searchLength, hash);
        jit.and32(TrustedImm32(AtomListTable::numberOfHashes - 1), hash);
        jit.addPtr(hash, slot);

        JumpList otherWord;
        JumpList foundInTable;
        Label probe = jit.label();
        jit.load8(Address(slot, offsetOfSlots), byteSize);
        jit.branchTest32(CCallHelpers::Zero, byteSize).linkTo(returnNotFound, &jit);
        jit.load64(BaseIndex(cursor, byteSize, CCallHelpers::TimesEight, -static_cast<int32_t>(sizeof(EncodedJSValue))), element);
        jit.loadPtr(Address(element, JSString::offsetOfValue()), characters);
        otherWord.append(jit.branch32(CCallHelpers::NotEqual, Address(characters, StringImpl::lengthMemoryOffset()), searchLength));
        jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
        Jump isShort = jit.branch32(CCallHelpers::Below, searchLength, TrustedImm32(wordSize));
        jit.move(searchLength, byteSize);
        Label compareWords = jit.label();
        jit.load64(BaseIndex(characters, byteSize, CCallHelpers::TimesOne, -wordSize), word);
        otherWord.append(jit.branch64(CCallHelpers::NotEqual, BaseIndex(searchCharacters, byteSize, CCallHelpers::TimesOne, -wordSize), word));
        jit.sub32(TrustedImm32(wordSize), byteSize);
        jit.branch32(CCallHelpers::AboveOrEqual, byteSize, TrustedImm32(wordSize)).linkTo(compareWords, &jit);
        jit.load64(Address(characters), word);
        foundInTable.append(jit.branch64(CCallHelpers::Equal, Address(searchCharacters), word));

        otherWord.link(&jit);
        Label nextSlot = jit.label();
        jit.addPtr(TrustedImm32(1), slot);
        jit.jump().linkTo(probe, &jit);

        isShort.link(&jit);
        Jump isShorter = jit.branch32(CCallHelpers::Below, searchLength, TrustedImm32(4));
        jit.load32(BaseIndex(characters, searchLength, CCallHelpers::TimesOne, -4), word);
        jit.load32(BaseIndex(searchCharacters, searchLength, CCallHelpers::TimesOne, -4), byteSize);
        jit.branch32(CCallHelpers::NotEqual, word, byteSize).linkTo(nextSlot, &jit);
        jit.load32(Address(characters), word);
        foundInTable.append(jit.branch32(CCallHelpers::Equal, Address(searchCharacters), word));
        jit.jump().linkTo(nextSlot, &jit);

        isShorter.link(&jit);
        jit.load8(Address(characters), word);
        jit.load8(Address(searchCharacters), byteSize);
        jit.branch32(CCallHelpers::NotEqual, word, byteSize).linkTo(nextSlot, &jit);
        jit.load8(BaseIndex(characters, searchLength, CCallHelpers::TimesOne, -1), word);
        jit.load8(BaseIndex(searchCharacters, searchLength, CCallHelpers::TimesOne, -1), byteSize);
        jit.branch32(CCallHelpers::NotEqual, word, byteSize).linkTo(nextSlot, &jit);
        jit.move(searchLength, byteSize);
        jit.urshift32(TrustedImm32(1), byteSize);
        jit.load8(BaseIndex(characters, byteSize, CCallHelpers::TimesOne), word);
        jit.load8(BaseIndex(searchCharacters, byteSize, CCallHelpers::TimesOne), byteSize);
        jit.branch32(CCallHelpers::NotEqual, word, byteSize).linkTo(nextSlot, &jit);

        foundInTable.link(&jit);
        jit.load8(Address(slot, offsetOfSlots), cursor);
        jit.sub32(TrustedImm32(1), cursor);
        returnResult(jit, cursor);

        searchesForRope.link(&jit);
        jit.load32(Address(searchString, JSRopeString::offsetOfLength()), searchLength);
        loadSubstringCharactersAndFlags(jit, searchString, searchCharacters, byteSize, flags, slowPath);
        jit.jump().linkTo(hasCharacters, &jit);
    }

    slowPath.link(&jit);
    convertCursorToIndex();
    jit.callOperationAndReturn(Entry::operationAOTArrayIndexOfString, { });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
