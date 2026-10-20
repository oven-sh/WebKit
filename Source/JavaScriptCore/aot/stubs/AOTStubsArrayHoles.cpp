/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArrays.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "ArrayConventions.h"
#include "Butterfly.h"
#include "CompleteSubspace.h"
#include "IndexingHeader.h"
#include "IndexingType.h"
#include "JSCJSValueInlines.h"
#include "JSObject.h"
#include "MarkedSpace.h"
#include "OperationResult.h"
#include "PureNaN.h"
#include <bit>
#include <initializer_list>
#include <optional>
#include <type_traits>

namespace JSC { namespace AOT {

static_assert(JSObject::butterflyOffset() == sizeof(JSCell) && sizeof(JSCell) == sizeof(uint64_t));
static_assert(sizeof(IndexingHeader) == sizeof(EncodedJSValue) && MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
static_assert(Butterfly::offsetOfPublicLength() == -static_cast<ptrdiff_t>(sizeof(IndexingHeader)) && Butterfly::offsetOfVectorLength() == Butterfly::offsetOfPublicLength() + static_cast<ptrdiff_t>(sizeof(uint32_t)));
static_assert(descriptorOf(StubFamily::NewArrayWithLength).numberOfForms == static_cast<unsigned>(ArrayElementKind::Contiguous) + 1);
static_assert(descriptorOf(StubFamily::NewEmptyArrayAtSite).numberOfForms == static_cast<unsigned>(ArrayElementKind::Contiguous) + 1);
static_assert(std::is_same_v<decltype(&operationAOTNewArrayAtReviewedSite), OperationReturnType<JSCell*> (JIT_OPERATION_ATTRIBUTES *)(Instance*, void*, uint32_t)>);

#if CPU(ARM64)
static constexpr StubRegister allocationScratchRegister = StubRegister::T11;
static constexpr StubRegister storageRegister = StubRegister::T12;
#elif CPU(X86_64)
static constexpr StubRegister allocationScratchRegister = StubRegister::A0;
static constexpr StubRegister storageRegister = StubRegister::A5;
#endif

enum class HoleArrayOperand : uint8_t { None, LengthAndCapacity, Capacity };

struct HoleArrayShape {
    IndexingType indexingType;
    HoleArrayOperand operand;
    unsigned fixedCapacity { 0 };
};

struct HoleArrayRegisters {
    GPRReg array;
    GPRReg allocator;
    GPRReg scratch;
    GPRReg storage;
    GPRReg capacity;
    // Without one, the length is 0.
    std::optional<GPRReg> length;
};

static HoleArrayRegisters allocateHoleArrayRegisters(StubAssembler& jit, HoleArrayOperand operand)
{
    auto allocate = [&](GPRReg allocator, std::optional<GPRReg> length) {
        return HoleArrayRegisters {
            .array = jit.result(),
            .allocator = allocator,
            .scratch = jit.allocateScratch(allocationScratchRegister),
            .storage = jit.allocateScratch(storageRegister),
            .capacity = jit.allocateScratch(StubRegister::T13),
            .length = length,
        };
    };
    switch (operand) {
    case HoleArrayOperand::None:
        return allocate(jit.allocateScratch(StubRegister::T9), std::nullopt);
    case HoleArrayOperand::LengthAndCapacity:
        return allocate(jit.clobberOperand(0), jit.allocateScratch(StubRegister::T14));
    case HoleArrayOperand::Capacity:
        return allocate(jit.clobberOperand(0), std::nullopt);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitLoadAllocatorForCapacity(StubAssembler& jit, const HoleArrayRegisters& registers)
{
    const auto [array, allocator, scratch, storage, capacity, length] = registers;
    jit.add32(TrustedImm32(1), capacity, scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
}

static void emitChooseStorageAllocator(StubAssembler& jit, const HoleArrayShape& shape, const HoleArrayRegisters& registers, JumpList& slowPath)
{
    const auto [array, allocator, scratch, storage, capacity, length] = registers;
    switch (shape.operand) {
    case HoleArrayOperand::None:
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), allocator);
        jit.loadPtr(Address(allocator, offsetOfArrayStorageAllocator(shape.fixedCapacity)), allocator);
        break;
    case HoleArrayOperand::LengthAndCapacity: {
        const GPRReg lengthAndCapacity = allocator;
        jit.and32(TrustedImm32((1 << holeArrayCapacityShift) - 1), lengthAndCapacity, *length);
        jit.urshift32(lengthAndCapacity, TrustedImm32(holeArrayCapacityShift), capacity);
        slowPath.append(jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(maxHoleArrayCapacity)));
        emitLoadAllocatorForCapacity(jit, registers);
        break;
    }
    case HoleArrayOperand::Capacity: {
        const GPRReg requestedCapacity = allocator;
        jit.or32(TrustedImm32(1), requestedCapacity, capacity);
        emitLoadAllocatorForCapacity(jit, registers);
        break;
    }
    }
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
}

static void emitStoreIndexingHeader(StubAssembler& jit, const HoleArrayRegisters& registers)
{
    const auto [array, allocator, scratch, storage, capacity, length] = registers;
    if (length)
        jit.store32(*length, Address(storage, 0));
    else
        jit.store32(TrustedImm32(0), Address(storage, 0));
    jit.store32(capacity, Address(storage, sizeof(uint32_t)));
}

// The hole is in scratch, and the capacity is odd and at least 3.
static void emitFillByPairsAfterFirst(StubAssembler& jit, const HoleArrayRegisters& registers)
{
    const auto [array, allocator, scratch, storage, capacity, length] = registers;
    const GPRReg cursor = allocator;
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
}

// Writes the indexing header and the holes, and turns storage into the butterfly. The allocator is dead.
static void emitFillWithHoles(StubAssembler& jit, const HoleArrayShape& shape, const HoleArrayRegisters& registers)
{
    static_assert(arrayLiteralVectorLength(0) >= 2);
    const auto [array, allocator, scratch, storage, capacity, length] = registers;
    TrustedImm64 hole(shape.indexingType == ArrayWithDouble ? std::bit_cast<int64_t>(PNaN) : JSValue::encode(JSValue()));
    switch (shape.operand) {
    case HoleArrayOperand::None:
        jit.store64(TrustedImm64(static_cast<uint64_t>(shape.fixedCapacity) << 32), Address(storage, 0));
        jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
        jit.move(hole, scratch);
        for (unsigned i = 0; i + 1 < shape.fixedCapacity; i += 2)
            jit.storePair64(scratch, scratch, storage, TrustedImm32(i * sizeof(EncodedJSValue)));
        jit.store64(scratch, Address(storage, (shape.fixedCapacity - 1) * sizeof(EncodedJSValue)));
        return;
    case HoleArrayOperand::LengthAndCapacity: {
        emitStoreIndexingHeader(jit, registers);
        jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
        Jump isFull = jit.branch32(CCallHelpers::AboveOrEqual, *length, capacity);
        jit.move(hole, scratch);
        Label fill = jit.label();
        jit.store64(scratch, BaseIndex(storage, *length, CCallHelpers::TimesEight));
        jit.add32(TrustedImm32(1), *length);
        jit.branch32(CCallHelpers::Below, *length, capacity).linkTo(fill, &jit);
        isFull.link(&jit);
        return;
    }
    case HoleArrayOperand::Capacity:
        emitStoreIndexingHeader(jit, registers);
        jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
        jit.move(hole, scratch);
        jit.store64(scratch, Address(storage));
        emitFillByPairsAfterFirst(jit, registers);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitCallNewArrayOfHoles(StubAssembler& jit, const HoleArrayShape& shape, const HoleArrayRegisters& registers)
{
    const GPRReg lengthAndCapacity = registers.storage;
    const GPRReg flags = registers.capacity;
    switch (shape.operand) {
    case HoleArrayOperand::None:
        jit.move(TrustedImm32(shape.fixedCapacity << holeArrayCapacityShift), lengthAndCapacity);
        break;
    case HoleArrayOperand::LengthAndCapacity:
        jit.lshift32(registers.capacity, TrustedImm32(holeArrayCapacityShift), lengthAndCapacity);
        jit.or32(*registers.length, lengthAndCapacity);
        break;
    case HoleArrayOperand::Capacity:
        jit.lshift32(registers.capacity, TrustedImm32(holeArrayCapacityShift), lengthAndCapacity);
        break;
    }
    jit.move(TrustedImm32(shape.indexingType), flags);
    jit.callOperationAndReturn(Entry::operationAOTNewArrayOfHoles, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = lengthAndCapacity,
        .thirdArgumentIn = flags,
    });
}

static void emitArrayOfHoles(StubAssembler& jit, const HoleArrayShape& shape)
{
    static_assert(sizeof(IndexingHeader) + maxHoleArrayCapacity * sizeof(EncodedJSValue) <= MarkedSpace::largeCutoff && maxHoleArrayCapacity & 1);
    RELEASE_ASSERT(shape.indexingType == ArrayWithDouble || shape.indexingType == ArrayWithInt32 || shape.indexingType == ArrayWithContiguous);
    RELEASE_ASSERT(shape.operand != HoleArrayOperand::None || shape.fixedCapacity & 1);
    const HoleArrayRegisters registers = allocateHoleArrayRegisters(jit, shape.operand);
    const auto [array, allocator, scratch, storage, capacity, length] = registers;

    JumpList slowPath;
    JumpList callerSlowPath;
    switch (shape.operand) {
    case HoleArrayOperand::None:
    case HoleArrayOperand::LengthAndCapacity:
        callerSlowPath.append(branchIfHavingABadTime(jit));
        break;
    case HoleArrayOperand::Capacity:
        break;
    }
    emitChooseStorageAllocator(jit, shape, registers, slowPath);
    allocateCell(jit, storage, allocator, scratch, slowPath);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(arrayAllocator)), allocator);
    allocateCell(jit, array, allocator, scratch, slowPath);
    emitFillWithHoles(jit, shape, registers);
    jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(shape.indexingType)), scratch);
    jit.storePair64(scratch, storage, array, TrustedImm32(0));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    if (!callerSlowPath.empty()) {
        callerSlowPath.link(&jit);
        if (shape.operand == HoleArrayOperand::None && shape.indexingType == ArrayWithDouble) {
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
    emitCallNewArrayOfHoles(jit, shape, registers);
}

void emitEmptyArrayOfHoles(StubAssembler& jit, EmptyArray form)
{
    switch (form) {
    case EmptyArray::DoubleCapacity1:
        emitArrayOfHoles(jit, { .indexingType = ArrayWithDouble, .operand = HoleArrayOperand::None, .fixedCapacity = 1 });
        return;
    case EmptyArray::DoubleCapacity3:
        emitArrayOfHoles(jit, { .indexingType = ArrayWithDouble, .operand = HoleArrayOperand::None, .fixedCapacity = 3 });
        return;
    case EmptyArray::DoubleDefaultCapacity:
        emitArrayOfHoles(jit, { .indexingType = ArrayWithDouble, .operand = HoleArrayOperand::None, .fixedCapacity = arrayLiteralVectorLength(0) });
        return;
    case EmptyArray::Int32DefaultCapacity:
        emitArrayOfHoles(jit, { .indexingType = ArrayWithInt32, .operand = HoleArrayOperand::None, .fixedCapacity = arrayLiteralVectorLength(0) });
        return;
    case EmptyArray::ContiguousCapacity1:
    case EmptyArray::ContiguousCapacity3:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubSignature signatureOfNewEmptyArrayWithCapacity(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::T9) });
}

void generateNewEmptyArrayWithCapacity(StubAssembler& jit)
{
    static_assert(arrayLiteralVectorLength(0) == 5);
    const GPRReg requestedCapacity = jit.operand(0);
    jit.linkToStub(jit.branch64(CCallHelpers::BelowOrEqual, requestedCapacity, TrustedImm32(1)), newEmptyArray(EmptyArray::ContiguousCapacity1));
    jit.linkToStub(jit.branch64(CCallHelpers::BelowOrEqual, requestedCapacity, TrustedImm32(3)), newEmptyArray(EmptyArray::ContiguousCapacity3));
    jit.linkToStub(jit.branch64(CCallHelpers::BelowOrEqual, requestedCapacity, TrustedImm32(arrayLiteralVectorLength(0))), arrayLiteral(0));
    jit.linkToStub(jit.branch64(CCallHelpers::Above, requestedCapacity, TrustedImm32(maxHoleArrayCapacity)), arrayLiteral(0));
    jit.linkToStub(branchIfHavingABadTime(jit), arrayLiteral(0));
    emitArrayOfHoles(jit, { .indexingType = ArrayWithContiguous, .operand = HoleArrayOperand::Capacity });
}

StubSignature signatureOfNewArrayWithLength(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::T9) });
}

