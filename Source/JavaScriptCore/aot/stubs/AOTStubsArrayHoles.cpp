/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArrays.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTCompactTable.h"
#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsHashTables.h"
#include "ArrayConventions.h"
#include "Butterfly.h"
#include "CompleteSubspace.h"
#include "IndexingHeader.h"
#include "IndexingType.h"
#include "JSCJSValueInlines.h"
#include "JSCellButterfly.h"
#include "JSObject.h"
#include "MarkedSpace.h"
#include "OperationResult.h"
#include "PureNaN.h"
#include <bit>
#include <initializer_list>
#include <type_traits>

namespace JSC { namespace AOT {

static_assert(descriptorOf(StubFamily::NewArrayWithLength).numberOfForms == static_cast<unsigned>(ArrayElementKind::Contiguous) + 1);
static_assert(descriptorOf(StubFamily::NewArrayOfLength).numberOfForms == static_cast<unsigned>(ArrayElementKind::Contiguous) + 1);
static_assert(std::is_same_v<decltype(&operationAOTNewArrayWithHoles), OperationReturnType<JSCell*> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, uint64_t, uint32_t, uint32_t)>);
static_assert(std::is_same_v<decltype(&operationAOTNewArrayOfGiven), OperationReturnType<JSCell*> (JIT_OPERATION_ATTRIBUTES *)(Instance*, EncodedJSValue)>);
static_assert(descriptorOf(StubFamily::NewEmptyArrayAtSite).numberOfForms == static_cast<unsigned>(ArrayElementKind::Contiguous) + 1);
static_assert(std::is_same_v<decltype(&operationAOTNewArrayAtReviewedSite), OperationReturnType<JSCell*> (JIT_OPERATION_ATTRIBUTES *)(Instance*, void*, uint32_t)>);
static_assert(static_cast<unsigned>(ArrayElementKind::Int32) == 0 && static_cast<unsigned>(ArrayElementKind::Double) == 1 && descriptorOf(StubFamily::NewArrayWithLengthAtSite).numberOfForms == 2);
static_assert(std::is_same_v<decltype(&operationAOTNewArrayWithLengthAtReviewedSite), JSCell* (JIT_OPERATION_ATTRIBUTES *)(Instance*, void*, uint32_t)>);

enum class HoleArrayOperand : uint8_t { None, LengthAndCapacity, Capacity, Length };

static void emitLoadAllocatorForCapacity(StubAssembler& jit, GPRReg capacity, GPRReg scratch, GPRReg allocator)
{
    jit.add32(TrustedImm32(1), capacity, scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    static_assert(sizeof(JSArray) == MarkedSpace::sizeStep);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep() + sizeof(Allocator)), allocator);
}

