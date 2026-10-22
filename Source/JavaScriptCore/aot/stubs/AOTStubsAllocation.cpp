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
#include "AOTStubsField.h"
#include "CompleteSubspace.h"
#include "JSArrayBufferView.h"
#include "JSObject.h"
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
    countCellOfAllocation(jit, allocation, record);
    loadAllocationSiteRecord(jit, allocation, record, allocator);
    jit.loadPtr(Address(record, AllocationSiteRecord::offsetOfAllocator()), allocator);
    return allocateCellWithLateRefill(jit, cell, allocator, scratch, slowPath, cellSize, CountedUnder::Allocation);
}

// The first chunk is left to whoever stores the header. Two chunks at a time from the last one down, where the size is not known: with an even number of chunks the first one is zeroed too.
// So a cell whose size is not known has two chunks or more: AllocationEmitter::findOnlyAllocationIn() sees to that.
static void zeroBehindFirstChunk(StubAssembler& jit, const AllocationSiteRegisters& registers, std::optional<unsigned> cellSize)
{
    const auto [cell, allocation, record, allocator, scratch] = registers;
#if CPU(X86_64)
    constexpr FPRReg zero = CCallHelpers::fpTempRegister;
    auto store = [&](int32_t offset, std::optional<GPRReg> index = std::nullopt) {
        if (jit.supportsAVX())
            index ? jit.m_assembler.vmovups_rm(zero, offset, cell, *index, CCallHelpers::TimesOne) : jit.m_assembler.vmovups_rm(zero, offset, cell);
        else
            index ? jit.m_assembler.movups_rm(zero, offset, cell, *index, CCallHelpers::TimesOne) : jit.m_assembler.movups_rm(zero, offset, cell);
    };
    jit.moveZeroToDouble(zero);
#endif
    if (cellSize) {
        for (unsigned offset = copyChunkBytes; offset < *cellSize; offset += copyChunkBytes) {
#if CPU(X86_64)
            store(offset);
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
    store(0, scratch);
    store(copyChunkBytes, scratch);
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
    } else {
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

// As Object::ensureExtensionGranules(). An Extension with room takes the granules where it is: behind those that it holds, or, while no collector may be visiting it, among them. One without room
// is replaced: a larger one takes the granules held, each at its place among the new ones, which are zero, and the old one stays as it is. The caller has the barrier.
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
        .returns = OperationCall::Returns::Value,
        .firstArgument = OperationCall::FirstArgument::Instance,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
