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

static void installStorage(StubAssembler& jit, GPRReg object, GPRReg storage, GPRReg newStructureID)
{
    jit.or32(TrustedImm32(StructureID::nukedStructureIDBit), Address(object, JSCell::structureIDOffset()));
    jit.storeFence();
    jit.storePtr(storage, Address(object, JSObject::butterflyOffset()));
    jit.storeFence();
    jit.store32(newStructureID, Address(object, JSCell::structureIDOffset()));
}

void growOutOfLineStorage(StubAssembler& jit, const StorageGrowth& growth, JumpList& slowPath)
{
    const GPRReg object = growth.object;
    const GPRReg storage = growth.storage;
    const GPRReg allocator = growth.scratch;
    RELEASE_ASSERT(noOverlap(object, growth.newStructureID, growth.oldCapacity, growth.newCapacity, storage, allocator, growth.addedValue.value_or(InvalidGPRReg), instanceGPR));

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
    installStorage(jit, object, storage, growth.newStructureID);
}

AssemblyHelpers::FreeListRefill createOutOfLineStorage(StubAssembler& jit, const StorageGrowth& growth, JumpList& slowPath)
{
    constexpr size_t storageSize = initialOutOfLineCapacity * sizeof(EncodedJSValue);
    const GPRReg allocator = growth.scratch;
    RELEASE_ASSERT(noOverlap(growth.object, growth.newStructureID, growth.oldCapacity, growth.storage, allocator, growth.addedValue.value_or(InvalidGPRReg), instanceGPR));

    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), allocator);
    jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(storageSize) * sizeof(Allocator)), allocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, growth.storage, allocator, growth.oldCapacity, slowPath, storageSize);
    emitFirstStorageInitialization(jit, growth);
    installStorage(jit, growth.object, growth.storage, growth.newStructureID);
    return refill;
}

void widenStorageOfIntegersForAnyValue(StubAssembler& jit, GPRReg array, GPRReg scratch, JumpList& isLeftToOthers)
{
    jit.load32(Address(array, JSCell::structureIDOffset()), scratch);
    isLeftToOthers.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, Instance::offsetOfNewArrayHeader(ArrayWithInt32))));
    jit.load32(Address(instanceGPR, Instance::offsetOfNewArrayHeader(ArrayWithContiguous)), scratch);
    isLeftToOthers.append(jit.branchTest32(CCallHelpers::Zero, scratch));
    jit.store32(scratch, Address(array, JSCell::structureIDOffset()));
    jit.store8(TrustedImm32(ArrayWithContiguous), Address(array, JSCell::indexingTypeAndMiscOffset()));
}

static void branchUnlessHasOriginalArrayStructure(StubAssembler& jit, GPRReg array, GPRReg structureID, JumpList& otherwise)
{
    JumpList hasOriginalStructure;
    jit.load32(Address(array, JSCell::structureIDOffset()), structureID);
    for (IndexingType indexingType : { ArrayWithContiguous, ArrayWithInt32 })
        hasOriginalStructure.append(jit.branch32(CCallHelpers::Equal, structureID, Address(instanceGPR, Instance::offsetOfNewArrayHeader(indexingType))));
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, structureID, Address(instanceGPR, Instance::offsetOfNewArrayHeader(ArrayWithDouble))));
    hasOriginalStructure.link(&jit);
}

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

#if CPU(X86_64)
static void loadPair(StubAssembler& jit, BaseIndex from)
{
    constexpr FPRReg pair = CCallHelpers::fpTempRegister;
    if (jit.supportsAVX())
        jit.m_assembler.vmovups_mr(from.offset, from.base, from.index, from.scale, pair);
    else
        jit.m_assembler.movups_mr(from.offset, from.base, from.index, from.scale, pair);
}

static void storePair(StubAssembler& jit, BaseIndex to)
{
    constexpr FPRReg pair = CCallHelpers::fpTempRegister;
    if (jit.supportsAVX())
        jit.m_assembler.vmovups_rm(pair, to.offset, to.base, to.index, to.scale);
    else
        jit.m_assembler.movups_rm(pair, to.offset, to.base, to.index, to.scale);
}

static void loadPairOfHoles(StubAssembler& jit, GPRReg array, GPRReg scratch)
{
    constexpr FPRReg pair = CCallHelpers::fpTempRegister;
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.and32(TrustedImm32(IndexingShapeMask), scratch);
    jit.moveZeroToDouble(pair);
    static_assert(!JSValue::ValueEmpty);
    Jump holdsNoDoubles = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(DoubleShape));
    jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), scratch);
    jit.move64ToDouble(scratch, pair);
    if (jit.supportsAVX())
        jit.m_assembler.vpunpcklqdq_rrr(pair, pair, pair);
    else
        jit.m_assembler.punpcklqdq_rr(pair, pair);
    holdsNoDoubles.link(&jit);
}