void generateNewArrayWithLength(StubAssembler& jit)
{
    emitArrayOfHoles(jit, { .indexingType = indexingTypeOf(static_cast<ArrayElementKind>(jit.key().form())), .operand = HoleArrayOperand::LengthAndCapacity });
}

static constexpr IndexingType indexingShapeOf(ArrayElementKind kind)
{
    return indexingTypeOf(kind) & IndexingShapeMask;
}

// The realm is not having a bad time, so no stub entered from here returns 0.
static void emitJumpToEmptyArrayStub(StubAssembler& jit, ArrayElementKind kind, GPRReg profileWord, GPRReg capacity)
{
    static_assert(ArrayAllocationProfile::capacityMask == 0xff && (BASE_CONTIGUOUS_VECTOR_LEN_MAX | 1) <= maxHoleArrayCapacity);
    RELEASE_ASSERT(capacity == gprOf(StubRegister::T9));
    jit.zeroExtend8To32(profileWord, capacity);
    Jump hasDefaultCapacity = jit.branchTest32(CCallHelpers::Zero, capacity);
    switch (kind) {
    case ArrayElementKind::Int32:
        jit.linkToStub(hasDefaultCapacity, newEmptyArray(EmptyArray::Int32DefaultCapacity));
        break;
    case ArrayElementKind::Double:
        jit.linkToStub(hasDefaultCapacity, newEmptyArray(EmptyArray::DoubleDefaultCapacity));
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

StubSignature signatureOfNewEmptyArrayAtSite(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::T9) });
}

