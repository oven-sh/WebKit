/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArrays.h"

#if ENABLE(AOT)

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
#include "JSObject.h"
#include "JSString.h"
#include "SparseArrayValueMap.h"
#include "Structure.h"
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2));

enum class ElementQuestion : bool { Get, Has };

static StubKey stubAtIndexFor(ElementQuestion question)
{
    return question == ElementQuestion::Get ? arrayGetAtIndex() : arrayHasAtIndex();
}

static void callOperationAndReturn(StubAssembler& jit, ElementQuestion question)
{
    Entry operation = question == ElementQuestion::Get ? Entry::operationAOTGetByVal : Entry::operationAOTInByValAndCache;
    jit.callOperationAndReturn(operation, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters });
}

// The search of WTF::HashTable::inlineLookup().
void emitFindSparseElement(StubAssembler& jit, const SparseElementSearch& search, JumpList& isAbsent)
{
    const auto [owner, index, entry, probe, table] = search;
    constexpr unsigned entrySizeShift = 4;
    static_assert(sizeof(SparseArrayEntry) == 1 << entrySizeShift);
    const Address attributes(entry, SparseArrayEntry::offsetOfAttributes());

    emitHashOfBits(jit, index, entry, probe, table);
    jit.loadPtr(Address(owner, JSObject::butterflyOffset()), table);
    jit.loadPtr(Address(table, ArrayStorage::sparseMapOffset()), table);
    isAbsent.append(jit.branchTestPtr(CCallHelpers::Zero, table));
    jit.loadPtr(Address(table, SparseArrayValueMap::offsetOfTable()), table);
    isAbsent.append(jit.branchTestPtr(CCallHelpers::Zero, table));
    jit.move(TrustedImm32(0), probe);

    Label hasBucketNumber = jit.label();
    jit.and32(Address(table, SparseArrayValueMap::offsetOfTableSizeMask()), entry);
    jit.lshift64(TrustedImm32(entrySizeShift), entry);
    jit.add64(table, entry);
    isAbsent.append(jit.branchTest32(CCallHelpers::NonZero, attributes, TrustedImm32(static_cast<int32_t>(SparseArrayEntry::isEmptyBucketFlag))));
    Jump hasOtherIndex = jit.branch32(CCallHelpers::NotEqual, Address(entry, SparseArrayEntry::offsetOfIndex()), index);
    Jump isFound = jit.branchTest32(CCallHelpers::Zero, attributes, TrustedImm32(SparseArrayEntry::isDeletedBucketFlag));

    hasOtherIndex.link(&jit);
    jit.sub64(table, entry);
    jit.urshift64(TrustedImm32(entrySizeShift), entry);
    jit.add32(TrustedImm32(1), probe);
    jit.add32(probe, entry);
    jit.jump().linkTo(hasBucketNumber, &jit);

    isFound.link(&jit);
}

