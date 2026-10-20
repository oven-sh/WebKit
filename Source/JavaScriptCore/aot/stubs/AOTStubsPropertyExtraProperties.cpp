/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyExtraProperties.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTInstance.h"
#include "AOTLimits.h"
#include "AOTPropertyCachesFront.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubsBarriers.h"
#include "Allocator.h"
#include "CompleteSubspace.h"
#include "JSObject.h"
#include "MarkedSpace.h"
#include "Structure.h"
#include <type_traits>
#include <utility>

namespace JSC { namespace AOT {

static_assert(exists(getExtraPropertyFrontEnd()) && exists(putExtraPropertyFrontEnd()));
static_assert(widthOf(hiddenReferenceRepresentation(false)) == sizeof(uint32_t) && widthOf(hiddenReferenceRepresentation(true)) == sizeof(uint64_t));
static_assert(representsAbsentAsZero(hiddenReferenceRepresentation(false)) && representsAbsentAsZero(hiddenReferenceRepresentation(true)));
static_assert(sizeof(StructureID) == sizeof(uint32_t) && sizeof(WriteBarrierStructureID) == sizeof(StructureID));
static_assert(std::is_same_v<decltype(std::declval<const CompiledStructure&>().extraPropertiesCellSize()), uint16_t>);
static_assert(-Slot::wordIndexFor(firstOutOfLineOffset) - 2 == 0 && -Slot::wordIndexFor(firstOutOfLineOffset + 4) - 2 == 4);

static constexpr int32_t maxCapacityDoubledWithoutCall = defaultLimitValue(LimitID::MaxCapacityGrownByStub, hostArchitecture());

void loadExtraProperties(StubAssembler& jit, GPRReg object, GPRReg hiddenWordOffset, GPRReg result, JumpList& hasNone)
{
    RELEASE_ASSERT(noOverlap(object, hiddenWordOffset) && noOverlap(object, result));
    const BaseIndex hiddenWord(object, hiddenWordOffset, CCallHelpers::TimesOne);
    if (jit.options().useWideReferences) {
        jit.load64(hiddenWord, result);
        hasNone.append(jit.branchTest64(CCallHelpers::Zero, result));
        return;
    }
    jit.load32(hiddenWord, result);
    hasNone.append(jit.branchTest32(CCallHelpers::Zero, result));
    decodeCell32(jit, result, result);
}

static void loadWordIndex(StubAssembler& jit, const CachedSlot& slot, GPRReg result)
{
    slot.loadSecondPayloadWord(jit, result);
    jit.signExtend32ToPtr(result, result);
}

static void loadExtraPropertiesBySlot(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, JumpList& miss)
{
    RELEASE_ASSERT(Slot::payloadOf(slot.kind()) == SlotPayload::ExtraProperty);
    RELEASE_ASSERT(noOverlap(slot.header(), r.object, r.extraProperties, r.wordIndex, r.scratch));
    slot.loadHiddenWordOffset(jit, r.scratch);
    loadExtraProperties(jit, r.object, r.scratch, r.extraProperties, miss);
}

static void selectStorage(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r)
{
    loadWordIndex(jit, slot, r.wordIndex);
    Jump isInline = slot.branchUnlessFlag(jit, Slot::isOutOfLine);
    jit.loadPtr(Address(r.extraProperties, JSObject::butterflyOffset()), r.extraProperties);
    isInline.link(&jit);
}

void locateExtraProperty(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, JumpList& miss)
{
    loadExtraPropertiesBySlot(jit, slot, r, miss);
    selectStorage(jit, slot, r);
}

void readExtraPropertyAndReturn(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, GPRReg result, JumpList& miss)
{
    locateExtraProperty(jit, slot, r, miss);
    jit.load64(BaseIndex(r.extraProperties, r.wordIndex, CCallHelpers::TimesEight), result);
    jit.returnToCaller();
}

void storeExtraProperty(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, GPRReg value, const StoreFinish& finish, JumpList& miss)
{
    const GPRReg owner = r.scratch;
    RELEASE_ASSERT(noOverlap(value, r.object, r.extraProperties, r.wordIndex, r.scratch));
    loadExtraPropertiesBySlot(jit, slot, r, miss);
    jit.move(r.extraProperties, owner);
    selectStorage(jit, slot, r);
    jit.store64(value, BaseIndex(r.extraProperties, r.wordIndex, CCallHelpers::TimesEight));
    finish(owner, true);
}

static void loadNewStructure(StubAssembler& jit, const CachedSlot& slot, GPRReg result)
{
    slot.loadFirstPayloadWord(jit, result);
    decodeStructureID(jit, result);
}

static void storeNewStructureID(StubAssembler& jit, const CachedSlot& slot, GPRReg object, GPRReg scratch)
{
    slot.loadFirstPayloadWord(jit, scratch);
    jit.store32(scratch, Address(object, JSCell::structureIDOffset()));
}

static void addToExtraProperties(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, GPRReg value, const StoreFinish& finish, ExtraPropertyAdditionExits& exits)
{
    const Address butterfly(r.extraProperties, JSObject::butterflyOffset());
    slot.loadHiddenWordOffset(jit, r.scratch);
    loadExtraProperties(jit, r.object, r.scratch, r.extraProperties, exits.cannot);
    exits.cannot.append(branchIfBarrierIsNeeded(jit, r.extraProperties, r.scratch));
    loadNewStructure(jit, slot, r.scratch);
    jit.load32(Address(r.scratch, CompiledStructure::offsetOfExtraPropertiesStructureID()), r.scratch);
    loadWordIndex(jit, slot, r.wordIndex);
    auto storeStructureIDs = [&] {
        jit.store32(r.scratch, Address(r.extraProperties, JSCell::structureIDOffset()));
        storeNewStructureID(jit, slot, r.object, r.scratch);
    };
    Jump isOutOfLine = slot.branchIfFlag(jit, Slot::isOutOfLine);
    storeStructureIDs();
    jit.store64(value, BaseIndex(r.extraProperties, r.wordIndex, CCallHelpers::TimesEight));
    finish(r.object, false);

    isOutOfLine.link(&jit);
    exits.growsStorage.append(slot.branchIfFlag(jit, Slot::growsStorage));
    exits.cannot.append(jit.branchTestPtr(CCallHelpers::Zero, butterfly));
    storeStructureIDs();
    jit.loadPtr(butterfly, r.extraProperties);
    jit.store64(value, BaseIndex(r.extraProperties, r.wordIndex, CCallHelpers::TimesEight));
    finish(r.object, false);
}

static void allocateExtraProperties(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, JumpList& cannot)
{
    const GPRReg newStructure = r.wordIndex;
    const GPRReg allocator = r.wordIndex;
    const GPRReg offsetOfAllocator = r.scratch;
    static_assert(MarkedSpace::sizeStep == 2 * sizeof(Allocator));
    loadNewStructure(jit, slot, newStructure);
    jit.load16(Address(newStructure, CompiledStructure::offsetOfExtraPropertiesCellSize()), offsetOfAllocator);
    jit.urshift32(TrustedImm32(1), offsetOfAllocator);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, offsetOfAllocator, CCallHelpers::TimesOne, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    cannot.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, r.extraProperties, allocator, r.scratch, cannot);
}

static void initializeExtraProperties(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, GPRReg value)
{
    const GPRReg newStructure = r.wordIndex;
    const GPRReg end = r.scratch;
    loadNewStructure(jit, slot, newStructure);
    jit.load32(Address(newStructure, CompiledStructure::offsetOfExtraPropertiesStructureID()), r.scratch);
    jit.store32(r.scratch, Address(r.extraProperties, JSCell::structureIDOffset()));
    jit.store32(TrustedImm32(JSFinalObject::defaultTypeInfoBlob()), Address(r.extraProperties, JSCell::indexingTypeAndMiscOffset()));
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), Address(r.extraProperties, JSObject::butterflyOffset()));
    jit.store64(value, Address(r.extraProperties, JSObject::offsetOfInlineStorage()));
    jit.load16(Address(newStructure, CompiledStructure::offsetOfExtraPropertiesCellSize()), end);
    Label clears = jit.label();
    jit.sub32(TrustedImm32(sizeof(EncodedJSValue)), end);
    jit.store64(TrustedImm32(0), BaseIndex(r.extraProperties, end, CCallHelpers::TimesOne));
    jit.branch32(CCallHelpers::Above, end, TrustedImm32(JSObject::offsetOfInlineStorage() + sizeof(EncodedJSValue))).linkTo(clears, &jit);
    fenceAfterAllocation(jit);
}

