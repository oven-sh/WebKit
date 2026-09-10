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
#include "JSCInlines.h"
#include "SourceProvider.h"
#include "JSModuleEnvironment.h"
#include "JSModuleNamespaceObject.h"
#include "JSONObject.h"

namespace JSC {

const ClassInfo SyntheticModuleRecord::s_info = { "ModuleRecord"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(SyntheticModuleRecord) };


Structure* SyntheticModuleRecord::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

SyntheticModuleRecord* SyntheticModuleRecord::create(JSGlobalObject* globalObject, VM& vm, Structure* structure, const Identifier& moduleKey, SourceProviderSourceType sourceType)
{
    SyntheticModuleRecord* instance = new (NotNull, allocateCell<SyntheticModuleRecord>(vm)) SyntheticModuleRecord(vm, structure, moduleKey, sourceType);
    instance->finishCreation(globalObject, vm);
    return instance;
}

SyntheticModuleRecord::SyntheticModuleRecord(VM& vm, Structure* structure, const Identifier& moduleKey, SourceProviderSourceType sourceType)
    : Base(vm, structure, moduleKey, sourceType)
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
#endif
}

DEFINE_VISIT_CHILDREN(SyntheticModuleRecord);

Synchronousness SyntheticModuleRecord::link(JSGlobalObject*, RefPtr<ScriptFetcher>)
{
    return Synchronousness::Sync;
}

JSValue SyntheticModuleRecord::evaluate(JSGlobalObject* globalObject)
{
    if (m_primaryPending) [[unlikely]] {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        materializePrimaryIfPending(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }

    return jsUndefined();
}

#if USE(BUN_JSC_ADDITIONS)
SyntheticModuleRecord* SyntheticModuleRecord::tryCreateWithExportNamesAndValues(JSGlobalObject* globalObject, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType sourceType)
{
    return tryCreateWithExportNamesAndValues(globalObject, moduleKey, exportNames, exportValues, sourceType, nullptr);
}

SyntheticModuleRecord* SyntheticModuleRecord::tryCreateWithExportNamesAndValues(JSGlobalObject* globalObject, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, JSObject* lazyExportsSource)
{
    return tryCreateWithExportNamesAndValues(globalObject, moduleKey, exportNames, exportValues, SourceProviderSourceType::Module, lazyExportsSource);
}

SyntheticModuleRecord* SyntheticModuleRecord::tryCreateWithExportNamesAndValues(JSGlobalObject* globalObject, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType sourceType, JSObject* lazyExportsSource)
#else
SyntheticModuleRecord* SyntheticModuleRecord::tryCreateWithExportNamesAndValues(JSGlobalObject* globalObject, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType sourceType)
#endif
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(exportNames.size() == exportValues.size());

    auto* moduleRecord = create(globalObject, vm, globalObject->syntheticModuleRecordStructure(), moduleKey, sourceType);

    SymbolTable* exportSymbolTable = SymbolTable::create(vm);
    {
        auto offset = exportSymbolTable->takeNextScopeOffset(NoLockingNecessary);
        exportSymbolTable->add(NoLockingNecessary, vm.propertyNames->starNamespacePrivateName.impl(), SymbolTableEntry(VarOffset(offset)));
    }
    for (auto& exportName : exportNames) {
        auto offset = exportSymbolTable->takeNextScopeOffset(NoLockingNecessary);
        exportSymbolTable->add(NoLockingNecessary, exportName.impl(), SymbolTableEntry(VarOffset(offset)));
        moduleRecord->addExportEntry(ExportEntry::createLocal(exportName, exportName));
    }

    JSModuleEnvironment* moduleEnvironment = JSModuleEnvironment::create(vm, globalObject, nullptr, exportSymbolTable, jsTDZValue(), moduleRecord);
    moduleRecord->setModuleEnvironment(globalObject, moduleEnvironment);
    RETURN_IF_EXCEPTION(scope, { });

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
        RETURN_IF_EXCEPTION(scope, { });
        ASSERT(putResult);
    }

#if USE(BUN_JSC_ADDITIONS)
    if (hasLazyExports)
        moduleRecord->m_lazyExportsSource.set(vm, moduleRecord, lazyExportsSource);
#endif

    return moduleRecord;

}

