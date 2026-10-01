/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTRuntime.h"
#include "CodeBlock.h"
#include "FrameTracers.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

// An operation is called by a stub, in a frame of the stub's: which is what it takes itself to have been called from, and what `callFrame`
// is in all of them. Where the stub is to go back to says which function called it, and where that has got to.
// (The function whose code that is. What it is in the middle of may be what another does: FunctionRef::placeAt().)
ALWAYS_INLINE FunctionRef caller(JSGlobalObject* globalObject, CallFrame* callFrame) { return FunctionRef::at(globalObject->aotInstance(), removeCodePtrTag(callFrame->rawReturnPC())); }
// The function whose bytecode it is that the caller is in the middle of: itself, unless that is one that was made part of it. Whatever goes
// by a number that the bytecode has is a matter for this one; slots and the like are the caller's.
ALWAYS_INLINE FunctionRef functionOfBytecodeOfCaller(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    FunctionRef function = caller(globalObject, callFrame);
    if (!function.info().function()->hasInlineFrames) [[likely]]
        return function;
    return function.placeAt(removeCodePtrTag(callFrame->rawReturnPC())).function;
}
ALWAYS_INLINE BytecodeIndex bytecodeIndexOfCaller(JSGlobalObject* globalObject, CallFrame* callFrame) { return caller(globalObject, callFrame).bytecodeIndexAt(removeCodePtrTag(callFrame->rawReturnPC())); }

// Whatever a function that has no Data of its own comes to an operation for, it may well be for want of one. See Instance::misses.
// (One in so many is looked at, and counts for as many: finding out whose it is takes longer than some operations do.)
ALWAYS_INLINE void countOperationFor(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    constexpr uint32_t oneIn = 8;
    if (++globalObject->aotInstance()->operationsNotCounted % oneIn) [[likely]]
        return;
    FunctionRef function = caller(globalObject, callFrame);
    if (!function.instance->dataIfItHasAny(function.index)) [[unlikely]]
        function.instance->countMisses(function.index, oneIn);
}

#define AOT_OPERATION_BEGIN(globalObject) \
    VM& vm = (globalObject)->vm(); \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    countOperationFor(globalObject, callFrame); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

// For a stub that a function may have jumped to on its way out: then there is nobody that it is done on behalf of.
#define AOT_OPERATION_BEGIN_FOR_NOBODY(globalObject) \
    VM& vm = (globalObject)->vm(); \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

// What an operation wants to know about the function that called it, other than what it was passed. All of it is what a
// function is linked with, none of it is bytecode, metadata or a profile, and this is the only place that says where it is.
// (Which may be the one that is nobody's: SharedData. That goes for its slots too, and neither is written to.)
ALWAYS_INLINE Data* callerData(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    FunctionRef function = caller(globalObject, callFrame);
    Data* data = function.instance->dataIfItHasAny(function.index);
    return data ? data : function.instance->sharedData;
}
ALWAYS_INLINE UnlinkedCodeBlock* callerCode(JSGlobalObject* globalObject, CallFrame* callFrame) { return functionOfBytecodeOfCaller(globalObject, callFrame).ensureUnlinkedCodeBlock(); }

// What a slot refers to it does not keep alive: Data::finalizeUnconditionally() empties it when that dies. A collection of the young
// only looks at the ones that have said that they have something new. An identifier of a structure that has died is sooner or later
// that of another.
ALWAYS_INLINE void didFillSlot(VM&, Data* data)
{
    if (data == SharedData::get())
        return;
    data->slotEpoch++;
    if (!data->hasBeenFilledSinceLastCollection)
        data->noteFilled();
}
ALWAYS_INLINE const Identifier& identifierAt(JSGlobalObject* globalObject, CallFrame* callFrame, unsigned index) { return static_cast<const Identifier*>(caller(globalObject, callFrame).info().identifiers)[index]; }
// The same, for an operation that is called often enough to be told: which of the caller's known callees it is, plus one, or none for the
// caller itself (Lowering::whoseBytecode()). Looking it up means going through the places the caller calls from.
ALWAYS_INLINE FunctionRef functionOfBytecodeOfCaller(JSGlobalObject* globalObject, CallFrame* callFrame, uint32_t whose)
{
    FunctionRef function = caller(globalObject, callFrame);
    if (!whose) [[likely]]
        return function;
    return { function.instance, function.info().function()->knownCallees()[whose - 1] };
}
// (Code that is evaluated is not made part of anything, and nothing is made part of it.)
ALWAYS_INLINE PutPropertySlot::Context putByIdContextOf(JSGlobalObject* globalObject, CallFrame* callFrame) { return caller(globalObject, callFrame).codeType() == EvalCode ? PutPropertySlot::PutByIdEval : PutPropertySlot::PutById; }

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
