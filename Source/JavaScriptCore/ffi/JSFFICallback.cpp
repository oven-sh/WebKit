/*
 * Copyright (C) 2026 Oven-sh Inc. All rights reserved.
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
#include "JSCInlines.h"
#include "Options.h"
#include <wtf/DataLog.h>
#include <wtf/RawPointer.h>

namespace JSC {

// FFI-SPEC-GAP: the spec does not name the JS-visible class name; Bun's
// existing wrapper class is "JSCallback", but that name belongs to Bun's JS
// glue, so the engine cell reports itself as "FFICallback".
const ClassInfo JSFFICallback::s_info = { "FFICallback"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSFFICallback) };

// FFI-SPEC-GAP: SPEC section 9.1 lists close() as a C++ member only and
// names just the `ptr` / `threadsafe` own properties as the JS surface, but
// row T's stress suite (JSTests/stress/ffi-callbacks.js) closes callbacks
// from JS via `cb.close()` -- Bun-parity with JSCallback.prototype.close.
// The engine therefore installs a `close` own function next to `ptr` in
// finishCreation so every creator (BunFFI.cpp, $vm.ffiCallback, testFFI)
// gets it; it forwards to JSFFICallback::close() and returns undefined.
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

JSFFICallback::~JSFFICallback() = default;

void JSFFICallback::destroy(JSCell* cell)
{
    static_cast<JSFFICallback*>(cell)->JSFFICallback::~JSFFICallback();
}

Structure* JSFFICallback::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

JSFFICallback* JSFFICallback::create(VM& vm, JSGlobalObject* globalObject, Structure* structure, JSObject* callable, Ref<FFI::Signature>&& signature)
{
    // Materialize the per-global FFIContext eagerly on the mutator (parity with
    // JSFFIFunction::create): ffiCallbackDispatch reads globalObject->ffiContext() from a
    // callback that native code may fire before any JSFFIFunction was ever created, and the
    // lazy first-creation must never run there (nor race a mutator ffiContext()).
    globalObject->ffiContext();

    auto scope = DECLARE_THROW_SCOPE(vm);

    // bun:ffi requires the JIT (SPEC section 0.1): the callback entry thunk is
    // generated machine code. VM initialization folds "the executable
    // allocator is disabled" into Options::useJIT() == false.
    // FFI-SPEC-GAP: the spec assigns the "bun:ffi requires the JIT" check to
    // creation without naming a site; JSFFICallback::create performs it so every
    // creator (BunFFI.cpp, $vm.ffiCallback, testFFI) throws identically.
    if (!Options::useJIT()) [[unlikely]] {
        throwTypeError(globalObject, scope, "bun:ffi requires the JIT"_s);
        return nullptr;
    }

#if !FFI_CALLBACK_THUNK_SUPPORTED
    // 32-bit / JIT-less / unsupported-CPU builds compile the entry thunk out
    // entirely (SPEC section 14).
    UNUSED_PARAM(structure);
    UNUSED_PARAM(callable);
    UNUSED_PARAM(signature);
    throwTypeError(globalObject, scope, "bun:ffi is not supported on this architecture"_s);
    return nullptr;
#else
    ASSERT(callable);
    JSFFICallback* callback = new (NotNull, allocateCell<JSFFICallback>(vm)) JSFFICallback(vm, structure, WTF::move(signature));
    callback->finishCreation(vm, callable);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!callback->m_entryCode) [[unlikely]] {
        // Executable memory exhaustion; the cell is otherwise valid and will
        // be swept normally.
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    return callback;
#endif
}

void JSFFICallback::finishCreation(VM& vm, JSObject* callable)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));

    m_callable.set(vm, this, callable);

#if FFI_CALLBACK_THUNK_SUPPORTED
    m_entryCode = FFI::generateCallbackThunk(vm, *this);
    if (!m_entryCode) [[unlikely]]
        return; // create() throws the OutOfMemoryError.

    if (Options::verboseFFI()) [[unlikely]]
        dataLogLn("FFI: created JSFFICallback ", RawPointer(this), " signature ", m_signature->toString(), " entrypoint ", RawPointer(nativeEntrypoint()));

    // The JS surface lives here (SPEC section 9.1) so that BunFFI.cpp,
    // $vm.ffiCallback and testFFI all produce identical objects: read-only,
    // don't-enum, don't-delete own properties "ptr" (the code pointer as a
    // double-encoded pointer, Bun parity) and "threadsafe" (always false in v1).
    constexpr unsigned attributes = static_cast<unsigned>(PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    putDirect(vm, Identifier::fromString(vm, "ptr"_s), jsNumber(static_cast<double>(reinterpret_cast<uintptr_t>(nativeEntrypoint()))), attributes);
    putDirect(vm, Identifier::fromString(vm, "threadsafe"_s), jsBoolean(false), attributes);
    // The `close` JS method (see the FFI-SPEC-GAP above ffiCallbackProtoFuncClose).
    putDirectNativeFunction(vm, globalObject(), Identifier::fromString(vm, "close"_s), 0, ffiCallbackProtoFuncClose, ImplementationVisibility::Public, NoIntrinsic, attributes);
#endif
}

void* JSFFICallback::nativeEntrypoint() const
{
    if (!m_entryCode)
        return nullptr;
    // The thunk is entered by foreign C code with a plain call/blr, so hand
    // out the C-function-pointer-signed address (identity on non-ARM64E).
    return untagCFunctionPtr<void*, JITThunkPtrTag>(m_entryCode.code().taggedPtr());
}

void JSFFICallback::close()
{
    if (m_closed)
        return;
    m_closed = true;
    // Nothing is dropped native-side: the entry code lives as long as the
    // cell and nativeEntrypoint() keeps returning it. Only the JS-visible "ptr"
    // property becomes null so Bun's glue stops handing the pointer out.
    VM& vm = this->vm();
    constexpr unsigned attributes = static_cast<unsigned>(PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    putDirect(vm, Identifier::fromString(vm, "ptr"_s), jsNull(), attributes);
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