static void createExtraProperties(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, GPRReg value, const StoreFinish& finish, JumpList& cannot)
{
    const GPRReg hiddenWordOffset = r.wordIndex;
    allocateExtraProperties(jit, slot, r, cannot);
    initializeExtraProperties(jit, slot, r, value);
    slot.loadHiddenWordOffset(jit, hiddenWordOffset);
    if (jit.options().useWideReferences)
        jit.store64(r.extraProperties, BaseIndex(r.object, hiddenWordOffset, CCallHelpers::TimesOne));
    else {
        encodeCell32(jit, r.extraProperties, r.scratch);
        jit.store32(r.scratch, BaseIndex(r.object, hiddenWordOffset, CCallHelpers::TimesOne));
    }
    jit.or32(TrustedImm32(orderWordHasExtraProperties), Address(r.object, offsetOfOrderWord));
    storeNewStructureID(jit, slot, r.object, r.scratch);
    finish(r.object, false);
}

void addExtraPropertyInNewStorage(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyGrowthRegisters& growth, GPRReg value, const StoreFinish& finish, JumpList& cannot)
{
    const ExtraPropertyRegisters& r = growth.access;
    const GPRReg newCapacity = r.wordIndex;
    RELEASE_ASSERT(slot.kind() == Slot::Kind::ExtraPropertyAddition && slot.form() == SlotForm::Slot);
    RELEASE_ASSERT(noOverlap(value, r.object, r.extraProperties, r.wordIndex, r.scratch, growth.oldCapacity, growth.storage, growth.scratch));
    cannot.append(slot.branchIfFlag(jit, Slot::storageHasIndexingHeader));
    // Storage grows when it is full, so the index of the added property is the capacity that the storage had.
    jit.move(r.wordIndex, growth.oldCapacity);
    jit.neg32(growth.oldCapacity);
    jit.sub32(TrustedImm32(2), growth.oldCapacity);
    cannot.append(jit.branch32(CCallHelpers::Above, growth.oldCapacity, TrustedImm32(maxCapacityDoubledWithoutCall)));
    jit.lshift32(growth.oldCapacity, TrustedImm32(1), newCapacity);
    Jump hasStorage = jit.branchTest32(CCallHelpers::NonZero, growth.oldCapacity);
    jit.move(TrustedImm32(initialOutOfLineCapacity), newCapacity);
    hasStorage.link(&jit);
    growOutOfLineStorage(jit, {
        .object = r.extraProperties,
        .newStructureID = r.scratch,
        .oldCapacity = growth.oldCapacity,
        .newCapacity = newCapacity,
        .storage = growth.storage,
        .scratch = growth.scratch,
        .addedValue = value,
    }, cannot);
    storeNewStructureID(jit, slot, r.object, r.scratch);
    finish(r.object, false);
}

