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

#include "config.h"
#include "SyntheticModuleRecord.h"

#include "ArgList.h"
#include "BuiltinNames.h"
#include "ImportOrderWalk.h"
#include "JSCInlines.h"
#include "JSModuleEnvironment.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "JSONObject.h"

namespace JSC {

const ClassInfo SyntheticModuleRecord::s_info = { "ModuleRecord"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(SyntheticModuleRecord) };


Structure* SyntheticModuleRecord::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

SyntheticModuleRecord* SyntheticModuleRecord::create(JSGlobalObject* globalObject, VM& vm, Structure* structure, JSModuleLoader* moduleLoader, const Identifier& moduleKey, SourceProviderSourceType sourceType)
{
    SyntheticModuleRecord* instance = new (NotNull, allocateCell<SyntheticModuleRecord>(vm)) SyntheticModuleRecord(vm, structure, moduleLoader, moduleKey, sourceType);
    instance->finishCreation(globalObject, vm);
    return instance;
}

SyntheticModuleRecord::SyntheticModuleRecord(VM& vm, Structure* structure, JSModuleLoader* moduleLoader, const Identifier& moduleKey, SourceProviderSourceType sourceType)
    : Base(vm, structure, moduleLoader, moduleKey, sourceType)
{
}

void SyntheticModuleRecord::destroy(JSCell* cell)
{
    SyntheticModuleRecord* thisObject = static_cast<SyntheticModuleRecord*>(cell);
    thisObject->SyntheticModuleRecord::~SyntheticModuleRecord();
}

void SyntheticModuleRecord::finishCreation(JSGlobalObject* globalObject, VM& vm)
{
    Base::finishCreation(globalObject, vm);
    ASSERT(inherits(info()));
}

template<typename Visitor>
void SyntheticModuleRecord::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    SyntheticModuleRecord* thisObject = uncheckedDowncast<SyntheticModuleRecord>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
#if USE(BUN_JSC_ADDITIONS)
    visitor.append(thisObject->m_lazyExportsSource);
    visitor.append(thisObject->m_recordOfLaterExports);
    visitor.append(thisObject->m_errorOfGenerator);
    Locker locker { thisObject->cellLock() };
    visitor.append(thisObject->m_namespacesOfExportsSoFar.begin(), thisObject->m_namespacesOfExportsSoFar.end());
#endif
}

DEFINE_VISIT_CHILDREN(SyntheticModuleRecord);

Synchronousness SyntheticModuleRecord::link(JSGlobalObject*, RefPtr<ScriptFetcher>)
{
    return Synchronousness::Sync;
}

#if USE(BUN_JSC_ADDITIONS)
SyntheticModuleRecord* SyntheticModuleRecord::createWithDeferredGenerator(JSGlobalObject* globalObject, JSModuleLoader* moduleLoader, const Identifier& moduleKey, Ref<SyntheticSourceProvider>&& generator)
{
    VM& vm = globalObject->vm();
    auto* moduleRecord = create(globalObject, vm, globalObject->syntheticModuleRecordStructure(), moduleLoader, moduleKey, SourceProviderSourceType::Module);
    moduleRecord->m_generator = WTF::move(generator);
    moduleRecord->m_isOfDeferredModule = true;
    return moduleRecord;
}

void SyntheticModuleRecord::setGeneratorOfMutableExports(Ref<SyntheticSourceProvider>&& generator)
{
    ASSERT(generator->copiesMutableExports() && !hasLiveExports());
    m_generator = WTF::move(generator);
}

void SyntheticModuleRecord::runDeferredModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    MarkedArgumentBuffer exportValues;
    Vector<Identifier, 4> exportNames;
    Ref generator = *m_generator;
    bool isStillRunning = std::exchange(m_isRunningDeferredModule, true);
    JSObject* lazyExportsSource = generator->deferredModule()->generate(globalObject, this, exportNames, exportValues, isStillRunning);
    m_isRunningDeferredModule = isStillRunning;

    if (scope.exception()) [[unlikely]] {
    } else if (!moduleEnvironmentMayBeNull()) [[likely]]
        initializeExports(globalObject, exportNames, exportValues, lazyExportsSource);
    else if (updateExports(globalObject, exportNames, exportValues, lazyExportsSource)) {
        moduleLoader()->forgetFailuresToLink();
        for (unsigned index = 0; index < m_namespacesOfExportsSoFar.size() && !scope.exception(); ++index)
            m_namespacesOfExportsSoFar[index]->listExportsAgain(globalObject);
    }
    if (isStillRunning) [[unlikely]]
        RELEASE_AND_RETURN(scope, void());
    didGenerate(vm, scope);
    Locker locker { cellLock() };
    m_namespacesOfExportsSoFar.clear();
}

