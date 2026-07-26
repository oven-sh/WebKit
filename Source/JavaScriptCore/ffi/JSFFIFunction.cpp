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
#include "JSFFIFunction.h"

#if USE(BUN_JSC_ADDITIONS)

#include "Error.h"
#include "FFICallHost.h"
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

// FFI-SPEC-GAP: the class name is "Function" (as JSStrictFunction / JSSloppyFunction) so an FFI
// function is indistinguishable from an ordinary function to JS, matching Bun's current bun:ffi
// functions. Its ClassInfo is kept address-unique like every other "Function"-named ClassInfo.
const ClassInfo JSFFIFunction::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSFFIFunction) };
CLASSINFO_KEEP_ADDRESS_UNIQUE(JSFFIFunction);

JSFFIFunction::JSFFIFunction(VM& vm, NativeExecutable* executable, JSGlobalObject* globalObject, Structure* structure, Ref<FFI::Signature>&& signature, void* target, RefPtr<JITCode>&& icCode, const FFI::CallHooks* hooks)
    : Base(vm, executable, globalObject, structure)
    , m_signature(WTF::move(signature))
    , m_target(target)
    , m_icCode(WTF::move(icCode))
    , m_hooks(hooks)
{
    // A hooked function never has an IC stub: hooks run only in the C++ host path.
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

Structure* JSFFIFunction::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    ASSERT(globalObject);
    return Structure::create(vm, globalObject, prototype, TypeInfo(JSFunctionType, StructureFlags), info());
}

JSFFIFunction* JSFFIFunction::create(VM& vm, JSGlobalObject* globalObject, Structure* structure, Ref<FFI::Signature>&& signatureRef, void* target, const String& name, JSObject* owner, const FFI::CallHooks* hooks)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    // bun:ffi requires the JIT (spec section 0.1): the invoke thunk and callback thunks are
    // JIT-generated, and TinyCC (which this replaces) was itself a JIT. Options::useJIT() is
    // false whenever the ExecutableAllocator could not be initialized, so this single check
    // covers both conditions.
    if (!Options::useJIT()) [[unlikely]] {
        throwTypeError(globalObject, scope, "bun:ffi requires the JIT"_s);
        return nullptr;
    }

#if !USE(JSVALUE64) || ENABLE(JIT_CAGE) || !(CPU(X86_64) || CPU(ARM64))
    // FFI-SPEC-GAP: on 32-bit, JIT-operation-validation and non-x86-64/arm64 builds the FFI
    // machinery is compiled out (spec section 14); rather than crashing, creation throws so the
    // JS-facing entry points behave uniformly.
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

    // Materialize the per-global FFIContext eagerly on the mutator thread: DFG/FTL CallFFI
    // codegen bakes &globalObject->ffiContext() as a compile-time constant from a compiler
    // thread (spec sections 10.4/10.5), which must never observe the lazy first-creation.
    globalObject->ffiContext();

    // A JSFunction's executable is immutable after construction, so the (possibly
    // diversified) executable is built before the cell (spec section 8.1).
    NativeExecutable* base = vm.getHostFunction(FFI::ffiHostCall, ImplementationVisibility::Public, NoIntrinsic, callHostFunctionAsConstructor, nullptr, length, name);

    RefPtr<JITCode> stub;
#if ENABLE(JIT)
    // The stub bakes only the target and the signature, never the cell pointer (the callee cell
    // is read from CallFrameSlot::callee), so no cell is needed to generate it. Generation
    // failure or Options::useFFIICStub() == false yields the plain host function executable.
    // A HOOKED function is permanently host-path-only (its before/after hooks are invoked from
    // exactly one place, FFI::ffiHostCall), so it never gets a stub; the DFG likewise refuses to
    // turn calls to it into CallFFI (isHostPathOnly()).
    if (Options::useFFIICStub() && !hooks)
        stub = FFI::generateICStubCode(vm, globalObject, signature.get(), target);