static void emitArrayOfHoles(StubAssembler& jit, IndexingType indexingType, HoleArrayOperand operand, unsigned fixedCapacity = 0)
{
    static_assert(sizeOfArrayWithStorage(maxHoleArrayCapacity) <= MarkedSpace::largeCutoff && maxHoleArrayCapacity & 1 && arrayLiteralVectorLength(0) >= 2);
    RELEASE_ASSERT(indexingType == ArrayWithDouble || indexingType == ArrayWithInt32 || indexingType == ArrayWithContiguous);
    RELEASE_ASSERT(operand != HoleArrayOperand::None || fixedCapacity & 1);
    const GPRReg array = jit.result();
    const auto [allocator, length] = [&]() -> std::pair<GPRReg, GPRReg> {
        switch (operand) {
        case HoleArrayOperand::None:
            return { jit.allocateScratch(StubRegister::T9), InvalidGPRReg };
        case HoleArrayOperand::LengthAndCapacity:
        case HoleArrayOperand::Length:
            return { jit.clobberOperand(0), jit.allocateScratch(StubRegister::T14) };
        case HoleArrayOperand::Capacity:
            return { jit.clobberOperand(0), InvalidGPRReg };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }();
    const GPRReg scratch = jit.allocateScratch(arrayAllocationScratchRegister);
    const GPRReg storage = jit.allocateScratch(arrayStorageRegister);
    const GPRReg capacity = jit.allocateScratch(StubRegister::T13);
    const TrustedImm64 hole(indexingType == ArrayWithDouble ? std::bit_cast<int64_t>(PNaN) : JSValue::encode(JSValue()));

    JumpList slowPath;
    JumpList callerSlowPath;
    JumpList isLeftToOperation;
    bool hasAllocatorInInstance = operand == HoleArrayOperand::None && ShortArrayWords::indexOfAllocator(fixedCapacity) < ShortArrayWords::numberOfAllocators;
    Jump hasNoAllocatorInInstance;
    Label hasAllocator;
    switch (operand) {
    case HoleArrayOperand::None:
        if (!hasAllocatorInInstance)
            callerSlowPath.append(branchIfHavingABadTime(jit));
        break;
    case HoleArrayOperand::LengthAndCapacity:
    case HoleArrayOperand::Capacity:
        break;
    case HoleArrayOperand::Length:
        isLeftToOperation.append(jit.branch64(CCallHelpers::Above, allocator, TrustedImm32(maxHoleArrayCapacity)));
        isLeftToOperation.append(branchIfHavingABadTime(jit));
        break;
    }
    switch (operand) {
    case HoleArrayOperand::None:
        if (hasAllocatorInInstance) {
            jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(shortArrays.allocators) + ShortArrayWords::indexOfAllocator(fixedCapacity) * sizeof(void*)), allocator);
            break;
        }
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
        jit.loadPtr(Address(allocator, offsetOfArrayWithStorageAllocator(fixedCapacity)), allocator);
        break;
    case HoleArrayOperand::LengthAndCapacity:
        jit.and32(TrustedImm32((1 << holeArrayCapacityShift) - 1), allocator, length);
        jit.urshift32(allocator, TrustedImm32(holeArrayCapacityShift), capacity);
        static_assert(ShortArrayWords::indexOfAllocator(2 * ShortArrayWords::numberOfAllocators - 1) == ShortArrayWords::numberOfAllocators - 1);
        hasNoAllocatorInInstance = jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(2 * ShortArrayWords::numberOfAllocators - 1));
        jit.urshift32(capacity, TrustedImm32(1), scratch);
        jit.loadPtr(BaseIndex(instanceGPR, scratch, CCallHelpers::TimesEight, static_cast<int32_t>(AOT_INSTANCE_OFFSET(shortArrays.allocators))), allocator);
        hasAllocator = jit.label();
        break;
    case HoleArrayOperand::Capacity:
        jit.or32(TrustedImm32(1), allocator, capacity);
        slowPath.append(jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(maxHoleArrayCapacity)));
        emitLoadAllocatorForCapacity(jit, capacity, scratch, allocator);
        break;
    case HoleArrayOperand::Length:
        jit.move(allocator, length);
        jit.or32(TrustedImm32(1), allocator, capacity);
        emitLoadAllocatorForCapacity(jit, capacity, scratch, allocator);
        break;
    }
    if (!hasAllocatorInInstance && operand != HoleArrayOperand::LengthAndCapacity)
        slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    std::optional<unsigned> cellSize;
    if (operand == HoleArrayOperand::None && sizeOfArrayWithStorage(fixedCapacity) <= MarkedSpace::preciseCutoff)
        cellSize = sizeOfArrayWithStorage(fixedCapacity);
    const AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, array, allocator, scratch, slowPath, cellSize);
    constexpr int32_t offsetOfLength = sizeof(JSArray);
    constexpr int32_t offsetOfCapacity = offsetOfLength + sizeof(uint32_t);
    jit.addPtr(TrustedImm32(sizeof(JSArray) + sizeof(IndexingHeader)), array, storage);

    switch (operand) {
    case HoleArrayOperand::None: {
        jit.store64(TrustedImm64(static_cast<uint64_t>(fixedCapacity) << 32), Address(array, offsetOfLength));
        jit.move(hole, scratch);
        unsigned i = 0;
        for (; i + 1 < fixedCapacity; i += 2)
            jit.storePair64(scratch, scratch, storage, TrustedImm32(i * sizeof(EncodedJSValue)));
        if (i < fixedCapacity)
            jit.store64(scratch, Address(storage, i * sizeof(EncodedJSValue)));
        break;
    }
    case HoleArrayOperand::LengthAndCapacity: {
        jit.store32(length, Address(array, offsetOfLength));
        jit.store32(capacity, Address(array, offsetOfCapacity));
        const GPRReg cursor = allocator;
        JumpList isFull;
        isFull.append(jit.branch32(CCallHelpers::AboveOrEqual, length, capacity));
        jit.move(hole, scratch);
        jit.store64(scratch, BaseIndex(storage, length, CCallHelpers::TimesEight));
        jit.or32(TrustedImm32(1), length);
        isFull.append(jit.branch32(CCallHelpers::AboveOrEqual, length, capacity));
        jit.getEffectiveAddress(BaseIndex(storage, length, CCallHelpers::TimesEight), cursor);
        jit.getEffectiveAddress(BaseIndex(storage, capacity, CCallHelpers::TimesEight), capacity);
        Label fillByPairs = jit.label();
#if CPU(ARM64)
        jit.storePair64(scratch, scratch, CCallHelpers::PostIndexAddress(cursor, 2 * sizeof(EncodedJSValue)));
#else
        jit.storePair64(scratch, scratch, cursor, TrustedImm32(0));
        jit.addPtr(TrustedImm32(2 * sizeof(EncodedJSValue)), cursor);
#endif
        jit.branchPtr(CCallHelpers::Below, cursor, capacity).linkTo(fillByPairs, &jit);
        isFull.link(&jit);
        break;
    }
    case HoleArrayOperand::Capacity: {
        const GPRReg cursor = allocator;
        jit.store32(TrustedImm32(0), Address(array, offsetOfLength));
        jit.store32(capacity, Address(array, offsetOfCapacity));
        jit.move(hole, scratch);
        jit.store64(scratch, Address(storage));
        jit.getEffectiveAddress(BaseIndex(storage, capacity, CCallHelpers::TimesEight), capacity);
        jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), storage, cursor);
        Label fillByPairs = jit.label();
#if CPU(ARM64)
        jit.storePair64(scratch, scratch, CCallHelpers::PostIndexAddress(cursor, 2 * sizeof(EncodedJSValue)));
#else
        jit.storePair64(scratch, scratch, cursor, TrustedImm32(0));
        jit.addPtr(TrustedImm32(2 * sizeof(EncodedJSValue)), cursor);
#endif
        jit.branchPtr(CCallHelpers::Below, cursor, capacity).linkTo(fillByPairs, &jit);
        break;
    }
    case HoleArrayOperand::Length: {
        const GPRReg cursor = allocator;
        jit.store32(length, Address(array, offsetOfLength));
        jit.store32(capacity, Address(array, offsetOfCapacity));
        jit.move(hole, scratch);
        jit.store64(scratch, Address(storage));
        jit.getEffectiveAddress(BaseIndex(storage, capacity, CCallHelpers::TimesEight), capacity);
        jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), storage, cursor);
        Jump hasRoomForOne = jit.branchPtr(CCallHelpers::AboveOrEqual, cursor, capacity);
        Label fillByPairs = jit.label();
#if CPU(ARM64)
        jit.storePair64(scratch, scratch, CCallHelpers::PostIndexAddress(cursor, 2 * sizeof(EncodedJSValue)));
#else
        jit.storePair64(scratch, scratch, cursor, TrustedImm32(0));
        jit.addPtr(TrustedImm32(2 * sizeof(EncodedJSValue)), cursor);
