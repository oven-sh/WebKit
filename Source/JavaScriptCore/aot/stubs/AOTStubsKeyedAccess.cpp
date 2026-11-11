/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsKeyed.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTClosureWord.h"
#include "AOTCompiledStructure.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeType.h"
#include "AOTStubRegisters.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsElements.h"
#include "AOTStubsField.h"
#include "AOTStubsPropertyAccessors.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "AOTStubsPropertyProbes.h"
#include "AOTStubsStrings.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSArrayBufferView.h"
#include "JSCJSValue.h"
#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "JSGlobalProxy.h"
#include "JSObject.h"
#include "JSString.h"
#include "StackAlignment.h"
#include <array>
#include <bit>
#include <optional>

namespace JSC { namespace AOT {

static_assert(!JSValue::ValueEmpty && !static_cast<size_t>(LeafAnswer::TakeSlowPath) && !static_cast<size_t>(LeafPresence::TakeSlowPath));
static_assert(LeafPresence::Absent < LeafPresence::Inherited && LeafPresence::Inherited < LeafPresence::Own);

namespace {

struct KeyedProbeRegisters {
    GPRReg base;
    GPRReg key;
    GPRReg uid;
    GPRReg structureID;
    GPRReg entry;
    GPRReg header;
    GPRReg payload;
};

} // namespace

static StubSignature signatureOfKeyedOperands(unsigned numberOfOperands, StubResult result)
{
    constexpr std::array operands { firstStubOperandRegister, keyCellRegister, storedValueByValRegister };
    RELEASE_ASSERT(numberOfOperands <= operands.size());
    StubSignature signature;
    for (unsigned index = 0; index < numberOfOperands; ++index)
        signature.addOperand(gprOf(operands[index]));
    signature.result = result;
    if (result == StubResult::GPR)
        signature.firstResult = gprOf(firstStubOperandRegister);
    return signature;
}

StubSignature signatureOfGetByValCached(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfGetByValCachedCells(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfGetByValCachedAtPlace(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfGetByValCachedBehindPlaces(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfGetByValCachedKeepingRegisters(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfGetByValCachedOrOwnAccessor(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfGetByValRunningNoCode(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfGetByValCachedCellsKeepingRegisters(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfGetByValCachedAtPlaceKeepingRegisters(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfGetByValCachedBehindPlacesKeepingRegisters(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfGetByValOfKeyOfPair(StubKey) { return signatureOfKeyedOperands(3, StubResult::GPR); }
StubSignature signatureOfGetSuperByVal(StubKey) { return signatureOfKeyedOperands(3, StubResult::GPR); }
StubSignature signatureOfInByValCached(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfDeleteByIdCached(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfDeleteByValCached(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
StubSignature signatureOfPutByValCached(StubKey) { return signatureOfKeyedOperands(3, StubResult::None); }
StubSignature signatureOfDefineByValCached(StubKey) { return signatureOfKeyedOperands(3, StubResult::None); }
StubSignature signatureOfToPropertyKey(StubKey) { return signatureOfKeyedOperands(1, StubResult::GPR); }

void loadNameOfKey(StubAssembler& jit, GPRReg key, GPRReg uid, GPRReg, JumpList& hasNoName)
{
    constexpr bool keyIsString = false;
    constexpr bool keyIsSymbol = false;
    RELEASE_ASSERT(noOverlap(key, uid, instanceGPR));
    hasNoName.append(jit.loadCacheableIdentifierImpl(key, uid, keyIsString, keyIsSymbol));
}

static void emitLeafLookupCall(StubAssembler& jit, Entry entry, GPRReg base, GPRReg second, std::optional<GPRReg> value)
{
    RELEASE_ASSERT(noOverlap(base, operationArgumentGPR(2), operationArgumentGPR(3)));
    RELEASE_ASSERT(!value || second != operationArgumentGPR(3));
    if (value)
        jit.move(*value, operationArgumentGPR(3));
    jit.move(second, operationArgumentGPR(2));
    jit.move(base, operationArgumentGPR(1));
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.callOperation(entry, jit.reuseScratch(StubRegister::T11));
}

void callLeafLookup(StubAssembler& jit, Entry entry, GPRReg second, std::optional<GPRReg> value, GPRReg baseAfterCall, const RegisterSet& lostAnyway)
{
    const GPRReg base = jit.clobberOperand(0);
    RELEASE_ASSERT(base == GPRInfo::returnValueGPR);
    RELEASE_ASSERT(noOverlap(base, second, baseAfterCall) && value != base && value != baseAfterCall);

    if (descriptorOf(jit.key().family()).clobbers == StubClobbers::ClobbersOnlyScratch) {
        jit.move(base, baseAfterCall);
        jit.emitFunctionPrologue();
#if CPU(X86_64)
        jit.andPtr(TrustedImm32(-static_cast<int32_t>(stackAlignmentBytes())), CCallHelpers::stackPointerRegister);
        jit.subPtr(TrustedImm32(sizeof(CPURegister)), CCallHelpers::stackPointerRegister);
#endif
        jit.preservingRegistersClobberedBy(StubCallee::Operation, true, lostAnyway, [&](unsigned) {
            emitLeafLookupCall(jit, entry, base, second, value);
        });
        jit.emitFunctionEpilogue();
        return;
    }

    const GPRReg key = jit.operand(1);
    jit.emitFunctionPrologue();
#if CPU(ARM64)
    jit.pushPair(base, key);
    if (value)
        jit.pushPair(*value, *value);
#else
    jit.push(base);
    jit.push(key);
    if (value) {
        jit.push(*value);
        jit.push(*value);
    }
#endif
    emitLeafLookupCall(jit, entry, base, second, value);
#if CPU(ARM64)
    if (value)
        jit.popPair(*value, jit.reuseScratch(StubRegister::A3));
    jit.popPair(baseAfterCall, key);
#else
    if (value) {
        jit.pop(*value);
        jit.pop(*value);
    }
    jit.pop(key);
    jit.pop(baseAfterCall);
#endif
    jit.emitFunctionEpilogueWithEmptyFrame();
}

static KeyedProbeRegisters allocateKeyedProbeRegisters(StubAssembler& jit)
{
    return {
        .base = jit.clobberOperand(0),
        .key = jit.clobberOperand(1),
        .uid = jit.allocateScratch(StubRegister::T10),
        .structureID = jit.allocateScratch(StubRegister::T11),
        .entry = jit.allocateScratch(StubRegister::T14),
        .header = jit.allocateScratch(slotHeaderRegister),
        .payload = jit.allocateScratch(slotPayloadRegister),
    };
}

enum class ElementUnderText : bool { IsLookedFor, MayBeAdded };

static void loadIndexOfText(StubAssembler& jit, const KeyedProbeRegisters& registers, JumpList& isNoSuchKey, ElementUnderText element = ElementUnderText::IsLookedFor)
{
    const GPRReg key = registers.key;
    const GPRReg characters = registers.uid;
    const GPRReg length = registers.structureID;
    const GPRReg index = registers.entry;
    const GPRReg digit = registers.header;
    const GPRReg position = registers.payload;
    static constexpr int32_t maxDigits = 4;

    Jump hasIndexedStorage = jit.branchTest8(CCallHelpers::NonZero, Address(registers.base, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask));
    isNoSuchKey.append(branchIfNotCompiledObjectOfThisImage(jit, registers.base));
    if (element == ElementUnderText::IsLookedFor)
        isNoSuchKey.append(jit.branchTest32(CCallHelpers::Zero, Address(registers.base, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)));
    hasIndexedStorage.link(&jit);
    isNoSuchKey.append(jit.branchIfNotString(key));
    jit.loadPtr(Address(key, JSString::offsetOfValue()), characters);
    isNoSuchKey.append(jit.branchIfRopeStringImpl(characters));
    jit.load32(Address(characters, StringImpl::lengthMemoryOffset()), length);
    jit.sub32(length, TrustedImm32(1), index);
    isNoSuchKey.append(jit.branch32(CCallHelpers::AboveOrEqual, index, TrustedImm32(maxDigits)));
    isNoSuchKey.append(jit.branchTest32(CCallHelpers::Zero, Address(characters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
    jit.load8(Address(characters), index);
    jit.sub32(TrustedImm32('0'), index);
    isNoSuchKey.append(jit.branch32(CCallHelpers::Above, index, TrustedImm32(9)));
    Jump hasOneDigit = jit.branch32(CCallHelpers::Equal, length, TrustedImm32(1));
    isNoSuchKey.append(jit.branchTest32(CCallHelpers::Zero, index));
    jit.move(TrustedImm32(1), position);
    Label nextDigit = jit.label();
    jit.load8(BaseIndex(characters, position, CCallHelpers::TimesOne), digit);
    jit.sub32(TrustedImm32('0'), digit);
    isNoSuchKey.append(jit.branch32(CCallHelpers::Above, digit, TrustedImm32(9)));
    jit.mul32(TrustedImm32(10), index, index);
    jit.add32(digit, index);
    jit.add32(TrustedImm32(1), position);
    jit.branch32(CCallHelpers::Below, position, length).linkTo(nextDigit, &jit);
    hasOneDigit.link(&jit);
}

static void findNameOfText(StubAssembler& jit, GPRReg characters, GPRReg length, GPRReg first, GPRReg last, GPRReg uid, JumpList& isNotFound)
{
    RELEASE_ASSERT(noOverlap(characters, length, first, last, uid, instanceGPR));
    const GPRReg entry = uid;
    static_assert(NameOfText::widthOfWordFor(NameOfText::maxLength) == 8 && NameOfText::maxLength == 16 && NameOfText::widthOfWordFor(7) == 4 && NameOfText::widthOfWordFor(3) == 2 && NameOfText::minLength == 2);
    jit.sub32(length, TrustedImm32(NameOfText::minLength), entry);
    isNotFound.append(jit.branch32(CCallHelpers::Above, entry, TrustedImm32(NameOfText::maxLength - NameOfText::minLength)));
    Jump isBelowEight = jit.branch32(CCallHelpers::Below, length, TrustedImm32(8));
    jit.load64(Address(characters), first);
    jit.load64(BaseIndex(characters, length, CCallHelpers::TimesOne, -8), last);
    Jump hasWordsOfEight = jit.jump();

    isBelowEight.link(&jit);
    Jump isBelowFour = jit.branch32(CCallHelpers::Below, length, TrustedImm32(4));
    jit.load32(Address(characters), first);
    jit.load32(BaseIndex(characters, length, CCallHelpers::TimesOne, -4), last);
    Jump hasWordsOfFour = jit.jump();

    isBelowFour.link(&jit);
    jit.load16Unaligned(Address(characters), first);
    jit.load16Unaligned(BaseIndex(characters, length, CCallHelpers::TimesOne, -2), last);

    hasWordsOfEight.link(&jit);
    hasWordsOfFour.link(&jit);
    jit.getEffectiveAddress(BaseIndex(first, last, CCallHelpers::TimesTwo), entry);
    jit.add64(length, entry);
    jit.move(TrustedImm64(static_cast<int64_t>(NameOfText::multiplier)), characters);
    jit.mul64(characters, entry);
    jit.urshift64(TrustedImm32(NameOfText::pairShift), entry);
    jit.lshift64(TrustedImm32(NameOfText::sizeShift + 1), entry);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.namesOfTexts)), entry);
    auto branchUnlessEntryHasText = [&](int32_t offsetOfEntry, JumpList& hasOtherText) {
        hasOtherText.append(jit.branch64(CCallHelpers::NotEqual, Address(entry, offsetOfEntry + OBJECT_OFFSETOF(NameOfText, first)), first));
        hasOtherText.append(jit.branch64(CCallHelpers::NotEqual, Address(entry, offsetOfEntry + OBJECT_OFFSETOF(NameOfText, last)), last));
        hasOtherText.append(jit.branch64(CCallHelpers::NotEqual, Address(entry, offsetOfEntry + OBJECT_OFFSETOF(NameOfText, length)), length));
    };
    JumpList isNotInFront;
    branchUnlessEntryHasText(0, isNotInFront);
    jit.loadPtr(Address(entry, OBJECT_OFFSETOF(NameOfText, uid)), uid);
    Jump isFound = jit.jump();

    isNotInFront.link(&jit);
    branchUnlessEntryHasText(sizeof(NameOfText), isNotFound);
    jit.loadPtr(Address(entry, sizeof(NameOfText) + OBJECT_OFFSETOF(NameOfText, uid)), uid);
    isFound.link(&jit);
}

static constexpr int32_t offsetOfKeyedValuePlacesFor(SlotAccess access)
{
    return static_cast<int32_t>(InstanceLayout::offsetOfKeyedValuePlaces + KeyedValuePlace::offsetOfFirst(access));
}

#if CPU(X86_64)
static BaseIndex inKeyedValuePlace(GPRReg place, SlotAccess access, ptrdiff_t offset)
{
    return BaseIndex(instanceGPR, place, CCallHelpers::TimesOne, static_cast<int32_t>(offsetOfKeyedValuePlacesFor(access) + offset));
}
#else
static Address inKeyedValuePlace(GPRReg place, SlotAccess, ptrdiff_t offset)
{
    return Address(place, static_cast<int32_t>(offset));
}
#endif

static void locateKeyedValuePlace(StubAssembler& jit, SlotAccess access, GPRReg key, GPRReg structureID, GPRReg place)
{
    jit.add32(key, structureID, place);
    jit.and32(TrustedImm32(static_cast<int32_t>(KeyedValuePlace::offsetMask)), place);
#if !CPU(X86_64)
    jit.addPtr(instanceGPR, place);
    jit.addPtr(TrustedImm32(offsetOfKeyedValuePlacesFor(access)), place);
#else
    UNUSED_PARAM(access);
#endif
}

static Jump findKeyedValuePlace(StubAssembler& jit, SlotAccess access, const KeyedProbeRegisters& registers, GPRReg wordIndex, JumpList& isNotFound, std::optional<StubKey> entryAtPlace = std::nullopt)
{
    RELEASE_ASSERT(wordIndex != registers.entry);
    jit.load32(Address(registers.base, JSCell::structureIDOffset()), registers.structureID);
    locateKeyedValuePlace(jit, access, registers.key, registers.structureID, registers.entry);
    isNotFound.append(jit.branchPtr(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, key)), registers.key));
    if (entryAtPlace)
        jit.bindEntry(*entryAtPlace);
    Jump isOfNoValue = jit.branch32(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), registers.structureID);
    jit.load32SignedExtendTo64(inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, wordIndex)), wordIndex);
    return isOfNoValue;
}

static void branchUnlessPlaceSaysAbsent(StubAssembler& jit, const KeyedProbeRegisters& registers, GPRReg scratch, JumpList& doesNotSay)
{
    RELEASE_ASSERT(noOverlap(scratch, registers.base, registers.key, registers.structureID, registers.entry));
    jit.add32(TrustedImm32(KeyedValuePlace::absenceBit), registers.structureID, scratch);
    doesNotSay.append(jit.branch32(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), scratch));
}

static FieldAddressRegisters findFieldByPlace(StubAssembler& jit, const KeyedProbeRegisters& registers, JumpList& doesNotSay, JumpList& isNotInCell, JumpList* returnsEncodedValueUnlessAbsent = nullptr)
{
    const FieldAddressRegisters field {
        .object = registers.base,
        .slotData = registers.payload,
        .extensionWordOffset = InvalidGPRReg,
        .address = registers.header,
        .representation = registers.entry,
    };
    jit.add32(TrustedImm32(KeyedValuePlace::fieldBit), registers.structureID, registers.header);
    doesNotSay.append(jit.branch32(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), registers.header));
    jit.load32(inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, slotData)), field.slotData);
    if (returnsEncodedValueUnlessAbsent) {
        static_assert(!traitsOf(SlotRepresentation::JSValue).absentBits && !traitsOf(SlotRepresentation::Cell).absentBits);
        Jump holdsAnotherForm = jit.branch32(CCallHelpers::Above, field.slotData, TrustedImm32(static_cast<int32_t>(SlotData::create(SlotData::maxOffset, representationsReadAsEncodedValues.back()).encoded())));
        jit.and32(TrustedImm32(SlotData::maxOffset), field.slotData);
        jit.load64(BaseIndex(registers.base, field.slotData, CCallHelpers::TimesOne), registers.uid);
        returnsEncodedValueUnlessAbsent->append(jit.branchTest64(CCallHelpers::Zero, registers.uid));
        jit.move(registers.uid, registers.base);
        jit.returnToCaller();
        holdsAnotherForm.link(&jit);
    }
    computeFieldAddress(jit, field, false, isNotInCell);
    return field;
}

static void rememberFieldOfKey(StubAssembler& jit, const KeyedProbeRegisters& registers, const ProbedSlot& slot)
{
    Jump isOutOfLine = CachedSlot(slot.form, Slot::Kind::Field, slot.header, slot.payload).branchIfFlag(jit, Slot::isOutOfLine);
    locateKeyedValuePlace(jit, SlotAccess::Read, registers.key, registers.structureID, registers.entry);
    jit.storePtr(registers.key, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, key)));
    jit.store32(slot.payload, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, slotData)));
    jit.add32(TrustedImm32(KeyedValuePlace::fieldBit), registers.structureID);
    jit.store32(registers.structureID, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)));
    jit.sub32(TrustedImm32(KeyedValuePlace::fieldBit), registers.structureID);
    isOutOfLine.link(&jit);
}

