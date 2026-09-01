/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ThreadLocalObject.h"

#include "CustomGetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "ThreadObject.h"

namespace JSC {

const ClassInfo JSThreadLocalObject::s_info = { "ThreadLocal"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSThreadLocalObject) };

static JSC_DECLARE_HOST_FUNCTION(callThreadLocal);
static JSC_DECLARE_HOST_FUNCTION(constructThreadLocal);
static JSC_DECLARE_CUSTOM_GETTER(threadLocalValueGetter);
static JSC_DECLARE_CUSTOM_SETTER(threadLocalValueSetter);

JSThreadLocalObject::JSThreadLocalObject(VM& vm, Structure* structure)
    : Base(vm, structure)
    , m_key(ThreadManager::singleton().allocateThreadLocalKey())
{
}

JSThreadLocalObject* JSThreadLocalObject::create(VM& vm, Structure* structure)
{
    JSThreadLocalObject* object = new (NotNull, allocateCell<JSThreadLocalObject>(vm)) JSThreadLocalObject(vm, structure);
    object->finishCreation(vm);
    return object;
}

void JSThreadLocalObject::destroy(JSCell* cell)
{
    static_cast<JSThreadLocalObject*>(cell)->JSThreadLocalObject::~JSThreadLocalObject();
}

JSC_DEFINE_HOST_FUNCTION(callThreadLocal, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "calling ThreadLocal constructor without new is invalid"_s);
}

JSC_DEFINE_HOST_FUNCTION(constructThreadLocal, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue prototype = callFrame->jsCallee()->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    Structure* structure = JSThreadLocalObject::createStructure(vm, globalObject, prototype.isObject() ? prototype : jsNull());
    return JSValue::encode(JSThreadLocalObject::create(vm, structure));
}

JSC_DEFINE_CUSTOM_GETTER(threadLocalValueGetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSThreadLocalObject* threadLocal = dynamicDowncast<JSThreadLocalObject>(JSValue::decode(thisValue));
    if (!threadLocal)
        return throwVMTypeError(globalObject, scope, "ThreadLocal.prototype.value called on incompatible receiver"_s);
    // SPEC-api 5.8: get touches only the current ThreadState's map,
    // lock-free. A thread with no ThreadState yet cannot have run the setter
    // (the setter goes through ensureCurrentThreadState()), so its slot is
    // the initial undefined (I13) — don't allocate/install a lazy TS just to
    // discover that.
    ThreadState* state = currentThreadStateIfExists();
    if (!state)
        return JSValue::encode(jsUndefined());
    auto it = state->threadLocals.find(threadLocal->key());
    if (it == state->threadLocals.end())
        return JSValue::encode(jsUndefined());
    JSValue value = it->value.get();
    return JSValue::encode(value ? value : jsUndefined());
}

JSC_DEFINE_CUSTOM_SETTER(threadLocalValueSetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, EncodedJSValue encodedValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSThreadLocalObject* threadLocal = dynamicDowncast<JSThreadLocalObject>(JSValue::decode(thisValue));
    if (!threadLocal) {
        throwTypeError(globalObject, scope, "ThreadLocal.prototype.value called on incompatible receiver"_s);
        return false;
    }
    Ref<ThreadState> state = ensureCurrentThreadState();
    // SPEC-api 5.10: the ThreadState finalizer hook must be registered before
    // the FIRST Strong is created in a lazy main/embedder ThreadState ("first
    // lazy-TS Strong" row) — otherwise nothing ever clears threadLocals for a
    // thread that never touches Thread.current, and ~ThreadState's
    // RELEASE_ASSERTs fire at TLS teardown / the Strongs outlive the VM.
    // No-op on spawned threads (jsThread is set at spawn).
    ensureJSThreadForState(globalObject, state.get());
    // SPEC-api 5.8/5.10: the Strong is created here, under the JSLock (we are
    // executing JS), in the owner thread's own map only. HashMap::set
    // destroys any previously stored Strong — that is the normative
    // "overwrite" clear point (also under the JSLock). Any JS value is
    // storable, including undefined: an explicit undefined store keeps its
    // slot (indistinguishable from the initial state through this accessor).
    state->threadLocals.set(threadLocal->key(), Strong<Unknown>(vm, JSValue::decode(encodedValue)));
    return true;
}

JSValue createThreadLocalProperty(VM& vm, JSObject* globalObjectArg)
{
    JSGlobalObject* globalObject = uncheckedDowncast<JSGlobalObject>(globalObjectArg);
    JSObject* prototype = constructEmptyObject(globalObject);
    prototype->putDirectCustomAccessor(vm, Identifier::fromString(vm, "value"_s), CustomGetterSetter::create(vm, threadLocalValueGetter, threadLocalValueSetter), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::CustomAccessor));

    JSFunction* constructor = JSFunction::create(vm, globalObject, 0, "ThreadLocal"_s, callThreadLocal, ImplementationVisibility::Public, NoIntrinsic, constructThreadLocal);
    constructor->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirect(vm, vm.propertyNames->toStringTagSymbol, jsNontrivialString(vm, "ThreadLocal"_s), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly));
    return constructor;
}

} // namespace JSC
