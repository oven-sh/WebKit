/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsAllocation.h"

#if ENABLE(AOT)

#include "AOTAllocationSiteRecord.h"
#include "AOTCellFormat.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "CompleteSubspace.h"
#include "JSArrayBufferView.h"
#include "LocalAllocator.h"
#include "MarkedSpace.h"
#include <optional>
#include <type_traits>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

static_assert(offsetOfFirstField == copyChunkBytes && offsetOfOrderWord == sizeof(JSCell) && offsetOfShapeWord == sizeof(JSCell) + sizeof(uint32_t));
static_assert(AllocationSiteRecord::offsetOfOrderAndShapeWords() == AllocationSiteRecord::offsetOfHeader() + static_cast<ptrdiff_t>(sizeof(uint64_t)) && sizeof(JSCell) == sizeof(uint64_t));
static_assert(minExtensionSize == copyChunkBytes);

struct AllocationSiteRegisters {
    GPRReg cell;
    GPRReg allocation;
    GPRReg record;
    GPRReg allocator;
    GPRReg scratch;
};

#if CPU(ARM64)
struct VectorPair {
    FPRReg first;
    FPRReg second;
};
#endif

struct ObjectCopyRegisters {
    GPRReg cell;
    GPRReg source;
    GPRReg endOffset;
    GPRReg header;
    GPRReg firstScratch;
    GPRReg secondScratch;
#if CPU(ARM64)
    std::optional<VectorPair> loopVectors { };
#endif
};

void loadAllocationSiteRecord(StubAssembler& jit, GPRReg allocation, GPRReg record, GPRReg scratch)
{
    static_assert(std::is_same_v<decltype(ObjectWords::distancesToAllocationSites), int32_t*>);
    constexpr int32_t distanceToEmptyRecord = -static_cast<int32_t>(sizeof(AllocationSiteRecord));
    ASSERT(noOverlap(record, allocation) && noOverlap(record, scratch));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(objects.distancesToAllocationSites)), record);
#if CPU(ARM64)
    jit.load32SignedExtendTo64(BaseIndex(record, allocation, CCallHelpers::TimesFour, 0, CCallHelpers::Extend::ZExt32), scratch);
    jit.addPtr(scratch, record);
    jit.addPtr(TrustedImm32(distanceToEmptyRecord), record);
#else
    jit.load32SignedExtendTo64(BaseIndex(record, allocation, CCallHelpers::TimesFour), scratch);
    jit.getEffectiveAddress(BaseIndex(record, scratch, CCallHelpers::TimesOne, distanceToEmptyRecord), record);
#endif
}

static AllocationSiteRegisters allocateSiteRegisters(StubAssembler& jit)
{
    return {
        .cell = jit.result(),
        .allocation = jit.operand(0),
        .record = jit.allocateScratch(StubRegister::T11),
        .allocator = jit.allocateScratch(StubRegister::T12),
        .scratch = jit.allocateScratch(StubRegister::T13),
    };
}

[[nodiscard]] static AssemblyHelpers::FreeListRefill allocateAtSite(StubAssembler& jit, const AllocationSiteRegisters& registers, JumpList& slowPath, std::optional<unsigned> cellSize = std::nullopt)
{
    const auto [cell, allocation, record, allocator, scratch] = registers;
    loadAllocationSiteRecord(jit, allocation, record, allocator);
    jit.loadPtr(Address(record, AllocationSiteRecord::offsetOfAllocator()), allocator);
    return allocateCellWithLateRefill(jit, cell, allocator, scratch, slowPath, cellSize);
}

StubSignature signatureOfAllocateCell(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::T9) });
}

void generateAllocateCell(StubAssembler& jit)
{
    const AllocationSiteRegisters registers = allocateSiteRegisters(jit);
    const auto [cell, allocation, record, allocator, scratch] = registers;
    JumpList slowPath;
    static_assert(cellSizeStep == MarkedSpace::sizeStep && maxCellSizeInKey == MarkedSpace::preciseCutoff);
    const unsigned steps = jit.key().count();
    const AssemblyHelpers::FreeListRefill refill = allocateAtSite(jit, registers, slowPath, steps ? std::optional<unsigned>(steps * cellSizeStep) : std::nullopt);
    switch (static_cast<CellInitialization>(jit.key().form())) {
    case CellInitialization::Header:
        jit.load64(Address(record, AllocationSiteRecord::offsetOfHeader()), scratch);
        jit.store64(scratch, Address(cell));
        break;
    case CellInitialization::HeaderAndWords:
#if CPU(X86_64)
        if (jit.supportsAVX()) {
            jit.m_assembler.vmovups_mr(AllocationSiteRecord::offsetOfHeader(), record, CCallHelpers::fpTempRegister);
            jit.m_assembler.vmovups_rm(CCallHelpers::fpTempRegister, 0, cell);
        } else {
            jit.m_assembler.movups_mr(AllocationSiteRecord::offsetOfHeader(), record, CCallHelpers::fpTempRegister);
            jit.m_assembler.movups_rm(CCallHelpers::fpTempRegister, 0, cell);
        }
#else
        jit.loadPair64(record, TrustedImm32(AllocationSiteRecord::offsetOfHeader()), allocator, scratch);
        jit.storePair64(allocator, scratch, cell, TrustedImm32(0));
#endif
        break;
    }
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowPath);
    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTAllocateSlow, {
        .returns = OperationCall::Returns::Value,
        .firstArgument = OperationCall::FirstArgument::Instance,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = allocation,
    });
}