void addExtraProperty(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, GPRReg value, const StoreFinish& finish, ExtraPropertyAdditionExits& exits)
{
    const Address orderWord(r.object, offsetOfOrderWord);
    RELEASE_ASSERT(slot.kind() == Slot::Kind::ExtraPropertyAddition && slot.form() == SlotForm::Slot);
    RELEASE_ASSERT(noOverlap(slot.header(), value, r.object, r.extraProperties, r.wordIndex, r.scratch));
    exits.isNotExtensible.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(orderWordIsNotExtensible)));
    exits.cannot.append(branchIfBarrierIsNeeded(jit, r.object, r.scratch));
    Jump hasNone = jit.branchTest32(CCallHelpers::Zero, orderWord, TrustedImm32(orderWordHasExtraProperties));
    addToExtraProperties(jit, slot, r, value, finish, exits);

    hasNone.link(&jit);
    createExtraProperties(jit, slot, r, value, finish, exits.cannot);
}

// payload is not slotPayloadRegister: on x86-64 that one is callee-saved for an operation, and a front end stands in for one.
struct ExtraPropertyProbe {
    CachedSlot slot(SlotForm form, Slot::Kind kind) const { return CachedSlot(form, kind, header, form == SlotForm::Slot ? payload : InvalidGPRReg); }

