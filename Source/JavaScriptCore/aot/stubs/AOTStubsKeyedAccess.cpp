/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsKeyed.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTRuntimeTable.h"
#include "AOTStubRegisters.h"
#include "AOTStubsElements.h"
#include "AOTStubsField.h"
#include "AOTStubsPropertyAccessors.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "AOTStubsPropertyProbes.h"
#include "AOTStubsStrings.h"
#include "AOTThreshold.h"
#include "JSArrayBufferView.h"
#include "JSCJSValue.h"
#include "JSObject.h"
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

void callLeafLookup(StubAssembler& jit, Entry entry, GPRReg second, std::optional<GPRReg> value, GPRReg baseAfterCall)
{
    const GPRReg base = jit.clobberOperand(0);
    RELEASE_ASSERT(base == GPRInfo::returnValueGPR);
    RELEASE_ASSERT(noOverlap(base, second, baseAfterCall) && value != base && value != baseAfterCall);

    if (descriptorOf(jit.key().family()).clobbers == StubClobbers::ClobbersOnlyScratch) {
        jit.move(base, baseAfterCall);
        jit.emitFunctionPrologue();
#if CPU(X86_64)
        // The caller may have pushed. preservingRegistersClobberedBy expects the stack pointer of a stub's entry.
        jit.andPtr(TrustedImm32(-static_cast<int32_t>(stackAlignmentBytes())), CCallHelpers::stackPointerRegister);
        jit.subPtr(TrustedImm32(sizeof(CPURegister)), CCallHelpers::stackPointerRegister);
#endif
        jit.preservingRegistersClobberedBy(StubCallee::Operation, true, [&](unsigned) {
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

// A trap is handed an index as its text and asks its target under that text. Goes on where the base may have elements and the key is the text of an index of at most four digits, with the index in registers.entry. Preserves base and key.
static void loadIndexOfText(StubAssembler& jit, const KeyedProbeRegisters& registers, JumpList& isNoSuchKey)
{
    const GPRReg key = registers.key;
    const GPRReg characters = registers.uid;
    const GPRReg length = registers.structureID;
    const GPRReg index = registers.entry;
    const GPRReg digit = registers.header;
    const GPRReg position = registers.payload;
    AOT_THRESHOLD(int32_t, maxDigits, 4);

    // The elements of a compiled object are those of its extra properties.
    Jump hasIndexedStorage = jit.branchTest8(CCallHelpers::NonZero, Address(registers.base, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask));
    isNoSuchKey.append(jit.branchIfNotType(registers.base, CompiledObjectType));
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
    // "01" is a name.
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

// characters and length say a text of 8-bit characters. Leaves the name that PropertyCacheWords::namesOfTexts has for it in uid. Overwrites all its registers.
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

// One operand of x86-64 reaches a place by its distance from the instance. Elsewhere the register of the place has its address.
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

// The base and the key are cells. Leaves Slot::wordIndexFor() of the value in wordIndex. Hands back the jump that is taken where the place is of the key and not of a value of the Structure.
static Jump findKeyedValuePlace(StubAssembler& jit, SlotAccess access, const KeyedProbeRegisters& registers, GPRReg wordIndex, JumpList& isNotFound)
{
    RELEASE_ASSERT(wordIndex != registers.entry);
    jit.load32(Address(registers.base, JSCell::structureIDOffset()), registers.structureID);
    locateKeyedValuePlace(jit, access, registers.key, registers.structureID, registers.entry);
    isNotFound.append(jit.branchPtr(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, key)), registers.key));
    Jump isOfNoValue = jit.branch32(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), registers.structureID);
    jit.load32SignedExtendTo64(inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, wordIndex)), wordIndex);
    return isOfNoValue;
}

// The place is of the key. Goes on where it says that the objects of the Structure find nothing under the key.
static void branchUnlessPlaceSaysAbsent(StubAssembler& jit, const KeyedProbeRegisters& registers, GPRReg scratch, JumpList& doesNotSay)
{
    RELEASE_ASSERT(noOverlap(scratch, registers.base, registers.key, registers.structureID, registers.entry));
    jit.add32(TrustedImm32(KeyedValuePlace::absenceBit), registers.structureID, scratch);
    doesNotSay.append(jit.branch32(CCallHelpers::NotEqual, inKeyedValuePlace(registers.entry, SlotAccess::Read, OBJECT_OFFSETOF(KeyedValuePlace, structureID)), scratch));
}

