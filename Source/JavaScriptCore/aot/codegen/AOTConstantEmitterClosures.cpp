/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConstantEmitter.h"

#if ENABLE(AOT)

#include "AOTAllocationEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTChooseCallingConventions.h"
#include "AOTClosure.h"
#include "AOTClosureTestEmitter.h"
#include "AOTConvention.h"
#include "AOTDerivedFunctions.h"
#include "AOTFunctionKinds.h"
#include "AOTFunctionTables.h"
#include "AOTInstance.h"
#include "AOTPlacement.h"
#include "AOTSlimClosure.h"
#include "AOTStubsClosures.h"
#include "AOTVector.h"
#include "MarkedSpace.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned slimClosureAllocatorIndex(unsigned numberOfCaptures)
{
    return MarkedSpace::sizeClassToIndex(SlimClosure::allocationSize(numberOfCaptures)) - 1;
}

static_assert(!slimClosureAllocatorIndex(0) && slimClosureAllocatorIndex(1) == 1 && slimClosureAllocatorIndex(2) == 1);
static_assert(slimClosureAllocatorIndex(ClosureRequest::maxSlimClosureCaptures) < ClosureWords::numberOfSlimClosureAllocators);
static_assert(ClosureRequest::maxNumberOfCaptures <= ClosureRequest::maxSlimClosureCaptures);

void ConstantEmitter::generateNewClosure(const ValueContext& value)
{
    defineCell(value, closureCreatedBy(value));
}

CellPointer ConstantEmitter::closureCreatedBy(const ValueContext& value)
{
    FunctionID function = value->function();
    std::span<const ValueID> captures = value->capturesOperands();
    RELEASE_ASSERT(captures.size() == m_program.at(function).captures.size() && captures.size() <= Closure::maxNumberOfCaptures);

    FunctionID entrypoint = inputs().derivedFunctions.find(derivedFunctionKey(FunctionRole::GenericEntrypoint, function));
    if (!entrypoint)
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);

    NewClosureRequest request { function, entrypoint, m_placement.functionKind(entrypoint), value->allocation(), value->storage(), value->closureForm(), captures };
    if (value->isLent() && (request.form != ClosureForm::Slim || request.storage != AllocationStorage::Frame))
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);
    switch (request.form) {
    case ClosureForm::Slim:
        return newSlimClosure(value, request);
    case ClosureForm::Full:
        if (request.storage == AllocationStorage::Heap && !request.allocation)
            return failWithoutCell(value, InternalErrorKind::PlacedPayloadMissing);
        if ((value.shape() != CodeShape::Fast || request.storage == AllocationStorage::Heap) && canUseClosureStub(request))
            return newClosureByStub(value, request);
        return newFullClosure(value, request);
    case ClosureForm::Deferred:
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);
    case ClosureForm::Singleton:
        return singletonClosure(value, function);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CellPointer ConstantEmitter::singletonClosure(const OperationContext& context, FunctionID function)
{
    FunctionID entrypoint = function ? inputs().derivedFunctions.find(derivedFunctionKey(FunctionRole::GenericEntrypoint, function)) : FunctionID();
    if (!entrypoint)
        return failWithoutCell(context, InternalErrorKind::OpcodeWithoutCode);
    if (!m_placement.closureCellNumber(entrypoint))
        return failWithoutCell(context, InternalErrorKind::PlacedPayloadMissing);
    if (context.shape() != CodeShape::Fast) {
        LValue operands[] = { integer(static_cast<int64_t>(entrypoint.index())).value() };
        return CellPointer(callStub(context, closureOfTable(), operands).first);
    }
    CellPointer created = emitter<ClosureTestEmitter>().closureIfCreated(entrypoint);
    return either<CellPointer>(isNotEmpty(boxCell(created)), [&] {
        return created;
    }, [&] {
        // The operation that takes the closure declares no write, so a call with its effects would let B3 reuse the empty word of the closure table.
        return callWithoutSite<CellPointer>(RuntimeOperation::SingletonClosure, { word32(static_cast<int32_t>(entrypoint.index())) }, { .isOnRarePath = true });
    });
}

bool ConstantEmitter::canUseClosureStub(const NewClosureRequest& request) const
{
    if (request.kind.closureStructure || request.captures.size() > ClosureRequest::maxNumberOfCaptures)
        return false;
    return std::ranges::none_of(request.captures, [&](ValueID capture) {
        return b3TypeOf(representationOf(capture)) == B3::Double;
    });
}

CellPointer ConstantEmitter::newSlimClosure(const ValueContext& value, const NewClosureRequest& request)
{
    unsigned count = request.captures.size();
    if (count > ClosureRequest::maxSlimClosureCaptures)
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);

    uint64_t requestWord = request.entrypoint.index() | static_cast<uint64_t>(request.kind.slimClosureKind()) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit;
    if (value.shape() != CodeShape::Fast || request.storage != AllocationStorage::Frame)
        return callClosureStub(value, request, requestWord, SlimClosure::allocationSize(count));

    CellPointer cell = emitter<CellAllocationEmitter>().allocateInFrame(SlimClosure::allocationSize(count));
    CodeGenerator::InitializationScope scope(generator());
    emitter<CellAllocationEmitter>().storeHeader(cell, loadImmutable64(AOT_INSTANCE_OFFSET(slimClosureHeaders) + request.kind.slimClosureKind() * sizeof(uint64_t)));
    storeCaptures(cell, request, m_heaps.AOTSlimClosure_captures);
    storeField(cell, m_heaps.AOTSlimClosure_word, closureWordOf(request.entrypoint));
    return cell;
}

