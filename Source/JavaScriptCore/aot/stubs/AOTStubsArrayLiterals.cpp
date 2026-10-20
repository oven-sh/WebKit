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
#include "AOTStubsObjectCreation.h"
#include "ArrayConventions.h"
#include "Butterfly.h"
#include "CompleteSubspace.h"
#include "IndexingHeader.h"
#include "IndexingType.h"
#include "JSObject.h"
#include "MarkedSpace.h"
#include "PureNaN.h"
#include <algorithm>
#include <bit>
#include <optional>

namespace JSC { namespace AOT {

static_assert(JSObject::butterflyOffset() == sizeof(JSCell) && sizeof(JSCell) == sizeof(uint64_t));
static_assert(sizeof(IndexingHeader) == sizeof(EncodedJSValue) && MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
static_assert(Butterfly::offsetOfPublicLength() == -static_cast<ptrdiff_t>(sizeof(IndexingHeader)) && Butterfly::offsetOfVectorLength() == Butterfly::offsetOfPublicLength() + static_cast<ptrdiff_t>(sizeof(uint32_t)));
static_assert(descriptorOf(StubFamily::NewEmptyArray).numberOfForms == static_cast<unsigned>(EmptyArray::Int32DefaultCapacity) + 1);

#if CPU(ARM64)
static constexpr StubRegister allocationScratchRegister = StubRegister::T11;
static constexpr StubRegister storageRegister = StubRegister::T12;
#elif CPU(X86_64)
static constexpr StubRegister allocationScratchRegister = StubRegister::A0;
static constexpr StubRegister storageRegister = StubRegister::A5;
#endif
static constexpr StubRegister countAndNumberMaskRegister = StubRegister::T13;

ptrdiff_t offsetOfArrayStorageAllocator(unsigned vectorLength)
{
    return CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(sizeof(IndexingHeader) + vectorLength * sizeof(EncodedJSValue)) * sizeof(Allocator);
}

Jump branchIfHavingABadTime(StubAssembler& jit)
{
    return jit.branchTest32(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader)));
}

static Jump branchIfEitherIsNaN(CCallHelpers& jit, FPRReg first, FPRReg second)
{
#if CPU(ARM64)
    jit.m_assembler.fcmp<64>(first, second);
    jit.m_assembler.b_cond(ARM64Assembler::ConditionVS);
    AssemblerLabel branch = jit.m_assembler.labelIgnoringWatchpoints();
    jit.m_assembler.nop();
    return Jump(branch, ARM64Assembler::JumpCondition, ARM64Assembler::ConditionVS);
#else
    if (jit.supportsAVX())
        jit.m_assembler.vucomisd_rr(second, first);
    else
        jit.m_assembler.ucomisd_rr(second, first);
    return Jump(jit.m_assembler.jCC(X86Assembler::ConditionP));
#endif
}

ArrayRefills allocateArrayWithLateRefills(StubAssembler& jit, const ArrayAllocation& allocation, JumpList& slowPath)
{
    const GPRReg storage = allocation.storage;
    const GPRReg allocator = allocation.allocator;
    ArrayRefills refills;
    slowPath.append(branchIfHavingABadTime(jit));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), allocator);
    jit.loadPtr(Address(allocator, offsetOfArrayStorageAllocator(allocation.vectorLength)), allocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    refills.storage = allocateCellWithLateRefill(jit, storage, allocator, allocation.scratch, slowPath);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(arrayAllocator)), allocator);
    refills.array = allocateCellWithLateRefill(jit, allocation.array, allocator, allocation.scratch, slowPath);
    jit.store64(TrustedImm64(static_cast<uint64_t>(allocation.vectorLength) << 32 | allocation.length), Address(storage, 0));
    return refills;
}

void finishArrayAllocation(StubAssembler& jit, const ArrayAllocation& allocation)
{
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), allocation.storage);
    jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(allocation.indexingType)), allocation.scratch);
    jit.storePair64(allocation.scratch, allocation.storage, allocation.array, TrustedImm32(0));
    fenceAfterAllocation(jit);
}

void emitLateRefills(StubAssembler& jit, const ArrayRefills& refills, JumpList& slowPath)
{
    emitLateRefill(jit, refills.storage, slowPath);
    emitLateRefill(jit, refills.array, slowPath);
}

struct ArrayLiteralShape {
    unsigned count { 0 };
    std::optional<unsigned> emptyCapacity { };
    bool areDoubles { false };
};

