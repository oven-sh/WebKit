/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
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

#include "AbstractModuleRecord.h"
#include "ArgList.h"
#include "SourceCode.h"

namespace JSC {

class SyntheticSourceProvider;

class JSGlobalObject;

// https://tc39.es/proposal-json-modules/#sec-synthetic-module-records
class SyntheticModuleRecord final : public AbstractModuleRecord {
    friend class LLIntOffsetsExtractor;
public:
    using Base = AbstractModuleRecord;

    DECLARE_EXPORT_INFO;

    DECLARE_VISIT_CHILDREN;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.syntheticModuleRecordSpace<mode>();
    }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue);
    static SyntheticModuleRecord* create(JSGlobalObject*, VM&, Structure*, const Identifier& moduleKey, SourceProviderSourceType);

    static SyntheticModuleRecord* parseJSONModule(JSGlobalObject*, const Identifier& moduleKey, SourceCode&&);
    // Module graph instances: JSON modules carry mutable state (their parsed
    // value), so each graph gets its own environment with a fresh parse.
    // True for data modules: a JSON source to re-parse, or exports that are all
    // plain data (primitives / arrays / plain objects), deep-copied per graph.
    // Native/builtin modules (functions, host objects, lazy exports) are shared.
    bool hasPerGraphInstanceState();
    JSModuleEnvironment* createGraphInstanceEnvironment(JSGlobalObject*);
    // Host synthetic modules with per-graph state of their own (a CommonJS
    // module behind an ESM import): the provider regenerates per graph, and if
    // the record was first created for a graph, the primary bindings stay lazy
    // until the primary graph links to them (then the provider runs for it).
    void setSyntheticSourceProvider(RefPtr<SyntheticSourceProvider>&& provider, bool primaryPending) { m_provider = WTF::move(provider); m_primaryPending = primaryPending; }
    // The primary environment's values are produced on first use by the primary graph.
    bool primaryPending() const { return m_primaryPending; }
    SyntheticSourceProvider* syntheticSourceProvider() const { return m_provider.get(); }
    void materializePrimaryIfPending(JSGlobalObject*);
    static SyntheticModuleRecord* createTextModule(JSGlobalObject*, const Identifier& moduleKey, SourceCode&&);

    Synchronousness link(JSGlobalObject*, RefPtr<ScriptFetcher> = nullptr);
    JS_EXPORT_PRIVATE JSValue NODELETE evaluate(JSGlobalObject*);

#if USE(BUN_JSC_ADDITIONS)
    // Creates a record (reported as a JavaScript module, SourceProviderSourceType::Module) exporting exportValues
    // under exportNames. An empty JSValue in exportValues declares a lazy export: its binding is left uninitialized
    // and is filled in by materializeLazyExport(), which reads the property of the same name off lazyExportsSource.
    // That happens the first time something binds to the export, i.e. when an importing module links a named import
    // of it or when it is read off a module namespace object.
    JS_EXPORT_PRIVATE static SyntheticModuleRecord* tryCreateWithExportNamesAndValues(JSGlobalObject*, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, JSObject* lazyExportsSource);

    bool hasLazyExports() const { return !!m_lazyExportsSource; }

    // No-op unless this record has lazy exports and localName is one of them that nobody has materialized (or
    // overridden through JSModuleNamespaceObject::overrideExportValue) yet. May run arbitrary JS and throw.
    JS_EXPORT_PRIVATE void materializeLazyExport(JSGlobalObject*, PropertyName localName);

    // Convenience for code holding a Resolution: materializes the binding if it points into a lazy synthetic module.
    static void materializeLazyExport(JSGlobalObject*, AbstractModuleRecord*, PropertyName localName);
#endif

private:
    SyntheticModuleRecord(VM&, Structure*, const Identifier& moduleKey, SourceProviderSourceType);

    static SyntheticModuleRecord* tryCreateDefaultExportSyntheticModule(JSGlobalObject*, const Identifier& moduleKey, JSValue, SourceProviderSourceType);
    static SyntheticModuleRecord* tryCreateWithExportNamesAndValues(JSGlobalObject*, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType);
#if USE(BUN_JSC_ADDITIONS)
    static SyntheticModuleRecord* tryCreateWithExportNamesAndValues(JSGlobalObject*, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType, JSObject* lazyExportsSource);
#endif

    void finishCreation(JSGlobalObject*, VM&);

#if USE(BUN_JSC_ADDITIONS)
    WriteBarrier<JSObject> m_lazyExportsSource;
    SourceCode m_jsonSource;
    RefPtr<SyntheticSourceProvider> m_provider;
    bool m_primaryPending { false };
    enum class PlainDataState : uint8_t { Unknown, Yes, No };
    PlainDataState m_plainDataState { PlainDataState::Unknown };
#endif
};

} // namespace JSC
