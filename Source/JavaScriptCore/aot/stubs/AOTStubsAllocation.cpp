/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsAllocation.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTObjectWords.h"
#include "AOTRuntimeTable.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsField.h"
#include "AOTStubsObjectCreation.h"
#include "CompleteSubspace.h"
#include "JSArrayBufferView.h"
#include "JSObject.h"
#include "LocalAllocator.h"
#include "MarkedSpace.h"
#include <optional>
#include <type_traits>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

using Returns = OperationCall::Returns;
using Saves = OperationCall::Saves;

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

#if CPU(X86_64)
static void loadChunk(StubAssembler& jit, GPRReg base, int32_t offset)
{
    constexpr FPRReg chunk = CCallHelpers::fpTempRegister;
    if (jit.supportsAVX())
        jit.m_assembler.vmovups_mr(offset, base, chunk);
    else
        jit.m_assembler.movups_mr(offset, base, chunk);
}

static void storeChunk(StubAssembler& jit, GPRReg base, int32_t offset, std::optional<GPRReg> index = std::nullopt)
{
    constexpr FPRReg chunk = CCallHelpers::fpTempRegister;
    if (jit.supportsAVX())
        index ? jit.m_assembler.vmovups_rm(chunk, offset, base, *index, CCallHelpers::TimesOne) : jit.m_assembler.vmovups_rm(chunk, offset, base);
    else
        index ? jit.m_assembler.movups_rm(chunk, offset, base, *index, CCallHelpers::TimesOne) : jit.m_assembler.movups_rm(chunk, offset, base);
}
#endif

static void copyChunk(StubAssembler& jit, Address from, Address to, GPRReg firstScratch, GPRReg secondScratch)
{
#if CPU(X86_64)
    UNUSED_PARAM(firstScratch);
    UNUSED_PARAM(secondScratch);
    loadChunk(jit, from.base, from.offset);
    storeChunk(jit, to.base, to.offset);
#else
    jit.loadPair64(from.base, TrustedImm32(from.offset), firstScratch, secondScratch);
    jit.storePair64(firstScratch, secondScratch, to.base, TrustedImm32(to.offset));
#endif
}

static constexpr int32_t distanceToEmptyRecord = -static_cast<int32_t>(sizeof(AllocationSiteRecord));

#if CPU(ARM64)
static constexpr int32_t distanceLeftToOffsets = distanceToEmptyRecord;
#else
static constexpr int32_t distanceLeftToOffsets = 0;
#endif

static Address inRecordAtSite(GPRReg record, ptrdiff_t offset)
{
    return Address(record, distanceLeftToOffsets + static_cast<int32_t>(offset));
}

static void loadAllocationSiteRecordForOffsets(StubAssembler& jit, GPRReg allocation, GPRReg record, GPRReg scratch)
{
    static_assert(std::is_same_v<decltype(ObjectWords::distancesToAllocationSites), int32_t*>);
    ASSERT(noOverlap(record, allocation) && noOverlap(record, scratch));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(objects.distancesToAllocationSites)), record);
#if CPU(ARM64)
    jit.load32SignedExtendTo64(BaseIndex(record, allocation, CCallHelpers::TimesFour, 0, CCallHelpers::Extend::ZExt32), scratch);
    jit.addPtr(scratch, record);
#else
    jit.load32SignedExtendTo64(BaseIndex(record, allocation, CCallHelpers::TimesFour), scratch);
    jit.getEffectiveAddress(BaseIndex(record, scratch, CCallHelpers::TimesOne, distanceToEmptyRecord), record);
#endif
}

void loadAllocationSiteRecord(StubAssembler& jit, GPRReg allocation, GPRReg record, GPRReg scratch)
{
    loadAllocationSiteRecordForOffsets(jit, allocation, record, scratch);
    if constexpr (!!distanceLeftToOffsets)
        jit.addPtr(TrustedImm32(distanceLeftToOffsets), record);
}

static AllocationSiteRegisters allocateSiteRegisters(StubAssembler& jit)
{
    return { jit.result(), jit.operand(0), jit.allocateScratch(StubRegister::T11), jit.allocateScratch(scratchBesideShortStubImmediate), jit.allocateScratch(StubRegister::T13) };
}

[[nodiscard]] static AssemblyHelpers::FreeListRefill allocateAtSite(StubAssembler& jit, const AllocationSiteRegisters& registers, JumpList& slowPath, std::optional<unsigned> cellSize)
{
    const auto [cell, allocation, record, allocator, scratch] = registers;
    loadAllocationSiteRecordForOffsets(jit, allocation, record, allocator);
    jit.loadPtr(inRecordAtSite(record, AllocationSiteRecord::offsetOfAllocator()), allocator);
    return allocateCellWithLateRefill(jit, cell, allocator, scratch, slowPath, cellSize);
}

