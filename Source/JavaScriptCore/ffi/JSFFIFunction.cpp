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
#include "JSFFIFunction.h"

#if USE(BUN_JSC_ADDITIONS)

#include "Error.h"
#include "FFICallHost.h"
#include "FFIConversions.h"
#include "FFISignature.h"
#include "JITCode.h"
#include "JSCInlines.h"
#include "JSObjectInlines.h"
#include "NativeExecutable.h"
#include "SlotVisitorInlines.h"
#include "StructureInlines.h"
#include <wtf/DataLog.h>
#include <wtf/RawPointer.h>

#if ENABLE(JIT)
#include "FFIICStub.h"
#endif

namespace JSC {

const ClassInfo JSFFIFunction::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSFFIFunction) };
CLASSINFO_KEEP_ADDRESS_UNIQUE(JSFFIFunction);

JSFFIFunction::JSFFIFunction(VM& vm, NativeExecutable* executable, JSGlobalObject* globalObject, Structure* structure, Ref<FFI::Signature>&& signature, void* target, RefPtr<JITCode>&& icCode, const FFI::CallHooks* hooks)
    : Base(vm, executable, globalObject, structure)
    , m_signature(WTF::move(signature))
    , m_target(target)
    , m_icCode(WTF::move(icCode))
    , m_hooks(hooks)
{
    ASSERT(!m_hooks || !m_icCode);
}

JSFFIFunction::~JSFFIFunction() = default;

template<typename Visitor>
void JSFFIFunction::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<JSFFIFunction>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_owner); // keeps the owner (e.g. library handle) alive
}

DEFINE_VISIT_CHILDREN(JSFFIFunction);

void JSFFIFunction::destroy(JSCell* cell)
{
    static_cast<JSFFIFunction*>(cell)->JSFFIFunction::~JSFFIFunction();
}

static constexpr unsigned ffiIntrinsicAttributes = static_cast<unsigned>(PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
static constexpr PropertyOffset ptrOffset = firstOutOfLineOffset;
static constexpr PropertyOffset nativeOffset = firstOutOfLineOffset + 1;

Structure* JSFFIFunction::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    ASSERT(globalObject);
    Structure* structure = Structure::create(vm, globalObject, prototype, TypeInfo(JSFunctionType, StructureFlags), info());
    PropertyOffset offset;
    structure = Structure::addPropertyTransition(vm, structure, Identifier::fromString(vm, "ptr"_s), ffiIntrinsicAttributes, offset);
    ASSERT_UNUSED(offset, offset == ptrOffset);
    structure = Structure::addPropertyTransition(vm, structure, Identifier::fromString(vm, "native"_s), ffiIntrinsicAttributes, offset);
    ASSERT(offset == nativeOffset);
    return structure;
}

JSFFIFunction* JSFFIFunction::create(VM& vm, JSGlobalObject* globalObject, Structure* structure, Ref<FFI::Signature>&& signatureRef, void* target, const String& name, JSObject* owner, const FFI::CallHooks* hooks)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!Options::useJIT()) [[unlikely]] {
        throwTypeError(globalObject, scope, "bun:ffi requires the JIT"_s);
        return nullptr;
    }

#if !USE(JSVALUE64) || ENABLE(JIT_CAGE) || !(CPU(X86_64) || CPU(ARM64))
    UNUSED_PARAM(structure);
    UNUSED_PARAM(signatureRef);
    UNUSED_PARAM(target);
    UNUSED_PARAM(name);
    UNUSED_PARAM(owner);
    UNUSED_PARAM(hooks);
    throwTypeError(globalObject, scope, "bun:ffi is not supported on this architecture"_s);
    return nullptr;
#else
    Ref<FFI::Signature> signature = WTF::move(signatureRef);
    unsigned length = signature->argumentCount();

    globalObject->ffiContext();

    NativeExecutable* base = vm.getHostFunction(FFI::ffiHostCall, ImplementationVisibility::Public, NoIntrinsic, callHostFunctionAsConstructor, nullptr, length, name);

    RefPtr<JITCode> stub;
#if ENABLE(JIT)
    if (Options::useFFIICStub() && !hooks)
        stub = FFI::generateICStubCode(vm, globalObject, signature.get(), target);
#endif

    NativeExecutable* executable = base;
    if (stub) {
        executable = NativeExecutable::create(vm, Ref<JITCode>(*stub), FFI::ffiHostCall, base->generatedJITCodeForConstruct(), callHostFunctionAsConstructor, ImplementationVisibility::Public, length, name);
    }

    JSFFIFunction* function = new (NotNull, allocateCell<JSFFIFunction>(vm)) JSFFIFunction(vm, executable, globalObject, structure, WTF::move(signature), target, WTF::move(stub), hooks);
    function->finishCreation(vm);
    if (owner)
        function->m_owner.set(vm, function, owner); // write-barriered: the owner outlives this function

    function->setButterfly(vm, Butterfly::create(vm, function, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0));
    function->putDirectOffset(vm, ptrOffset, FFI::pointerToJSValue(globalObject, reinterpret_cast<uint64_t>(target)));
    function->putDirectOffset(vm, nativeOffset, function);

    dataLogLnIf(Options::verboseFFI(), "FFI: created JSFFIFunction '", name, "' ", function->signature().toString(), " target=", RawPointer(target), " icStub=", !!function->icCode());

    RELEASE_AND_RETURN(scope, function);
#endif
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