void rememberAbsenceOfKey(StubAssembler& jit, GPRReg key, GPRReg header, GPRReg place, GPRReg scratch)
{
    RELEASE_ASSERT(noOverlap(key, header, place, scratch, instanceGPR));
    static_assert(!Slot::offsetOfStructureID());
    jit.zeroExtend32ToWord(header, scratch);
    locateKeyedValuePlace(jit, SlotAccess::Read, key, scratch, place);
    jit.storePtr(key, inKeyedValuePlace(place, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, key)));
    jit.add32(TrustedImm32(KeyedValuePlace::absenceBit), scratch);
    jit.store32(scratch, inKeyedValuePlace(place, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)));
    jit.store8(TrustedImm32(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.placesSayAbsence)));
}

void rememberOwnGetterOfKey(StubAssembler& jit, GPRReg key, GPRReg header, GPRReg wordIndex, GPRReg place, GPRReg scratch)
{
    RELEASE_ASSERT(noOverlap(key, header, wordIndex, place, scratch, instanceGPR));
    static_assert(!Slot::offsetOfStructureID());
    jit.zeroExtend32ToWord(header, scratch);
    locateKeyedValuePlace(jit, SlotAccess::Read, key, scratch, place);
    jit.storePtr(key, inKeyedValuePlace(place, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, key)));
    jit.add32(TrustedImm32(KeyedValuePlace::getterBit), scratch);
    jit.store32(scratch, inKeyedValuePlace(place, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)));
    jit.store32(wordIndex, inKeyedValuePlace(place, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, wordIndex)));
}

void rememberGetterAboveOfKey(StubAssembler& jit, GPRReg key, GPRReg header, GPRReg addressOfWord, GPRReg place, GPRReg scratch)
{
    RELEASE_ASSERT(noOverlap(key, header, addressOfWord, place, scratch, instanceGPR, cellCageBaseGPR));
    static_assert(!Slot::offsetOfStructureID());
    const GPRReg wordInCellCage = addressOfWord;
    jit.sub64(cellCageBaseGPR, wordInCellCage);
    jit.urshift64(TrustedImm32(3), wordInCellCage);
    jit.urshift64(wordInCellCage, TrustedImm32(32), scratch);
    Jump liesOutsideCage = jit.branchTest64(CCallHelpers::NonZero, scratch);
    jit.zeroExtend32ToWord(header, scratch);
    locateKeyedValuePlace(jit, SlotAccess::Read, key, scratch, place);
    jit.storePtr(key, inKeyedValuePlace(place, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, key)));
    jit.add32(TrustedImm32(KeyedValuePlace::getterAboveBits), scratch);
    jit.store32(scratch, inKeyedValuePlace(place, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)));
    jit.store32(wordInCellCage, inKeyedValuePlace(place, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, wordInCellCage)));
    jit.store8(TrustedImm32(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.placesSayAbsence)));
    liesOutsideCage.link(&jit);
}

static void rememberKeyedValuePlace(StubAssembler& jit, SlotAccess access, const KeyedProbeRegisters& registers, GPRReg wordIndex)
{
    RELEASE_ASSERT(noOverlap(wordIndex, registers.base, registers.key, registers.structureID, registers.entry));
    locateKeyedValuePlace(jit, access, registers.key, registers.structureID, registers.entry);
    jit.storePtr(registers.key, inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, key)));
    jit.store32(registers.structureID, inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, structureID)));
    jit.store32(wordIndex, inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, wordIndex)));
}

static void returnOwnValueAt(StubAssembler& jit, GPRReg base, GPRReg wordIndex)
{
    Jump isOutOfLine = jit.branchTest64(CCallHelpers::Signed, wordIndex);
    jit.load64(BaseIndex(base, wordIndex, CCallHelpers::TimesEight), base);
    jit.returnToCaller();

    isOutOfLine.link(&jit);
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), base);
    jit.load64(BaseIndex(base, wordIndex, CCallHelpers::TimesEight), base);
    jit.returnToCaller();
}

static GPRReg kindOfPlaceForStores(const KeyedProbeRegisters& registers) { return registers.header; }

static Jump findExtraPropertyByPlace(StubAssembler& jit, SlotAccess access, const KeyedProbeRegisters& registers, JumpList& hasNoExtraProperties)
{
    const GPRReg hiddenWordOffset = registers.uid;
    const GPRReg wordIndex = registers.header;
    const GPRReg extraProperties = registers.payload;
    constexpr ptrdiff_t offsetOfExtraProperty = OBJECT_OFFSETOF(KeyedValuePlace, extraProperty);
    Jump doesNotSay;
    if (access == SlotAccess::Store)
        doesNotSay = jit.branch32(CCallHelpers::NotEqual, kindOfPlaceForStores(registers), TrustedImm32(KeyedValuePlace::extraPropertyBits));
    else {
        jit.add32(TrustedImm32(KeyedValuePlace::extraPropertyBits), registers.structureID, registers.header);
        doesNotSay = jit.branch32(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), registers.header);
    }
    jit.load16(inKeyedValuePlace(registers.entry, access, offsetOfExtraProperty + OBJECT_OFFSETOF(KeyedValuePlace::ExtraProperty, hiddenWordOffset)), hiddenWordOffset);
    loadExtraProperties(jit, registers.base, hiddenWordOffset, extraProperties, hasNoExtraProperties);
    jit.load16SignedExtendTo32(inKeyedValuePlace(registers.entry, access, offsetOfExtraProperty + OBJECT_OFFSETOF(KeyedValuePlace::ExtraProperty, wordIndex)), wordIndex);
    jit.signExtend32ToPtr(wordIndex, wordIndex);
    return doesNotSay;
}

static void returnExtraPropertyByPlace(StubAssembler& jit, const KeyedProbeRegisters& registers, JumpList& hasNoExtraProperties)
{
    Jump doesNotSay = findExtraPropertyByPlace(jit, SlotAccess::Read, registers, hasNoExtraProperties);
    jit.move(registers.payload, registers.base);
    returnOwnValueAt(jit, registers.base, registers.header);
    doesNotSay.link(&jit);
}

void rememberPlaceOfExtraProperty(StubAssembler& jit, SlotAccess access, const CachedSlot& extraProperty, GPRReg object, GPRReg key, GPRReg place, GPRReg structureID)
{
    RELEASE_ASSERT(extraProperty.kind() == Slot::Kind::ExtraProperty && noOverlap(extraProperty.header(), object, key, place, structureID, instanceGPR));
    const GPRReg scratch = structureID;
    constexpr ptrdiff_t offsetOfExtraProperty = OBJECT_OFFSETOF(KeyedValuePlace, extraProperty);
    locateKeyedValuePlace(jit, access, key, structureID, place);
    extraProperty.loadSecondPayloadWord(jit, scratch);
    jit.add32(TrustedImm32(-INT16_MIN), scratch);
    Jump isTooFar = jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(UINT16_MAX));
    jit.add32(TrustedImm32(INT16_MIN), scratch);
    jit.store16(scratch, inKeyedValuePlace(place, access, offsetOfExtraProperty + OBJECT_OFFSETOF(KeyedValuePlace::ExtraProperty, wordIndex)));
    extraProperty.loadHiddenWordOffset(jit, scratch);
    jit.store16(scratch, inKeyedValuePlace(place, access, offsetOfExtraProperty + OBJECT_OFFSETOF(KeyedValuePlace::ExtraProperty, hiddenWordOffset)));
    jit.storePtr(key, inKeyedValuePlace(place, access, OBJECT_OFFSETOF(KeyedValuePlace, key)));
    jit.load32(Address(object, JSCell::structureIDOffset()), scratch);
    jit.add32(TrustedImm32(KeyedValuePlace::extraPropertyBits), scratch);
    jit.store32(scratch, inKeyedValuePlace(place, access, OBJECT_OFFSETOF(KeyedValuePlace, structureID)));
    isTooFar.link(&jit);
}

static void rememberExtraPropertyOfKey(StubAssembler& jit, const KeyedProbeRegisters& registers, const ProbedSlot& slot)
{
    rememberPlaceOfExtraProperty(jit, SlotAccess::Read, CachedSlot(slot.form, Slot::Kind::ExtraProperty, slot.header, slot.payload), registers.base, registers.key, registers.entry, registers.structureID);
    jit.load32(Address(registers.base, JSCell::structureIDOffset()), registers.structureID);
}

static void storeOwnValueAtAndReturn(StubAssembler& jit, GPRReg base, GPRReg wordIndex, GPRReg storage, GPRReg value)
{
    RELEASE_ASSERT(noOverlap(base, wordIndex, storage, value));
    Jump isOutOfLine = jit.branchTest64(CCallHelpers::Signed, wordIndex);
    jit.store64(value, BaseIndex(base, wordIndex, CCallHelpers::TimesEight));
    Label isStored = jit.label();
    returnAfterStoring(jit, base, value);

    isOutOfLine.link(&jit);
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.store64(value, BaseIndex(storage, wordIndex, CCallHelpers::TimesEight));
    jit.jump().linkTo(isStored, &jit);
}

static void storeExtraPropertyByPlaceAndReturn(StubAssembler& jit, const KeyedProbeRegisters& registers, GPRReg value, JumpList& hasNoExtraProperties)
{
    Jump doesNotSay = findExtraPropertyByPlace(jit, SlotAccess::Store, registers, hasNoExtraProperties);
    storeOwnValueAtAndReturn(jit, registers.payload, registers.header, registers.entry, value);
    doesNotSay.link(&jit);
}

static constexpr ptrdiff_t offsetOfStructureIDAfterAddition = RestOfPlaceForStores::distanceFromPlace + OBJECT_OFFSETOF(RestOfPlaceForStores, structureIDAfterAddition);
static constexpr ptrdiff_t offsetOfTypeOfField = RestOfPlaceForStores::distanceFromPlace + OBJECT_OFFSETOF(RestOfPlaceForStores, typeOfField);

#if CPU(X86_64)
static BaseIndex inAcceptedKind(GPRReg entry, ptrdiff_t offset)
{
    return BaseIndex(instanceGPR, entry, CCallHelpers::TimesEight, static_cast<int32_t>(Instance::offsetOfAcceptedKindsOfValues + offset));
}
#else
static Address inAcceptedKind(GPRReg entry, ptrdiff_t offset)
{
    return Address(entry, static_cast<int32_t>(offset));
}
#endif

static_assert(sizeof(Structure) >= 1u << AcceptedKindOfValue::structureIDShift);
static_assert(sizeof(AcceptedKindOfValue) == 1u << CCallHelpers::TimesEight);

static void locateAcceptedKind(StubAssembler&, GPRReg value, GPRReg type, GPRReg kind, GPRReg entry);

void branchUnlessKindOfValueIsAccepted(StubAssembler& jit, GPRReg value, GPRReg type, GPRReg kind, GPRReg entry, JumpList& isNotKnownToBeAccepted)
{
    locateAcceptedKind(jit, value, type, kind, entry);
    isNotKnownToBeAccepted.append(jit.branch32(CCallHelpers::NotEqual, inAcceptedKind(entry, OBJECT_OFFSETOF(AcceptedKindOfValue, type)), type));
    isNotKnownToBeAccepted.append(jit.branch32(CCallHelpers::NotEqual, inAcceptedKind(entry, OBJECT_OFFSETOF(AcceptedKindOfValue, kind)), kind));
}

void rememberAcceptedKindOfValue(StubAssembler& jit, GPRReg value, GPRReg type, GPRReg filedInPlaceOfType, GPRReg place, GPRReg scratch, GPRReg secondScratch)
{
    const GPRReg record = place;
    const GPRReg test = place;
    const GPRReg end = secondScratch;
    const GPRReg kindOfTest = scratch;
    RELEASE_ASSERT(noOverlap(value, type, filedInPlaceOfType, place, scratch, secondScratch, instanceGPR));
    JumpList asksMoreThanKind;
    jit.zeroExtend32ToWord(type, record);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), record);
    asksMoreThanKind.append(jit.branchTest8(CCallHelpers::NonZero, Address(record, RuntimeType::offsetOfFlags()), TrustedImm32(RuntimeType::hasIntegerRange)));
    asksMoreThanKind.append(jit.branchTest16(CCallHelpers::NonZero, Address(record, RuntimeType::offsetOfNumberOfLiteralStrings())));
    jit.load8(Address(record, OBJECT_OFFSETOF(RuntimeType, numberOfTestsOfMoreThanOneFunctionIfFew)), end);
    asksMoreThanKind.append(jit.branch32(CCallHelpers::Equal, end, TrustedImm32(RuntimeType::hasManyTestsOfMoreThanOneFunction)));
    jit.getEffectiveAddress(BaseIndex(record, end, CCallHelpers::TimesEight, static_cast<int32_t>(RuntimeType::offsetOfCellTests())), end);
    jit.addPtr(TrustedImm32(RuntimeType::offsetOfCellTests()), test);
    Label looksAtTest = jit.label();
    Jump hasLookedAtAll = jit.branchPtr(CCallHelpers::AboveOrEqual, test, end);
    jit.load8(Address(test, RuntimeCellTest::offsetOfKind()), kindOfTest);
    asksMoreThanKind.append(jit.branch32(CCallHelpers::AboveOrEqual, kindOfTest, TrustedImm32(4)));
    jit.addPtr(TrustedImm32(sizeof(RuntimeCellTest)), test);
    jit.jump().linkTo(looksAtTest, &jit);
    hasLookedAtAll.link(&jit);

    locateAcceptedKind(jit, value, filedInPlaceOfType, scratch, place);
    jit.store32(filedInPlaceOfType, inAcceptedKind(place, OBJECT_OFFSETOF(AcceptedKindOfValue, type)));
    jit.store32(scratch, inAcceptedKind(place, OBJECT_OFFSETOF(AcceptedKindOfValue, kind)));
    asksMoreThanKind.link(&jit);
}

