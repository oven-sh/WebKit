/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCellAllocationEmitter.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTAllocationEmitter.h"
#include "AOTAllocationSiteRecord.h"
#include "AOTArrayStorageForms.h"
#include "AOTBarrierEmitter.h"
#include "AOTCellFormat.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTPlacement.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsArrays.h"
#include "AOTVector.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "ArrayConventions.h"
#include "B3PatchpointValue.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include "IndexingHeader.h"
#include "JSArray.h"
#include "JSPromise.h"
#include "JSString.h"
#include "MarkedSpace.h"
#include <algorithm>
#include <array>
#include <type_traits>
#include <wtf/MathExtras.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(sizeof(JSArray) <= MarkedSpace::preciseCutoff && !(sizeof(JSArray) % MarkedSpace::sizeStep));
static_assert(sizeof(JSRopeString) <= MarkedSpace::preciseCutoff && !(sizeof(JSRopeString) % MarkedSpace::sizeStep));

static constexpr ptrdiff_t offsetOfAllocator(InlineAllocation kind)
{
    return AOT_INSTANCE_OFFSET(allocation.inlineAllocations) + static_cast<unsigned>(kind) * sizeof(InlineAllocationData) + OBJECT_OFFSETOF(InlineAllocationData, allocator);
}

static constexpr ptrdiff_t offsetOfHeader(InlineAllocation kind)
{
    return AOT_INSTANCE_OFFSET(allocation.inlineAllocations) + static_cast<unsigned>(kind) * sizeof(InlineAllocationData) + OBJECT_OFFSETOF(InlineAllocationData, header);
}

static constexpr std::optional<size_t> knownCellSizeFor(size_t bytes)
{
    if (bytes <= MarkedSpace::preciseCutoff && !(bytes % MarkedSpace::sizeStep))
        return bytes;
    return std::nullopt;
}

CellAllocationEmitter::CellAllocationEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

CellPointer CellAllocationEmitter::allocateHeapCell(Pointer allocatorOrNull, LBasicBlock slowPath)
{
    branchUnless(Condition(m_out.notNull(allocatorOrNull.value())), slowPath);
    return allocateHeapCell(allocatorOrNull, std::nullopt, slowPath);
}

CellPointer CellAllocationEmitter::allocateHeapCell(Pointer allocator, std::optional<size_t> cellSize, LBasicBlock slowPath)
{
    LBasicBlock continuation = newBlock();
    std::array<const B3::AbstractHeap*, 2> heaps { &m_heaps.collectorState, &m_heaps.entire(AbstractHeap::AllocatorState) };
    B3::PatchpointValue* allocation = m_out.patchpoint(B3::pointerType());
    allocation->effects = B3::Effects::none();
    allocation->effects.terminal = true;
    m_heaps.decoratePatchpointRead(heaps, allocation);
    m_heaps.decoratePatchpointWrite(heaps, allocation);
    allocation->setRunsWithoutFrame(true);
    allocation->append(B3::ConstrainedValue(allocator.value(), B3::ValueRep::SomeRegister));
    allocation->numGPScratchRegisters = 1;
    allocation->resultConstraints = { B3::ValueRep::SomeEarlyRegister };
    allocation->clobber(RegisterSet::macroClobberedGPRs());
    m_out.m_block->setSuccessors(normalEdge(continuation), rareEdge(slowPath));
    allocation->setGenerator([cellSize, countsCell = build().recordedOptions().countsBranches, forcesSlowPath = generator().forcesGCSlowPaths(), units = &m_records.units](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (countsCell) {
            size_t kind = ExactAllocationCounts::kindOfCompiledCode();
            jit.loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.counters.allocationCounts)), params[0].gpr());
            jit.load32(CCallHelpers::Address(params[1].gpr(), LocalAllocator::offsetOfCellSize()), params.gpScratch(0));
            jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(params[0].gpr(), static_cast<int32_t>(ExactAllocationCounts::offsetOfCells(kind))));
            jit.add64(params.gpScratch(0), CCallHelpers::Address(params[0].gpr(), static_cast<int32_t>(ExactAllocationCounts::offsetOfBytes(kind))));
        }
        WTF::Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        CCallHelpers::JumpList slowCases;
        std::optional<CCallHelpers::FreeListRefill> refill;
        if (forcesSlowPath)
            slowCases.append(jit.jump());
        else
            refill = jit.emitAllocateFromFreeList(params[0].gpr(), params[1].gpr(), params.gpScratch(0), slowCases, cellSize ? std::optional<unsigned>(static_cast<unsigned>(*cellSize)) : std::nullopt);
        CCallHelpers::Jump done;
        if (refill && !params.fallsThroughToSuccessor(0))
            done = jit.jump();
        params.addLatePath([=](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            CCallHelpers::JumpList toSlowPath = slowCases;
            if (refill) {
                // The same bytes at every site with these three registers: the outliner keeps one copy of them.
                Vector<CCallHelpers::Label, 10> boundaries;
                auto appendBoundary = [&] {
                    boundaries.append(jit.label());
                };
                ScopedLambda<void()> atBoundary(appendBoundary);
                jit.emitFreeListRefill(*refill, toSlowPath, &atBoundary);
                RegisterSet named = RegisterSet::macroClobberedGPRs();
                named.add(refill->result, IgnoreVectors);
                named.add(refill->allocator, IgnoreVectors);
                named.add(refill->scratch, IgnoreVectors);
                for (size_t index = 0; index + 1 < boundaries.size(); ++index)
                    units->addPlain(boundaries[index], boundaries[index + 1], named);
            }
            toSlowPath.linkTo(*labels[1], &jit);
            if (done.isSet())
                done.linkTo(*labels[0], &jit);
        });
    });
    m_out.appendTo(continuation);
    return CellPointer(allocation);
}