static void zeroBehindFirstChunk(StubAssembler& jit, const AllocationSiteRegisters& registers, std::optional<unsigned> cellSize)
{
    const auto [cell, allocation, record, allocator, scratch] = registers;
#if CPU(X86_64)
    jit.moveZeroToDouble(CCallHelpers::fpTempRegister);
#endif
    if (cellSize) {
        for (unsigned offset = copyChunkBytes; offset < *cellSize; offset += copyChunkBytes) {
#if CPU(X86_64)
            storeChunk(jit, cell, offset);
#else
            jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, cell, TrustedImm32(offset));
#endif
        }
        return;
    }

    static_assert(!(MarkedSpace::sizeStep % copyChunkBytes) && maxCellSizeInKey >= 2 * copyChunkBytes);
    jit.load32(Address(allocator, LocalAllocator::offsetOfCellSize()), scratch);
    jit.sub64(TrustedImm32(2 * copyChunkBytes), scratch);
    Label loop = jit.label();
#if CPU(X86_64)
    storeChunk(jit, cell, 0, scratch);
    storeChunk(jit, cell, copyChunkBytes, scratch);
#else
    jit.add64(cell, scratch, allocator);
    jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, allocator, TrustedImm32(0));
    jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, allocator, TrustedImm32(copyChunkBytes));
#endif
    jit.branchSub64(CCallHelpers::PositiveOrZero, TrustedImm32(2 * copyChunkBytes), scratch).linkTo(loop, &jit);
}

StubSignature signatureOfAllocateCell(StubKey key)
{
    return StubSignature::withGPRResult({ gprOf(shortStubImmediateRegister) }).withResultIn(key.operandRegister()).fillingOperandInFront(0);
}

void generateAllocateCell(StubAssembler& jit)
{
    const AllocationSiteRegisters registers = allocateSiteRegisters(jit);
    const auto [cell, allocation, record, allocator, scratch] = registers;
    JumpList slowPath;
    static_assert(cellSizeStep == MarkedSpace::sizeStep && maxCellSizeInKey == MarkedSpace::preciseCutoff);
    const unsigned steps = jit.key().count();
    const std::optional<unsigned> cellSize = steps ? std::optional<unsigned>(steps * cellSizeStep) : std::nullopt;
    const AssemblyHelpers::FreeListRefill refill = allocateAtSite(jit, registers, slowPath, cellSize);
    const auto initialization = static_cast<CellInitialization>(jit.key().form());
    if (zeroesBody(initialization))
        zeroBehindFirstChunk(jit, registers, cellSize);
    if (!storesWords(initialization)) {
        jit.load64(inRecordAtSite(record, AllocationSiteRecord::offsetOfHeader()), scratch);
        jit.store64(scratch, Address(cell));
        if (zeroesBody(initialization))
            jit.store64(TrustedImm32(0), Address(cell, sizeof(JSCell)));
    } else
        copyChunk(jit, inRecordAtSite(record, AllocationSiteRecord::offsetOfHeader()), Address(cell), allocator, scratch);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowPath);
    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTAllocateSlow, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = allocation });
}

StubSignature signatureOfAllocateCellFromTemplate(StubKey key)
{
    return StubSignature::withGPRResult({ gprOf(shortStubImmediateRegister), gprOf(StubRegister::T10) }).withResultIn(key.operandRegister());
}

void generateAllocateCellFromTemplate(StubAssembler& jit)
{
    const AllocationSiteRegisters registers = allocateSiteRegisters(jit);
    const auto [cell, allocation, record, allocator, scratch] = registers;
    const GPRReg body = jit.operand(1);
    RELEASE_ASSERT(noOverlap(cell, allocation, body, record, allocator, scratch, instanceGPR));
    static_assert(offsetOfTemplateInCell(CellInitialization::Header) == sizeof(JSCell) && offsetOfTemplateInCell(CellInitialization::HeaderAndWords) == offsetOfFirstField);
    static_assert(offsetOfFirstEnvironmentVariable == sizeof(JSCell) && cellSizeStep == copyChunkBytes);
    const auto initialization = static_cast<CellInitialization>(jit.key().form());
    const unsigned size = jit.key().count() * copyChunkBytes;
    const std::optional<unsigned> cellSize = size <= maxCellSizeInKey ? std::optional<unsigned>(size) : std::nullopt;
    JumpList slowPath;
    const AssemblyHelpers::FreeListRefill refill = allocateAtSite(jit, registers, slowPath, cellSize);
    for (unsigned offset = copyChunkBytes; offset < size; offset += copyChunkBytes)
        copyChunk(jit, Address(body, offset - offsetOfTemplateInCell(initialization)), Address(cell, offset), allocator, scratch);
    if (storesWords(initialization))
        copyChunk(jit, inRecordAtSite(record, AllocationSiteRecord::offsetOfHeader()), Address(cell), allocator, scratch);
    else {
        jit.load64(inRecordAtSite(record, AllocationSiteRecord::offsetOfHeader()), scratch);
        jit.store64(scratch, Address(cell));
        jit.load64(Address(body), scratch);
        jit.store64(scratch, Address(cell, sizeof(JSCell)));
    }
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowPath);
    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTAllocateSlowFromTemplate, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = allocation, .thirdArgumentIn = body });
}

