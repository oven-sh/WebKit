/*
 * Copyright (C) 2026 Anthropic PBC.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JSTracedFunction.h"

#if USE(BUN_JSC_ADDITIONS)

#include "ExecutableBaseInlines.h"
#include "JSCInlines.h"
#include "JSTracedFunctionInlines.h"

namespace JSC {

const ClassInfo JSTracedFunction::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSTracedFunction) };

JSTracedFunction::JSTracedFunction(VM& vm, NativeExecutable* executable, JSGlobalObject* globalObject, Structure* structure, Shape shape, JSObject* target, JSValue data, double length)
    : Base(vm, executable, globalObject, structure)
    , m_targetFunction(target, WriteBarrierEarlyInit)
    , m_data(data, WriteBarrierEarlyInit)
    , m_length(length)
    , m_shape(shape)
{
}

JSTracedFunction* JSTracedFunction::create(VM& vm, JSGlobalObject* globalObject, Shape shape, JSObject* target, JSValue data, const String& name, unsigned length)
{
    ASSERT(shape == Shape::CallLast || (target && target->isCallable()));
    bool fast = shape == Shape::CallLast || (target && target->type() == JSFunctionType);
    NativeExecutable* executable = vm.getTracedFunction(fast);
    Structure* structure = globalObject->tracedFunctionStructure();
    JSTracedFunction* function = new (NotNull, allocateCell<JSTracedFunction>(vm)) JSTracedFunction(vm, executable, globalObject, structure, shape, target, data, length);
    function->finishCreation(vm, name);
    return function;
}

void JSTracedFunction::finishCreation(VM& vm, const String& name)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
    if (!name.isNull())
        m_name.set(vm, this, jsString(vm, name));
}

template<typename Visitor>
void JSTracedFunction::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSTracedFunction* thisObject = uncheckedDowncast<JSTracedFunction>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_targetFunction);
    visitor.append(thisObject->m_data);
    visitor.append(thisObject->m_name);
}

DEFINE_VISIT_CHILDREN(JSTracedFunction);

// The C++ path (no JIT, exotic callees). tracedFunctionCallGenerator is the
// fast equivalent; keep the two in step.
JSC_DEFINE_HOST_FUNCTION(tracedFunctionCallGeneric, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* traced = uncheckedDowncast<JSTracedFunction>(callFrame->jsCallee());
    auto& hooks = vm.tracedFunctionHooks();

    JSValue target = jsUndefined();
    if (traced->shape() == JSTracedFunction::Shape::Wrap)
        target = traced->targetFunction();
    else if (callFrame->argumentCount())
        target = callFrame->uncheckedArgument(callFrame->argumentCount() - 1);

    // The enter hook's value is opaque (any JSValue, falsy ones included); the
    // empty JSValue — also what a missing hook leaves — means "not traced".
    JSValue span;
    if (hooks.enter) {
        span = JSValue::decode(hooks.enter(globalObject, callFrame, traced));
        RETURN_IF_EXCEPTION(scope, { });
    }
    bool isTraced = !!span; // JSValue::operator bool is !isEmpty(), not JS truthiness
    if (traced->shape() == JSTracedFunction::Shape::CallLast && !target.isCallable()) {
        // `span(name, attributes?)` with no callback: the hook's value is the result.
        return JSValue::encode(isTraced ? span : jsUndefined());
    }

    auto callData = JSC::getCallData(target);
    if (callData.type == CallData::Type::None) [[unlikely]]
        return throwVMTypeError(globalObject, scope, "Bun.otel.wrap target is not callable"_s);
    auto cacheEntrypoint = [&] {
        // Cache the arity-check entry point so later calls take the thunk
        // (tracedFunctionCallGenerator bails here while it is null; installing
        // new code clears it again).
        if (callData.type == CallData::Type::JS && callData.js.functionExecutable->hasJITCodeForCall())
            callData.js.functionExecutable->entrypointFor(CodeSpecializationKind::CodeForCall, ArityCheckMode::MustCheckArity);
    };
    cacheEntrypoint();
    JSValue result;
    {
        MarkedArgumentBuffer args;
        JSValue thisValue;
        if (traced->shape() == JSTracedFunction::Shape::Wrap) {
            thisValue = callFrame->thisValue();
            for (unsigned i = 0; i < callFrame->argumentCount(); ++i)
                args.append(callFrame->uncheckedArgument(i));
            if (args.hasOverflowed()) [[unlikely]] {
                throwOutOfMemoryError(globalObject, scope);
                return { };
            }
        } else {
            thisValue = jsUndefined();
            args.append(isTraced ? span : jsUndefined());
        }
        result = call(globalObject, target, callData, thisValue, args);
    }
    cacheEntrypoint();
    if (Exception* exception = scope.exception()) [[unlikely]] {
        // The thunk's equivalent is UnwindFunctor seeing the frame.
        if (isTraced && hooks.unwind)
            hooks.unwind(globalObject, traced, span, exception);
        return { };
    }
    if (isTraced && hooks.leave)
        RELEASE_AND_RETURN(scope, hooks.leave(globalObject, traced, JSValue::encode(span), JSValue::encode(result)));
    return JSValue::encode(result);
}

// Same body; a distinct function so JITThunks::hostFunctionStub (keyed on the
// function pointer) gives the thunk-backed and the generic NativeExecutable
// separate entries, as remoteFunctionCallForJSFunction does.
JSC_DEFINE_HOST_FUNCTION(tracedFunctionCallForJSFunction, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return tracedFunctionCallGeneric(globalObject, callFrame);
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