void SyntheticModuleRecord::didListExportsSoFar(VM& vm, JSModuleNamespaceObject* namespaceObject)
{
    Locker locker { cellLock() };
    m_namespacesOfExportsSoFar.append(WriteBarrier<JSModuleNamespaceObject>(vm, this, namespaceObject));
}

void SyntheticModuleRecord::copyMutableExportsIfOutdated(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(!deferredModule());

    RefPtr generator = std::exchange(m_generator, nullptr);
    if (!generator || wasReached() || loadedAt() == vm.importOrderState().clock)
        return;

    MarkedArgumentBuffer exportValues;
    Vector<Identifier, 4> exportNames;
    JSObject* lazyExportsSource = generator->generate(globalObject, moduleKey(), exportNames, exportValues);
    // A getter among the properties may have loaded a graph that imports this module.
    if (wasReached()) [[unlikely]]
        RELEASE_AND_RETURN(scope, void());
    if (!scope.exception()) [[likely]] {
        clearExports();
        m_lazyExportsSource.clear();
        initializeExports(globalObject, exportNames, exportValues, lazyExportsSource);
    }
    didGenerate(vm, scope);
}

void SyntheticModuleRecord::didGenerate(VM& vm, ThrowScope& scope)
{
    Exception* exception = scope.exception();
    if (exception && vm.isTerminationException(exception)) [[unlikely]]
        return;
    m_generator = nullptr;
    if (exception) [[unlikely]]
        m_errorOfGenerator.set(vm, this, exception->value());
}

// Importers may be bound to the names the record has, so it keeps them all. Returns whether it has new ones.
bool SyntheticModuleRecord::updateExports(JSGlobalObject* globalObject, const Vector<Identifier, 4>& exportNames, ArgList exportValues, JSObject* lazyExportsSource)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (lazyExportsSource)
        m_lazyExportsSource.set(vm, this, lazyExportsSource);

    bool hasNewNames = false;
    Vector<Identifier, 4> laterNames;
    MarkedArgumentBuffer laterValues;
    for (unsigned index = 0; index < exportNames.size(); ++index) {
        const Identifier& exportName = exportNames[index];
        JSValue exportValue = exportValues.at(index);
        std::optional<ExportEntry> entry = tryGetExportEntry(exportName.impl());
        if (!entry || entry->type == ExportEntry::Type::Indirect) {
            hasNewNames |= !entry;
            laterNames.append(exportName);
            laterValues.appendWithCrashOnOverflow(exportValue);
            continue;
        }
        if (!exportValue)
            continue;
        constexpr bool shouldThrowReadOnlyError = false;
        constexpr bool ignoreReadOnlyErrors = true;
        bool putResult = false;
        symbolTablePutTouchWatchpointSet(moduleEnvironment(), globalObject, exportName, exportValue, shouldThrowReadOnlyError, ignoreReadOnlyErrors, putResult);
        RETURN_IF_EXCEPTION(scope, false);
        ASSERT(putResult);
    }
    if (laterNames.isEmpty())
        return false;

    if (m_recordOfLaterExports) {
        m_recordOfLaterExports->updateExports(globalObject, laterNames, laterValues, lazyExportsSource);
        RETURN_IF_EXCEPTION(scope, false);
    } else {
        auto* record = tryCreateWithExportNamesAndValues(globalObject, moduleLoader(), moduleKey(), laterNames, laterValues, lazyExportsSource);
        RETURN_IF_EXCEPTION(scope, false);
        record->setWasReached();
        m_recordOfLaterExports.set(vm, this, record);
    }
    for (const Identifier& exportName : laterNames) {
        if (!exportEntries().contains(exportName.impl()))
            addExportEntry(ExportEntry::createIndirect(exportName, exportName, moduleKey(), ScriptFetchParameters::Type::JavaScript));
    }
    return hasNewNames;
}
#endif

JSValue SyntheticModuleRecord::evaluate([[maybe_unused]] JSGlobalObject* globalObject)
{
#if USE(BUN_JSC_ADDITIONS)
    // An importer can have been linked to what it had exported by then.
    if (JSValue error = errorOfGenerator()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        throwException(globalObject, scope, error);
        return { };
    }
#endif
    return jsUndefined();
}