// The place is of the key. Goes on where it says in which slot of the cell the objects of the Structure have their field, with the address and the representation of that slot.
static FieldAddressRegisters findFieldByPlace(StubAssembler& jit, const KeyedProbeRegisters& registers, JumpList& doesNotSay)
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
    computeFieldAddress(jit, field, false, doesNotSay);
    return field;
}

// A slot of the hashed table names a field. In which slot of the cell a field lies holds for as long as the Structure lives. What an empty slot means is left to the tables.
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

// A slot of the hashed table has said where the own value is. What it says holds for as long as the Structure lives.
static void rememberKeyedValuePlace(StubAssembler& jit, SlotAccess access, const KeyedProbeRegisters& registers, GPRReg wordIndex)
{
    RELEASE_ASSERT(noOverlap(wordIndex, registers.base, registers.key, registers.structureID, registers.entry));
    locateKeyedValuePlace(jit, access, registers.key, registers.structureID, registers.entry);
    jit.storePtr(registers.key, inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, key)));
    jit.store32(registers.structureID, inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, structureID)));
    jit.store32(wordIndex, inKeyedValuePlace(registers.entry, access, OBJECT_OFFSETOF(KeyedValuePlace, wordIndex)));
}

// A word index below zero counts from the out-of-line storage.
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

enum class StructureIDOfBase : bool { IsNotLoaded, IsLoaded };

