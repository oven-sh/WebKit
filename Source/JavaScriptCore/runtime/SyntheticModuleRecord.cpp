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
#include "JSModuleEnvironment.h"
#include "JSModuleNamespaceObject.h"
#include "JSONObject.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

#if USE(BUN_JSC_ADDITIONS)
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(SyntheticModuleRecord::Deferred);
#endif

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
    visitor.append(thisObject->m_deferredGeneratorError);
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
    moduleRecord->m_deferred = makeUnique<Deferred>(WTF::move(generator));
    moduleRecord->m_hasPendingExports = true;
    // A synthetic record has no code, so nothing else uses the field for the frame of a module's body.
    if (JSCell* owner = moduleRecord->m_deferred->generator->deferredOwner())
        moduleRecord->internalField(Field::Frame).set(vm, moduleRecord, owner);
    return moduleRecord;
}

unsigned SyntheticModuleRecord::reserveExportOffset(UniquedStringImpl* exportName)
{
    ASSERT(hasPendingExports());
    // The generator failed: the record never gets an environment, and nothing reads the offset.
    if (!m_deferred)
        return firstExportOffset;
    auto& deferred = *m_deferred;
    if (auto iterator = deferred.reservedOffsetByName.find(exportName); iterator != deferred.reservedOffsetByName.end())
        return iterator->value;
    while (deferred.reservedOffsets.contains(deferred.nextOffset))
        ++deferred.nextOffset;
    unsigned offset = deferred.nextOffset++;
    deferred.reservedOffsets.add(offset);
    deferred.reservedOffsetByName.add(exportName, offset);
    return offset;
}

bool SyntheticModuleRecord::reserveExportOffset(UniquedStringImpl* exportName, unsigned scopeOffset)
{
    ASSERT(hasPendingExports());
    if (!m_deferred)
        return true;
    if (scopeOffset < firstExportOffset)
        return false;
    auto& deferred = *m_deferred;
    if (auto iterator = deferred.reservedOffsetByName.find(exportName); iterator != deferred.reservedOffsetByName.end())
        return iterator->value == scopeOffset;
    if (!deferred.reservedOffsets.add(scopeOffset).isNewEntry)
        return false;
    deferred.reservedOffsetByName.add(exportName, scopeOffset);
    return true;
}

void SyntheticModuleRecord::runDeferredGenerator(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (JSValue error = m_deferredGeneratorError.get()) {
        throwException(globalObject, scope, error);
        return;
    }
    if (!m_deferred)
        return;

    // The generator runs once. A failure here or in initializeExports() is what every later evaluation reports: the
    // record stays without exports, so a module that links against it later still gets as far as evaluating it.
    auto failed = [&](Exception* exception) {
        if (vm.isTerminationException(exception))
            return;
        m_deferredGeneratorError.set(vm, this, exception->value());
        m_deferred = nullptr;
        internalField(Field::Frame).set(vm, this, jsUndefined());
    };

    Ref generator = m_deferred->generator;
    MarkedArgumentBuffer exportValues;
    Vector<Identifier, 4> exportNames;
    JSObject* lazyExportsSource = generator->generate(globalObject, moduleKey(), exportNames, exportValues);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        failed(exception);
        return;
    }

    // The generator can run code that loads a graph containing this record (a CommonJS module that require()s an ES
    // module importing it back). That load evaluated this record: it ran the generator again, got the exports as they
    // were at that point, and its modules bound to them.
    if (!m_deferred)
        return;

    initializeExports(globalObject, exportNames, exportValues, lazyExportsSource);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        failed(exception);
        return;
    }
    m_deferred = nullptr;
    m_hasPendingExports = false;
    internalField(Field::Frame).set(vm, this, jsUndefined());
}
#endif

JSValue SyntheticModuleRecord::evaluate(JSGlobalObject* globalObject)
{
#if USE(BUN_JSC_ADDITIONS)
    if (m_hasPendingExports) [[unlikely]] {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        runDeferredGenerator(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
#else
    UNUSED_PARAM(globalObject);
#endif
    return jsUndefined();
}

#if USE(BUN_JSC_ADDITIONS)
SyntheticModuleRecord* SyntheticModuleRecord::tryCreateWithExportNamesAndValues(JSGlobalObject* globalObject, JSModuleLoader* moduleLoader, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType sourceType)
{
    return tryCreateWithExportNamesAndValues(globalObject, moduleLoader, moduleKey, exportNames, exportValues, sourceType, nullptr);
}

SyntheticModuleRecord* SyntheticModuleRecord::tryCreateWithExportNamesAndValues(JSGlobalObject* globalObject, JSModuleLoader* moduleLoader, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, JSObject* lazyExportsSource)
{
    return tryCreateWithExportNamesAndValues(globalObject, moduleLoader, moduleKey, exportNames, exportValues, SourceProviderSourceType::Module, lazyExportsSource);
}

SyntheticModuleRecord* SyntheticModuleRecord::tryCreateWithExportNamesAndValues(JSGlobalObject* globalObject, JSModuleLoader* moduleLoader, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType sourceType, JSObject* lazyExportsSource)
#else
SyntheticModuleRecord* SyntheticModuleRecord::tryCreateWithExportNamesAndValues(JSGlobalObject* globalObject, JSModuleLoader* moduleLoader, const Identifier& moduleKey, const Vector<Identifier, 4>& exportNames, ArgList exportValues, SourceProviderSourceType sourceType)
#endif
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* moduleRecord = create(globalObject, vm, globalObject->syntheticModuleRecordStructure(), moduleLoader, moduleKey, sourceType);
#if USE(BUN_JSC_ADDITIONS)
    moduleRecord->initializeExports(globalObject, exportNames, exportValues, lazyExportsSource);
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
#if USE(BUN_JSC_ADDITIONS)
    if (m_deferred && !m_deferred->reservedOffsetByName.isEmpty()) [[unlikely]] {
        // Code that was linked before now embeds these offsets. A reserved name that is not exported keeps its slot, empty.
        auto& deferred = *m_deferred;
        unsigned nextOffset = firstExportOffset;
        for (auto& exportName : exportNames) {
            unsigned offset;
            if (auto iterator = deferred.reservedOffsetByName.find(exportName.impl()); iterator != deferred.reservedOffsetByName.end())
                offset = iterator->value;
            else {
                while (deferred.reservedOffsets.contains(nextOffset))
                    ++nextOffset;
                offset = nextOffset++;
            }
            exportSymbolTable->add(NoLockingNecessary, exportName.impl(), SymbolTableEntry(VarOffset(ScopeOffset(offset))));
            moduleRecord->addExportEntry(ExportEntry::createLocal(exportName, exportName));
        }
        for (unsigned offset : deferred.reservedOffsets)
            exportSymbolTable->didUseScopeOffset(ScopeOffset(offset));
    } else
#endif
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
