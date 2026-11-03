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
static_assert(descriptorOf(StubFamily::NewEmptyArrayAtSite).numberOfForms == static_cast<unsigned>(ArrayElementKind::Contiguous) + 1);
static_assert(std::is_same_v<decltype(&operationAOTNewArrayAtReviewedSite), OperationReturnType<JSCell*> (JIT_OPERATION_ATTRIBUTES *)(Instance*, void*, uint32_t)>);

enum class HoleArrayOperand : uint8_t { None, LengthAndCapacity, Capacity };

static void emitLoadAllocatorForCapacity(StubAssembler& jit, GPRReg capacity, GPRReg scratch, GPRReg allocator)
{
    jit.add32(TrustedImm32(1), capacity, scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    static_assert(sizeof(JSArray) == MarkedSpace::sizeStep);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep() + sizeof(Allocator)), allocator);
}

static void emitArrayOfHoles(StubAssembler& jit, IndexingType indexingType, HoleArrayOperand operand, unsigned fixedCapacity = 0, unsigned fixedLength = 0)
{
    static_assert(sizeOfArrayWithStorage(maxHoleArrayCapacity) <= MarkedSpace::largeCutoff && maxHoleArrayCapacity & 1 && arrayLiteralVectorLength(0) >= 2);
    RELEASE_ASSERT(indexingType == ArrayWithDouble || indexingType == ArrayWithInt32 || indexingType == ArrayWithContiguous);
    RELEASE_ASSERT(operand != HoleArrayOperand::None || fixedCapacity & 1);
    RELEASE_ASSERT(fixedLength <= fixedCapacity);
    const GPRReg array = jit.result();
    // The operand comes where the allocator goes. Without a length, it is 0.
    const auto [allocator, length] = [&]() -> std::pair<GPRReg, GPRReg> {
        switch (operand) {
        case HoleArrayOperand::None:
            return { jit.allocateScratch(StubRegister::T9), InvalidGPRReg };
        case HoleArrayOperand::LengthAndCapacity:
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
    bool hasAllocatorInInstance = operand == HoleArrayOperand::None && ShortArrayWords::indexOfAllocator(fixedCapacity) < ShortArrayWords::numberOfAllocators;
    // The operation makes an array with elements in plain storage even in a realm that is having a bad time.
    RELEASE_ASSERT(!fixedLength || hasAllocatorInInstance);
    switch (operand) {
    case HoleArrayOperand::None:
        if (!hasAllocatorInInstance)
            callerSlowPath.append(branchIfHavingABadTime(jit));
        break;
    case HoleArrayOperand::LengthAndCapacity:
        callerSlowPath.append(branchIfHavingABadTime(jit));
        break;
    case HoleArrayOperand::Capacity:
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
        slowPath.append(jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(maxHoleArrayCapacity)));
        emitLoadAllocatorForCapacity(jit, capacity, scratch, allocator);
        break;
    case HoleArrayOperand::Capacity:
        jit.or32(TrustedImm32(1), allocator, capacity);
        emitLoadAllocatorForCapacity(jit, capacity, scratch, allocator);
        break;
    }
    if (!hasAllocatorInInstance)
        slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    std::optional<unsigned> cellSize;
    if (operand == HoleArrayOperand::None && sizeOfArrayWithStorage(fixedCapacity) <= MarkedSpace::preciseCutoff)
        cellSize = sizeOfArrayWithStorage(fixedCapacity);
    const AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, array, allocator, scratch, slowPath, cellSize);
    constexpr int32_t offsetOfLength = sizeof(JSArray);
    constexpr int32_t offsetOfCapacity = offsetOfLength + sizeof(uint32_t);
    jit.addPtr(TrustedImm32(sizeof(JSArray) + sizeof(IndexingHeader)), array, storage);

    // The indexing header and the holes. The allocator is dead.
    switch (operand) {
    case HoleArrayOperand::None: {
        jit.store64(TrustedImm64(static_cast<uint64_t>(fixedCapacity) << 32 | fixedLength), Address(array, offsetOfLength));
        if (fixedLength == fixedCapacity)
            break;
        jit.move(hole, scratch);
        unsigned i = fixedLength;
        for (; i + 1 < fixedCapacity; i += 2)
            jit.storePair64(scratch, scratch, storage, TrustedImm32(i * sizeof(EncodedJSValue)));
        if (i < fixedCapacity)
            jit.store64(scratch, Address(storage, i * sizeof(EncodedJSValue)));
        break;
    }
    case HoleArrayOperand::LengthAndCapacity: {
        jit.store32(length, Address(array, offsetOfLength));
        jit.store32(capacity, Address(array, offsetOfCapacity));
        // The capacity is odd: one hole at the length, then pairs from the odd position there or behind it.
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
        // The capacity is odd and at least 3: one hole, then pairs.
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
    }
    jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(indexingType)), scratch);
    jit.storePair64(scratch, storage, array, TrustedImm32(0));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowPath);
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
        jit.move(TrustedImm32(fixedCapacity << holeArrayCapacityShift | fixedLength), lengthAndCapacity);
        break;
    case HoleArrayOperand::LengthAndCapacity:
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
    case EmptyArray::ContiguousCapacity1:
    case EmptyArray::ContiguousCapacity3:
    case EmptyArray::ContiguousCapacity1OwnStorage:
    case EmptyArray::ContiguousCapacity3OwnStorage:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubSignature signatureOfNewEmptyArrayWithCapacity(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T9) }); }