StubSignature signatureOfInitializeObjectInFrame(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::T9) });
    signature.addOperand(signature.firstResult);
    return signature;
}

void generateInitializeObjectInFrame(StubAssembler& jit)
{
    const GPRReg cell = jit.result();
    const GPRReg allocation = jit.operand(0);
    const GPRReg record = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    RELEASE_ASSERT(cell == jit.operand(1) && noOverlap(cell, allocation, record, scratch, instanceGPR));

    auto loadRecord = [&] {
        loadAllocationSiteRecord(jit, allocation, record, scratch);
    };
    auto initialize = [&] {
        copyChunk(jit, Address(record, AllocationSiteRecord::offsetOfHeader()), Address(cell), record, scratch);
        jit.store8(TrustedImm32(frameCellState), Address(cell, JSCell::cellStateOffset()));
    };
    loadRecord();
    Jump hasNoHeader = jit.branchTest64(CCallHelpers::Zero, Address(record, AllocationSiteRecord::offsetOfHeader()), TrustedImm32(-1));
    initialize();
    jit.returnToCaller();

    hasNoHeader.link(&jit);
    auto initializeFromFilledRecord = [&] {
        loadRecord();
        initialize();
    };
    jit.callOperationAndReturn(Entry::operationAOTGetHeaderOfAllocation, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = allocation, .valueDestination = record }, initializeFromFilledRecord);
}

StubSignature signatureOfAllocateInstance(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::T9), gprOf(StubRegister::A2) });
    signature.result = StubResult::TwoGPRs;
    signature.secondResult = gprOf(StubRegister::T11);
    return signature.fillingOperandInFront(1);
}

void generateAllocateInstance(StubAssembler& jit)
{
    const GPRReg cell = jit.result();
    const GPRReg allocation = jit.operand(0);
    const GPRReg prototype = jit.operand(1);
    const GPRReg record = jit.secondResult();
    const GPRReg allocator = jit.allocateScratch(StubRegister::T12);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    const AllocationSiteRegisters site { cell, allocation, record, allocator, scratch };
    RELEASE_ASSERT(noOverlap(cell, allocation, prototype, record, allocator, scratch, instanceGPR));
    const unsigned steps = jit.key().count();
    const std::optional<unsigned> cellSize = steps ? std::optional<unsigned>(steps * cellSizeStep) : std::nullopt;

    JumpList isOutOfLine;
    loadAllocationSiteRecord(jit, allocation, record, allocator);
    isOutOfLine.append(jit.branchPtr(CCallHelpers::NotEqual, Address(record, AllocationSiteRecord::offsetOfPrototype()), prototype));
    jit.loadPtr(Address(record, AllocationSiteRecord::offsetOfAllocator()), allocator);
    const AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, cell, allocator, scratch, isOutOfLine, cellSize);
    zeroBehindFirstChunk(jit, site, cellSize);
    copyChunk(jit, Address(record, AllocationSiteRecord::offsetOfHeader()), Address(cell), allocator, scratch);
    jit.returnToCaller();

    emitLateRefill(jit, refill, isOutOfLine);
    isOutOfLine.link(&jit);
    jit.jumpToStub(StubKey(StubFamily::AllocateInstanceOutOfLine));
}

StubSignature signatureOfAllocateInstanceOutOfLine(StubKey key)
{
    StubSignature signature = signatureOfAllocateInstance(key);
    signature.addOperand(signature.secondResult);
    return signature;
}

