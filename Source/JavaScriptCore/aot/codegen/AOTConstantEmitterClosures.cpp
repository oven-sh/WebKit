/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConstantEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTChooseCallingConventions.h"
#include "AOTClosure.h"
#include "AOTClosureTestEmitter.h"
#include "AOTConvention.h"
#include "AOTDerivedFunctions.h"
#include "AOTFlowResults.h"
#include "AOTFunctionKinds.h"
#include "AOTFunctionTables.h"
#include "AOTGuardEmitter.h"
#include "AOTInstance.h"
#include "AOTPlacement.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTShapeTestEmitter.h"
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
    if (context.shape() != CodeShape::Fast)
        return CellPointer(callStub(context, closureOfTable(), { }, { .immediate = m_placement.closureNumberOf(entrypoint) }).first);
    CellPointer created = emitter<ClosureTestEmitter>().closureIfCreated(entrypoint);
    return either<CellPointer>(isNotEmpty(boxCell(created)), [&] {
        return created;
    }, [&] {
        // The operation that takes the closure declares no write, so a call with its effects would let B3 reuse the empty word of the closure table.
        return CellPointer(emitter<RuntimeCallEmitter>().callStubWithoutSite(closureOfTable(), std::span<const LValue>(), { .immediate = m_placement.closureNumberOf(entrypoint), .isOnRarePath = true, .convertsNoCells = m_placement.functionKind(entrypoint).isSlim }).first);
    });
}