StubSignature signatureOfNewArrayWithLength(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T9) }); }
StubSignature signatureOfNewArrayOfLength(StubKey) { return StubSignature::withGPRResult({ }); }
StubSignature signatureOfNewEmptyArrayAtSite(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T9) }); }
StubSignature signatureOfNewArrayFrontEnd(StubKey) { return operationFrontEndSignature(2); }

void generateNewEmptyArrayWithCapacity(StubAssembler& jit)
{
    static_assert(arrayLiteralVectorLength(0) == 5);
    const GPRReg requestedCapacity = jit.operand(0);
    jit.linkToStub(jit.branch64(CCallHelpers::BelowOrEqual, requestedCapacity, TrustedImm32(1)), newEmptyArray(EmptyArray::ContiguousCapacity1));
    jit.linkToStub(jit.branch64(CCallHelpers::BelowOrEqual, requestedCapacity, TrustedImm32(3)), newEmptyArray(EmptyArray::ContiguousCapacity3));
    jit.linkToStub(jit.branch64(CCallHelpers::BelowOrEqual, requestedCapacity, TrustedImm32(arrayLiteralVectorLength(0))), arrayLiteral(0));
    jit.linkToStub(jit.branch64(CCallHelpers::Above, requestedCapacity, TrustedImm32(maxHoleArrayCapacity)), arrayLiteral(0));
    jit.linkToStub(branchIfHavingABadTime(jit), arrayLiteral(0));
    emitArrayOfHoles(jit, ArrayWithContiguous, HoleArrayOperand::Capacity);
}

void generateNewArrayWithLength(StubAssembler& jit)
{
    emitArrayOfHoles(jit, indexingTypeOf(static_cast<ArrayElementKind>(jit.key().form())), HoleArrayOperand::LengthAndCapacity);
}

static_assert(std::is_same_v<decltype(&operationAOTNewArrayOfMembers), OperationReturnType<JSCell*> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, uint64_t)>);

StubSignature signatureOfNewArrayOfMembers(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1) }); }

