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
#include "AOTStubsCallFrames.h"
#include "AOTStubsClosures.h"
#include "AirStackSlot.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"

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

// Stack words of the tail call from an entrypoint into its function, if that call needs an argument list frame.
std::optional<uint32_t> EnvironmentEmitter::stackWordsOfHandOverFromListFrame() const
{
    const CallingConventions& conventions = inputs().conventions;
    std::optional<uint32_t> words;
    for (const Block& block : m_function.blocks) {
        if (block.terminator.kind != TerminatorKind::ReturnCall)
            continue;
        const Value& call = m_function.at(block.terminator.callOperand());
        if (call.opcode != Opcode::CallDirect)
            return std::nullopt;
        uint32_t passed = conventions.stackWordsReservedByCallersOf(m_program, call.function());
        if (words && *words != passed)
            return std::nullopt;
        words = passed;
    }
    if (!words || !*words || (hasStackArgumentAreaOf(m_function.role) && *words <= conventions.stackWordsReservedByCallersOf(m_program, m_id)))
        return std::nullopt;
    return words;
}

void EnvironmentEmitter::generateRecordCallee(const ValueContext& value)
{
    CellPointer closure = in<CellPointer>(value->closureOperand());
    unsigned spilledBytes = inputs().conventions.of(m_program, m_id).spillsArgumentRegisters ? sizeOfSpilledArgumentRegisters : 0;
    std::optional<uint32_t> words = stackWordsOfHandOverFromListFrame();
    // On arm64 the stub takes fewer instructions than the record in line, which moves the arguments out of its way. On x64 it takes as many, and a few cycles more. There its caller needs no frame: an entrypoint that does nothing else goes in front of the function.
    if ((isARM64() || value.shape() != CodeShape::Fast) && !spilledBytes && !words) {
        // Whoever names the frame pointer has a frame. Where the function has none, nothing lies on the stack but what the call by value left there.
        B3::PatchpointValue* stackPointerOfCaller = m_out.patchpoint(B3::pointerType());
        stackPointerOfCaller->effects = B3::Effects::none();
        stackPointerOfCaller->setRunsWithoutFrame(true);
        stackPointerOfCaller->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            if (params.context().frameIsSetUp)
                jit.lea64(params.code().addressInFrame(sizeof(CallerFrameAndPC)), params[0].gpr());
            else
                jit.addPtr(CCallHelpers::TrustedImm32(isX86_64() ? sizeof(void*) : 0), CCallHelpers::stackPointerRegister, params[0].gpr());
        });
        callStub(value, recordCallee(), { closure.value(), stackPointerOfCaller }, { .accessesCollectorState = true });
        return;
    }
    LValue stackPointerOfCaller;
    if (words) {
        // The function is called from the list frame, so a stack walk looks for the record under the stack pointer of that call.
        B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::pointerType());
        patchpoint->numGPScratchRegisters = 1;
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        patchpoint->effects = B3::Effects::none();
        patchpoint->effects.reads = B3::HeapRange::top();
        patchpoint->setNeedsFrameRecord(true);
        patchpoint->setGenerator([numberOfWords = *words, spilledBytes](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            emitFindStackPointerOfArgumentListFrame(jit, params[0].gpr(), params.gpScratch(0), numberOfWords, spilledBytes);
        });
        stackPointerOfCaller = patchpoint;
    } else
        stackPointerOfCaller = m_out.add(m_out.framePointer(), static_cast<ptrdiff_t>(sizeof(CallerFrameAndPC) + spilledBytes));
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
    callVoidOperation(value, RuntimeOperation::RecordCallee, { closure, Pointer(stackPointerOfCaller) }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void EnvironmentEmitter::generateKeepEnvironmentOfCalleeInFrame(const ValueContext& value)
{
    const PlacedSlot& slot = m_placement.slot(value->layout(), value->field());
    bool isStored = slot.hasStorage() && !slot.needsExtension();
    m_records.sites.fieldsOfCallees.set(SiteRecords::keyOfCall(value->position, value->inlineFrame), isStored ? std::optional(slot.data().encoded()) : std::nullopt);
    if (isStored)
        keepInFrame(value, in<CellPointer>(value->environmentOperand()));
}

void EnvironmentEmitter::keepInFrame(const ValueContext& value, CellPointer kept)
{
    uint32_t depth = SiteRecords::wordOfCalleeCalledFrom(m_function, value->inlineFrame);
    auto& words = m_records.sites.wordsOfCallees;
    while (words.size() <= depth)
        words.append(nullptr);
    if (!words[depth]) {
        words[depth] = m_proc.addStackSlot(sizeof(uint64_t));
        words[depth]->setIsDeadAtSuspensions(true);
    }
    LValue word = m_out.m_block->appendNew<B3::SlotBaseValue>(m_proc, m_out.origin(), words[depth]);
    m_out.store64(kept.value(), m_out.address(m_heaps.frameSlots, word, 0));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
