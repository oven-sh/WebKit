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

#define AOT_OPERATION_BEGIN(globalObject) \
    VM& vm = (globalObject)->vm(); \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

// What an operation wants to know about the function that called it, other than what it was passed. All of it is what a
// function is linked with, none of it is bytecode, metadata or a profile, and this is the only place that says where it is.
// For finding out what the fast paths miss (Options::aotReportSlowPaths): says every so often what got here most.
void noteSlowPathSlow(ASCIILiteral operation, JSValue base, UniquedStringImpl* name, ASCIILiteral detail);
ALWAYS_INLINE void noteSlowPath(ASCIILiteral operation, JSValue base = { }, UniquedStringImpl* name = nullptr, ASCIILiteral detail = ""_s)
{
    if (Options::aotReportSlowPaths()) [[unlikely]]
        noteSlowPathSlow(operation, base, name, detail);
}

ALWAYS_INLINE Data* callerData(CallFrame* callFrame) { return dataOf(callFrame); }
ALWAYS_INLINE UnlinkedCodeBlock* callerCode(CallFrame* callFrame) { return dataOf(callFrame)->unlinkedCodeBlock; }

// What a slot refers to it does not keep alive: Data::finalizeUnconditionally() empties it when that dies. A collection of the young
// only looks at the ones that have said that they have something new. An identifier of a structure that has died is sooner or later
// that of another.
ALWAYS_INLINE void didFillSlot(VM&, Data* data)
{
    data->slotEpoch++;
    if (!data->hasBeenFilledSinceLastCollection)
        data->noteFilled();
}
ALWAYS_INLINE const Identifier& identifierAt(CallFrame* callFrame, unsigned index) { return callerCode(callFrame)->identifier(index); }
ALWAYS_INLINE FunctionExecutable* functionDeclAt(CallFrame* callFrame, unsigned index) { return callerData(callFrame)->codeBlock->functionDecl(index); }
ALWAYS_INLINE FunctionExecutable* functionExprAt(CallFrame* callFrame, unsigned index) { return callerData(callFrame)->codeBlock->functionExpr(index); }
ALWAYS_INLINE PutPropertySlot::Context putByIdContextOf(CallFrame* callFrame) { return callerCode(callFrame)->codeType() == EvalCode ? PutPropertySlot::PutByIdEval : PutPropertySlot::PutById; }

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