// The table is walked in one go: nothing runs in between, so nobody is left standing behind an entry.
void generateNewArrayOfMembers(StubAssembler& jit)
{
    const IndexingType made = indexingTypeOf(static_cast<ArrayElementKind>(jit.key().form()));
    const GPRReg set = jit.operand(0);
    const GPRReg array = jit.result();
    const GPRReg capacity = jit.allocateScratch(StubRegister::A2);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T9);
    const GPRReg table = jit.allocateScratch(StubRegister::T10);
    const GPRReg storage = jit.allocateScratch(StubRegister::T11);
    const GPRReg member = jit.allocateScratch(StubRegister::T12);
    const GPRReg used = jit.allocateScratch(StubRegister::T13);
    const GPRReg column = jit.allocateScratch(StubRegister::T14);
    const GPRReg entry = allocator;
    const GPRReg position = table;
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    RELEASE_ASSERT(noOverlap(set, array, capacity, allocator, table, storage, member, used, column));
    const Address flags(table, CompactTable::offsetOfKeysShift());
    const BaseIndex written(storage, position, CCallHelpers::TimesEight);

    JumpList givesUp;
    jit.loadPtr(Address(set, offsetOfHashTableStorage()), table);
    givesUp.append(jit.branchTestPtr(CCallHelpers::Zero, table));
    givesUp.append(jit.branch8(CCallHelpers::Equal, Address(table, JSCell::typeInfoTypeOffset()), TrustedImm32(JSCellButterflyType)));
    givesUp.append(jit.branchTest8(CCallHelpers::NonZero, flags, TrustedImm32(CompactTable::keysAreInImage)));
    // A number never fits a narrow key.
    if (made != ArrayWithContiguous)
        givesUp.append(jit.branchTest8(CCallHelpers::Zero, flags, TrustedImm32(CompactTable::keysAreWide)));
    jit.load32(Address(table, CompactTable::offsetOfAliveCount()), used);
    jit.or32(TrustedImm32(1), used, capacity);
    givesUp.append(jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(maxHoleArrayCapacity)));
    givesUp.append(branchIfHavingABadTime(jit));
    emitLoadAllocatorForCapacity(jit, capacity, member, allocator);
    givesUp.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, array, allocator, member, givesUp);

    jit.store32(used, Address(array, sizeof(JSArray)));
    jit.store32(capacity, Address(array, sizeof(JSArray) + sizeof(uint32_t)));
    jit.addPtr(TrustedImm32(sizeof(JSArray) + sizeof(IndexingHeader)), array, storage);
    // Behind an even number of members one place is left. The last of an odd number is written over the hole.
    jit.sub32(TrustedImm32(1), capacity);
    if (made == ArrayWithDouble) {
        jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), member);
        jit.store64(member, BaseIndex(storage, capacity, CCallHelpers::TimesEight));
    } else
        jit.store64(TrustedImm32(0), BaseIndex(storage, capacity, CCallHelpers::TimesEight));

    jit.load8(flags, member);
    jit.and32(TrustedImm32(CompactTable::keysShiftMask), member);
    jit.move(TrustedImm32(2), column);
    jit.lshift64(member, column);
    jit.add64(table, column);
    jit.addPtr(TrustedImm32(CompactTable::offsetOfIndex()), column);
    jit.load32(Address(table, CompactTable::offsetOfUsed()), used);
    jit.move(TrustedImm32(0), entry);

    auto finish = [&] {
        jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(made)), member);
        jit.storePair64(member, storage, array, TrustedImm32(0));
        fenceAfterAllocation(jit);
        jit.returnToCaller();
    };
    auto forEachMember = [&](bool keysAreWide, NOESCAPE const Invocable<void(BaseIndex)> auto& store) {
        auto load = [&] {
            if (keysAreWide)
                jit.load64(BaseIndex(column, entry, CCallHelpers::TimesEight), member);
            else
                jit.load32(BaseIndex(column, entry, CCallHelpers::TimesFour), member);
        };
        Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, used);
        Jump hasDeletedEntries = jit.branch32(CCallHelpers::NotEqual, used, Address(table, CompactTable::offsetOfAliveCount()));
        // Each member goes where its entry stands.
        Label copy = jit.label();
        load();
        store(BaseIndex(storage, entry, CCallHelpers::TimesEight));
        jit.add32(TrustedImm32(1), entry);
        jit.branch32(CCallHelpers::Below, entry, used).linkTo(copy, &jit);
        isEmpty.link(&jit);
        finish();

        // An entry whose key is 0 was deleted.
        hasDeletedEntries.link(&jit);
        jit.move(TrustedImm32(0), position);
        Label loop = jit.label();
        load();
        jit.add32(TrustedImm32(1), entry);
        Jump isDeleted = jit.branchTest64(CCallHelpers::Zero, member);
        store(written);
        jit.add32(TrustedImm32(1), position);
        isDeleted.link(&jit);
        jit.branch32(CCallHelpers::Below, entry, used).linkTo(loop, &jit);
        finish();
    };

    JumpList findsOtherKind;
    switch (made) {
    case ArrayWithInt32:
        forEachMember(true, [&](BaseIndex place) {
            findsOtherKind.append(jit.branchIfNotInt32(member));
            jit.store64(member, place);
        });
        break;
    case ArrayWithDouble:
        forEachMember(true, [&](BaseIndex place) {
            Jump isInt32 = jit.branchIfInt32(member);
            findsOtherKind.append(jit.branchIfNotNumber(member));
            unboxDouble(jit, member, capacity, number);
            // It would read as a hole.
            findsOtherKind.append(jit.branchIfNaN(number));
            Jump isDouble = jit.jump();
            isInt32.link(&jit);
            jit.convertInt32ToDouble(member, number);
            isDouble.link(&jit);
            jit.storeDouble(number, place);
        });
        break;
    case ArrayWithContiguous: {
        Jump keysAreWide = jit.branchTest8(CCallHelpers::NonZero, flags, TrustedImm32(CompactTable::keysAreWide));
        forEachMember(false, [&](BaseIndex place) {
            Jump isImmediate = jit.branch32(CCallHelpers::Below, member, TrustedImm32(minCell32));
            decodeCell32(jit, member, member);
            isImmediate.link(&jit);
            jit.store64(member, place);
        });
        keysAreWide.link(&jit);
        forEachMember(true, [&](BaseIndex place) {
            jit.store64(member, place);
        });
        break;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The collector may find the array that is given up on the stack.
    if (!findsOtherKind.empty()) {
        findsOtherKind.link(&jit);
        jit.store32(TrustedImm32(0), Address(array, sizeof(JSArray)));
        jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(made)), member);
        jit.storePair64(member, storage, array, TrustedImm32(0));
        fenceAfterAllocation(jit);
    }
    givesUp.link(&jit);
    jit.move(TrustedImm32(made), capacity);
    jit.callOperationAndReturn(Entry::operationAOTNewArrayOfMembers, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters });
}

