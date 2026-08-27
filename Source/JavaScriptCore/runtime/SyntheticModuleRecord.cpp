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
#include "SourceProvider.h"
#include "StructureInlines.h"
#include "ArrayConstructor.h"
#include "ObjectConstructor.h"
#include "JSArray.h"

#include "ArgList.h"
#include "BuiltinNames.h"
#include "JSCInlines.h"
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
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A record first produced for a module graph instance gives the primary its
    // own values when the primary graph evaluates it.
    materializePrimaryIfPending(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
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
    JSObject* source = m_lazyExportsSource.get();
    if (!source)
        return;

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // *namespace* lives in the same symbol table but is not an export; getModuleNamespace() owns that binding.
    if (localName == vm.propertyNames->starNamespacePrivateName)
        return;

    if (m_primaryPending)
        RELEASE_AND_RETURN(scope, materializePrimaryIfPending(globalObject));

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

// Plain data = null/undefined/booleans/numbers/strings/bigints, arrays of plain
// data, and ordinary objects (Object.prototype or null prototype, data
// properties only) of plain data. Bounded so pathological modules count as "no".
static bool isPlainData(JSGlobalObject* globalObject, JSValue value, unsigned depth, unsigned& budget)
{
    if (!value || !budget--)
        return false;
    if (!value.isCell() || value.isString() || value.isBigInt() || value.isSymbol())
        return !value.isSymbol();
    if (depth > 64)
        return false;
    JSObject* object = value.getObject();
    if (!object || object->type() == JSFunctionType)
        return false;
    VM& vm = globalObject->vm();
    if (isJSArray(object)) {
        if (object->type() != ArrayType || object->getPrototypeDirect() != globalObject->arrayPrototype())
            return false;
        JSArray* array = uncheckedDowncast<JSArray>(object);
        for (unsigned i = 0; i < array->length(); ++i) {
            JSValue element = array->canGetIndexQuickly(i) ? array->getIndexQuickly(i) : JSValue();
            if (!element)
                return false;
            if (!isPlainData(globalObject, element, depth + 1, budget))
                return false;
        }
        return true;
    }
    if ((object->type() != FinalObjectType && object->type() != ObjectType) || object->inlineTypeFlags() & OverridesGetOwnPropertySlot) {
        dataLogLnIf(Options::dumpModuleLoadingState(), "[graph-instance]   not plain: type=", object->type());
        return false;
    }
    JSValue prototype = object->getPrototypeDirect();
    if (!prototype.isNull() && prototype != globalObject->objectPrototype()) {
        dataLogLnIf(Options::dumpModuleLoadingState(), "[graph-instance]   not plain: prototype");
        return false;
    }
    Structure* structure = object->structure();
    if (structure->hasAnyKindOfGetterSetterProperties() || structure->isUncacheableDictionary() || hasIndexedProperties(object->indexingType())) {
        dataLogLnIf(Options::dumpModuleLoadingState(), "[graph-instance]   not plain: getters=", structure->hasAnyKindOfGetterSetterProperties(), " uncacheableDict=", structure->isUncacheableDictionary(), " indexed=", hasIndexedProperties(object->indexingType()));
        return false;
    }
    bool ok = true;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
        if (entry.attributes() & PropertyAttribute::Accessor) {
            ok = false;
            return false;
        }
        if (!isPlainData(globalObject, object->getDirect(entry.offset()), depth + 1, budget)) {
            ok = false;
            return false;
        }
        return true;
    });
    return ok;
}

static JSValue clonePlainData(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value.isCell() || value.isString() || value.isBigInt())
        return value;
    JSObject* object = value.getObject();
    if (isJSArray(object)) {
        JSArray* source = uncheckedDowncast<JSArray>(object);
        MarkedArgumentBuffer elements;
        for (unsigned i = 0; i < source->length(); ++i) {
            JSValue element = clonePlainData(globalObject, source->canGetIndexQuickly(i) ? source->getIndexQuickly(i) : jsUndefined());
            RETURN_IF_EXCEPTION(scope, { });
            elements.append(element);
        }
        RELEASE_AND_RETURN(scope, constructArray(globalObject, static_cast<ArrayAllocationProfile*>(nullptr), elements));
    }
    JSValue prototype = object->getPrototypeDirect();
    JSObject* copy = prototype.isNull() ? constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure()) : constructEmptyObject(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    Vector<std::pair<PropertyName, PropertyOffset>, 8> properties;
    object->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
        properties.append({ entry.key(), entry.offset() });
        return true;
    });
    for (auto& [name, offset] : properties) {
        JSValue cloned = clonePlainData(globalObject, object->getDirect(offset));
        RETURN_IF_EXCEPTION(scope, { });
        copy->putDirect(vm, name, cloned);
    }
    return copy;
}

