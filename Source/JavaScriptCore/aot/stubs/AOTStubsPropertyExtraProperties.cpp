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

void loadExtraPropertyBySlot(StubAssembler& jit, const CachedSlot& slot, GPRReg object, GPRReg result, GPRReg scratch, JumpList& miss)
{
    RELEASE_ASSERT(Slot::payloadOf(slot.kind()) == SlotPayload::ExtraProperty && noOverlap(slot.header(), object, result, scratch));
    slot.loadHiddenWordOffset(jit, scratch);
    loadExtraProperties(jit, object, scratch, result, miss);
    loadWordIndex(jit, slot, scratch);
    Jump isInline = slot.branchUnlessFlag(jit, Slot::isOutOfLine);
    jit.loadPtr(Address(result, JSObject::butterflyOffset()), result);
    isInline.link(&jit);
    jit.load64(BaseIndex(result, scratch, CCallHelpers::TimesEight), result);
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