// A size class has its allocator from the first allocation that goes through the heap.
Pointer CellAllocationEmitter::allocatorForSize(Pointer subspace, Integer size, LBasicBlock isTooLarge, LBasicBlock hasNoAllocator)
{
    static_assert(!(MarkedSpace::sizeStep & (MarkedSpace::sizeStep - 1)), "MarkedSpace::sizeStep must be a power of two.");
    constexpr unsigned stepShift = getLSBSet(MarkedSpace::sizeStep);
    LValue sizeClassIndex = m_out.lShr(m_out.add(size.value(), m_out.constIntPtr(MarkedSpace::sizeStep - 1)), m_out.constInt32(stepShift));
    branchUnless(Condition(m_out.belowOrEqual(sizeClassIndex, m_out.constIntPtr(MarkedSpace::largeCutoff >> stepShift))), isTooLarge);
    Pointer allocator(m_out.loadPtr(m_out.baseIndex(m_heaps.CompleteSubspace_allocatorForSizeStep, subspace.value(), sizeClassIndex)));
    branchUnless(Condition(m_out.notNull(allocator.value())), hasNoAllocator);
    return allocator;
}

Pointer CellAllocationEmitter::allocatorForSize(Pointer subspace, size_t size)
{
    RELEASE_ASSERT(size <= MarkedSpace::largeCutoff);
    return Pointer(m_out.loadPtr(subspace.value(), m_heaps.CompleteSubspace_allocatorForSizeStep[MarkedSpace::sizeClassToIndex(size)]));
}

void CellAllocationEmitter::storeHeader(CellPointer cell, Integer header)
{
    static_assert(!JSCell::structureIDOffset());
    m_out.store64(header.value(), cell.value(), m_heaps.JSCell_header);
}

CellPointer CellAllocationEmitter::allocateObject(InlineAllocation kind, LBasicBlock slowPath)
{
    const B3::AbstractHeap& heap = m_heaps.entire(AbstractHeap::AllocatorState);
    CellPointer result = allocateHeapCell(Pointer(m_out.loadPtr(instanceField(heap, offsetOfAllocator(kind)))), slowPath);
    CodeGenerator::InitializationScope scope(generator());
    storeHeader(result, Integer(m_out.load64(instanceField(heap, offsetOfHeader(kind)))));
    m_out.storePtr(m_out.intPtrZero, result.value(), m_heaps.JSObject_butterfly);
    return result;
}

CellAllocationEmitter::AllocationSiteAddress CellAllocationEmitter::allocationSiteAddress(AllocationID id)
{
    static_assert(std::is_same_v<decltype(ObjectWords::distancesToAllocationSites), int32_t*>);
    Pointer distances = loadImmutablePointer(AOT_INSTANCE_OFFSET(objects.distancesToAllocationSites));
    Word32 distance(m_out.load32(m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), distances.value(), static_cast<ptrdiff_t>(id.index()) * sizeof(int32_t))));
    return { distances, signExtended(distance) };
}