static void copyChunk(StubAssembler& jit, const ObjectCopyRegisters& registers, int32_t offset)
{
#if CPU(X86_64)
    constexpr FPRReg chunk = CCallHelpers::fpTempRegister;
    if (jit.supportsAVX()) {
        jit.m_assembler.vmovups_mr(offset, registers.source, chunk);
        jit.m_assembler.vmovups_rm(chunk, offset, registers.cell);
    } else {
        jit.m_assembler.movups_mr(offset, registers.source, chunk);
        jit.m_assembler.movups_rm(chunk, offset, registers.cell);
    }
#else
    jit.loadPair64(registers.source, TrustedImm32(offset), registers.firstScratch, registers.secondScratch);
    jit.storePair64(registers.firstScratch, registers.secondScratch, registers.cell, TrustedImm32(offset));
#endif
}

#if CPU(X86_64)
static void copyIndexedChunk(StubAssembler& jit, const ObjectCopyRegisters& registers, int32_t offset)
{
    constexpr FPRReg chunk = CCallHelpers::fpTempRegister;
    const GPRReg index = registers.endOffset;
    if (jit.supportsAVX()) {
        jit.m_assembler.vmovups_mr(offset, registers.source, index, CCallHelpers::TimesOne, chunk);
        jit.m_assembler.vmovups_rm(chunk, offset, registers.cell, index, CCallHelpers::TimesOne);
    } else {
        jit.m_assembler.movups_mr(offset, registers.source, index, CCallHelpers::TimesOne, chunk);
        jit.m_assembler.movups_rm(chunk, offset, registers.cell, index, CCallHelpers::TimesOne);
    }
}
#endif

// The loop moves two chunks at a time, from the last field down. With an odd number of chunks it also copies the source's header and words.
static void copyFields(StubAssembler& jit, const ObjectCopyRegisters& registers, unsigned copiedFieldBytes)
{
    for (unsigned offset = copyChunkBytes; offset <= copiedFieldBytes; offset += copyChunkBytes)
        copyChunk(jit, registers, offset);
    if (copiedFieldBytes)
        return;
#if CPU(ARM64)
    const GPRReg from = registers.firstScratch;
    const GPRReg to = registers.secondScratch;
    RELEASE_ASSERT(registers.loopVectors);
    const VectorPair vectors = *registers.loopVectors;
    jit.add64(registers.source, registers.endOffset, from);
    jit.add64(registers.cell, registers.endOffset, to);
    Label loop = jit.label();
    jit.loadPairVector(CCallHelpers::PostIndexAddress(from, -2 * static_cast<int>(copyChunkBytes)), vectors.first, vectors.second);
    jit.storePairVector(vectors.first, vectors.second, CCallHelpers::PostIndexAddress(to, -2 * static_cast<int>(copyChunkBytes)));
#else
    Label loop = jit.label();
    copyIndexedChunk(jit, registers, 0);
    copyIndexedChunk(jit, registers, copyChunkBytes);
#endif
    jit.branchSub64(CCallHelpers::PositiveOrZero, TrustedImm32(2 * copyChunkBytes), registers.endOffset).linkTo(loop, &jit);
}

static void initializeCopy(StubAssembler& jit, const ObjectCopyRegisters& registers, unsigned copiedFieldBytes)
{
    const GPRReg words = registers.secondScratch;
    copyFields(jit, registers, copiedFieldBytes);
    jit.load64(Address(registers.source, offsetOfOrderWord), words);
    jit.and64(TrustedImm64(static_cast<int64_t>(orderCountAndShapeMask)), words);
    jit.storePair64(registers.header, words, registers.cell, TrustedImm32(0));
}

StubSignature signatureOfCopyObject(StubKey key)
{
    if (key.count())
        return StubSignature::withGPRResult({ gprOf(StubRegister::T9), gprOf(StubRegister::A1) });
    return StubSignature::withGPRResult({ gprOf(StubRegister::T9), gprOf(StubRegister::A1), gprOf(copyEndOffsetRegister) });
}