static void emitArrayLiteral(StubAssembler& jit, ArrayLiteralShape shape)
{
    const auto [count, emptyCapacity, areDoubles] = shape;
    auto number = [&](unsigned i) { return jit.operandFPR(1 + i); };
    static_assert(maxArrayElementsInRegisters == 4);
    RELEASE_ASSERT(count <= maxArrayElementsInRegisters);
    static_assert(arrayLiteralVectorLength(0) == (BASE_CONTIGUOUS_VECTOR_LEN_EMPTY | 1));
    RELEASE_ASSERT(!emptyCapacity || (!count && *emptyCapacity & 1));
    const unsigned vectorLength = emptyCapacity.value_or(arrayLiteralVectorLength(count));
    const unsigned countAndCapacity = count | emptyCapacity.value_or(0) << capacityShiftInArrayLiteralCount;
    const ArrayAllocation allocation {
        .array = jit.result(),
        .storage = jit.allocateScratch(storageRegister),
        .allocator = jit.allocateScratch(StubRegister::T9),
        .scratch = jit.allocateScratch(allocationScratchRegister),
        .length = count,
        .vectorLength = vectorLength,
        .indexingType = areDoubles ? ArrayWithDouble : ArrayWithContiguous,
    };
    const GPRReg storage = allocation.storage;
    const GPRReg scratch = allocation.scratch;

    JumpList slowPath;
    for (unsigned i = 0; areDoubles && i < count; i += 2)
        slowPath.append(branchIfEitherIsNaN(jit, number(i), number(std::min(i + 1, count - 1))));
    ArrayRefills refills = allocateArrayWithLateRefills(jit, allocation, slowPath);
    if (areDoubles) {
        for (unsigned i = 0; i < count; ++i)
            jit.storeDouble(number(i), Address(storage, sizeof(IndexingHeader) + i * sizeof(double)));
        if (count < vectorLength)
            jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), scratch);
        for (unsigned i = count; i < vectorLength; ++i)
            jit.store64(scratch, Address(storage, sizeof(IndexingHeader) + i * sizeof(double)));
    } else
        emitStoreValuesAndClear(jit, 1, count, vectorLength, storage, sizeof(IndexingHeader));
    finishArrayAllocation(jit, allocation);
    jit.returnToCaller();

    emitLateRefills(jit, refills, slowPath);
    slowPath.link(&jit);
    if (areDoubles) {
        jit.jumpToStub(arrayLiteralWithDoubleSlow());
        return;
    }
    jit.callOperationAndReturn(Entry::operationAOTNewArray, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .spilledArguments = OperationCall::SpilledArguments { count, countAndCapacity },
    });
}

StubSignature signatureOfArrayLiteral(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ });
    for (unsigned i = 0; i < key.count(); ++i)
        signature.addOperand(operationArgumentGPR(1 + i));
    return signature;
}

void generateArrayLiteral(StubAssembler& jit)
{
    emitArrayLiteral(jit, { .count = jit.key().count() });
}

static StubSignature signatureTakingDoubles(unsigned count)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(countAndNumberMaskRegister) });
    for (unsigned i = 0; i < count; ++i)
        signature.addOperand(FPRInfo::toArgumentRegister(i));
    return signature;
}

StubSignature signatureOfArrayLiteralWithDouble(StubKey key)
{
    return signatureTakingDoubles(key.count());
}

void generateArrayLiteralWithDouble(StubAssembler& jit)
{
    emitArrayLiteral(jit, { .count = jit.key().count(), .areDoubles = true });
}

StubSignature signatureOfArrayLiteralWithDoubleSlow(StubKey)
{
    return signatureTakingDoubles(maxArrayElementsInRegisters);
}

void generateArrayLiteralWithDoubleSlow(StubAssembler& jit)
{
    jit.callOperationAndReturn(Entry::operationAOTNewArrayFromDoubles, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = jit.operand(0),
    });
}

StubSignature signatureOfNewEmptyArray(StubKey)
{
    return StubSignature::withGPRResult({ });
}

void generateNewEmptyArray(StubAssembler& jit)
{
    const EmptyArray form = static_cast<EmptyArray>(jit.key().form());
    switch (form) {
    case EmptyArray::ContiguousCapacity1:
        emitArrayLiteral(jit, { .emptyCapacity = 1 });
        return;
    case EmptyArray::ContiguousCapacity3:
        emitArrayLiteral(jit, { .emptyCapacity = 3 });
        return;
    case EmptyArray::DoubleCapacity1:
    case EmptyArray::DoubleCapacity3:
    case EmptyArray::DoubleDefaultCapacity:
    case EmptyArray::Int32DefaultCapacity:
        emitEmptyArrayOfHoles(jit, form);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
