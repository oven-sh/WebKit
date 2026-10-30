/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArrays.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "Butterfly.h"
#include "ClonedArguments.h"
#include "CompleteSubspace.h"
#include "DirectArguments.h"
#include "IndexingHeader.h"
#include "IndexingType.h"
#include "JSObject.h"
#include "MarkedSpace.h"
#include "OperationResult.h"
#include "PureNaN.h"
#include <bit>
#include <type_traits>

namespace JSC { namespace AOT {

static_assert(JSObject::butterflyOffset() == sizeof(JSCell) && sizeof(JSCell) == sizeof(uint64_t));
static_assert(sizeof(IndexingHeader) == sizeof(EncodedJSValue) && MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
static_assert(Butterfly::offsetOfPublicLength() == -static_cast<ptrdiff_t>(sizeof(IndexingHeader)) && Butterfly::offsetOfVectorLength() == Butterfly::offsetOfPublicLength() + static_cast<ptrdiff_t>(sizeof(uint32_t)));
static_assert(descriptorOf(StubFamily::ArraySlice).numberOfForms == static_cast<unsigned>(ArraySliceRange::WholeWithPropertyRoom) + 1);
static_assert(std::is_same_v<decltype(&operationAOTArraySlice), OperationReturnType<EncodedJSValue> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, int64_t, int64_t)>);
static_assert(std::is_same_v<decltype(&operationAOTArrayCopy), OperationReturnType<EncodedJSValue> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*)>);
static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2) && gprOf(StubRegister::A3) == operationArgumentGPR(3));

struct ArrayCopyRegisters {
    GPRReg result;
    GPRReg shape;
    GPRReg source;
    GPRReg count;
    GPRReg storage;
    GPRReg scratch;
};

static ArrayCopyRegisters allocateArrayCopyRegisters(StubAssembler& jit)
{
    return {
        .result = jit.result(),
        .shape = jit.allocateScratch(StubRegister::T9),
#if CPU(ARM64)
        .source = jit.allocateScratch(StubRegister::T11),
        .count = jit.allocateScratch(StubRegister::T12),
        .storage = jit.allocateScratch(StubRegister::T13),
        .scratch = jit.allocateScratch(StubRegister::T14),
#else
        .source = jit.allocateScratch(StubRegister::T10),
        .count = jit.allocateScratch(StubRegister::T11),
        .storage = jit.allocateScratch(StubRegister::T12),
        .scratch = jit.allocateScratch(StubRegister::T13),
#endif
    };
}

static void emitClampToLength(StubAssembler& jit, GPRReg unclamped, GPRReg length, GPRReg clamped)
{
    jit.move(unclamped, clamped);
    Jump isNotNegative = jit.branch64(CCallHelpers::GreaterThanOrEqual, clamped, TrustedImm32(0));
    jit.add64(length, clamped);
    Jump isFromEnd = jit.branch64(CCallHelpers::GreaterThanOrEqual, clamped, TrustedImm32(0));
    jit.move(TrustedImm32(0), clamped);
    Jump isStart = jit.jump();
    isNotNegative.link(&jit);
    Jump isInBounds = jit.branch64(CCallHelpers::LessThanOrEqual, clamped, length);
    jit.move(length, clamped);
    isInBounds.link(&jit);
    isFromEnd.link(&jit);
    isStart.link(&jit);
}