// Overwrites the registers of the probe but the base and the key, and the StructureID where that is loaded.
static void loadNameOfKeyCell(StubAssembler& jit, const KeyedProbeRegisters& registers, StructureIDOfBase structureIDOfBase, JumpList& hasNoName)
{
    const GPRReg key = registers.key;
    const GPRReg uid = registers.uid;
    RELEASE_ASSERT(noOverlap(key, uid, instanceGPR));
    Jump isString = jit.branchIfString(key);
    hasNoName.append(jit.branchIfNotSymbol(key));
    hasNoName.append(jit.loadCacheableIdentifierImpl(key, uid, false, true));
    Jump isLoaded = jit.jump();

    isString.link(&jit);
    // AssemblyHelpers::branchIfNotAtomStringImpl(), and where it leaves the text of the key is looked at: a flat string, or a piece of one.
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
    // The first word of a piece holds its three flags and nothing else, so the bits above them count how often the piece has come here. The third time it leaves for the operation, which puts the name in its place.
    constexpr int32_t oneVisitOfPiece = 1 << 3;
    constexpr int32_t twoVisitsOfPiece = 2 * oneVisitOfPiece;
    static_assert(!(~JSRopeString::stringMask & (oneVisitOfPiece | twoVisitsOfPiece)) && (~JSRopeString::stringMask | oneVisitOfPiece | twoVisitsOfPiece) <= UINT8_MAX);
    hasNoName.append(jit.branchTestPtr(CCallHelpers::NonZero, uid, TrustedImm32(twoVisitsOfPiece)));
    jit.add8(TrustedImm32(oneVisitOfPiece), Address(key, JSString::offsetOfValue()));
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

// The base and the key are cells. Leaves the kind of the slot in the register of the name. At isInNoTable that register has HashedSlot::keyFor() of the name and structureID the StructureID of the base.
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

static ObjectLookupRegisters lookupRegistersOf(StubAssembler& jit, const KeyedProbeRegisters& registers, GPRReg name)
{
    return {
        .receiver = registers.base,
        .name = name,
        .uid = registers.uid,
        .object = registers.header,
        .structure = registers.structureID,
        .size = registers.payload,
        .vector = registers.entry,
        .index = indexRegisterOfObjectLookup(jit),
    };
}

// The property table of lookup.object has listed a plain value, with the offset above the eight bits of the attributes in lookup.structure, which is kept. Writes size, vector and index.
// Where the objects of a Structure keep a value stays for as long as the Structure lives, unless it is a dictionary that gives a name up and remains the same Structure.
static void rememberPlaceOfListedValue(StubAssembler& jit, SlotAccess access, const KeyedProbeRegisters& registers, const ObjectLookupRegisters& lookup)
{
    // Structure::isUncacheableDictionary(): of the two bits of the kind, the upper one.
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
    // Slot::wordIndexFor().
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

// No table has the Structure and the name: mostly because they meet for the first time, as where an object serves as a dictionary. The objects themselves answer, and nothing is filed.
static void answerFromObjectsAndReturn(StubAssembler& jit, ObjectLookupQuestion question, const KeyedProbeRegisters& registers, JumpList& isInNoTable, JumpList& miss)
{
    isInNoTable.link(&jit);
    countMissOfPair(jit, registers.structureID, registers.uid, registers.header, miss);
    const ObjectLookupRegisters lookup = lookupRegistersOf(jit, registers, jit.allocateScratch(StubRegister::T9));
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

// The base and the key are cells. Returns the own value or the field that a place names, undefined where the place says so, and the value that a slot of the hashed table names. Goes on with a slot of another kind.
// A field whose slot does not say its value is asked of the hashed table, or left to emptyFieldSlot.
static KeyedValueRead returnValueOfKey(StubAssembler& jit, const KeyedProbeRegisters& registers, JumpList& hasNoName, JumpList& isInNoTable, JumpList* emptyFieldSlot)
{
    const GPRReg base = registers.base;
    const GPRReg header = registers.header;
    JumpList hasNoPlace;
    JumpList placeIsOfNoField;
    Jump placeIsOfNoValue = findKeyedValuePlace(jit, SlotAccess::Read, registers, header, hasNoPlace);
    Label returnValueAtWordIndex = jit.label();
    returnOwnValueAt(jit, base, header);

    placeIsOfNoValue.link(&jit);
    Label readFieldByPlace = jit.label();
    FieldReadExits fieldIsNotRead;
    readFieldAndReturn(jit, {
        .field = findFieldByPlace(jit, registers, placeIsOfNoField),
        .value = registers.uid,
        .result = base,
        .number = jit.allocateScratch(FPRInfo::fpRegT2),
        .numberScratch = jit.allocateScratch(FPRInfo::fpRegT3),
    }, fieldIsNotRead);

    placeIsOfNoField.link(&jit);
    branchUnlessPlaceSaysAbsent(jit, registers, header, hasNoPlace);
    jit.move(TrustedImm64(JSValue::ValueUndefined), base);
    jit.returnToCaller();

    hasNoPlace.link(&jit);
    if (emptyFieldSlot) {
        emptyFieldSlot->append(fieldIsNotRead.isAbsent);
        emptyFieldSlot->append(fieldIsNotRead.cannot);
    } else {
        fieldIsNotRead.isAbsent.link(&jit);
        fieldIsNotRead.cannot.link(&jit);
    }
    const ProbedSlot slot = findKeyedSlot(jit, SlotAccess::Read, registers, StructureIDOfBase::IsLoaded, hasNoName, isInNoTable);
    Jump isNotValue = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Value);
    // An own property has no holder: Slot::createInStorage().
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

void generateGetByValCached(StubAssembler& jit)
{
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    const GPRReg header = registers.header;
    RELEASE_ASSERT(signatureOf(getByVal(false)).operands == jit.signature().operands);

    jit.linkToStub(jit.branchIfNotCell(key), getByVal(false));
    jit.linkToStub(jit.branchIfNotCell(base), getByVal(false));
    jit.bindEntry(getByValCachedCells());

    JumpList miss;
    JumpList isInNoTable;
    const ProbedSlot slot = returnValueOfKey(jit, registers, miss, isInNoTable, nullptr).slot;
    Jump isNotField = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Field);
    rememberFieldOfKey(jit, registers, slot);
    isNotField.link(&jit);
    jumpToSlotHandlerOfKind(jit, SlotHandlerTable::ReadByVal, header, registers.uid);

    answerFromObjectsAndReturn(jit, ObjectLookupQuestion::Read, registers, isInNoTable, miss);

    miss.link(&jit);
    JumpList leafAnswers;
    loadIndexOfText(jit, registers, leafAnswers);
    boxInt32(jit, registers.entry, key);
    jit.jumpToStub(getByVal(false));
    leafAnswers.link(&jit);
    callLeafLookup(jit, Entry::operationAOTGetByValGeneric, key, std::nullopt, header);
    Jump hasNoAnswer = jit.branchTest64(CCallHelpers::Zero, base);
    jit.returnToCaller();

    hasNoAnswer.link(&jit);
    jit.move(header, base);
    jit.exitOnMiss(Entry::operationAOTGetByValAndCache, { .secondArgumentIn = base, .thirdArgumentIn = key });
}

// Looks a key up in one object for another receiver, as super[key] and Reflect.get() do. A data property is the same for every receiver, so whatever is filed for the object that the search starts at says it. A getter gets the receiver.
void generateGetSuperByVal(StubAssembler& jit)
{
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    const GPRReg receiver = jit.operand(2);
    const GPRReg start = jit.allocateScratch(StubRegister::T9);
    const GPRReg getterSetter = registers.uid;
    static_assert(areDistinctOnEveryISA(firstStubOperandRegister, keyCellRegister, storedValueByValRegister, StubRegister::T9, StubRegister::T10, StubRegister::T11, StubRegister::T14, slotHeaderRegister, slotPayloadRegister));
    RELEASE_ASSERT(noOverlap(base, key, receiver, start, registers.uid, registers.structureID, registers.entry, registers.header, registers.payload));
    JumpList nothingIsFiled;
    JumpList whatIsFiledDoesNotAnswer;

    jit.move(base, start);
    Jump startsAtNoCell = jit.branchIfNotCell(base);
    whatIsFiledDoesNotAnswer.append(jit.branchIfNotCell(key));
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

    // Own data properties, which the handlers of reads by key read without a look at anything else.
    isNotFieldGetter.link(&jit);
    JumpList handlerAnswers;
    for (Slot::Kind kind : { Slot::Kind::ArrayLength, Slot::Kind::FarValue, Slot::Kind::Constant })
        handlerAnswers.append(jit.branch32(CCallHelpers::Equal, registers.uid, TrustedImm32(static_cast<int32_t>(kind))));
    whatIsFiledDoesNotAnswer.append(jit.jump());
    handlerAnswers.link(&jit);
    jumpToSlotHandlerOfKind(jit, SlotHandlerTable::ReadByVal, read.slot.header, registers.uid);

    // It calls nothing, so what it answers hangs on no receiver: an element under a text that reads as an index, and a data property, which it files. The receiver is kept across the call as the value of a store is.
    nothingIsFiled.link(&jit);
    jit.move(start, base);
    // An element in plain storage is the same for every receiver.
    JumpList leafAnswers;
    {
        const GPRReg index = registers.entry;
        const GPRReg shape = registers.uid;
        const GPRReg storage = registers.uid;
        const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
        const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT1);
        loadIndexOfText(jit, registers, leafAnswers);
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

// An element in the object's own storage answers both questions. Where there is none, what stands above the object says whether the index may be found in another way.
// Where the caller asks for them, proxies leave by isProxy and nothing else does.
static void returnPresenceOfElement(StubAssembler& jit, PresenceQuestion question, const KeyedProbeRegisters& registers, JumpList& doesNotSay, JumpList* isProxy)
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

    // Leaves by hasOtherStorage with the shape in its register.
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
    JumpList isNoElementOfBase;
    JumpList hasNoSuchStorage;
    doesNotSay.append(jit.branchIfNotInt32(key));
    doesNotSay.append(jit.branchIfNotCell(base));
    returnTrueForElementOf(base, true, isAbsentInOwnStorage, hasNoSuchStorage);

    // An array answers for its length and leaves its elements to its storage.
    isAbsentInOwnStorage.link(&jit);
    isNoElementOfBase.append(jit.branchIfType(base, ArrayType));
    Label isOrdinaryUnlessFlagged = jit.label();
    doesNotSay.append(jit.branchTest8(CCallHelpers::NonZero, Address(base, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetOwnPropertySlot)));

    // A negative key is a name.
    isNoElementOfBase.link(&jit);
    Label hasNoElement = jit.label();
    doesNotSay.append(jit.branch32(CCallHelpers::LessThan, key, TrustedImm32(0)));
    JumpList isFoundNowhere;
    if (asksIn) {
        // As a read of an absent element: nothing above a pristine array has elements, and the Object.prototype of the realm has none and takes none. Whatever else stands above is looked at.
        const GPRReg above = structure;
        loadStructure(jit, base, structure);
        isFoundNowhere.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_hasPristineElementsBits)));
        Label hasStructure = jit.label();
        jit.load64(Address(structure, Structure::prototypeOffset()), above);
        isFoundNowhere.append(jit.branch64(CCallHelpers::Equal, above, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.objectPrototype))));
        Jump isNoCell = jit.branchIfNotCell(above);
        // Where each object has its own prototype the word is empty.
        doesNotSay.append(jit.branchTest64(CCallHelpers::Zero, above));
        doesNotSay.append(jit.branchTest8(CCallHelpers::NonZero, Address(above, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetOwnPropertySlot)));
        doesNotSay.append(jit.branchTest8(CCallHelpers::NonZero, Address(above, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask)));
        loadStructure(jit, above, structure);
        jit.jump().linkTo(hasStructure, &jit);

        isNoCell.link(&jit);
        doesNotSay.append(jit.branch64(CCallHelpers::NotEqual, above, TrustedImm64(JSValue::ValueNull)));
    }
    isFoundNowhere.link(&jit);
    jit.move(TrustedImm32(0), jit.result());
    jit.returnToCaller();

    hasNoSuchStorage.link(&jit);
    doesNotSay.append(jit.branchTest32(CCallHelpers::NonZero, shape));
    jit.load8(Address(base, JSCell::typeInfoTypeOffset()), type);
    Jump isNotCompiledObject = jit.branch32(CCallHelpers::NotEqual, type, TrustedImm32(CompiledObjectType));
    // The elements of a compiled object are those of its extra properties.
    JumpList isAbsentInExtraProperties;
    JumpList extraPropertiesHaveOtherStorage;
    const GPRReg hiddenWordOffset = type;
    jit.branchTest32(CCallHelpers::Zero, Address(base, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)).linkTo(hasNoElement, &jit);
    loadStructure(jit, base, structure);
    jit.load32(Address(structure, CompiledStructure::offsetOfExtraPropertiesSlot()), hiddenWordOffset);
    doesNotSay.append(jit.branchTest32(CCallHelpers::Zero, hiddenWordOffset, TrustedImm32(SlotData::maxOffset)));
    jit.and32(TrustedImm32(SlotData::maxOffset), hiddenWordOffset);
    loadExtraProperties(jit, base, hiddenWordOffset, extraProperties, doesNotSay);
    returnTrueForElementOf(extraProperties, false, isAbsentInExtraProperties, extraPropertiesHaveOtherStorage);

    isAbsentInExtraProperties.linkTo(hasNoElement, &jit);
    extraPropertiesHaveOtherStorage.link(&jit);
    jit.branchTest32(CCallHelpers::Zero, shape).linkTo(hasNoElement, &jit);
    doesNotSay.append(jit.jump());

    isNotCompiledObject.link(&jit);
    if (isProxy)
        isProxy->append(jit.branch32(CCallHelpers::Equal, type, TrustedImm32(ProxyObjectType)));
    // A typed array has the indexes below its length and lets nobody else answer for a number. A length may exceed 31 bits, so a negative key must not look like an index.
    const GPRReg length = storage;
    jit.sub32(type, TrustedImm32(FirstTypedArrayType), length);
    Jump isNoTypedArray = jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(NumberOfTypedArrayTypesExcludingDataView));
    doesNotSay.append(jit.branchTest8(CCallHelpers::NonZero, Address(base, JSArrayBufferView::offsetOfMode()), TrustedImm32(isResizableOrGrowableSharedMode)));
    jit.signExtend32ToPtr(key, index);
    jit.loadPtr(Address(base, JSArrayBufferView::offsetOfLength()), length);
    jit.compare64(CCallHelpers::Below, index, length, jit.result());
    jit.returnToCaller();

    // `in` throws for what is no object.
    isNoTypedArray.link(&jit);
    doesNotSay.append(jit.branch32(CCallHelpers::Below, type, TrustedImm32(ObjectType)));
    jit.jump().linkTo(isOrdinaryUnlessFlagged, &jit);
}

