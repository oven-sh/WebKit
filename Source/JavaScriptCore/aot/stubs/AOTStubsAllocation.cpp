/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsAllocation.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTRuntimeTable.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsField.h"
#include "AOTStubsObjectCreation.h"
#include "CompleteSubspace.h"
#include "JSArrayBufferView.h"
#include "JSObject.h"
#include "LocalAllocator.h"
#include "MarkedSpace.h"
#include <optional>
#include <type_traits>
#include <utility>
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
static void loadChunk(StubAssembler& jit, GPRReg base, int32_t offset, std::optional<GPRReg> index = std::nullopt)
{
    constexpr FPRReg chunk = CCallHelpers::fpTempRegister;
    if (jit.supportsAVX())
        index ? jit.m_assembler.vmovups_mr(offset, base, *index, CCallHelpers::TimesOne, chunk) : jit.m_assembler.vmovups_mr(offset, base, chunk);
    else
        index ? jit.m_assembler.movups_mr(offset, base, *index, CCallHelpers::TimesOne, chunk) : jit.m_assembler.movups_mr(offset, base, chunk);
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

// Only ARM64 uses the scratches.
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
    return { jit.result(), jit.operand(0), jit.allocateScratch(StubRegister::T11), jit.allocateScratch(StubRegister::T12), jit.allocateScratch(StubRegister::T13) };
}

[[nodiscard]] static AssemblyHelpers::FreeListRefill allocateAtSite(StubAssembler& jit, const AllocationSiteRegisters& registers, JumpList& slowPath, std::optional<unsigned> cellSize = std::nullopt)
{
    const auto [cell, allocation, record, allocator, scratch] = registers;
    countCellOfAllocation(jit, allocation, record);
    loadAllocationSiteRecord(jit, allocation, record, allocator);
    jit.loadPtr(Address(record, AllocationSiteRecord::offsetOfAllocator()), allocator);
    return allocateCellWithLateRefill(jit, cell, allocator, scratch, slowPath, cellSize, CountedUnder::Allocation);
}

// The first chunk is left to whoever stores the header.
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
    const std::optional<unsigned> cellSize = steps ? std::optional<unsigned>(steps * cellSizeStep) : std::nullopt;
    const AssemblyHelpers::FreeListRefill refill = allocateAtSite(jit, registers, slowPath, cellSize);
    const auto initialization = static_cast<CellInitialization>(jit.key().form());
    if (zeroesBody(initialization))
        zeroBehindFirstChunk(jit, registers, cellSize);
    if (!storesWords(initialization)) {
        jit.load64(Address(record, AllocationSiteRecord::offsetOfHeader()), scratch);
        jit.store64(scratch, Address(cell));
        if (zeroesBody(initialization))
            jit.store64(TrustedImm32(0), Address(cell, sizeof(JSCell)));
    } else
        copyChunk(jit, Address(record, AllocationSiteRecord::offsetOfHeader()), Address(cell), allocator, scratch);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowPath);
    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTAllocateSlow, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = allocation });
}

StubSignature signatureOfAllocateInstance(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::T9), gprOf(StubRegister::A2) });
    signature.result = StubResult::TwoGPRs;
    signature.secondResult = gprOf(StubRegister::T11);
    return signature;
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
    countCellOfAllocation(jit, allocation, record);
    loadAllocationSiteRecord(jit, allocation, record, allocator);
    isOutOfLine.append(jit.branchPtr(CCallHelpers::NotEqual, Address(record, AllocationSiteRecord::offsetOfPrototype()), prototype));
    jit.loadPtr(Address(record, AllocationSiteRecord::offsetOfAllocator()), allocator);
    const AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, cell, allocator, scratch, isOutOfLine, cellSize, CountedUnder::Allocation);
    zeroBehindFirstChunk(jit, site, cellSize);
    copyChunk(jit, Address(record, AllocationSiteRecord::offsetOfHeader()), Address(cell), allocator, scratch);
    jit.returnToCaller();

    emitLateRefill(jit, refill, isOutOfLine);
    isOutOfLine.link(&jit);
    jit.jumpToStub(StubKey(StubFamily::AllocateInstanceOutOfLine));
}