static_assert(std::is_same_v<decltype(&operationAOTNewArrayOfElements), OperationReturnType<JSCell*> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, uint64_t)>);
static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2));

StubSignature signatureOfNewArrayOfElements(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }

void generateNewArrayOfElements(StubAssembler& jit)
{
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    const IndexingType made = indexingTypeOf(static_cast<ArrayElementKind>(jit.key().form()));
    const GPRReg source = jit.operand(0);
    const GPRReg capacity = jit.clobberOperand(1);
    const GPRReg array = jit.result();
    const GPRReg allocator = jit.allocateScratch(StubRegister::T9);
    const GPRReg elements = jit.allocateScratch(StubRegister::T10);
    const GPRReg storage = jit.allocateScratch(StubRegister::T11);
    const GPRReg element = jit.allocateScratch(StubRegister::T12);
    const GPRReg count = jit.allocateScratch(StubRegister::T13);
    const GPRReg kind = jit.allocateScratch(StubRegister::T14);
    const GPRReg position = allocator;
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    RELEASE_ASSERT(noOverlap(source, capacity, array, allocator, elements, storage, element, count, kind));
    const BaseIndex read(elements, position, CCallHelpers::TimesEight);
    const BaseIndex written(storage, position, CCallHelpers::TimesEight);

    JumpList givesUp;
    jit.load8(Address(source, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeMask), kind);
    jit.sub32(TrustedImm32(Int32Shape), kind);
    givesUp.append(jit.branch32(CCallHelpers::Above, kind, TrustedImm32((made & IndexingShapeMask) - Int32Shape)));
    givesUp.append(jit.branch64(CCallHelpers::Above, capacity, TrustedImm32(maxHoleArrayCapacity)));
    jit.or32(TrustedImm32(1), capacity);
    jit.loadPtr(Address(source, JSObject::butterflyOffset()), elements);
    jit.load32(Address(elements, Butterfly::offsetOfPublicLength()), count);
    givesUp.append(jit.branch32(CCallHelpers::Above, count, capacity));
    givesUp.append(branchIfHavingABadTime(jit));
    emitLoadAllocatorForCapacity(jit, capacity, element, allocator);
    givesUp.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, array, allocator, element, givesUp);

    jit.store32(count, Address(array, sizeof(JSArray)));
    jit.store32(capacity, Address(array, sizeof(JSArray) + sizeof(uint32_t)));
    jit.addPtr(TrustedImm32(sizeof(JSArray) + sizeof(IndexingHeader)), array, storage);
    jit.move(count, position);
    Jump isFull = jit.branch32(CCallHelpers::AboveOrEqual, position, capacity);
    if (made == ArrayWithDouble)
        jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), element);
    Label clear = jit.label();
    if (made == ArrayWithDouble)
        jit.store64(element, written);
    else
        jit.store64(TrustedImm32(0), written);
    jit.add32(TrustedImm32(1), position);
    jit.branch32(CCallHelpers::Below, position, capacity).linkTo(clear, &jit);
    isFull.link(&jit);

    auto finish = [&] {
        jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(made)), element);
        jit.storePair64(element, storage, array, TrustedImm32(0));
        fenceAfterAllocation(jit);
        jit.returnToCaller();
    };
    JumpList findsHole;
    auto giveUp = [&] {
        if (!findsHole.empty()) {
            findsHole.link(&jit);
            jit.store32(TrustedImm32(0), Address(array, sizeof(JSArray)));
            jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(made)), element);
            jit.storePair64(element, storage, array, TrustedImm32(0));
            fenceAfterAllocation(jit);
        }
        givesUp.link(&jit);
        jit.or64(TrustedImm64(static_cast<int64_t>(made) << 32), capacity);
        jit.callOperationAndReturn(Entry::operationAOTNewArrayOfElements, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters });
    };

    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, count);
    jit.move(TrustedImm32(0), position);
    if (made == ArrayWithInt32) {
        Label copy = jit.label();
        jit.load64(read, element);
        findsHole.append(jit.branchTest64(CCallHelpers::Zero, element));
        jit.store64(element, written);
        jit.add32(TrustedImm32(1), position);
        jit.branch32(CCallHelpers::Below, position, count).linkTo(copy, &jit);
        isEmpty.link(&jit);
        finish();
        giveUp();
        return;
    }
    if (made == ArrayWithDouble) {
        Jump holdsIntegers = jit.branchTest32(CCallHelpers::Zero, kind);
        Label copy = jit.label();
        jit.loadDouble(read, number);
        findsHole.append(jit.branchDouble(CCallHelpers::DoubleNotEqualOrUnordered, number, number));
        jit.storeDouble(number, written);
        jit.add32(TrustedImm32(1), position);
        jit.branch32(CCallHelpers::Below, position, count).linkTo(copy, &jit);
        isEmpty.link(&jit);
        Label isFilled = jit.label();
        finish();
        holdsIntegers.link(&jit);
        Label convert = jit.label();
        jit.load64(read, element);
        findsHole.append(jit.branchTest64(CCallHelpers::Zero, element));
        jit.convertInt32ToDouble(element, number);
        jit.storeDouble(number, written);
        jit.add32(TrustedImm32(1), position);
        jit.branch32(CCallHelpers::Below, position, count).linkTo(convert, &jit);
        jit.jump().linkTo(isFilled, &jit);
        giveUp();
        return;
    }
    Jump holdsDoubles = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(DoubleShape - Int32Shape));
    Label copy = jit.label();
    jit.load64(read, element);
    Jump isNoHole = jit.branchTest64(CCallHelpers::NonZero, element);
    jit.move(TrustedImm64(JSValue::ValueUndefined), element);
    isNoHole.link(&jit);
    jit.store64(element, written);
    jit.add32(TrustedImm32(1), position);
    jit.branch32(CCallHelpers::Below, position, count).linkTo(copy, &jit);

    isEmpty.link(&jit);
    Label isFilled = jit.label();
    finish();

    holdsDoubles.link(&jit);
    jit.move(count, position);
    Label box = jit.label();