    ExtraPropertyRegisters access;
    GPRReg identifierNumber;
    GPRReg header;
    GPRReg payload;
};

static ExtraPropertyProbe allocateProbe(StubAssembler& jit)
{
    ExtraPropertyProbe probe {
        .access = { jit.operand(0), jit.allocateScratch(StubRegister::T10), jit.allocateScratch(StubRegister::T11), jit.allocateScratch(StubRegister::T9) },
        .identifierNumber = jit.clobberOperand(1),
        .header = jit.allocateScratch(slotHeaderRegister),
        .payload = jit.result(),
    };
    jit.zeroExtend32ToWord(probe.identifierNumber, probe.identifierNumber);
    return probe;
}

static void probeFront(StubAssembler& jit, SlotAccess access, const ExtraPropertyProbe& probe, JumpList& isNotInFront)
{
    const GPRReg structureID = probe.access.wordIndex;
    const GPRReg row = probe.access.scratch;
    const GPRReg scratch = probe.access.extraProperties;
    isNotInFront.append(jit.branch32(CCallHelpers::Above, probe.identifierNumber, TrustedImm32(RowDisplacementTable::maxColumn)));
    jit.load32(Address(probe.access.object, JSCell::structureIDOffset()), structureID);
    loadFrontCell(jit, access, { structureID, probe.identifierNumber, row, probe.header, scratch });
    isNotInFront.append(branchIfCellNameDiffers(jit, probe.identifierNumber, probe.header, scratch));
}

static void probeHashedTable(StubAssembler& jit, SlotAccess access, const ExtraPropertyProbe& probe, JumpList& miss)
{
    const GPRReg structureID = probe.access.wordIndex;
    const GPRReg uid = probe.access.extraProperties;
    const GPRReg entry = probe.access.scratch;
    jit.load32(Address(probe.access.object, JSCell::structureIDOffset()), structureID);
    loadIdentifier(jit, probe.identifierNumber, uid, miss);
    findHashedSlot(jit, access, { structureID, uid, entry, probe.header, probe.payload, probe.payload }, miss);
}

static void callWriteBarrier(StubAssembler& jit, GPRReg owner)
{
    static_assert(gprOf(firstStubOperandRegister) == GPRInfo::returnValueGPR);
    jit.move(owner, jit.result());
#if CPU(ARM64)
    jit.pushToSave(CCallHelpers::linkRegister);
    jit.callStub(writeBarrier());
    jit.popToRestore(CCallHelpers::linkRegister);
#else
    jit.subPtr(TrustedImm32(sizeof(CPURegister)), CCallHelpers::stackPointerRegister);
    jit.callStub(writeBarrier());
    jit.addPtr(TrustedImm32(sizeof(CPURegister)), CCallHelpers::stackPointerRegister);
#endif
}

StubSignature signatureOfGetExtraPropertyFrontEnd(StubKey)
{
    return operationFrontEndSignature(2);
}