void generateNewEmptyArrayAtSite(StubAssembler& jit)
{
    constexpr unsigned numberOfShapeBits = std::popcount(ArrayAllocationProfile::shapeMask);
    const ArrayElementKind seed = static_cast<ArrayElementKind>(jit.key().form());
    const GPRReg profile = jit.clobberOperand(0);
    const GPRReg word = jit.allocateScratch(StubRegister::T12);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const GPRReg capacity = profile;
    const GPRReg learnedShape = scratch;
    const Address wordInProfile(profile, OBJECT_OFFSETOF(ArrayAllocationProfile, word));

    jit.load32(wordInProfile, word);
    jit.add32(TrustedImm32(1 << ArrayAllocationProfile::allocationCountShift), word);
    jit.store32(word, wordInProfile);
    jit.and32(TrustedImm32(ArrayAllocationProfile::intervalMask), word, scratch);
    Jump isDueForReview = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ArrayAllocationProfile::countAtReview));
    jit.linkToStub(branchIfHavingABadTime(jit), arrayLiteral(0));

    if (seed != ArrayElementKind::Contiguous)
        extractBits(jit, word, ArrayAllocationProfile::shapeShift, numberOfShapeBits, learnedShape);
    for (ArrayElementKind kind : { ArrayElementKind::Int32, ArrayElementKind::Double }) {
        if (indexingShapeOf(kind) < indexingShapeOf(seed))
            continue;
        Jump needsMoreGeneralShape = jit.branch32(CCallHelpers::Above, learnedShape, TrustedImm32(indexingShapeOf(kind)));
        emitJumpToEmptyArrayStub(jit, kind, word, capacity);
        needsMoreGeneralShape.link(&jit);
    }
    emitJumpToEmptyArrayStub(jit, ArrayElementKind::Contiguous, word, capacity);

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