// Copies count elements, at least one, from source to the elements of storage. Only storage and shape survive.
static void emitCopyElements(StubAssembler& jit, const ArrayCopyRegisters& registers)
{
    const auto [result, shape, source, count, storage, scratch] = registers;
#if CPU(ARM64)
    const GPRReg endOfSource = count;
    const GPRReg copy = jit.allocateScratch(StubRegister::T15);
    const GPRReg second = result;
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage, copy);
    Jump isEven = jit.branchTest32(CCallHelpers::Zero, count, TrustedImm32(1));
    jit.getEffectiveAddress(BaseIndex(source, count, CCallHelpers::TimesEight), endOfSource);
    jit.load64(CCallHelpers::PostIndexAddress(source, sizeof(EncodedJSValue)), scratch);
    jit.store64(scratch, CCallHelpers::PostIndexAddress(copy, sizeof(EncodedJSValue)));
    Jump wasLast = jit.branchPtr(CCallHelpers::Equal, source, endOfSource);
    Jump hasPairs = jit.jump();
    isEven.link(&jit);
    jit.getEffectiveAddress(BaseIndex(source, count, CCallHelpers::TimesEight), endOfSource);
    hasPairs.link(&jit);
    Label copyPair = jit.label();
    jit.loadPair64(CCallHelpers::PostIndexAddress(source, 2 * sizeof(EncodedJSValue)), scratch, second);
    jit.storePair64(scratch, second, CCallHelpers::PostIndexAddress(copy, 2 * sizeof(EncodedJSValue)));
    jit.branchPtr(CCallHelpers::Below, source, endOfSource).linkTo(copyPair, &jit);
    wasLast.link(&jit);
#else
    const GPRReg endOfSource = source;
    const GPRReg endOfCopy = count;
    const GPRReg index = result;
    jit.getEffectiveAddress(BaseIndex(source, count, CCallHelpers::TimesEight), endOfSource);
    jit.move(count, index);
    jit.neg64(index);
    jit.getEffectiveAddress(BaseIndex(storage, count, CCallHelpers::TimesEight, sizeof(IndexingHeader)), endOfCopy);
    Jump isEven = jit.branchTest32(CCallHelpers::Zero, index, TrustedImm32(1));
    jit.load64(BaseIndex(endOfSource, index, CCallHelpers::TimesEight), scratch);
    jit.store64(scratch, BaseIndex(endOfCopy, index, CCallHelpers::TimesEight));
    Jump wasLast = jit.branchAdd64(CCallHelpers::Zero, TrustedImm32(1), index);
    isEven.link(&jit);
    Label copy = jit.label();
    jit.load64(BaseIndex(endOfSource, index, CCallHelpers::TimesEight), scratch);
    jit.store64(scratch, BaseIndex(endOfCopy, index, CCallHelpers::TimesEight));
    jit.load64(BaseIndex(endOfSource, index, CCallHelpers::TimesEight, sizeof(EncodedJSValue)), scratch);
    jit.store64(scratch, BaseIndex(endOfCopy, index, CCallHelpers::TimesEight, sizeof(EncodedJSValue)));
    jit.branchAdd64(CCallHelpers::NonZero, TrustedImm32(2), index).linkTo(copy, &jit);
    wasLast.link(&jit);
#endif
}