CellPointer ConstantEmitter::makeClosureOfField(const OperationContext& context, std::span<const DeferredFunction> functions, CellPointer object, uint32_t offsetOfSlot, bool capturesObject, std::optional<uint32_t> offsetOfEnvironment, NOESCAPE const ScopedLambda<CellPointer()>& makeInRunTime)
{
    // One stub makes the closures of one kind. Which function a closure is of is a number in the request.
    struct Made {
        const DeferredFunction* function;
        uint32_t closureNumber;
    };
    Vector<Made, 4> made;
    std::optional<unsigned> slimClosureKind;
    ASSERT(!capturesObject || !offsetOfEnvironment);
    bool hasCapture = capturesObject || offsetOfEnvironment;
    if (offsetOfEnvironment && std::max(offsetOfSlot, *offsetOfEnvironment) > std::numeric_limits<uint16_t>::max())
        return makeInRunTime();
    for (const DeferredFunction& deferred : functions) {
        const Function& callee = m_program.at(deferred.function);
        FunctionID entrypoint = inputs().derivedFunctions.find(derivedFunctionKey(FunctionRole::GenericEntrypoint, deferred.function));
        // A class with one class object has one closure, which the run time keeps.
        if (!entrypoint || callee.captures.size() != hasCapture || m_placement.classConstructedBy(entrypoint) || m_placement.classConstructedBy(deferred.function))
            continue;
        FunctionKindData kind = m_placement.functionKind(entrypoint);
        if (!kind.isSlim || kind.isConstructor || (slimClosureKind && *slimClosureKind != kind.slimClosureKind()))
            continue;
        if (hasCapture) {
            Representation representation = declaredRepresentationOf(m_program, callee.captures[0]);
            if (representation != Representation::Cell && representation != Representation::JSValue)
                continue;
        }
        slimClosureKind = kind.slimClosureKind();
        made.append({ &deferred, m_placement.closureNumberOf(entrypoint) });
    }
    if (made.isEmpty())
        return makeInRunTime();

    ShapeTestEmitter& shapes = emitter<ShapeTestEmitter>();
    std::optional<Word32> shapeWord;
    auto isOf = [&](const DeferredFunction& deferred) {
        if (!shapeWord)
            shapeWord = shapes.shapeWordOf(object);
        Condition result;
        for (LayoutID layout : deferred.layouts)
            result = logicalOr(result, shapes.isShapeOf(*shapeWord, layout));
        return result;
    };
    auto makeInStub = [&] {
        Word32 closureNumber = word32(made.last().closureNumber);
        for (size_t index = 0; index + 1 < made.size(); ++index)
            closureNumber = select(isOf(*made[index].function), word32(made[index].closureNumber), closureNumber);
        uint64_t offsets = static_cast<uint64_t>(offsetOfSlot) << 32 | static_cast<uint64_t>(offsetOfEnvironment.value_or(0)) << 48;
        Integer request = bitOr(zeroExtended(closureNumber), integer(static_cast<int64_t>(offsets)));
        StubKey stub = offsetOfEnvironment ? makeClosureOfFieldOverEnvironment(*slimClosureKind) : AOT::makeClosureOfField(*slimClosureKind, capturesObject);
        return CellPointer(callStub(context, stub, { request.value(), object.value() }, { .isOnRarePath = true, .accessesCollectorState = true }).first);
    };
    Condition isMadeInStub;
    if (made.size() != functions.size()) {
        for (const Made& one : made)
            isMadeInStub = logicalOr(isMadeInStub, isOf(*one.function));
    }
    // The stub makes a closure of this instance. The closure of a field is of the instance of its object.
    if (inputs().flow.programInstances() != ProgramInstances::AtMostOne) {
        Condition belongs = emitter<GuardEmitter>().belongsToInstance(object);
        isMadeInStub = isMadeInStub ? logicalAnd(isMadeInStub, belongs) : belongs;
    }
    if (!isMadeInStub)
        return makeInStub();
    return either<CellPointer>(isMadeInStub, makeInStub, makeInRunTime);
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

    uint64_t requestWord = m_placement.closureNumberOf(request.entrypoint) | static_cast<uint64_t>(request.kind.slimClosureKind()) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit;
    if (value.shape() != CodeShape::Fast || request.storage != AllocationStorage::Frame)
        return callClosureStub(value, request, requestWord, SlimClosure::allocationSize(count));

    CellPointer cell = emitter<CellAllocationEmitter>().allocateInFrame(value, SlimClosure::allocationSize(count));
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
    uint32_t closureNumber = m_placement.closureNumberOf(request.entrypoint);
    if (!ClosureRequest::canEncode(closureNumber, executable, allocation)) {
        generator().failAtLimit(value, LimitID::ClosureAllocationSites, static_cast<uint64_t>(allocation) + 1);
        return zeroOf(Representation::Cell).as<CellPointer>();
    }
    return callClosureStub(value, request, ClosureRequest::encode(closureNumber, executable, allocation), Closure::allocationSize(request.captures.size()));
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
            operands[0] = integer(static_cast<int64_t>(ClosureRequest::closureNumberOf(requestWord))).value();
            return CellPointer(callStub(value, AOT::newSlimClosure(request.kind.slimClosureKind(), count), operands.span()).first);
        }
        return CellPointer(callStub(value, newClosure(count), operands.span()).first);
    case AllocationStorage::Frame:
        operands.append(emitter<CellAllocationEmitter>().allocateInFrame(value, cellSize).value());
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
    case AllocationStorage::Heap:
        cell = cellAllocation.allocate(value, request.allocation);
        isOnHeap = true;
        break;
    case AllocationStorage::Frame:
        cell = cellAllocation.allocateInFrame(value, Closure::allocationSize(request.captures.size()));
        break;
    case AllocationStorage::FrameUntilMaterialized:
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);
    }

    {
        CodeGenerator::InitializationScope scope(generator());
        if (!isOnHeap || structure)
            cellAllocation.storeHeader(cell, loadImmutable64(AOT_INSTANCE_OFFSET(closures.closureHeaders) + structure * sizeof(uint64_t)));
        storeField(cell, m_heaps.JSNonArray_butterfly, integer(0));
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
    return Integer(m_out.load64(m_out.address(m_heaps.AOTImage_tables, closureWords.value(), static_cast<ptrdiff_t>(m_placement.closureNumberOf(entrypoint)) * sizeof(uint64_t))));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
