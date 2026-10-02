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

// An operation is called by a stub, from the stub's frame. That frame is what the operation sees as its caller, and what
// `callFrame` refers to in all of them. The stub's return address identifies the function that called it, and the position in that
// function.
// (This returns the function that the machine code belongs to. The code at that position may have been inlined from another
// function: see FunctionRef::locationForReturnAddress().)
ALWAYS_INLINE FunctionRef caller(Instance* instance, CallFrame* callFrame) { return FunctionRef::at(instance, removeCodePtrTag(callFrame->rawReturnPC())); }
// The function whose bytecode the caller is executing: the caller itself, unless the code was inlined from another function.
// Indices that come from the bytecode have to be resolved against this function. Slots and similar data belong to the caller.
ALWAYS_INLINE FunctionRef bytecodeOwnerOfCaller(Instance* instance, CallFrame* callFrame)
{
    FunctionRef function = caller(instance, callFrame);
    if (!function.info().function()->hasInlineFrames) [[likely]]
        return function;
    return function.locationForReturnAddress(removeCodePtrTag(callFrame->rawReturnPC())).function;
}
ALWAYS_INLINE BytecodeIndex bytecodeIndexOfCaller(Instance* instance, CallFrame* callFrame) { return caller(instance, callFrame).bytecodeIndexAt(removeCodePtrTag(callFrame->rawReturnPC())); }

// When a function without its own Data calls an operation, the reason may well be that it has no inline caches. See
// Instance::misses.
// (Only one call in eight is examined, and it counts for eight, because identifying the caller takes longer than some operations
// do.)
ALWAYS_INLINE void countOperationFor(Instance* instance, CallFrame* callFrame)
{
    constexpr uint32_t oneIn = 8;
    if (++instance->uncountedOperations % oneIn) [[likely]]
        return;
    FunctionRef function = caller(instance, callFrame);
    if (!function.instance->dataIfExists(function.index)) [[unlikely]]
        function.instance->countMisses(function.index, oneIn);
}

#define AOT_OPERATION_BEGIN(instance) \
    JSGlobalObject* globalObject = (instance)->globalObject; \
    UNUSED_VARIABLE(globalObject); \
    VM& vm = *(instance)->vm; \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    countOperationFor(instance, callFrame); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

// For a stub that a function may have tail-called, in which case there is no calling function.
#define AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance) \
    JSGlobalObject* globalObject = (instance)->globalObject; \
    UNUSED_VARIABLE(globalObject); \
    VM& vm = *(instance)->vm; \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

// What an operation needs to know about its calling function, other than its arguments. All of it is data that a function is linked
// with, none of it is bytecode, metadata or a profile, and this is the only place that knows where it is.
// (The result may be the shared placeholder, SharedData. Neither it nor its slots are ever written to.)
ALWAYS_INLINE Data* callerData(Instance* instance, CallFrame* callFrame)
{
    FunctionRef function = caller(instance, callFrame);
    Data* data = function.instance->dataIfExists(function.index);
    return data ? data : function.instance->sharedData;
}
ALWAYS_INLINE UnlinkedCodeBlock* callerCode(Instance* instance, CallFrame* callFrame) { return bytecodeOwnerOfCaller(instance, callFrame).ensureUnlinkedCodeBlock(); }

// A slot does not keep what it refers to alive: Data::finalizeUnconditionally() clears it when that dies. An eden collection only
// visits the Datas that have been marked as filled since the last collection. The slot has to be cleared because the StructureID of
// a dead structure is eventually reused.
ALWAYS_INLINE void didFillSlot(VM&, Data* data)
{
    if (data == SharedData::get())
        return;
    data->slotEpoch++;
    if (!data->hasBeenFilledSinceLastCollection)
        data->noteFilled();
}
ALWAYS_INLINE const Identifier& identifierAt(Instance* instance, CallFrame* callFrame, unsigned index) { return static_cast<const Identifier*>(caller(instance, callFrame).info().identifiers)[index]; }
// The same, for an operation that is called often enough to be passed the answer. whose: the index of one of the caller's known
// callees, plus one, or zero for the caller itself (Lowering::whoseBytecode()). Looking it up would mean searching the caller's
// call sites.
ALWAYS_INLINE FunctionRef bytecodeOwnerOfCaller(Instance* instance, CallFrame* callFrame, uint32_t whose)
{
    FunctionRef function = caller(instance, callFrame);
    if (!whose) [[likely]]
        return function;
    return { function.instance, function.info().function()->knownCallees()[whose - 1] };
}
// (Eval code is never inlined, and nothing is inlined into it.)
ALWAYS_INLINE PutPropertySlot::Context putByIdContextOf(Instance* instance, CallFrame* callFrame) { return caller(instance, callFrame).codeType() == EvalCode ? PutPropertySlot::PutByIdEval : PutPropertySlot::PutById; }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