void generateInByValCached(StubAssembler& jit)
{
    const PresenceQuestion question = static_cast<PresenceQuestion>(jit.key().form());
    const bool asksIn = question == PresenceQuestion::In;
    RELEASE_ASSERT(asksIn || question == PresenceQuestion::HasOwn);
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    const GPRReg header = registers.header;
    JumpList miss;
    JumpList needsOperation;

    Jump keyIsCell = jit.branchIfCell(key);
    Label asksForIndex = jit.label();
    if (asksIn && jit.options().callsTrapsOfProxies.predicted()) {
        JumpList isProxy;
        returnPresenceOfElement(jit, question, registers, needsOperation, &isProxy);

        isProxy.link(&jit);
        checkStubOperands(throughProxy(TrapOfProxy::Has), { base, registers.payload, header });
        loadKeptTextOfIndex(jit, key, registers.structureID, registers.uid, registers.payload, needsOperation);
        jit.move(TrustedImm32(0), header);
        jit.jumpToStub(throughProxy(TrapOfProxy::Has));
    } else
        returnPresenceOfElement(jit, question, registers, needsOperation, nullptr);

    keyIsCell.link(&jit);
    needsOperation.append(jit.branchIfNotCell(base));
    // A read of a string, a symbol or a bigint fills a place too, and `in` throws for those.
    needsOperation.append(jit.branchIfNotObject(base));
    JumpList hasNoPlace;
    JumpList placeIsOfNoField;
    JumpList isPresent;
    Jump placeIsOfNoValue = findKeyedValuePlace(jit, SlotAccess::Read, registers, header, hasNoPlace);
    Label returnTrue = jit.label();
    jit.move(TrustedImm32(1), jit.result());
    jit.returnToCaller();

    placeIsOfNoValue.link(&jit);
    branchOnFieldPresence(jit, findFieldByPlace(jit, registers, placeIsOfNoField), registers.uid, isPresent, hasNoPlace);
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
    returnFieldPresence(jit, CachedSlot(SlotForm::Slot, Slot::Kind::Field, header, registers.payload), {
        .object = base,
        .slotData = registers.uid,
        .extensionWordOffset = registers.structureID,
        .address = registers.structureID,
        .representation = registers.entry,
    }, needsOperation);

    isNotField.link(&jit);
    JumpList kindDoesNotSay;
    returnPresenceBySlot(jit, question, slot, base, kindDoesNotSay, KindOfProbedSlot::IsInT10);

    kindDoesNotSay.link(&jit);
    loadSlotKind(jit, slot, registers.uid);
    jit.branch32(CCallHelpers::Equal, registers.uid, TrustedImm32(static_cast<int32_t>(Slot::Kind::FieldGetter))).linkTo(readsSlot, &jit);
    if (asksIn && jit.options().callsTrapsOfProxies.predicted()) {
        RELEASE_ASSERT(base == gprOf(firstStubOperandRegister) && registers.payload == gprOf(slotPayloadRegister));
        miss.append(jit.branch32(CCallHelpers::NotEqual, registers.uid, TrustedImm32(static_cast<int32_t>(Slot::Kind::Uncacheable))));
        jit.linkToStub(jit.branchTest64(CCallHelpers::NonZero, registers.payload), throughProxy(TrapOfProxy::Has));
    }
    miss.append(jit.jump());

    answerFromObjectsAndReturn(jit, asksIn ? ObjectLookupQuestion::In : ObjectLookupQuestion::HasOwn, registers, isInNoTable, miss);

    miss.link(&jit);
    JumpList leafAnswers;
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

static void returnTrue(StubAssembler& jit)
{
    jit.move(TrustedImm32(1), jit.result());
    jit.returnToCaller();
}

// While a collector marks, every cell owes a barrier, so nothing here is seen half done.
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
    returnTrue(jit);

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
    returnTrue(jit);

    isNotAmongExtraProperties.link(&jit);
    Jump isNoField = jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(static_cast<int32_t>(Slot::Kind::FieldRemoval)));
    const CachedSlot fieldRemoval(probed.form, Slot::Kind::FieldRemoval, probed.header, probed.payload);
    needsOperation.append(jit.branch32(CCallHelpers::NotEqual, Address(base, offsetOfOrderWord), fieldRemoval.registerOfFirstPayloadWord()));
    fieldRemoval.loadSecondPayloadWord(jit, scratch);
    jit.store32(scratch, Address(base, offsetOfOrderWord));
    const GPRReg pattern = registers.key;
    const GPRReg offset = registers.entry;
    const BaseIndex slotOfField(base, offset, CCallHelpers::TimesOne);
    fieldRemoval.loadHiddenWordOffset(jit, offset);
    jit.urshift64(probed.header, TrustedImm32(Slot::flagsShiftInHeader + Slot::absentPatternShift), pattern);
    jit.and32(TrustedImm32(Slot::absentPatternMask), pattern);
    JumpList isCleared;
    auto writeIfPatternIs = [&](AbsentPattern candidate, unsigned width, uint64_t bits) {
        Jump isOther = jit.branch32(CCallHelpers::NotEqual, pattern, TrustedImm32(static_cast<int32_t>(candidate)));
        jit.move(TrustedImm64(static_cast<int64_t>(bits)), scratch);
        if (width == sizeof(uint64_t))
            jit.store64(scratch, slotOfField);
        else if (width == sizeof(uint32_t))
            jit.store32(scratch, slotOfField);
        else
            jit.store8(scratch, slotOfField);
        isCleared.append(jit.jump());
        isOther.link(&jit);
    };
    writeIfPatternIs(AbsentPattern::ZeroInEightBytes, sizeof(uint64_t), 0);
    writeIfPatternIs(AbsentPattern::ZeroInFourBytes, sizeof(uint32_t), 0);
    writeIfPatternIs(AbsentPattern::Int32, sizeof(uint32_t), static_cast<uint32_t>(absentInt32));
    writeIfPatternIs(AbsentPattern::Double, sizeof(uint64_t), absentDouble);
    writeIfPatternIs(AbsentPattern::Integer, sizeof(uint64_t), static_cast<uint64_t>(absentInteger));
    static_assert(absentPatternOf(SlotRepresentation::OptionalBoolean) == AbsentPattern::ZeroInOneByte);
    jit.move(TrustedImm32(0), scratch);
    jit.store8(scratch, slotOfField);
    isCleared.link(&jit);
    Jump staysAsBorn = fieldRemoval.branchUnlessFlag(jit, Slot::leavesStateOfBirth);
    jit.load8(Address(base, JSCell::typeInfoFlagsOffset()), scratch);
    jit.or32(TrustedImm32(TypeInfoPerCellBit), scratch);
    jit.store8(scratch, Address(base, JSCell::typeInfoFlagsOffset()));
    staysAsBorn.link(&jit);
    returnTrue(jit);

    isNoField.link(&jit);
    returnTrue(jit);
}