void generateAllocateInstanceOutOfLine(StubAssembler& jit)
{
    const GPRReg cell = jit.result();
    const GPRReg allocation = jit.clobberOperand(0);
    const GPRReg prototype = jit.operand(1);
    const GPRReg record = jit.clobberOperand(2);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(record == jit.secondResult() && noOverlap(cell, allocation, prototype, record, scratch, instanceGPR));
    static_assert(operationArgumentGPR(2) == gprOf(StubRegister::A2));

    JumpList allocatesSlowly;
    allocatesSlowly.append(jit.branchPtr(CCallHelpers::Equal, Address(record, AllocationSiteRecord::offsetOfPrototype()), prototype));

    const GPRReg mask = jit.allocateScratch(StubRegister::T12);
    RELEASE_ASSERT(noOverlap(cell, allocation, prototype, record, scratch, mask, instanceGPR));
    JumpList isNotRemembered;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(classes.instancesBelowPrototypes.entries)), record);
    isNotRemembered.append(jit.branchTestPtr(CCallHelpers::Zero, record));
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(classes.instancesBelowPrototypes.maskOfOffsets)), mask);
    static_assert(firstOffsetOfInstanceBelowPrototype(16, 3) == (19 * multiplierOfInstanceBelowPrototype) >> shiftOfProductOfInstanceBelowPrototype);
    jit.zeroExtend32ToWord(allocation, scratch);
    jit.add64(prototype, scratch);
    jit.move(TrustedImm64(static_cast<int64_t>(multiplierOfInstanceBelowPrototype)), cell);
    jit.mul64(cell, scratch);
    jit.urshift64(TrustedImm32(shiftOfProductOfInstanceBelowPrototype), scratch);
    Label looksAtEntry = jit.label();
    jit.and64(mask, scratch);
    jit.load64(BaseIndex(record, scratch, CCallHelpers::TimesOne, OBJECT_OFFSETOF(InstanceBelowPrototype, prototype)), cell);
    isNotRemembered.append(jit.branchTestPtr(CCallHelpers::Zero, cell));
    Jump isOfOtherPrototype = jit.branchPtr(CCallHelpers::NotEqual, cell, prototype);
    jit.load32(BaseIndex(record, scratch, CCallHelpers::TimesOne, OBJECT_OFFSETOF(InstanceBelowPrototype, allocationOfSite)), cell);
    Jump isRemembered = jit.branch32(CCallHelpers::Equal, cell, allocation);
    isOfOtherPrototype.link(&jit);
    jit.add64(TrustedImm32(sizeof(InstanceBelowPrototype)), scratch);
    jit.jump().linkTo(looksAtEntry, &jit);

    isRemembered.link(&jit);
    jit.add64(scratch, record);
    jit.loadPtr(Address(record, OBJECT_OFFSETOF(InstanceBelowPrototype, allocator)), mask);
    allocateCell(jit, cell, mask, scratch, isNotRemembered);
    jit.load32(Address(mask, LocalAllocator::offsetOfCellSize()), scratch);
    jit.loadPtr(Address(record, OBJECT_OFFSETOF(InstanceBelowPrototype, words)), mask);
    Label copiesWord = jit.label();
    jit.sub64(TrustedImm32(sizeof(uint64_t)), scratch);
    jit.load64(BaseIndex(mask, scratch, CCallHelpers::TimesOne), allocation);
    jit.store64(allocation, BaseIndex(cell, scratch, CCallHelpers::TimesOne));
    jit.branchTest64(CCallHelpers::NonZero, scratch).linkTo(copiesWord, &jit);
    jit.move(TrustedImm32(0), record);
    jit.returnToCaller();

    isNotRemembered.link(&jit);

    const GPRReg setsPrototype = scratch;
    static_assert(areDistinctOnEveryISA(StubRegister::T9, StubRegister::T13, StubRegister::A0, StubRegister::A1, StubRegister::A3, StubRegister::A4));
    RELEASE_ASSERT(allocation == gprOf(StubRegister::T9) && setsPrototype == gprOf(StubRegister::T13));
    jit.move(TrustedImm32(0), record);
    jit.move(TrustedImm32(1), setsPrototype);
    Jump isReady = jit.jump();
    allocatesSlowly.link(&jit);
    jit.move(TrustedImm32(0), setsPrototype);
    isReady.link(&jit);
    jit.preservingCallerSavedRegisters(true, [&](unsigned) {
        jit.move(instanceGPR, operationArgumentGPR(0));
        Jump namesPrototype = jit.branchTest32(CCallHelpers::NonZero, setsPrototype);
        jit.move(allocation, operationArgumentGPR(1));
        jit.callOperation(Entry::operationAOTAllocateSlow, jit.reuseScratch(StubRegister::Operation));
        Jump isMade = jit.jump();
        namesPrototype.link(&jit);
        jit.move(allocation, operationArgumentGPR(3));
        jit.move(TrustedImm32(0), operationArgumentGPR(1));
        jit.move(TrustedImm32(1), operationArgumentGPR(4));
        jit.callOperation(Entry::operationAOTSetConstructPrototype, jit.reuseScratch(StubRegister::Operation));
        isMade.link(&jit);
    });
    jit.pollBeforeReturningFromNativeCode();
    jit.returnToCaller();
}

StubSignature signatureOfAllocateExtension(StubKey)
{
    return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(StubRegister::T9), gprOf(StubRegister::A2) });
}

void generateAllocateExtension(StubAssembler& jit)
{
    const unsigned size = jit.key().count() * copyChunkBytes;
    const GPRReg owner = jit.operand(0);
    const GPRReg wordOffset = jit.operand(1);
    const GPRReg granuleBits = jit.clobberOperand(2);
    const GPRReg storage = jit.allocateScratch(StubRegister::R0);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T12);
    const GPRReg temporary = jit.allocateScratch(StubRegister::T11);
    const BaseIndex extensionWord(owner, wordOffset, CCallHelpers::TimesOne);
    const bool isNarrow = traitsOf(hiddenReferenceRepresentation(jit.options().useWideReferences)).isNarrowReference;
    JumpList slowPath;
    Jump hasNone;
    if (isNarrow) {
        jit.load32(extensionWord, storage);
        hasNone = jit.branchTest32(CCallHelpers::Zero, storage);
        decodeCell32(jit, storage, storage);
        jit.addPtr(TrustedImm32(offsetOfEmptyExtensionInCage), storage);
    } else {
        jit.load64(extensionWord, storage);
        hasNone = jit.branchTest64(CCallHelpers::Zero, storage);
    }
    jit.load64(Address(storage, offsetOfExtensionGranuleBits), temporary);
    jit.not64(temporary);
    jit.and64(temporary, granuleBits);
    appendGranule(jit, storage, granuleBits, temporary, allocator, slowPath);
    jit.returnToCaller();
    hasNone.link(&jit);
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
    return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

