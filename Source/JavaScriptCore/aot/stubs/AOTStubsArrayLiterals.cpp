/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArrays.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationData.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubProgramTables.h"
#include "AOTStubRegisters.h"
#include "AOTStubsObjectCreation.h"
#include "ArrayConventions.h"
#include "Butterfly.h"
#include "CompleteSubspace.h"
#include "IndexingHeader.h"
#include "IndexingType.h"
#include "JSCellButterfly.h"
#include "JSObject.h"
#include "MarkedSpace.h"
#include "PureNaN.h"
#include <algorithm>
#include <bit>
#include <optional>

namespace JSC { namespace AOT {

static_assert(descriptorOf(StubFamily::NewEmptyArray).numberOfForms == static_cast<unsigned>(EmptyArray::ContiguousCapacity3OwnStorage) + 1);

static constexpr StubRegister countAndNumberMaskRegister = StubRegister::T13;

ptrdiff_t offsetOfArrayWithStorageAllocator(unsigned vectorLength, unsigned propertyRoom)
{
    return CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(sizeOfArrayWithStorage(vectorLength, propertyRoom)) * sizeof(Allocator);
}

static ptrdiff_t offsetOfArrayStorageAllocator(unsigned vectorLength)
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

struct ArrayLiteralShape {
    unsigned count { 0 };
    std::optional<unsigned> emptyCapacity { };
    bool areDoubles { false };
    bool areInt32 { false };
    bool hasStorageOfItsOwn { false };
};

static void emitStoreBoxedInt32(StubAssembler& jit, unsigned count, GPRReg base, size_t offset, GPRReg scratch)
{
    constexpr int32_t tag = static_cast<int32_t>(JSValue::NumberTag >> 32);
    auto at = [&](unsigned i) { return static_cast<int32_t>(offset + i * sizeof(EncodedJSValue)); };
#if CPU(ARM64)
    jit.move(TrustedImm32(tag), scratch);
    for (unsigned i = 0; i < count; ++i)
        jit.storePair32(jit.operand(i), scratch, base, TrustedImm32(at(i)));
#else
    UNUSED_PARAM(scratch);
    for (unsigned i = 0; i < count; ++i) {
        jit.store32(jit.operand(i), Address(base, at(i)));
        jit.store32(TrustedImm32(tag), Address(base, at(i) + static_cast<int32_t>(sizeof(int32_t))));
    }
#endif
}

static void emitArrayLiteral(StubAssembler& jit, ArrayLiteralShape shape)
{
    const auto [count, emptyCapacity, areDoubles, areInt32, hasStorageOfItsOwn] = shape;
    auto number = [&](unsigned i) { return jit.operandFPR(1 + i); };
    static_assert(maxArrayElementsInRegisters == 4);
    RELEASE_ASSERT(count <= maxArrayElementsInRegisters);
    static_assert(arrayLiteralVectorLength(0) == (BASE_CONTIGUOUS_VECTOR_LEN_EMPTY | 1));
    RELEASE_ASSERT(!emptyCapacity || (!count && *emptyCapacity & 1));
    const unsigned vectorLength = emptyCapacity.value_or(arrayLiteralVectorLength(count));
    const unsigned countAndCapacity = count | emptyCapacity.value_or(0) << capacityShiftInArrayLiteralCount;
    const GPRReg array = jit.result();
    const GPRReg storage = jit.allocateScratch(arrayStorageRegister);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T9);
    const GPRReg scratch = jit.allocateScratch(arrayAllocationScratchRegister);