// The base is a cell and its name is in registers.uid, where it stays for the operation. The register of the key is free.
static void removeNamedPropertyAndReturn(StubAssembler& jit, const KeyedProbeRegisters& registers)
{
    JumpList needsOperation;
    JumpList isInNoTable;
    // A dictionary gives a name up and remains the same Structure.
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

// No table has the Structure and the name.
static void replaceOwnValueAndReturn(StubAssembler& jit, SlotOperation operation, const KeyedProbeRegisters& registers, GPRReg value, JumpList& isInNoTable, JumpList& miss)
{
    isInNoTable.link(&jit);
    countMissOfPair(jit, registers.structureID, registers.uid, registers.header, miss);
    // HashedSlot::keyFor(): the search has put the access into the low bits of the name.
    jit.xor64(TrustedImm32(static_cast<int32_t>(accessOf(operation))), registers.uid);
    const ObjectLookupRegisters lookup = lookupRegistersOf(jit, registers, jit.reuseScratch(StubRegister::T9));
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
        hasNoPlace.append(findKeyedValuePlace(jit, SlotAccess::Store, registers, wordIndex, hasNoPlace));
        storeAtWordIndex = jit.label();
        storeOwnValueAtAndReturn(jit, base, wordIndex, storage, value);

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

    replaceOwnValueAndReturn(jit, operation, registers, value, isInNoTable, miss);
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
    loadIndexOfText(jit, registers, leafStores);
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
    miss.append(jit.branchIfNotType(base, CompiledObjectType));
    checkStubOperands(storeElementOfCompiledObject(PutMode::Define), { base, registers.key, value });
    jit.jumpToStub(storeElementOfCompiledObject(PutMode::Define));
}

void generateDefineByValCached(StubAssembler& jit)
{
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg value = jit.clobberOperand(2);

    JumpList miss;
    Jump keyIsCell = jit.branchIfCell(registers.key);
    defineOwnElementAndReturn(jit, registers, value, miss);

    keyIsCell.link(&jit);
    miss.append(jit.branchIfNotCell(registers.base));
    storeByKeyedSlot(jit, SlotOperation::Define, registers, value, miss);

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

    // The texts of the small indexes are kept once they have been asked for.
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
