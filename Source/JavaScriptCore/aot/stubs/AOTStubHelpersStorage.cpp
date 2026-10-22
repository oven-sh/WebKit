/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubHelpers.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTInstance.h"
#include "AOTStubRegisters.h"
#include "ArrayConventions.h"
#include "Butterfly.h"
#include "CompleteSubspace.h"
#include "IndexingHeader.h"
#include "IndexingType.h"
#include "JSCJSValueInlines.h"
#include "JSObject.h"
#include "LocalAllocator.h"
#include "MarkedSpace.h"
#include "PureNaN.h"
#include "Structure.h"
#include "StructureID.h"
#include <bit>

namespace JSC { namespace AOT {

static void emitStorageGrowthValidation(StubAssembler& jit, const StorageGrowth& growth)
{
    JumpList isValid;
    JumpList isInvalid;
    isInvalid.append(jit.branchTest8(CCallHelpers::NonZero, Address(growth.object, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask)));
    Jump grows = jit.branchTest32(CCallHelpers::NonZero, growth.oldCapacity);
    isValid.append(jit.branch32(CCallHelpers::Equal, growth.newCapacity, TrustedImm32(initialOutOfLineCapacity)));
    isInvalid.append(jit.jump());

    grows.link(&jit);
    isInvalid.append(jit.branch32(CCallHelpers::Below, growth.oldCapacity, TrustedImm32(initialOutOfLineCapacity)));
    isInvalid.append(jit.branch32(CCallHelpers::Above, growth.newCapacity, TrustedImm32(MarkedSpace::largeCutoff / sizeof(EncodedJSValue))));
    jit.lshift32(growth.oldCapacity, TrustedImm32(1), growth.storage);
    isValid.append(jit.branch32(CCallHelpers::Equal, growth.storage, growth.newCapacity));

    isInvalid.link(&jit);
    jit.breakpoint();
    isValid.link(&jit);
}

static void emitFirstStorageInitialization(StubAssembler& jit, const StorageGrowth& growth)
{
    constexpr size_t storageSize = initialOutOfLineCapacity * sizeof(EncodedJSValue);
    const GPRReg storage = growth.storage;
    const Address firstProperty(storage, storageSize - sizeof(EncodedJSValue));

    for (unsigned i = 0; i + 1 < initialOutOfLineCapacity; ++i)
        jit.store64(TrustedImm32(0), Address(storage, i * sizeof(EncodedJSValue)));
    if (growth.addedValue)
        jit.store64(*growth.addedValue, firstProperty);
    else
        jit.store64(TrustedImm32(0), firstProperty);
    jit.addPtr(TrustedImm32(storageSize + sizeof(IndexingHeader)), storage);
}

static void copyAndClearPair(StubAssembler& jit, GPRReg oldValues, GPRReg copiedValues, GPRReg newSlots)
{
    constexpr int pairSize = 2 * sizeof(EncodedJSValue);
#if CPU(ARM64)
    jit.transferPair64(CCallHelpers::PostIndexAddress(oldValues, pairSize), CCallHelpers::PostIndexAddress(copiedValues, pairSize));
    jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, CCallHelpers::PostIndexAddress(newSlots, pairSize));
#else
    for (int offset : { 0, pairSize / 2 }) {
        jit.transfer64(Address(oldValues, offset), Address(copiedValues, offset));
        jit.store64(TrustedImm32(0), Address(newSlots, offset));
    }
    for (GPRReg pointer : { oldValues, copiedValues, newSlots })
        jit.addPtr(TrustedImm32(pairSize), pointer);
#endif
}

// From its base upward the storage has oldCapacity new slots, highest index first, then the oldCapacity old values.
static void emitDoubledStorageInitialization(StubAssembler& jit, const StorageGrowth& growth)
{
    static_assert(!(initialOutOfLineCapacity % 2) && outOfLineGrowthFactor == 2);
    const GPRReg remainingValues = growth.oldCapacity;
    const GPRReg newSlots = growth.storage;
    const GPRReg copiedValues = growth.newCapacity;
    const GPRReg oldValues = growth.scratch;

    jit.loadPtr(Address(growth.object, JSObject::butterflyOffset()), oldValues);
    jit.lshiftPtr(remainingValues, TrustedImm32(3), copiedValues);
    jit.subPtr(copiedValues, oldValues);
    jit.subPtr(TrustedImm32(sizeof(IndexingHeader)), oldValues);
    jit.addPtr(newSlots, copiedValues);

    Label nextPair = jit.label();
    copyAndClearPair(jit, oldValues, copiedValues, newSlots);
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(2), remainingValues).linkTo(nextPair, &jit);

