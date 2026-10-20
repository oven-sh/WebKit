/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCellAllocationEmitter.h"

#if ENABLE(AOT)

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
    allocation->setGenerator([cellSize, forcesSlowPath = generator().forcesGCSlowPaths(), units = &m_records.units](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
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

Pointer CellAllocationEmitter::allocatorForSize(Pointer subspace, Integer size, LBasicBlock slowPath)
{
    static_assert(!(MarkedSpace::sizeStep & (MarkedSpace::sizeStep - 1)), "MarkedSpace::sizeStep must be a power of two.");
    constexpr unsigned stepShift = getLSBSet(MarkedSpace::sizeStep);
    LValue sizeClassIndex = m_out.lShr(m_out.add(size.value(), m_out.constIntPtr(MarkedSpace::sizeStep - 1)), m_out.constInt32(stepShift));
    branchUnless(Condition(m_out.belowOrEqual(sizeClassIndex, m_out.constIntPtr(MarkedSpace::largeCutoff >> stepShift))), slowPath);
    Pointer allocator(m_out.loadPtr(m_out.baseIndex(m_heaps.CompleteSubspace_allocatorForSizeStep, subspace.value(), sizeClassIndex)));
    branchUnless(Condition(m_out.notNull(allocator.value())), slowPath);
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

void CellAllocationEmitter::fillWords(Pointer base, Integer beginIndex, Integer endIndex, Integer word, const B3::AbstractHeap& heap)
{
    LBasicBlock fillLoop = newBlock();
    LBasicBlock filled = newBlock();
    ValueFromBlock originalIndex = m_out.anchor(endIndex.value());
    ValueFromBlock originalPointer = m_out.anchor(offsetBy(base, beginIndex, 3).value());
    m_out.branch(notEqual(endIndex, beginIndex).value(), normalEdge(fillLoop), normalEdge(filled));

    m_out.appendTo(fillLoop);
    LValue index = m_out.phi(B3::Int64, originalIndex);
    LValue pointer = m_out.phi(B3::pointerType(), originalPointer);
    m_out.store64(word.value(), TypedPointer(heap, pointer));
    LValue nextIndex = m_out.sub(index, m_out.constInt64(1));
    m_out.addIncomingToPhi(index, m_out.anchor(nextIndex));
    m_out.addIncomingToPhi(pointer, m_out.anchor(m_out.add(pointer, m_out.intPtrEight)));
    m_out.branch(m_out.notEqual(nextIndex, beginIndex.value()), normalEdge(fillLoop), normalEdge(filled));

    m_out.appendTo(filled);
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

CellPointer CellAllocationEmitter::allocate(const OperationContext& context, AllocationID id)
{
    const PlacedAllocation& allocation = m_placement.allocation(id);
    bool hasOrderAndShapeWords = isObjectLayout(m_program.at(allocation.layout).kind);
    if (context.shape() != CodeShape::Fast) {
        CellInitialization form = hasOrderAndShapeWords ? CellInitialization::HeaderAndWords : CellInitialization::Header;
        return CellPointer(callStub(context, allocateCell(form, knownCellSizeFor(allocation.size)), { m_out.constInt64(id.index()) }).first);
    }

    const B3::AbstractHeap& recordHeap = m_heaps.entire(AbstractHeap::AllocatorState);
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    AllocationSiteAddress site = allocationSiteAddress(id);
    Pointer record = recordAt(site);
    Pointer allocator(m_out.loadPtr(m_out.address(recordHeap, record.value(), AllocationSiteRecord::offsetOfAllocator())));
    CellPointer cell = allocateHeapCell(allocator, knownCellSizeFor(allocation.size), slowPath);
    {
        CodeGenerator::InitializationScope scope(generator());
        if (hasOrderAndShapeWords)
            copyHeaderAndWords(site, cell);
        else
            storeHeader(cell, Integer(m_out.load64(m_out.address(recordHeap, record.value(), AllocationSiteRecord::offsetOfHeader()))));
    }
    join.add(cell);

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::AllocateSlow, { word32(id.index()) }, { .isOnRarePath = true }));
    return join.finish();
}

CellPointer CellAllocationEmitter::allocateObjectCopy(const OperationContext& context, AllocationID id, CellPointer source, unsigned fieldBytes)
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
    if (context.shape() != CodeShape::Fast) {
        branchUnless(below(length, word32(1 << holeArrayCapacityShift)), slowPath);
        Word32 lengthAndCapacity = bitOr(length, shiftLeft(vectorLength, word32(holeArrayCapacityShift)));
        CellPointer array(callStub(context, newArrayWithLength(arrayElementKindOf(indexingType)), { zeroExtended(lengthAndCapacity).value() }).first);
        branchUnless(Condition(m_out.notNull(array.value())), slowPath);
        return { array, Pointer(m_out.loadPtr(array.value(), m_heaps.JSObject_butterfly)) };
    }

    const ArrayStorageForm& form = arrayStorageFormOf(arrayStorageOf(indexingType));
    Integer header = loadMutable64(Instance::offsetOfNewArrayHeader(form.indexingType));
    branchUnless(isNotZero(header), slowPath);
    Integer size(m_out.add(m_out.shl(zeroExtended(vectorLength).value(), m_out.constInt32(3)), m_out.constIntPtr(sizeof(IndexingHeader))));
    Pointer allocator = allocatorForSize(loadImmutablePointer(AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), size, slowPath);
    Pointer storage = asPointer(allocateHeapCell(allocator, std::nullopt, slowPath));
    Pointer butterfly = offsetBy(storage, sizeof(IndexingHeader));

    CodeGenerator::InitializationScope scope(generator());
    m_out.store32(length.value(), butterfly.value(), m_heaps.Butterfly_publicLength);
    m_out.store32(vectorLength.value(), butterfly.value(), m_heaps.Butterfly_vectorLength);
    fillWords(butterfly, zeroExtended(length), zeroExtended(vectorLength), integer(static_cast<int64_t>(form.hole)), m_heaps.forIndexingType(form.indexingType)->atAnyIndex());
    CellPointer array = allocateHeapCell(loadImmutablePointer(AOT_INSTANCE_PREFIX_OFFSET(arrayAllocator)), sizeof(JSArray), slowPath);
    storeHeader(array, header);
    m_out.storePtr(butterfly.value(), array.value(), m_heaps.JSObject_butterfly);
    return { array, butterfly };
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
    Word32 minVectorLength = select(isZero(count), word32(BASE_CONTIGUOUS_VECTOR_LEN_EMPTY), word32(BASE_CONTIGUOUS_VECTOR_LEN));
    Word32 vectorLength = bitOr(select(below(minVectorLength, count), count, minVectorLength), word32(1));
    auto [array, butterfly] = allocateArray(context, count, vectorLength, ArrayWithContiguous, slowPath);
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
    join.add(callOperation<CellPointer>(context, RuntimeOperation::NewArray, { count, buffer }, { .isOnRarePath = true }));
    return join.finish();
}

CellPointer CellAllocationEmitter::allocateRope(LBasicBlock slowPath)
{
    CellPointer rope = allocateHeapCell(loadImmutablePointer(AOT_INSTANCE_PREFIX_OFFSET(ropeStringAllocator)), sizeof(JSRopeString), slowPath);
    CodeGenerator::InitializationScope scope(generator());
    storeHeader(rope, loadImmutable64(AOT_INSTANCE_PREFIX_OFFSET(stringHeader)));
    return rope;
}

void CellAllocationEmitter::storeRopeFibers(CellPointer rope, Integer fiber0, Integer length, Integer fiber1, Integer fiber2)
{
    using CompactFibers = JSRopeString::CompactFibers;
    LValue lengthWord = m_out.bitOr(length.value(), m_out.shl(fiber1.value(), m_out.constInt32(CompactFibers::shiftOfFiber1LowerInLengthWord)));
    LValue upperWord = m_out.bitOr(m_out.lShr(fiber1.value(), m_out.constInt32(CompactFibers::widthOfFiber1LowerInLengthWord)), m_out.shl(fiber2.value(), m_out.constInt32(CompactFibers::shiftOfFiber2InUpperWord)));
    m_out.store64(fiber0.value(), rope.value(), m_heaps.JSRopeString_fiber0);
    m_out.store64(lengthWord, rope.value(), m_heaps.JSRopeString_fiber1);
    m_out.store64(upperWord, rope.value(), m_heaps.JSRopeString_fiber2);
}

CellPointer CellAllocationEmitter::newPromise(LBasicBlock slowPath)
{
    CellPointer result = allocateObject(InlineAllocation::Promise, slowPath);
    {
        CodeGenerator::InitializationScope scope(generator());
        m_out.store64(m_out.int64Zero, result.value(), m_heaps.JSPromise_packed);
        m_out.store64(m_out.constInt64(JSValue::encode(JSValue())), result.value(), m_heaps.JSPromise_slot);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