#if USE(BUN_JSC_ADDITIONS)
SyntheticModuleRecord* SyntheticModuleRecord::tryCreateWithExportNamesAndValues(JSGlobalObject* globalObject, JSModuleLoader* moduleLoader, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType sourceType)
{
    return tryCreateWithExportNamesAndValues(globalObject, moduleLoader, moduleKey, exportNames, exportValues, sourceType, nullptr);
}

SyntheticModuleRecord* SyntheticModuleRecord::tryCreateWithExportNamesAndValues(JSGlobalObject* globalObject, JSModuleLoader* moduleLoader, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, JSObject* lazyExportsSource, LazyExports lazyExports)
{
    return tryCreateWithExportNamesAndValues(globalObject, moduleLoader, moduleKey, exportNames, exportValues, SourceProviderSourceType::Module, lazyExportsSource, lazyExports);
}

SyntheticModuleRecord* SyntheticModuleRecord::tryCreateWithExportNamesAndValues(JSGlobalObject* globalObject, JSModuleLoader* moduleLoader, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType sourceType, JSObject* lazyExportsSource, LazyExports lazyExports)
#else
SyntheticModuleRecord* SyntheticModuleRecord::tryCreateWithExportNamesAndValues(JSGlobalObject* globalObject, JSModuleLoader* moduleLoader, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType sourceType)
#endif
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* moduleRecord = create(globalObject, vm, globalObject->syntheticModuleRecordStructure(), moduleLoader, moduleKey, sourceType);
#if USE(BUN_JSC_ADDITIONS)
    moduleRecord->initializeExports(globalObject, exportNames, exportValues, lazyExportsSource);
    RETURN_IF_EXCEPTION(scope, { });
    if (lazyExports == LazyExports::Live) {
        RELEASE_ASSERT(lazyExportsSource);
        moduleRecord->m_lazyExportsSource.clear();
        moduleRecord->setHasLiveExports();
        moduleRecord->setLiveExportsSource(vm, lazyExportsSource);
    }
#else
    moduleRecord->initializeExports(globalObject, exportNames, exportValues, nullptr);
#endif
    RETURN_IF_EXCEPTION(scope, { });
    return moduleRecord;
}

void SyntheticModuleRecord::initializeExports(JSGlobalObject* globalObject, const Vector<Identifier, 4>& exportNames, ArgList exportValues, [[maybe_unused]] JSObject* lazyExportsSource)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(exportNames.size() == exportValues.size());

    auto* moduleRecord = this;

    SymbolTable* exportSymbolTable = SymbolTable::create(vm);
    {
        auto offset = exportSymbolTable->takeNextScopeOffset(NoLockingNecessary);
        exportSymbolTable->add(NoLockingNecessary, vm.propertyNames->starNamespacePrivateName.impl(), SymbolTableEntry(VarOffset(offset)));
        offset = exportSymbolTable->takeNextScopeOffset(NoLockingNecessary);
        exportSymbolTable->add(NoLockingNecessary, vm.propertyNames->builtinNames().moduleLoaderPrivateName().impl(), SymbolTableEntry(VarOffset(offset)));
    }
    for (auto& exportName : exportNames) {
        auto offset = exportSymbolTable->takeNextScopeOffset(NoLockingNecessary);
        exportSymbolTable->add(NoLockingNecessary, exportName.impl(), SymbolTableEntry(VarOffset(offset)));
        moduleRecord->addExportEntry(ExportEntry::createLocal(exportName, exportName));
    }

    JSModuleEnvironment* moduleEnvironment = JSModuleEnvironment::create(vm, globalObject, nullptr, exportSymbolTable, jsTDZValue(), moduleRecord);
    moduleRecord->setModuleEnvironment(globalObject, moduleEnvironment);
    RETURN_IF_EXCEPTION(scope, void());

#if USE(BUN_JSC_ADDITIONS)
    bool hasLazyExports = false;
#endif
    for (unsigned index = 0; index < exportNames.size(); ++index) {
        PropertyName exportName = exportNames[index];
        JSValue exportValue = exportValues.at(index);
#if USE(BUN_JSC_ADDITIONS)
        if (!exportValue) {
            // Lazy export: JSModuleEnvironment::create() above initialized the binding to the TDZ value, and it stays
            // that way until materializeLazyExport() fills it in.
            ASSERT(lazyExportsSource);
            hasLazyExports = true;
            continue;
        }
#endif
        constexpr bool shouldThrowReadOnlyError = false;
        constexpr bool ignoreReadOnlyErrors = true;
        bool putResult = false;
        symbolTablePutTouchWatchpointSet(moduleEnvironment, globalObject, exportName, exportValue, shouldThrowReadOnlyError, ignoreReadOnlyErrors, putResult);
        RETURN_IF_EXCEPTION(scope, void());
        ASSERT(putResult);
    }