    if (growth.addedValue)
        jit.store64(*growth.addedValue, Address(newSlots, -static_cast<int32_t>(sizeof(EncodedJSValue))));
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), copiedValues, growth.storage);
}

void growOutOfLineStorage(StubAssembler& jit, const StorageGrowth& growth, JumpList& slowPath)
{
    const GPRReg object = growth.object;
    const GPRReg storage = growth.storage;
    const GPRReg allocator = growth.scratch;
    RELEASE_ASSERT(noOverlap(object, growth.newStructureID, growth.oldCapacity, growth.newCapacity, storage, allocator, growth.addedValue.value_or(InvalidGPRReg), instanceGPR));

    if (jit.options().validates)
        emitStorageGrowthValidation(jit, growth);

    // The allocator for newCapacity values is entry newCapacity / 2, at byte newCapacity * 4.
    static_assert(MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*) && !(initialOutOfLineCapacity % 2));
    jit.loadPtr(Address(object, JSObject::butterflyOffset()), storage);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, growth.newCapacity, CCallHelpers::TimesFour, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    Jump grows = jit.branchTest32(CCallHelpers::NonZero, growth.oldCapacity);

    slowPath.append(jit.branchTestPtr(CCallHelpers::NonZero, storage));
    allocateCell(jit, storage, allocator, growth.newCapacity, slowPath);
    emitFirstStorageInitialization(jit, growth);
    Jump hasNewStorage = jit.jump();

    grows.link(&jit);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, storage));
    allocateCell(jit, storage, allocator, growth.newCapacity, slowPath);
    emitDoubledStorageInitialization(jit, growth);

    hasNewStorage.link(&jit);
    jit.or32(TrustedImm32(StructureID::nukedStructureIDBit), Address(object, JSCell::structureIDOffset()));
    jit.storeFence();
    jit.storePtr(storage, Address(object, JSObject::butterflyOffset()));
    jit.storeFence();
    jit.store32(growth.newStructureID, Address(object, JSCell::structureIDOffset()));
}

static void emitVectorGrowthValidation(StubAssembler& jit, const VectorGrowth& growth)
{
    JumpList isInvalid;
    isInvalid.append(jit.branchPtr(CCallHelpers::NotEqual, growth.butterfly, Address(growth.array, JSObject::butterflyOffset())));
    isInvalid.append(jit.branch32(CCallHelpers::NotEqual, growth.length, Address(growth.butterfly, Butterfly::offsetOfPublicLength())));
    Jump isValid = jit.branch32(CCallHelpers::Equal, growth.length, Address(growth.butterfly, Butterfly::offsetOfVectorLength()));
    isInvalid.link(&jit);
    jit.breakpoint();
    isValid.link(&jit);
}

// The butterfly of such an array holds the indexing header and the vector and nothing else, and the vector is the array's own.
static void branchUnlessHasOriginalArrayStructure(StubAssembler& jit, GPRReg array, GPRReg structureID, JumpList& otherwise)
{
    JumpList hasOriginalStructure;
    jit.load32(Address(array, JSCell::structureIDOffset()), structureID);
    for (IndexingType indexingType : { ArrayWithContiguous, ArrayWithInt32 })
        hasOriginalStructure.append(jit.branch32(CCallHelpers::Equal, structureID, Address(instanceGPR, Instance::offsetOfNewArrayHeader(indexingType))));
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, structureID, Address(instanceGPR, Instance::offsetOfNewArrayHeader(ArrayWithDouble))));
    hasOriginalStructure.link(&jit);
}