StubSignature signatureOfArraySlice(StubKey key)
{
    switch (static_cast<ArraySliceRange>(key.form())) {
    case ArraySliceRange::Bounded:
    case ArraySliceRange::BoundedWithPropertyRoom:
        return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) });
    case ArraySliceRange::Whole:
    case ArraySliceRange::WholeWithPropertyRoom:
        return StubSignature::withGPRResult({ gprOf(StubRegister::A1) });
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateArraySlice(StubAssembler& jit)
{
    const ArraySliceRange range = static_cast<ArraySliceRange>(jit.key().form());
    const bool isWhole = range == ArraySliceRange::Whole || range == ArraySliceRange::WholeWithPropertyRoom;
    const unsigned propertyRoom = range == ArraySliceRange::BoundedWithPropertyRoom || range == ArraySliceRange::WholeWithPropertyRoom ? propertyRoomOfArray : 0;
    // Turns the cell into the address of the indexing header.
    auto skipCellAndPropertyRoom = [&](GPRReg cell) {
        for (unsigned offset = 0; offset < propertyRoom; offset += sizeof(EncodedJSValue))
            jit.store64(TrustedImm32(0), Address(cell, sizeof(JSArray) + offset));
        jit.addPtr(TrustedImm32(sizeof(JSArray) + propertyRoom), cell);
    };
    const GPRReg array = jit.operand(0);
    const ArrayCopyRegisters registers = allocateArrayCopyRegisters(jit);
    const auto [result, relativeShape, source, count, storage, scratch] = registers;
    const GPRReg length = result;
    const GPRReg storageAllocator = result;
    const GPRReg vectorLength = result;
    const GPRReg from = storage;
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    static_assert(AOT_INSTANCE_PREFIX_OFFSET(newArrayWithDoubleHeader) == AOT_INSTANCE_PREFIX_OFFSET(newArrayWithInt32Header) + 8);
    static_assert(AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader) == AOT_INSTANCE_PREFIX_OFFSET(newArrayWithInt32Header) + 16);
    static_assert(sizeOfArrayWithStorage(maxLengthForElementwiseCopy | 1, propertyRoomOfArray) <= MarkedSpace::largeCutoff && sizeof(JSArray) == MarkedSpace::sizeStep && !(propertyRoomOfArray % MarkedSpace::sizeStep));

    JumpList slowPath;
    loadPristineArrayKind(jit, array, relativeShape, IndexingShapeMask);
    jit.sub32(TrustedImm32(Int32Shape), relativeShape);
    slowPath.append(jit.branch32(CCallHelpers::Above, relativeShape, TrustedImm32(ContiguousShape - Int32Shape)));
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), source);
    if (isWhole)
        jit.load32(Address(source, Butterfly::offsetOfPublicLength()), count);
    else {
        jit.load32(Address(source, Butterfly::offsetOfPublicLength()), length);
        emitClampToLength(jit, jit.operand(1), length, from);
        emitClampToLength(jit, jit.operand(2), length, count);
        jit.sub64(from, count);
        Jump hasElements = jit.branch64(CCallHelpers::GreaterThan, count, TrustedImm32(0));
        jit.move(TrustedImm32(0), count);
        hasElements.link(&jit);
        jit.getEffectiveAddress(BaseIndex(source, from, CCallHelpers::TimesEight), source);
    }
    slowPath.append(jit.branch32(CCallHelpers::Above, count, TrustedImm32(maxLengthForElementwiseCopy)));
    slowPath.append(branchIfHavingABadTime(jit));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), storageAllocator);
    Jump isOne = jit.branch32(CCallHelpers::Equal, count, TrustedImm32(1));

    jit.or32(TrustedImm32(1), count, scratch);
    jit.add32(TrustedImm32(1), scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    jit.loadPtr(BaseIndex(storageAllocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep() + (1 + propertyRoom / MarkedSpace::sizeStep) * sizeof(Allocator)), storageAllocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, storageAllocator));
    allocateCell(jit, storage, storageAllocator, scratch, slowPath);
    skipCellAndPropertyRoom(storage);
    jit.or32(TrustedImm32(1), count, vectorLength);
    jit.store32(count, Address(storage, 0));
    jit.store32(vectorLength, Address(storage, sizeof(uint32_t)));
    jit.move(TrustedImm32(0), scratch);
    Jump isNotDouble = jit.branch32(CCallHelpers::NotEqual, relativeShape, TrustedImm32(DoubleShape - Int32Shape));
    jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), scratch);
    isNotDouble.link(&jit);
    jit.store64(scratch, BaseIndex(storage, vectorLength, CCallHelpers::TimesEight));
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, count);
    emitCopyElements(jit, registers);
    isEmpty.link(&jit);

    Label hasStorage = jit.label();
    jit.addPtr(TrustedImm32(-static_cast<int32_t>(sizeof(JSArray) + propertyRoom)), storage, result);
    jit.load64(BaseIndex(instanceGPR, relativeShape, CCallHelpers::TimesFour, AOT_INSTANCE_PREFIX_OFFSET(newArrayWithInt32Header)), scratch);
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.storePair64(scratch, storage, result, TrustedImm32(0));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    isOne.link(&jit);
    jit.loadPtr(Address(storageAllocator, offsetOfArrayWithStorageAllocator(1, propertyRoom)), storageAllocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, storageAllocator));
    allocateCell(jit, storage, storageAllocator, scratch, slowPath);
    skipCellAndPropertyRoom(storage);
    jit.store32(TrustedImm32(1), Address(storage, 0));
    jit.store32(TrustedImm32(1), Address(storage, sizeof(uint32_t)));
    jit.load64(Address(source, 0), scratch);
    jit.store64(scratch, Address(storage, sizeof(IndexingHeader)));
    jit.jump().linkTo(hasStorage, &jit);

    slowPath.link(&jit);
    jit.callOperationAndReturn(isWhole ? Entry::operationAOTArrayCopy : Entry::operationAOTArraySlice, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
    });
}