void SyntheticModuleRecord::materializePrimaryIfPending(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // While a graph is loading, the primary is nobody's business yet.
    if (!m_primaryPending || !m_provider || globalObject->currentGraphInstanceForLoading())
        return;
    m_primaryPending = false; // before generate(): the provider may re-enter
    MarkedArgumentBuffer values;
    Vector<Identifier, 4> names;
    m_provider->generate(globalObject, moduleKey(), names, values);
    if (scope.exception()) [[unlikely]] {
        m_primaryPending = true; // a later use retries
        return;
    }
    JSModuleEnvironment* environment = moduleEnvironment();
    SymbolTable* symbolTable = environment->symbolTable();
    for (const auto& [key, entry] : exportEntries()) {
        SymbolTableEntry::Fast symbolEntry = symbolTable->get(entry.localName.impl());
        if (symbolEntry.isNull())
            continue;
        JSValue value = jsUndefined();
        for (unsigned i = 0; i < names.size(); ++i) {
            if (names[i] == entry.localName) {
                value = values.at(i);
                break;
            }
        }
        environment->variableAt(symbolEntry.scopeOffset()).set(vm, environment, value ? value : jsUndefined());
    }
}

bool SyntheticModuleRecord::hasPerGraphInstanceState()
{
    if (!m_jsonSource.isNull())
        return true;
    if (m_provider && m_provider->regeneratesPerGraphInstance())
        return true;
    if (m_primaryPending)
        return false;
    if (m_plainDataState != PlainDataState::Unknown)
        return m_plainDataState == PlainDataState::Yes;
    m_plainDataState = PlainDataState::No;
#if USE(BUN_JSC_ADDITIONS)
    if (hasLazyExports())
        return false;
#endif
    JSModuleEnvironment* environment = moduleEnvironmentMayBeNull();
    if (!environment || exportEntries().isEmpty())
        return false;
    JSGlobalObject* globalObject = environment->globalObject();
    unsigned budget = 100000;
    for (const auto& [key, entry] : exportEntries()) {
        SymbolTableEntry::Fast symbolEntry = environment->symbolTable()->get(entry.localName.impl());
        if (symbolEntry.isNull()) {
            dataLogLnIf(Options::dumpModuleLoadingState(), "[graph-instance] synthetic ", moduleKey().string(), ": export ", entry.localName.string(), " has no slot");
            return false;
        }
        JSValue value = environment->variableAt(symbolEntry.scopeOffset()).get();
        if (!isPlainData(globalObject, value, 0, budget)) {
            dataLogLnIf(Options::dumpModuleLoadingState(), "[graph-instance] synthetic ", moduleKey().string(), ": export ", entry.localName.string(), " is not plain data");
            return false;
        }
    }
    m_plainDataState = PlainDataState::Yes;
    return true;
}

