/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTCompileOptions.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleLinkWalk.h"
#include "AOTModuleNamespace.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "DeferGCInlines.h"
#include "DisallowVMEntry.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSModuleLoader.h"
#include "ObjectConstructor.h"
#include <wtf/ASCIICType.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/URL.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Modules);

static bool isAbsoluteFileSystemPath(StringView path)
{
#if OS(WINDOWS)
    auto isSeparator = [](char16_t character) {
        return character == '/' || character == '\\';
    };
    if (path.isEmpty())
        return false;
    if (isSeparator(path[0]))
        return true;
    return path.length() >= 3 && isASCIIAlpha(path[0]) && path[1] == ':' && isSeparator(path[2]);
#else
    return path.startsWith('/');
#endif
}

static AbstractModuleRecord::ModulePhase targetPhaseOf(const ModuleExportData& moduleExport)
{
    return moduleExport.isDeferred ? AbstractModuleRecord::ModulePhase::Defer : AbstractModuleRecord::ModulePhase::Evaluation;
}

static StoredModuleStatus storedStatusFor(ModuleStatus status)
{
    switch (status) {
    case ModuleStatus::Unlinked:
        return StoredModuleStatus::Unlinked;
    case ModuleStatus::Linked:
        return StoredModuleStatus::Linked;
    case ModuleStatus::Evaluating:
        return StoredModuleStatus::Evaluating;
    case ModuleStatus::Evaluated:
        return StoredModuleStatus::Evaluated;
    case ModuleStatus::Linking:
    case ModuleStatus::EvaluatingAsync:
    case ModuleStatus::Failed:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static CyclicModuleRecord::Status recordStatusFor(ModuleStatus status)
{
    switch (status) {
    case ModuleStatus::Unlinked:
        return CyclicModuleRecord::Status::Unlinked;
    case ModuleStatus::Linking:
        return CyclicModuleRecord::Status::Linking;
    case ModuleStatus::Linked:
        return CyclicModuleRecord::Status::Linked;
    case ModuleStatus::Evaluating:
        return CyclicModuleRecord::Status::Evaluating;
    case ModuleStatus::EvaluatingAsync:
        return CyclicModuleRecord::Status::EvaluatingAsync;
    case ModuleStatus::Evaluated:
        return CyclicModuleRecord::Status::Evaluated;
    case ModuleStatus::Failed:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ModuleStatus statusOf(const ModuleRecord& record)
{
    switch (record.status()) {
    case CyclicModuleRecord::Status::New:
    case CyclicModuleRecord::Status::Unlinked:
        return ModuleStatus::Unlinked;
    case CyclicModuleRecord::Status::Linking:
        return ModuleStatus::Linking;
    case CyclicModuleRecord::Status::Linked:
        return ModuleStatus::Linked;
    case CyclicModuleRecord::Status::Evaluating:
        return ModuleStatus::Evaluating;
    case CyclicModuleRecord::Status::EvaluatingAsync:
        return ModuleStatus::EvaluatingAsync;
    case CyclicModuleRecord::Status::Evaluated:
        return record.evaluationError() ? ModuleStatus::Failed : ModuleStatus::Evaluated;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static CyclicModuleRecord::Status initialRecordStatus(StoredModuleStatus status, const ModuleData& moduleData)
{
    switch (status) {
    case StoredModuleStatus::Unlinked:
        if (!moduleData.needsLink())
            return CyclicModuleRecord::Status::Linked;
        return moduleData.reachesUncompiledCode() ? CyclicModuleRecord::Status::New : CyclicModuleRecord::Status::Unlinked;
    case StoredModuleStatus::Linked:
        return CyclicModuleRecord::Status::Linked;
    case StoredModuleStatus::Evaluating:
        return CyclicModuleRecord::Status::Evaluating;
    case StoredModuleStatus::Evaluated:
        return CyclicModuleRecord::Status::Evaluated;
    case StoredModuleStatus::InRecord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Visitor>
void Modules::visitRoots(Visitor& visitor)
{
    for (ModuleRecord* record : m_records.values())
        visitor.appendUnbarriered(record);
    for (auto& loaded : m_loaded.values()) {
        visitor.appendUnbarriered(loaded->namespaceObject);
        visitor.appendUnbarriered(loaded->deferredNamespaceObject);
        visitor.appendUnbarriered(loaded->importMeta);
        visitor.appendUnbarriered(loaded->requirer);
        visitor.appendUnbarriered(loaded->requireFunction);
        visitor.appendUnbarriered(loaded->initialExports);
        visitor.appendUnbarriered(loaded->capturedExports);
        visitor.appendUnbarriered(loaded->uncompiledRecord);
    }
    visitor.appendUnbarriered(m_namespaceStructure);
}

template void Modules::visitRoots(SlotVisitor&);
template void Modules::visitRoots(AbstractSlotVisitor&);

Modules::Modules(State& state)
    : m_state(state)
    , m_instance(state.instance)
    , m_table(state.tables)
    , m_admitsCodeOutsideImage(hasFlag(state.instance.options().admitted(), AdmittedCode::RuntimeCode))
    , m_loadsModulesByName(state.instance.vm->aotState().image->loadsModulesByName())
    , m_replacesLoaderAnswers(state.instance.vm->aotState().image->replacesLoaderAnswers())
{
    m_instance.modules.status = state.tableArena.allocate<uint8_t>(m_table.size());
    m_storedStatuses = unsafeMakeSpan(m_instance.modules.status, m_table.size());
    m_instance.modules.requireEdges = state.tableArena.allocate<uint8_t>(m_table.numberOfRequireEdges());
    state.caches->add(CacheKind::StrongRoots, "modules"_s, *this, RootVolatility::GreyedByExecution);
}

Modules::~Modules() = default;

Modules& modulesOf(Instance& instance)
{
    return *instance.state->modules;
}

Modules* modulesOf(JSGlobalObject* realm)
{
    Instance* instance = Instance::of(realm);
    return instance ? instance->state->modules.get() : nullptr;
}

const ModuleHost& Modules::host() const
{
    return moduleHostOf(m_instance.globalObject);
}

const Identifier& Modules::identifier(uint32_t number) const
{
    return m_instance.vm->aotState().identifiers->identifier(number);
}

JSModuleLoader* Modules::loader() const
{
    return m_instance.globalObject->moduleLoader();
}

ModuleStatus Modules::statusInRecord(ModuleID module) const
{
    ModuleRecord* record = recordIfExists(module);
    RELEASE_ASSERT(record);
    return statusOf(*record);
}

void Modules::setStatus(ModuleID module, ModuleStatus status)
{
    ModuleRecord* record = recordIfExists(module);
    if (!record) {
        setStoredStatus(module, storedStatusFor(status));
        return;
    }
    record->setStatus(recordStatusFor(status));
    if (status == ModuleStatus::Evaluated && !record->cycleRoot())
        record->setCycleRoot(*m_instance.vm, record);
}

ModuleRecord* Modules::ensureRecord(ModuleID module)
{
    if (ModuleRecord* record = recordIfExists(module))
        return record;

    VM& vm = *m_instance.vm;
    const ModuleData& moduleData = data(module);
    ModuleRecord* record = ModuleRecord::create(m_instance, vm.aotState().moduleRecordStructure.get(), loader(), moduleKey(module), module);
    record->setHasTLA(moduleData.hasTopLevelAwait());
    record->setStatus(initialRecordStatus(storedStatus(module), moduleData));
    if (record->status() == CyclicModuleRecord::Status::Evaluated)
        record->setCycleRoot(vm, record);
    {
        Locker locker { m_state.caches->lock() };
        m_records.add(module, record);
    }
    setStoredStatus(module, StoredModuleStatus::InRecord);
    return record;
}

void Modules::retireRecord(ModuleRecord* record)
{
    ASSERT(!record->isRetained());
    ASSERT(record->status() == CyclicModuleRecord::Status::Evaluated);
    ASSERT(!record->evaluationError());
    ASSERT(record->asyncEvaluationOrder().isUnset());
    ModuleID module = record->module();
    setStoredStatus(module, StoredModuleStatus::Evaluated);
    Locker locker { m_state.caches->lock() };
    m_records.remove(module);
}

void Modules::setFailed(ModuleID module, JSValue error)
{
    ModuleRecord* record = ensureRecord(module);
    record->retain();
    record->setEvaluationError(*m_instance.vm, error);
    record->setStatus(CyclicModuleRecord::Status::Evaluated);
}

void Modules::throwEvaluationError(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleRecord* record = recordIfExists(module);
    RELEASE_ASSERT(record && record->evaluationError());
    if (JSPromise* capability = record->topLevelCapability())
        capability->markAsHandled();
    throwException(m_instance.globalObject, scope, record->evaluationError());
}

JSValue Modules::value(ModuleID module) const
{
    return ProgramStatics::readVariableWithoutAllocating(m_instance, data(module).value);
}

void Modules::setValue(ModuleID module, JSValue value)
{
    ProgramStatics::writeVariable(m_instance, data(module).value, value);
}

auto Modules::loadedIfExists(ModuleID module) const -> LoadedModule*
{
    return m_loaded.get(module);
}

auto Modules::ensureLoaded(ModuleID module) -> LoadedModule&
{
    if (LoadedModule* loaded = loadedIfExists(module))
        return *loaded;
    RELEASE_ASSERT(module.index() < m_table.size());
    auto created = makeUnique<LoadedModule>();
    LoadedModule& loaded = *created;
    Locker locker { m_state.caches->lock() };
    m_loaded.add(module, WTF::move(created));
    return loaded;
}

Identifier Modules::moduleKey(ModuleID module)
{
    return Identifier::fromString(*m_instance.vm, m_table.pathText(module));
}

JSString* Modules::pathString(ModuleID module)
{
    return m_state.strings->string(*m_instance.vm, StringID(data(module).publicPath));
}

void Modules::throwExportNotFound(ModuleID exporter, uint32_t nameIdentifier)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* globalObject = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& name = identifier(nameIdentifier);
    if (name == vm.propertyNames->defaultKeyword) {
        throwSyntaxError(globalObject, scope, makeString("Missing 'default' export in module '"_s, m_table.pathText(exporter), "'."_s));
        return;
    }
    throwSyntaxError(globalObject, scope, makeString("Export named '"_s, name.string(), "' not found in module '"_s, m_table.pathText(exporter), "'."_s));
}

void Modules::throwExportAmbiguous(ModuleID exporter, uint32_t nameIdentifier)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    throwSyntaxError(m_instance.globalObject, scope, makeString("Export named '"_s, identifier(nameIdentifier).string(), "' cannot be resolved due to ambiguous multiple bindings in module '"_s, m_table.pathText(exporter), "'."_s));
}

JSPromise* Modules::rejectedPromiseWithPendingException(JSGlobalObject* globalObject, ThrowScope& scope)
{
    if (JSPromise* promise = JSPromise::rejectedPromiseWithCaughtException(globalObject, scope))
        return promise;
    // A termination cannot be caught: it stays pending, and the promise is never settled.
    return JSPromise::create(globalObject->vm(), globalObject->promiseStructure());
}

JSValue Modules::readExportIfInitialized(ModuleID module, unsigned index) const
{
    std::span<const ModuleExportData> exports = m_table.exports(module);
    RELEASE_ASSERT(index < exports.size());
    const ModuleExportData& moduleExport = exports[index];
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable:
        return ProgramStatics::readVariableWithoutAllocating(m_instance, moduleExport.variable);
    case ModuleExportKind::Namespace: {
        LoadedModule* target = loadedIfExists(moduleExport.targetModule());
        JSObject* namespaceObject = target ? target->namespaceObjectFor(targetPhaseOf(moduleExport)) : nullptr;
        return namespaceObject ? JSValue(namespaceObject) : JSValue();
    }
    case ModuleExportKind::Indirect:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue Modules::readExport(ModuleID module, unsigned index)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* globalObject = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const ModuleExportData> exports = m_table.exports(module);
    RELEASE_ASSERT(index < exports.size());
    const ModuleExportData& moduleExport = exports[index];
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable: {
        JSValue value = ProgramStatics::readVariable(m_instance, moduleExport.variable);
        if (!value) [[unlikely]] {
            DeferGCForAWhile deferGC(vm);
            const Identifier& name = identifier(moduleExport.name);
            throwException(globalObject, scope, createTDZError(globalObject, StringView(name.string())));
        }
        return value;
    }
    case ModuleExportKind::Namespace:
        RELEASE_AND_RETURN(scope, moduleNamespace(moduleExport.targetModule(), targetPhaseOf(moduleExport)));
    case ModuleExportKind::Indirect: {
        const IndirectExportData& indirectExport = m_table.indirectExport(moduleExport);
        JSObject* namespaceObject = moduleNamespace(indirectExport.targetModule(), ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, namespaceObject->get(globalObject, identifier(indirectExport.importName)));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSObject* Modules::capturedExports(ModuleID module) const
{
    LoadedModule* loaded = loadedIfExists(module);
    RELEASE_ASSERT(loaded && loaded->capturedExports);
    return loaded->capturedExports;
}

bool Modules::isExportAmbiguous(ModuleID module, unsigned index) const
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->ambiguousExports.get(index);
}

Structure* Modules::namespaceStructure()
{
    if (!m_namespaceStructure) {
        JSGlobalObject* globalObject = m_instance.globalObject;
        m_namespaceStructure = ModuleNamespace::createStructure(*m_instance.vm, globalObject, globalObject->moduleNamespaceObjectStructure()->storedPrototype());
    }
    return m_namespaceStructure;
}

JSObject* Modules::createNamespace(ModuleID module, ModulePhase phase)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* globalObject = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);
    LoadedModule& loaded = ensureLoaded(module);
    JSObject*& namespaceObject = loaded.namespaceObjectFor(phase);

    if (moduleData.isUncompiledCode()) {
        JSObject* loaderNamespace = uncompiledNamespace(module, phase);
        RETURN_IF_EXCEPTION(scope, nullptr);
        namespaceObject = loaderNamespace;
        return loaderNamespace;
    }

    if (status(module) == ModuleStatus::Unlinked || (moduleData.capturesExports() && !loaded.capturedExports)) {
        link(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (namespaceObject)
            return namespaceObject;
    }

    Vector<DynamicExport> dynamicExports = collectDynamicExports(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ModuleNamespace::DynamicExports exportObjects;
    if (!dynamicExports.isEmpty()) {
        Structure* nullPrototypeStructure = globalObject->nullPrototypeObjectStructure();
        exportObjects.capturedValues = constructEmptyObject(vm, nullPrototypeStructure);
        exportObjects.names = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, dynamicExports.size());
        for (unsigned i = 0; i < dynamicExports.size(); ++i) {
            const DynamicExport& dynamicExport = dynamicExports[i];
            if (dynamicExport.liveSource) {
                if (!exportObjects.liveSources)
                    exportObjects.liveSources = constructEmptyObject(vm, nullPrototypeStructure);
                exportObjects.liveSources->putDirectMayBeIndex(globalObject, dynamicExport.name, dynamicExport.liveSource);
            } else
                exportObjects.capturedValues->putDirectMayBeIndex(globalObject, dynamicExport.name, dynamicExport.capturedValue);
            RETURN_IF_EXCEPTION(scope, nullptr);
            JSString* text = jsString(vm, dynamicExport.name.string());
            text->toIdentifier(globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
            exportObjects.names->setIndex(vm, i, text);
        }
    }

    namespaceObject = ModuleNamespace::create(vm, namespaceStructure(), module, phase, exportObjects);
    return namespaceObject;
}

JSObject* Modules::moduleNamespace(ModuleID module, ModulePhase phase)
{
    if (LoadedModule* loaded = loadedIfExists(module)) {
        if (JSObject* namespaceObject = loaded->namespaceObjectFor(phase))
            return namespaceObject;
    }
    return createNamespace(module, phase);
}

JSObject* Modules::importMeta(ModuleID module)
{
    JSGlobalObject* globalObject = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.importMeta)
        return loaded.importMeta;
    JSObject* meta = loader()->createImportMetaProperties(globalObject, pathString(module), nullptr, nullptr);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_ASSERT(meta);
    loaded.importMeta = meta;
    return meta;
}

JSCell* Modules::builtinCell(BuiltinID builtin, ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const Builtin& entry = BuiltinTable::singleton().at(builtin);
    RELEASE_ASSERT(entry.scope == BuiltinScope::Module);

    if (!entry.holder) {
        if (builtin == Builtins::ModuleObject)
            RELEASE_AND_RETURN(scope, moduleObject(module));
        if (builtin == Builtins::ImportMeta)
            RELEASE_AND_RETURN(scope, importMeta(module));
        RELEASE_ASSERT(builtin == Builtins::ModuleRequire);
        JSValue function = requireFunction(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        RELEASE_ASSERT(function.isCell());
        return function.asCell();
    }

    // The variable `exports` keeps the object that the body started with, whatever the program stores to module.exports.
    if (builtin == Builtins::ModuleObjectExports) {
        LoadedModule* loaded = loadedIfExists(module);
        if (JSValue initialExports = loaded ? loaded->initialExports : JSValue())
            return initialExports.isCell() ? initialExports.asCell() : nullptr;
    }

    JSCell* holder = builtinCell(entry.holder, module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!holder || !holder->isObject())
        return nullptr;
    RELEASE_AND_RETURN(scope, m_state.builtinCells->readMember(m_instance.globalObject, asObject(holder), builtin));
}

JSString* Modules::resolve(ModuleID module, ResolvedSpecifierForm form)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* globalObject = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);
    bool isURL = form == ResolvedSpecifierForm::URL;
    switch (moduleData.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
        return isURL ? jsString(vm, URL::fileURLWithFileSystemPath(m_table.pathText(module)).string()) : pathString(module);
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
        return pathString(module);
    case ModuleKind::External: {
        JSString* specifier = pathString(module);
        Identifier key;
        {
            DisallowVMEntry disallowVMEntry(vm);
            key = loader()->resolve(globalObject, specifier, pathString(*moduleData.referrerModule()), nullptr, true);
        }
        if (scope.exception()) [[unlikely]] {
            TRY_CLEAR_EXCEPTION(scope, specifier);
            return specifier;
        }
        if (isURL && isAbsoluteFileSystemPath(key.string()))
            return jsString(vm, URL::fileURLWithFileSystemPath(key.string()).string());
        return jsString(vm, key.string());
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const SourceOrigin& Modules::sourceOrigin(ModuleID module)
{
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.sourceOrigin)
        return *loaded.sourceOrigin;
    switch (data(module).kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
        loaded.sourceOrigin = makeUniqueWithoutFastMallocCheck<SourceOrigin>(URL::fileURLWithFileSystemPath(m_table.pathText(module)));
        break;
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        loaded.sourceOrigin = makeUniqueWithoutFastMallocCheck<SourceOrigin>(URL(m_table.pathText(module)));
        break;
    }
    return *loaded.sourceOrigin;
}

void Modules::throwFailure(ModuleFailureID id)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* globalObject = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleFailureData& failure = m_table.failure(id);
    StringID specifier = failure.specifierString();
    ModuleID importer = failure.importerModule();
    {
        DisallowVMEntry disallowVMEntry(vm);
        loader()->resolve(globalObject, m_state.strings->string(vm, specifier), pathString(importer), nullptr, true);
    }
    RETURN_IF_EXCEPTION(scope, void());
    throwException(globalObject, scope, createError(globalObject, makeString("Cannot find module '"_s, ProgramStrings::text(m_table.programTables(), specifier), "' from '"_s, m_table.pathText(importer), "'"_s)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