static constexpr uint64_t extensionUnitsByNumberOfGranules = [] {
    uint64_t units = 0;
    for (unsigned count = 1; count <= maxGranulesOfExtensionFromStub; ++count)
        units |= static_cast<uint64_t>(extensionCellSizeFor(count) / minExtensionSize) << (4 * count);
    return units;
}();
static_assert(extensionCellSizeFor(maxGranulesOfExtensionFromStub) / minExtensionSize < 16 && 4 * maxGranulesOfExtensionFromStub < 64);
static_assert(minExtensionSize == MarkedSpace::sizeStep && minExtensionSize == 2 * sizeof(uint64_t) && MarkedSpace::sizeClassToIndex(minExtensionSize) == 1);

void loadUnitsOfExtensionWith(StubAssembler& jit, GPRReg granuleBits, GPRReg units, GPRReg scratch)
{
    jit.countPopulation64(granuleBits, scratch, CCallHelpers::fpTempRegister);
    jit.lshift32(TrustedImm32(2), scratch);
    jit.move(TrustedImm64(static_cast<int64_t>(extensionUnitsByNumberOfGranules)), units);
    jit.urshift64(scratch, units);
    jit.and32(TrustedImm32(15), units);
}

static void loadGranulesOfFirstExtension(StubAssembler& jit, GPRReg object, GPRReg result)
{
    RELEASE_ASSERT(object != result);
    static_assert(LayoutData::prefixSize(false, false, true) == sizeof(SlotData) && !LayoutData::prefixSize(false, false, false) && std::endian::native == std::endian::little);
    LayoutData withSlotOfExtraProperties { };
    withSlotOfExtraProperties.hasExtraPropertiesSlot = true;
    loadStructure(jit, object, result);
    jit.loadPtr(Address(result, CompiledStructure::offsetOfLayout()), result);
    Jump hasNoSuchSlot = jit.branchTest32(CCallHelpers::Zero, Address(result), TrustedImm32(static_cast<int32_t>(withSlotOfExtraProperties.firstWord())));
    jit.subPtr(TrustedImm32(sizeof(SlotData)), result);
    hasNoSuchSlot.link(&jit);
    jit.load64(Address(result, -static_cast<int32_t>(sizeof(uint64_t))), result);
}

void allocateFirstExtensionWithOneGranule(StubAssembler& jit, GPRReg object, GPRReg granuleBit, GPRReg extensionAndGranule, GPRReg scratch, GPRReg secondScratch, Label hasGranule, JumpList& isLeftToStub)
{
    RELEASE_ASSERT(!jit.options().useWideReferences && noOverlap(object, granuleBit, extensionAndGranule, scratch, secondScratch));
    static_assert(extensionCellSizeFor(1) == offsetOfFirstExtensionSlot + extensionGranuleSize);
    Jump holdsGranules = jit.branchTest64(CCallHelpers::NonZero, Address(extensionAndGranule, offsetOfExtensionGranuleBits));
    loadGranulesOfFirstExtension(jit, object, scratch);
    jit.or64(granuleBit, scratch);
    isLeftToStub.append(jit.branch64(CCallHelpers::NotEqual, scratch, granuleBit));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cagedAuxiliarySpace)), scratch);
    jit.loadPtr(Address(scratch, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(extensionCellSizeFor(1)) * sizeof(Allocator)), scratch);
    isLeftToStub.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    const AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, extensionAndGranule, scratch, secondScratch, isLeftToStub, extensionCellSizeFor(1));
    jit.store64(granuleBit, Address(extensionAndGranule, offsetOfExtensionGranuleBits));
    jit.store64(TrustedImm32(0), Address(extensionAndGranule, offsetOfFirstExtensionSlot));
    fenceAfterAllocation(jit);
    loadExtensionWordOffset(jit, object, scratch);
    encodeCell32(jit, extensionAndGranule, secondScratch);
    jit.sub32(TrustedImm32(offsetOfEmptyExtensionInCage >> cell32Shift), secondScratch);
    jit.store32(secondScratch, BaseIndex(object, scratch, CCallHelpers::TimesOne));
    jit.addPtr(TrustedImm32(offsetOfFirstExtensionSlot), extensionAndGranule);
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, object, scratch);
    jit.jump().linkTo(hasGranule, &jit);

    needsBarrier.link(&jit);
    checkStubOperands(writeBarrier(), { object });
    RELEASE_ASSERT(scratch == gprOf(StubRegister::T9));
    jit.emitFunctionPrologue();
    jit.callStub(writeBarrier());
    jit.emitFunctionEpilogue();
    jit.jump().linkTo(hasGranule, &jit);

    emitLateRefill(jit, refill, isLeftToStub);
    holdsGranules.link(&jit);
}

