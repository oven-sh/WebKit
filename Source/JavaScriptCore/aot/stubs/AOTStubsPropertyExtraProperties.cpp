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
#include "AOTPropertyCachesFront.h"
#include "AOTSlotRepresentation.h"
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

static constexpr int32_t maxCapacityDoubledWithoutCall = 32;
static_assert(2 * maxCapacityDoubledWithoutCall * sizeof(EncodedJSValue) <= MarkedSpace::largeCutoff);

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

void loadExtraPropertiesByStructure(StubAssembler& jit, GPRReg object, GPRReg structure, GPRReg scratch, GPRReg result, JumpList& hasNone)
{
    RELEASE_ASSERT(noOverlap(object, structure, scratch) && noOverlap(object, scratch, result));
    jit.load32(Address(structure, CompiledStructure::offsetOfExtraPropertiesSlot()), scratch);
    jit.and32(TrustedImm32(SlotData::maxOffset), scratch);
    Jump isHeldByObject = jit.branchTest32(CCallHelpers::NonZero, scratch);
    hasNone.append(jit.branchTest8(CCallHelpers::Zero, Address(structure, CompiledStructure::offsetOfFlags()), TrustedImm32(static_cast<uint8_t>(CompiledStructure::Flag::HasExtraProperties))));
    jit.load16(Address(structure, CompiledStructure::offsetOfHighBitsOfRareData()), scratch);
    jit.lshift64(TrustedImm32(32), scratch);
    jit.load32(Address(structure, CompiledStructure::offsetOfLowBitsOfRareData()), result);
    jit.or64(scratch, result);
    hasNone.append(jit.branchTestPtr(CCallHelpers::Zero, result));
    jit.loadPtr(Address(result, CompiledStructure::offsetOfHeldExtraPropertiesInRareData()), result);
    hasNone.append(jit.branchTestPtr(CCallHelpers::Zero, result));
    Jump isLoaded = jit.jump();

    isHeldByObject.link(&jit);
    loadExtraProperties(jit, object, scratch, result, hasNone);
    isLoaded.link(&jit);
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

static_assert(PackedSlot::hiddenWordOffsetShift == CCallHelpers::TimesFour && cell32Shift == CCallHelpers::TimesEight && Slot::wordIndexFor(0) > 0);

void loadExtraPropertyByFrontCell(StubAssembler& jit, GPRReg cell, GPRReg object, GPRReg storage, GPRReg wordIndex, GPRReg result, JumpList& hasNone)
{
    const CachedSlot slot(SlotForm::PackedSlot, Slot::Kind::ExtraProperty, cell, InvalidGPRReg);
    RELEASE_ASSERT(noOverlap(cell, object, storage, wordIndex));
    extractBits(jit, cell, locationShiftInCell, 8 * sizeof(PackedSlot::location), storage);
    const BaseIndex hiddenWord(object, storage, CCallHelpers::TimesFour);
    if (jit.options().useWideReferences) {
        jit.load64(hiddenWord, storage);
        hasNone.append(jit.branchTest64(CCallHelpers::Zero, storage));
    } else {
        jit.load32(hiddenWord, storage);
        hasNone.append(jit.branchTest32(CCallHelpers::Zero, storage));
        decodeCell32(jit, storage, storage);
    }
    Jump isInCell = slot.branchUnlessFlag(jit, Slot::isOutOfLine);
    jit.loadPtr(Address(storage, JSObject::butterflyOffset()), storage);
    isInCell.link(&jit);
#if CPU(ARM64)
    UNUSED_PARAM(wordIndex);
    jit.load64(BaseIndex(storage, cell, CCallHelpers::TimesEight, 0, CCallHelpers::Extend::SExt32), result);
#else
    jit.signExtend32ToPtr(cell, wordIndex);
    jit.load64(BaseIndex(storage, wordIndex, CCallHelpers::TimesEight), result);
#endif
}

#if CPU(X86_64)
void loadExtraPropertyByFrontCell(StubAssembler& jit, const BaseIndex& cell, bool isOutOfLine, GPRReg object, GPRReg word, GPRReg result, JumpList& hasNone)
{
    RELEASE_ASSERT(noOverlap(object, word, cell.base, cell.index, cellCageBaseGPR));
    auto inCell = [&](ptrdiff_t offset) {
        return BaseIndex(cell.base, cell.index, cell.scale, cell.offset + static_cast<int32_t>(offset));
    };
    jit.load8(inCell(PackedSlot::offsetOfLocation()), word);
    const BaseIndex hiddenWord(object, word, CCallHelpers::TimesFour);
    if (jit.options().useWideReferences) {
        jit.load64(hiddenWord, word);
        hasNone.append(jit.branchTest64(CCallHelpers::Zero, word));
        if (isOutOfLine)
            jit.loadPtr(Address(word, JSObject::butterflyOffset()), word);
        jit.load32SignedExtendTo64(inCell(PackedSlot::offsetOfPayload()), cell.index);
        jit.load64(BaseIndex(word, cell.index, CCallHelpers::TimesEight), result);
        return;
    }
    jit.load32(hiddenWord, word);
    hasNone.append(jit.branchTest32(CCallHelpers::Zero, word));
    if (!isOutOfLine) {
        jit.add32(inCell(PackedSlot::offsetOfPayload()), word);
        jit.load64(BaseIndex(cellCageBaseGPR, word, CCallHelpers::TimesEight), result);
        return;
    }
    jit.loadPtr(BaseIndex(cellCageBaseGPR, word, CCallHelpers::TimesEight, JSObject::butterflyOffset()), word);
    jit.load32SignedExtendTo64(inCell(PackedSlot::offsetOfPayload()), cell.index);
    jit.load64(BaseIndex(word, cell.index, CCallHelpers::TimesEight), result);
}
#endif

void storeExtraProperty(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, GPRReg value, const StoreFinish& finish, JumpList& miss)
{
    const GPRReg owner = r.scratch;
    RELEASE_ASSERT(noOverlap(value, r.object, r.extraProperties, r.wordIndex, r.scratch));
    loadExtraPropertiesBySlot(jit, slot, r, miss);
    jit.move(r.extraProperties, owner);
    selectStorage(jit, slot, r);
    jit.store64(value, BaseIndex(r.extraProperties, r.wordIndex, CCallHelpers::TimesEight));
    finish(owner);
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

static void addToExtraProperties(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, GPRReg value, ExtraPropertyAdditionExits& exits)
{
    static_assert(!static_cast<unsigned>(CellState::PossiblyBlack));
    const Address butterfly(r.extraProperties, JSObject::butterflyOffset());
    slot.loadHiddenWordOffset(jit, r.scratch);
    loadExtraProperties(jit, r.object, r.scratch, r.extraProperties, exits.cannot);
    exits.cannot.append(jit.branchTest8(CCallHelpers::Zero, Address(r.extraProperties, JSCell::cellStateOffset())));
    loadNewStructure(jit, slot, r.scratch);
    jit.load32(Address(r.scratch, CompiledStructure::offsetOfExtraPropertiesStructureID()), r.scratch);
    loadWordIndex(jit, slot, r.wordIndex);
    auto storeStructureIDs = [&] {
        jit.store32(r.scratch, Address(r.extraProperties, JSCell::structureIDOffset()));
        storeNewStructureID(jit, slot, r.object, r.scratch);
    };
    Jump isOutOfLine = slot.branchIfFlag(jit, Slot::isOutOfLine);
    jit.store64(value, BaseIndex(r.extraProperties, r.wordIndex, CCallHelpers::TimesEight));
    storeStructureIDs();
    Label isAdded = jit.label();
    returnAfterAddingExtraProperty(jit, r.object, r.extraProperties, r.scratch);

    isOutOfLine.link(&jit);
    exits.growsStorage.append(slot.branchIfFlag(jit, Slot::growsStorage));
    exits.cannot.append(jit.branchTestPtr(CCallHelpers::Zero, butterfly));
    storeStructureIDs();
    jit.loadPtr(butterfly, r.scratch);
    jit.store64(value, BaseIndex(r.scratch, r.wordIndex, CCallHelpers::TimesEight));
    jit.jump().linkTo(isAdded, &jit);
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

static void createExtraProperties(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, GPRReg value, JumpList& cannot)
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
    fenceAfterAllocation(jit);
    jit.or32(TrustedImm32(orderWordHasExtraProperties), Address(r.object, offsetOfOrderWord));
    storeNewStructureID(jit, slot, r.object, r.scratch);
    returnAfterAddingExtraProperty(jit, r.object, std::nullopt, r.scratch);
}

void addExtraPropertyInNewStorage(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyGrowthRegisters& growth, GPRReg value, JumpList& cannot)
{
    const ExtraPropertyRegisters& r = growth.access;
    const GPRReg newCapacity = r.wordIndex;
    RELEASE_ASSERT(slot.kind() == Slot::Kind::ExtraPropertyAddition && slot.form() == SlotForm::Slot);
    RELEASE_ASSERT(noOverlap(value, r.object, r.extraProperties, r.wordIndex, r.scratch, growth.oldCapacity, growth.storage, growth.scratch));
    cannot.append(slot.branchIfFlag(jit, Slot::storageHasIndexingHeader));
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
    returnAfterAddingExtraProperty(jit, r.object, r.extraProperties, r.scratch);
}

void addExtraProperty(StubAssembler& jit, const CachedSlot& slot, const ExtraPropertyRegisters& r, GPRReg value, ExtraPropertyAdditionExits& exits)
{
    const Address orderWord(r.object, offsetOfOrderWord);
    RELEASE_ASSERT(slot.kind() == Slot::Kind::ExtraPropertyAddition && slot.form() == SlotForm::Slot);
    RELEASE_ASSERT(noOverlap(slot.header(), value, r.object, r.extraProperties, r.wordIndex, r.scratch));
    exits.isNotExtensible.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(orderWordIsNotExtensible)));
    Jump hasNone = jit.branchTest32(CCallHelpers::Zero, orderWord, TrustedImm32(orderWordHasExtraProperties));
    addToExtraProperties(jit, slot, r, value, exits);

    hasNone.link(&jit);
    createExtraProperties(jit, slot, r, value, exits.cannot);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