Pointer CellAllocationEmitter::recordAt(const AllocationSiteAddress& site)
{
    return offsetBy(Pointer(m_out.add(site.distances.value(), site.distance.value())), -static_cast<ptrdiff_t>(sizeof(AllocationSiteRecord)));
}

Pointer CellAllocationEmitter::allocationSiteRecord(AllocationID id)
{
    return recordAt(allocationSiteAddress(id));
}

// The header, the order word and the shape word lie in the record as they lie in the cell.
void CellAllocationEmitter::copyHeaderAndWords(const AllocationSiteAddress& site, CellPointer cell)
{
    static_assert(AllocationSiteRecord::offsetOfOrderAndShapeWords() == AllocationSiteRecord::offsetOfHeader() + static_cast<ptrdiff_t>(sizeof(uint64_t)));
    static_assert(offsetOfOrderWord == sizeof(JSCell) && offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t) && sizeof(JSCell) == sizeof(uint64_t));
    const B3::AbstractHeap& recordHeap = m_heaps.entire(AbstractHeap::AllocatorState);
    std::array<const B3::AbstractHeap*, 3> written { &m_heaps.JSCell_header, &m_heaps.AOTObject_orderWord, &m_heaps.entire(AbstractHeap::ShapeWords) };
    B3::PatchpointValue* copy = m_out.patchpoint(B3::Void);
    copy->effects = B3::Effects::none();
    m_heaps.decoratePatchpointRead(&recordHeap, copy);
    m_heaps.decoratePatchpointWrite(written, copy);
    copy->appendSomeRegister(site.distances.value());
    copy->appendSomeRegister(site.distance.value());
    copy->appendSomeRegister(cell.value());
    copy->setRunsWithoutFrame(true);
    if (isX86_64())
        copy->numFPScratchRegisters = 1;
    else
        copy->numGPScratchRegisters = 2;
    copy->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        constexpr int32_t offsetOfRecord = -static_cast<int32_t>(sizeof(AllocationSiteRecord)) + static_cast<int32_t>(AllocationSiteRecord::offsetOfHeader());
        GPRReg distances = params[0].gpr();
        GPRReg distance = params[1].gpr();
        GPRReg cell = params[2].gpr();
#if CPU(X86_64)
        FPRReg chunk = params.fpScratch(0);
        if (jit.supportsAVX()) {
            jit.m_assembler.vmovups_mr(offsetOfRecord, distances, distance, CCallHelpers::TimesOne, chunk);
            jit.m_assembler.vmovups_rm(chunk, 0, cell);
        } else {
            jit.m_assembler.movups_mr(offsetOfRecord, distances, distance, CCallHelpers::TimesOne, chunk);
            jit.m_assembler.movups_rm(chunk, 0, cell);
        }
#else
        GPRReg first = params.gpScratch(0);
        GPRReg second = params.gpScratch(1);
        jit.addPtr(distances, distance, first);
        jit.loadPair64(first, CCallHelpers::TrustedImm32(offsetOfRecord), first, second);
        jit.storePair64(first, second, cell, CCallHelpers::TrustedImm32(0));
#endif
    });
}

void CellAllocationEmitter::copyHeaderAndWords(AllocationID id, CellPointer cell)
{
    copyHeaderAndWords(allocationSiteAddress(id), cell);
}