static void locateAcceptedKind(StubAssembler& jit, GPRReg value, GPRReg type, GPRReg kind, GPRReg entry)
{
    RELEASE_ASSERT(noOverlap(value, type, kind, entry, instanceGPR));
    JumpList hasKind;
    Jump isCell = jit.branchIfCell(value);
    Jump isNotInt32 = jit.branchIfNotInt32(value);
    jit.move(TrustedImm32(static_cast<int32_t>(AcceptedKindOfValue::kindOfInt32)), kind);
    hasKind.append(jit.jump());

    isNotInt32.link(&jit);
    Jump isNotNumber = jit.branchIfNotNumber(value);
    jit.move(TrustedImm32(static_cast<int32_t>(AcceptedKindOfValue::kindOfDouble)), kind);
    hasKind.append(jit.jump());

    isNotNumber.link(&jit);
    jit.add32(TrustedImm32(static_cast<int32_t>(AcceptedKindOfValue::kindOfImmediateZero)), value, kind);
    hasKind.append(jit.jump());

    isCell.link(&jit);
    Jump isOfOtherRealm;
    if (!jit.options().hasOneInstance) {
        loadStructure(jit, value, entry);
        isOfOtherRealm = branchIfNotOfThisRealm(jit, entry);
    }
    loadShapeWordWithMark(jit, value, kind);
    hasKind.append(jit.branch32(CCallHelpers::AboveOrEqual, kind, TrustedImm32(static_cast<int32_t>(AcceptedKindOfValue::leastWordThatIsKind))));
    Jump isCompiledClosure = jit.branch8(CCallHelpers::Equal, Address(value, JSCell::typeInfoTypeOffset()), TrustedImm32(CompiledClosureType));
    if (isOfOtherRealm.isSet())
        isOfOtherRealm.link(&jit);
    jit.load32(Address(value, JSCell::structureIDOffset()), kind);
    jit.urshift32(TrustedImm32(AcceptedKindOfValue::structureIDShift), kind);
    hasKind.append(jit.jump());

    static_assert(ClosureWord::functionShift == 32 && AcceptedKindOfValue::kindOfCompiledClosureZero + ClosureWord::functionMask < AcceptedKindOfValue::kindOfInt32);
    isCompiledClosure.link(&jit);
    jit.load32(Address(value, offsetOfClosureEntry + sizeof(uint32_t)), kind);
    jit.and32(TrustedImm32(ClosureWord::functionMask), kind);
    jit.add32(TrustedImm32(static_cast<int32_t>(AcceptedKindOfValue::kindOfCompiledClosureZero)), kind);
    hasKind.link(&jit);
    jit.add32(type, kind, entry);
    jit.and32(TrustedImm32(static_cast<int32_t>(AcceptedKindOfValue::indexMask)), entry);
#if !CPU(X86_64)
    jit.getEffectiveAddress(BaseIndex(instanceGPR, entry, CCallHelpers::TimesEight), entry);
    jit.addPtr(TrustedImm32(static_cast<int32_t>(Instance::offsetOfAcceptedKindsOfValues)), entry);
#endif
}

static void notePlaceCopiedFromSlot(StubAssembler& jit)
{
    jit.store8(TrustedImm32(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.hasCachedSlotValues)));
}

void rememberPlaceOfValueInField(StubAssembler& jit, GPRReg object, GPRReg key, GPRReg value, GPRReg type, GPRReg address, GPRReg classOfSlot, GPRReg place, GPRReg scratch, GPRReg secondScratch)
{
    const GPRReg offsetOfSlot = address;
    const GPRReg record = place;
    const GPRReg test = place;
    const GPRReg end = secondScratch;
    const GPRReg kindOfTest = scratch;
    const GPRReg kind = scratch;
    const GPRReg entry = place;
    RELEASE_ASSERT(noOverlap(object, key, value, type, address, classOfSlot, place, scratch, secondScratch, instanceGPR));
    static_assert(alignof(RuntimeType) >= 4);
    JumpList asksMoreThanKind;
    jit.zeroExtend32ToWord(type, record);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), record);
    asksMoreThanKind.append(jit.branchTest8(CCallHelpers::NonZero, Address(record, RuntimeType::offsetOfFlags()), TrustedImm32(RuntimeType::hasIntegerRange)));
    asksMoreThanKind.append(jit.branchTest16(CCallHelpers::NonZero, Address(record, RuntimeType::offsetOfNumberOfLiteralStrings())));
    jit.load8(Address(record, OBJECT_OFFSETOF(RuntimeType, numberOfTestsOfMoreThanOneFunctionIfFew)), end);
    asksMoreThanKind.append(jit.branch32(CCallHelpers::Equal, end, TrustedImm32(RuntimeType::hasManyTestsOfMoreThanOneFunction)));
    jit.getEffectiveAddress(BaseIndex(record, end, CCallHelpers::TimesEight, static_cast<int32_t>(RuntimeType::offsetOfCellTests())), end);
    jit.addPtr(TrustedImm32(RuntimeType::offsetOfCellTests()), test);
    Label looksAtTest = jit.label();
    Jump hasLookedAtAll = jit.branchPtr(CCallHelpers::AboveOrEqual, test, end);
    jit.load8(Address(test, RuntimeCellTest::offsetOfKind()), kindOfTest);
    static_assert(static_cast<unsigned>(CellTestKind::IsObject) < 4 && static_cast<unsigned>(CellTestKind::IsCallable) < 4 && static_cast<unsigned>(CellTestKind::CellTypeIs) < 4 && static_cast<unsigned>(CellTestKind::ShapeIn) < 4);
    asksMoreThanKind.append(jit.branch32(CCallHelpers::AboveOrEqual, kindOfTest, TrustedImm32(4)));
    jit.addPtr(TrustedImm32(sizeof(RuntimeCellTest)), test);
    jit.jump().linkTo(looksAtTest, &jit);
    hasLookedAtAll.link(&jit);

    jit.or32(classOfSlot, type);
    jit.load32(Address(object, JSCell::structureIDOffset()), scratch);
    locateKeyedValuePlace(jit, SlotAccess::Store, key, scratch, place);
    jit.storePtr(key, inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, key)));
    jit.add32(TrustedImm32(KeyedValuePlace::fieldBit), scratch);
    jit.store32(scratch, inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, structureID)));
    jit.subPtr(object, offsetOfSlot);
    jit.and32(TrustedImm32(AcceptedKindOfValue::classesWithNarrowReferences), classOfSlot);
    jit.lshift32(TrustedImm32(30), classOfSlot);
    jit.or32(classOfSlot, offsetOfSlot);
    jit.store32(offsetOfSlot, inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, offsetOfSlot)));
    jit.store32(type, inKeyedValuePlace(place, SlotAccess::Store, offsetOfTypeOfField));
    notePlaceCopiedFromSlot(jit);
    locateAcceptedKind(jit, value, type, kind, entry);
    jit.store32(type, inAcceptedKind(entry, OBJECT_OFFSETOF(AcceptedKindOfValue, type)));
    jit.store32(kind, inAcceptedKind(entry, OBJECT_OFFSETOF(AcceptedKindOfValue, kind)));
    asksMoreThanKind.link(&jit);
}

void rememberPlaceOfFieldOfAnyValue(StubAssembler& jit, GPRReg object, GPRReg key, GPRReg offsetOfSlot, GPRReg place, GPRReg scratch)
{
    RELEASE_ASSERT(noOverlap(object, key, offsetOfSlot, place, scratch, instanceGPR));
    jit.load32(Address(object, JSCell::structureIDOffset()), scratch);
    locateKeyedValuePlace(jit, SlotAccess::Store, key, scratch, place);
    jit.storePtr(key, inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, key)));
    jit.add32(TrustedImm32(KeyedValuePlace::fieldOfAnyValueBits), scratch);
    jit.store32(scratch, inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, structureID)));
    jit.store32(offsetOfSlot, inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, offsetOfSlot)));
    notePlaceCopiedFromSlot(jit);
}

static void storeInFieldOfAnyValueAtPlaceAndReturn(StubAssembler& jit, SlotOperation operation, const KeyedProbeRegisters& registers, GPRReg value, JumpList& isLeftToSlot)
{
    const GPRReg base = registers.base;
    const GPRReg place = registers.entry;
    const GPRReg offsetOfSlot = registers.payload;
    Jump doesNotSay = jit.branch32(CCallHelpers::NotEqual, kindOfPlaceForStores(registers), TrustedImm32(KeyedValuePlace::fieldOfAnyValueBits));
    isLeftToSlot.append(jit.branchTest32(CCallHelpers::NonZero, Address(base, offsetOfOrderWord), TrustedImm32(integrityBitsBlockingStore(operation))));
    jit.load32(inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, offsetOfSlot)), offsetOfSlot);
    isLeftToSlot.append(jit.branchTest64(CCallHelpers::Zero, BaseIndex(base, offsetOfSlot, CCallHelpers::TimesOne)));
    jit.store64(value, BaseIndex(base, offsetOfSlot, CCallHelpers::TimesOne));
    returnAfterStoring(jit, base, value);
    doesNotSay.link(&jit);
}

static void storeInFieldAtPlaceAndReturn(StubAssembler& jit, SlotOperation operation, const KeyedProbeRegisters& registers, GPRReg value, JumpList& isLeftToSlot)
{
    const GPRReg base = registers.base;
    const GPRReg place = registers.entry;
    const GPRReg kind = registers.header;
    const GPRReg type = registers.payload;
    const GPRReg entry = registers.uid;
    const GPRReg offsetOfSlot = registers.payload;
    const GPRReg reference = registers.header;
    Jump doesNotSay = jit.branch32(CCallHelpers::NotEqual, kindOfPlaceForStores(registers), TrustedImm32(KeyedValuePlace::fieldBit));
    isLeftToSlot.append(jit.branchTest32(CCallHelpers::NonZero, Address(base, offsetOfOrderWord), TrustedImm32(integrityBitsBlockingStore(operation))));
    jit.load32(inKeyedValuePlace(place, SlotAccess::Store, offsetOfTypeOfField), type);
    locateAcceptedKind(jit, value, type, kind, entry);
    isLeftToSlot.append(jit.branch32(CCallHelpers::NotEqual, inAcceptedKind(entry, OBJECT_OFFSETOF(AcceptedKindOfValue, type)), type));
    isLeftToSlot.append(jit.branch32(CCallHelpers::NotEqual, inAcceptedKind(entry, OBJECT_OFFSETOF(AcceptedKindOfValue, kind)), kind));
    jit.load32(inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, offsetOfSlot)), offsetOfSlot);
    static_assert(!traitsOf(SlotRepresentation::JSValue).absentBits && !traitsOf(SlotRepresentation::Cell).absentBits && !traitsOf(SlotRepresentation::Cell32).absentBits && !traitsOf(SlotRepresentation::OptionalCell32).absentBits);
    Jump isNarrow = jit.branchTest32(CCallHelpers::Signed, offsetOfSlot);
    isLeftToSlot.append(jit.branchTest64(CCallHelpers::Zero, BaseIndex(base, offsetOfSlot, CCallHelpers::TimesOne)));
    jit.store64(value, BaseIndex(base, offsetOfSlot, CCallHelpers::TimesOne));
    Label isStored = jit.label();
    returnAfterStoring(jit, base, value);

    isNarrow.link(&jit);
    jit.and32(TrustedImm32(static_cast<int32_t>(~KeyedValuePlace::narrowReferenceBit)), offsetOfSlot);
    isLeftToSlot.append(jit.branchTest32(CCallHelpers::Zero, BaseIndex(base, offsetOfSlot, CCallHelpers::TimesOne)));
    Jump isMarker = jit.branchIfNotCell(value);
    encodeCellOfProgram(jit, value, reference);
    jit.store32(reference, BaseIndex(base, offsetOfSlot, CCallHelpers::TimesOne));
    jit.jump().linkTo(isStored, &jit);

    static_assert(traitsOf(SlotRepresentation::OptionalCell32).undefinedBits == JSValue::ValueUndefined && traitsOf(SlotRepresentation::OptionalCell32).nullBits == JSValue::ValueNull);
    isMarker.link(&jit);
    jit.store32(value, BaseIndex(base, offsetOfSlot, CCallHelpers::TimesOne));
    jit.returnToCaller();

    doesNotSay.link(&jit);
}

void rememberPlaceOfAddition(StubAssembler& jit, const CachedSlot& addition, GPRReg object, GPRReg key, GPRReg place, GPRReg scratch)
{
    RELEASE_ASSERT(addition.kind() == Slot::Kind::Transition && noOverlap(addition.header(), object, key, place, scratch, instanceGPR));
    jit.load32(Address(object, JSCell::structureIDOffset()), scratch);
    locateKeyedValuePlace(jit, SlotAccess::Store, key, scratch, place);
    jit.storePtr(key, inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, key)));
    jit.add32(TrustedImm32(KeyedValuePlace::additionBit), scratch);
    jit.store32(scratch, inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, structureID)));
    addition.loadOwnWordIndex(jit, scratch);
    jit.store32(scratch, inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, wordIndex)));
    addition.loadFirstPayloadWord(jit, scratch);
    jit.store32(scratch, inKeyedValuePlace(place, SlotAccess::Store, offsetOfStructureIDAfterAddition));
    notePlaceCopiedFromSlot(jit);
}

static void addAtPlaceAndReturn(StubAssembler& jit, const KeyedProbeRegisters& registers, GPRReg value)
{
    const GPRReg base = registers.base;
    const GPRReg wordIndex = registers.uid;
    const GPRReg newStructureID = registers.payload;
    const GPRReg storage = registers.entry;
    checkStubOperands(writeBarrier(), { base });
    Jump doesNotSay = jit.branch32(CCallHelpers::NotEqual, kindOfPlaceForStores(registers), TrustedImm32(KeyedValuePlace::additionBit));
    jit.load32SignedExtendTo64(inKeyedValuePlace(registers.entry, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, wordIndex)), wordIndex);
    jit.load32(inKeyedValuePlace(registers.entry, SlotAccess::Store, offsetOfStructureIDAfterAddition), newStructureID);
    Jump isOutOfLine = jit.branchTest64(CCallHelpers::Signed, wordIndex);
    jit.store64(value, BaseIndex(base, wordIndex, CCallHelpers::TimesEight));
    Label isStored = jit.label();
    jit.store32(newStructureID, Address(base, JSCell::structureIDOffset()));
    jit.jumpToStub(writeBarrier());

    isOutOfLine.link(&jit);
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.store64(value, BaseIndex(storage, wordIndex, CCallHelpers::TimesEight));
    jit.jump().linkTo(isStored, &jit);

    doesNotSay.link(&jit);
}

static void storeAtPlaceInGlobalObjectAndReturn(StubAssembler& jit, const KeyedProbeRegisters& registers, GPRReg value)
{
    const GPRReg globalObject = registers.header;
    const GPRReg structureIDOfGlobalObject = registers.payload;
    const GPRReg wordIndex = registers.uid;
    const GPRReg place = registers.entry;
    JumpList doesNotSay;
    doesNotSay.append(jit.branch8(CCallHelpers::NotEqual, Address(registers.base, JSCell::typeInfoTypeOffset()), TrustedImm32(GlobalProxyType)));
    jit.loadPtr(Address(registers.base, JSGlobalProxy::targetOffset()), globalObject);
    doesNotSay.append(jit.branchPtr(CCallHelpers::NotEqual, globalObject, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));
    doesNotSay.append(jit.branchPtr(CCallHelpers::NotEqual, Address(globalObject, JSGlobalObject::offsetOfGlobalThis()), registers.base));
    jit.load32(Address(globalObject, JSCell::structureIDOffset()), structureIDOfGlobalObject);
    jit.add32(TrustedImm32(KeyedValuePlace::globalObjectBit), structureIDOfGlobalObject);
    doesNotSay.append(jit.branch32(CCallHelpers::NotEqual, inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), structureIDOfGlobalObject));
    jit.load32SignedExtendTo64(inKeyedValuePlace(place, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, wordIndex)), wordIndex);
    storeOwnValueAtAndReturn(jit, globalObject, wordIndex, place, value);

    doesNotSay.link(&jit);
}