static void growExtensionAndReturn(StubAssembler& jit, GPRReg owner, GPRReg granuleBits, GPRReg replacement, JumpList& givesUp)
{
    const GPRReg allocator = jit.allocateScratch(StubRegister::T12);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const GPRReg from = gprOf(StubRegister::T9);
    const GPRReg held = gprOf(StubRegister::T10);
    const GPRReg remaining = gprOf(StubRegister::T14);
    const GPRReg to = allocator;
    RELEASE_ASSERT(noOverlap(owner, granuleBits, replacement, allocator, scratch, from, held, remaining));

    jit.preserving({ from, held, remaining }, [&] {
        JumpList cannot;
        JumpList isDone;
        auto publish = [&](GPRReg wordOffset, GPRReg word) {
            fenceAfterAllocation(jit);
            encodeCell32(jit, replacement, word);
            jit.sub32(TrustedImm32(offsetOfEmptyExtensionInCage >> cell32Shift), word);
            jit.store32(word, BaseIndex(owner, wordOffset, CCallHelpers::TimesOne));
            isDone.append(jit.jump());
        };
        auto loadAllocatorOfCells = [&](unsigned size) {
            jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cagedAuxiliarySpace)), allocator);
            jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(size) * sizeof(Allocator)), allocator);
            cannot.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
        };
        loadExtensionWordOffset(jit, owner, from);
        jit.load32(BaseIndex(owner, from, CCallHelpers::TimesOne), held);
        Jump hasNone = jit.branchTest32(CCallHelpers::Zero, held);
        decodeExtension(jit, held, from);
        jit.load64(Address(from, offsetOfExtensionGranuleBits), held);
        jit.move(held, remaining);
        jit.or64(granuleBits, remaining);
        Jump hasOne = jit.jump();
        hasNone.link(&jit);
        loadGranulesOfFirstExtension(jit, owner, remaining);
        jit.or64(granuleBits, remaining);
        jit.countPopulation64(remaining, scratch, CCallHelpers::fpTempRegister);
        Jump getsSeveral = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(1));
        static_assert(extensionCellSizeFor(1) == offsetOfFirstExtensionSlot + extensionGranuleSize);
        loadAllocatorOfCells(extensionCellSizeFor(1));
        allocateCell(jit, replacement, allocator, scratch, cannot);
        jit.store64(remaining, Address(replacement, offsetOfExtensionGranuleBits));
        jit.store64(TrustedImm32(0), Address(replacement, offsetOfFirstExtensionSlot));
        publish(from, scratch);
        getsSeveral.link(&jit);
        hasOne.link(&jit);
        jit.countPopulation64(remaining, scratch, CCallHelpers::fpTempRegister);
        cannot.append(jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(maxGranulesOfExtensionFromStub)));

        JumpList isFilled;
        {
            static_assert(extensionCapacityFor(1) == 1 && extensionCapacityFor(2) == 3 && extensionCellSizeFor(2) == offsetOfFirstExtensionSlot + 3 * extensionGranuleSize);
            JumpList isOtherGrowth;
            isOtherGrowth.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(2)));
            isOtherGrowth.append(jit.branchTest64(CCallHelpers::Zero, held));
            jit.move(held, scratch);
            jit.sub64(TrustedImm32(1), scratch);
            isOtherGrowth.append(jit.branchTest64(CCallHelpers::NonZero, scratch, held));
            loadAllocatorOfCells(extensionCellSizeFor(2));
            allocateCell(jit, replacement, allocator, scratch, cannot);
            jit.store64(remaining, Address(replacement, offsetOfExtensionGranuleBits));
            jit.load64(Address(from, offsetOfFirstExtensionSlot), scratch);
            jit.xor64(held, remaining);
            Jump isAboveHeld = jit.branch64(CCallHelpers::Above, remaining, held);
            jit.store64(TrustedImm32(0), Address(replacement, offsetOfFirstExtensionSlot));
            jit.store64(scratch, Address(replacement, offsetOfFirstExtensionSlot + extensionGranuleSize));
            Jump isPlaced = jit.jump();
            isAboveHeld.link(&jit);
            jit.store64(scratch, Address(replacement, offsetOfFirstExtensionSlot));
            jit.store64(TrustedImm32(0), Address(replacement, offsetOfFirstExtensionSlot + extensionGranuleSize));
            isPlaced.link(&jit);
            jit.store64(TrustedImm32(0), Address(replacement, offsetOfFirstExtensionSlot + 2 * extensionGranuleSize));
            isFilled.append(jit.jump());
            isOtherGrowth.link(&jit);
        }
        loadUnitsOfExtensionWith(jit, remaining, allocator, scratch);
        loadUnitsOfExtensionWith(jit, held, replacement, scratch);
        Jump hasNoRoom = jit.branch32(CCallHelpers::NotEqual, allocator, replacement);

        jit.move(from, replacement);
        jit.xor64(held, remaining, scratch);
        jit.move(scratch, allocator);
        jit.neg64(allocator);
        jit.and64(scratch, allocator);
        Jump isAmongHeld = jit.branch64(CCallHelpers::Below, allocator, held);
        jit.store64(remaining, Address(from, offsetOfExtensionGranuleBits));
        isDone.append(jit.jump());

        isAmongHeld.link(&jit);
        Jump collectorMayVisit = jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced)));
        {
            const GPRReg missing = remaining;
            const GPRReg word = allocator;
            const GPRReg place = replacement;
            const GPRReg last = scratch;
            jit.xor64(held, missing);
            Label inserts = jit.label();
            jit.move(missing, word);
            jit.neg64(word);
            jit.and64(missing, word);
            jit.move(word, place);
            jit.sub64(TrustedImm32(1), place);
            jit.and64(held, place);
            jit.countPopulation64(place, place, CCallHelpers::fpTempRegister);
            jit.countPopulation64(held, last, CCallHelpers::fpTempRegister);
            jit.or64(word, held);
            jit.getEffectiveAddress(BaseIndex(from, place, CCallHelpers::TimesEight, offsetOfFirstExtensionSlot), place);
            jit.getEffectiveAddress(BaseIndex(from, last, CCallHelpers::TimesEight, offsetOfFirstExtensionSlot), last);
            Jump isBehindAll = jit.branchPtr(CCallHelpers::Equal, place, last);
            Label moves = jit.label();
            jit.load64(Address(last, -static_cast<int32_t>(sizeof(uint64_t))), word);
            jit.store64(word, Address(last));
            jit.subPtr(TrustedImm32(sizeof(uint64_t)), last);
            jit.branchPtr(CCallHelpers::NotEqual, place, last).linkTo(moves, &jit);
            jit.store64(TrustedImm32(0), Address(place));
            isBehindAll.link(&jit);
            jit.move(missing, word);
            jit.sub64(TrustedImm32(1), word);
            jit.and64(word, missing);
            jit.branchTest64(CCallHelpers::NonZero, missing).linkTo(inserts, &jit);
            jit.store64(held, Address(from, offsetOfExtensionGranuleBits));
            jit.move(from, replacement);
            isDone.append(jit.jump());
        }

        collectorMayVisit.link(&jit);
        loadUnitsOfExtensionWith(jit, remaining, allocator, scratch);
        hasNoRoom.link(&jit);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cagedAuxiliarySpace)), scratch);
        jit.loadPtr(BaseIndex(scratch, allocator, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
        cannot.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
        allocateCell(jit, replacement, allocator, scratch, cannot);
        jit.store64(remaining, Address(replacement, offsetOfExtensionGranuleBits));

        const GPRReg end = scratch;
        jit.load32(Address(allocator, LocalAllocator::offsetOfCellSize()), end);
        jit.addPtr(replacement, end);
        jit.xor64(held, remaining);
        jit.move(remaining, to);
        jit.neg64(to);
        jit.and64(remaining, to);
        Jump liesAmongHeld = jit.branch64(CCallHelpers::BelowOrEqual, to, held);
        {
            const GPRReg endOfHeld = held;
            const GPRReg word = remaining;
            jit.addPtr(TrustedImm32(offsetOfFirstExtensionSlot), replacement, to);
            jit.countPopulation64(held, held, CCallHelpers::fpTempRegister);
            jit.addPtr(TrustedImm32(offsetOfFirstExtensionSlot), from);
            jit.getEffectiveAddress(BaseIndex(from, held, CCallHelpers::TimesEight), endOfHeld);
            Jump asksFirst = jit.jump();
            Label copies = jit.label();
            jit.load64(Address(from), word);
            jit.store64(word, Address(to));
            jit.addPtr(TrustedImm32(sizeof(uint64_t)), from);
            jit.addPtr(TrustedImm32(sizeof(uint64_t)), to);
            asksFirst.link(&jit);
            jit.branchPtr(CCallHelpers::Below, from, endOfHeld).linkTo(copies, &jit);
            Label clearsRest = jit.label();
            jit.store64(TrustedImm32(0), Address(to));
            jit.addPtr(TrustedImm32(sizeof(uint64_t)), to);
            jit.branchPtr(CCallHelpers::Below, to, end).linkTo(clearsRest, &jit);
            isFilled.append(jit.jump());
        }

        liesAmongHeld.link(&jit);
        jit.or64(held, remaining);
        jit.addPtr(TrustedImm32(offsetOfFirstExtensionSlot), replacement, to);
        Label clears = jit.label();
        jit.store64(TrustedImm32(0), Address(to));
        jit.addPtr(TrustedImm32(sizeof(uint64_t)), to);
        jit.branchPtr(CCallHelpers::Below, to, end).linkTo(clears, &jit);

        jit.addPtr(TrustedImm32(offsetOfFirstExtensionSlot), from);
        jit.addPtr(TrustedImm32(offsetOfFirstExtensionSlot), replacement, to);
        Label places = jit.label();
        jit.move(remaining, scratch);
        jit.neg64(scratch);
        jit.and64(remaining, scratch);
        Jump isNew = jit.branchTest64(CCallHelpers::Zero, held, scratch);
        jit.load64(Address(from), scratch);
        jit.store64(scratch, Address(to));
        jit.addPtr(TrustedImm32(sizeof(uint64_t)), from);
        isNew.link(&jit);
        jit.addPtr(TrustedImm32(sizeof(uint64_t)), to);
        jit.move(remaining, scratch);
        jit.sub64(TrustedImm32(1), scratch);
        jit.and64(scratch, remaining);
        jit.branchTest64(CCallHelpers::NonZero, remaining).linkTo(places, &jit);

        isFilled.link(&jit);
        loadExtensionWordOffset(jit, owner, scratch);
        publish(scratch, to);

        cannot.link(&jit);
        jit.move(TrustedImm32(0), replacement);
        isDone.link(&jit);
    });
    givesUp.append(jit.branchTestPtr(CCallHelpers::Zero, replacement));
    jit.returnToCaller();
}