void CellAllocationEmitter::ensureRecordIsFilled(const OperationContext& context, AllocationID id)
{
    LBasicBlock isEmpty = newBlock();
    LBasicBlock continuation = newBlock();
    Integer header(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), allocationSiteRecord(id).value(), AllocationSiteRecord::offsetOfHeader())));
    m_out.branch(isNotZero(header).value(), normalEdge(continuation), rareEdge(isEmpty));

    // The operation fills the record and allocates no cell.
    m_out.appendTo(isEmpty);
    callOperation<Integer>(context, RuntimeOperation::GetHeaderOfAllocation, { word32(static_cast<int32_t>(id.index())) }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

static constexpr size_t maxBytesZeroedInLine = 256;
static constexpr unsigned maxInstructionsForEachStoreOfZeroSaved = 4;

ZeroedBy CellAllocationEmitter::whoZeroesBody(const OperationContext& context, AllocationID id, unsigned storesOfZero) const
{
    const PlacedAllocation& allocation = m_placement.allocation(id);
    CellInitialization form = isObjectLayout(m_program.at(allocation.layout).kind) ? CellInitialization::HeaderAndWordsAndZeros : CellInitialization::HeaderAndZeros;
    unsigned instructionsInStub = instructionsToZeroBody(form, allocation.size, knownCellSizeFor(allocation.size).has_value());
    if (storesOfZero >= instructionsInStub)
        return ZeroedBy::Stub;
    // A store of zero is seven or eight bytes at every place that allocates. Outside fast code each one saved may cost a few instructions in the stub.
    if (context.shape() != CodeShape::Fast && storesOfZero && instructionsInStub <= maxInstructionsForEachStoreOfZeroSaved * (storesOfZero + 1))
        return ZeroedBy::Stub;
    if (allocation.size <= maxBytesZeroedInLine && storesOfZero >= instructionsToZeroBody(form, allocation.size, true))
        return ZeroedBy::CodeInLine;
    return ZeroedBy::Nobody;
}

void CellAllocationEmitter::zeroBody(CellPointer cell, AllocationID id)
{
    const PlacedAllocation& allocation = m_placement.allocation(id);
    const Layout& layout = m_program.at(allocation.layout);
    std::array<const B3::AbstractHeap*, 2> written { &m_heaps.entire(layout.isEnvironment() ? AbstractHeap::EnvironmentFields : AbstractHeap::ObjectFields), &m_heaps.entire(AbstractHeap::RuntimeState) };
    if (!isObjectLayout(layout.kind))
        m_out.store64(m_out.constInt64(0), m_out.address(*written[0], cell.value(), sizeof(JSCell)));
    unsigned end = static_cast<unsigned>(WTF::roundUpToMultipleOf<copyChunkBytes>(allocation.size));
    if (end <= copyChunkBytes)
        return;
    B3::PatchpointValue* zero = m_out.patchpoint(B3::Void);
    zero->effects = B3::Effects::none();
    m_heaps.decoratePatchpointWrite(written, zero);
    zero->appendSomeRegister(cell.value());
    zero->setRunsWithoutFrame(true);
    if (isX86_64())
        zero->numFPScratchRegisters = 1;
    zero->setGenerator([end](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        GPRReg cell = params[0].gpr();
#if CPU(X86_64)
        FPRReg zero = params.fpScratch(0);
        jit.moveZeroToDouble(zero);
#endif
        for (unsigned offset = copyChunkBytes; offset < end; offset += copyChunkBytes) {
#if CPU(X86_64)
            if (jit.supportsAVX())
                jit.m_assembler.vmovups_rm(zero, offset, cell);
            else
                jit.m_assembler.movups_rm(zero, offset, cell);
#else
            jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, cell, CCallHelpers::TrustedImm32(offset));
#endif
        }
    });
}

CellPointer CellAllocationEmitter::allocate(const OperationContext& context, AllocationID id, WholeCell, ZeroedBy zeroedBy)
{
    return allocateWithoutPlace(context, id, zeroedBy);
}

CellPointer CellAllocationEmitter::allocateCellOfBothObjects(const OperationContext& context, const PlaceInCell& place)
{
    RELEASE_ASSERT(place.kind() == PlaceInCell::Kind::BehindOuterObject);
    return allocateWithoutPlace(context, place.outerAllocation(), ZeroedBy::Nobody);
}

CellPointer CellAllocationEmitter::allocateWithoutPlace(const OperationContext& context, AllocationID id, ZeroedBy zeroedBy)
{
    const PlacedAllocation& allocation = m_placement.allocation(id);
    bool hasOrderAndShapeWords = isObjectLayout(m_program.at(allocation.layout).kind);
    CellInitialization form = hasOrderAndShapeWords ? CellInitialization::HeaderAndWords : CellInitialization::Header;
    if (zeroedBy == ZeroedBy::Stub)
        form = hasOrderAndShapeWords ? CellInitialization::HeaderAndWordsAndZeros : CellInitialization::HeaderAndZeros;
    CellPointer cell(callStub(context, allocateCell(form, knownCellSizeFor(allocation.size)), { m_out.constInt64(id.index()) }).first);
    if (zeroedBy == ZeroedBy::CodeInLine) {
        CodeGenerator::InitializationScope scope(generator());
        zeroBody(cell, id);
    }
    return cell;
}

CellPointer CellAllocationEmitter::allocateZeroedByNumber(const OperationContext& context, Word32 numberOfAllocation, WholeCell)
{
    return CellPointer(callStub(context, allocateCell(CellInitialization::HeaderAndWordsAndZeros), { zeroExtended(numberOfAllocation).value() }).first);
}

