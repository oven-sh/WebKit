/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
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
#include "JSFFICallback.h"

#if USE(BUN_JSC_ADDITIONS)

#include "Error.h"
#include "ExceptionHelpers.h"
#include "FFICallbackThunk.h"
#include "FFIConversions.h"
#include "JSCInlines.h"
#include "Options.h"
#include <wtf/DataLog.h>
#include <wtf/RawPointer.h>

namespace JSC {

const ClassInfo JSFFICallback::s_info = { "FFICallback"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSFFICallback) };

static JSC_DECLARE_HOST_FUNCTION(ffiCallbackProtoFuncClose);
JSC_DEFINE_HOST_FUNCTION(ffiCallbackProtoFuncClose, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* callback = dynamicDowncast<JSFFICallback>(callFrame->thisValue());
    if (!callback) [[unlikely]]
        return throwVMTypeError(globalObject, scope, "FFICallback.prototype.close called on an incompatible receiver"_s);
    callback->close();
    return JSValue::encode(jsUndefined());
}

JSFFICallback::JSFFICallback(VM& vm, Structure* structure, Ref<FFI::Signature>&& signature)
    : Base(vm, structure)
    , m_signature(WTF::move(signature))
{
}

JSFFICallback::~JSFFICallback()
{
    if (!m_threadsafe)
        return;
    m_threadsafe->cellDestroyed();
    // An entrypoint was handed out and never close()d, so native code was never told to stop calling it
    // (the global object is going away under it): keep the handle, and with it the thunk, alive for the
    // rest of the process.
    if (!m_closed && m_threadsafe->entryCode())
        m_threadsafe->ref();
}

void JSFFICallback::destroy(JSCell* cell)
{
    static_cast<JSFFICallback*>(cell)->JSFFICallback::~JSFFICallback();
}

JSObject* JSFFICallback::createPrototype(VM& vm, JSGlobalObject* globalObject)
{
    JSObject* prototype = JSFinalObject::create(vm, JSFinalObject::createStructure(vm, globalObject, globalObject->objectPrototype(), 1));
    constexpr unsigned attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "close"_s), 0, ffiCallbackProtoFuncClose, ImplementationVisibility::Public, NoIntrinsic, attributes);
    return prototype;
}

Structure* JSFFICallback::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

JSFFICallback* JSFFICallback::create(VM& vm, JSGlobalObject* globalObject, Structure* structure, JSObject* callable, Ref<FFI::Signature>&& signature, bool threadsafe, void* embedderContext)
{
    globalObject->ffiContext();

    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!Options::useJIT()) [[unlikely]] {
        throwTypeError(globalObject, scope, "bun:ffi requires the JIT"_s);
        return nullptr;
    }

#if !FFI_CALLBACK_THUNK_SUPPORTED
    UNUSED_PARAM(structure);
    UNUSED_PARAM(callable);
    UNUSED_PARAM(signature);
    UNUSED_PARAM(threadsafe);
    UNUSED_PARAM(embedderContext);
    throwTypeError(globalObject, scope, "bun:ffi is not supported on this architecture"_s);
    return nullptr;
#else
    ASSERT(callable);
    if (threadsafe && !FFI::FFIContext::threadsafeDispatch()) [[unlikely]] {
        throwTypeError(globalObject, scope, "bun:ffi: no threadsafe dispatch registered (FFIContext::setThreadsafeDispatch)"_s);
        return nullptr;
    }
    JSFFICallback* callback = new (NotNull, allocateCell<JSFFICallback>(vm)) JSFFICallback(vm, structure, WTF::move(signature));
    if (threadsafe)
        callback->m_threadsafe = FFI::ThreadsafeCallbackHandle::create(callback, callback->m_signature.copyRef(), embedderContext);
    callback->finishCreation(vm, callable);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!callback->entryCode()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    globalObject->ffiContext().addLiveCallback(vm, *globalObject, callback);
    return callback;
#endif
}

void JSFFICallback::finishCreation(VM& vm, JSObject* callable)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));

    m_callable.set(vm, this, callable);

#if FFI_CALLBACK_THUNK_SUPPORTED
    auto entryCode = FFI::generateCallbackThunk(vm, *this);
    if (!entryCode) [[unlikely]]
        return; // create() throws the OutOfMemoryError.
    if (m_threadsafe)
        m_threadsafe->setEntryCode(WTF::move(entryCode));
    else
        m_entryCode = WTF::move(entryCode);

    if (Options::verboseFFI()) [[unlikely]]
        dataLogLn("FFI: created JSFFICallback ", RawPointer(this), " signature ", m_signature->toString(), " entrypoint ", RawPointer(nativeEntrypoint()));

    constexpr unsigned attributes = static_cast<unsigned>(PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    putDirect(vm, Identifier::fromString(vm, "ptr"_s), FFI::pointerToJSValue(globalObject(), reinterpret_cast<uint64_t>(nativeEntrypoint())), attributes);
    putDirect(vm, Identifier::fromString(vm, "threadsafe"_s), jsBoolean(isThreadsafe()), attributes);
#endif
}

const MacroAssemblerCodeRef<JITThunkPtrTag>& JSFFICallback::entryCode() const
{
    return m_threadsafe ? m_threadsafe->entryCode() : m_entryCode;
}

void* JSFFICallback::nativeEntrypoint() const
{
    return untagCFunctionPtr<void*, JITThunkPtrTag>(entryCode().code().taggedPtr());
}

const char* JSFFICallback::setReturnCString(const CString& string)
{
    m_returnCString.clear();
    m_returnCString.append(string.span());
    m_returnCString.append('\0');
    return m_returnCString.begin();
}

void JSFFICallback::close()
{
    if (m_closed)
        return;
    m_closed = true;
    VM& vm = this->vm();
    // A threadsafe callback with invocations still queued stays rooted until the last of them has run.
    if (!m_threadsafe || !m_threadsafe->markClosedAndReportPending())
        unroot();
    constexpr unsigned attributes = static_cast<unsigned>(PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    putDirect(vm, Identifier::fromString(vm, "ptr"_s), jsNull(), attributes);
}

void JSFFICallback::unroot()
{
    if (auto* globalObject = this->globalObject())
        globalObject->ffiContext().removeLiveCallback(*globalObject, this);
}

template<typename Visitor>
void JSFFICallback::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSFFICallback* thisObject = uncheckedDowncast<JSFFICallback>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());

    Base::visitChildren(thisObject, visitor);

    visitor.append(thisObject->m_callable);
}

DEFINE_VISIT_CHILDREN(JSFFICallback);

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