StubSignature signatureOfCreateArguments(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) });
}

void generateCreateArguments(StubAssembler& jit)
{
    constexpr unsigned kind = static_cast<unsigned>(InlineAllocation::Arguments);
    constexpr unsigned wordsInFrontOfElements = initialOutOfLineCapacity + sizeof(IndexingHeader) / sizeof(EncodedJSValue);
    constexpr unsigned maxCount = MarkedSpace::largeCutoff / sizeof(EncodedJSValue) - wordsInFrontOfElements;
    static_assert(std::is_same_v<decltype(&operationAOTCreateUnmappedArguments), OperationReturnType<JSCell*> (JIT_OPERATION_ATTRIBUTES *)(Instance*, uint32_t, JSCell*, const EncodedJSValue*)>);
    static_assert(clonedArgumentsLengthPropertyOffset == firstOutOfLineOffset && ClonedArguments::offsetOfCallee() == sizeof(JSNonFinalObject) && sizeof(ClonedArguments) == sizeof(JSNonFinalObject) + sizeof(void*));
    const GPRReg count = jit.operand(0);
    const GPRReg rest = jit.operand(1);
    const GPRReg parameters = jit.operand(2);
    const auto [result, position, source, restLength, storage, scratch] = allocateArrayCopyRegisters(jit);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const GPRReg allocator = position;
    const GPRReg numberOfParameters = result;
    const GPRReg copiedRest = result;
    RELEASE_ASSERT(noOverlap(count, rest, parameters, result, position, source, restLength, storage, scratch));

    JumpList slowPath;
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].allocator))));
    slowPath.append(jit.branch32(CCallHelpers::Above, count, TrustedImm32(maxCount)));
    jit.loadPtr(Address(rest, JSObject::butterflyOffset()), source);
    jit.load32(Address(source, Butterfly::offsetOfPublicLength()), restLength);
    slowPath.append(jit.branch32(CCallHelpers::Above, restLength, count));

    jit.add32(TrustedImm32(wordsInFrontOfElements + 1), count, scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, storage, allocator, scratch, slowPath);
    for (unsigned unused = 0; unused + 1 < initialOutOfLineCapacity; ++unused)
        jit.store64(TrustedImm32(0), Address(storage, unused * sizeof(EncodedJSValue)));
    boxInt32(jit, count, scratch);
    jit.store64(scratch, Address(storage, (initialOutOfLineCapacity - 1) * sizeof(EncodedJSValue)));
    jit.addPtr(TrustedImm32(wordsInFrontOfElements * sizeof(EncodedJSValue)), storage);
    jit.store32(count, Address(storage, Butterfly::offsetOfPublicLength()));
    jit.store32(count, Address(storage, Butterfly::offsetOfVectorLength()));

    jit.move(count, numberOfParameters);
    jit.sub32(restLength, numberOfParameters);
    Jump hasNoParameters = jit.branchTest32(CCallHelpers::Zero, numberOfParameters);
    jit.move(TrustedImm32(0), position);
    Label copyParameter = jit.label();
    jit.load64(BaseIndex(parameters, position, CCallHelpers::TimesEight), scratch);
    jit.store64(scratch, BaseIndex(storage, position, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), position);
    jit.branch32(CCallHelpers::Below, position, numberOfParameters).linkTo(copyParameter, &jit);
    hasNoParameters.link(&jit);

    Jump hasNoRest = jit.branchTest32(CCallHelpers::Zero, restLength);
    jit.getEffectiveAddress(BaseIndex(storage, numberOfParameters, CCallHelpers::TimesEight), copiedRest);
    jit.move(TrustedImm32(0), position);
    jit.load8(Address(rest, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.and32(TrustedImm32(IndexingShapeMask), scratch);
    Jump holdsDoubles = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(DoubleShape));
    Jump holdsValues = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ContiguousShape));
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(Int32Shape)));
    holdsValues.link(&jit);
    Label copyValue = jit.label();
    jit.load64(BaseIndex(source, position, CCallHelpers::TimesEight), scratch);
    slowPath.append(jit.branchTest64(CCallHelpers::Zero, scratch));
    jit.store64(scratch, BaseIndex(copiedRest, position, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), position);
    jit.branch32(CCallHelpers::Below, position, restLength).linkTo(copyValue, &jit);
    hasNoRest.link(&jit);

    Label isFilled = jit.label();
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].allocator)), allocator);
    allocateCell(jit, result, allocator, scratch, slowPath);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].header)), scratch);
    jit.storePair64(scratch, storage, result, TrustedImm32(0));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.calleeOfStrictArguments)), scratch);
    jit.storePtr(scratch, Address(result, ClonedArguments::offsetOfCallee()));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    holdsDoubles.link(&jit);
    Label boxNumber = jit.label();
    jit.loadDouble(BaseIndex(source, position, CCallHelpers::TimesEight), number);
    boxDouble(jit, PureDouble::branchIfNaN(jit, number, slowPath), scratch);
    jit.store64(scratch, BaseIndex(copiedRest, position, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), position);
    jit.branch32(CCallHelpers::Below, position, restLength).linkTo(boxNumber, &jit);
    jit.jump().linkTo(isFilled, &jit);

    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTCreateUnmappedArguments, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
    });
}