#endif
        jit.branchPtr(CCallHelpers::Below, cursor, capacity).linkTo(fillByPairs, &jit);
        hasRoomForOne.link(&jit);
        break;
    }
    }
    jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(indexingType)), scratch);
    storeHeaderAndButterfly(jit, scratch, storage, array);
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    if (operand == HoleArrayOperand::LengthAndCapacity) {
        hasNoAllocatorInInstance.link(&jit);
        slowPath.append(jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(maxHoleArrayCapacity)));
        callerSlowPath.append(branchIfHavingABadTime(jit));
        emitLoadAllocatorForCapacity(jit, capacity, scratch, allocator);
        jit.branchTestPtr(CCallHelpers::NonZero, allocator).linkTo(hasAllocator, &jit);
        slowPath.append(jit.jump());
    }
    emitLateRefill(jit, refill, slowPath);
    if (operand == HoleArrayOperand::LengthAndCapacity) {
        slowPath.link(&jit);
        slowPath = JumpList();
        callerSlowPath.append(branchIfHavingABadTime(jit));
        slowPath.append(jit.jump());
    }
    if (!isLeftToOperation.empty()) {
        isLeftToOperation.link(&jit);
        jit.move(TrustedImm32(0), scratch);
        OperationCall call {
            .returns = OperationCall::Returns::Value,
            .saves = OperationCall::Saves::CallerSavedRegisters,
            .secondArgumentIn = scratch,
            .thirdArgumentIn = allocator,
        };
        call.constantArguments[0] = OperationCall::ConstantArgument { operationArgumentGPR(3), static_cast<uint32_t>(indexingType) };
        call.constantArguments[1] = OperationCall::ConstantArgument { operationArgumentGPR(4), 0 };
        jit.callOperationAndReturn(Entry::operationAOTNewArrayWithHoles, call);
    }
    if (!callerSlowPath.empty()) {
        callerSlowPath.link(&jit);
        if (operand == HoleArrayOperand::None && indexingType == ArrayWithDouble) {
            const GPRReg countAndNumberMask = capacity;
            RELEASE_ASSERT(signatureOfArrayLiteralWithDoubleSlow(arrayLiteralWithDoubleSlow()).operands[0] == Reg(countAndNumberMask));
            jit.move(TrustedImm32(0), countAndNumberMask);
            jit.jumpToStub(arrayLiteralWithDoubleSlow());
        } else {
            jit.move(TrustedImm32(0), array);
            jit.returnToCaller();
        }
    }

    slowPath.link(&jit);
    const GPRReg lengthAndCapacity = storage;
    const GPRReg flags = capacity;
    switch (operand) {
    case HoleArrayOperand::None:
        jit.move(TrustedImm32(fixedCapacity << holeArrayCapacityShift), lengthAndCapacity);
        break;
    case HoleArrayOperand::LengthAndCapacity:
    case HoleArrayOperand::Length:
        jit.lshift32(capacity, TrustedImm32(holeArrayCapacityShift), lengthAndCapacity);
        jit.or32(length, lengthAndCapacity);
        break;
    case HoleArrayOperand::Capacity:
        jit.lshift32(capacity, TrustedImm32(holeArrayCapacityShift), lengthAndCapacity);
        break;
    }
    jit.move(TrustedImm32(indexingType), flags);
    const StubSignature slow = signatureOfNewArrayOfHolesSlow(newArrayOfHolesSlow());
    RELEASE_ASSERT(slow.operands[0] == Reg(lengthAndCapacity) && slow.operands[1] == Reg(flags));
    jit.jumpToStub(newArrayOfHolesSlow());
}

StubSignature signatureOfNewArrayOfHolesSlow(StubKey) { return StubSignature::withGPRResult({ gprOf(arrayStorageRegister), gprOf(StubRegister::T13) }); }

void generateNewArrayOfHolesSlow(StubAssembler& jit)
{
    jit.callOperationAndReturn(Entry::operationAOTNewArrayOfHoles, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = jit.operand(0),
        .thirdArgumentIn = jit.operand(1),
    });
}

StubSignature signatureOfNewArrayFrontEnd(StubKey) { return operationFrontEndSignature(2); }

void generateNewArrayFrontEnd(StubAssembler& jit)
{
    static_assert(arrayLiteralVectorLength(2) == 3 && arrayLiteralVectorLength(3) == 3 && maxHoleArrayCapacity & 1);
    static_assert(maxHoleArrayCapacity < 1u << capacityShiftInArrayLiteralCount);
    const GPRReg count = jit.operand(0);
    const GPRReg values = jit.operand(1);
    const GPRReg array = jit.result();
    const GPRReg allocator = jit.allocateScratch(StubRegister::T9);
    const GPRReg capacity = jit.allocateScratch(StubRegister::T10);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const GPRReg storage = jit.allocateScratch(StubRegister::T12);
    const GPRReg position = allocator;
    RELEASE_ASSERT(noOverlap(count, values, array, allocator, capacity, scratch, storage));

    JumpList callsOperation;
    jit.zeroExtend32ToWord(count, count);
    jit.sub32(count, TrustedImm32(1), scratch);
    callsOperation.append(jit.branch32(CCallHelpers::AboveOrEqual, scratch, TrustedImm32(maxHoleArrayCapacity)));
    callsOperation.append(branchIfHavingABadTime(jit));
    jit.or32(TrustedImm32(1), count, capacity);
    emitLoadAllocatorForCapacity(jit, capacity, scratch, allocator);
    callsOperation.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, array, allocator, scratch, callsOperation);

    jit.addPtr(TrustedImm32(sizeof(JSArray)), array, storage);
    jit.store32(count, Address(storage, 0));
    jit.store32(capacity, Address(storage, sizeof(uint32_t)));
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.store64(TrustedImm32(0), BaseIndex(storage, capacity, CCallHelpers::TimesEight, -static_cast<int32_t>(sizeof(EncodedJSValue))));
    jit.move(TrustedImm32(0), position);
    Label copy = jit.label();
    jit.load64(BaseIndex(values, position, CCallHelpers::TimesEight), scratch);
    jit.store64(scratch, BaseIndex(storage, position, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), position);
    jit.branch32(CCallHelpers::Below, position, count).linkTo(copy, &jit);
    jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(ArrayWithContiguous)), scratch);
    storeHeaderAndButterfly(jit, scratch, storage, array);
    fenceAfterAllocation(jit);
    jit.move(TrustedImm32(0), jit.secondResult());
    jit.returnToCaller();

    callsOperation.link(&jit);
    jit.tailCallOperation(Entry::RawNewArray, OperationCall::FirstArgument::Instance);
}