CellPointer ConstantEmitter::newClosureByStub(const ValueContext& value, const NewClosureRequest& request)
{
    unsigned executable = static_cast<unsigned>(FunctionTables::executableKind(request.kind));
    uint32_t allocation = request.storage == AllocationStorage::Heap ? request.allocation.index() : 0;
    if (!ClosureRequest::canEncode(request.entrypoint.index(), executable, allocation)) {
        generator().failAtLimit(value, LimitID::ClosureAllocationSites, static_cast<uint64_t>(allocation) + 1);
        return zeroOf(Representation::Cell).as<CellPointer>();
    }
    return callClosureStub(value, request, ClosureRequest::encode(request.entrypoint.index(), executable, allocation), Closure::allocationSize(request.captures.size()));
}

CellPointer ConstantEmitter::callClosureStub(const ValueContext& value, const NewClosureRequest& request, uint64_t requestWord, size_t cellSize)
{
    unsigned count = request.captures.size();
    Vector<LValue, 6> operands;
    operands.append(integer(static_cast<int64_t>(requestWord)).value());
    operands.appendVector(captureWordsBySlot(request));
    switch (request.storage) {
    case AllocationStorage::Heap:
        if (ClosureRequest::isSlim(requestWord)) {
            operands[0] = integer(static_cast<int64_t>(request.entrypoint.index())).value();
            return CellPointer(callStub(value, AOT::newSlimClosure(request.kind.slimClosureKind(), count), operands.span()).first);
        }
        return CellPointer(callStub(value, newClosure(count), operands.span()).first);
    case AllocationStorage::Frame:
        operands.append(emitter<CellAllocationEmitter>().allocateInFrame(cellSize).value());
        return CellPointer(callStub(value, initializeClosure(count), operands.span()).first);
    case AllocationStorage::FrameUntilMaterialized:
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CellPointer ConstantEmitter::newFullClosure(const ValueContext& value, const NewClosureRequest& request)
{
    CellAllocationEmitter& cellAllocation = emitter<CellAllocationEmitter>();
    unsigned structure = request.kind.closureStructure;
    unsigned executable = static_cast<unsigned>(FunctionTables::executableKind(request.kind));
    bool isOnHeap = false;
    CellPointer cell;
    switch (request.storage) {
    case AllocationStorage::Heap: {
        std::optional<WholeCell> ownCell = emitter<AllocationEmitter>().placeInCellOf(value).ownCell();
        if (!ownCell)
            return failWithoutCell(value, InternalErrorKind::PlacedPayloadMissing);
        cell = cellAllocation.allocate(value, request.allocation, *ownCell);
        isOnHeap = true;
        break;
    }
    case AllocationStorage::Frame:
        cell = cellAllocation.allocateInFrame(Closure::allocationSize(request.captures.size()));
        break;
    case AllocationStorage::FrameUntilMaterialized:
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);
    }

    {
        CodeGenerator::InitializationScope scope(generator());
        if (!isOnHeap || structure)
            cellAllocation.storeHeader(cell, loadImmutable64(AOT_INSTANCE_OFFSET(closures.closureHeaders) + structure * sizeof(uint64_t)));
        storeField(cell, m_heaps.JSObject_butterfly, integer(0));
        storeField(cell, m_heaps.JSCallee_scope, loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject)));
        storeField(cell, m_heaps.JSFunction_executableOrRareData, loadImmutablePointer(AOT_INSTANCE_OFFSET(closures.executables) + executable * sizeof(JSCell*)));
        storeCaptures(cell, request, m_heaps.AOTClosure_captures);
        storeField(cell, m_heaps.AOTClosure_entry, closureWordOf(request.entrypoint));
    }
    if (isOnHeap)
        emitter<BarrierEmitter>().fenceAfterInitialization();
    return cell;
}

Operand ConstantEmitter::captureOf(const NewClosureRequest& request, unsigned index) const
{
    Operand capture = get(request.captures[index]);
    RELEASE_ASSERT(capture.representation == declaredRepresentationOf(m_program, m_program.at(request.function).captures[index]));
    return capture;
}

Vector<LValue, 4> ConstantEmitter::captureWordsBySlot(const NewClosureRequest& request)
{
    const Function& callee = m_program.at(request.function);
    unsigned count = request.captures.size();
    Vector<LValue, 4> words(FillWith { }, count, nullptr);
    for (unsigned i = 0; i < count; ++i) {
        Operand capture = captureOf(request, i);
        LType type = b3TypeOf(capture.representation);
        LValue word = type == B3::Int32 ? m_out.zeroExtPtr(capture.value) : type == B3::Double ? m_out.bitCast(capture.value, B3::Int64) : capture.value;
        words[closureCaptureSlotOf(m_program, callee, i)] = word;
    }
    return words;
}

void ConstantEmitter::storeCaptures(CellPointer closure, const NewClosureRequest& request, B3::IndexedAbstractHeap& captureWords)
{
    const Function& callee = m_program.at(request.function);
    for (unsigned i = 0; i < request.captures.size(); ++i) {
        if (request.storage == AllocationStorage::Frame && build().recordedOptions().isSabotaged(Sabotage::ClosureInFrameKeepsNoCaptures))
            break;
        Operand capture = captureOf(request, i);
        LValue stored = b3TypeOf(capture.representation) == B3::Int32 ? m_out.zeroExtPtr(capture.value) : capture.value;
        m_out.store(stored, m_out.address(closure.value(), captureWords[closureCaptureSlotOf(m_program, callee, i)]));
    }
}

Integer ConstantEmitter::closureWordOf(FunctionID entrypoint)
{
    Pointer closureWords = loadImmutablePointer(AOT_INSTANCE_OFFSET(functionTables.closureWords));
    return Integer(m_out.load64(m_out.address(m_heaps.AOTImage_tables, closureWords.value(), static_cast<ptrdiff_t>(entrypoint.index()) * sizeof(uint64_t))));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