enum class StructureIDOfBase : bool { IsNotLoaded, IsLoaded };

static void loadNameOfKeyCell(StubAssembler& jit, const KeyedProbeRegisters& registers, StructureIDOfBase structureIDOfBase, JumpList& hasNoName)
{
    const GPRReg key = registers.key;
    const GPRReg uid = registers.uid;
    RELEASE_ASSERT(noOverlap(key, uid, instanceGPR));
    Jump isString = jit.branchIfString(key);
    hasNoName.append(jit.branchIfNotSymbol(key));
    hasNoName.append(jit.branchTest8(CCallHelpers::NonZero, Address(key, JSCell::typeInfoFlagsOffset()), TrustedImm32(TypeInfoPerCellBit)));
    hasNoName.append(jit.loadCacheableIdentifierImpl(key, uid, false, true));
    Jump isLoaded = jit.jump();

    isString.link(&jit);
    const GPRReg characters = registers.entry;
    const GPRReg length = registers.structureID;
    jit.loadPtr(Address(key, JSString::offsetOfValue()), uid);
    Jump isKnownAtom = jit.branchTest8(CCallHelpers::NonZero, Address(key, JSCell::typeInfoFlagsOffset()), TrustedImm32(TypeInfoPerCellBit));
    Jump isRope = jit.branchIfRopeStringImpl(uid);
    Jump isAtom = jit.branchTest32(CCallHelpers::NonZero, Address(uid, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsAtom()));
    hasNoName.append(jit.branchTest32(CCallHelpers::Zero, Address(uid, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(uid, StringImpl::lengthMemoryOffset()), length);
    jit.loadPtr(Address(uid, StringImpl::dataOffset()), characters);
    Jump hasCharacters = jit.jump();

    isRope.link(&jit);
    hasNoName.append(jit.branchTestPtr(CCallHelpers::Zero, uid, TrustedImm32(JSRopeString::isSubstringInPointer)));
    using Uses = UsesOfSubstringAsKey;
    static_assert(!(~JSRopeString::stringMask & (Uses::one | Uses::two)) && (~JSRopeString::stringMask | Uses::one | Uses::two) <= UINT8_MAX);
    hasNoName.append(jit.branchTestPtr(CCallHelpers::NonZero, uid, TrustedImm32(Uses::two)));
    jit.add8(TrustedImm32(Uses::one), Address(key, JSString::offsetOfValue()));
    jit.load32(Address(key, JSRopeString::offsetOfLength()), length);
    loadSubstringBaseAndOffset(jit, key, uid, characters);
    jit.loadPtr(Address(uid, JSString::offsetOfValue()), uid);
    hasNoName.append(jit.branchTest32(CCallHelpers::Zero, Address(uid, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.addPtr(Address(uid, StringImpl::dataOffset()), characters);

    hasCharacters.link(&jit);
    findNameOfText(jit, characters, length, registers.header, registers.payload, uid, hasNoName);
    if (structureIDOfBase == StructureIDOfBase::IsLoaded)
        jit.load32(Address(registers.base, JSCell::structureIDOffset()), registers.structureID);
    isKnownAtom.link(&jit);
    isAtom.link(&jit);
    isLoaded.link(&jit);
}

static ProbedSlot findKeyedSlot(StubAssembler& jit, SlotAccess access, const KeyedProbeRegisters& registers, StructureIDOfBase structureIDOfBase, JumpList& hasNoName, JumpList& isInNoTable)
{
    loadNameOfKeyCell(jit, registers, structureIDOfBase, hasNoName);
    if (structureIDOfBase == StructureIDOfBase::IsNotLoaded)
        jit.load32(Address(registers.base, JSCell::structureIDOffset()), registers.structureID);
    findHashedSlot(jit, access, {
        .structureID = registers.structureID,
        .uid = registers.uid,
        .entry = registers.entry,
        .header = registers.header,
        .payload = registers.payload,
        .scratch = registers.payload,
        .kind = registers.uid,
    }, isInNoTable);
    return { SlotForm::Slot, registers.header, registers.payload };
}

static ObjectLookupRegisters lookupRegistersOf(const KeyedProbeRegisters& registers, GPRReg name, GPRReg index)
{
    return {
        .receiver = registers.base,
        .name = name,
        .uid = registers.uid,
        .object = registers.header,
        .structure = registers.structureID,
        .size = registers.payload,
        .vector = registers.entry,
        .index = index,
    };
}

static void rememberPlaceOfListedValue(StubAssembler& jit, SlotAccess access, const KeyedProbeRegisters& registers, const ObjectLookupRegisters& lookup)
{
    constexpr int32_t uncachedDictionary = 2 << Structure::s_dictionaryKindShift;
    static_assert(Structure::s_dictionaryKindMask == 3);
    const GPRReg offsetAndAttributes = lookup.structure;
    const GPRReg structureID = lookup.size;
    const GPRReg place = lookup.vector;
    const GPRReg wordIndex = lookup.index;
    Jump isOfOtherObject = jit.branchPtr(CCallHelpers::NotEqual, lookup.object, lookup.receiver);
    loadStructure(jit, lookup.receiver, place);
    Jump mayGiveNamesUp = jit.branchTest32(CCallHelpers::NonZero, Address(place, Structure::bitFieldOffset()), TrustedImm32(uncachedDictionary));
    jit.load32(Address(lookup.receiver, JSCell::structureIDOffset()), structureID);
    jit.urshift32(offsetAndAttributes, TrustedImm32(8), wordIndex);
    Jump isOutOfLine = jit.branch32(CCallHelpers::AboveOrEqual, wordIndex, TrustedImm32(firstOutOfLineOffset));
    jit.add32(TrustedImm32(Slot::wordIndexFor(0)), wordIndex);
    Jump hasWordIndex = jit.jump();
    isOutOfLine.link(&jit);
    jit.neg32(wordIndex);
    jit.add32(TrustedImm32(Slot::wordIndexFor(firstOutOfLineOffset) + firstOutOfLineOffset), wordIndex);
    hasWordIndex.link(&jit);
    locateKeyedValuePlace(jit, access, registers.key, structureID, place);
    jit.storePtr(registers.key, inKeyedValuePlace(place, access, OBJECT_OFFSETOF(KeyedValuePlace, key)));
    jit.store32(structureID, inKeyedValuePlace(place, access, OBJECT_OFFSETOF(KeyedValuePlace, structureID)));
    jit.store32(wordIndex, inKeyedValuePlace(place, access, OBJECT_OFFSETOF(KeyedValuePlace, wordIndex)));
    if (access == SlotAccess::Store)
        jit.store8(TrustedImm32(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.placesTakeReplacementsAsUnwatched)));
    isOfOtherObject.link(&jit);
    mayGiveNamesUp.link(&jit);
}

static void answerKeyFromObjectsAndReturn(StubAssembler& jit, ObjectLookupQuestion question, const KeyedProbeRegisters& registers, JumpList& isInNoTable, JumpList& miss)
{
    isInNoTable.link(&jit);
    countMissOfPair(jit, registers.structureID, registers.uid, registers.header, miss);
    const IndexRegisterOfObjectLookup index(jit);
    const ObjectLookupRegisters lookup = lookupRegistersOf(registers, jit.allocateScratch(StubRegister::T9), index);
    auto emitRememberingOfPlace = [&] {
        rememberPlaceOfListedValue(jit, SlotAccess::Read, registers, lookup);
    };
    const ScopedLambda<void()> rememberPlaceOfOwnValue(emitRememberingOfPlace);
    answerFromObjectsAndReturn(jit, question, lookup, ObjectLookupName::IsGivenByUID, miss, question == ObjectLookupQuestion::Read ? &rememberPlaceOfOwnValue : nullptr);
}

static Jump branchIfKeyedSlotKindDiffers(StubAssembler& jit, const KeyedProbeRegisters& registers, Slot::Kind kind)
{
    return jit.branch32(CCallHelpers::NotEqual, registers.uid, TrustedImm32(static_cast<int32_t>(kind)));
}

struct KeyedValueRead {
    ProbedSlot slot;
    Label readFieldByPlace;
};

static KeyedValueRead returnValueOfKey(StubAssembler& jit, const KeyedProbeRegisters& registers, JumpList& hasNoName, JumpList& isInNoTable, JumpList* emptyFieldSlot, std::optional<StubKey> entryAtPlace = std::nullopt, std::optional<StubKey> entryBehindPlaces = std::nullopt, bool baseIsReceiver = false)
{
    const GPRReg base = registers.base;
    const GPRReg header = registers.header;
    JumpList hasNoPlace;
    JumpList placeIsOfNoField;
    Jump placeIsOfNoValue = findKeyedValuePlace(jit, SlotAccess::Read, registers, header, hasNoPlace, entryAtPlace);
    Label returnValueAtWordIndex = jit.label();
    returnOwnValueAt(jit, base, header);

    placeIsOfNoValue.link(&jit);
    Label readFieldByPlace = jit.label();
    FieldReadExits fieldIsNotRead;
    readFieldAndReturn(jit, {
        .field = findFieldByPlace(jit, registers, placeIsOfNoField, hasNoPlace, &fieldIsNotRead.isAbsent),
        .value = registers.uid,
        .result = base,
        .number = jit.allocateScratch(FPRInfo::fpRegT2),
        .numberScratch = jit.allocateScratch(FPRInfo::fpRegT3),
        .commonRepresentations = CommonRepresentations::EncodedValuesAreReadElsewhere,
    }, fieldIsNotRead);

    placeIsOfNoField.link(&jit);
    JumpList placeDoesNotSayAbsent;
    branchUnlessPlaceSaysAbsent(jit, registers, header, placeDoesNotSayAbsent);
    jit.move(TrustedImm64(JSValue::ValueUndefined), base);
    jit.returnToCaller();

    placeDoesNotSayAbsent.link(&jit);
    if (baseIsReceiver) {
        const GPRReg getterSetter = registers.uid;
        jit.add32(TrustedImm32(KeyedValuePlace::getterBit), registers.structureID, header);
        Jump placeIsOfNoGetter = jit.branch32(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), header);
        jit.load32SignedExtendTo64(inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, wordIndex)), header);
        Jump isOutOfLine = jit.branchTest64(CCallHelpers::Signed, header);
        jit.load64(BaseIndex(base, header, CCallHelpers::TimesEight), getterSetter);
        Jump hasGetterSetter = jit.jump();
        isOutOfLine.link(&jit);
        jit.loadPtr(Address(base, JSObject::butterflyOffset()), getterSetter);
        jit.load64(BaseIndex(getterSetter, header, CCallHelpers::TimesEight), getterSetter);
        hasGetterSetter.link(&jit);
        JumpList cannotCallGetter;
        branchUnlessAccessorPair(jit, getterSetter, cannotCallGetter);
        callGetterAndReturn(jit, base, getterSetter, cannotCallGetter);
        cannotCallGetter.link(&jit);
        jit.load32(Address(base, JSCell::structureIDOffset()), registers.structureID);
        hasNoPlace.append(jit.jump());
        placeIsOfNoGetter.link(&jit);

        jit.add32(TrustedImm32(KeyedValuePlace::getterAboveBits), registers.structureID, header);
        Jump placeIsOfNoGetterAbove = jit.branch32(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), header);
        jit.load32(inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, wordInCellCage)), header);
        jit.load64(BaseIndex(cellCageBaseGPR, header, CCallHelpers::TimesEight), getterSetter);
        JumpList cannotCallGetterAbove;
        branchUnlessAccessorPair(jit, getterSetter, cannotCallGetterAbove);
        callGetterAndReturn(jit, base, getterSetter, cannotCallGetterAbove);
        cannotCallGetterAbove.link(&jit);
        jit.load32(Address(base, JSCell::structureIDOffset()), registers.structureID);
        hasNoPlace.append(jit.jump());
        placeIsOfNoGetterAbove.link(&jit);
    }
    returnExtraPropertyByPlace(jit, registers, hasNoPlace);

    hasNoPlace.link(&jit);
    if (emptyFieldSlot) {
        emptyFieldSlot->append(fieldIsNotRead.isAbsent);
        emptyFieldSlot->append(fieldIsNotRead.cannot);
    } else {
        fieldIsNotRead.isAbsent.link(&jit);
        fieldIsNotRead.cannot.link(&jit);
    }
    if (entryBehindPlaces)
        jit.bindEntry(*entryBehindPlaces);
    const ProbedSlot slot = findKeyedSlot(jit, SlotAccess::Read, registers, StructureIDOfBase::IsLoaded, hasNoName, isInNoTable);
    Jump isNotValue = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Value);
    Jump isInherited = jit.branchTest64(CCallHelpers::NonZero, slot.payload);
    jit.rshift64(TrustedImm32(Slot::locationShiftInHeader), header);
    rememberKeyedValuePlace(jit, SlotAccess::Read, registers, header);
    jit.jump().linkTo(returnValueAtWordIndex, &jit);

    isInherited.link(&jit);
    loadValueBySlot(jit, CachedSlot(slot.form, Slot::Kind::Value, slot.header, slot.payload), base, slot.payload, slot.header, base);
    jit.returnToCaller();

    isNotValue.link(&jit);
    return { slot, readFieldByPlace };
}

static void generateGetByValCached(StubAssembler& jit, RegistersOfCaller registersOfCaller)
{
    const bool keepsRegisters = registersOfCaller == RegistersOfCaller::AreKept;
    const StubKey readOfElement = keepsRegisters ? getByValKeepingRegisters() : getByVal(false);
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    const GPRReg header = registers.header;
    RELEASE_ASSERT(signatureOf(readOfElement).operands == jit.signature().operands);

    jit.linkToStub(jit.branchIfNotCell(key), readOfElement);
    jit.linkToStub(jit.branchIfNotCell(base), readOfElement);
    jit.bindEntry(getByValCachedCells(registersOfCaller));

    JumpList miss;
    JumpList isInNoTable;
    const ProbedSlot slot = returnValueOfKey(jit, registers, miss, isInNoTable, nullptr, getByValCachedAtPlace(registersOfCaller), getByValCachedBehindPlaces(registersOfCaller), true).slot;
    Jump isNotField = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Field);
    rememberFieldOfKey(jit, registers, slot);
    isNotField.link(&jit);
    Jump isNotExtraProperty = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::ExtraProperty);
    rememberExtraPropertyOfKey(jit, registers, slot);
    isNotExtraProperty.link(&jit);
    jumpToSlotHandlerOfKind(jit, SlotHandlerTable::ReadByVal, header, registers.uid);

    answerKeyFromObjectsAndReturn(jit, ObjectLookupQuestion::Read, registers, isInNoTable, miss);

    miss.link(&jit);
    JumpList leafAnswers;
    loadIndexOfText(jit, registers, leafAnswers);
    boxInt32(jit, registers.entry, key);
    jit.jumpToStub(readOfElement);
    leafAnswers.link(&jit);
    callLeafLookup(jit, Entry::operationAOTGetByValGeneric, key, std::nullopt, header, keepsRegisters ? floatingPointRegistersLostToAccessors(StubCallee::Operation) : RegisterSet());
    Jump hasNoAnswer = jit.branchTest64(CCallHelpers::Zero, base);
    jit.returnToCaller();

    hasNoAnswer.link(&jit);
    jit.move(header, base);
    jit.exitOnMiss(Entry::operationAOTGetByValAndCache, { .saves = keepsRegisters ? OperationCall::Saves::CallerSavedGPRsAndFewFPRs : OperationCall::Saves::Nothing, .secondArgumentIn = base, .thirdArgumentIn = key });
}