#if USE(BUN_JSC_ADDITIONS)
void SyntheticModuleRecord::materializeLazyExport(JSGlobalObject* globalObject, PropertyName localName)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (m_primaryPending) [[unlikely]] {
        materializePrimaryIfPending(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
    }

    JSObject* source = m_lazyExportsSource.get();
    if (!source)
        return;

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

SyntheticModuleRecord* SyntheticModuleRecord::tryCreateDefaultExportSyntheticModule(JSGlobalObject* globalObject, const Identifier& moduleKey, JSValue defaultExport, SourceProviderSourceType sourceType)
{
    VM& vm = globalObject->vm();

    Vector<Identifier, 4> exportNames;
    auto exportValues = WTF::toArray<EncodedJSValue>({
        JSValue::encode(defaultExport),
    });
    exportNames.append(vm.propertyNames->defaultKeyword);
    return tryCreateWithExportNamesAndValues(globalObject, moduleKey, exportNames, ArgList { exportValues.data(), exportValues.size() }, sourceType);
}

SyntheticModuleRecord* SyntheticModuleRecord::parseJSONModule(JSGlobalObject* globalObject, const Identifier& moduleKey, SourceCode&& sourceCode)
{
    // https://tc39.es/proposal-json-modules/#sec-parse-json-module
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue result = JSONParseWithException(globalObject, sourceCode.view());
    RETURN_IF_EXCEPTION(scope, { });

    SyntheticModuleRecord* record = SyntheticModuleRecord::tryCreateDefaultExportSyntheticModule(globalObject, moduleKey, result, SourceProviderSourceType::JSON);
    RETURN_IF_EXCEPTION(scope, { });
    if (record && Options::useModuleGraphInstances())
        record->m_jsonSource = WTF::move(sourceCode);
    RELEASE_AND_RETURN(scope, record);
}

bool SyntheticModuleRecord::regeneratesPerGraphInstance() const
{
    if (!m_jsonSource.isNull())
        return true;
    return m_provider && m_provider->regeneratesPerGraphInstance();
}

void SyntheticModuleRecord::setSyntheticSourceProvider(RefPtr<SyntheticSourceProvider>&& provider, bool primaryPending)
{
    m_provider = WTF::move(provider);
    m_primaryPending = primaryPending;
}

// A fresh record for the same module in a graph instance: a JSON module parses
// its source again; a host module runs its provider again (the caller has made
// the instance the current loading instance, so the provider can consult it).
SyntheticModuleRecord* SyntheticModuleRecord::createForGraphInstance(JSGlobalObject* globalObject, SyntheticModuleRecord* templateRecord)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(templateRecord->regeneratesPerGraphInstance());
    if (!templateRecord->m_jsonSource.isNull())
        RELEASE_AND_RETURN(scope, parseJSONModule(globalObject, templateRecord->moduleKey(), SourceCode { templateRecord->m_jsonSource }));
    RefPtr<SyntheticSourceProvider> provider = templateRecord->m_provider;
    MarkedArgumentBuffer values;
    Vector<Identifier, 4> names;
    JSObject* lazyExportsSource = provider->generate(globalObject, templateRecord->moduleKey(), names, values);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (values.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    SyntheticModuleRecord* record = tryCreateWithExportNamesAndValues(globalObject, templateRecord->moduleKey(), names, values, lazyExportsSource);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (record)
        record->setSyntheticSourceProvider(WTF::move(provider), false);
    return record;
}

SyntheticModuleRecord* SyntheticModuleRecord::createPendingPrimary(JSGlobalObject* globalObject, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, RefPtr<SyntheticSourceProvider>&& provider)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* moduleRecord = create(globalObject, vm, globalObject->syntheticModuleRecordStructure(), moduleKey, SourceProviderSourceType::Module);
    SymbolTable* exportSymbolTable = SymbolTable::create(vm);
    {
        auto offset = exportSymbolTable->takeNextScopeOffset(NoLockingNecessary);
        exportSymbolTable->add(NoLockingNecessary, vm.propertyNames->starNamespacePrivateName.impl(), SymbolTableEntry(VarOffset(offset)));
    }
    for (auto& exportName : exportNames) {
        auto offset = exportSymbolTable->takeNextScopeOffset(NoLockingNecessary);
        exportSymbolTable->add(NoLockingNecessary, exportName.impl(), SymbolTableEntry(VarOffset(offset)));
        moduleRecord->addExportEntry(ExportEntry::createLocal(exportName, exportName));
    }
    JSModuleEnvironment* moduleEnvironment = JSModuleEnvironment::create(vm, globalObject, nullptr, exportSymbolTable, jsTDZValue(), moduleRecord);
    moduleRecord->setModuleEnvironment(globalObject, moduleEnvironment);
    RETURN_IF_EXCEPTION(scope, nullptr);
    moduleRecord->setSyntheticSourceProvider(WTF::move(provider), true);
    return moduleRecord;
}

void SyntheticModuleRecord::materializePrimaryIfPending(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // While a graph instance is loading, the template's own values are nobody's business yet.
    if (!m_primaryPending || !m_provider || globalObject->currentGraphInstanceForLoading())
        return;
    m_primaryPending = false; // before generate(): the provider may re-enter
    MarkedArgumentBuffer values;
    Vector<Identifier, 4> names;
    JSObject* lazyExportsSource = m_provider->generate(globalObject, moduleKey(), names, values);
    if (scope.exception()) [[unlikely]] {
        m_primaryPending = true; // a later use retries
        return;
    }
    if (values.hasOverflowed()) [[unlikely]] {
        m_primaryPending = true;
        throwOutOfMemoryError(globalObject, scope);
        return;
    }
    // Genuinely lazy exports of this run are read off the provider's source from now on.
    m_lazyExportsSource.setMayBeNull(vm, this, lazyExportsSource);
    JSModuleEnvironment* environment = moduleEnvironment();
    SymbolTable* symbolTable = environment->symbolTable();
    for (unsigned i = 0; i < names.size(); ++i) {
        JSValue value = values.at(i);
        if (!value)
            continue;
        SymbolTableEntry::Fast entry = symbolTable->get(names[i].impl());
        if (entry.isNull() || environment->variableAt(entry.scopeOffset()).get())
            continue;
        bool putResult = false;
        symbolTablePutTouchWatchpointSet(environment, globalObject, names[i], value, false, true, putResult);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

SyntheticModuleRecord* SyntheticModuleRecord::createTextModule(JSGlobalObject* globalObject, const Identifier& moduleKey, SourceCode&& sourceCode)
{
    // https://tc39.es/proposal-import-text/#sec-create-text-module
    VM& vm = globalObject->vm();
    return SyntheticModuleRecord::tryCreateDefaultExportSyntheticModule(globalObject, moduleKey, jsString(vm, sourceCode.view()), SourceProviderSourceType::Text);
}

} // namespace JSC