CellPointer CellAllocationEmitter::allocateObjectCopy(const OperationContext& context, AllocationID id, WholeCell, CellPointer source, unsigned fieldBytes)
{
    RELEASE_ASSERT(fieldBytes && !(fieldBytes % copyChunkBytes));
    LValue allocation = m_out.constInt64(id.index());
    if (fieldBytes <= maxBytesCopiedWithoutLoop)
        return CellPointer(callStub(context, copyObject(fieldBytes), { allocation, source.value() }).first);
    return CellPointer(callStub(context, copyObject(std::nullopt), { allocation, source.value(), m_out.constInt64(fieldBytes - copyChunkBytes) }).first);
}

CellPointer CellAllocationEmitter::allocateInFrame(size_t size)
{
    return CellPointer(m_out.lockedStackSlot(size));
}

std::pair<CellPointer, Pointer> CellAllocationEmitter::allocateArray(const OperationContext& context, Word32 length, Word32 vectorLength, IndexingType indexingType, LBasicBlock slowPath)
{
    branchUnless(below(length, word32(1 << holeArrayCapacityShift)), slowPath);
    Word32 lengthAndCapacity = bitOr(length, shiftLeft(vectorLength, word32(holeArrayCapacityShift)));
    CellPointer array(callStub(context, newArrayWithLength(arrayElementKindOf(indexingType)), { zeroExtended(lengthAndCapacity).value() }).first);
    branchUnless(Condition(m_out.notNull(array.value())), slowPath);
    return { array, Pointer(m_out.loadPtr(array.value(), m_heaps.JSObject_butterfly)) };
}

CellPointer CellAllocationEmitter::newArrayOf(const OperationContext& context, std::span<const Boxed> elements)
{
    unsigned length = static_cast<unsigned>(elements.size());
    if (length <= maxArrayElementsInRegisters) {
        Vector<LValue, maxArrayElementsInRegisters> operands;
        for (Boxed element : elements)
            operands.append(element.value());
        return CellPointer(callStub(context, arrayLiteral(length), operands.span()).first);
    }
    if (context.shape() != CodeShape::Fast)
        return callOperation<CellPointer>(context, RuntimeOperation::NewArray, { word32(static_cast<int32_t>(length)), argumentBuffer(elements) });

    unsigned vectorLength = std::max<unsigned>(length, BASE_CONTIGUOUS_VECTOR_LEN) | 1;
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    auto [array, butterfly] = allocateArray(context, word32(static_cast<int32_t>(length)), word32(static_cast<int32_t>(vectorLength)), ArrayWithContiguous, slowPath);
    {
        CodeGenerator::InitializationScope scope(generator());
        for (unsigned i = 0; i < length; ++i)
            m_out.store64(elements[i].value(), butterfly.value(), m_heaps.indexedContiguousProperties[i]);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    join.add(array);

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::NewArray, { word32(static_cast<int32_t>(length)), argumentBuffer(elements) }, { .isOnRarePath = true }));
    return join.finish();
}

CellPointer CellAllocationEmitter::newArrayFromBuffer(const OperationContext& context, Word32 count, Pointer buffer)
{
    LBasicBlock slowPath = newBlock();
    LBasicBlock copyLoop = newBlock();
    LBasicBlock copyElement = newBlock();
    LBasicBlock copied = newBlock();
    Join<CellPointer> join(m_out);
    auto [array, butterfly] = allocateArray(context, count, bitOr(count, word32(1)), ArrayWithContiguous, slowPath);
    {
        CodeGenerator::InitializationScope scope(generator());
        LValue end = m_out.shl(zeroExtended(count).value(), m_out.constInt32(3));
        ValueFromBlock start = m_out.anchor(m_out.intPtrZero);
        m_out.jump(copyLoop);

        m_out.appendTo(copyLoop);
        LValue offset = m_out.phi(B3::pointerType(), start);
        m_out.branch(m_out.below(offset, end), normalEdge(copyElement), normalEdge(copied));

        m_out.appendTo(copyElement);
        LValue element = m_out.load64(TypedPointer(m_heaps.argumentBuffer, m_out.add(buffer.value(), offset)));
        m_out.store64(element, TypedPointer(m_heaps.indexedContiguousProperties.atAnyIndex(), m_out.add(butterfly.value(), offset)));
        m_out.addIncomingToPhi(offset, m_out.anchor(m_out.add(offset, m_out.intPtrEight)));
        m_out.jump(copyLoop);

        m_out.appendTo(copied);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    join.add(array);

    m_out.appendTo(slowPath);
    Word32 countOrCapacityOfEmptyArray = select(isZero(count), word32(1 << capacityShiftInArrayLiteralCount), count);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::NewArray, { countOrCapacityOfEmptyArray, buffer }, { .isOnRarePath = true }));
    return join.finish();
}

