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

class JSGlobalObject;
class JSModuleLoader;

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
    static SyntheticModuleRecord* create(JSGlobalObject*, VM&, Structure*, JSModuleLoader*, const Identifier& moduleKey, SourceProviderSourceType);

    static SyntheticModuleRecord* tryCreateDefaultExportSyntheticModule(JSGlobalObject*, JSModuleLoader*, const Identifier& moduleKey, JSValue, SourceProviderSourceType);

    static SyntheticModuleRecord* parseJSONModule(JSGlobalObject*, JSModuleLoader*, const Identifier& moduleKey, SourceCode&&);
    static SyntheticModuleRecord* createTextModule(JSGlobalObject*, JSModuleLoader*, const Identifier& moduleKey, SourceCode&&);

    Synchronousness link(JSGlobalObject*, RefPtr<ScriptFetcher> = nullptr);
    NODELETE JS_EXPORT_PRIVATE JSValue evaluate(JSGlobalObject*);

#if USE(BUN_JSC_ADDITIONS)
    // Creates a record (reported as a JavaScript module, SourceProviderSourceType::Module) exporting exportValues
    // under exportNames. An empty JSValue in exportValues declares a lazy export: its binding is left uninitialized
    // and is filled in by materializeLazyExport(), which reads the property of the same name off lazyExportsSource.
    // That happens the first time something binds to the export, i.e. when an importing module links a named import
    // of it or when it is read off a module namespace object.
    // LazyExports::Live gives the record live exports (AbstractModuleRecord::hasLiveExports) backed by lazyExportsSource.
    // Its bindings stay uninitialized.
    enum class LazyExports : bool { ReadOnce, Live };
    JS_EXPORT_PRIVATE static SyntheticModuleRecord* tryCreateWithExportNamesAndValues(JSGlobalObject*, JSModuleLoader*, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, JSObject* lazyExportsSource, LazyExports = LazyExports::ReadOnce);

    // Creates a record without exports for a SyntheticSourceProvider::createDeferred() provider. An ImportOrderWalk gives
    // the record its exports before anything links against it.
    static SyntheticModuleRecord* createWithDeferredGenerator(JSGlobalObject*, JSModuleLoader*, const Identifier& moduleKey, Ref<SyntheticSourceProvider>&&);
    // See SyntheticSourceProvider::copiesMutableExports().
    void setGeneratorOfMutableExports(Ref<SyntheticSourceProvider>&&);

    // Until the module has run to its end or thrown.
    SyntheticSourceProvider::DeferredModule* deferredModule() const { return m_generator ? m_generator->deferredModule() : nullptr; }
    RefPtr<SyntheticSourceProvider> ownerOfDeferredModule() const { return m_generator; }
    bool isOfDeferredModule() const { return m_isOfDeferredModule; }
    bool isRunningDeferredModule() const { return m_isRunningDeferredModule; }
    void didStopRunningDeferredModule() { m_isRunningDeferredModule = false; }
    JSValue errorOfGenerator() const { return m_errorOfGenerator.get(); }
    bool isReadyToBeLinked() const { return wasReached() && !m_generator && !m_errorOfGenerator; }

    // These two run arbitrary JS and throw what the generator throws.
    // A load that the running module started, and that comes back to it, links against what it has exported so far.
    void runDeferredModule(JSGlobalObject*);
    void copyMutableExportsIfOutdated(JSGlobalObject*);

    // An environment cannot grow, so the bindings of the names that were exported after it was made are another record's.
    SyntheticModuleRecord* recordOfLaterExports() const { return m_recordOfLaterExports.get(); }
    void didListExportsSoFar(VM&, JSModuleNamespaceObject*);

    bool hasLazyExports() const { return !!m_lazyExportsSource; }

    // No-op unless this record has lazy exports and localName is one of them that nobody has materialized (or
    // overridden through JSModuleNamespaceObject::overrideExportValue) yet. May run arbitrary JS and throw.
    JS_EXPORT_PRIVATE void materializeLazyExport(JSGlobalObject*, PropertyName localName);

    // Convenience for code holding a Resolution: materializes the binding if it points into a lazy synthetic module.
    static void materializeLazyExport(JSGlobalObject*, AbstractModuleRecord*, PropertyName localName);
#endif

private:
    SyntheticModuleRecord(VM&, Structure*, JSModuleLoader*, const Identifier& moduleKey, SourceProviderSourceType);

    static SyntheticModuleRecord* tryCreateWithExportNamesAndValues(JSGlobalObject*, JSModuleLoader*, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType);
#if USE(BUN_JSC_ADDITIONS)
    static SyntheticModuleRecord* tryCreateWithExportNamesAndValues(JSGlobalObject*, JSModuleLoader*, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType, JSObject* lazyExportsSource, LazyExports = LazyExports::ReadOnce);
#endif

    void finishCreation(JSGlobalObject*, VM&);

    void initializeExports(JSGlobalObject*, const Vector<Identifier, 4>& exportNames, ArgList exportValues, JSObject* lazyExportsSource);
#if USE(BUN_JSC_ADDITIONS)
    bool updateExports(JSGlobalObject*, const Vector<Identifier, 4>& exportNames, ArgList exportValues, JSObject* lazyExportsSource);
    void didGenerate(VM&, ThrowScope&);
#endif

#if USE(BUN_JSC_ADDITIONS)
    WriteBarrier<JSObject> m_lazyExportsSource;
    RefPtr<SyntheticSourceProvider> m_generator;
    WriteBarrier<SyntheticModuleRecord> m_recordOfLaterExports;
    Vector<WriteBarrier<JSModuleNamespaceObject>> m_namespacesOfExportsSoFar;
    WriteBarrier<Unknown> m_errorOfGenerator;
    bool m_isOfDeferredModule { false };
    bool m_isRunningDeferredModule { false };
#endif
};

} // namespace JSC