void generateCopyObject(StubAssembler& jit)
{
    const unsigned copiedFieldBytes = jit.key().count() * copyChunkBytes;
    const AllocationSiteRegisters site = allocateSiteRegisters(jit);
    ObjectCopyRegisters copy {
        .cell = site.cell,
        .source = jit.operand(1),
        .endOffset = copiedFieldBytes ? jit.allocateScratch(copyEndOffsetRegister) : jit.clobberOperand(2),
        .header = site.record,
        .firstScratch = site.allocator,
        .secondScratch = site.scratch,
    };
#if CPU(ARM64)
    if (!copiedFieldBytes)
        copy.loopVectors = VectorPair { jit.allocateScratch(ARM64Registers::q30), jit.allocateScratch(ARM64Registers::q31) };
#endif
    JumpList slowPath;
    const AssemblyHelpers::FreeListRefill refill = allocateAtSite(jit, site, slowPath);
    jit.load64(Address(site.record, AllocationSiteRecord::offsetOfHeader()), copy.header);
    initializeCopy(jit, copy, copiedFieldBytes);
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowPath);
    slowPath.link(&jit);
    if (copiedFieldBytes) {
        jit.move(TrustedImm32(copiedFieldBytes - copyChunkBytes), copy.endOffset);
        jit.jumpToStub(jit.key().withCount(0));
        return;
    }
    jit.preservingCallerSavedRegisters(true, [&](unsigned) {
        jit.move(site.allocation, operationArgumentGPR(1));
        jit.move(instanceGPR, operationArgumentGPR(0));
        jit.callOperation(Entry::operationAOTAllocateSlow, jit.reuseScratch(StubRegister::Operation));
    });
    jit.load64(Address(copy.cell), copy.header);
    initializeCopy(jit, copy, copiedFieldBytes);
    fenceAfterAllocation(jit);
    jit.returnToCaller();
}

StubSignature signatureOfAllocateExtension(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::T9));
    signature.addOperand(gprOf(StubRegister::A2));
    return signature;
}

void generateAllocateExtension(StubAssembler& jit)
{
    const unsigned size = jit.key().count() * copyChunkBytes;
    const GPRReg owner = jit.operand(0);
    const GPRReg wordOffset = jit.operand(1);
    const GPRReg granuleBits = jit.operand(2);
    const GPRReg storage = jit.allocateScratch(StubRegister::R0);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T12);
    const GPRReg temporary = jit.allocateScratch(StubRegister::T11);
    const BaseIndex extensionWord(owner, wordOffset, CCallHelpers::TimesOne);
    const bool isNarrow = traitsOf(hiddenReferenceRepresentation(jit.options().useWideReferences)).isNarrowReference;
    JumpList slowPath;
    // An Extension that lacks the granules gets one in the room it has, or the operation replaces it.
    if (isNarrow) {
        jit.load32(extensionWord, storage);
        Jump hasNone = jit.branchTest32(CCallHelpers::Zero, storage);
        decodeCell32(jit, storage, storage);
        jit.addPtr(TrustedImm32(offsetOfEmptyExtensionInCage), storage);
        appendGranule(jit, storage, granuleBits, temporary, allocator, slowPath);
        jit.returnToCaller();
        hasNone.link(&jit);
    } else {
        jit.load64(extensionWord, storage);
        Jump hasNone = jit.branchTest64(CCallHelpers::Zero, storage);
        appendGranule(jit, storage, granuleBits, temporary, allocator, slowPath);
        jit.returnToCaller();
        hasNone.link(&jit);
    }
    // The owner names its Extension by a narrow reference.
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cagedAuxiliarySpace)), allocator);
    jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(size) * sizeof(Allocator)), allocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    const AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, storage, allocator, temporary, slowPath);
    jit.move(TrustedImm32(0), temporary);
    jit.store64(temporary, Address(storage, sizeof(uint64_t)));
    for (unsigned offset = copyChunkBytes; offset < size; offset += copyChunkBytes)
        jit.storePair64(temporary, temporary, storage, TrustedImm32(offset));
    static_assert(!offsetOfExtensionGranuleBits && offsetOfFirstExtensionSlot == sizeof(uint64_t));
    jit.store64(granuleBits, Address(storage));
    fenceAfterAllocation(jit);
    if (isNarrow) {
        encodeCell32(jit, storage, storage);
        jit.sub32(TrustedImm32(offsetOfEmptyExtensionInCage >> cell32Shift), storage);
        jit.store32(storage, extensionWord);
    } else
        jit.store64(storage, extensionWord);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowPath);
    slowPath.link(&jit);
    jit.jumpToStub(StubKey(StubFamily::AllocateExtensionSlow));
}

