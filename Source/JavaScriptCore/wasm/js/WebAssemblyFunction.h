/*
 * Copyright (C) 2016-2024 Apple Inc. All rights reserved.
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

#pragma once

#include <wtf/Platform.h>

#if ENABLE(WEBASSEMBLY)

#include "ArityCheckMode.h"
#include "MacroAssemblerCodeRef.h"
#include "Options.h"
#include "WasmCallee.h"
#include "WebAssemblyFunctionBase.h"
#include <wtf/Noncopyable.h>

namespace JSC {

class JSGlobalObject;
struct ProtoCallFrame;
class WebAssemblyInstance;

class WebAssemblyFunction final : public WebAssemblyFunctionBase {
    friend JSC::LLIntOffsetsExtractor;

public:
    using Base = WebAssemblyFunctionBase;

    static constexpr unsigned StructureFlags = Base::StructureFlags;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.webAssemblyFunctionSpace<mode>();
    }

    DECLARE_EXPORT_INFO;

    DECLARE_VISIT_CHILDREN;

    JS_EXPORT_PRIVATE static WebAssemblyFunction* create(VM&, JSGlobalObject*, Structure*, unsigned, const String&, JSWebAssemblyInstance*, Wasm::IPIntCallee&, WasmToWasmImportableFunction::LoadLocation, Ref<const Wasm::RTT>&&);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    Wasm::JSToWasmCallee* jsToWasmCallee() const { return m_boxedJSToWasmCallee.get(); }
    JS_EXPORT_PRIVATE Wasm::JSToWasmCallee& ensureJSToWasmCallee();
    CodePtr<WasmEntryPtrTag> jsToWasm(ArityCheckMode arity)
    {
        ASSERT_UNUSED(arity, arity == ArityCheckMode::ArityCheckNotRequired || arity == ArityCheckMode::MustCheckArity);
        return ensureJSToWasmCallee().entrypoint();
    }

    CodePtr<JSEntryPtrTag> jsCallICEntrypoint()
    {
#if ENABLE(JIT)
        // UNGIL SD7/§I item (2) interim (AB-15): until the generated-code arm
        // lands (VMLite::isSpawned JSToWasm prologue check), refuse the warm
        // JS->wasm IC entrypoint entirely under useJSThreads — every call
        // takes the cold callWebAssemblyFunction path, where the SD7
        // spawned-thread refusal trips deterministically. Without this, a
        // spawned Thread WARM-calling a carrier-created export would run
        // wasm whose stack checks compare CARRIER-published limits (wasm
        // soft-limit reads are VM-level/carrier-only by AB-17 §A.2.2
        // premise (b)) against the spawned thread's stack pointer — silent
        // missed-overflow corruption instead of fail-stop. Flag-off cost:
        // zero (one option load on an IC-miss-only path).
        if (Options::useJSThreads()) [[unlikely]]
            return nullptr;

        if (m_taintedness >= SourceTaintedOrigin::IndirectlyTainted)
            return nullptr;

        // Prep the entrypoint for the slow path.
        executable()->entrypointFor(CodeSpecializationKind::CodeForCall, ArityCheckMode::MustCheckArity);
        if (!m_jsToWasmICJITCode)
            m_jsToWasmICJITCode = signature().jsToWasmICEntrypoint();
        return m_jsToWasmICJITCode;
#else
        return nullptr;
#endif
    }

    SourceTaintedOrigin taintedness() const { return m_taintedness; }

    static constexpr ptrdiff_t offsetOfBoxedJSToWasmCallee() { return OBJECT_OFFSETOF(WebAssemblyFunction, m_boxedJSToWasmCallee); }
    static constexpr ptrdiff_t offsetOfFrameSize() { return OBJECT_OFFSETOF(WebAssemblyFunction, m_frameSize); }

private:
    WebAssemblyFunction(VM&, NativeExecutable*, JSGlobalObject*, Structure*, JSWebAssemblyInstance*, Wasm::IPIntCallee&, WasmToWasmImportableFunction::LoadLocation entrypointLoadLocation, Ref<const Wasm::RTT>&&);

    CodePtr<JSEntryPtrTag> jsCallEntrypointSlow();

    // The JS->Wasm interpreter trampoline reads this directly. Populated by
    // ensureJSToWasmCallee, which create() invokes before the wrapper is exposed.
    RefPtr<Wasm::JSToWasmCallee, BoxedNativeCalleePtrTraits<Wasm::JSToWasmCallee>> m_boxedJSToWasmCallee;
    uint32_t m_frameSize { 0 };
    SourceTaintedOrigin m_taintedness;

#if ENABLE(JIT)
    CodePtr<JSEntryPtrTag> m_jsToWasmICJITCode;
#endif
};

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