StubSignature signatureOfNewArrayFromBuffer(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T9), gprOf(StubRegister::T14) }); }

void generateNewArrayFromBuffer(StubAssembler& jit)
{
    static_assert(maxHoleArrayCapacity & 1 && maxHoleArrayCapacity < 1u << capacityShiftInArrayLiteralCount);
    const GPRReg count = jit.clobberOperand(0);
    const GPRReg values = jit.operand(1);
    const GPRReg array = jit.result();
    const GPRReg allocator = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    const GPRReg storage = allocator;
    constexpr int32_t offsetOfWordInFront = -static_cast<int32_t>(sizeof(EncodedJSValue));
    RELEASE_ASSERT(noOverlap(count, values, array, allocator, scratch));

    JumpList callsOperation;
    jit.zeroExtend32ToWord(count, count);
    callsOperation.append(jit.branch32(CCallHelpers::Above, count, TrustedImm32(maxHoleArrayCapacity)));
    callsOperation.append(branchIfHavingABadTime(jit));
    jit.or32(TrustedImm32(1), count, scratch);
    emitLoadAllocatorForCapacity(jit, scratch, scratch, allocator);
    callsOperation.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, array, allocator, scratch, callsOperation);

    jit.addPtr(TrustedImm32(sizeof(JSArray)), array, storage);
    jit.or32(TrustedImm32(1), count, scratch);
    jit.store32(count, Address(storage, 0));
    jit.store32(scratch, Address(storage, sizeof(uint32_t)));
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.store64(TrustedImm32(0), BaseIndex(storage, scratch, CCallHelpers::TimesEight, offsetOfWordInFront));
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, count);
    Label copy = jit.label();
    jit.load64(BaseIndex(values, count, CCallHelpers::TimesEight, offsetOfWordInFront), scratch);
    jit.store64(scratch, BaseIndex(storage, count, CCallHelpers::TimesEight, offsetOfWordInFront));
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(1), count).linkTo(copy, &jit);
    isEmpty.link(&jit);
    jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(ArrayWithContiguous)), scratch);
    storeHeaderAndButterfly(jit, scratch, storage, array);
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    callsOperation.link(&jit);
    Jump hasValues = jit.branchTest32(CCallHelpers::NonZero, count);
    jit.move(TrustedImm32(1 << capacityShiftInArrayLiteralCount), count);
    hasValues.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNewArray, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = count,
        .thirdArgumentIn = values,
    });
}

StubSignature signatureOfNewArrayOfList(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }

void generateNewArrayOfList(StubAssembler& jit)
{
    static_assert(Int32Shape + 4 == ContiguousShape && AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader) == AOT_INSTANCE_PREFIX_OFFSET(newArrayWithInt32Header) + 16);
    static_assert(Int32Shape + 2 == DoubleShape && AOT_INSTANCE_PREFIX_OFFSET(newArrayWithDoubleHeader) == AOT_INSTANCE_PREFIX_OFFSET(newArrayWithInt32Header) + 8);
    static_assert(std::is_same_v<decltype(&operationAOTNewArrayOfList), JSCell* (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, uint32_t)>);
    const bool isContiguous = jit.key().form();
    const GPRReg iterable = jit.operand(0);
    const GPRReg capacity = jit.clobberOperand(1);
    const GPRReg array = jit.result();
    const GPRReg relativeShape = jit.allocateScratch(StubRegister::T9);
    const GPRReg source = jit.allocateScratch(StubRegister::T10);
    const GPRReg count = jit.allocateScratch(StubRegister::T11);
    const GPRReg storage = jit.allocateScratch(StubRegister::T12);
    const GPRReg element = jit.allocateScratch(StubRegister::T13);
    const GPRReg position = jit.allocateScratch(StubRegister::T14);
    const GPRReg allocator = position;
    RELEASE_ASSERT(noOverlap(iterable, capacity, array, relativeShape, source, count, storage, element, position));

    JumpList isNotServed;
    isNotServed.append(jit.branchIfNotCell(iterable));
    Jump isNoArray = jit.branch8(CCallHelpers::NotEqual, Address(iterable, JSCell::typeInfoTypeOffset()), TrustedImm32(ArrayType));
    loadPristineArrayKind(jit, iterable, relativeShape, IndexingShapeMask);
    jit.sub32(TrustedImm32(Int32Shape), relativeShape);
    isNotServed.append(jit.branch32(CCallHelpers::Above, relativeShape, TrustedImm32(ContiguousShape - Int32Shape)));
    jit.loadPtr(Address(iterable, JSObject::butterflyOffset()), source);
    jit.load32(Address(source, Butterfly::offsetOfPublicLength()), count);
    isNotServed.append(jit.branch32(CCallHelpers::Above, count, TrustedImm32(maxHoleArrayCapacity)));
    jit.add32(count, capacity);
    jit.or32(TrustedImm32(1), capacity);
    isNotServed.append(jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(maxHoleArrayCapacity)));
    isNotServed.append(branchIfHavingABadTime(jit));
    JumpList callsOperation;
    emitLoadAllocatorForCapacity(jit, capacity, element, allocator);
    callsOperation.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, array, allocator, element, callsOperation);

    jit.addPtr(TrustedImm32(sizeof(JSArray)), array, storage);
    jit.store32(count, Address(storage, 0));
    jit.store32(capacity, Address(storage, sizeof(uint32_t)));
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    Jump holdsFractions = jit.branch32(CCallHelpers::Equal, relativeShape, TrustedImm32(DoubleShape - Int32Shape));
    Label clear = jit.label();
    jit.sub32(TrustedImm32(1), capacity);
    jit.store64(TrustedImm32(0), BaseIndex(storage, capacity, CCallHelpers::TimesEight));
    jit.branch32(CCallHelpers::Above, capacity, count).linkTo(clear, &jit);
    jit.move(TrustedImm32(isContiguous ? ContiguousShape - Int32Shape : 0), relativeShape);
    JumpList isFilled;
    isFilled.append(jit.branchTest32(CCallHelpers::Zero, count));
    jit.move(TrustedImm32(0), position);
    auto copyPairs = [&](const auto& leaveUnlessTaken) {
        const GPRReg next = relativeShape;
        constexpr int32_t distanceToNext = sizeof(EncodedJSValue);
        JumpList leaves;
        jit.sub32(TrustedImm32(1), count);
        leaves.append(jit.branch32(CCallHelpers::AboveOrEqual, position, count));
        Label copyPair = jit.label();
        jit.load64(BaseIndex(source, position, CCallHelpers::TimesEight), element);
        jit.load64(BaseIndex(source, position, CCallHelpers::TimesEight, distanceToNext), next);
        leaveUnlessTaken(element, leaves);
        leaveUnlessTaken(next, leaves);
        jit.store64(element, BaseIndex(storage, position, CCallHelpers::TimesEight));
        jit.store64(next, BaseIndex(storage, position, CCallHelpers::TimesEight, distanceToNext));
        jit.add32(TrustedImm32(2), position);
        jit.branch32(CCallHelpers::Below, position, count).linkTo(copyPair, &jit);
        leaves.link(&jit);
        jit.add32(TrustedImm32(1), count);
    };
    if (!isContiguous) {
        copyPairs([&](GPRReg value, JumpList& leaves) { leaves.append(jit.branchIfNotInt32(value)); });
        jit.move(TrustedImm32(0), relativeShape);
        isFilled.append(jit.branch32(CCallHelpers::AboveOrEqual, position, count));
        Label copyWholeNumber = jit.label();
        jit.load64(BaseIndex(source, position, CCallHelpers::TimesEight), element);
        Jump isNoWholeNumber = jit.branchIfNotInt32(element);
        jit.store64(element, BaseIndex(storage, position, CCallHelpers::TimesEight));
        jit.add32(TrustedImm32(1), position);
        jit.branch32(CCallHelpers::Below, position, count).linkTo(copyWholeNumber, &jit);
        isFilled.append(jit.jump());
        isNoWholeNumber.link(&jit);
    }
    copyPairs([&](GPRReg value, JumpList& leaves) { leaves.append(jit.branchTest64(CCallHelpers::Zero, value)); });
    jit.move(TrustedImm32(ContiguousShape - Int32Shape), relativeShape);
    isFilled.append(jit.branch32(CCallHelpers::AboveOrEqual, position, count));
    Label copy = jit.label();
    jit.load64(BaseIndex(source, position, CCallHelpers::TimesEight), element);
    Jump isHole = jit.branchTest64(CCallHelpers::Zero, element);
    Label behindHole = jit.label();
    jit.store64(element, BaseIndex(storage, position, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), position);
    jit.branch32(CCallHelpers::Below, position, count).linkTo(copy, &jit);
    Label isFilledHere = jit.label();
    isFilled.link(&jit);
    jit.load64(BaseIndex(instanceGPR, relativeShape, CCallHelpers::TimesFour, AOT_INSTANCE_PREFIX_OFFSET(newArrayWithInt32Header)), element);
    storeHeaderAndButterfly(jit, element, storage, array);
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    isHole.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), element);
    jit.jump().linkTo(behindHole, &jit);

    const GPRReg doubled = relativeShape;
    const GPRReg infinityDoubled = capacity;
    holdsFractions.link(&jit);
    if (!isContiguous) {
        jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), element);
        Label clearFractions = jit.label();
        jit.sub32(TrustedImm32(1), capacity);
        jit.store64(element, BaseIndex(storage, capacity, CCallHelpers::TimesEight));
        jit.branch32(CCallHelpers::Above, capacity, count).linkTo(clearFractions, &jit);
        jit.branchTest32(CCallHelpers::Zero, count).linkTo(isFilledHere, &jit);
        jit.move(TrustedImm64(static_cast<int64_t>(PureDoubleBits::infinityDoubled)), infinityDoubled);
        jit.move(TrustedImm32(0), position);
        Label copyFraction = jit.label();
        jit.load64(BaseIndex(source, position, CCallHelpers::TimesEight), element);
        Jump isHoleAmongFractions;
        PureDoubleBits::branchIfNaN(jit, element, doubled, infinityDoubled, isHoleAmongFractions);
        jit.store64(element, BaseIndex(storage, position, CCallHelpers::TimesEight));
        jit.add32(TrustedImm32(1), position);
        jit.branch32(CCallHelpers::Below, position, count).linkTo(copyFraction, &jit);
        jit.move(TrustedImm32(DoubleShape - Int32Shape), relativeShape);
        jit.jump().linkTo(isFilledHere, &jit);

        isHoleAmongFractions.link(&jit);
        jit.load32(Address(storage, -static_cast<int32_t>(sizeof(uint32_t))), capacity);
    }
    Label clearValues = jit.label();
    jit.sub32(TrustedImm32(1), capacity);
    jit.store64(TrustedImm32(0), BaseIndex(storage, capacity, CCallHelpers::TimesEight));
    jit.branch32(CCallHelpers::Above, capacity, count).linkTo(clearValues, &jit);
    jit.move(TrustedImm32(ContiguousShape - Int32Shape), relativeShape);
    jit.branchTest32(CCallHelpers::Zero, count).linkTo(isFilledHere, &jit);
    jit.move(TrustedImm64(static_cast<int64_t>(PureDoubleBits::infinityDoubled)), infinityDoubled);
    jit.move(TrustedImm32(0), position);
    Label boxFraction = jit.label();
    jit.load64(BaseIndex(source, position, CCallHelpers::TimesEight), element);
    Jump isHoleToBox;
    boxDoubleBits(jit, PureDoubleBits::branchIfNaN(jit, element, doubled, infinityDoubled, isHoleToBox));
    Label behindHoleToBox = jit.label();
    jit.store64(element, BaseIndex(storage, position, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), position);
    jit.branch32(CCallHelpers::Below, position, count).linkTo(boxFraction, &jit);
    jit.move(TrustedImm32(ContiguousShape - Int32Shape), relativeShape);
    jit.jump().linkTo(isFilledHere, &jit);

    isHoleToBox.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), element);
    jit.jump().linkTo(behindHoleToBox, &jit);

    isNoArray.link(&jit);
    static_assert(Int8ArrayType < Float64ArrayType && Float64ArrayType < BigInt64ArrayType);
    jit.load8(Address(iterable, JSCell::typeInfoTypeOffset()), count);
    jit.sub32(TrustedImm32(Int8ArrayType), count);
    Jump isView = jit.branch32(CCallHelpers::BelowOrEqual, count, TrustedImm32(Float64ArrayType - Int8ArrayType));
    isNotServed.link(&jit);
    jit.move(TrustedImm32(0), array);
    jit.returnToCaller();

    auto callOperationWithKind = [&](Entry operation) {
        jit.lshift32(TrustedImm32(1), capacity);
        if (isContiguous)
            jit.or32(TrustedImm32(1), capacity);
        jit.callOperationAndReturn(operation, {
            .returns = OperationCall::Returns::Value,
            .saves = OperationCall::Saves::CallerSavedRegisters,
            .checksException = false,
            .secondArgumentIn = iterable,
            .thirdArgumentIn = capacity,
        });
    };
    callsOperation.link(&jit);
    callOperationWithKind(Entry::operationAOTNewArrayOfList);
    isView.link(&jit);
    callOperationWithKind(Entry::operationAOTNewArrayOfView);
}