void generateGetByValCached(StubAssembler& jit)
{
    generateGetByValCached(jit, RegistersOfCaller::AreLost);
}

void generateGetByValCachedOrOwnAccessor(StubAssembler& jit)
{
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg base = registers.base;
    const GPRReg header = registers.header;
    RELEASE_ASSERT(signatureOf(getByVal(false)).operands == jit.signature().operands);
    jit.linkToStub(jit.branchIfNotCell(registers.key), getByVal(false));
    jit.load32(Address(base, JSCell::structureIDOffset()), registers.structureID);
    locateKeyedValuePlace(jit, SlotAccess::Read, registers.key, registers.structureID, registers.entry);
    jit.linkToStub(jit.branchPtr(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, key)), registers.key), getByValCachedBehindPlaces());
    jit.add32(TrustedImm32(KeyedValuePlace::getterBit), registers.structureID, header);
    jit.linkToStub(jit.branch32(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), header), getByValCachedAtPlace());
    jit.load32SignedExtendTo64(inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, wordIndex)), header);
    Jump isOutOfLine = jit.branchTest64(CCallHelpers::Signed, header);
    jit.load64(BaseIndex(base, header, CCallHelpers::TimesEight), base);
    jit.returnToCaller();
    isOutOfLine.link(&jit);
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), base);
    jit.load64(BaseIndex(base, header, CCallHelpers::TimesEight), base);
    jit.returnToCaller();
}

void generateGetByValRunningNoCode(StubAssembler& jit)
{
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    const GPRReg start = jit.allocateScratch(StubRegister::T9);
    RELEASE_ASSERT(noOverlap(base, key, start, registers.uid, registers.structureID, registers.entry, registers.header, registers.payload));
    JumpList asksOperation;

    Jump keyIsNoCell = jit.branchIfNotCell(key);
    Jump startsAtNoCell = jit.branchIfNotCell(base);
    jit.move(base, start);
    Label hasHolder = jit.label();
    const KeyedValueRead read = returnValueOfKey(jit, registers, asksOperation, asksOperation, &asksOperation);
    auto slotOf = [&](Slot::Kind kind) {
        return CachedSlot(read.slot.form, kind, read.slot.header, read.slot.payload);
    };
    Jump isNotField = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Field);
    asksOperation.append(slotOf(Slot::Kind::Field).branchIfFlag(jit, Slot::isOutOfLine));
    rememberFieldOfKey(jit, registers, read.slot);
    jit.jump().linkTo(read.readFieldByPlace, &jit);

    isNotField.link(&jit);
    asksOperation.append(branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::InheritedField));
    slotOf(Slot::Kind::InheritedField).loadHolder(jit, base);
    jit.jump().linkTo(hasHolder, &jit);

    asksOperation.link(&jit);
    jit.move(start, base);
    callLeafLookup(jit, Entry::operationAOTGetNamedByValGeneric, key, std::nullopt, registers.header);
    jit.returnToCaller();

    keyIsNoCell.link(&jit);
    startsAtNoCell.link(&jit);
    jit.move(TrustedImm64(JSValue::encode(JSValue())), base);
    jit.returnToCaller();
}

void generateGetByValCachedKeepingRegisters(StubAssembler& jit)
{
    generateGetByValCached(jit, RegistersOfCaller::AreKept);
}

void generateGetByValOfKeyOfPair(StubAssembler& jit)
{
    static_assert(gprOf(keyCellRegister) == operationArgumentGPR(1) && gprOf(storedValueByValRegister) == operationArgumentGPR(2));
    static_assert(OBJECT_OFFSETOF(KeyedValuePlace, wordIndex) == OBJECT_OFFSETOF(KeyedValuePlace, slotData) && OBJECT_OFFSETOF(KeyedValuePlace, extraProperty) == OBJECT_OFFSETOF(KeyedValuePlace, slotData));
    static_assert(KeyedValuePlace::absenceBit < KeyedValuePlace::fieldBit && KeyedValuePlace::fieldBit < KeyedValuePlace::extraPropertyBits);
    const bool baseIsCell = static_cast<BaseOfKeyedRead>(jit.key().form()) == BaseOfKeyedRead::IsCell;
    const GPRReg first = jit.operand(1);
    const GPRReg second = jit.operand(2);
    const GPRReg pair = jit.allocateScratch(StubRegister::T15);
    const GPRReg structureID = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T10);
    const GPRReg entry = jit.allocateScratch(StubRegister::T14);
    const GPRReg header = jit.allocateScratch(slotHeaderRegister);
    const GPRReg slot = jit.allocateScratch(slotPayloadRegister);
    auto inPair = [&](ptrdiff_t offset) {
        return Address(pair, static_cast<int32_t>(offset));
    };
    auto marks = [](uint32_t bits) {
        return TrustedImm32(static_cast<int32_t>(bits));
    };

    locateKeysOfPair(jit, first, second, pair, structureID);
    JumpList isFound;
    for (unsigned way = 0; way < StringKeyCache::waysOfKeysOfPairs; ++way) {
        Jump hasOtherFirst = jit.branch64(CCallHelpers::NotEqual, inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, first)), first);
        isFound.append(jit.branch64(CCallHelpers::Equal, inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, second)), second));
        hasOtherFirst.link(&jit);
        if (way + 1 < StringKeyCache::waysOfKeysOfPairs)
            jit.addPtr(TrustedImm32(sizeof(StringKeyCache::KeyOfPair)), pair);
    }
    jit.callOperationAndReturn(Entry::operationAOTConcatenatePair, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedGPRsAndFewFPRs,
        .valueDestination = first,
        .spilledArguments = OperationCall::SpilledArguments { 2, static_cast<uint32_t>(UseOfJoinedText::LikelyName) },
    }, [&] {
        jit.jumpToStub(baseIsCell ? getByValCachedCells(RegistersOfCaller::AreKept) : getByValCached(RegistersOfCaller::AreKept));
    });

    isFound.link(&jit);
    const KeyedProbeRegisters registers {
        .base = jit.clobberOperand(0),
        .key = jit.clobberOperand(1),
        .uid = scratch,
        .structureID = structureID,
        .entry = entry,
        .header = header,
        .payload = slot,
    };
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    RELEASE_ASSERT(noOverlap(pair, base, key, scratch, structureID, registers.entry, header, slot));
    Jump baseIsNoCell;
    if (!baseIsCell)
        baseIsNoCell = jit.branchIfNotCell(base);
    jit.load32(Address(base, JSCell::structureIDOffset()), structureID);
    Jump structureMismatch = jit.branch32(CCallHelpers::NotEqual, inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, structureIDOfPlace)), structureID);
    jit.load32(inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, slotOfPlace)), slot);

    using Read = StringKeyCache::KeyOfPair::Read;
    using OtherRead = StringKeyCache::KeyOfPair::OtherRead;
    static_assert(!static_cast<uint32_t>(Read::Int32) && static_cast<uint32_t>(Read::OptionalInt32) == 1 && static_cast<uint32_t>(Read::EncodedValueOrNarrowReference) == 2 && static_cast<uint32_t>(Read::Other) == 3);
    static_assert(OtherRead::Field < OtherRead::ValueOutOfLine && OtherRead::ValueOutOfLine < OtherRead::Absence && OtherRead::Absence < OtherRead::ExtraProperty && static_cast<uint32_t>(OtherRead::ExtraProperty) == 3);
    static_assert(StringKeyCache::KeyOfPair::bitsBelowRead == 30 && StringKeyCache::KeyOfPair::bitsBelowOtherRead == 28 && StringKeyCache::KeyOfPair::bitsOfRepresentation == 8 && StringKeyCache::KeyOfPair::isNarrowReference == 1);
    static_assert(numberOfEnumerators<SlotRepresentation> <= 1 << StringKeyCache::KeyOfPair::bitsOfRepresentation);
    JumpList asksTables;
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT2);
    const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT3);
    auto readKnownFieldAndReturn = [&](SlotRepresentation representation, int32_t displacement) {
        FieldReadExits fieldIsNotRead;
        readFieldAndReturn(jit, {
            .field = { .object = base, .slotData = InvalidGPRReg, .extensionWordOffset = InvalidGPRReg, .address = slot, .representation = registers.entry },
            .value = scratch,
            .result = base,
            .number = number,
            .numberScratch = numberScratch,
            .slot = BaseIndex(base, slot, CCallHelpers::TimesOne, displacement),
        }, fieldIsNotRead, representation);
        asksTables.append(fieldIsNotRead.isAbsent);
        asksTables.append(fieldIsNotRead.cannot);
    };

#if CPU(X86_64)
    constexpr int32_t displacementUnderLowerBit = INT32_MIN;
    constexpr int32_t displacementOfNarrowReference = -2 * static_cast<int32_t>(StringKeyCache::KeyOfPair::isNarrowReference);
    jit.add32(slot, slot);
    Jump isNoInteger(jit.m_assembler.jCC(X86Assembler::ConditionC));
    Jump isOptionalInt32(jit.m_assembler.jCC(X86Assembler::ConditionS));
#else
    constexpr int32_t displacementUnderLowerBit = 0;
    constexpr int32_t displacementOfNarrowReference = 0;
    Jump isNoInteger = jit.branchTest32(CCallHelpers::NonZero, slot, marks(1u << 31));
    Jump isOptionalInt32 = jit.branchTest32(CCallHelpers::NonZero, slot, marks(1u << 30));
    jit.lshift32(TrustedImm32(1), slot);
#endif
    readKnownFieldAndReturn(SlotRepresentation::Int32, 0);

    isOptionalInt32.link(&jit);
#if !CPU(X86_64)
    jit.lshift32(TrustedImm32(2), slot);
    jit.urshift32(TrustedImm32(1), slot);
#endif
    readKnownFieldAndReturn(SlotRepresentation::OptionalInt32, displacementUnderLowerBit);

    isNoInteger.link(&jit);
#if CPU(X86_64)
    Jump isOther(jit.m_assembler.jCC(X86Assembler::ConditionS));
    Jump isNarrowReference = jit.branchTest32(CCallHelpers::NonZero, slot, marks(2 * StringKeyCache::KeyOfPair::isNarrowReference));
#else
    Jump isOther = jit.branchTest32(CCallHelpers::NonZero, slot, marks(1u << 30));
    Jump isNarrowReference = jit.branchTest32(CCallHelpers::NonZero, slot, marks(StringKeyCache::KeyOfPair::isNarrowReference));
    jit.lshift32(TrustedImm32(1), slot);
#endif
    readKnownFieldAndReturn(SlotRepresentation::JSValue, 0);

    isNarrowReference.link(&jit);
#if !CPU(X86_64)
    jit.urshift32(TrustedImm32(1), slot);
    jit.lshift32(TrustedImm32(2), slot);
#endif
    readKnownFieldAndReturn(SlotRepresentation::OptionalCell32, displacementOfNarrowReference);

    isOther.link(&jit);
#if CPU(X86_64)
    jit.lshift32(TrustedImm32(2), slot);
    Jump isAbsenceOrExtraProperty(jit.m_assembler.jCC(X86Assembler::ConditionC));
    Jump isValueOutOfLine(jit.m_assembler.jCC(X86Assembler::ConditionS));
#else
    Jump isAbsenceOrExtraProperty = jit.branchTest32(CCallHelpers::NonZero, slot, marks(1u << 29));
    jit.lshift32(TrustedImm32(3), slot);
    Jump isValueOutOfLine = jit.branchTest32(CCallHelpers::Signed, slot);
#endif
    FieldReadExits fieldIsNotRead;
    jit.urshift32(TrustedImm32(3 + StringKeyCache::KeyOfPair::bitsOfRepresentation), slot);
    jit.addPtr(base, slot);
    jit.load8(inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, slotOfPlace)), registers.entry);
    readFieldAndReturn(jit, {
        .field = { .object = base, .slotData = InvalidGPRReg, .extensionWordOffset = InvalidGPRReg, .address = slot, .representation = registers.entry },
        .value = scratch,
        .result = base,
        .number = number,
        .numberScratch = numberScratch,
        .commonRepresentations = CommonRepresentations::AreReadElsewhere,
    }, fieldIsNotRead);

    isValueOutOfLine.link(&jit);
    jit.signExtend32ToPtr(slot, slot);
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), base);
    jit.load64(BaseIndex(base, slot, CCallHelpers::TimesOne), base);
    jit.returnToCaller();

    isAbsenceOrExtraProperty.link(&jit);
#if CPU(X86_64)
    Jump isExtraProperty(jit.m_assembler.jCC(X86Assembler::ConditionS));
#else
    Jump isExtraProperty = jit.branchTest32(CCallHelpers::NonZero, slot, marks(1u << 28));
#endif
    jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.timesAbsencesWereForgotten)), header);
    jit.xor32(inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, slotOfPlace)), header);
    Jump absenceIsForgotten = jit.branchTest32(CCallHelpers::NonZero, header, marks(StringKeyCache::KeyOfPair::maskBelowOtherRead));
    jit.move(TrustedImm64(JSValue::ValueUndefined), base);
    jit.returnToCaller();

    isExtraProperty.link(&jit);
    jit.load16SignedExtendTo32(inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, slotOfPlace)), header);
    jit.signExtend32ToPtr(header, header);
    jit.load16(inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, slotOfPlace) + 2), scratch);
    jit.and32(marks(StringKeyCache::KeyOfPair::maskBelowOtherRead >> 16), scratch);
    loadExtraProperties(jit, base, scratch, slot, asksTables);
    jit.move(slot, base);
    returnOwnValueAt(jit, base, header);

    absenceIsForgotten.link(&jit);
    jit.store32(TrustedImm32(0), inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, structureIDOfPlace)));
    structureMismatch.link(&jit);
    jit.loadPtr(inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, key)), key);
    locateKeyedValuePlace(jit, SlotAccess::Read, key, structureID, registers.entry);
    jit.linkToStub(jit.branchPtr(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, key)), key), getByValCachedBehindPlaces(RegistersOfCaller::AreKept));
    JumpList readsAtPlace;
    Jump hasNoPlaceYet = jit.branch32(CCallHelpers::Equal, inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, structureIDOfPlace)), TrustedImm32(0));
    constexpr int32_t offsetOfMisses = AOT_INSTANCE_OFFSET(propertyCaches.missesOfPlacesOfPairs);
#if CPU(X86_64)
    jit.m_assembler.addb_im(StringKeyCache::KeyOfPair::stepOfMisses, offsetOfMisses, instanceGPR);
    readsAtPlace.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionNC)));
#else
    jit.load8(Address(instanceGPR, offsetOfMisses), scratch);
    jit.add32(TrustedImm32(StringKeyCache::KeyOfPair::stepOfMisses), scratch);
    jit.store8(scratch, Address(instanceGPR, offsetOfMisses));
    readsAtPlace.append(jit.branchTest32(CCallHelpers::Zero, scratch, TrustedImm32(UINT8_MAX + 1)));