#if CPU(X86_64)
    jit.loadDouble(BaseIndex(elements, position, CCallHelpers::TimesEight, -static_cast<int32_t>(sizeof(double))), number);
#else
    jit.sub32(TrustedImm32(1), position);
    jit.loadDouble(read, number);
#endif
    Jump isHole;
    PureDouble pure = PureDouble::branchIfNaN(jit, number, isHole);
    boxDouble(jit, pure, element);
    Label store = jit.label();
#if CPU(X86_64)
    jit.store64(element, BaseIndex(storage, position, CCallHelpers::TimesEight, -static_cast<int32_t>(sizeof(EncodedJSValue))));
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(1), position).linkTo(box, &jit);
#else
    jit.store64(element, written);
    jit.branchTest32(CCallHelpers::NonZero, position).linkTo(box, &jit);
#endif
    jit.jump().linkTo(isFilled, &jit);
    isHole.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), element);
    jit.jump().linkTo(store, &jit);
    giveUp();
}

void generateNewArrayOfLength(StubAssembler& jit)
{
    static_assert(ShortArrayWords::indexOfAllocator(arrayLiteralVectorLength(maxArrayLengthInStubKey)) < ShortArrayWords::numberOfAllocators);
    const unsigned length = jit.key().count();
    emitArrayOfHoles(jit, indexingTypeOf(static_cast<ArrayElementKind>(jit.key().form())), HoleArrayOperand::None, arrayLiteralVectorLength(length), length);
}