// The third operand is the record of the allocation.
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
    const GPRReg allocator = jit.allocateScratch(StubRegister::T12);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(record == jit.secondResult() && noOverlap(cell, allocation, prototype, record, allocator, scratch, instanceGPR));
    static_assert(operationArgumentGPR(2) == gprOf(StubRegister::A2));

    // The allocator of the record had no room. The record, which is not zero, stays in the register of the second result.
    JumpList allocatesSlowly;
    allocatesSlowly.append(jit.branchPtr(CCallHelpers::Equal, Address(record, AllocationSiteRecord::offsetOfPrototype()), prototype));

    const GPRReg instance = record;
    const GPRReg cellAtBirth = allocator;
    const GPRReg offset = scratch;
    JumpList isNotAtFirstPlace;
    JumpList isNotRemembered;
    JumpList hasNoRoom;
    static_assert(sizeof(InstanceOfClassBelow) == 1 << shiftOfPrototypeForInstanceOfClassBelow);
    constexpr int32_t bitsOfPlace = (numberOfInstancesOfClassesBelow - 1) << shiftOfPrototypeForInstanceOfClassBelow;
    auto branchIfNotAtPlace = [&](JumpList& isElsewhere) {
        jit.and32(TrustedImm32(bitsOfPlace), instance);
        jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(classes.instancesOfClassesBelow)), instance);
        isElsewhere.append(jit.branchPtr(CCallHelpers::NotEqual, Address(instance, OBJECT_OFFSETOF(InstanceOfClassBelow, prototype)), prototype));
        isElsewhere.append(jit.branch32(CCallHelpers::NotEqual, Address(instance, OBJECT_OFFSETOF(InstanceOfClassBelow, allocation)), allocation));
    };
    jit.move(prototype, instance);
    branchIfNotAtPlace(isNotAtFirstPlace);
    Label isRemembered = jit.label();
    static_assert(MarkedSpace::sizeStep == 2 * sizeof(Allocator) && MarkedSpace::sizeStep == copyChunkBytes);
    jit.load32(Address(instance, OBJECT_OFFSETOF(InstanceOfClassBelow, offsetOfAllocator)), offset);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, offset, CCallHelpers::TimesOne, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    const AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, cell, allocator, scratch, hasNoRoom, std::nullopt, CountedUnder::Allocation);
    jit.load32(Address(instance, OBJECT_OFFSETOF(InstanceOfClassBelow, offsetOfAllocator)), offset);
    jit.loadPtr(Address(instance, OBJECT_OFFSETOF(InstanceOfClassBelow, cellAtBirth)), cellAtBirth);
    jit.getEffectiveAddress(BaseIndex(offset, offset, CCallHelpers::TimesOne, -static_cast<int32_t>(copyChunkBytes)), offset);
#if CPU(ARM64)
    const GPRReg firstWord = allocation;
    const GPRReg secondWord = jit.allocateScratch(StubRegister::T14);
    const GPRReg address = jit.allocateScratch(StubRegister::T15);
#endif
    Label copyNextChunk = jit.label();
#if CPU(X86_64)
    loadChunk(jit, cellAtBirth, 0, offset);
    storeChunk(jit, cell, 0, offset);
#else
    jit.add64(cellAtBirth, offset, address);
    jit.loadPair64(address, TrustedImm32(0), firstWord, secondWord);
    jit.add64(cell, offset, address);
    jit.storePair64(firstWord, secondWord, address, TrustedImm32(0));