#if USE(BUN_JSC_ADDITIONS)
    if (hasLazyExports)
        moduleRecord->m_lazyExportsSource.set(vm, moduleRecord, lazyExportsSource);
#endif
}

#if USE(BUN_JSC_ADDITIONS)
void SyntheticModuleRecord::materializeLazyExport(JSGlobalObject* globalObject, PropertyName localName)
{
    JSObject* source = m_lazyExportsSource.get();
    if (!source)
        return;

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // *namespace* lives in the same symbol table but is not an export; getModuleNamespace() owns that binding.
    if (localName == vm.propertyNames->starNamespacePrivateName)
        return;

    JSModuleEnvironment* environment = moduleEnvironment();
    SymbolTable* symbolTable = environment->symbolTable();
    ScopeOffset scopeOffset;
    {
        ConcurrentJSLocker locker(symbolTable->m_lock);
        auto iter = symbolTable->find(locker, localName.uid());
        if (iter == symbolTable->end(locker))
            return;
        scopeOffset = iter->value.scopeOffset();
    }

    // Either the value was provided up front, an earlier call materialized it, or something wrote the binding
    // directly (JSModuleNamespaceObject::overrideExportValue). In all of those cases the binding is what it should be.
    if (environment->variableAt(scopeOffset).get())
        return;

    JSValue value = source->get(globalObject, localName);
    RETURN_IF_EXCEPTION(scope, void());

    // The getter may have re-entered and filled this binding itself. Whatever got there first is what any binding
    // created in the meantime has observed, so keep it.
    if (environment->variableAt(scopeOffset).get())
        return;

    constexpr bool shouldThrowReadOnlyError = false;
    constexpr bool ignoreReadOnlyErrors = true;
    bool putResult = false;
    symbolTablePutTouchWatchpointSet(environment, globalObject, localName, value, shouldThrowReadOnlyError, ignoreReadOnlyErrors, putResult);
    RETURN_IF_EXCEPTION(scope, void());
    ASSERT(putResult);
}

void SyntheticModuleRecord::materializeLazyExport(JSGlobalObject* globalObject, AbstractModuleRecord* moduleRecord, PropertyName localName)
{
    auto* syntheticModuleRecord = dynamicDowncast<SyntheticModuleRecord>(moduleRecord);
    if (!syntheticModuleRecord || !syntheticModuleRecord->hasLazyExports()) [[likely]]
        return;
    syntheticModuleRecord->materializeLazyExport(globalObject, localName);
}
#endif

SyntheticModuleRecord* SyntheticModuleRecord::tryCreateDefaultExportSyntheticModule(JSGlobalObject* globalObject, JSModuleLoader* moduleLoader, const Identifier& moduleKey, JSValue defaultExport, SourceProviderSourceType sourceType)
{
    VM& vm = globalObject->vm();

    Vector<Identifier, 4> exportNames;
    auto exportValues = WTF::toArray<EncodedJSValue>({
        JSValue::encode(defaultExport),
    });
    exportNames.append(vm.propertyNames->defaultKeyword);
    return tryCreateWithExportNamesAndValues(globalObject, moduleLoader, moduleKey, exportNames, ArgList { exportValues.data(), exportValues.size() }, sourceType);
}

SyntheticModuleRecord* SyntheticModuleRecord::parseJSONModule(JSGlobalObject* globalObject, JSModuleLoader* moduleLoader, const Identifier& moduleKey, SourceCode&& sourceCode)
{
    // https://tc39.es/proposal-json-modules/#sec-parse-json-module
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue result = JSONParseWithException(globalObject, sourceCode.view());
    RETURN_IF_EXCEPTION(scope, { });

    RELEASE_AND_RETURN(scope, SyntheticModuleRecord::tryCreateDefaultExportSyntheticModule(globalObject, moduleLoader, moduleKey, result, SourceProviderSourceType::JSON));
}

SyntheticModuleRecord* SyntheticModuleRecord::createTextModule(JSGlobalObject* globalObject, JSModuleLoader* moduleLoader, const Identifier& moduleKey, SourceCode&& sourceCode)
{
    // https://tc39.es/proposal-import-text/#sec-create-text-module
    VM& vm = globalObject->vm();
    return SyntheticModuleRecord::tryCreateDefaultExportSyntheticModule(globalObject, moduleLoader, moduleKey, jsString(vm, sourceCode.view()), SourceProviderSourceType::Text);
}

} // namespace JSC