static void fillAndCopyByPairs(StubAssembler& jit, const VectorGrowth& growth, GPRReg vectorLength)
{
    static_assert(MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(IndexingHeader) == sizeof(EncodedJSValue));
    constexpr int32_t elementSize = sizeof(EncodedJSValue);
    const GPRReg butterfly = growth.butterfly;
    const GPRReg length = growth.length;
    const GPRReg storage = growth.storage;
    const GPRReg index = growth.scratch;

    jit.zeroExtend32ToWord(length, length);
    loadPairOfHoles(jit, growth.array, growth.scratch);
    jit.move(vectorLength, index);
    Label fillPrevious = jit.label();
    jit.sub64(TrustedImm32(2), index);
    storePair(jit, BaseIndex(storage, index, CCallHelpers::TimesEight));
    jit.branch64(CCallHelpers::GreaterThan, index, length).linkTo(fillPrevious, &jit);

    jit.transfer64(BaseIndex(butterfly, length, CCallHelpers::TimesEight, -elementSize), BaseIndex(storage, length, CCallHelpers::TimesEight, -elementSize));
    jit.and32(TrustedImm32(~1), length, index);
    Jump hasNoPair = jit.branchTest32(CCallHelpers::Zero, index);
    Label copyPrevious = jit.label();
    loadPair(jit, BaseIndex(butterfly, index, CCallHelpers::TimesEight, -2 * elementSize));
    storePair(jit, BaseIndex(storage, index, CCallHelpers::TimesEight, -2 * elementSize));
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(2), index).linkTo(copyPrevious, &jit);
    hasNoPair.link(&jit);

    jit.store32(length, Address(storage, Butterfly::offsetOfPublicLength()));
    jit.store32(vectorLength, Address(storage, Butterfly::offsetOfVectorLength()));
}
#else
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

static void fillAndCopyByPairs(StubAssembler& jit, const VectorGrowth& growth, GPRReg vectorLength)
{
    static_assert(MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(IndexingHeader) == sizeof(EncodedJSValue));
    constexpr int32_t elementSize = sizeof(EncodedJSValue);
    constexpr int32_t pairSize = 2 * elementSize;
    const GPRReg butterfly = growth.butterfly;
    const GPRReg length = growth.length;
    const GPRReg storage = growth.storage;
    const GPRReg end = vectorLength;
    const GPRReg copied = growth.scratch;

    jit.zeroExtend32ToWord(length, length);
    jit.store32(length, Address(storage, Butterfly::offsetOfPublicLength()));
    jit.store32(vectorLength, Address(storage, Butterfly::offsetOfVectorLength()));

    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, length);
    jit.transfer64(BaseIndex(butterfly, length, CCallHelpers::TimesEight, -elementSize), BaseIndex(storage, length, CCallHelpers::TimesEight, -elementSize));
    jit.and32(TrustedImm32(~1), length, end);
    Jump hasNoPair = jit.branchTest32(CCallHelpers::Zero, end);
    jit.getEffectiveAddress(BaseIndex(storage, end, CCallHelpers::TimesEight), end);
    jit.move(storage, copied);
    Label copyNext = jit.label();
    jit.transferPair64(CCallHelpers::PostIndexAddress(butterfly, pairSize), CCallHelpers::PostIndexAddress(copied, pairSize));
    jit.branchPtr(CCallHelpers::Below, copied, end).linkTo(copyNext, &jit);
    hasNoPair.link(&jit);
    isEmpty.link(&jit);

    const GPRReg hole = growth.scratch;
    const GPRReg filled = length;
    loadHoleOf(jit, growth.array, hole);
    jit.store64(hole, BaseIndex(storage, length, CCallHelpers::TimesEight));
    jit.or32(TrustedImm32(1), length);
    jit.load32(Address(storage, Butterfly::offsetOfVectorLength()), end);
    jit.getEffectiveAddress(BaseIndex(storage, end, CCallHelpers::TimesEight), end);
    jit.getEffectiveAddress(BaseIndex(storage, length, CCallHelpers::TimesEight), filled);
    Label fillNext = jit.label();
    jit.storePair64(hole, hole, CCallHelpers::PostIndexAddress(filled, pairSize));
    jit.branchPtr(CCallHelpers::Below, filled, end).linkTo(fillNext, &jit);
}
#endif

static void growVector(StubAssembler& jit, const VectorGrowth& growth, std::optional<GPRReg> neededLength, JumpList& slowPath)
{
    const GPRReg array = growth.array;
    const GPRReg butterfly = growth.butterfly;
    const GPRReg length = growth.length;
    const GPRReg storage = growth.storage;
    const GPRReg allocator = growth.allocator;
    const GPRReg scratch = growth.scratch;
    const GPRReg vectorLength = allocator;
    RELEASE_ASSERT(noOverlap(array, butterfly, length, storage, allocator, scratch, instanceGPR));
    RELEASE_ASSERT(!neededLength || noOverlap(*neededLength, array, butterfly, length, storage, allocator, scratch, instanceGPR));

    slowPath.append(jit.branch32(CCallHelpers::Above, neededLength.value_or(length), TrustedImm32(maxLengthForElementwiseCopy)));
    slowPath.append(branchIfBarrierIsNeeded(jit, array, scratch));
    if (growth.isOfExtraProperties) {
        jit.load32(Address(array, JSCell::structureIDOffset()), scratch);
        slowPath.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.headerOfExtraPropertiesWithElements))));
    } else
        branchUnlessHasOriginalArrayStructure(jit, array, scratch, slowPath);
    loadAllocatorOfNextVector(jit, growth, neededLength, slowPath);
    allocateCell(jit, storage, allocator, scratch, slowPath);

    jit.load32(Address(allocator, LocalAllocator::offsetOfCellSize()), vectorLength);
    jit.sub32(TrustedImm32(sizeof(IndexingHeader)), vectorLength);
    jit.urshift32(TrustedImm32(3), vectorLength);
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    fillAndCopyByPairs(jit, growth, vectorLength);
    jit.storePtr(storage, Address(array, JSObject::butterflyOffset()));
}