#endif
    jit.branchSub64(CCallHelpers::PositiveOrZero, TrustedImm32(copyChunkBytes), offset).linkTo(copyNextChunk, &jit);
    jit.load64(Address(instance, OBJECT_OFFSETOF(InstanceOfClassBelow, header)), offset);
    jit.store64(offset, Address(cell));
    jit.move(TrustedImm32(0), record);
    jit.returnToCaller();

    // secondPlaceOfInstanceOfClassBelow(), in bytes.
    isNotAtFirstPlace.link(&jit);
    jit.move(prototype, instance);
    jit.urshift32(TrustedImm32(shiftOfPrototypeForInstanceOfClassBelow), instance);
    jit.or32(TrustedImm32(1 << shiftOfPrototypeForInstanceOfClassBelow), instance);
    jit.xor32(prototype, instance);
    branchIfNotAtPlace(isNotRemembered);
    jit.jump().linkTo(isRemembered, &jit);

    // What is saved around the operation comes back, the second result with it.
    // On ARM64 the first result and the first argument are one register: what is read while the arguments are laid out is in neither.
    const GPRReg setsPrototype = scratch;
    static_assert(areDistinctOnEveryISA(StubRegister::T9, StubRegister::T13, StubRegister::A0, StubRegister::A1, StubRegister::A3, StubRegister::A4));
    RELEASE_ASSERT(allocation == gprOf(StubRegister::T9) && setsPrototype == gprOf(StubRegister::T13));
    emitLateRefill(jit, refill, hasNoRoom);
    hasNoRoom.link(&jit);
    jit.move(instance, allocation);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(classes.instancesOfClassesBelow)), scratch);
    jit.subPtr(scratch, allocation);
    jit.urshift32(TrustedImm32(shiftOfPrototypeForInstanceOfClassBelow), allocation);
    jit.or32(TrustedImm32(static_cast<int32_t>(allocatesInstanceOfClassBelow)), allocation);
    jit.move(TrustedImm32(0), record);
    allocatesSlowly.link(&jit);
    jit.move(TrustedImm32(0), setsPrototype);
    Jump isReady = jit.jump();
    isNotRemembered.link(&jit);
    jit.move(TrustedImm32(0), record);
    jit.move(TrustedImm32(1), setsPrototype);
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
    jit.returnToCaller();
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
    const auto [cell, allocation, header, firstScratch, secondScratch] = site;
    const GPRReg source = jit.operand(1);
    const GPRReg endOffset = copiedFieldBytes ? jit.allocateScratch(copyEndOffsetRegister) : jit.clobberOperand(2);
#if CPU(ARM64)
    std::optional<std::pair<FPRReg, FPRReg>> loopVectors;
    if (!copiedFieldBytes)
        loopVectors = std::pair<FPRReg, FPRReg> { jit.allocateScratch(ARM64Registers::q30), jit.allocateScratch(ARM64Registers::q31) };