static void returnExtraProperty(StubAssembler& jit, SlotForm form, const ExtraPropertyProbe& probe, JumpList& hasOtherKind, JumpList& callsOperation)
{
    hasOtherKind.append(branchIfSlotKindDiffers(jit, { form, probe.header }, Slot::Kind::ExtraProperty, probe.access.scratch));
    locateExtraProperty(jit, probe.slot(form, Slot::Kind::ExtraProperty), probe.access, callsOperation);
    jit.load64(BaseIndex(probe.access.extraProperties, probe.access.wordIndex, CCallHelpers::TimesEight), jit.result());
    jit.move(TrustedImm32(0), jit.secondResult());
    jit.returnToCaller();
}

void generateGetExtraPropertyFrontEnd(StubAssembler& jit)
{
    const ExtraPropertyProbe probe = allocateProbe(jit);
    JumpList isNotInFront;
    JumpList callsOperation;
    probeFront(jit, SlotAccess::Read, probe, isNotInFront);
    returnExtraProperty(jit, SlotForm::PackedSlot, probe, isNotInFront, callsOperation);

    isNotInFront.link(&jit);
    probeHashedTable(jit, SlotAccess::Read, probe, callsOperation);
    returnExtraProperty(jit, SlotForm::Slot, probe, callsOperation, callsOperation);

    callsOperation.link(&jit);
    jit.tailCallOperation(Entry::RawGetExtraProperty, OperationCall::FirstArgument::Instance);
}

StubSignature signatureOfPutExtraPropertyFrontEnd(StubKey)
{
    return operationFrontEndSignature(3);
}

void generatePutExtraPropertyFrontEnd(StubAssembler& jit)
{
    const ExtraPropertyProbe probe = allocateProbe(jit);
    const GPRReg value = jit.operand(2);
    const GPRReg kind = probe.access.scratch;
    JumpList isNotInFront;
    JumpList callsOperation;
    ExtraPropertyAdditionExits additionExits;
    auto finish = [&](GPRReg owner, bool valueMayNeedBarrier) {
        if (valueMayNeedBarrier) {
            Jump isNotCell = jit.branchIfNotCell(value);
            callWriteBarrier(jit, owner);
            isNotCell.link(&jit);
        }
        jit.move(TrustedImm32(0), jit.result());
        jit.returnToCaller();
    };
    probeFront(jit, SlotAccess::Store, probe, isNotInFront);
    isNotInFront.append(branchIfSlotKindDiffers(jit, { SlotForm::PackedSlot, probe.header }, Slot::Kind::ExtraProperty, kind));
    storeExtraProperty(jit, probe.slot(SlotForm::PackedSlot, Slot::Kind::ExtraProperty), probe.access, value, finish, callsOperation);

    isNotInFront.link(&jit);
    probeHashedTable(jit, SlotAccess::Store, probe, callsOperation);
    Jump isNotReplacement = branchIfSlotKindDiffers(jit, { SlotForm::Slot, probe.header }, Slot::Kind::ExtraProperty, kind);
    storeExtraProperty(jit, probe.slot(SlotForm::Slot, Slot::Kind::ExtraProperty), probe.access, value, finish, callsOperation);

    isNotReplacement.link(&jit);
    callsOperation.append(branchIfSlotKindDiffers(jit, { SlotForm::Slot, probe.header }, Slot::Kind::ExtraPropertyAddition, kind));
    addExtraProperty(jit, probe.slot(SlotForm::Slot, Slot::Kind::ExtraPropertyAddition), probe.access, value, finish, additionExits);

    additionExits.isNotExtensible.link(&jit);
    additionExits.growsStorage.link(&jit);
    additionExits.cannot.link(&jit);
    callsOperation.link(&jit);
    jit.tailCallOperation(Entry::RawPutExtraProperty, OperationCall::FirstArgument::Instance);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