StubSignature signatureOfAllocateExtensionSlow(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::A2));
    return signature;
}

void generateAllocateExtensionSlow(StubAssembler& jit)
{
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1));
    jit.callOperationAndReturn(Entry::operationAOTEnsureExtensionGranules, {
        .returns = OperationCall::Returns::Void,
        .firstArgument = OperationCall::FirstArgument::Instance,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .thirdArgumentIn = jit.operand(1),
    });
}

StubSignature signatureOfNewTypedArray(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), FPRInfo::argumentFPR0 });
}

void generateNewTypedArray(StubAssembler& jit)
{
    const GPRReg result = jit.result();
    const GPRReg type = jit.operand(0);
    const FPRReg lengthAsDouble = jit.operandFPR(1);
    const GPRReg length = jit.allocateScratch(StubRegister::A2);
    const GPRReg size = jit.allocateScratch(StubRegister::A3);
    const GPRReg storage = jit.allocateScratch(StubRegister::T9);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T10);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const GPRReg record = jit.allocateScratch(StubRegister::T12);
    const FPRReg scratchFPR = jit.allocateScratch(FPRInfo::argumentFPR1);
    constexpr unsigned recordSizeShift = getLSBSet(sizeof(TypedArrayAllocation));
    constexpr unsigned stepShift = getLSBSet(MarkedSpace::sizeStep);
    constexpr unsigned maxBytes = 256;
    static_assert(noOverlap(GPRInfo::returnValueGPR, gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3), gprOf(StubRegister::T9), gprOf(StubRegister::T10), gprOf(StubRegister::T11), gprOf(StubRegister::T12)));
    static_assert(hasOneBitSet(MarkedSpace::sizeStep) && sizeof(TypedArrayAllocation) == 32 && JSArrayBufferView::offsetOfByteOffset() == JSArrayBufferView::offsetOfLength() + static_cast<ptrdiff_t>(sizeof(uint64_t)));
    static_assert(maxBytes <= JSArrayBufferView::fastSizeLimit && maxBytes <= MarkedSpace::largeCutoff);
    auto inRecord = [&](ptrdiff_t offset) { return Address(record, AOT_INSTANCE_OFFSET(allocation.typedArrayAllocations) + offset); };
    JumpList slowCases;
    jit.branchConvertDoubleToInt32(lengthAsDouble, length, slowCases, scratchFPR, false);
    jit.sub32(length, TrustedImm32(1), scratch);
    slowCases.append(jit.branch32(CCallHelpers::AboveOrEqual, scratch, TrustedImm32(maxBytes)));
    jit.zeroExtend32ToWord(type, record);
    jit.lshift64(TrustedImm32(recordSizeShift), record);
    jit.addPtr(instanceGPR, record);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, inRecord(OBJECT_OFFSETOF(TypedArrayAllocation, allocator))));
    jit.load32(inRecord(OBJECT_OFFSETOF(TypedArrayAllocation, elementSize)), size);
    jit.mul32(length, size);
    jit.add32(TrustedImm32(7), size);
    jit.and32(TrustedImm32(~7), size);
    slowCases.append(jit.branch32(CCallHelpers::Above, size, TrustedImm32(maxBytes)));
    jit.add32(TrustedImm32(MarkedSpace::sizeStep - 1), size, scratch);
    jit.urshift32(TrustedImm32(stepShift), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.primitiveAuxiliarySpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, scratch, CCallHelpers::ScalePtr, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, storage, allocator, scratch, slowCases);
    jit.urshift32(TrustedImm32(3), size);
    Label nextWord = jit.label();
    jit.sub32(TrustedImm32(1), size);
    jit.store64(TrustedImm32(0), BaseIndex(storage, size, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, size).linkTo(nextWord, &jit);
    jit.loadPtr(inRecord(OBJECT_OFFSETOF(TypedArrayAllocation, allocator)), allocator);
    allocateCell(jit, result, allocator, scratch, slowCases);
    jit.load64(inRecord(OBJECT_OFFSETOF(TypedArrayAllocation, header)), scratch);
    jit.store64(scratch, Address(result));
    jit.store64(TrustedImm32(0), Address(result, JSObject::butterflyOffset()));
    jit.storePtr(storage, Address(result, JSArrayBufferView::offsetOfVector()));
    jit.store64(length, Address(result, JSArrayBufferView::offsetOfLength()));
    jit.store64(TrustedImm32(0), Address(result, JSArrayBufferView::offsetOfByteOffset()));
    jit.store8(TrustedImm32(FastTypedArray), Address(result, JSArrayBufferView::offsetOfMode()));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNewTypedArray, {
        .returns = OperationCall::Returns::Value,
        .firstArgument = OperationCall::FirstArgument::Instance,
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