#endif

    // The loop moves two chunks at a time, from the last field down. With an odd number of chunks it also copies the source's header and words.
    auto initializeCopyAndReturn = [&] {
        for (unsigned offset = copyChunkBytes; offset <= copiedFieldBytes; offset += copyChunkBytes)
            copyChunk(jit, Address(source, offset), Address(cell, offset), firstScratch, secondScratch);
        if (!copiedFieldBytes) {
#if CPU(ARM64)
            const GPRReg from = firstScratch;
            const GPRReg to = secondScratch;
            RELEASE_ASSERT(loopVectors);
            jit.add64(source, endOffset, from);
            jit.add64(cell, endOffset, to);
            Label loop = jit.label();
            jit.loadPairVector(CCallHelpers::PostIndexAddress(from, -2 * static_cast<int>(copyChunkBytes)), loopVectors->first, loopVectors->second);
            jit.storePairVector(loopVectors->first, loopVectors->second, CCallHelpers::PostIndexAddress(to, -2 * static_cast<int>(copyChunkBytes)));
#else
            Label loop = jit.label();
            loadChunk(jit, source, 0, endOffset);
            storeChunk(jit, cell, 0, endOffset);
            loadChunk(jit, source, copyChunkBytes, endOffset);
            storeChunk(jit, cell, copyChunkBytes, endOffset);
#endif
            jit.branchSub64(CCallHelpers::PositiveOrZero, TrustedImm32(2 * copyChunkBytes), endOffset).linkTo(loop, &jit);
        }
        const GPRReg words = secondScratch;
        jit.load64(Address(source, offsetOfOrderWord), words);
        jit.and64(TrustedImm64(static_cast<int64_t>(orderCountAndShapeMask)), words);
        jit.storePair64(header, words, cell, TrustedImm32(0));
        fenceAfterAllocation(jit);
        jit.returnToCaller();
    };

    JumpList slowPath;
    const AssemblyHelpers::FreeListRefill refill = allocateAtSite(jit, site, slowPath);
    jit.load64(Address(site.record, AllocationSiteRecord::offsetOfHeader()), header);
    initializeCopyAndReturn();

    emitLateRefill(jit, refill, slowPath);
    slowPath.link(&jit);
    if (copiedFieldBytes) {
        jit.move(TrustedImm32(copiedFieldBytes - copyChunkBytes), endOffset);
        jit.jumpToStub(jit.key().withCount(0));
        return;
    }
    jit.preservingCallerSavedRegisters(true, [&](unsigned) {
        jit.move(allocation, operationArgumentGPR(1));
        jit.move(instanceGPR, operationArgumentGPR(0));
        jit.callOperation(Entry::operationAOTAllocateSlow, jit.reuseScratch(StubRegister::Operation));
    });
    jit.load64(Address(cell), header);
    initializeCopyAndReturn();
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
    const GPRReg granuleBits = jit.operand(2);
    const GPRReg storage = jit.allocateScratch(StubRegister::R0);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T12);
    const GPRReg temporary = jit.allocateScratch(StubRegister::T11);
    const BaseIndex extensionWord(owner, wordOffset, CCallHelpers::TimesOne);
    const bool isNarrow = traitsOf(hiddenReferenceRepresentation(jit.options().useWideReferences)).isNarrowReference;
    JumpList slowPath;
    // An Extension that lacks the granules gets one in the room it has, or the operation replaces it.
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
    appendGranule(jit, storage, granuleBits, temporary, allocator, slowPath);
    jit.returnToCaller();
    hasNone.link(&jit);
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
    return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

// Sixteen bytes are a unit. An entry is four bits wide.
static constexpr unsigned maxGranulesOfExtensionReplacedByStub = 13;
static constexpr uint64_t extensionUnitsByNumberOfGranules = [] {
    uint64_t units = 0;
    for (unsigned count = 1; count <= maxGranulesOfExtensionReplacedByStub; ++count)
        units |= static_cast<uint64_t>(extensionCellSizeFor(count) / minExtensionSize) << (4 * count);
    return units;
}();
static_assert(extensionCellSizeFor(maxGranulesOfExtensionReplacedByStub) / minExtensionSize < 16 && 4 * maxGranulesOfExtensionReplacedByStub < 64);
static_assert(minExtensionSize == MarkedSpace::sizeStep && minExtensionSize == 2 * sizeof(uint64_t) && MarkedSpace::sizeClassToIndex(minExtensionSize) == 1);

static void loadUnitsOfExtensionWith(StubAssembler& jit, GPRReg granuleBits, GPRReg units, GPRReg scratch)
{
    jit.countPopulation64(granuleBits, scratch, CCallHelpers::fpTempRegister);
    jit.lshift32(TrustedImm32(2), scratch);
    jit.move(TrustedImm64(static_cast<int64_t>(extensionUnitsByNumberOfGranules)), units);
    jit.urshift64(scratch, units);
    jit.and32(TrustedImm32(15), units);
}

