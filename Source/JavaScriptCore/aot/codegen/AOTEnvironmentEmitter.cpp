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
    m_out.store32As8(m_out.constInt32(1), escapeMarkOf(in<CellPointer>(environment), *isEscapedOffset));
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
    LValue stackPointerOfCaller = m_out.add(m_out.framePointer(), m_out.constIntPtr(bytesAboveFrameRecord));
    const B3::AbstractHeap& heap = m_heaps.collectorState;
    LValue list = m_out.loadPtr(instanceField(heap, AOT_INSTANCE_OFFSET(heap.recordedCallees)));
    constexpr ptrdiff_t offsetOfLast = -static_cast<ptrdiff_t>(sizeof(RecordedCallee));

    LBasicBlock loop = newBlock();
    LBasicBlock replaceLast = newBlock();
    LBasicBlock isNotLast = newBlock();
    LBasicBlock liesBelowLast = newBlock();
    LBasicBlock liesAboveLast = newBlock();
    LBasicBlock dropLast = newBlock();
    LBasicBlock add = newBlock();
    LBasicBlock callRuntime = newBlock();
    LBasicBlock continuation = newBlock();
    ValueFromBlock topAtEntry = m_out.anchor(m_out.loadPtr(m_out.address(heap, list, OBJECT_OFFSETOF(RecordedCalleeList, top))));
    m_out.jump(loop);

    m_out.appendTo(loop);
    LValue top = m_out.phi(B3::pointerType(), topAtEntry);
    LValue stackPointerOfLast = m_out.loadPtr(m_out.address(heap, top, offsetOfLast + OBJECT_OFFSETOF(RecordedCallee, stackPointerOfCaller)));
    m_out.branch(m_out.equal(stackPointerOfLast, stackPointerOfCaller), normalEdge(replaceLast), normalEdge(isNotLast));

    // The call that the last entry stands for has returned to where this one comes from.
    m_out.appendTo(replaceLast);
    m_out.storePtr(closure.value(), m_out.address(heap, top, offsetOfLast + OBJECT_OFFSETOF(RecordedCallee, closure)));
    m_out.storePtr(top, m_out.address(heap, list, OBJECT_OFFSETOF(RecordedCalleeList, top)));
    m_out.jump(continuation);

    m_out.appendTo(isNotLast);
    m_out.branch(m_out.above(stackPointerOfLast, stackPointerOfCaller), normalEdge(liesBelowLast), normalEdge(liesAboveLast));

    // So has one that was made from further down. The entry in front of the first has no stack pointer.
    m_out.appendTo(liesAboveLast);
    m_out.branch(m_out.isNull(stackPointerOfLast), rareEdge(callRuntime), normalEdge(dropLast));

    m_out.appendTo(dropLast);
    m_out.addIncomingToPhi(top, m_out.anchor(m_out.add(top, m_out.constIntPtr(offsetOfLast))));
    m_out.jump(loop);

    m_out.appendTo(liesBelowLast);
    m_out.branch(m_out.notEqual(top, m_out.loadPtr(m_out.address(heap, list, OBJECT_OFFSETOF(RecordedCalleeList, end)))), normalEdge(add), rareEdge(callRuntime));

    m_out.appendTo(add);
    m_out.storePtr(stackPointerOfCaller, m_out.address(heap, top, OBJECT_OFFSETOF(RecordedCallee, stackPointerOfCaller)));
    m_out.storePtr(closure.value(), m_out.address(heap, top, OBJECT_OFFSETOF(RecordedCallee, closure)));
    m_out.storePtr(m_out.add(top, m_out.constIntPtr(sizeof(RecordedCallee))), m_out.address(heap, list, OBJECT_OFFSETOF(RecordedCalleeList, top)));
    m_out.jump(continuation);

    m_out.appendTo(callRuntime);
    callVoidOperation(value, RuntimeOperation::RecordCallee, { closure });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