void generateAllocateExtensionSlow(StubAssembler& jit)
{
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1));
    const bool isNarrow = traitsOf(hiddenReferenceRepresentation(jit.options().useWideReferences)).isNarrowReference;
    const GPRReg owner = jit.operand(0);
    const GPRReg extension = jit.allocateScratch(StubRegister::R0);
    RELEASE_ASSERT(extension == extensionMadeBySlowStub);
    if (isNarrow) {
        JumpList givesUp;
        growExtensionAndReturn(jit, owner, jit.operand(1), extension, givesUp);
        givesUp.link(&jit);
    }
    jit.callOperationAndReturn(Entry::operationAOTEnsureExtensionGranules, { .returns = Returns::Void, .saves = Saves::CallerSavedRegisters, .checksException = false, .thirdArgumentIn = jit.operand(1) }, [&] {
        loadExtensionWordOffset(jit, owner, extension);
        if (isNarrow) {
            jit.load32(BaseIndex(owner, extension, CCallHelpers::TimesOne), extension);
            decodeExtension(jit, extension, extension);
        } else
            jit.load64(BaseIndex(owner, extension, CCallHelpers::TimesOne), extension);
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
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1));
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
    jit.callOperationAndReturn(Entry::operationAOTNewTypedArray, { });
}

StubSignature signatureOfNewDictionary(StubKey)
{
    return StubSignature::withGPRResult({ });
}