// The allocator that Butterfly::optimalContiguousVectorLength(0, nextLength(length + 1)) rounds up to, or the same for nextLength(neededLength).
static void loadAllocatorOfNextVector(StubAssembler& jit, const VectorGrowth& growth, std::optional<GPRReg> neededLength, JumpList& slowPath)
{
    static_assert(sizeof(IndexingHeader) == sizeof(EncodedJSValue) && MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
    constexpr unsigned longestNextVector = maxLengthForElementwiseCopy + 1 + (maxLengthForElementwiseCopy + 1) / 2;
    static_assert(sizeof(IndexingHeader) + longestNextVector * sizeof(EncodedJSValue) <= MarkedSpace::largeCutoff);
    const GPRReg allocator = growth.allocator;
    const GPRReg vectorLength = growth.scratch;

    if (neededLength)
        jit.move(*neededLength, vectorLength);
    else
        jit.add32(TrustedImm32(1), growth.length, vectorLength);
    jit.urshift32(vectorLength, TrustedImm32(1), allocator);
    jit.add32(allocator, vectorLength);
    Jump isLongEnough = jit.branch32(CCallHelpers::AboveOrEqual, vectorLength, TrustedImm32(BASE_CONTIGUOUS_VECTOR_LEN));
    jit.move(TrustedImm32(BASE_CONTIGUOUS_VECTOR_LEN), vectorLength);
    isLongEnough.link(&jit);

    const GPRReg sizeStep = vectorLength;
    jit.add32(TrustedImm32(2), sizeStep);
    jit.urshift32(TrustedImm32(1), sizeStep);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, sizeStep, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
}

static void loadHoleOf(StubAssembler& jit, GPRReg array, GPRReg hole)
{
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), hole);
    jit.and32(TrustedImm32(IndexingShapeMask), hole);
    Jump holdsDoubles = jit.branch32(CCallHelpers::Equal, hole, TrustedImm32(DoubleShape));
    jit.move(TrustedImm64(JSValue::encode(JSValue())), hole);
    Jump isLoaded = jit.jump();
    holdsDoubles.link(&jit);
    jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), hole);
    isLoaded.link(&jit);
}

static void growVector(StubAssembler& jit, const VectorGrowth& growth, std::optional<GPRReg> neededLength, JumpList& slowPath)
{
    const auto [array, butterfly, length, storage, allocator, scratch] = growth;
    const GPRReg index = length;
    const GPRReg vectorLength = allocator;
    RELEASE_ASSERT(noOverlap(array, butterfly, length, storage, allocator, scratch, instanceGPR));
    RELEASE_ASSERT(!neededLength || noOverlap(*neededLength, array, butterfly, length, storage, allocator, scratch, instanceGPR));

    slowPath.append(jit.branch32(CCallHelpers::Above, neededLength.value_or(length), TrustedImm32(maxLengthForElementwiseCopy)));
    // Only the array leads to the new vector, so an array that the collector will not visit again is left to C++.
    // While the collector marks beside the program every cell needs a barrier, so nobody reads the array during the stores below.
    slowPath.append(branchIfBarrierIsNeeded(jit, array, scratch));
    branchUnlessHasOriginalArrayStructure(jit, array, scratch, slowPath);
    // The length of an array with ArrayStorage may lie beyond its vector.
    if (jit.options().validates && !neededLength)
        emitVectorGrowthValidation(jit, growth);
    loadAllocatorOfNextVector(jit, growth, neededLength, slowPath);
    allocateCell(jit, storage, allocator, scratch, slowPath);

    jit.load32(Address(allocator, LocalAllocator::offsetOfCellSize()), vectorLength);
    jit.sub32(TrustedImm32(sizeof(IndexingHeader)), vectorLength);
    jit.urshift32(TrustedImm32(3), vectorLength);
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.store32(length, Address(storage, Butterfly::offsetOfPublicLength()));
    jit.store32(vectorLength, Address(storage, Butterfly::offsetOfVectorLength()));

    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, index);
    Label copyPrevious = jit.label();
    jit.sub32(TrustedImm32(1), index);
    jit.transfer64(BaseIndex(butterfly, index, CCallHelpers::TimesEight), BaseIndex(storage, index, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, index).linkTo(copyPrevious, &jit);
    isEmpty.link(&jit);

    const GPRReg hole = scratch;
    loadHoleOf(jit, array, hole);
    jit.load32(Address(storage, Butterfly::offsetOfPublicLength()), index);
    Label fillNext = jit.label();
    jit.store64(hole, BaseIndex(storage, index, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::Below, index, vectorLength).linkTo(fillNext, &jit);
    jit.storePtr(storage, Address(array, JSObject::butterflyOffset()));
}

void growFullVector(StubAssembler& jit, const VectorGrowth& growth, JumpList& slowPath)
{
    growVector(jit, growth, std::nullopt, slowPath);
}

void growVectorFor(StubAssembler& jit, const VectorGrowth& growth, GPRReg neededLength, JumpList& slowPath)
{
    growVector(jit, growth, neededLength, slowPath);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
