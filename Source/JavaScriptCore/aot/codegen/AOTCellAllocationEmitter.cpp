/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCellAllocationEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBarrierEmitter.h"
#include "AOTCellFormat.h"
#include "AOTClosuresLent.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsArrays.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "ArrayConventions.h"
#include "CCallHelpers.h"
#include "JSArray.h"
#include "JSPromise.h"
#include "JSString.h"
#include "MarkedSpace.h"
#include <algorithm>
#include <array>
#include <type_traits>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(sizeof(JSArray) <= MarkedSpace::preciseCutoff && !(sizeof(JSArray) % MarkedSpace::sizeStep));
static_assert(sizeof(JSRopeString) <= MarkedSpace::preciseCutoff && !(sizeof(JSRopeString) % MarkedSpace::sizeStep));

static constexpr std::optional<size_t> knownCellSizeFor(size_t bytes)
{
    if (bytes && bytes <= MarkedSpace::preciseCutoff && !(bytes % MarkedSpace::sizeStep))
        return bytes;
    return std::nullopt;
}

CellPointer CellAllocationEmitter::allocateHeapCell(Pointer allocatorOrNull, LBasicBlock slowPath)
{
    branchUnless(Condition(m_out.notNull(allocatorOrNull.value())), slowPath);
    return allocateHeapCell(allocatorOrNull, std::nullopt, slowPath);
}

CellPointer CellAllocationEmitter::allocateHeapCell(Pointer allocator, std::optional<size_t> cellSize, LBasicBlock slowPath)
{
    LBasicBlock continuation = newBlock();
    std::array<const Machine::AbstractHeap*, 2> heaps { &m_heaps.collectorState, &m_heaps.entire(AbstractHeap::AllocatorState) };
    Machine::PatchOperation* allocation = m_out.patchpoint(Machine::pointerType());
    allocation->effects = Machine::Effects::none();
    allocation->effects.terminal = true;
    m_heaps.decoratePatchpointRead(heaps, allocation);
    m_heaps.decoratePatchpointWrite(heaps, allocation);
    allocation->setRunsWithoutFrame(true);
    allocation->appendSomeRegister(allocator.value());
    allocation->numGPScratchRegisters = 1;
    allocation->resultConstraints = { Machine::ValueRep::SomeEarlyRegister };
    allocation->clobber(RegisterSet::macroClobberedGPRs());
    m_out.m_block->setSuccessors(normalEdge(continuation), rareEdge(slowPath));
    allocation->setGenerator([cellSize](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::JumpList slowCases;
        CCallHelpers::FreeListRefill refill = jit.emitAllocateFromFreeList(params[0].gpr(), params[1].gpr(), params.gpScratch(0), slowCases, cellSize ? std::optional<unsigned>(static_cast<unsigned>(*cellSize)) : std::nullopt);
        RELEASE_ASSERT(slowCases.empty());
        CCallHelpers::Jump done;
        if (!params.fallsThroughToSuccessor(0))
            done = jit.jump();
        params.addLatePath([refill, done, hasCell = params.successorLabels()[0], hasNoCell = params.successorLabels()[1]](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            CCallHelpers::JumpList toSlowPath;
            jit.emitFreeListRefill(refill, toSlowPath);
            toSlowPath.linkTo(*hasNoCell, &jit);
            if (done.isSet())
                done.linkTo(*hasCell, &jit);
        });
    });
    m_out.appendToContinuation(continuation);
    return CellPointer(allocation);
}

void CellAllocationEmitter::storeHeader(CellPointer cell, Integer header)
{
    static_assert(!JSCell::structureIDOffset());
    m_out.store64(header.value(), cell.value(), m_heaps.JSCell_header);
}

CellPointer CellAllocationEmitter::allocateCellWithHeader(InlineAllocation kind, LBasicBlock slowPath)
{
    const Machine::AbstractHeap& heap = m_heaps.entire(AbstractHeap::AllocatorState);
    ptrdiff_t offsetOfData = AOT_INSTANCE_OFFSET(allocation.inlineAllocations) + static_cast<unsigned>(kind) * sizeof(InlineAllocationData);
    CellPointer result = allocateHeapCell(Pointer(m_out.loadPtr(instanceField(heap, offsetOfData + OBJECT_OFFSETOF(InlineAllocationData, allocator)))), slowPath);
    CodeGenerator::InitializationScope scope(generator());
    storeHeader(result, Integer(m_out.load64(instanceField(heap, offsetOfData + OBJECT_OFFSETOF(InlineAllocationData, header)))));
    return result;
}

