/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "CodeBlock.h"
#include "FrameTracers.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

ALWAYS_INLINE FunctionRef caller(Instance* instance, CallFrame* callFrame) { return FunctionRef::at(instance, removeCodePtrTag(callFrame->rawReturnPC())); }
ALWAYS_INLINE FunctionRef callerBytecodeOwner(Instance* instance, CallFrame* callFrame)
{
    FunctionRef function = caller(instance, callFrame);
    if (!function.info().function()->hasInlineFrames) [[likely]]
        return function;
    return function.locationForReturnAddress(removeCodePtrTag(callFrame->rawReturnPC()), callFrame->callerFrame()).function;
}
ALWAYS_INLINE BytecodeIndex callerBytecodeIndex(Instance* instance, CallFrame* callFrame) { return caller(instance, callFrame).bytecodeIndexAt(removeCodePtrTag(callFrame->rawReturnPC()), callFrame->callerFrame()); }

ALWAYS_INLINE void countOperationNamed(Instance* instance, const char* name, const char* detail = nullptr)
{
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(*instance->vm).countOperation(name, detail);
}

ALWAYS_INLINE void countOperationBySlotState(Instance* instance, const char* name, const Slot* slot)
{
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(*instance->vm).countOperationBySlotState(name, slot);
}

ALWAYS_INLINE void countOperationFor(Instance* instance, CallFrame* callFrame)
{
    constexpr uint32_t samplingInterval = 8;
    instance->operationSamplingState = instance->operationSamplingState * 1664525u + 1013904223u;
    static_assert(samplingInterval == 1u << 3);
    if (instance->operationSamplingState >> (32 - 3)) [[likely]]
        return;
    FunctionRef function = caller(instance, callFrame);
    if (!function.instance->dataIfExists(function.index)) [[unlikely]]
        function.instance->countMisses(function.index, samplingInterval);
}

#define AOT_OPERATION_BEGIN(instance) \
    JSGlobalObject* globalObject = (instance)->globalObject; \
    UNUSED_VARIABLE(globalObject); \
    VM& vm = *(instance)->vm; \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    ++(instance)->effectEpoch; \
    countOperationFor(instance, callFrame); \
    countOperationNamed(instance, __func__); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

#define AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance) \
    JSGlobalObject* globalObject = (instance)->globalObject; \
    UNUSED_VARIABLE(globalObject); \
    VM& vm = *(instance)->vm; \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    ++(instance)->effectEpoch; \
    countOperationNamed(instance, __func__); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

ALWAYS_INLINE Data* callerData(Instance* instance, CallFrame* callFrame)
{
    FunctionRef function = caller(instance, callFrame);
    Data* data = function.instance->dataIfExists(function.index);
    return data ? data : function.instance->sharedData;
}
ALWAYS_INLINE UnlinkedCodeBlock* callerCode(Instance* instance, CallFrame* callFrame) { return callerBytecodeOwner(instance, callFrame).ensureUnlinkedCodeBlock(); }

ALWAYS_INLINE void didFillSlot(VM&, Data* data)
{
    if (data == SharedData::get())
        return;
    data->slotEpoch++;
    if (!data->hasBeenFilledSinceLastCollection)
        data->noteFilled();
}
ALWAYS_INLINE const Identifier& identifierAt(Instance* instance, CallFrame* callFrame, unsigned index) { UNUSED_PARAM(callFrame); return instance->program->identifierAsIdentifier(index); }
ALWAYS_INLINE FunctionRef callerBytecodeOwner(Instance* instance, CallFrame* callFrame, uint32_t whose)
{
    FunctionRef function = caller(instance, callFrame);
    if (!whose) [[likely]]
        return function;
    return { function.instance, function.info().function()->knownCallees()[whose - 1] };
}
ALWAYS_INLINE PutPropertySlot::Context putByIdContextOf(Instance* instance, CallFrame* callFrame) { return caller(instance, callFrame).codeType() == EvalCode ? PutPropertySlot::PutByIdEval : PutPropertySlot::PutById; }

template<typename CellType>
ALWAYS_INLINE void prepareInlineAllocation(Instance* instance, Instance::InlineAllocation kind, Structure* structure)
{
    auto& data = instance->inlineAllocations[static_cast<unsigned>(kind)];
    if (data.allocator) [[likely]]
        return;
    static_assert(!JSCell::structureIDOffset() && JSCell::indexingTypeAndMiscOffset() == sizeof(uint32_t));
    data.header = static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
    data.allocator = subspaceFor<CellType>(*instance->vm)->allocatorFor(sizeof(CellType), AllocatorForMode::EnsureAllocator).localAllocator();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