JSModuleEnvironment* SyntheticModuleRecord::createGraphInstanceEnvironment(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(hasPerGraphInstanceState());
    JSModuleEnvironment* primary = moduleEnvironment();
    JSModuleEnvironment* environment = JSModuleEnvironment::create(vm, globalObject, nullptr, primary->symbolTable(), jsTDZValue(), this);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!m_jsonSource.isNull()) {
        JSValue value = JSONParseWithException(globalObject, m_jsonSource.view());
        RETURN_IF_EXCEPTION(scope, nullptr);
        bool putResult = false;
        symbolTablePutTouchWatchpointSet(environment, globalObject, vm.propertyNames->defaultKeyword, value, false, true, putResult);
        RETURN_IF_EXCEPTION(scope, nullptr);
        return environment;
    }
    if (m_provider && m_provider->regeneratesPerGraphInstance()) {
        // The host produces this graph's values (the caller has set the graph as
        // the current loading instance); names not produced stay undefined.
        MarkedArgumentBuffer values;
        Vector<Identifier, 4> names;
        m_provider->generate(globalObject, moduleKey(), names, values);
        RETURN_IF_EXCEPTION(scope, nullptr);
        SymbolTable* symbolTable = primary->symbolTable();
        for (const auto& [key, entry] : exportEntries()) {
            SymbolTableEntry::Fast symbolEntry = symbolTable->get(entry.localName.impl());
            JSValue value = jsUndefined();
            for (unsigned i = 0; i < names.size(); ++i) {
                if (names[i] == entry.localName) {
                    value = values.at(i);
                    break;
                }
            }
            environment->variableAt(symbolEntry.scopeOffset()).set(vm, environment, value ? value : jsUndefined());
        }
        return environment;
    }
    // Deep-copy every export. `default` and named exports of a data module are
    // usually the same object graph (named = default's properties); clone
    // `default` once and re-derive the named exports from the copy when they
    // alias, so the aliasing survives. The primary's values may have been
    // mutated by script since they were judged plain data: re-check right here
    // (no script runs between this check and the copy) and hand this instance
    // the primary's values unchanged if they no longer qualify.
    SymbolTable* symbolTable = primary->symbolTable();
    {
        unsigned budget = 100000;
        bool stillPlainData = true;
        for (const auto& [key, entry] : exportEntries()) {
            SymbolTableEntry::Fast symbolEntry = symbolTable->get(entry.localName.impl());
            if (symbolEntry.isNull() || !isPlainData(globalObject, primary->variableAt(symbolEntry.scopeOffset()).get(), 0, budget)) {
                stillPlainData = false;
                break;
            }
        }
        if (!stillPlainData) {
            for (const auto& [key, entry] : exportEntries()) {
                SymbolTableEntry::Fast symbolEntry = symbolTable->get(entry.localName.impl());
                if (!symbolEntry.isNull())
                    environment->variableAt(symbolEntry.scopeOffset()).set(vm, environment, primary->variableAt(symbolEntry.scopeOffset()).get());
            }
            return environment;
        }
    }
    SymbolTableEntry::Fast defaultEntry = symbolTable->get(vm.propertyNames->defaultKeyword.impl());
    JSValue defaultOriginal = defaultEntry.isNull() ? JSValue() : primary->variableAt(defaultEntry.scopeOffset()).get();
    JSValue defaultCopy = defaultOriginal ? clonePlainData(globalObject, defaultOriginal) : JSValue();
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (const auto& [key, entry] : exportEntries()) {
        SymbolTableEntry::Fast symbolEntry = symbolTable->get(entry.localName.impl());
        JSValue original = primary->variableAt(symbolEntry.scopeOffset()).get();
        JSValue copy;
        if (entry.localName == vm.propertyNames->defaultKeyword)
            copy = defaultCopy;
        else if (defaultOriginal && defaultOriginal.isObject() && defaultCopy.isObject()) {
            JSValue aliased = defaultOriginal.getObject()->getDirect(vm, entry.localName);
            copy = aliased == original ? defaultCopy.getObject()->getDirect(vm, entry.localName) : clonePlainData(globalObject, original);
        } else
            copy = clonePlainData(globalObject, original);
        RETURN_IF_EXCEPTION(scope, nullptr);
        environment->variableAt(symbolEntry.scopeOffset()).set(vm, environment, copy);
    }
    return environment;
}

SyntheticModuleRecord* SyntheticModuleRecord::createTextModule(JSGlobalObject* globalObject, const Identifier& moduleKey, SourceCode&& sourceCode)
{
    // https://tc39.es/proposal-import-text/#sec-create-text-module
    VM& vm = globalObject->vm();
    return SyntheticModuleRecord::tryCreateDefaultExportSyntheticModule(globalObject, moduleKey, jsString(vm, sourceCode.view()), SourceProviderSourceType::Text);
}

} // namespace JSC
