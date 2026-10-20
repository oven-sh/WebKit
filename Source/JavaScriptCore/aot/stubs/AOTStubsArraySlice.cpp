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
#include "CompleteSubspace.h"
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
static_assert(descriptorOf(StubFamily::ArraySlice).numberOfForms == static_cast<unsigned>(ArraySliceRange::Whole) + 1);
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
        return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) });
    case ArraySliceRange::Whole:
        return StubSignature::withGPRResult({ gprOf(StubRegister::A1) });
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateArraySlice(StubAssembler& jit)
{
    const bool isWhole = static_cast<ArraySliceRange>(jit.key().form()) == ArraySliceRange::Whole;
    const GPRReg array = jit.operand(0);
    const ArrayCopyRegisters registers = allocateArrayCopyRegisters(jit);
    const auto [result, relativeShape, source, count, storage, scratch] = registers;
    const GPRReg length = result;
    const GPRReg storageAllocator = result;
    const GPRReg vectorLength = result;
    const GPRReg from = storage;
    const GPRReg cellAllocator = source;
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    static_assert(AOT_INSTANCE_PREFIX_OFFSET(newArrayWithDoubleHeader) == AOT_INSTANCE_PREFIX_OFFSET(newArrayWithInt32Header) + 8);
    static_assert(AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader) == AOT_INSTANCE_PREFIX_OFFSET(newArrayWithInt32Header) + 16);
    static_assert(sizeof(IndexingHeader) + (maxLengthForElementwiseCopy | 1) * sizeof(EncodedJSValue) <= MarkedSpace::largeCutoff);

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
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), storageAllocator);
    Jump isOne = jit.branch32(CCallHelpers::Equal, count, TrustedImm32(1));

    jit.or32(TrustedImm32(1), count, scratch);
    jit.add32(TrustedImm32(1), scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    jit.loadPtr(BaseIndex(storageAllocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), storageAllocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, storageAllocator));
    allocateCell(jit, storage, storageAllocator, scratch, slowPath);
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
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(arrayAllocator)), cellAllocator);
    allocateCell(jit, result, cellAllocator, scratch, slowPath);
    jit.load64(BaseIndex(instanceGPR, relativeShape, CCallHelpers::TimesFour, AOT_INSTANCE_PREFIX_OFFSET(newArrayWithInt32Header)), scratch);
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.storePair64(scratch, storage, result, TrustedImm32(0));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    isOne.link(&jit);
    jit.loadPtr(Address(storageAllocator, offsetOfArrayStorageAllocator(1)), storageAllocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, storageAllocator));
    allocateCell(jit, storage, storageAllocator, scratch, slowPath);
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
