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
#if USE(BUN_JSC_ADDITIONS)
    JS_EXPORT_PRIVATE JSValue evaluate(JSGlobalObject*);
#else
    JS_EXPORT_PRIVATE JSValue LEGACY_NODELETE evaluate(JSGlobalObject*);
#endif

#if USE(BUN_JSC_ADDITIONS)
    // Creates a record (reported as a JavaScript module, SourceProviderSourceType::Module) exporting exportValues
    // under exportNames. An empty JSValue in exportValues declares a lazy export: its binding is left uninitialized
    // and is filled in by materializeLazyExport(), which reads the property of the same name off lazyExportsSource.
    // That happens the first time something binds to the export, i.e. when an importing module links a named import
    // of it or when it is read off a module namespace object.
    JS_EXPORT_PRIVATE static SyntheticModuleRecord* tryCreateWithExportNamesAndValues(JSGlobalObject*, JSModuleLoader*, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, JSObject* lazyExportsSource);

    // Creates a record without exports for a SyntheticSourceProvider::createDeferred() provider: the record
    // hasPendingExports() until runDeferredGenerator() gives it its exports. evaluate() calls that, so the generator runs
    // where InnerModuleEvaluation reaches the record. Until then modules that import from the record link without its
    // export names (Resolution::Type::Pending) and bind when they are about to run (CyclicModuleRecord::linkPendingImports).
    static SyntheticModuleRecord* createWithDeferredGenerator(JSGlobalObject*, JSModuleLoader*, const Identifier& moduleKey, Ref<SyntheticSourceProvider>&&);

    // No-op unless the record still has a deferred generator. Runs arbitrary JS and throws what the generator throws,
    // again on every later call.
    void runDeferredGenerator(JSGlobalObject*);

    // For a record that hasPendingExports(): the scope offset at which its module environment will hold `exportName`,
    // for code that embeds the offset before the environment exists. That is code linked while the record has not
    // evaluated (an import cycle, JSScope::abstractResolve) and code shared with a record of another loader
    // (JSModuleRecord::resolvesImportsLike). The second form asks for the offset that shared code embeds: false if the
    // name or the offset is promised otherwise.
    unsigned reserveExportOffset(UniquedStringImpl* exportName);
    bool reserveExportOffset(UniquedStringImpl* exportName, unsigned scopeOffset);

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
    static SyntheticModuleRecord* tryCreateWithExportNamesAndValues(JSGlobalObject*, JSModuleLoader*, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType, JSObject* lazyExportsSource);
#endif

    void finishCreation(JSGlobalObject*, VM&);

    void initializeExports(JSGlobalObject*, const Vector<Identifier, 4>& exportNames, ArgList exportValues, JSObject* lazyExportsSource);

#if USE(BUN_JSC_ADDITIONS)
    // Before *namespace* and the loader there are no exports.
    static constexpr unsigned firstExportOffset = 2;
public:
    // Where every synthetic record's environment keeps the record's namespace object (*namespace*).
    static constexpr unsigned namespaceScopeOffset = 0;
private:

    struct Deferred {
        WTF_MAKE_STRUCT_TZONE_ALLOCATED(Deferred);
        Deferred(Ref<SyntheticSourceProvider>&& generator)
            : generator(WTF::move(generator))
        {
        }

        Ref<SyntheticSourceProvider> generator;
        UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, unsigned, IdentifierRepHash> reservedOffsetByName;
        UncheckedKeyHashSet<unsigned, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> reservedOffsets;
        unsigned nextOffset { firstExportOffset };
    };

    WriteBarrier<JSObject> m_lazyExportsSource;
    std::unique_ptr<Deferred> m_deferred;
    WriteBarrier<Unknown> m_deferredGeneratorError;
#endif
};

} // namespace JSC