std::pair<CellPointer, Pointer> CellAllocationEmitter::allocateArrayInFrame(Word32 length, unsigned capacity, LBasicBlock isOnHeap)
{
    Integer header = loadMutable64(AOT_INSTANCE_OFFSET(allocation.frameArrayHeader));
    branchUnless(isNotZero(header), isOnHeap);
    CellPointer array = allocateInFrame(sizeOfFrameArray(capacity));
    Pointer butterfly = offsetBy(asPointer(array), sizeOfFrameArray(0));

    CodeGenerator::InitializationScope scope(generator());
    storeHeader(array, header);
    m_out.storePtr(butterfly.value(), array.value(), m_heaps.JSObject_butterfly);
    m_out.store32(length.value(), butterfly.value(), m_heaps.Butterfly_publicLength);
    m_out.store32(length.value(), butterfly.value(), m_heaps.Butterfly_vectorLength);
    return { array, butterfly };
}

CellPointer CellAllocationEmitter::newArrayInFrameOf(const OperationContext& context, std::span<const Boxed> elements)
{
    unsigned length = static_cast<unsigned>(elements.size());
    if (length <= maxArrayElementsInRegisters) {
        Vector<LValue, maxArrayElementsInRegisters + 1> operands;
        for (Boxed element : elements)
            operands.append(element.value());
        operands.append(allocateInFrame(sizeOfFrameArray(length)).value());
        return CellPointer(callStub(context, frameArrayLiteral(length), operands.span()).first);
    }

    LBasicBlock isOnHeap = newBlock();
    Join<CellPointer> join(m_out);
    auto [array, butterfly] = allocateArrayInFrame(word32(static_cast<int32_t>(length)), length, isOnHeap);
    {
        CodeGenerator::InitializationScope scope(generator());
        for (unsigned i = 0; i < length; ++i)
            m_out.store64(elements[i].value(), butterfly.value(), m_heaps.indexedContiguousProperties[i]);
    }
    join.add(array);

    m_out.appendTo(isOnHeap);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::NewArray, { word32(static_cast<int32_t>(length)), argumentBuffer(elements) }, { .isOnRarePath = true }));
    return join.finish();
}

CellPointer CellAllocationEmitter::newArrayInFrameFromBuffer(const OperationContext& context, Word32 count, Pointer buffer, unsigned capacity)
{
    LBasicBlock isOnHeap = newBlock();
    LBasicBlock copyLoop = newBlock();
    LBasicBlock copyElement = newBlock();
    LBasicBlock copied = newBlock();
    Join<CellPointer> join(m_out);
    branchUnless(belowOrEqual(count, word32(static_cast<int32_t>(capacity))), isOnHeap);
    auto [array, butterfly] = allocateArrayInFrame(count, capacity, isOnHeap);
    {
        CodeGenerator::InitializationScope scope(generator());
        LValue end = m_out.shl(zeroExtended(count).value(), m_out.constInt32(3));
        ValueFromBlock start = m_out.anchor(m_out.intPtrZero);
        m_out.jump(copyLoop);

        m_out.appendTo(copyLoop);
        LValue offset = m_out.phi(B3::pointerType(), start);
        m_out.branch(m_out.below(offset, end), normalEdge(copyElement), normalEdge(copied));

        m_out.appendTo(copyElement);
        LValue element = m_out.load64(TypedPointer(m_heaps.argumentBuffer, m_out.add(buffer.value(), offset)));
        m_out.store64(element, TypedPointer(m_heaps.indexedContiguousProperties.atAnyIndex(), m_out.add(butterfly.value(), offset)));
        m_out.addIncomingToPhi(offset, m_out.anchor(m_out.add(offset, m_out.intPtrEight)));
        m_out.jump(copyLoop);

        m_out.appendTo(copied);
    }
    join.add(array);

    m_out.appendTo(isOnHeap);
    join.add(newArrayFromBuffer(context, count, buffer));
    return join.finish();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
