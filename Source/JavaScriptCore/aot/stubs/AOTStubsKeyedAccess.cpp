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
#include "JSCJSValue.h"
#include "JSObject.h"
#include "StackAlignment.h"
#include <array>
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

static ProbedSlot findKeyedSlot(StubAssembler& jit, SlotAccess access, const KeyedProbeRegisters& registers, JumpList& miss)
{
    loadNameOfKey(jit, registers.key, registers.uid, registers.structureID, miss);
    jit.load32(Address(registers.base, JSCell::structureIDOffset()), registers.structureID);
    findHashedSlot(jit, access, {
        .structureID = registers.structureID,
        .uid = registers.uid,
        .entry = registers.entry,
        .header = registers.header,
        .payload = registers.payload,
        .scratch = registers.payload,
    }, miss);
    return { SlotForm::Slot, registers.header, registers.payload };
}

void generateGetByValCached(StubAssembler& jit)
{
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg base = registers.base;
    const GPRReg key = registers.key;
    const GPRReg header = registers.header;
    RELEASE_ASSERT(descriptorOf(getByVal(false).family()).signature(getByVal(false)).operands == jit.signature().operands);

    jit.linkToStub(jit.branchIfNotCell(key), getByVal(false));
    jit.linkToStub(jit.branchIfNotCell(base), getByVal(false));
    jit.bindEntry(getByValCachedCells());

    JumpList miss;
    const ProbedSlot slot = findKeyedSlot(jit, SlotAccess::Read, registers, miss);
    Jump isNotValue = branchIfSlotKindDiffers(jit, slot, Slot::Kind::Value, registers.uid);
    loadValueBySlot(jit, CachedSlot(slot.form, Slot::Kind::Value, slot.header, slot.payload), base, registers.uid, registers.structureID, base);
    jit.returnToCaller();

    isNotValue.link(&jit);
    jumpToSlotHandler(jit, SlotHandlerTable::ReadByVal, header, registers.uid);

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
    isNotFound.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(Int32Shape)));
    isContiguous.link(&jit);
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    jit.zeroExtend32ToWord(registers.key, index);
    isNotFound.append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength())));
    isNotFound.append(jit.branchTest64(CCallHelpers::Zero, BaseIndex(storage, index, CCallHelpers::TimesEight)));
    jit.move(TrustedImm32(1), jit.result());
    jit.returnToCaller();
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
    const ProbedSlot slot = findKeyedSlot(jit, SlotAccess::Read, registers, miss);
    Jump isNotField = branchIfSlotKindDiffers(jit, slot, Slot::Kind::Field, registers.uid);
    returnFieldPresence(jit, CachedSlot(SlotForm::Slot, Slot::Kind::Field, header, registers.payload), {
        .object = base,
        .slotData = registers.uid,
        .extensionWordOffset = registers.structureID,
        .address = registers.structureID,
        .representation = registers.entry,
    }, needsOperation);

    isNotField.link(&jit);
    returnPresenceBySlot(jit, question, slot, base, miss);

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

static void storeByKeyedSlot(StubAssembler& jit, SlotOperation operation, const KeyedProbeRegisters& registers, GPRReg value, JumpList& miss)
{
    const GPRReg base = registers.base;
    const GPRReg wordIndex = registers.uid;
    const GPRReg storage = registers.entry;

    const ProbedSlot slot = findKeyedSlot(jit, accessOf(operation), registers, miss);
    const CachedSlot ownValue(slot.form, Slot::Kind::Value, slot.header, slot.payload);
    Jump isNotValue = branchIfSlotKindDiffers(jit, slot, Slot::Kind::Value, registers.uid);
    ownValue.loadOwnWordIndex(jit, wordIndex);
    jit.move(base, storage);
    Jump isInline = ownValue.branchUnlessFlag(jit, Slot::isOutOfLine);
    jit.loadPtr(Address(base, JSObject::butterflyOffset()), storage);
    isInline.link(&jit);
    jit.store64(value, BaseIndex(storage, wordIndex, CCallHelpers::TimesEight));
    returnAfterStoring(jit, base, value);

    isNotValue.link(&jit);
    jumpToSlotHandler(jit, slotHandlerTable(operation, SlotOrigin::ByVal, slot.form), slot.header, registers.uid);
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
    RELEASE_ASSERT(descriptorOf(storeElement.family()).signature(storeElement).operands == jit.signature().operands);

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

void generateDefineByValCached(StubAssembler& jit)
{
    const KeyedProbeRegisters registers = allocateKeyedProbeRegisters(jit);
    const GPRReg value = jit.clobberOperand(2);

    JumpList miss;
    miss.append(jit.branchIfNotCell(registers.key));
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