// As Object::ensureExtensionGranules().
static void growExtensionAndReturn(StubAssembler& jit, GPRReg owner, GPRReg granuleBits, JumpList& givesUp)
{
    const GPRReg replacement = jit.allocateScratch(StubRegister::R0);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T12);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const GPRReg from = gprOf(StubRegister::T9);
    const GPRReg held = gprOf(StubRegister::T10);
    const GPRReg remaining = gprOf(StubRegister::T14);
    const GPRReg to = allocator;
    RELEASE_ASSERT(noOverlap(owner, granuleBits, replacement, allocator, scratch, from, held, remaining));

    // replacement is zero behind it where the operation is to do it.
    jit.preserving({ from, held, remaining }, [&] {
        JumpList cannot;
        JumpList isDone;
        loadExtensionWordOffset(jit, owner, from);
        jit.load32(BaseIndex(owner, from, CCallHelpers::TimesOne), from);
        cannot.append(jit.branchTest32(CCallHelpers::Zero, from));
        decodeCell32(jit, from, from);
        jit.addPtr(TrustedImm32(offsetOfEmptyExtensionInCage), from);
        jit.load64(Address(from, offsetOfExtensionGranuleBits), held);
        jit.move(held, remaining);
        jit.or64(granuleBits, remaining);
        jit.countPopulation64(remaining, scratch, CCallHelpers::fpTempRegister);
        cannot.append(jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(maxGranulesOfExtensionReplacedByStub)));
        loadUnitsOfExtensionWith(jit, remaining, allocator, scratch);
        loadUnitsOfExtensionWith(jit, held, replacement, scratch);
        Jump hasNoRoom = jit.branch32(CCallHelpers::NotEqual, allocator, replacement);

        // The lowest of the new ones is above all that are held: what is behind those is zero.
        jit.move(from, replacement);
        jit.xor64(held, remaining, scratch);
        jit.move(scratch, allocator);
        jit.neg64(allocator);
        jit.and64(scratch, allocator);
        Jump isAmongHeld = jit.branch64(CCallHelpers::Below, allocator, held);
        jit.store64(remaining, Address(from, offsetOfExtensionGranuleBits));
        isDone.append(jit.jump());

        // As Extension::tryInsert() leaves it. One new granule after the other, the lowest first: those above its place move up by one.
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
        loadUnitsOfExtensionWith(jit, remaining, end, to);
        jit.lshift32(TrustedImm32(4), end);
        jit.addPtr(replacement, end);
        jit.addPtr(TrustedImm32(offsetOfFirstExtensionSlot), replacement, to);
        Label clears = jit.label();
        jit.store64(TrustedImm32(0), Address(to));
        jit.addPtr(TrustedImm32(sizeof(uint64_t)), to);
        jit.branchPtr(CCallHelpers::Below, to, end).linkTo(clears, &jit);

        jit.addPtr(TrustedImm32(offsetOfFirstExtensionSlot), from);
        jit.addPtr(TrustedImm32(offsetOfFirstExtensionSlot), replacement, to);
        Label places = jit.label();
        // The lowest bit that is left.
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

        fenceAfterAllocation(jit);
        loadExtensionWordOffset(jit, owner, scratch);
        encodeCell32(jit, replacement, replacement);
        jit.sub32(TrustedImm32(offsetOfEmptyExtensionInCage >> cell32Shift), replacement);
        jit.store32(replacement, BaseIndex(owner, scratch, CCallHelpers::TimesOne));
        // No Extension lies at the beginning of the cage.
        isDone.append(jit.jump());

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
    if (traitsOf(hiddenReferenceRepresentation(jit.options().useWideReferences)).isNarrowReference) {
        JumpList givesUp;
        growExtensionAndReturn(jit, jit.operand(0), jit.operand(1), givesUp);
        givesUp.link(&jit);
    }
    jit.callOperationAndReturn(Entry::operationAOTEnsureExtensionGranules, { .returns = Returns::Void, .saves = Saves::CallerSavedRegisters, .checksException = false, .thirdArgumentIn = jit.operand(1) });
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
    jit.callOperationAndReturn(Entry::operationAOTNewTypedArray, { });
}

StubSignature signatureOfNewDictionary(StubKey)
{
    return StubSignature::withGPRResult({ });
}

void generateNewDictionary(StubAssembler& jit)
{
    static_assert(exists(newDictionary(numberOfRoomierFinalObjects)) && !exists(newDictionary(numberOfRoomierFinalObjects + 1)));
    const unsigned kind = static_cast<unsigned>(InlineAllocation::DefaultFinalObject) + jit.key().count();
    const unsigned inlineCapacity = JSFinalObject::defaultInlineCapacity + 2 * jit.key().count();
    const size_t numberOfWords = JSFinalObject::allocationSize(inlineCapacity) / sizeof(uint64_t);
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
        .constantArguments = { OperationCall::ConstantArgument { operationArgumentGPR(1), inlineCapacity } },
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