#endif
    hasNoPlaceYet.link(&jit);
    const GPRReg kind = header;
    jit.load32(inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), kind);
    jit.load32(inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, slotData)), slot);
    jit.xor32(structureID, kind);
    JumpList isEncoded;
    Jump placeIsOfField = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(KeyedValuePlace::fieldBit));
    Jump placeIsOfNoValueOrAbsence = jit.branch32(CCallHelpers::Above, kind, TrustedImm32(KeyedValuePlace::fieldBit));
    Jump placeSaysAbsent = jit.branchTest32(CCallHelpers::NonZero, kind);
    Jump valueIsOutOfLine = jit.branch32(CCallHelpers::LessThan, slot, TrustedImm32(0));
    readsAtPlace.append(jit.branch32(CCallHelpers::Above, slot, marks(SlotData::maxOffset / static_cast<uint32_t>(sizeof(EncodedJSValue)))));
    jit.lshift32(TrustedImm32(2), slot);
    jit.or32(marks(StringKeyCache::KeyOfPair::markOf(Read::EncodedValueOrNarrowReference)), slot);
    isEncoded.append(jit.jump());

    valueIsOutOfLine.link(&jit);
    readsAtPlace.append(jit.branch32(CCallHelpers::LessThan, slot, TrustedImm32(-(1 << (StringKeyCache::KeyOfPair::bitsBelowOtherRead - 1)))));
    jit.and32(marks(StringKeyCache::KeyOfPair::maskBelowOtherRead), slot);
    jit.or32(marks(StringKeyCache::KeyOfPair::markOf(OtherRead::ValueOutOfLine)), slot);
    isEncoded.append(jit.jump());

    placeIsOfField.link(&jit);
    static_assert(SlotData::offsetBits == 24 && SlotData::maxOffset == (1u << SlotData::offsetBits) - 1);
    static_assert(!(static_cast<uint32_t>(representationsReadAsEncodedValues.back()) / 2) && static_cast<uint32_t>(representationsReadAsNarrowReferences.front()) / 2 == StringKeyCache::KeyOfPair::isNarrowReference && static_cast<uint32_t>(representationsReadAsNarrowReferences.back()) == 3);
    static_assert(static_cast<uint32_t>(SlotRepresentation::Int32) << StringKeyCache::KeyOfPair::bitsBelowRead == StringKeyCache::KeyOfPair::markOf(Read::Int32) && static_cast<uint32_t>(SlotRepresentation::OptionalInt32) << StringKeyCache::KeyOfPair::bitsBelowRead == StringKeyCache::KeyOfPair::markOf(Read::OptionalInt32));
    static_assert(representationsReadAsInt32.front() == SlotRepresentation::Int32 && representationsReadAsInt32.back() == SlotRepresentation::OptionalInt32);
    jit.urshift32(slot, TrustedImm32(SlotData::offsetBits), scratch);
    jit.and32(TrustedImm32(SlotData::maxOffset), slot);
    Jump isOtherField = jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(static_cast<int32_t>(representationsReadAsInt32.back())));
    jit.urshift32(TrustedImm32(1), slot);
    Jump isInteger = jit.branch32(CCallHelpers::AboveOrEqual, scratch, TrustedImm32(static_cast<int32_t>(representationsReadAsInt32.front())));
    jit.urshift32(TrustedImm32(1), scratch);
    jit.or32(scratch, slot);
    jit.or32(marks(StringKeyCache::KeyOfPair::markOf(Read::EncodedValueOrNarrowReference)), slot);
    isEncoded.append(jit.jump());

    isInteger.link(&jit);
    jit.lshift32(TrustedImm32(StringKeyCache::KeyOfPair::bitsBelowRead), scratch);
    jit.or32(scratch, slot);
    isEncoded.append(jit.jump());

    isOtherField.link(&jit);
    readsAtPlace.append(jit.branch32(CCallHelpers::Above, slot, marks(StringKeyCache::KeyOfPair::maxOffsetOfOtherField)));
    jit.lshift32(TrustedImm32(StringKeyCache::KeyOfPair::bitsOfRepresentation), slot);
    jit.or32(scratch, slot);
    jit.or32(marks(StringKeyCache::KeyOfPair::markOf(OtherRead::Field)), slot);
    isEncoded.append(jit.jump());

    placeSaysAbsent.link(&jit);
    jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.timesAbsencesWereForgotten)), slot);
    jit.and32(marks(StringKeyCache::KeyOfPair::maskBelowOtherRead), slot);
    jit.or32(marks(StringKeyCache::KeyOfPair::markOf(OtherRead::Absence)), slot);
    isEncoded.append(jit.jump());

    placeIsOfNoValueOrAbsence.link(&jit);
    static_assert(!OBJECT_OFFSETOF(KeyedValuePlace::ExtraProperty, hiddenWordOffset) && OBJECT_OFFSETOF(KeyedValuePlace::ExtraProperty, wordIndex) == 2 && sizeof(KeyedValuePlace::ExtraProperty) == 4);
    readsAtPlace.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(KeyedValuePlace::extraPropertyBits)));
    jit.urshift32(slot, TrustedImm32(16), scratch);
    jit.lshift32(TrustedImm32(16), slot);
    readsAtPlace.append(jit.branchTest32(CCallHelpers::NonZero, slot, marks(~StringKeyCache::KeyOfPair::maskBelowOtherRead)));
    jit.or32(scratch, slot);
    jit.or32(marks(StringKeyCache::KeyOfPair::markOf(OtherRead::ExtraProperty)), slot);

    isEncoded.link(&jit);
    jit.store32(structureID, inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, structureIDOfPlace)));
    jit.store32(slot, inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, slotOfPlace)));
    jit.store8(TrustedImm32(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.pairsHavePlaces)));
    readsAtPlace.link(&jit);
    jit.jumpToStub(getByValCachedAtPlace(RegistersOfCaller::AreKept));

    asksTables.link(&jit);
    fieldIsNotRead.isAbsent.link(&jit);
    fieldIsNotRead.cannot.link(&jit);
    jit.loadPtr(inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, key)), key);
    jit.jumpToStub(getByValCachedBehindPlaces(RegistersOfCaller::AreKept));

    if (!baseIsCell) {
        baseIsNoCell.link(&jit);
        jit.loadPtr(inPair(OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, key)), key);
        jit.jumpToStub(getByValCached(RegistersOfCaller::AreKept));
    }
}

void generateGetSuperByVal(StubAssembler& jit)
{
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    const GPRReg receiver = jit.operand(2);
    const GPRReg start = jit.allocateScratch(StubRegister::T9);
    const GPRReg getterSetter = registers.uid;
    RELEASE_ASSERT(noOverlap(base, key, receiver, start, registers.uid, registers.structureID, registers.entry, registers.header, registers.payload));
    JumpList nothingIsFiled;
    JumpList whatIsFiledDoesNotAnswer;

    jit.move(base, start);
    Jump startsAtNoCell = jit.branchIfNotCell(base);
    Jump keyIsNoCell = jit.branchIfNotCell(key);
    Label hasHolder = jit.label();
    const KeyedValueRead read = returnValueOfKey(jit, registers, nothingIsFiled, nothingIsFiled, &whatIsFiledDoesNotAnswer);
    auto slotOf = [&](Slot::Kind kind) {
        return CachedSlot(read.slot.form, kind, read.slot.header, read.slot.payload);
    };
    Jump isNotField = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Field);
    whatIsFiledDoesNotAnswer.append(slotOf(Slot::Kind::Field).branchIfFlag(jit, Slot::isOutOfLine));
    rememberFieldOfKey(jit, registers, read.slot);
    jit.jump().linkTo(read.readFieldByPlace, &jit);

    isNotField.link(&jit);
    Jump isNotAbsent = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Absent);
    rememberAbsenceOfKey(jit, key, read.slot.header, registers.entry, registers.structureID);
    jit.move(TrustedImm64(JSValue::ValueUndefined), base);
    jit.returnToCaller();

    isNotAbsent.link(&jit);
    Jump isNotInheritedField = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::InheritedField);
    slotOf(Slot::Kind::InheritedField).loadHolder(jit, base);
    jit.jump().linkTo(hasHolder, &jit);

    isNotInheritedField.link(&jit);
    Jump isNotGetter = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Getter);
    loadValueBySlot(jit, slotOf(Slot::Kind::Getter), base, registers.uid, registers.structureID, getterSetter);
    Label callGetter = jit.label();
    branchUnlessAccessorPair(jit, getterSetter, whatIsFiledDoesNotAnswer);
    callGetterAndReturn(jit, receiver, getterSetter, whatIsFiledDoesNotAnswer);

    isNotGetter.link(&jit);
    Jump isNotFieldGetter = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::FieldGetter);
    {
        const FieldAddressRegisters field { .object = base, .slotData = registers.uid, .extensionWordOffset = InvalidGPRReg, .address = registers.structureID, .representation = registers.entry };
        whatIsFiledDoesNotAnswer.append(slotOf(Slot::Kind::FieldGetter).branchIfFlag(jit, Slot::isOutOfLine));
        slotOf(Slot::Kind::FieldGetter).loadFirstPayloadWord(jit, field.slotData);
        computeFieldAddress(jit, field, false, whatIsFiledDoesNotAnswer);
        loadGetterSetterInField(jit, field.address, field.representation, getterSetter, whatIsFiledDoesNotAnswer);
        jit.jump().linkTo(callGetter, &jit);
    }

    isNotFieldGetter.link(&jit);
    JumpList handlerAnswers;
    for (Slot::Kind kind : { Slot::Kind::ArrayLength, Slot::Kind::FarValue, Slot::Kind::Constant })
        handlerAnswers.append(jit.branch32(CCallHelpers::Equal, registers.uid, TrustedImm32(static_cast<int32_t>(kind))));
    whatIsFiledDoesNotAnswer.append(jit.jump());
    handlerAnswers.link(&jit);
    jumpToSlotHandlerOfKind(jit, SlotHandlerTable::ReadByVal, read.slot.header, registers.uid);

    nothingIsFiled.link(&jit);
    jit.move(start, base);
    JumpList leafAnswers;
    {
        const GPRReg index = registers.entry;
        const GPRReg shape = registers.uid;
        const GPRReg storage = registers.uid;
        const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
        const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT1);
        loadIndexOfText(jit, registers, leafAnswers);
        Label hasIndex = jit.label();
        jit.load8(Address(base, JSCell::indexingTypeAndMiscOffset()), shape);
        jit.and32(TrustedImm32(IndexingShapeMask), shape);
        Jump isDouble = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
        Jump isContiguous = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
        leafAnswers.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(Int32Shape)));
        isContiguous.link(&jit);
        jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
        leafAnswers.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
        jit.load64(BaseIndex(storage, index, CCallHelpers::TimesEight), storage);
        leafAnswers.append(jit.branchTest64(CCallHelpers::Zero, storage));
        jit.move(storage, jit.result());
        jit.returnToCaller();

        keyIsNoCell.link(&jit);
        whatIsFiledDoesNotAnswer.append(jit.branchIfNotInt32(key));
        jit.zeroExtend32ToWord(key, index);
        jit.jump().linkTo(hasIndex, &jit);

        isDouble.link(&jit);
        JumpList isNotInt32;
        jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
        leafAnswers.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
        jit.loadDouble(BaseIndex(storage, index, CCallHelpers::TimesEight), number);
        const PureDouble element = PureDouble::branchIfNaN(jit, number, leafAnswers);
        branchIfNotExactInt32(jit, element.fpr(), storage, isNotInt32, numberScratch);
        boxInt32(jit, storage, jit.result());
        jit.returnToCaller();
        isNotInt32.link(&jit);
        boxDouble(jit, element, jit.result());
        jit.returnToCaller();
    }
    leafAnswers.link(&jit);
    whatIsFiledDoesNotAnswer.append(jit.branchIfNotCell(key));
    callLeafLookup(jit, Entry::operationAOTGetByValGeneric, key, receiver, registers.header);
    Jump hasNoAnswer = jit.branchTest64(CCallHelpers::Zero, base);
    jit.returnToCaller();

    hasNoAnswer.link(&jit);
    jit.move(registers.header, base);
    startsAtNoCell.link(&jit);
    jit.exitOnMiss(Entry::operationAOTSuperGet, { .secondArgumentIn = base, .thirdArgumentIn = key, .fourthArgumentIn = receiver });

    whatIsFiledDoesNotAnswer.link(&jit);
    jit.move(start, base);
    jit.callOperationAndReturn(Entry::operationAOTGetByValWithThis, { .secondArgumentIn = base, .thirdArgumentIn = receiver, .fourthArgumentIn = key });
}