// The operands are those of the operation: the count and where the values are.
void generateNewArrayFrontEnd(StubAssembler& jit)
{
    static_assert(arrayLiteralVectorLength(2) == 3 && arrayLiteralVectorLength(3) == 3 && maxHoleArrayCapacity & 1);
    static_assert(maxHoleArrayCapacity < storageOfItsOwnInArrayLiteralCount && storageOfItsOwnInArrayLiteralCount < 1u << capacityShiftInArrayLiteralCount);
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
    // A hole behind an even number of values.
    jit.store64(TrustedImm32(0), BaseIndex(storage, capacity, CCallHelpers::TimesEight, -static_cast<int32_t>(sizeof(EncodedJSValue))));
    jit.move(TrustedImm32(0), position);
    Label copy = jit.label();
    jit.load64(BaseIndex(values, position, CCallHelpers::TimesEight), scratch);
    jit.store64(scratch, BaseIndex(storage, position, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), position);
    jit.branch32(CCallHelpers::Below, position, count).linkTo(copy, &jit);
    jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(ArrayWithContiguous)), scratch);
    jit.storePair64(scratch, storage, array, TrustedImm32(0));
    fenceAfterAllocation(jit);
    jit.move(TrustedImm32(0), jit.secondResult());
    jit.returnToCaller();

    callsOperation.link(&jit);
    jit.tailCallOperation(Entry::RawNewArray, OperationCall::FirstArgument::Instance);
}