void growStorageWithElements(StubAssembler& jit, const StorageGrowthWithElements& growth, JumpList& slowPath)
{
    constexpr int32_t wordSize = sizeof(EncodedJSValue);
    constexpr unsigned maxVectorLength = maxLengthForElementwiseCopy;
    static_assert(sizeof(IndexingHeader) == wordSize && MarkedSpace::sizeStep == 2 * wordSize && !(initialOutOfLineCapacity % 2));
    const GPRReg butterfly = growth.butterfly;
    const GPRReg newCapacity = growth.newCapacity;
    const GPRReg storage = growth.storage;
    const GPRReg allocator = growth.allocator;
    const GPRReg vectorLength = growth.scratch;
    RELEASE_ASSERT(noOverlap(growth.object, growth.newStructureID, butterfly, newCapacity, storage, allocator, growth.scratch, growth.addedValue, instanceGPR));

    jit.load32(Address(butterfly, Butterfly::offsetOfVectorLength()), vectorLength);
    slowPath.append(jit.branch32(CCallHelpers::Above, vectorLength, TrustedImm32(maxVectorLength)));
    const GPRReg sizeStep = storage;
    jit.add32(newCapacity, vectorLength, sizeStep);
    jit.add32(TrustedImm32(2), sizeStep);
    jit.urshift32(TrustedImm32(1), sizeStep);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, sizeStep, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, storage, allocator, growth.scratch, slowPath);

    const GPRReg index = growth.scratch;
    const GPRReg indexOfLastProperty = allocator;
    Jump isDoubled = jit.branch32(CCallHelpers::NotEqual, newCapacity, TrustedImm32(initialOutOfLineCapacity));
    constexpr int32_t firstStorageSize = (initialOutOfLineCapacity + 1) * wordSize;
    jit.addPtr(TrustedImm32(firstStorageSize), storage);
    for (unsigned i = 0; i + 1 < initialOutOfLineCapacity; ++i)
        jit.store64(TrustedImm32(0), Address(storage, static_cast<int32_t>(i) * wordSize - firstStorageSize));
    jit.store64(growth.addedValue, Address(storage, -2 * wordSize));
    jit.move(TrustedImm64(-1), indexOfLastProperty);

    Label copyWords = jit.label();
    jit.load32(Address(butterfly, Butterfly::offsetOfVectorLength()), index);
    jit.or32(TrustedImm32(1), index);
    Label copyPrevious = jit.label();
    jit.sub64(TrustedImm32(2), index);
#if CPU(X86_64)
    loadPair(jit, BaseIndex(butterfly, index, CCallHelpers::TimesEight));
    storePair(jit, BaseIndex(storage, index, CCallHelpers::TimesEight));
#else
    for (int32_t offset : { 0, wordSize })
        jit.transfer64(BaseIndex(butterfly, index, CCallHelpers::TimesEight, offset), BaseIndex(storage, index, CCallHelpers::TimesEight, offset));
#endif
    jit.branch64(CCallHelpers::NotEqual, index, indexOfLastProperty).linkTo(copyPrevious, &jit);
    installStorage(jit, growth.object, storage, growth.newStructureID);
    Jump done = jit.jump();

    isDoubled.link(&jit);
    jit.lshift32(newCapacity, TrustedImm32(3), index);
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), index);
    jit.addPtr(index, storage);
    jit.urshift32(newCapacity, TrustedImm32(1), indexOfLastProperty);
    jit.neg64(indexOfLastProperty);
    jit.sub64(TrustedImm32(1), indexOfLastProperty);
    jit.zeroExtend32ToWord(newCapacity, index);
    jit.neg64(index);
    jit.sub64(TrustedImm32(1), index);
    Label clearNext = jit.label();
    jit.store64(TrustedImm32(0), BaseIndex(storage, index, CCallHelpers::TimesEight));
    jit.add64(TrustedImm32(1), index);
    jit.branch64(CCallHelpers::NotEqual, index, indexOfLastProperty).linkTo(clearNext, &jit);
    jit.store64(growth.addedValue, BaseIndex(storage, indexOfLastProperty, CCallHelpers::TimesEight, -wordSize));
    jit.jump().linkTo(copyWords, &jit);
    done.link(&jit);
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