    JumpList slowPath;
    for (unsigned i = 0; areDoubles && i < count; i += 2)
        slowPath.append(branchIfEitherIsNaN(jit, number(i), number(std::min(i + 1, count - 1))));
    std::optional<AssemblyHelpers::FreeListRefill> storageRefill;
    if (hasStorageOfItsOwn) {
        slowPath.append(branchIfHavingABadTime(jit));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), allocator);
        jit.loadPtr(Address(allocator, offsetOfArrayStorageAllocator(vectorLength)), allocator);
        slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
        storageRefill = allocateCellWithLateRefill(jit, storage, allocator, scratch, slowPath);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(arrayAllocator)), allocator);
    } else {
        RELEASE_ASSERT(ShortArrayWords::indexOfAllocator(vectorLength) < ShortArrayWords::numberOfAllocators);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(shortArrays.allocators) + ShortArrayWords::indexOfAllocator(vectorLength) * sizeof(void*)), allocator);
    }
    static_assert(sizeof(JSArray) <= MarkedSpace::preciseCutoff);
    AssemblyHelpers::FreeListRefill arrayRefill = allocateCellWithLateRefill(jit, array, allocator, scratch, slowPath, hasStorageOfItsOwn ? sizeof(JSArray) : sizeOfArrayWithStorage(vectorLength));
    if (!hasStorageOfItsOwn)
        jit.addPtr(TrustedImm32(sizeof(JSArray)), array, storage);
    jit.store64(TrustedImm64(static_cast<uint64_t>(vectorLength) << 32 | count), Address(storage, 0));
    if (areDoubles) {
        for (unsigned i = 0; i < count; ++i)
            jit.storeDouble(number(i), Address(storage, sizeof(IndexingHeader) + i * sizeof(double)));
        if (count < vectorLength)
            jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), scratch);
        for (unsigned i = count; i < vectorLength; ++i)
            jit.store64(scratch, Address(storage, sizeof(IndexingHeader) + i * sizeof(double)));
    } else if (areInt32) {
        emitStoreBoxedInt32(jit, count, storage, sizeof(IndexingHeader), scratch);
        emitStoreValuesAndClear(jit, 1, 0, vectorLength - count, storage, sizeof(IndexingHeader) + count * sizeof(EncodedJSValue));
    } else
        emitStoreValuesAndClear(jit, 1, count, vectorLength, storage, sizeof(IndexingHeader));
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(areDoubles ? ArrayWithDouble : areInt32 ? ArrayWithInt32 : ArrayWithContiguous)), scratch);
    jit.storePair64(scratch, storage, array, TrustedImm32(0));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    if (storageRefill)
        emitLateRefill(jit, *storageRefill, slowPath);
    emitLateRefill(jit, arrayRefill, slowPath);
    slowPath.link(&jit);
    if (areDoubles) {
        jit.jumpToStub(arrayLiteralWithDoubleSlow());
        return;
    }
    jit.callOperationAndReturn(Entry::operationAOTNewArray, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .spilledArguments = OperationCall::SpilledArguments { count, countAndCapacity | (hasStorageOfItsOwn ? storageOfItsOwnInArrayLiteralCount : 0) | (areInt32 ? int32ValuesInArrayLiteralCount : 0) },
    });
}

StubSignature signatureOfArrayLiteral(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ });
    for (unsigned i = 0; i < key.count(); ++i)
        signature.addOperand(operationArgumentGPR(1 + i));
    return signature;
}

void generateArrayLiteral(StubAssembler& jit) { emitArrayLiteral(jit, { .count = jit.key().count() }); }

StubSignature signatureOfArrayLiteralWithInt32(StubKey key) { return signatureOfArrayLiteral(arrayLiteral(key.count())); }
void generateArrayLiteralWithInt32(StubAssembler& jit) { emitArrayLiteral(jit, { .count = jit.key().count(), .areInt32 = true }); }

StubSignature signatureOfFrameArrayLiteral(StubKey key)
{
    StubSignature signature = signatureOfArrayLiteral(arrayLiteral(key.count()));
    signature.addOperand(operationArgumentGPR(1 + maxArrayElementsInRegisters));
    return signature;
}

// The cell, the indexing header and the elements, one behind the other. A realm whose arrays are not pristine has no header for such a cell, and gets an array on the heap.
void generateFrameArrayLiteral(StubAssembler& jit)
{
    const unsigned count = jit.key().count();
    const GPRReg cell = jit.operand(count);
    const GPRReg scratch = jit.allocateScratch(arrayAllocationScratchRegister);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.frameArrayHeader)), scratch);
    Jump isOnHeap = jit.branchTest64(CCallHelpers::Zero, scratch);
    jit.store64(scratch, Address(cell, 0));
    jit.addPtr(TrustedImm32(sizeOfFrameArray(0)), cell, scratch);
    jit.store64(scratch, Address(cell, JSObject::butterflyOffset()));
    jit.store64(TrustedImm64(static_cast<uint64_t>(count) << 32 | count), Address(cell, sizeof(JSArray)));
    for (unsigned i = 0; i < count; ++i)
        jit.store64(jit.operand(i), Address(cell, sizeOfFrameArray(i)));
    jit.move(cell, jit.result());
    jit.returnToCaller();

    isOnHeap.link(&jit);
    jit.jumpToStub(arrayLiteral(count));
}

static StubSignature signatureTakingDoubles(unsigned count)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(countAndNumberMaskRegister) });
    for (unsigned i = 0; i < count; ++i)
        signature.addOperand(FPRInfo::toArgumentRegister(i));
    return signature;
}