void emitEmptyArrayOfHoles(StubAssembler& jit, EmptyArray form)
{
    switch (form) {
    case EmptyArray::DoubleCapacity1:
        return emitArrayOfHoles(jit, ArrayWithDouble, HoleArrayOperand::None, 1);
    case EmptyArray::DoubleCapacity3:
        return emitArrayOfHoles(jit, ArrayWithDouble, HoleArrayOperand::None, 3);
    case EmptyArray::DoubleDefaultCapacity:
        return emitArrayOfHoles(jit, ArrayWithDouble, HoleArrayOperand::None, arrayLiteralVectorLength(0));
    case EmptyArray::Int32DefaultCapacity:
        return emitArrayOfHoles(jit, ArrayWithInt32, HoleArrayOperand::None, arrayLiteralVectorLength(0));
    case EmptyArray::Int32Capacity1:
        return emitArrayOfHoles(jit, ArrayWithInt32, HoleArrayOperand::None, 1);
    case EmptyArray::Int32Capacity3:
        return emitArrayOfHoles(jit, ArrayWithInt32, HoleArrayOperand::None, 3);
    case EmptyArray::ContiguousCapacity1:
    case EmptyArray::ContiguousCapacity3:
    case EmptyArray::ContiguousCapacity5:
    case EmptyArray::ContiguousCapacity7:
    case EmptyArray::ContiguousCapacity9:
    case EmptyArray::ContiguousCapacity13:
    case EmptyArray::ContiguousCapacity17:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubSignature signatureOfNewEmptyArrayWithCapacity(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T9) }); }
StubSignature signatureOfNewArrayWithLength(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T9) }); }
StubSignature signatureOfNewEmptyArrayAtSite(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::T9) });
#if CPU(X86_64)
    return signature.fillingKeptRegisterInFront(X86Registers::edx);
#else
    return signature;
#endif
}

void generateNewEmptyArrayWithCapacity(StubAssembler& jit)
{
    static_assert(arrayLiteralVectorLength(0) == 3);
    const GPRReg requestedCapacity = jit.operand(0);
    jit.linkToStub(jit.branch64(CCallHelpers::BelowOrEqual, requestedCapacity, TrustedImm32(1)), newEmptyArray(EmptyArray::ContiguousCapacity1));
    jit.linkToStub(jit.branch64(CCallHelpers::BelowOrEqual, requestedCapacity, TrustedImm32(arrayLiteralVectorLength(0))), arrayLiteral(0));
    jit.linkToStub(jit.branch64(CCallHelpers::BelowOrEqual, requestedCapacity, TrustedImm32(5)), newEmptyArray(EmptyArray::ContiguousCapacity5));
    jit.linkToStub(jit.branch64(CCallHelpers::Above, requestedCapacity, TrustedImm32((1 << (32 - holeArrayCapacityShift)) - 1)), arrayLiteral(0));
    jit.linkToStub(branchIfHavingABadTime(jit), arrayLiteral(0));
    emitArrayOfHoles(jit, ArrayWithContiguous, HoleArrayOperand::Capacity);
}

void generateNewArrayWithLength(StubAssembler& jit)
{
    emitArrayOfHoles(jit, indexingTypeOf(static_cast<ArrayElementKind>(jit.key().form())), HoleArrayOperand::LengthAndCapacity);
}

StubSignature signatureOfNewArrayOfLength(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T9) }); }

void generateNewArrayOfLength(StubAssembler& jit)
{
    emitArrayOfHoles(jit, indexingTypeOf(static_cast<ArrayElementKind>(jit.key().form())), HoleArrayOperand::Length);
}

StubSignature signatureOfNewArrayOfGiven(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T9) }); }

void generateNewArrayOfGiven(StubAssembler& jit)
{
    const GPRReg given = jit.clobberOperand(0);
    RELEASE_ASSERT(signatureOfNewArrayOfLength(newArrayOfLength(ArrayElementKind::Int32)).operands[0] == Reg(given));
    Jump isNoInt32 = jit.branchIfNotInt32(given);
    jit.signExtend32ToPtr(given, given);
    jit.jumpToStub(newArrayOfLength(ArrayElementKind::Int32));

    isNoInt32.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNewArrayOfGiven, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = given,
    });
}


static constexpr IndexingType indexingShapeOf(ArrayElementKind kind)
{
    return indexingTypeOf(kind) & IndexingShapeMask;
}

