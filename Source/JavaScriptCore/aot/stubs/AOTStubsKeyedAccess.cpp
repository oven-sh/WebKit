/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsKeyed.h"

#if ENABLE(AOT)

#include "AOTSlowPaths.h"
#include "AOTStubsElements.h"
#include "AOTStubsField.h"
#include "AOTStubsStrings.h"
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

StubSignature signatureOfKeyedOperands(unsigned numberOfOperands, StubResult result)
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
StubSignature signatureOfInByValCached(StubKey) { return signatureOfKeyedOperands(2, StubResult::GPR); }
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

void callOperationOnOperandsAndReturn(StubAssembler& jit, Entry entry, unsigned numberOfOperands, OperationCall::Returns returns)
{
    RELEASE_ASSERT(numberOfOperands && numberOfOperands <= 3);
    for (unsigned index = numberOfOperands; index--;)
        jit.move(jit.operand(index), operationArgumentGPR(index + 1));
    jit.callOperationAndReturn(entry, { .returns = returns });
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
    jit.emitFunctionEpilogue();
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

#if CPU(X86_64)
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
    jit.x86Lea64(BaseIndex(first, last, CCallHelpers::TimesTwo), entry);
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
#endif

// Overwrites the registers of the probe but the base and the key.
static void loadNameOfKeyCell(StubAssembler& jit, const KeyedProbeRegisters& registers, JumpList& hasNoName)
{
    const GPRReg key = registers.key;
    const GPRReg uid = registers.uid;
    RELEASE_ASSERT(noOverlap(key, uid, instanceGPR));
    Jump isString = jit.branchIfString(key);
    hasNoName.append(jit.branchIfNotSymbol(key));
    hasNoName.append(jit.loadCacheableIdentifierImpl(key, uid, false, true));
    Jump isLoaded = jit.jump();

    isString.link(&jit);
#if CPU(X86_64)
    // AssemblyHelpers::branchIfNotAtomStringImpl(), and where it leaves the text of the key is looked at: a flat string, or a piece of one.
    const GPRReg characters = registers.entry;
    const GPRReg length = registers.structureID;
    jit.loadPtr(Address(key, JSString::offsetOfValue()), uid);
    Jump isKnownAtom = jit.branchTest8(CCallHelpers::NonZero, Address(key, JSCell::typeInfoFlagsOffset()), TrustedImm32(TypeInfoPerCellBit));
    Jump isRope = jit.branchIfRopeStringImpl(uid);
    Jump isAtom = jit.branchTest32(CCallHelpers::NonZero, Address(uid, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsAtom()));
    jit.load32(Address(uid, StringImpl::lengthMemoryOffset()), length);
    jit.loadPtr(Address(uid, StringImpl::dataOffset()), characters);
    Jump hasCharacters = jit.jump();

    isRope.link(&jit);
    loadSubstringCharacters(jit, key, uid, characters, length, hasNoName);

    hasCharacters.link(&jit);
    hasNoName.append(jit.branchTest32(CCallHelpers::Zero, Address(uid, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    findNameOfText(jit, characters, length, registers.header, registers.payload, uid, hasNoName);
    isKnownAtom.link(&jit);
    isAtom.link(&jit);
#else
    hasNoName.append(jit.loadCacheableIdentifierImpl(key, uid, true, false));
#endif
    isLoaded.link(&jit);
}

// The base and the key are cells. Leaves the kind of the slot in the register of the name. At isInNoTable that register has HashedSlot::keyFor() of the name and structureID the StructureID of the base.
static ProbedSlot findKeyedSlot(StubAssembler& jit, SlotAccess access, const KeyedProbeRegisters& registers, JumpList& hasNoName, JumpList& isInNoTable)
{
    loadNameOfKeyCell(jit, registers, hasNoName);
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

static ProbedSlot findKeyedSlot(StubAssembler& jit, SlotAccess access, const KeyedProbeRegisters& registers, JumpList& miss)
{
    return findKeyedSlot(jit, access, registers, miss, miss);
}

#if CPU(X86_64)
// No table has the Structure and the name: mostly because they meet for the first time, as where an object serves as a dictionary. The objects themselves answer, and nothing is filed.
static void answerFromObjectsAndReturn(StubAssembler& jit, ObjectLookupQuestion question, const KeyedProbeRegisters& registers, JumpList& isInNoTable, JumpList& miss)
{
    isInNoTable.link(&jit);
    countMissOfPair(jit, registers.structureID, registers.uid, registers.header, miss);
    answerFromObjectsAndReturn(jit, question, {
        .receiver = registers.base,
        .name = jit.allocateScratch(StubRegister::T9),
        .uid = registers.uid,
        .object = registers.header,
        .structure = registers.structureID,
        .size = registers.payload,
        .vector = registers.entry,
        .index = jit.scratchRegister(),
    }, ObjectLookupName::IsGivenByUID, miss);
}
#endif

static Jump branchIfKeyedSlotKindDiffers(StubAssembler& jit, const KeyedProbeRegisters& registers, Slot::Kind kind)
{
    return jit.branch32(CCallHelpers::NotEqual, registers.uid, TrustedImm32(static_cast<int32_t>(kind)));
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
#if CPU(X86_64)
    JumpList isInNoTable;
#else
    JumpList& isInNoTable = miss;
#endif
    const ProbedSlot slot = findKeyedSlot(jit, SlotAccess::Read, registers, miss, isInNoTable);
    Jump isNotValue = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Value);
    loadValueBySlot(jit, CachedSlot(slot.form, Slot::Kind::Value, slot.header, slot.payload), base, slot.payload, slot.header, base);
    jit.returnToCaller();

    isNotValue.link(&jit);
    jumpToSlotHandlerOfKind(jit, SlotHandlerTable::ReadByVal, header, registers.uid);

#if CPU(X86_64)
    answerFromObjectsAndReturn(jit, ObjectLookupQuestion::Read, registers, isInNoTable, miss);

#endif
    miss.link(&jit);
    callLeafLookup(jit, Entry::operationAOTGetByValGeneric, key, std::nullopt, header);
    Jump hasNoAnswer = jit.branchTest64(CCallHelpers::Zero, base);
    jit.returnToCaller();

    hasNoAnswer.link(&jit);
    jit.move(header, base);
    jit.exitOnMiss(Entry::operationAOTGetByValAndCache, { .secondArgumentIn = base, .thirdArgumentIn = key });
}

static LeafPresence leastPresenceFor(PresenceQuestion question)
{
    switch (question) {
    case PresenceQuestion::In:
        return LeafPresence::Inherited;
    case PresenceQuestion::HasOwn:
        return LeafPresence::Own;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Entry cachingOperationFor(PresenceQuestion question)
{
    switch (question) {
    case PresenceQuestion::In:
        return Entry::operationAOTInByValAndCache;
    case PresenceQuestion::HasOwn:
        return Entry::operationAOTHasOwnByValAndCache;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An element in the object's own storage answers both questions. A hole or an index past the end may be found in another way.
static void returnTrueForOwnElement(StubAssembler& jit, const KeyedProbeRegisters& registers, JumpList& isNotFound)
{
    const GPRReg base = registers.base;
    const GPRReg shape = registers.header;
    const GPRReg storage = registers.header;
    const GPRReg index = registers.uid;

    isNotFound.append(jit.branchIfNotInt32(registers.key));
    isNotFound.append(jit.branchIfNotCell(base));
    jit.load8(Address(base, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
    Jump isDouble = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
    isNotFound.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(Int32Shape)));
    isContiguous.link(&jit);
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.zeroExtend32ToWord(registers.key, index);
    isNotFound.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    isNotFound.append(jit.branchTest64(CCallHelpers::Zero, BaseIndex(storage, index, CCallHelpers::TimesEight)));
    Label isFound = jit.label();
    jit.move(TrustedImm32(1), jit.result());
    jit.returnToCaller();

    isDouble.link(&jit);
    const GPRReg element = registers.payload;
    const GPRReg hole = registers.entry;
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.zeroExtend32ToWord(registers.key, index);
    isNotFound.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    jit.load64(BaseIndex(storage, index, CCallHelpers::TimesEight), element);
    jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), hole);
    jit.branch64(CCallHelpers::NotEqual, element, hole).linkTo(isFound, &jit);
    isNotFound.append(jit.jump());
}

void generateInByValCached(StubAssembler& jit)
{
    const PresenceQuestion question = static_cast<PresenceQuestion>(jit.key().form());
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    const GPRReg header = registers.header;
    JumpList miss;
    JumpList needsOperation;

    Jump keyIsCell = jit.branchIfCell(key);
    returnTrueForOwnElement(jit, registers, needsOperation);

    keyIsCell.link(&jit);
    needsOperation.append(jit.branchIfNotCell(base));
    needsOperation.append(jit.branchIfNotObject(base));
#if CPU(X86_64)
    JumpList isInNoTable;
#else
    JumpList& isInNoTable = miss;
#endif
    const ProbedSlot slot = findKeyedSlot(jit, SlotAccess::Read, registers, miss, isInNoTable);
    Jump isNotField = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Field);
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
    miss.append(jit.jump());

#if CPU(X86_64)
    answerFromObjectsAndReturn(jit, question == PresenceQuestion::In ? ObjectLookupQuestion::In : ObjectLookupQuestion::HasOwn, registers, isInNoTable, miss);

#endif
    miss.link(&jit);
    callLeafLookup(jit, Entry::operationAOTInByValGeneric, key, std::nullopt, header);
    Jump hasNoAnswer = jit.branchTest32(CCallHelpers::Zero, base);
    jit.compare32(CCallHelpers::AboveOrEqual, base, TrustedImm32(static_cast<int32_t>(leastPresenceFor(question))), base);
    jit.returnToCaller();

    hasNoAnswer.link(&jit);
    jit.move(header, base);
    needsOperation.link(&jit);
    jit.exitOnMiss(cachingOperationFor(question), { .secondArgumentIn = base, .thirdArgumentIn = key });
}

#if CPU(X86_64)
// No table has the Structure and the name.
static void replaceOwnValueAndReturn(StubAssembler& jit, SlotOperation operation, const KeyedProbeRegisters& registers, GPRReg value, JumpList& isInNoTable, JumpList& miss)
{
    isInNoTable.link(&jit);
    countMissOfPair(jit, registers.structureID, registers.uid, registers.header, miss);
    // HashedSlot::keyFor(): the search has put the access into the low bits of the name.
    jit.xor64(TrustedImm32(static_cast<int32_t>(accessOf(operation))), registers.uid);
    replaceOwnValueAndReturn(jit, operation, {
        .receiver = registers.base,
        .name = jit.reuseScratch(StubRegister::T9),
        .uid = registers.uid,
        .object = registers.header,
        .structure = registers.structureID,
        .size = registers.payload,
        .vector = registers.entry,
        .index = jit.scratchRegister(),
    }, ObjectLookupName::IsGivenByUID, value, miss);
}
#endif

static void storeByKeyedSlot(StubAssembler& jit, SlotOperation operation, const KeyedProbeRegisters& registers, GPRReg value, JumpList& miss)
{
    const GPRReg base = registers.base;
    const GPRReg wordIndex = registers.uid;
    const GPRReg storage = registers.entry;
    JumpList isInNoTable;

    const ProbedSlot slot = findKeyedSlot(jit, accessOf(operation), registers, miss, isInNoTable);
    const CachedSlot ownValue(slot.form, Slot::Kind::Value, slot.header, slot.payload);
    Jump isNotValue = branchIfKeyedSlotKindDiffers(jit, registers, Slot::Kind::Value);
    ownValue.loadOwnWordIndex(jit, wordIndex);
    jit.move(base, storage);
    Jump isInline = ownValue.branchUnlessFlag(jit, Slot::isOutOfLine);
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    isInline.link(&jit);
    jit.store64(value, BaseIndex(storage, wordIndex, CCallHelpers::TimesEight));
    returnAfterStoring(jit, base, value);

    isNotValue.link(&jit);
    jumpToSlotHandlerOfKind(jit, slotHandlerTable(operation, SlotOrigin::ByVal, slot.form), slot.header, registers.uid);

#if CPU(X86_64)
    replaceOwnValueAndReturn(jit, operation, registers, value, isInNoTable, miss);
#else
    miss.append(isInNoTable);
#endif
}

static ECMAMode ecmaModeOf(PutMode mode)
{
    switch (mode) {
    case PutMode::Strict:
        return ECMAMode::strict();
    case PutMode::Sloppy:
        return ECMAMode::sloppy();
    case PutMode::Define:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    const StubKey storeElement = putByVal(false, ecmaModeOf(mode));
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
    miss.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(ArrayWithInt32)));
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

    isNotCell.link(&jit);
    isNotSymbol.link(&jit);
    callOperationOnOperandsAndReturn(jit, Entry::operationAOTToKey, 1, OperationCall::Returns::Value);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