void generateNewDictionary(StubAssembler& jit)
{
    constexpr unsigned kind = static_cast<unsigned>(InlineAllocation::DefaultFinalObject);
    const size_t numberOfWords = JSFinalObject::allocationSize(JSFinalObject::defaultInlineCapacity) / sizeof(uint64_t);
    const GPRReg result = jit.result();
    const GPRReg allocator = jit.allocateScratch(StubRegister::T12);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    JumpList slowCases;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].allocator)), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, result, allocator, scratch, slowCases);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].header)), scratch);
    jit.store64(scratch, Address(result));
    for (size_t word = 1; word < numberOfWords; ++word)
        jit.store64(TrustedImm32(0), Address(result, word * sizeof(uint64_t)));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNewDictionary, {
        .saves = Saves::CallerSavedRegisters,
        .checksException = false,
    });
}

void emitStoreValuesAndClear(StubAssembler& jit, unsigned firstValueArgument, unsigned count, unsigned end, GPRReg base, size_t offset)
{
    auto at = [&](unsigned i) { return static_cast<int32_t>(offset + i * sizeof(EncodedJSValue)); };
#if CPU(ARM64)
    auto word = [&](unsigned i) { return i < count ? GPRInfo::toArgumentRegister(firstValueArgument + i) : ARM64Registers::zr; };
    unsigned i = 0;
    for (; i + 1 < end; i += 2)
        jit.storePair64(word(i), word(i + 1), base, TrustedImm32(at(i)));
    if (i < end)
        jit.store64(word(i), Address(base, at(i)));
#else
    for (unsigned i = 0; i < count; ++i)
        jit.store64(GPRInfo::toArgumentRegister(firstValueArgument + i), Address(base, at(i)));
    for (unsigned i = count; i < end; ++i)
        jit.store64(TrustedImm32(0), Address(base, at(i)));
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
