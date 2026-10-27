/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEnvironmentEmitter.h"

#if ENABLE(AOT)

#include "AOTChooseCallingConventions.h"
#include "AOTConstantEmitter.h"
#include "AOTFunctionKinds.h"
#include "AOTPlacement.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTStaticsEmitter.h"
#include "AOTStubsClosures.h"

namespace JSC { namespace AOT { namespace Typed {

static bool canBeCopiedOutOfFrame(const PlannedLayout& layout)
{
    return layout.hasSlots() && !layout.extensionSize && layout.size > sizeof(JSCell) && !(layout.size % sizeof(uint64_t));
}

Operand EnvironmentEmitter::closureCapture(CellPointer closure, FunctionID function, unsigned index)
{
    const Function& callee = m_program.at(function);
    Representation representation = declaredRepresentationOf(m_program, callee.captures[index]);
    B3::IndexedAbstractHeap& captureWords = m_placement.functionKind(function).isSlim ? m_heaps.AOTSlimClosure_captures : m_heaps.AOTClosure_captures;
    TypedPointer word = m_out.address(closure.value(), captureWords[closureCaptureSlotOf(m_program, callee, index)]);
    return Operand(representation, m_out.load(word, b3TypeOf(representation)));
}

void EnvironmentEmitter::generateGetCapture(const ValueContext& value)
{
    define(value, closureCapture(in<CellPointer>(value->closureOperand()), value->function(), value->index()));
}

void EnvironmentEmitter::generateMaterializeEnvironment(const ValueContext& value)
{
    AllocationID allocation = value->allocation();
    const PlannedLayout& layout = m_placement.layout(value->layout());
    if (!allocation || !canBeCopiedOutOfFrame(layout)) {
        fail(value, allocation ? InternalErrorKind::SlotAccessNotPlaced : InternalErrorKind::PlacedPayloadMissing);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    CellPointer environment = in<CellPointer>(value->environmentOperand());
    uint64_t allocationAndSize = allocation.index() | static_cast<uint64_t>(layout.size) << 32;
    define(value, CellPointer(callStub(value, materializeEnvironment(), { environment.value(), m_out.constInt64(static_cast<int64_t>(allocationAndSize)) }).first));
}

TypedPointer EnvironmentEmitter::escapeMarkOf(CellPointer environment, uint32_t isEscapedOffset)
{
    return m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), environment.value(), isEscapedOffset);
}

void EnvironmentEmitter::generateMarkEnvironmentEscaped(const ValueContext& value)
{
    ValueID environment = value->environmentOperand();
    std::optional<uint32_t> isEscapedOffset = m_placement.layout(typeOf(environment).environment).isEscapedOffset;
    if (!isEscapedOffset)
        return;
    LValue isEscaped = m_out.constInt32(1);
    m_out.store32As8(isEscaped, escapeMarkOf(in<CellPointer>(environment), *isEscapedOffset));
}

void EnvironmentEmitter::generateReleaseEnvironment(const ValueContext& value)
{
    LayoutID layout = typeOf(value->environmentOperand()).environment;
    std::optional<uint32_t> isEscapedOffset = m_placement.layout(layout).isEscapedOffset;
    std::optional<uint32_t> freeCellOffset = m_placement.freeCellOffset(layout);
    if (!isEscapedOffset || !freeCellOffset)
        return;

    CellPointer environment = in<CellPointer>(value->environmentOperand());
    LBasicBlock release = newBlock();
    LBasicBlock continuation = newBlock();
    Word32 hasEscaped(m_out.load8ZeroExt32(escapeMarkOf(environment, *isEscapedOffset)));
    m_out.branch(isZero(hasEscaped).value(), normalEdge(release), normalEdge(continuation));

    m_out.appendTo(release);
    if (generator().validates())
        emitter<RuntimeValidationEmitter>().poisonEnvironment(environment, layout);
    m_out.store64(environment.value(), emitter<StaticsEmitter>().inStatics(m_heaps.collectorState, *freeCellOffset));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void EnvironmentEmitter::generateRecordCallee(const ValueContext& value)
{
    CellPointer closure = in<CellPointer>(value->closureOperand());
    ptrdiff_t bytesAboveFrameRecord = sizeof(CallerFrameAndPC) + (inputs().conventions.of(m_program, m_id).spillsArgumentRegisters ? sizeOfSpilledArgumentRegisters : 0);
    LValue stackPointerOfCaller = m_out.add(m_out.framePointer(), bytesAboveFrameRecord);
    const B3::AbstractHeap& heap = m_heaps.collectorState;
    LValue entries = m_out.loadPtr(instanceField(heap, AOT_INSTANCE_OFFSET(heap.recordedCallees)));

    LBasicBlock write = newBlock();
    LBasicBlock callRuntime = newBlock();
    LBasicBlock continuation = newBlock();
    constexpr ptrdiff_t offsetOfMask = OBJECT_OFFSETOF(RecordedCallee, stackPointerOfCaller) - static_cast<ptrdiff_t>(sizeof(RecordedCallee));
    LValue entry = m_out.add(entries, m_out.bitAnd(stackPointerOfCaller, m_out.loadPtr(m_out.address(heap, entries, offsetOfMask))));
    // The call that was made from further out may not have returned.
    m_out.branch(m_out.above(m_out.loadPtr(m_out.address(heap, entry, OBJECT_OFFSETOF(RecordedCallee, stackPointerOfCaller))), stackPointerOfCaller), rareEdge(callRuntime), normalEdge(write));

    m_out.appendTo(write);
    m_out.storePtr(stackPointerOfCaller, m_out.address(heap, entry, OBJECT_OFFSETOF(RecordedCallee, stackPointerOfCaller)));
    m_out.storePtr(closure.value(), m_out.address(heap, entry, OBJECT_OFFSETOF(RecordedCallee, closure)));
    m_out.jump(continuation);

    m_out.appendTo(callRuntime);
    callVoidOperation(value, RuntimeOperation::RecordCallee, { closure }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