#endif

    NativeExecutable* executable = base;
    if (stub) {
        // Install the stub AS the executable's call code (generatedJITCodeForCall()), so every
        // JS-initiated call site (LLInt call slow path, baseline/DFG/FTL call ICs and direct
        // calls) enters it through the normal executable path with no Repatch.cpp special case.
        // C++-initiated calls (Interpreter::executeCall's CallData::Type::Native branch: JSC::call,
        // Function.prototype.call/apply reached from C++, Reflect.apply, promise reactions) invoke
        // the executable's native function, FFI::ffiHostCall, directly and never enter the stub;
        // behavior is identical because the stub's slow path shares ffiHostCall's body.
        executable = NativeExecutable::create(vm, Ref<JITCode>(*stub), FFI::ffiHostCall, base->generatedJITCodeForConstruct(), callHostFunctionAsConstructor, ImplementationVisibility::Public, length, name);
    }

    JSFFIFunction* function = new (NotNull, allocateCell<JSFFIFunction>(vm)) JSFFIFunction(vm, executable, globalObject, structure, WTF::move(signature), target, WTF::move(stub), hooks);
    function->finishCreation(vm);
    if (owner)
        function->m_owner.set(vm, function, owner); // write-barriered: the owner outlives this function

    dataLogLnIf(Options::verboseFFI(), "FFI: created JSFFIFunction '", name, "' ", function->signature().toString(), " target=", RawPointer(target), " icStub=", !!function->icCode());

    RELEASE_AND_RETURN(scope, function);
#endif
}


// "ptr": intrinsic, read-only, non-enumerable -- served straight from m_target so no instance
// ever carries an own property (no Structure transition; see the class comment on StructureFlags).
bool JSFFIFunction::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    JSFFIFunction* thisObject = uncheckedDowncast<JSFFIFunction>(object);
    VM& vm = thisObject->vm();
    // "native" (Bun API parity: symbol.native is the raw callable -- for the engine-native
    // function that is the function itself). Intrinsic for the same reason as "ptr": adding it
    // as an own property would transition this cell's Structure and slow every polymorphic call.
    if (propertyName == Identifier::fromString(vm, "native"_s)) [[unlikely]] {
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete), thisObject);
        return true;
    }
    if (propertyName == Identifier::fromString(vm, "ptr"_s)) [[unlikely]] {
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete),
            FFI::pointerToJSValue(globalObject, reinterpret_cast<uint64_t>(thisObject->target())));
        return true;
    }
    return Base::getOwnPropertySlot(object, globalObject, propertyName, slot);
}

static ALWAYS_INLINE bool isIntrinsicFFIProperty(VM& vm, PropertyName propertyName)
{
    return propertyName == Identifier::fromString(vm, "ptr"_s) || propertyName == Identifier::fromString(vm, "native"_s);
}

// ReadOnly: a write is a TypeError in strict mode and a silent no-op otherwise -- and must never
// materialize an own property (that would transition every FFI function's Structure).
bool JSFFIFunction::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    if (isIntrinsicFFIProperty(cell->vm(), propertyName)) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(cell->vm());
        return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
    }
    return Base::put(cell, globalObject, propertyName, value, slot);
}

// DontDelete: strict-mode delete throws (via the false return), sloppy delete returns false.
bool JSFFIFunction::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, DeletePropertySlot& slot)
{
    if (isIntrinsicFFIProperty(cell->vm(), propertyName)) [[unlikely]]
        return false;
    return Base::deleteProperty(cell, globalObject, propertyName, slot);
}

void JSFFIFunction::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    // Both intrinsic properties are DontEnum (they must not perturb for-in / spread / JSON), so
    // they only appear when non-enumerable properties are requested (getOwnPropertyNames,
    // Reflect.ownKeys), matching what getOwnPropertySlot reports for them.
    VM& vm = object->vm();
    if (mode == DontEnumPropertiesMode::Include) {
        propertyNames.add(Identifier::fromString(vm, "ptr"_s));
        propertyNames.add(Identifier::fromString(vm, "native"_s));
    }
    Base::getOwnPropertyNames(object, globalObject, propertyNames, mode);
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