// Nothing stands under an index on the prototypes of the array, so an element that is not there reads as undefined and is in nothing. A position that is no index is a name.
static void answerAtIndex(StubAssembler& jit, ElementQuestion question)
{
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
    auto returnTrue = [&] {
        jit.move(TrustedImm32(1), result);
        jit.returnToCaller();
    };

    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), butterfly);
    Jump isContiguousShape = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
    Jump isInt32Shape = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(Int32Shape));
    Jump isDoubleShape = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
    isAbsent.append(jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(UndecidedShape)));
    static_assert(SlowPutArrayStorageShape > ArrayStorageShape && SlowPutArrayStorageShape == IndexingShapeMask - 2);
    callsOperation.append(jit.branch32(CCallHelpers::Below, shape, TrustedImm32(ArrayStorageShape)));
    callsOperation.append(jit.branch32(CCallHelpers::Above, shape, TrustedImm32(SlowPutArrayStorageShape)));
    jit.move(TrustedImm64(MAX_ARRAY_INDEX), result);
    callsOperation.append(jit.branch64(CCallHelpers::Above, position, result));
    Jump isOutsideVector = jit.branch32(CCallHelpers::AboveOrEqual, position, Address(butterfly, ArrayStorage::vectorLengthOffset()));
    jit.load64(BaseIndex(butterfly, position, CCallHelpers::TimesEight, ArrayStorage::vectorOffset()), result);
    Jump isHoleInVector = jit.branchTest64(CCallHelpers::Zero, result);
    if (question == ElementQuestion::Has)
        returnTrue();
    else
        jit.returnToCaller();

    isOutsideVector.link(&jit);
    isHoleInVector.link(&jit);
    const GPRReg index = shape;
    const GPRReg entry = result;
    jit.move(position, index);
    emitFindSparseElement(jit, { array, index, entry, position, butterfly }, isAbsentAtIndex);
    if (question == ElementQuestion::Has)
        returnTrue();
    else {
        Jump isAccessor = jit.branchTest32(CCallHelpers::NonZero, Address(entry, SparseArrayEntry::offsetOfAttributes()), TrustedImm32(static_cast<int32_t>(PropertyAttribute::Accessor)));
        jit.load64(Address(entry, SparseArrayEntry::offsetOfValue()), result);
        jit.returnToCaller();

        isAccessor.link(&jit);
        jit.move(index, position);
        callsOperation.append(jit.jump());
    }

    isContiguousShape.link(&jit);
    isInt32Shape.link(&jit);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), result);
    isAbsent.append(jit.branch64(CCallHelpers::AboveOrEqual, position, result));
    jit.load64(BaseIndex(butterfly, position, CCallHelpers::TimesEight), result);
    isAbsent.append(jit.branchTest64(CCallHelpers::Zero, result));
    if (question == ElementQuestion::Has)
        returnTrue();
    else
        jit.returnToCaller();

    isDoubleShape.link(&jit);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), result);
    isAbsent.append(jit.branch64(CCallHelpers::AboveOrEqual, position, result));
    jit.loadDouble(BaseIndex(butterfly, position, CCallHelpers::TimesEight), number);
    PureDouble element = PureDouble::branchIfNaN(jit, number, isAbsent);
    if (question == ElementQuestion::Has)
        returnTrue();
    else {
        JumpList isNotInt32;
        branchIfNotExactInt32(jit, number, result, isNotInt32, numberScratch);
        boxExactInt32(jit, result, result);
        jit.returnToCaller();
        isNotInt32.link(&jit);
        boxDouble(jit, element, result);
        jit.returnToCaller();
    }

    isAbsent.link(&jit);
    jit.move(TrustedImm64(MAX_ARRAY_INDEX), shape);
    Jump isName = jit.branch64(CCallHelpers::Above, position, shape);
    isAbsentAtIndex.link(&jit);
    Label isFoundNowhere = jit.label();
    jit.move(question == ElementQuestion::Get ? TrustedImm64(JSValue::ValueUndefined) : TrustedImm64(0), result);
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
    callOperationAndReturn(jit, question);
}

static void answerAtNumber(StubAssembler& jit, ElementQuestion question)
{
    const FPRReg position = jit.operandFPR(1);
    const GPRReg index = jit.allocateScratch(StubRegister::A2);
    const FPRReg converted = jit.allocateScratch(FPRInfo::argumentFPR1);

    jit.truncateDoubleToInt64(position, index);
    jit.convertInt64ToDouble(index, converted);
    Jump isNotInteger = jit.branchDouble(CCallHelpers::DoubleNotEqualOrUnordered, position, converted);
    checkStubOperands(stubAtIndexFor(question), { jit.operand(0), index });
    jit.jumpToStub(stubAtIndexFor(question));

    isNotInteger.link(&jit);
    boxDouble(jit, PureDouble::purify(jit, position, converted), index);
    callOperationAndReturn(jit, question);
}

static StubSignature signatureAtIndex()
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

static StubSignature signatureAtNumber()
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), FPRInfo::argumentFPR0 });
}

StubSignature signatureOfArrayGetAtIndex(StubKey) { return signatureAtIndex(); }
StubSignature signatureOfArrayHasAtIndex(StubKey) { return signatureAtIndex(); }
StubSignature signatureOfArrayGetAtNumber(StubKey) { return signatureAtNumber(); }
StubSignature signatureOfArrayHasAtNumber(StubKey) { return signatureAtNumber(); }