static void emitReleasedArrayReuse(StubAssembler& jit, ArrayElementKind kind, GPRReg capacity, GPRReg butterfly)
{
    const GPRReg array = jit.result();
    const GPRReg scratch = jit.reuseScratch(StubRegister::T13);
    const Address releasedArray(instanceGPR, AOT_INSTANCE_OFFSET(arrays.releasedArray));
    RELEASE_ASSERT(noOverlap(array, capacity, butterfly, scratch));

    jit.load64(releasedArray, array);
    Jump hasReleasedArray = jit.branchTest64(CCallHelpers::NonZero, array);
    Label isNotServed = jit.label();
    jit.addOutOfLinePart([=](StubAssembler& jit) {
        constexpr int32_t extraCapacity = 16;
        hasReleasedArray.link(&jit);
        jit.load64(Address(array), scratch);
        jit.branch64(CCallHelpers::NotEqual, scratch, Address(instanceGPR, Instance::offsetOfNewArrayHeader(indexingTypeOf(kind)))).linkTo(isNotServed, &jit);
        jit.loadPtr(Address(array, JSObject::butterflyOffset()), butterfly);
        jit.load32(Address(butterfly, Butterfly::offsetOfVectorLength()), scratch);
        jit.sub32(capacity, scratch);
        jit.add32(TrustedImm32(extraCapacity), capacity);
        Jump doesNotFit = jit.branch32(CCallHelpers::Above, scratch, capacity);
        jit.move(TrustedImm64(0), scratch);
        jit.store64(scratch, releasedArray);
        jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), capacity);
        Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, capacity);
        jit.store32(TrustedImm32(0), Address(butterfly, Butterfly::offsetOfPublicLength()));
        if (kind == ArrayElementKind::Double)
            jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), scratch);
        constexpr int32_t sizeOfElement = sizeof(EncodedJSValue);
        Jump hasPairsAlone = jit.branchTest32(CCallHelpers::Zero, capacity, TrustedImm32(1));
        jit.store64(scratch, BaseIndex(butterfly, capacity, CCallHelpers::TimesEight, -sizeOfElement));
        jit.sub32(TrustedImm32(1), capacity);
        Jump hadOneElement = jit.branchTest32(CCallHelpers::Zero, capacity);
        hasPairsAlone.link(&jit);
#if CPU(ARM64)
        const GPRReg end = capacity;
        jit.getEffectiveAddress(BaseIndex(butterfly, capacity, CCallHelpers::TimesEight), end);
        Label clearPair = jit.label();
        jit.storePair64(scratch, scratch, CCallHelpers::PreIndexAddress(end, -2 * sizeOfElement));
        jit.branchPtr(CCallHelpers::Above, end, butterfly).linkTo(clearPair, &jit);
#else
        Label clearPair = jit.label();
        jit.store64(scratch, BaseIndex(butterfly, capacity, CCallHelpers::TimesEight, -sizeOfElement));
        jit.store64(scratch, BaseIndex(butterfly, capacity, CCallHelpers::TimesEight, -2 * sizeOfElement));
        jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(2), capacity).linkTo(clearPair, &jit);
#endif
        hadOneElement.link(&jit);
        isEmpty.link(&jit);
        fenceAfterAllocation(jit);
        jit.returnToCaller();

        doesNotFit.link(&jit);
        jit.sub32(TrustedImm32(extraCapacity), capacity);
        jit.jump().linkTo(isNotServed, &jit);
    });
}

static void emitJumpToEmptyContiguousArrayStub(StubAssembler& jit, bool kindIsWritten, GPRReg capacity, GPRReg scratch)
{
    emitReleasedArrayReuse(jit, ArrayElementKind::Contiguous, capacity, scratch);
    if (!kindIsWritten) {
        jit.linkToStub(jit.branchTest32(CCallHelpers::Zero, capacity), arrayLiteral(0));
        jit.linkToStub(jit.branch32(CCallHelpers::BelowOrEqual, capacity, TrustedImm32(3)), newEmptyArrayWithCapacity());
    }
    jit.linkToStub(jit.branch32(CCallHelpers::BelowOrEqual, capacity, TrustedImm32(arrayLiteralVectorLength(0))), arrayLiteral(0));
    jit.linkToStub(jit.branch32(CCallHelpers::BelowOrEqual, capacity, TrustedImm32(5)), newEmptyArray(EmptyArray::ContiguousCapacity5));
    jit.linkToStub(jit.branch32(CCallHelpers::BelowOrEqual, capacity, TrustedImm32(7)), newEmptyArray(EmptyArray::ContiguousCapacity7));
    jit.linkToStub(jit.branch32(CCallHelpers::BelowOrEqual, capacity, TrustedImm32(9)), newEmptyArray(EmptyArray::ContiguousCapacity9));
    jit.linkToStub(jit.branch32(CCallHelpers::BelowOrEqual, capacity, TrustedImm32(13)), newEmptyArray(EmptyArray::ContiguousCapacity13));
    jit.linkToStub(jit.branch32(CCallHelpers::BelowOrEqual, capacity, TrustedImm32(17)), newEmptyArray(EmptyArray::ContiguousCapacity17));
    jit.jumpToStub(newEmptyArrayWithCapacity());
}