StubSignature signatureOfArrayLiteralWithDouble(StubKey key) { return signatureTakingDoubles(key.count()); }
StubSignature signatureOfArrayLiteralWithDoubleSlow(StubKey) { return signatureTakingDoubles(maxArrayElementsInRegisters); }

void generateArrayLiteralWithDouble(StubAssembler& jit) { emitArrayLiteral(jit, { .count = jit.key().count(), .areDoubles = true }); }

void generateArrayLiteralWithDoubleSlow(StubAssembler& jit)
{
    jit.callOperationAndReturn(Entry::operationAOTNewArrayFromDoubles, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = jit.operand(0),
    });
}

// On x86-64 the immediate and the result share a register.
StubSignature signatureOfArrayOfData(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ });
    signature.immediate = isARM64() ? gprOf(stubImmediateRegister) : GPRInfo::returnValueGPR;
    if (static_cast<ArrayOfData>(key.form()) == ArrayOfData::SharedValues)
        signature.addOperand(operationArgumentGPR(1));
    return signature;
}

void generateArrayOfData(StubAssembler& jit)
{
    const ArrayOfData form = static_cast<ArrayOfData>(jit.key().form());
    const GPRReg array = jit.result();
    // On ARM64 the immediate comes in T9.
    const GPRReg request = jit.allocateScratch(isARM64() ? StubRegister::T13 : StubRegister::T9);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T10);
    const GPRReg storage = jit.allocateScratch(arrayStorageRegister);
    const GPRReg scratch = jit.allocateScratch(arrayAllocationScratchRegister);
    RELEASE_ASSERT(noOverlap(array, request, allocator, storage, scratch, instanceGPR));
    const IndexingType indexingType = form == ArrayOfData::Int32 ? ArrayWithInt32 : form == ArrayOfData::Double ? ArrayWithDouble : ArrayWithContiguous;

    JumpList slowPath;
    jit.zeroExtend32ToWord(jit.immediate(), request);
    if (form == ArrayOfData::SharedValues) {
        slowPath.append(jit.branchTest64(CCallHelpers::Zero, jit.operand(0)));
        jit.addPtr(TrustedImm32(JSCellButterfly::offsetOfData()), jit.operand(0), storage);
    } else {
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), scratch);
        loadProgramTable(jit, scratch, ProgramTableIndex::requests, storage);
        jit.load32(BaseIndex(storage, request, CCallHelpers::TimesOne, OBJECT_OFFSETOF(DataRequest, root)), scratch);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.data)), storage);
        jit.getEffectiveAddress(BaseIndex(storage, scratch, CCallHelpers::TimesEight, DataNodeHeader::wordsBeforeElements * sizeof(uint64_t)), storage);
    }
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(arrayAllocator)), allocator);
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, array, allocator, scratch, slowPath);
    // A realm whose arrays are not pristine has no header for such a cell.
    jit.load64(Address(instanceGPR, Instance::offsetOfNewCopyOnWriteArrayHeader(indexingType)), scratch);
    slowPath.append(jit.branchTest64(CCallHelpers::Zero, scratch));
    jit.storePair64(scratch, storage, array, TrustedImm32(0));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowPath);
    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNewFromData, {
        .returns = OperationCall::Returns::Value,
        .firstArgument = OperationCall::FirstArgument::Instance,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = request,
    });
}

StubSignature signatureOfNewEmptyArray(StubKey) { return StubSignature::withGPRResult({ }); }

void generateNewEmptyArray(StubAssembler& jit)
{
    const EmptyArray form = static_cast<EmptyArray>(jit.key().form());
    switch (form) {
    case EmptyArray::ContiguousCapacity1:
        return emitArrayLiteral(jit, { .emptyCapacity = 1 });
    case EmptyArray::ContiguousCapacity3:
        return emitArrayLiteral(jit, { .emptyCapacity = 3 });
    case EmptyArray::ContiguousCapacity1OwnStorage:
        return emitArrayLiteral(jit, { .emptyCapacity = 1, .hasStorageOfItsOwn = true });
    case EmptyArray::ContiguousCapacity3OwnStorage:
        return emitArrayLiteral(jit, { .emptyCapacity = 3, .hasStorageOfItsOwn = true });
    case EmptyArray::DoubleCapacity1:
    case EmptyArray::DoubleCapacity3:
    case EmptyArray::DoubleDefaultCapacity:
    case EmptyArray::Int32DefaultCapacity:
        return emitEmptyArrayOfHoles(jit, form);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