static constexpr IndexingType indexingShapeOf(ArrayElementKind kind)
{
    return indexingTypeOf(kind) & IndexingShapeMask;
}

// No stub entered from here returns 0: one that would in a realm that is having a bad time is entered behind the test for that.
static void emitJumpToEmptyArrayStub(StubAssembler& jit, ArrayElementKind kind, GPRReg shapeAndCapacity, GPRReg capacity)
{
    static_assert(ArrayAllocationProfile::capacityMask == 0xff && (BASE_CONTIGUOUS_VECTOR_LEN_MAX | 1) <= maxHoleArrayCapacity);
    RELEASE_ASSERT(capacity == gprOf(StubRegister::T9));
    if (kind != ArrayElementKind::Double)
        jit.linkToStub(branchIfHavingABadTime(jit), arrayLiteral(0));
    jit.zeroExtend8To32(shapeAndCapacity, capacity);
    Jump hasDefaultCapacity = jit.branchTest32(CCallHelpers::Zero, capacity);
    switch (kind) {
    case ArrayElementKind::Int32:
        jit.linkToStub(hasDefaultCapacity, newEmptyArray(EmptyArray::Int32DefaultCapacity));
        break;
    case ArrayElementKind::Double:
        jit.linkToStub(hasDefaultCapacity, newEmptyArray(EmptyArray::DoubleDefaultCapacity));
        jit.linkToStub(jit.branch32(CCallHelpers::Equal, capacity, TrustedImm32(1)), newEmptyArray(EmptyArray::DoubleCapacity1));
        jit.linkToStub(jit.branch32(CCallHelpers::Equal, capacity, TrustedImm32(3)), newEmptyArray(EmptyArray::DoubleCapacity3));
        jit.linkToStub(branchIfHavingABadTime(jit), arrayLiteral(0));
        break;
    case ArrayElementKind::Contiguous:
        jit.linkToStub(hasDefaultCapacity, arrayLiteral(0));
        jit.jumpToStub(newEmptyArrayWithCapacity());
        return;
    }
    jit.or32(TrustedImm32(1), capacity);
    jit.lshift32(TrustedImm32(holeArrayCapacityShift), capacity);
    jit.jumpToStub(newArrayWithLength(kind));
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

    // Adding the negative carries unless the count was 0.
    Jump isNotDueForReview = jit.branchAdd32(CCallHelpers::Carry, TrustedImm32(-static_cast<int32_t>(ArrayAllocationProfile::oneAllocation)), wordInProfile);
    Jump isDueForReview = jit.jump();
    isNotDueForReview.link(&jit);

    jit.load16(wordInProfile, shapeAndCapacity);
    for (ArrayElementKind kind : { ArrayElementKind::Int32, ArrayElementKind::Double }) {
        if (indexingShapeOf(kind) < indexingShapeOf(seed))
            continue;
        uint32_t largestOfShape = static_cast<uint32_t>(indexingShapeOf(kind)) << ArrayAllocationProfile::shapeShift | ArrayAllocationProfile::capacityMask;
        Jump needsMoreGeneralShape = jit.branch32(CCallHelpers::Above, shapeAndCapacity, TrustedImm32(largestOfShape));
        emitJumpToEmptyArrayStub(jit, kind, shapeAndCapacity, capacity);
        needsMoreGeneralShape.link(&jit);
    }
    emitJumpToEmptyArrayStub(jit, ArrayElementKind::Contiguous, shapeAndCapacity, capacity);

    isDueForReview.link(&jit);
    jit.move(TrustedImm32(indexingTypeOf(seed)), scratch);
    jit.callOperationAndReturn(Entry::operationAOTNewArrayAtReviewedSite, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = profile,
        .thirdArgumentIn = scratch,
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