static void returnPresenceOfElement(StubAssembler& jit, PresenceQuestion question, const KeyedProbeRegisters& registers, JumpList& isUnknown, JumpList* isProxy = nullptr)
{
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    const GPRReg shape = registers.header;
    const GPRReg storage = registers.header;
    const GPRReg index = registers.uid;
    const GPRReg structure = registers.structureID;
    const GPRReg extraProperties = registers.structureID;
    const GPRReg type = registers.entry;
    const bool asksIn = question == PresenceQuestion::In;

    auto returnTrueForElementOf = [&](GPRReg owner, bool mayHoldUnboxedNumbers, JumpList& isAbsent, JumpList& hasOtherStorage) {
        Jump isDouble;
        jit.load8(Address(owner, JSCell::indexingTypeAndMiscOffset()), shape);
        jit.and32(TrustedImm32(IndexingShapeMask), shape);
        Jump isContiguous = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
        if (mayHoldUnboxedNumbers)
            isDouble = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
        hasOtherStorage.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(Int32Shape)));
        isContiguous.link(&jit);
        jit.loadPtr(Address(owner, JSObject::butterflyOffset()), storage);
        jit.zeroExtend32ToWord(key, index);
        isAbsent.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
        isAbsent.append(jit.branchTest64(CCallHelpers::Zero, BaseIndex(storage, index, CCallHelpers::TimesEight)));
        Label isFound = jit.label();
        jit.move(TrustedImm32(1), jit.result());
        jit.returnToCaller();
        if (!mayHoldUnboxedNumbers)
            return;

        isDouble.link(&jit);
        const GPRReg element = registers.payload;
        const GPRReg hole = registers.entry;
        jit.loadPtr(Address(owner, JSObject::butterflyOffset()), storage);
        jit.zeroExtend32ToWord(key, index);
        isAbsent.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
        jit.load64(BaseIndex(storage, index, CCallHelpers::TimesEight), element);
        jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), hole);
        jit.branch64(CCallHelpers::NotEqual, element, hole).linkTo(isFound, &jit);
        isAbsent.append(jit.jump());
    };

    JumpList isAbsentInOwnStorage;
    JumpList hasNoSuchStorage;
    isUnknown.append(jit.branchIfNotInt32(key));
    isUnknown.append(jit.branchIfNotCell(base));
    returnTrueForElementOf(base, true, isAbsentInOwnStorage, hasNoSuchStorage);

    isAbsentInOwnStorage.link(&jit);
    Jump baseIsArray = jit.branchIfType(base, ArrayType);
    Label isOrdinaryUnlessFlagged = jit.label();
    isUnknown.append(jit.branchTest8(CCallHelpers::NonZero, Address(base, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetOwnPropertySlot)));

    baseIsArray.link(&jit);
    Label hasNoElement = jit.label();
    isUnknown.append(jit.branch32(CCallHelpers::LessThan, key, TrustedImm32(0)));
    JumpList isFoundNowhere;
    if (asksIn) {
        const GPRReg above = structure;
        loadStructure(jit, base, structure);
        isFoundNowhere.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_hasPristineElementsBits)));
        Label hasStructure = jit.label();
        jit.load64(Address(structure, Structure::prototypeOffset()), above);
        isFoundNowhere.append(jit.branch64(CCallHelpers::Equal, above, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.objectPrototype))));
        Jump isNotCell = jit.branchIfNotCell(above);
        isUnknown.append(jit.branchTest64(CCallHelpers::Zero, above));
        isUnknown.append(jit.branchTest8(CCallHelpers::NonZero, Address(above, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetOwnPropertySlot)));
        isUnknown.append(jit.branchTest8(CCallHelpers::NonZero, Address(above, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask)));
        loadStructure(jit, above, structure);
        jit.jump().linkTo(hasStructure, &jit);

        isNotCell.link(&jit);
        isUnknown.append(jit.branch64(CCallHelpers::NotEqual, above, TrustedImm64(JSValue::ValueNull)));
    }
    isFoundNowhere.link(&jit);
    jit.move(TrustedImm32(0), jit.result());
    jit.returnToCaller();

    hasNoSuchStorage.link(&jit);
    isUnknown.append(jit.branchTest32(CCallHelpers::NonZero, shape));
    jit.load8(Address(base, JSCell::typeInfoTypeOffset()), type);
    JumpList isNotCompiledObject;
    isNotCompiledObject.append(jit.branch32(CCallHelpers::NotEqual, type, TrustedImm32(CompiledObjectType)));
    JumpList isAbsentInExtraProperties;
    JumpList extraPropertiesHaveOtherStorage;
    const GPRReg hiddenWordOffset = type;
    jit.branchTest32(CCallHelpers::Zero, Address(base, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)).linkTo(hasNoElement, &jit);
    loadStructure(jit, base, structure);
    jit.load32(Address(structure, CompiledStructure::offsetOfExtraPropertiesSlot()), hiddenWordOffset);
    isUnknown.append(jit.branchTest32(CCallHelpers::Zero, hiddenWordOffset, TrustedImm32(SlotData::maxOffset)));
    jit.and32(TrustedImm32(SlotData::maxOffset), hiddenWordOffset);
    loadExtraProperties(jit, base, hiddenWordOffset, extraProperties, isUnknown);
    returnTrueForElementOf(extraProperties, false, isAbsentInExtraProperties, extraPropertiesHaveOtherStorage);

    isAbsentInExtraProperties.linkTo(hasNoElement, &jit);
    extraPropertiesHaveOtherStorage.link(&jit);
    jit.branchTest32(CCallHelpers::Zero, shape).linkTo(hasNoElement, &jit);
    isUnknown.append(jit.jump());

    isNotCompiledObject.link(&jit);
    if (isProxy)
        isProxy->append(jit.branch32(CCallHelpers::Equal, type, TrustedImm32(ProxyObjectType)));
    // A typed array has the indexes below its length and lets nobody else answer for a number. A length may exceed 31 bits, so a negative key must not look like an index.
    const GPRReg length = storage;
    jit.sub32(type, TrustedImm32(FirstTypedArrayType), length);
    Jump isNotTypedArray = jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(NumberOfTypedArrayTypesExcludingDataView));
    isUnknown.append(jit.branchTest8(CCallHelpers::NonZero, Address(base, JSArrayBufferView::offsetOfMode()), TrustedImm32(isResizableOrGrowableSharedMode)));
    jit.signExtend32ToPtr(key, index);
    jit.loadPtr(Address(base, JSArrayBufferView::offsetOfLength()), length);
    jit.compare64(CCallHelpers::Below, index, length, jit.result());
    jit.returnToCaller();

    isNotTypedArray.link(&jit);
    isUnknown.append(jit.branch32(CCallHelpers::Below, type, TrustedImm32(ObjectType)));
    jit.jump().linkTo(isOrdinaryUnlessFlagged, &jit);
}

void generateInByValCached(StubAssembler& jit)
{
    const bool answersBoxed = jit.key().form() == hasOwnByValCachedBoxed().form();
    const PresenceQuestion question = answersBoxed ? PresenceQuestion::HasOwn : static_cast<PresenceQuestion>(jit.key().form());
    const bool asksIn = question == PresenceQuestion::In;
    RELEASE_ASSERT(asksIn || question == PresenceQuestion::HasOwn);
    if (answersBoxed)
        jit.boxBooleanAtEveryReturn();
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    const GPRReg header = registers.header;
    JumpList miss;
    JumpList needsOperation;
    JumpList leafAnswers;

    Jump keyIsCell = jit.branchIfCell(key);
    Label asksForIndex = jit.label();
    if (asksIn) {
        JumpList isProxy;
        returnPresenceOfElement(jit, question, registers, needsOperation, &isProxy);

        isProxy.link(&jit);
        checkStubOperands(throughProxy(TrapOfProxy::Has), { base, registers.payload, header });
        loadKeptTextOfIndex(jit, key, registers.structureID, registers.uid, registers.payload, needsOperation);
        jit.move(TrustedImm32(0), header);
        jit.jumpToStub(throughProxy(TrapOfProxy::Has));
    } else
        returnPresenceOfElement(jit, question, registers, needsOperation);

    keyIsCell.link(&jit);
    needsOperation.append(jit.branchIfNotCell(base));
    needsOperation.append(jit.branchIfNotObject(base));
    Label asksBase = jit.label();
    JumpList hasNoPlace;
    JumpList placeIsOfNoField;
    JumpList isPresent;
    Jump placeIsOfNoValue = findKeyedValuePlace(jit, SlotAccess::Read, registers, header, hasNoPlace);
    Label returnTrue = jit.label();
    jit.move(TrustedImm32(1), jit.result());
    jit.returnToCaller();

    placeIsOfNoValue.link(&jit);
    if (!asksIn) {
        jit.add32(TrustedImm32(KeyedValuePlace::notOwnBits), registers.structureID, header);
        Jump doesNotSayThatNothingIsOwn = jit.branch32(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), header);
        jit.move(TrustedImm32(0), jit.result());
        jit.returnToCaller();
        doesNotSayThatNothingIsOwn.link(&jit);
    }
    branchOnFieldPresence(jit, findFieldByPlace(jit, registers, placeIsOfNoField, hasNoPlace), registers.uid, isPresent, hasNoPlace);
    isPresent.linkTo(returnTrue, &jit);

    placeIsOfNoField.link(&jit);
    branchUnlessPlaceSaysAbsent(jit, registers, header, hasNoPlace);
    jit.move(TrustedImm32(0), jit.result());
    jit.returnToCaller();

    hasNoPlace.link(&jit);
    JumpList isInNoTable;
    const ProbedSlot slot = findKeyedSlot(jit, SlotAccess::Read, registers, StructureIDOfBase::IsLoaded, miss, isInNoTable);
    Jump isNotField = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Field);
    rememberFieldOfKey(jit, registers, slot);
    Label readsSlot = jit.label();
    const FieldAddressRegisters field {
        .object = base,
        .slotData = registers.uid,
        .extensionWordOffset = registers.structureID,
        .address = registers.structureID,
        .representation = registers.entry,
    };
    returnFieldPresence(jit, CachedSlot(SlotForm::Slot, Slot::Kind::Field, header, registers.payload), field, needsOperation, &leafAnswers);

    isNotField.link(&jit);
    JumpList kindIsUnknown;
    auto emitRememberingThatNothingIsOwn = [&] {
        locateKeyedValuePlace(jit, SlotAccess::Read, key, registers.structureID, registers.entry);
        jit.storePtr(key, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, key)));
        jit.add32(TrustedImm32(KeyedValuePlace::notOwnBits), registers.structureID);
        jit.store32(registers.structureID, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)));
    };
    const ScopedLambda<void()> rememberThatNothingIsOwn(emitRememberingThatNothingIsOwn);
    returnPresenceBySlot(jit, question, slot, base, asksBase, kindIsUnknown, KindOfProbedSlot::IsInT10, asksIn ? nullptr : &rememberThatNothingIsOwn);

    kindIsUnknown.link(&jit);
    loadSlotKind(jit, slot, registers.uid);
    jit.branch32(CCallHelpers::Equal, registers.uid, TrustedImm32(static_cast<int32_t>(Slot::Kind::FieldGetter))).linkTo(readsSlot, &jit);
    if (asksIn) {
        miss.append(jit.branch32(CCallHelpers::NotEqual, registers.uid, TrustedImm32(static_cast<int32_t>(Slot::Kind::Uncacheable))));
        jit.linkToStub(jit.branchTest64(CCallHelpers::NonZero, registers.payload), throughProxy(TrapOfProxy::Has));
    } else {
        for (Slot::Kind kind : { Slot::Kind::FieldOrInherited, Slot::Kind::FieldOrInheritedField }) {
            Jump isOfOtherKind = jit.branch32(CCallHelpers::NotEqual, registers.uid, TrustedImm32(static_cast<int32_t>(kind)));
            returnFieldPresence(jit, CachedSlot(SlotForm::Slot, kind, header, registers.payload), field, needsOperation);
            isOfOtherKind.link(&jit);
        }
    }
    miss.append(jit.jump());

    answerKeyFromObjectsAndReturn(jit, asksIn ? ObjectLookupQuestion::In : ObjectLookupQuestion::HasOwn, registers, isInNoTable, miss);

    miss.link(&jit);
    loadIndexOfText(jit, registers, leafAnswers);
    boxInt32(jit, registers.entry, key);
    jit.jump().linkTo(asksForIndex, &jit);
    leafAnswers.link(&jit);
    callLeafLookup(jit, Entry::operationAOTInByValGeneric, key, std::nullopt, header);
    Jump hasNoAnswer = jit.branchTest32(CCallHelpers::Zero, base);
    jit.compare32(CCallHelpers::AboveOrEqual, base, TrustedImm32(static_cast<int32_t>(asksIn ? LeafPresence::Inherited : LeafPresence::Own)), base);
    jit.returnToCaller();

    hasNoAnswer.link(&jit);
    jit.move(header, base);
    needsOperation.link(&jit);
    jit.exitOnMiss(asksIn ? Entry::operationAOTInByValAndCache : Entry::operationAOTHasOwnByValAndCache, { .secondArgumentIn = base, .thirdArgumentIn = key });
}

static void returnThatPropertyIsGone(StubAssembler& jit)
{
    jit.move(TrustedImm32(1), jit.result());
    jit.returnToCaller();
}

static void removeBySlotAndReturn(StubAssembler& jit, const KeyedProbeRegisters& registers, const ProbedSlot& probed, JumpList& needsOperation)
{
    const GPRReg base = registers.base;
    const GPRReg kind = registers.key;
    const GPRReg wordIndex = registers.key;
    const GPRReg storage = registers.entry;
    const GPRReg scratch = registers.structureID;

    static_assert(Slot::Kind::Uncacheable < Slot::Kind::Removal && Slot::Kind::NothingToRemove == static_cast<Slot::Kind>(numberOfSlotKinds - 1));
    needsOperation.append(jit.branch32(CCallHelpers::Below, kind, TrustedImm32(static_cast<int32_t>(Slot::Kind::Removal))));
    Jump isNotInOwnStorage = jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(static_cast<int32_t>(Slot::Kind::Removal)));
    const CachedSlot removal(probed.form, Slot::Kind::Removal, probed.header, probed.payload);
    needsOperation.append(branchIfBarrierIsNeeded(jit, base, scratch));
    removal.loadOwnWordIndex(jit, wordIndex);
    jit.move(base, storage);
    Jump isInline = removal.branchUnlessFlag(jit, Slot::isOutOfLine);
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    isInline.link(&jit);
    jit.store32(removal.registerOfFirstPayloadWord(), Address(base, JSCell::structureIDOffset()));
    jit.store64(TrustedImm32(0), BaseIndex(storage, wordIndex, CCallHelpers::TimesEight));
    returnThatPropertyIsGone(jit);

    isNotInOwnStorage.link(&jit);
    Jump isNotAmongExtraProperties = jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(static_cast<int32_t>(Slot::Kind::ExtraPropertyRemoval)));
    const CachedSlot extraRemoval(probed.form, Slot::Kind::ExtraPropertyRemoval, probed.header, probed.payload);
    const GPRReg newStructure = registers.key;
    const GPRReg extraProperties = registers.entry;
    extraRemoval.loadSecondPayloadWord(jit, scratch);
    needsOperation.append(jit.branch32(CCallHelpers::NotEqual, Address(base, offsetOfOrderWord), scratch));
    needsOperation.append(branchIfBarrierIsNeeded(jit, base, scratch));
    extraRemoval.loadFirstPayloadWord(jit, newStructure);
    decodeStructureID(jit, newStructure);
    jit.load32(Address(newStructure, CompiledStructure::offsetOfExtraPropertiesSlot()), scratch);
    jit.and32(TrustedImm32(SlotData::maxOffset), scratch);
    loadExtraProperties(jit, base, scratch, extraProperties, needsOperation);
    needsOperation.append(branchIfBarrierIsNeeded(jit, extraProperties, scratch));
    jit.load32(Address(newStructure, CompiledStructure::offsetOfExtraPropertiesStructureID()), scratch);
    jit.store32(scratch, Address(extraProperties, JSCell::structureIDOffset()));
    jit.store32(extraRemoval.registerOfFirstPayloadWord(), Address(base, JSCell::structureIDOffset()));
    extraRemoval.loadOwnWordIndex(jit, wordIndex);
    Jump isInCell = extraRemoval.branchUnlessFlag(jit, Slot::isOutOfLine);
    jit.loadPtr(Address(extraProperties, JSObject::butterflyOffset()), extraProperties);
    isInCell.link(&jit);
    jit.store64(TrustedImm32(0), BaseIndex(extraProperties, wordIndex, CCallHelpers::TimesEight));
    returnThatPropertyIsGone(jit);

    isNotAmongExtraProperties.link(&jit);
    Jump isNoField = jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(static_cast<int32_t>(Slot::Kind::FieldRemoval)));
    const CachedSlot fieldRemoval(probed.form, Slot::Kind::FieldRemoval, probed.header, probed.payload);
    const GPRReg pattern = registers.key;
    const GPRReg held = registers.key;
    const GPRReg offset = registers.entry;
    const BaseIndex slotOfField(base, offset, CCallHelpers::TimesOne);
    fieldRemoval.loadHiddenWordOffset(jit, offset);
    jit.urshift64(probed.header, TrustedImm32(Slot::flagsShiftInHeader + Slot::absentPatternShift), pattern);
    jit.and32(TrustedImm32(Slot::absentPatternMask), pattern);
    JumpList hasEightBytes;
    JumpList hasFourBytes;
    JumpList isAbsent;
    JumpList isCleared;
    auto branchIfPatternIs = [&](AbsentPattern candidate) {
        return jit.branch32(CCallHelpers::Equal, pattern, TrustedImm32(static_cast<int32_t>(candidate)));
    };
    auto branchUnlessOrderIsFiled = [&] {
        needsOperation.append(jit.branch32(CCallHelpers::NotEqual, Address(base, offsetOfOrderWord), fieldRemoval.registerOfFirstPayloadWord()));
    };
    static_assert(absentPatternOf(SlotRepresentation::OptionalBoolean) == AbsentPattern::ZeroInOneByte);
    jit.move(TrustedImm32(0), scratch);
    hasEightBytes.append(branchIfPatternIs(AbsentPattern::ZeroInEightBytes));
    hasFourBytes.append(branchIfPatternIs(AbsentPattern::ZeroInFourBytes));
    Jump hasOneByte = branchIfPatternIs(AbsentPattern::ZeroInOneByte);
    jit.move(TrustedImm32(absentInt32), scratch);
    hasFourBytes.append(branchIfPatternIs(AbsentPattern::Int32));
    jit.move(TrustedImm64(static_cast<int64_t>(absentDouble)), scratch);
    hasEightBytes.append(branchIfPatternIs(AbsentPattern::Double));
    jit.move(TrustedImm64(static_cast<int64_t>(absentInteger)), scratch);

    hasEightBytes.link(&jit);
    jit.load64(slotOfField, held);
    isAbsent.append(jit.branch64(CCallHelpers::Equal, held, scratch));
    branchUnlessOrderIsFiled();
    jit.store64(scratch, slotOfField);
    isCleared.append(jit.jump());

    hasOneByte.link(&jit);
    jit.load8(slotOfField, held);
    isAbsent.append(jit.branchTest32(CCallHelpers::Zero, held));
    branchUnlessOrderIsFiled();
    jit.store8(scratch, slotOfField);
    isCleared.append(jit.jump());

    hasFourBytes.link(&jit);
    jit.load32(slotOfField, held);
    isAbsent.append(jit.branch32(CCallHelpers::Equal, held, scratch));
    branchUnlessOrderIsFiled();
    jit.store32(scratch, slotOfField);

    isCleared.link(&jit);
    fieldRemoval.loadSecondPayloadWord(jit, scratch);
    jit.store32(scratch, Address(base, offsetOfOrderWord));
    Jump staysAsBorn = fieldRemoval.branchUnlessFlag(jit, Slot::leavesStateOfBirth);
    setFlagThatFieldHasLeft(jit, base, scratch);
    setMarkOfCell(jit, base, scratch);
    staysAsBorn.link(&jit);
    returnThatPropertyIsGone(jit);

    isAbsent.link(&jit);
    needsOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(base, offsetOfOrderWord), TrustedImm32(orderWordHasFieldOutsideSlot)));
    isNoField.link(&jit);
    returnThatPropertyIsGone(jit);
}