CellPointer CellAllocationEmitter::allocateObject(InlineAllocation kind, LBasicBlock slowPath)
{
    CellPointer result = allocateCellWithHeader(kind, slowPath);
    CodeGenerator::InitializationScope scope(generator());
    m_out.storePtr(m_out.intPtrZero, result.value(), m_heaps.JSNonArray_butterfly);
    return result;
}

MentionedAllocation CellAllocationEmitter::allocationOf(const OperationContext& context, LayoutID layout, std::span<const FieldIndex> initialFields)
{
    std::optional<PlacedAllocation> placed = m_plan.allocation(m_program, layout, initialFields);
    if (!placed) {
        fail(context, InternalErrorKind::PlacedPayloadMissing);
        return { PlacedAllocation { .layout = layout, .size = m_plan.layout(layout).size }, 0 };
    }
    return { *placed, m_mentions.allocation(*placed, initialFields) };
}

MentionedAllocation CellAllocationEmitter::environmentAllocationOf(LayoutID layout)
{
    PlacedAllocation placed = m_plan.environmentAllocation(layout);
    return { placed, m_mentions.allocation(placed, { }) };
}

MentionedAllocation CellAllocationEmitter::closureAllocationOf(unsigned numberOfCaptures)
{
    PlacedAllocation placed = m_plan.closureAllocation(numberOfCaptures);
    return { placed, m_mentions.allocation(placed, { }) };
}

Pointer CellAllocationEmitter::allocationSiteRecord(const MentionedAllocation& allocation)
{
    static_assert(std::is_same_v<decltype(ObjectWords::distancesToAllocationSites), int32_t*>);
    Pointer distances = loadImmutablePointer(AOT_INSTANCE_OFFSET(objects.distancesToAllocationSites));
    Pointer placeOfDistance = offsetBy(distances, rowOf(allocation), getLSBSet(sizeof(int32_t)));
    Word32 distance(m_out.load32(m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), placeOfDistance.value())));
    return offsetBy(Pointer(m_out.add(distances.value(), signExtended(distance).value())), -static_cast<ptrdiff_t>(sizeof(AllocationSiteRecord)));
}

ZeroedBy CellAllocationEmitter::whoZeroesBody(const OperationContext& context, const MentionedAllocation& allocation, unsigned storesOfZero, unsigned storesOfOtherPatterns) const
{
    size_t size = allocation.placed.size;
    if (!m_generator.usesLoopForms(context.block()) && !m_generator.isInRecursion(context.block())) {
        if (storesOfOtherPatterns && size > copyChunkBytes && size <= maxCellSizeFromTemplate)
            return ZeroedBy::StubFromTemplate;
        return storesOfZero ? ZeroedBy::Stub : ZeroedBy::Nobody;
    }
    bool hasOrderAndShapeWords = isObjectLayout(m_program.at(allocation.placed.layout).kind);
    CellInitialization form = hasOrderAndShapeWords ? CellInitialization::HeaderAndWordsAndZeros : CellInitialization::HeaderAndZeros;
    unsigned instructionsWithSizeKnown = instructionsToZeroBody(form, size, true);
    ZeroedBy zeroedBy = ZeroedBy::Nobody;
    if (storesOfZero >= instructionsToZeroBody(form, size, knownCellSizeFor(size).has_value()))
        zeroedBy = ZeroedBy::Stub;
    else if (size <= maxBytesZeroedInLine && storesOfZero >= instructionsWithSizeKnown)
        zeroedBy = ZeroedBy::CodeInLine;

    if (!storesOfOtherPatterns || size <= copyChunkBytes || size > maxCellSizeFromTemplate)
        return zeroedBy;
    unsigned fewestInstructionsForZeros = std::min(storesOfZero, instructionsWithSizeKnown);
    if (fewestInstructionsForZeros + storesOfOtherPatterns < instructionsToCopyBody(hasOrderAndShapeWords ? CellInitialization::HeaderAndWords : CellInitialization::Header, size))
        return zeroedBy;
    unsigned instructionsForZerosAtSite = zeroedBy == ZeroedBy::Stub ? 0 : fewestInstructionsForZeros;
    return instructionsForZerosAtSite + storesOfOtherPatterns > instructionsToNameTemplate ? ZeroedBy::StubFromTemplate : zeroedBy;
}