static void emitJumpToEmptyArrayStub(StubAssembler& jit, ArrayElementKind kind, GPRReg shapeAndCapacity, GPRReg capacity)
{
    static_assert(ArrayAllocationProfile::capacityMask == 0xff && (BASE_CONTIGUOUS_VECTOR_LEN_MAX | 1) <= maxHoleArrayCapacity);
    RELEASE_ASSERT(capacity == gprOf(StubRegister::T9) && kind != ArrayElementKind::Contiguous);
    if (kind != ArrayElementKind::Double)
        jit.linkToStub(branchIfHavingABadTime(jit), arrayLiteral(0));
    jit.zeroExtend8To32(shapeAndCapacity, capacity);
    emitReleasedArrayReuse(jit, kind, capacity, shapeAndCapacity);
    Jump hasDefaultCapacity = jit.branchTest32(CCallHelpers::Zero, capacity);
    switch (kind) {
    case ArrayElementKind::Int32:
        jit.linkToStub(hasDefaultCapacity, newEmptyArray(EmptyArray::Int32DefaultCapacity));
        jit.linkToStub(jit.branch32(CCallHelpers::Equal, capacity, TrustedImm32(1)), newEmptyArray(EmptyArray::Int32Capacity1));
        jit.linkToStub(jit.branch32(CCallHelpers::Equal, capacity, TrustedImm32(3)), newEmptyArray(EmptyArray::Int32Capacity3));
        break;
    case ArrayElementKind::Double:
        jit.linkToStub(hasDefaultCapacity, newEmptyArray(EmptyArray::DoubleDefaultCapacity));
        jit.linkToStub(jit.branch32(CCallHelpers::Equal, capacity, TrustedImm32(1)), newEmptyArray(EmptyArray::DoubleCapacity1));
        jit.linkToStub(jit.branch32(CCallHelpers::Equal, capacity, TrustedImm32(3)), newEmptyArray(EmptyArray::DoubleCapacity3));
        jit.linkToStub(branchIfHavingABadTime(jit), arrayLiteral(0));
        break;
    case ArrayElementKind::Contiguous:
        RELEASE_ASSERT_NOT_REACHED();
    }
    jit.or32(TrustedImm32(1), capacity);
    jit.lshift32(TrustedImm32(holeArrayCapacityShift), capacity);
    jit.jumpToStub(newArrayWithLength(kind));
}

static Jump branchIfReviewDue(StubAssembler& jit, Address wordInProfile)
{
    const TrustedImm32 negatedAllocation(-static_cast<int32_t>(ArrayAllocationProfile::oneAllocation));
#if CPU(X86_64)
    jit.add32(negatedAllocation, wordInProfile);
    return Jump(jit.m_assembler.jCC(X86Assembler::ConditionAE));
#else
    Jump isNotDueForReview = jit.branchAdd32(CCallHelpers::Carry, negatedAllocation, wordInProfile);
    Jump isDueForReview = jit.jump();
    isNotDueForReview.link(&jit);
    return isDueForReview;
#endif
}

void generateNewEmptyArrayAtSite(StubAssembler& jit)
{
    static_assert((ArrayAllocationProfile::shapeMask | ArrayAllocationProfile::capacityMask) == 0xffff && ArrayAllocationProfile::shapeShift == 8);
    const ArrayElementKind seed = static_cast<ArrayElementKind>(jit.key().form());
    const GPRReg profile = jit.clobberOperand(0);
    const GPRReg shapeAndCapacity = jit.allocateScratch(StubRegister::T12);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const GPRReg capacity = profile;
    const Address wordInProfile(profile, OBJECT_OFFSETOF(ArrayAllocationProfile, word));

    Jump isDueForReview = branchIfReviewDue(jit, wordInProfile);

    bool kindIsWritten = seed == ArrayElementKind::Contiguous;
    if (kindIsWritten)
        jit.load8(wordInProfile, capacity);
    else
        jit.load16(wordInProfile, shapeAndCapacity);
    for (ArrayElementKind kind : { ArrayElementKind::Int32, ArrayElementKind::Double }) {
        if (indexingShapeOf(kind) < indexingShapeOf(seed))
            continue;
        uint32_t largestOfShape = static_cast<uint32_t>(indexingShapeOf(kind)) << ArrayAllocationProfile::shapeShift | ArrayAllocationProfile::capacityMask;
        Jump hasShape = jit.branch32(CCallHelpers::BelowOrEqual, shapeAndCapacity, TrustedImm32(largestOfShape));
        jit.addOutOfLinePart([=](StubAssembler& jit) {
            hasShape.link(&jit);
            emitJumpToEmptyArrayStub(jit, kind, shapeAndCapacity, capacity);
        });
    }
    if (!kindIsWritten)
        jit.zeroExtend8To32(shapeAndCapacity, capacity);
    emitJumpToEmptyContiguousArrayStub(jit, kindIsWritten, capacity, shapeAndCapacity);

    isDueForReview.link(&jit);
    jit.move(TrustedImm32(indexingTypeOf(seed)), scratch);
    jit.callOperationAndReturn(Entry::operationAOTNewArrayAtReviewedSite, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = profile,
        .thirdArgumentIn = scratch,
    });
}

StubSignature signatureOfNewArrayWithLengthAtSite(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T9), gprOf(StubRegister::T12) }); }

void generateNewArrayWithLengthAtSite(StubAssembler& jit)
{
    static_assert(ArrayAllocationProfile::shapeShift == 8 && ArrayAllocationProfile::shapeMask == 0xff00);
    const ArrayElementKind seed = static_cast<ArrayElementKind>(jit.key().form());
    const GPRReg lengthAndCapacity = jit.clobberOperand(0);
    const GPRReg profile = jit.clobberOperand(1);
    const GPRReg shape = jit.allocateScratch(StubRegister::T11);
    const Address wordInProfile(profile, OBJECT_OFFSETOF(ArrayAllocationProfile, word));
    RELEASE_ASSERT(lengthAndCapacity == gprOf(StubRegister::T9) && noOverlap(lengthAndCapacity, profile, shape));

    Jump isDueForReview = branchIfReviewDue(jit, wordInProfile);

    jit.load16(wordInProfile, shape);
    jit.urshift32(TrustedImm32(ArrayAllocationProfile::shapeShift), shape);
    for (ArrayElementKind kind : { ArrayElementKind::Int32, ArrayElementKind::Double }) {
        if (indexingShapeOf(kind) >= indexingShapeOf(seed))
            jit.linkToStub(jit.branch32(CCallHelpers::BelowOrEqual, shape, TrustedImm32(indexingShapeOf(kind))), newArrayWithLength(kind));
    }
    jit.jumpToStub(newArrayWithLength(ArrayElementKind::Contiguous));

    isDueForReview.link(&jit);
    jit.or32(TrustedImm32(static_cast<int32_t>(indexingTypeOf(seed)) << seedShiftBesideLengthAndCapacity), lengthAndCapacity);
    jit.callOperationAndReturn(Entry::operationAOTNewArrayWithLengthAtReviewedSite, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = profile,
        .thirdArgumentIn = lengthAndCapacity,
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