StubSignature signatureOfCreateMappedArguments(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3), gprOf(StubRegister::A4), gprOf(StubRegister::A5) });
}

void generateCreateMappedArguments(StubAssembler& jit)
{
    constexpr unsigned kind = static_cast<unsigned>(InlineAllocation::MappedArguments);
    constexpr int32_t offsetOfSlots = WTF::roundUpToMultipleOf<sizeof(EncodedJSValue)>(sizeof(DirectArguments));
    constexpr unsigned wordsInFrontOfSlots = offsetOfSlots / sizeof(EncodedJSValue);
    constexpr unsigned maxCapacity = MarkedSpace::largeCutoff / sizeof(EncodedJSValue) - wordsInFrontOfSlots;
    static_assert(std::is_same_v<decltype(&operationAOTCreateMappedArguments), OperationReturnType<JSCell*> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, uint32_t, JSCell*, const EncodedJSValue*)>);
    static_assert(gprOf(StubRegister::A4) == operationArgumentGPR(4) && areDistinctOnEveryISA(StubRegister::R0, StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A4, StubRegister::A5, StubRegister::T9, StubRegister::T12, StubRegister::T13));
    const GPRReg callee = jit.operand(0);
    const GPRReg count = jit.operand(1);
    const GPRReg rest = jit.operand(2);
    const GPRReg parameters = jit.operand(3);
    const GPRReg capacity = jit.clobberOperand(4);
    const GPRReg result = jit.result();
    const GPRReg source = jit.allocateScratch(StubRegister::T9);
    const GPRReg restLength = jit.allocateScratch(StubRegister::T12);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const GPRReg allocator = source;
    const GPRReg position = source;
    const GPRReg numberOfParameters = capacity;
    const GPRReg copiedRest = capacity;
    RELEASE_ASSERT(DirectArguments::offsetOfSlot(0) == static_cast<size_t>(offsetOfSlots));

    JumpList slowPath;
    Jump holdsEveryArgument = jit.branch32(CCallHelpers::AboveOrEqual, capacity, count);
    jit.move(count, capacity);
    holdsEveryArgument.link(&jit);
    slowPath.append(jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(maxCapacity)));
    slowPath.append(jit.branchTest64(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].header))));
    jit.loadPtr(Address(rest, JSObject::butterflyOffset()), source);
    jit.load32(Address(source, Butterfly::offsetOfPublicLength()), restLength);
    slowPath.append(jit.branch32(CCallHelpers::Above, restLength, count));
    Jump needsNoStorage = jit.branchTest32(CCallHelpers::Zero, restLength);
    jit.load8(Address(rest, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.and32(TrustedImm32(IndexingShapeMask), scratch);
    jit.sub32(TrustedImm32(Int32Shape), scratch);
    slowPath.append(jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(ContiguousShape - Int32Shape)));
    needsNoStorage.link(&jit);

    jit.add32(TrustedImm32(wordsInFrontOfSlots + 1), capacity, scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, result, allocator, scratch, slowPath);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].header)), scratch);
    jit.store64(scratch, Address(result));
    jit.store64(TrustedImm32(0), Address(result, JSObject::butterflyOffset()));
    jit.store64(TrustedImm32(0), Address(result, DirectArguments::offsetOfModifiedArgumentsDescriptor()));
    jit.storePtr(callee, Address(result, DirectArguments::offsetOfCallee()));
    jit.store32(count, Address(result, DirectArguments::offsetOfLength()));
    jit.store32(capacity, Address(result, DirectArguments::offsetOfMinCapacity()));
    jit.store64(TrustedImm32(0), Address(result, DirectArguments::offsetOfMappedArguments()));

    jit.sub32(restLength, numberOfParameters);
    Jump hasNoParameters = jit.branchTest32(CCallHelpers::Zero, numberOfParameters);
    jit.move(TrustedImm32(0), position);
    Label copyParameter = jit.label();
    jit.load64(BaseIndex(parameters, position, CCallHelpers::TimesEight), scratch);
    jit.store64(scratch, BaseIndex(result, position, CCallHelpers::TimesEight, offsetOfSlots));
    jit.add32(TrustedImm32(1), position);
    jit.branch32(CCallHelpers::Below, position, numberOfParameters).linkTo(copyParameter, &jit);
    hasNoParameters.link(&jit);

    JumpList findsHole;
    Jump hasNoRest = jit.branchTest32(CCallHelpers::Zero, restLength);
    jit.getEffectiveAddress(BaseIndex(result, numberOfParameters, CCallHelpers::TimesEight, offsetOfSlots), copiedRest);
    jit.loadPtr(Address(rest, JSObject::butterflyOffset()), source);
    jit.load8(Address(rest, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.and32(TrustedImm32(IndexingShapeMask), scratch);
    Jump holdsDoubles = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(DoubleShape));
    Label copyValue = jit.label();
    jit.sub32(TrustedImm32(1), restLength);
    jit.load64(BaseIndex(source, restLength, CCallHelpers::TimesEight), scratch);
    findsHole.append(jit.branchTest64(CCallHelpers::Zero, scratch));
    jit.store64(scratch, BaseIndex(copiedRest, restLength, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, restLength).linkTo(copyValue, &jit);
    hasNoRest.link(&jit);

    Label isFilled = jit.label();
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    holdsDoubles.link(&jit);
    Label boxNumber = jit.label();
    jit.sub32(TrustedImm32(1), restLength);
    jit.loadDouble(BaseIndex(source, restLength, CCallHelpers::TimesEight), number);
    boxDouble(jit, PureDouble::branchIfNaN(jit, number, findsHole), scratch);
    jit.store64(scratch, BaseIndex(copiedRest, restLength, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, restLength).linkTo(boxNumber, &jit);
    jit.jump().linkTo(isFilled, &jit);

    // The collector may find the cell that is given up: it has no slots.
    findsHole.link(&jit);
    jit.store32(TrustedImm32(0), Address(result, DirectArguments::offsetOfLength()));
    jit.store32(TrustedImm32(0), Address(result, DirectArguments::offsetOfMinCapacity()));
    fenceAfterAllocation(jit);
    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTCreateMappedArguments, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