void CellAllocationEmitter::zeroBody(CellPointer cell, const MentionedAllocation& allocation)
{
    const Layout& layout = m_program.at(allocation.placed.layout);
    std::array<const Machine::AbstractHeap*, 2> written { &m_heaps.entire(layout.isEnvironment() ? AbstractHeap::EnvironmentFields : AbstractHeap::ObjectFields), &m_heaps.entire(AbstractHeap::RuntimeState) };
    if (!isObjectLayout(layout.kind))
        m_out.store64(m_out.constInt64(0), m_out.address(*written[0], cell.value(), sizeof(JSCell)));
    unsigned end = static_cast<unsigned>(WTF::roundUpToMultipleOf<copyChunkBytes>(allocation.placed.size));
    if (end <= copyChunkBytes)
        return;
    Machine::PatchOperation* zero = m_out.patchpoint(Machine::Void);
    zero->effects = Machine::Effects::none();
    m_heaps.decoratePatchpointWrite(written, zero);
    zero->appendSomeRegister(cell.value());
    zero->setRunsWithoutFrame(true);
    if (isX86_64())
        zero->numFPScratchRegisters = 1;
    zero->setGenerator([end](CCallHelpers& jit, const Machine::PatchParameters& params) {
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

CellPointer CellAllocationEmitter::allocateFromTemplate(const OperationContext& context, const MentionedAllocation& allocation, std::span<const uint8_t> bodyOfEmptyCell)
{
    CellInitialization form = isObjectLayout(m_program.at(allocation.placed.layout).kind) ? CellInitialization::HeaderAndWords : CellInitialization::Header;
    RELEASE_ASSERT(bodyOfEmptyCell.size() == sizeOfCellTemplate(form, allocation.placed.size));
    LValue addressOfTemplate = m_out.relocatable(SymbolSpace::Constant, m_out.addConstantData(bodyOfEmptyCell, ConstantKind::CellTemplate));
    return CellPointer(callStub(context, allocateCellFromTemplate(form, allocation.placed.size), { m_out.wideNumber(allocation.mention), addressOfTemplate }).first);
}

CellPointer CellAllocationEmitter::allocate(const OperationContext& context, const MentionedAllocation& allocation, ZeroedBy zeroedBy)
{
    RELEASE_ASSERT(zeroedBy != ZeroedBy::StubFromTemplate);
    bool hasOrderAndShapeWords = isObjectLayout(m_program.at(allocation.placed.layout).kind);
    CellInitialization form = hasOrderAndShapeWords ? CellInitialization::HeaderAndWords : CellInitialization::Header;
    if (zeroedBy == ZeroedBy::Stub)
        form = hasOrderAndShapeWords ? CellInitialization::HeaderAndWordsAndZeros : CellInitialization::HeaderAndZeros;
    CellPointer cell(callStub(context, allocateCell(form, knownCellSizeFor(allocation.placed.size)), { m_out.wideNumber(allocation.mention) }).first);
    if (zeroedBy == ZeroedBy::CodeInLine) {
        CodeGenerator::InitializationScope scope(generator());
        zeroBody(cell, allocation);
    }
    return cell;
}

std::pair<CellPointer, Condition> CellAllocationEmitter::allocateInstance(const OperationContext& context, const MentionedAllocation& allocation, CellPointer prototype)
{
    StubCallResult result = callStub(context, AOT::allocateInstance(knownCellSizeFor(allocation.placed.size)), { m_out.wideNumber(allocation.mention), prototype.value() });
    return { CellPointer(result.first), Condition(m_out.notZero64(result.second)) };
}

static constexpr unsigned maxFieldBytesOfCellCopiedWithoutLoop = 64;

void CellAllocationEmitter::copyFieldWords(CellPointer source, CellPointer cell, unsigned fieldBytes)
{
    RELEASE_ASSERT(fieldBytes && !(fieldBytes % copyChunkBytes));
    const Machine::AbstractHeap& fields = m_heaps.entire(AbstractHeap::ObjectFields);
    auto copyWord = [&](LValue from, LValue to, ptrdiff_t offset) {
        m_out.store64(m_out.load64(m_out.address(fields, from, offset)), m_out.address(fields, to, offset));
    };
    if (fieldBytes <= maxFieldBytesOfCellCopiedWithoutLoop) {
        for (unsigned offset = offsetOfFirstField; offset < offsetOfFirstField + fieldBytes; offset += sizeof(uint64_t))
            copyWord(source.value(), cell.value(), offset);
        return;
    }

    LBasicBlock copyLoop = newBlock();
    LBasicBlock copyFields = newBlock();
    LBasicBlock copied = newBlock();
    LValue end = m_out.constIntPtr(offsetOfFirstField + fieldBytes);
    ValueFromBlock start = m_out.anchor(m_out.constIntPtr(offsetOfFirstField));
    m_out.jump(copyLoop);

    m_out.appendTo(copyLoop);
    LValue offset = m_out.phi(Machine::pointerType(), start);
    m_out.branch(m_out.below(offset, end), normalEdge(copyFields), normalEdge(copied));

    m_out.appendTo(copyFields);
    LValue from = m_out.add(source.value(), offset);
    LValue to = m_out.add(cell.value(), offset);
    copyWord(from, to, 0);
    copyWord(from, to, sizeof(uint64_t));
    m_out.addIncomingToPhi(offset, m_out.anchor(m_out.add(offset, copyChunkBytes)));
    m_out.jump(copyLoop);

    m_out.appendTo(copied);
}

void CellAllocationEmitter::storeWordInLoop(CellPointer cell, const Layout& layout, unsigned begin, unsigned end, int64_t word)
{
    RELEASE_ASSERT(begin < end && !((end - begin) % bytesStoredInTurnOfLoop));
    const Machine::AbstractHeap& fields = m_heaps.entire(layout.isEnvironment() ? AbstractHeap::EnvironmentFields : AbstractHeap::ObjectFields);
    LBasicBlock storeWords = newBlock();
    LBasicBlock stored = newBlock();
    LValue bits = m_out.opaque(m_out.constInt64(word));
    ValueFromBlock start = m_out.anchor(m_out.constIntPtr(begin));
    m_out.jump(storeWords);

    m_out.appendTo(storeWords);
    LValue offset = m_out.phi(Machine::pointerType(), start);
    LValue to = m_out.add(cell.value(), offset);
    for (unsigned place = 0; place < bytesStoredInTurnOfLoop; place += sizeof(uint64_t))
        m_out.store64(bits, m_out.address(fields, to, place));
    LValue next = m_out.add(offset, bytesStoredInTurnOfLoop);
    m_out.addIncomingToPhi(offset, m_out.anchor(next));
    m_out.branch(m_out.below(next, m_out.constIntPtr(end)), normalEdge(storeWords), normalEdge(stored));

    m_out.appendTo(stored);
}

void CellAllocationEmitter::copyWords(CellPointer source, unsigned offsetInSource, CellPointer cell, unsigned offsetInCell, unsigned bytes)
{
    static constexpr unsigned bytesCopiedInTurnOfLoop = 8 * sizeof(uint64_t);
    RELEASE_ASSERT(bytes && !(bytes % sizeof(uint64_t)));
    const Machine::AbstractHeap& fields = m_heaps.entire(AbstractHeap::ObjectFields);
    auto copyWord = [&](LValue from, LValue to, unsigned place) {
        m_out.store64(m_out.load64(m_out.address(fields, from, offsetInSource + place)), m_out.address(fields, to, offsetInCell + place));
    };
    unsigned bytesInLoop = bytes > maxFieldBytesOfCellCopiedWithoutLoop ? bytes / bytesCopiedInTurnOfLoop * bytesCopiedInTurnOfLoop : 0;
    if (bytesInLoop) {
        LBasicBlock copyTurn = newBlock();
        LBasicBlock copied = newBlock();
        ValueFromBlock start = m_out.anchor(m_out.constIntPtr(0));
        m_out.jump(copyTurn);

        m_out.appendTo(copyTurn);
        LValue offset = m_out.phi(Machine::pointerType(), start);
        LValue from = m_out.add(source.value(), offset);
        LValue to = m_out.add(cell.value(), offset);
        for (unsigned place = 0; place < bytesCopiedInTurnOfLoop; place += sizeof(uint64_t))
            copyWord(from, to, place);
        LValue next = m_out.add(offset, bytesCopiedInTurnOfLoop);
        m_out.addIncomingToPhi(offset, m_out.anchor(next));
        m_out.branch(m_out.below(next, m_out.constIntPtr(bytesInLoop)), normalEdge(copyTurn), normalEdge(copied));

        m_out.appendTo(copied);
    }
    for (unsigned place = bytesInLoop; place < bytes; place += sizeof(uint64_t))
        copyWord(source.value(), cell.value(), place);
}

CellPointer CellAllocationEmitter::allocateObjectCopy(const OperationContext& context, const MentionedAllocation& allocation, CellPointer source, unsigned fieldBytes)
{
    RELEASE_ASSERT(isObjectLayout(m_program.at(allocation.placed.layout).kind) && offsetOfFirstField + fieldBytes <= WTF::roundUpToMultipleOf<copyChunkBytes>(allocation.placed.size));
    CellPointer cell(callStub(context, allocateCell(CellInitialization::Header, knownCellSizeFor(allocation.placed.size)), { m_out.wideNumber(allocation.mention) }).first);
    {
        CodeGenerator::InitializationScope scope(generator());
        storeField(cell, m_heaps.AOTObject_orderWord, bitAnd(loadField<Word32>(source, m_heaps.AOTObject_orderWord), static_cast<int32_t>(orderAndCountMask)));
        storeField(cell, m_heaps.AOTObject_shapeWord, loadField<Word32>(source, m_heaps.AOTObject_shapeWord));
        copyFieldWords(source, cell, fieldBytes);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    return cell;
}

LValue CellAllocationEmitter::slotOfFrameCell(const OperationContext& context, size_t size)
{
    const Operation& made = operationAt(context.valueID());
    ValueID call = callThatBorrowsClosure(m_code, context.valueID());
    if (!call)
        call = callThatBorrowsObject(m_code, context.valueID());
    if (call) {
        for (SlotOfClosuresLent& shared : m_slotsOfClosuresLent) {
            if (shared.blockIndex == made.blockIndex && shared.call.index() > context.valueID().index())
                continue;
            shared.slot->ensureSize(size);
            shared.blockIndex = made.blockIndex;
            shared.call = call;
            return m_out.slotBase(shared.slot);
        }
        Machine::StackSlot* slot = m_proc.addStackSlot(size);
        slot->setIsDeadAtSuspensions(true);
        m_slotsOfClosuresLent.append(SlotOfClosuresLent { slot, made.blockIndex, call });
        return m_out.slotBase(slot);
    }
    LValue slot = m_out.lockedStackSlot(size);
    slot->slot()->setIsDeadAtSuspensions(true);
    return slot;
}

CellPointer CellAllocationEmitter::allocateInFrame(const OperationContext& context, size_t size)
{
    if (AOT::hasFlag(build().options().testing, Wire::TestingSwitch::DisableFrameCells))
        generator().fail(context, InternalErrorKind::OpcodeWithoutCode, "a cell lies in a frame in a build that keeps cells out of frames"_s);
    build().statistics().count(StatisticID::AllocateInFramesChanges);
    return CellPointer(slotOfFrameCell(context, size));
}

std::pair<CellPointer, Pointer> CellAllocationEmitter::allocateArray(const OperationContext& context, Word32 length, Word32 vectorLength, IndexingType indexingType, LBasicBlock slowPath)
{
    branchUnless(below(length, word32(1 << holeArrayCapacityShift)), slowPath);
    Word32 lengthAndCapacity = bitOr(length, shiftLeft(vectorLength, word32(holeArrayCapacityShift)));
    CellPointer array(callStub(context, newArrayWithLength(arrayElementKindOf(indexingType)), { zeroExtended(lengthAndCapacity).value() }).first);
    branchUnless(Condition(m_out.notNull(array.value())), slowPath);
    return { array, Pointer(m_out.loadPtr(array.value(), m_heaps.JSArray_butterfly)) };
}

CellPointer CellAllocationEmitter::allocateArrayOfLength(const OperationContext& context, IndexingType indexingType, unsigned length)
{
    static_assert((maxArrayLengthInStubKey | 1) <= maxHoleArrayCapacity && maxHoleArrayCapacity < 1 << holeArrayCapacityShift);
    RELEASE_ASSERT(length && length <= maxArrayLengthInStubKey);
    ArrayElementKind kind = arrayElementKindOf(indexingType);
    int32_t lengthAndCapacity = static_cast<int32_t>(length | (length | 1) << holeArrayCapacityShift);
    CellPointer array(callStub(context, newArrayWithLength(kind), { m_out.constInt64(lengthAndCapacity) }).first);
    return either<CellPointer>(Condition(m_out.notNull(array.value())), [&] {
        return array;
    }, [&] {
        return callOperation<CellPointer>(context, RuntimeOperation::NewArrayOfHoles, { word32(lengthAndCapacity), word32(indexingTypeOf(kind)) }, { .isOnRarePath = true });
    });
}

void CellAllocationEmitter::finishArrayOfLength(const OperationContext& context, CellPointer array, unsigned length)
{
    LBasicBlock isHavingABadTime = newBlock();
    LBasicBlock continuation = newBlock();
    Integer headerOfNewArrays(m_out.load64(instanceField(m_heaps.entire(AbstractHeap::AllocatorState), Instance::offsetOfNewArrayHeader(ArrayWithContiguous))));
    m_out.branch(isNotZero(headerOfNewArrays).value(), normalEdge(continuation), rareEdge(isHavingABadTime));

    m_out.appendTo(isHavingABadTime);
    callOperation<CellPointer>(context, RuntimeOperation::TruncateNewArray, { array, word32(static_cast<int32_t>(length)) }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

CellPointer CellAllocationEmitter::newArrayOfNumbers(const OperationContext& context, std::span<const Boxed> elements)
{
    Vector<LValue, maxArrayElementsInRegisters> operands;
    for (Boxed element : elements)
        operands.append(element.value());
    return CellPointer(callStub(context, arrayLiteralOfNumbers(elements.size()), operands.span()).first);
}

CellPointer CellAllocationEmitter::newArrayOf(const OperationContext& context, std::span<const Boxed> elements, bool hasPropertyRoom)
{
    unsigned length = static_cast<unsigned>(elements.size());
    if (length <= maxArrayElementsInRegisters) {
        Vector<LValue, maxArrayElementsInRegisters> operands;
        for (Boxed element : elements)
            operands.append(element.value());
        return CellPointer(callStub(context, hasPropertyRoom && length ? arrayLiteralWithRoom(length, false) : arrayLiteral(length), operands.span()).first);
    }
    if (length <= maxArrayLengthInStubKey) {
        CellPointer array = allocateArrayOfLength(context, ArrayWithContiguous, length);
        {
            CodeGenerator::InitializationScope scope(generator());
            LValue butterfly = m_out.loadPtr(array.value(), m_heaps.JSArray_butterfly);
            for (unsigned i = 0; i < length; ++i)
                m_out.store64(elements[i].value(), butterfly, m_heaps.indexedContiguousProperties[i]);
        }
        emitter<BarrierEmitter>().fenceAfterInitialization(context);
        finishArrayOfLength(context, array, length);
        return array;
    }

    unsigned vectorLength = std::max<unsigned>(length, BASE_CONTIGUOUS_VECTOR_LEN) | 1;
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    Word32 publicLength = word32(static_cast<int32_t>(length));
    auto [array, butterfly] = allocateArray(context, publicLength, word32(static_cast<int32_t>(vectorLength)), ArrayWithContiguous, slowPath);
    {
        CodeGenerator::InitializationScope scope(generator());
        for (unsigned i = 0; i < length; ++i)
            m_out.store64(elements[i].value(), butterfly.value(), m_heaps.indexedContiguousProperties[i]);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization(context);
    join.add(array);

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::NewArray, { word32(static_cast<int32_t>(length)), argumentBuffer(elements) }, { .isOnRarePath = true }));
    return join.finish();
}

CellPointer CellAllocationEmitter::newArrayFromBuffer(const OperationContext& context, Word32 count, Pointer buffer)
{
    return CellPointer(callStub(context, AOT::newArrayFromBuffer(), { count.value(), buffer.value() }).first);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