void generateArrayGetAtIndex(StubAssembler& jit) { answerAtIndex(jit, ElementQuestion::Get); }
void generateArrayHasAtIndex(StubAssembler& jit) { answerAtIndex(jit, ElementQuestion::Has); }
void generateArrayGetAtNumber(StubAssembler& jit) { answerAtNumber(jit, ElementQuestion::Get); }
void generateArrayHasAtNumber(StubAssembler& jit) { answerAtNumber(jit, ElementQuestion::Has); }

static bool searchesFromStart(StubKey key)
{
    SearchedElements elements = static_cast<SearchedElements>(key.form());
    return elements == SearchedElements::ValuesFromStart || elements == SearchedElements::StringsFromStart;
}

StubSignature signatureOfArrayIndexOfString(StubKey key)
{
    if (searchesFromStart(key))
        return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) });
    return signatureAtIndex();
}

// characters holds the fiber word of a rope on entry. flags may alias string.
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
    const SearchedElements searchedElements = static_cast<SearchedElements>(jit.key().form());
    const bool elementsAreStrings = searchedElements == SearchedElements::Strings || searchedElements == SearchedElements::StringsFromStart;
    const bool hasStart = searchesFromStart(jit.key());
    const GPRReg array = jit.operand(0);
    const GPRReg searchString = jit.operand(1);
    const GPRReg cursor = hasStart ? jit.clobberOperand(2) : jit.allocateScratch(StubRegister::A3);
    // Address of the last element, plus 1, plus is8BitFlag if searchString is 8-bit.
    const GPRReg end = jit.allocateScratch(StubRegister::A4);
    // Length of searchString in the low 32 bits, its size in bytes in the high 32 bits.
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

    jit.loadPtr(Address(searchString, JSString::offsetOfValue()), searchCharacters);
    Jump searchStringIsRope = jit.branchIfRopeStringImpl(searchCharacters);
    jit.load32(Address(searchCharacters, StringImpl::lengthMemoryOffset()), lengthAndByteSize);

    JumpList hasOtherLength;
    Label compareLengths = jit.label();
    jit.load64(Address(cursor), element);
    if (!elementsAreStrings) {
        hasOtherLength.append(jit.branchIfNotCell(element));
        hasOtherLength.append(jit.branchTest64(CCallHelpers::Zero, element));
        hasOtherLength.append(jit.branchIfNotType(element, StringType));
    }
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
    loadCharactersAndFlagsOfFlatSearchString();
    packWidthAndByteSize();
    Jump compareFirstOfSameLength = jit.jump();

    searchStringIsRope.link(&jit);
    jit.load32(Address(searchString, JSRopeString::offsetOfLength()), lengthAndByteSize);
    loadSubstringCharactersAndFlags(jit, searchString, searchCharacters, byteSize, flags, slowPath);
    Jump hasSearchCharacters = jit.jump();

    firstRope.link(&jit);
    jit.branch32(CCallHelpers::NotEqual, Address(element, JSRopeString::offsetOfLength()), lengthAndByteSize).linkTo(skipElement, &jit);
    loadCharactersAndFlagsOfFlatSearchString();
    hasSearchCharacters.link(&jit);
    packWidthAndByteSize();

    Label loop = jit.label();
    jit.load64(Address(cursor), element);
    found.append(jit.branch64(CCallHelpers::Equal, element, searchString));
    if (!elementsAreStrings) {
        nextElement.append(jit.branchIfNotCell(element));
        nextElement.append(jit.branchTest64(CCallHelpers::Zero, element));
        nextElement.append(jit.branchIfNotType(element, StringType));
    }
    jit.loadPtr(Address(element, JSString::offsetOfValue()), characters);
    Jump elementIsRope = jit.branchIfRopeStringImpl(characters);
    nextElement.append(jit.branch32(CCallHelpers::NotEqual, Address(characters, StringImpl::lengthMemoryOffset()), lengthAndByteSize));
    compareFirstOfSameLength.link(&jit);
    jit.load32(Address(characters, StringImpl::flagsOffset()), flags);
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);

    Label compareCharacters = jit.label();
    jit.xor32(end, flags);
    Jump widthsDiffer = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(is8BitFlag));
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

    slowPath.link(&jit);
    convertCursorToIndex();
    jit.callOperationAndReturn(Entry::operationAOTArrayIndexOfString, { });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