static void removeNamedPropertyAndReturn(StubAssembler& jit, const KeyedProbeRegisters& registers)
{
    JumpList needsOperation;
    JumpList isInNoTable;
    loadStructure(jit, registers.base, registers.entry);
    needsOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(registers.entry, Structure::bitFieldOffset()), TrustedImm32(Structure::s_dictionaryKindMask << Structure::s_dictionaryKindShift)));
    jit.load32(Address(registers.base, JSCell::structureIDOffset()), registers.structureID);
    findHashedSlot(jit, SlotAccess::Delete, {
        .structureID = registers.structureID,
        .uid = registers.uid,
        .entry = registers.entry,
        .header = registers.header,
        .payload = registers.payload,
        .scratch = registers.payload,
        .kind = registers.key,
    }, isInNoTable);
    removeBySlotAndReturn(jit, registers, { SlotForm::Slot, registers.header, registers.payload }, needsOperation);

    isInNoTable.link(&jit);
    countMissOfPair(jit, registers.structureID, registers.uid, registers.header, needsOperation);
    needsOperation.link(&jit);
    jit.and64(TrustedImm32(~static_cast<int32_t>(HashedSlot::accessMask)), registers.uid);
    jit.exitOnMiss(static_cast<PutMode>(jit.key().form()) == PutMode::Sloppy ? Entry::operationAOTDeleteByNameSloppy : Entry::operationAOTDeleteByName, { .secondArgumentIn = registers.base, .thirdArgumentIn = registers.uid });
}

void generateDeleteByIdCached(StubAssembler& jit)
{
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg identifierNumber = registers.key;
    JumpList hasNoName;

    hasNoName.append(jit.branchIfNotCell(registers.base));
    loadIdentifier(jit, identifierNumber, registers.uid, hasNoName);
    removeNamedPropertyAndReturn(jit, registers);

    hasNoName.link(&jit);
    jit.exitOnMiss(static_cast<PutMode>(jit.key().form()) == PutMode::Sloppy ? Entry::operationAOTDeleteByIdSloppy : Entry::operationAOTDeleteById, { .secondArgumentIn = registers.base, .thirdArgumentIn = identifierNumber });
}

void generateDeleteByValCached(StubAssembler& jit)
{
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    JumpList hasNoName;

    hasNoName.append(jit.branchIfNotCell(registers.key));
    hasNoName.append(jit.branchIfNotCell(registers.base));
    loadNameOfKeyCell(jit, registers, StructureIDOfBase::IsNotLoaded, hasNoName);
    removeNamedPropertyAndReturn(jit, registers);

    hasNoName.link(&jit);
    jit.exitOnMiss(static_cast<PutMode>(jit.key().form()) == PutMode::Sloppy ? Entry::operationAOTDeleteByValSloppy : Entry::operationAOTDeleteByVal, { .secondArgumentIn = registers.base, .thirdArgumentIn = registers.key });
}

static void replaceOwnValueOfKeyAndReturn(StubAssembler& jit, SlotOperation operation, const KeyedProbeRegisters& registers, GPRReg value, JumpList& isInNoTable, JumpList& miss)
{
    isInNoTable.link(&jit);
    countMissOfPair(jit, registers.structureID, registers.uid, registers.header, miss);
    jit.xor64(TrustedImm32(static_cast<int32_t>(accessOf(operation))), registers.uid);
    const IndexRegisterOfObjectLookup index(jit);
    const ObjectLookupRegisters lookup = lookupRegistersOf(registers, jit.reuseScratch(StubRegister::T9), index);
    auto emitRememberingOfPlace = [&] {
        rememberPlaceOfListedValue(jit, SlotAccess::Store, registers, lookup);
    };
    const ScopedLambda<void()> rememberPlaceOfOwnValue(emitRememberingOfPlace);
    const bool isDefinition = operation == SlotOperation::Define;
    replaceOwnValueAndReturn(jit, operation, lookup, ObjectLookupName::IsGivenByUID, value, miss, isDefinition ? InvalidGPRReg : registers.key, isDefinition ? nullptr : &rememberPlaceOfOwnValue);
}

static void storeByKeyedSlot(StubAssembler& jit, SlotOperation operation, const KeyedProbeRegisters& registers, GPRReg value, JumpList& miss)
{
    const GPRReg base = registers.base;
    const GPRReg wordIndex = registers.uid;
    const GPRReg storage = registers.entry;
    JumpList isInNoTable;

    StructureIDOfBase structureIDOfBase = StructureIDOfBase::IsNotLoaded;
    const bool hasPlaces = accessOf(operation) == SlotAccess::Store;
    Label storeAtWordIndex;
    if (hasPlaces) {
        JumpList hasNoPlace;
        Jump placeIsOfNoValue = findKeyedValuePlace(jit, SlotAccess::Store, registers, wordIndex, hasNoPlace);
        storeAtWordIndex = jit.label();
        storeOwnValueAtAndReturn(jit, base, wordIndex, storage, value);

        placeIsOfNoValue.link(&jit);
        jit.load32(inKeyedValuePlace(registers.entry, SlotAccess::Store, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), kindOfPlaceForStores(registers));
        jit.sub32(registers.structureID, kindOfPlaceForStores(registers));
        storeInFieldOfAnyValueAtPlaceAndReturn(jit, operation, registers, value, hasNoPlace);
        addAtPlaceAndReturn(jit, registers, value);
        storeExtraPropertyByPlaceAndReturn(jit, registers, value, hasNoPlace);
        storeInFieldAtPlaceAndReturn(jit, operation, registers, value, hasNoPlace);
        storeAtPlaceInGlobalObjectAndReturn(jit, registers, value);
        hasNoPlace.link(&jit);
        structureIDOfBase = StructureIDOfBase::IsLoaded;
    }
    const ProbedSlot slot = findKeyedSlot(jit, accessOf(operation), registers, structureIDOfBase, miss, isInNoTable);
    const CachedSlot ownValue(slot.form, Slot::Kind::Value, slot.header, slot.payload);
    Jump isNotValue = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Value);
    ownValue.loadOwnWordIndex(jit, wordIndex);
    if (hasPlaces) {
        rememberKeyedValuePlace(jit, SlotAccess::Store, registers, wordIndex);
        jit.jump().linkTo(storeAtWordIndex, &jit);
    } else {
        jit.move(base, storage);
        Jump isInline = ownValue.branchUnlessFlag(jit, Slot::isOutOfLine);
        jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
        isInline.link(&jit);
        jit.store64(value, BaseIndex(storage, wordIndex, CCallHelpers::TimesEight));
        returnAfterStoring(jit, base, value);
    }

    isNotValue.link(&jit);
    jumpToSlotHandlerOfKind(jit, slotHandlerTable(operation, SlotOrigin::ByVal, slot.form), slot.header, registers.uid);

    replaceOwnValueOfKeyAndReturn(jit, operation, registers, value, isInNoTable, miss);
}

static Entry cachingOperationFor(PutMode mode)
{
    switch (mode) {
    case PutMode::Strict:
        return Entry::operationAOTPutByValAndCache;
    case PutMode::Sloppy:
        return Entry::operationAOTPutByValAndCacheSloppy;
    case PutMode::Define:
        return Entry::operationAOTDefineField;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void exitOnStoreMiss(StubAssembler& jit, PutMode mode, const KeyedProbeRegisters& registers, GPRReg value)
{
    jit.exitOnMiss(cachingOperationFor(mode), {
        .returns = OperationCall::Returns::Void,
        .secondArgumentIn = registers.base,
        .thirdArgumentIn = registers.key,
        .fourthArgumentIn = value,
    });
}

void generatePutByValCached(StubAssembler& jit)
{
    const PutMode mode = static_cast<PutMode>(jit.key().form());
    RELEASE_ASSERT(mode == PutMode::Strict || mode == PutMode::Sloppy);
    const StubKey storeElement = putByVal(false, mode == PutMode::Strict ? ECMAMode::strict() : ECMAMode::sloppy());
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    const GPRReg value = jit.clobberOperand(2);
    RELEASE_ASSERT(signatureOf(storeElement).operands == jit.signature().operands);

    jit.linkToStub(jit.branchIfNotCell(key), storeElement);
    jit.linkToStub(jit.branchIfNotCell(base), storeElement);

    JumpList miss;
    storeByKeyedSlot(jit, operationFor(mode), registers, value, miss);

    miss.link(&jit);
    JumpList leafStores;
    loadIndexOfText(jit, registers, leafStores, ElementUnderText::MayBeAdded);
    boxInt32(jit, registers.entry, key);
    jit.jumpToStub(storeElement);
    leafStores.link(&jit);
    callLeafLookup(jit, Entry::operationAOTPutByValGeneric, key, value, registers.header);
    Jump cannotStore = jit.branchTest32(CCallHelpers::Zero, base);
    jit.returnToCaller();

    cannotStore.link(&jit);
    jit.move(registers.header, base);
    exitOnStoreMiss(jit, mode, registers, value);
}

static void defineOwnElementAndReturn(StubAssembler& jit, const KeyedProbeRegisters& registers, GPRReg value, JumpList& miss)
{
    const GPRReg base = registers.base;
    const GPRReg shape = registers.header;
    const GPRReg storage = registers.header;
    const GPRReg index = registers.uid;
    const GPRReg newLength = registers.payload;

    miss.append(jit.branchIfNotInt32(registers.key));
    miss.append(jit.branchIfNotCell(base));
    jit.load8(Address(base, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IsArray | IndexingShapeMask | CopyOnWrite), shape);
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ArrayWithContiguous));
    Jump isNoArrayOfInt32 = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(ArrayWithInt32));
    miss.append(jit.branchIfNotInt32(value));
    isContiguous.link(&jit);
    Label loadStorage = jit.label();
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.zeroExtend32ToWord(registers.key, index);
    Jump isWithinLength = jit.branch32(CCallHelpers::Below, index, Address(storage, Butterfly::offsetOfPublicLength()));
    Jump isBeyondVector = jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfVectorLength()));
    jit.add32(TrustedImm32(1), index, newLength);
    jit.store32(newLength, Address(storage, Butterfly::offsetOfPublicLength()));
    isWithinLength.link(&jit);
    jit.store64(value, BaseIndex(storage, index, CCallHelpers::TimesEight));
    returnAfterStoring(jit, base, value);

    isBeyondVector.link(&jit);
    miss.append(jit.branch32(CCallHelpers::NotEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    growFullVector(jit, { base, storage, index, registers.payload, registers.entry, registers.structureID }, miss);
    jit.jump().linkTo(loadStorage, &jit);

    isNoArrayOfInt32.link(&jit);
    Jump holdsNoFractions = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(ArrayWithDouble));
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    miss.append(jit.branchIfNotNumber(value));
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.zeroExtend32ToWord(registers.key, index);
    miss.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfVectorLength())));
    Jump isWhole = jit.branchIfInt32(value);
    unboxDouble(jit, value, newLength, number);
    miss.append(jit.branchIfNaN(number));
    Jump isFraction = jit.jump();
    isWhole.link(&jit);
    jit.convertInt32ToDouble(value, number);
    isFraction.link(&jit);
    Jump liesWithinLength = jit.branch32(CCallHelpers::Below, index, Address(storage, Butterfly::offsetOfPublicLength()));
    jit.add32(TrustedImm32(1), index, newLength);
    jit.store32(newLength, Address(storage, Butterfly::offsetOfPublicLength()));
    liesWithinLength.link(&jit);
    jit.storeDouble(number, BaseIndex(storage, index, CCallHelpers::TimesEight));
    jit.returnToCaller();

    holdsNoFractions.link(&jit);
    miss.append(branchIfNotCompiledObject(jit, base));
    checkStubOperands(storeElementOfCompiledObject(PutMode::Define), { base, registers.key, value });
    jit.jumpToStub(storeElementOfCompiledObject(PutMode::Define));
}

void generateDefineByValCached(StubAssembler& jit)
{
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg value = jit.clobberOperand(2);

    JumpList miss;
    Jump keyIsCell = jit.branchIfCell(registers.key);
    Label defineElement = jit.label();
    defineOwnElementAndReturn(jit, registers, value, miss);

    keyIsCell.link(&jit);
    miss.append(jit.branchIfNotCell(registers.base));
    JumpList isNotFiled;
    storeByKeyedSlot(jit, SlotOperation::Define, registers, value, isNotFiled);

    isNotFiled.link(&jit);
    loadIndexOfText(jit, registers, miss, ElementUnderText::MayBeAdded);
    boxInt32(jit, registers.entry, registers.key);
    jit.jump().linkTo(defineElement, &jit);

    miss.link(&jit);
    exitOnStoreMiss(jit, PutMode::Define, registers, value);
}

void generateToPropertyKey(StubAssembler& jit)
{
    const GPRReg value = jit.clobberOperand(0);
    const GPRReg type = jit.allocateScratch(StubRegister::T11);

    Jump isNotCell = jit.branchIfNotCell(value);
    jit.load8(Address(value, JSCell::typeInfoTypeOffset()), type);
    Jump isString = jit.branch32(CCallHelpers::Equal, type, TrustedImm32(StringType));
    Jump isNotSymbol = jit.branch32(CCallHelpers::NotEqual, type, TrustedImm32(SymbolType));
    isString.link(&jit);
    jit.returnToCaller();

    isNotCell.link(&jit);
    JumpList hasNoKeptText;
    const GPRReg text = jit.allocateScratch(StubRegister::T13);
    hasNoKeptText.append(jit.branchIfNotInt32(value));
    loadKeptTextOfIndex(jit, value, type, jit.allocateScratch(StubRegister::T12), text, hasNoKeptText);
    jit.move(text, jit.result());
    jit.returnToCaller();
    hasNoKeptText.link(&jit);
    isNotSymbol.link(&jit);
    jit.move(jit.operand(0), operationArgumentGPR(1));
    jit.callOperationAndReturn(Entry::operationAOTToKey, { .returns = OperationCall::Returns::Value });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
