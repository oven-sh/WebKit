/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTErrorMessages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleLinkWalk.h"
#include "AOTModuleMicrotasks.h"
#include "AOTModuleNamespace.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "AOTThreshold.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "DisallowVMEntry.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "JSModuleLoader.h"
#include "MicrotaskQueueInlines.h"
#include "ModuleGraphLoadingState.h"
#include "ModuleRegistryEntry.h"
#include "ObjectConstructor.h"
#include "ScriptFetchParameters.h"
#include "SlotVisitorInlines.h"
#include "SourceProvider.h"
#include "TopExceptionScope.h"
#include "TypeError.h"
#include <wtf/ASCIICType.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/StackPointer.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/URL.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Modules);

static bool isAbsoluteFileSystemPath(StringView path)
{
#if OS(WINDOWS)
    auto isSeparator = [&](unsigned index) {
        return path[index] == '/' || path[index] == '\\';
    };
    return !path.isEmpty() && (isSeparator(0) || (path.length() >= 3 && isASCIIAlpha(path[0]) && path[1] == ':' && isSeparator(2)));
#else
    return path.startsWith('/');
#endif
}

static AbstractModuleRecord::ModulePhase targetPhaseOf(const ModuleExportData& moduleExport)
{
    return moduleExport.isDeferred ? AbstractModuleRecord::ModulePhase::Defer : AbstractModuleRecord::ModulePhase::Evaluation;
}

static CyclicModuleRecord::Status recordStatusFor(ModuleStatus status)
{
    RELEASE_ASSERT(status != ModuleStatus::Failed);
    return static_cast<CyclicModuleRecord::Status>(status);
}

static ModuleStatus statusOf(const ModuleRecord& record)
{
    if (record.status() == CyclicModuleRecord::Status::New)
        return ModuleStatus::Unlinked;
    if (record.status() == CyclicModuleRecord::Status::Evaluated && record.evaluationError())
        return ModuleStatus::Failed;
    return static_cast<ModuleStatus>(record.status());
}

template<typename Visitor>
void Modules::visitRoots(Visitor& visitor)
{
    m_storedReferences.forEachRegionToVisit(visitor, [&](size_t, size_t) {
        for (ModuleRecord* record : m_records.values())
            visitor.appendUnbarriered(record);
        for (auto& loaded : m_loaded.values()) {
#define AOT_VISIT_REFERENCE(type, name) visitor.appendUnbarriered(loaded->name.get());
            FOR_EACH_REFERENCE_OF_LOADED_MODULE(AOT_VISIT_REFERENCE)
#undef AOT_VISIT_REFERENCE
        }
        visitor.appendUnbarriered(m_namespaceStructure);
    });
}

template void Modules::visitRoots(SlotVisitor&);
template void Modules::visitRoots(AbstractSlotVisitor&);

Modules::Modules(State& state)
    : m_state(state)
    , m_instance(state.instance)
    , m_table(state.tables)
    , m_admitsCodeOutsideImage(hasFlag(state.instance.options().admitted(), AdmittedCode::Inspector) || state.instance.vm->aotState().image->loadsCodeOutsideImage())
    , m_admitsInspector(hasFlag(state.instance.options().admitted(), AdmittedCode::Inspector))
    , m_showsWaysBetweenModuleObjects(state.instance.vm->aotState().image->showsWaysBetweenModuleObjects())
    , m_loadsModulesByName(state.instance.vm->aotState().image->loadsModulesByName())
    , m_findsDataFilesByName(state.instance.vm->aotState().image->findsDataFilesByName())
    , m_buildHasSeenWhoNamesModules(!m_admitsInspector)
{
    m_instance.modules.status = state.tableArena.allocate<uint8_t>(m_table.size());
    m_storedStatuses = unsafeMakeSpan(m_instance.modules.status, m_table.size());
    m_instance.modules.evaluatingMark = static_cast<uint32_t>(StoredModuleStatus::Evaluating);
    m_modulesSettledForLinks.fill(0, m_table.size() / 64 + 1);
    m_loadedGroups.fill(0, m_table.size() / 64 + 1);
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

Identifier Modules::identifier(uint32_t number) const
{
    return m_instance.vm->aotState().identifiers->identifier(number);
}

JSModuleLoader* Modules::loader() const
{
    return m_instance.globalObject->moduleLoader();
}

ModuleStatus Modules::statusInRecord(ModuleID module) const
{
    ModuleRecord* record = m_records.get(module);
    RELEASE_ASSERT(record);
    ModuleStatus status = statusOf(*record);
    return status == ModuleStatus::Unlinked && !data(module).needsLink() ? ModuleStatus::Linked : status;
}

void Modules::setStatus(ModuleID module, ModuleStatus status)
{
    if (ModuleRecord* record = recordIfExists(module)) {
        record->setStatus(recordStatusFor(status));
        if (status == ModuleStatus::Evaluated && !record->cycleRoot())
            record->setCycleRoot(*m_instance.vm, record);
        return;
    }
    switch (status) {
    case ModuleStatus::Unlinked:
        return setStoredStatus(module, StoredModuleStatus::Unlinked);
    case ModuleStatus::Linked:
        return setStoredStatus(module, StoredModuleStatus::Linked);
    case ModuleStatus::Evaluating:
        return setStoredStatus(module, StoredModuleStatus::Evaluating);
    case ModuleStatus::Evaluated:
        return setStoredStatus(module, StoredModuleStatus::Evaluated);
    case ModuleStatus::Linking:
    case ModuleStatus::EvaluatingAsync:
    case ModuleStatus::Failed:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ModuleRecord* Modules::ensureRecordWithoutStatus(ModuleID module)
{
    if (ModuleRecord* record = m_records.get(module))
        return record;

    ModuleRecord* record = ModuleRecord::create(m_instance, m_instance.vm->aotState().moduleRecordStructure.get(), loader(), moduleKey(module), module);
    record->setHasTLA(data(module).hasTopLevelAwait());
    Locker locker { m_state.caches->lock() };
    m_records.add(module, record);
    m_storedReferences.recordAll();
    return record;
}

ModuleRecord* Modules::ensureRecord(ModuleID module)
{
    ModuleRecord* record = ensureRecordWithoutStatus(module);
    // While the body of a module with a record runs, the table has the status.
    if (record->holdsStatus())
        return record;

    ModuleStatus moduleStatus = status(module);
    // Compiled code began the module, and stores the status in the table when the body ends. The record takes the status when it is asked for again.
    if (moduleStatus == ModuleStatus::Evaluating && (data(module).evaluatesItself() || !data(module).bodyFunction())) {
        record->setStatus(CyclicModuleRecord::Status::Evaluating);
        return record;
    }
    record->setHoldsStatus();
    record->setStatus((moduleStatus == ModuleStatus::Unlinked && data(module).reachesUncompiledCode()) || isNotLoaded(module) ? CyclicModuleRecord::Status::New : recordStatusFor(moduleStatus));
    if (moduleStatus == ModuleStatus::Evaluated)
        record->setCycleRoot(*m_instance.vm, record);
    setStoredStatus(module, StoredModuleStatus::InRecord);
    return record;
}

void Modules::didLinkEntryPoint()
{
    m_entryPointIsLinked = true;
    ++m_linkEpoch;
    for (ModuleRecord* record : m_records.values()) {
        if (data(record->module()).isLinkedWithEntryPoint() && statusOf(*record) == ModuleStatus::Unlinked)
            record->setStatus(CyclicModuleRecord::Status::Linked);
    }
}

void Modules::retireRecord(ModuleRecord* record)
{
    ASSERT(!record->isRetained());
    ASSERT(record->status() == CyclicModuleRecord::Status::Evaluated);
    ASSERT(!record->evaluationError());
    ASSERT(record->asyncEvaluationOrder().isUnset());
    setStoredStatus(record->module(), StoredModuleStatus::Evaluated);
    loader()->removeEntryOfRecord(record);
    Locker locker { m_state.caches->lock() };
    m_records.remove(record->module());
}

// An import() of a module of a cycle settles as the evaluation of the root of the cycle does.
bool Modules::hasRunToItsEndWithItsCycle(const ModuleRecord& record) const
{
    auto hasRunToItsEnd = [](const CyclicModuleRecord& module) {
        return module.status() == CyclicModuleRecord::Status::Evaluated && !module.evaluationError() && module.asyncEvaluationOrder().isUnset();
    };
    const CyclicModuleRecord* root = record.cycleRoot();
    return hasRunToItsEnd(record) && (!root || hasRunToItsEnd(*root));
}

// An ES module of the program that has run needs no record, like one that ran with the graph of another: whoever asks for it again is handed a record that says so.
void Modules::retireRecordsOfSettledLoads()
{
    if (m_instance.vm->m_synchronousModuleQueue || m_innermostEvaluation || m_rootWhoseFilesRun || m_rootWhoseFilesRanAhead || m_fileWhoseJobsWaitForLoader || !m_levelsOfLoads.isEmpty())
        return;
    // The host loads the graph of the entry point.
    if (!m_firstModuleRequestedByHost || status(*m_firstModuleRequestedByHost) < ModuleStatus::Evaluating)
        return;
    if (m_loadsInFlight.containsIf([](const LoadInFlight& load) { return load.promise->status() == JSPromise::Status::Pending; }))
        return;
    m_loadsInFlight.clear();
    for (ModuleID module : std::exchange(m_modulesWhoseRecordsLoaderHolds, { })) {
        ModuleRecord* record = m_records.get(module);
        if (!record)
            continue;
        record->setIsHeldByLoader(false);
        if (record->isRetained() || !isLoadedWithRequests(module))
            continue;
        // The table has the status of a module that compiled code began.
        record->takeStatus();
        if (!record->holdsStatus())
            continue;
        if (status(module) == ModuleStatus::Linked)
            recordEvaluationIfNothingIsLeftToRun(module);
        if (hasRunToItsEndWithItsCycle(*record))
            retireRecord(record);
    }
}

void didSettleLoad(JSPromise* settled)
{
    if (Modules* modules = modulesOf(settled->realm()))
        modules->retireRecordsOfSettledLoads();
}

void Modules::setFailed(ModuleID module, JSValue error)
{
    ModuleRecord* record = ensureRecord(module);
    // While a body of the module runs, the table has the status.
    setStoredStatus(module, StoredModuleStatus::InRecord);
    record->setHoldsStatus();
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
    return data(module).hasValue() ? ProgramStatics::readVariableWithoutAllocating(m_instance, data(module).value) : JSValue();
}

void Modules::setValue(ModuleID module, JSValue value)
{
    RELEASE_ASSERT(data(module).hasValue());
    ProgramStatics::writeVariable(m_instance, data(module).value, value);
}

auto Modules::ensureLoaded(ModuleID module) -> LoadedModule&
{
    if (LoadedModule* loaded = loadedIfExists(module))
        return *loaded;
    RELEASE_ASSERT(module.index() < m_table.size());
    Locker locker { m_state.caches->lock() };
    return *m_loaded.add(module, makeUnique<LoadedModule>()).iterator->value;
}

Identifier Modules::moduleKey(ModuleID module)
{
    // Every importer of a module that the loader goes through asks for the key.
    if (ModuleRecord* record = m_records.get(module))
        return record->moduleKey();
    return Identifier::fromString(*m_instance.vm, m_table.pathText(module));
}

// The loader hands the key to the host, which asks for the module under it. Every importer of the module asks for the key, and the record that the host will ask for keeps it.
Identifier Modules::keyOfRequest(ModuleID target)
{
    Identifier key = ensureRecordWithoutStatus(target)->moduleKey();
    m_table.willBeAskedForPath(target, key.impl()->hash());
    return key;
}

JSString* Modules::pathString(ModuleID module)
{
    if (data(module).directory == Number24::max)
        return m_state.strings->string(*m_instance.vm, StringID(data(module).publicPathOrName));
    return jsString(*m_instance.vm, m_table.pathText(module));
}

void Modules::throwExportNotFound(const RequiredNameData& requiredName)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (const ExportStatementData* statement = m_table.findExportStatement(requiredName)) {
        throwException(realm, scope, createExportStatementFindsNothingError(realm, ProgramStrings::text(m_table.programTables(), statement->exportNameString()), ProgramStrings::text(m_table.programTables(), statement->specifierString())));
        return;
    }
    Identifier name = identifier(requiredName.name);
    String path = m_table.pathText(requiredName.exporterModule());
    bool isDefault = name == m_instance.vm->propertyNames->defaultKeyword;
    throwException(realm, scope, isDefault ? createMissingDefaultExportError(realm, path) : createExportNotFoundError(realm, name.string(), path));
}

void Modules::throwExportAmbiguous(const RequiredNameData& requiredName)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (const ExportStatementData* statement = m_table.findExportStatement(requiredName)) {
        throwException(realm, scope, createExportStatementFindsSeveralError(realm, ProgramStrings::text(m_table.programTables(), statement->exportNameString()), ProgramStrings::text(m_table.programTables(), statement->specifierString())));
        return;
    }
    throwException(realm, scope, createAmbiguousExportError(realm, identifier(requiredName.name).string(), m_table.pathText(requiredName.exporterModule())));
}

JSPromise* Modules::rejectedPromiseWithPendingException(JSGlobalObject* realm, ThrowScope& scope)
{
    if (JSPromise* promise = JSPromise::rejectedPromiseWithCaughtException(realm, scope))
        return promise;
    // A termination cannot be caught: it stays pending, and the promise is never settled.
    return JSPromise::create(realm->vm(), realm->promiseStructure());
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
        JSObject* namespaceObject = namespaceIfCreated(moduleExport.targetModule(), targetPhaseOf(moduleExport));
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
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const ModuleExportData> exports = m_table.exports(module);
    RELEASE_ASSERT(index < exports.size());
    const ModuleExportData& moduleExport = exports[index];
    // The caches answer for the namespace object of an evaluated module only.
    if (status(module) == ModuleStatus::Linked) [[unlikely]]
        recordEvaluationIfNothingIsLeftToRun(module);
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable: {
        JSValue value = ProgramStatics::readVariable(m_instance, moduleExport.variable);
        if (!value) [[unlikely]] {
            DeferGCForAWhile deferGC(vm);
            throwException(realm, scope, createTDZError(realm, StringView(identifier(moduleExport.name).string())));
        }
        return value;
    }
    case ModuleExportKind::Namespace:
        RELEASE_AND_RETURN(scope, moduleNamespace(moduleExport.targetModule(), targetPhaseOf(moduleExport)));
    case ModuleExportKind::Indirect: {
        const IndirectExportData& indirectExport = m_table.indirectExport(moduleExport);
        JSObject* namespaceObject = moduleNamespace(indirectExport.targetModule(), ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, namespaceObject->get(realm, identifier(indirectExport.importName)));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Whoever asks before the evaluation has come to the file gets what a link would have captured: the file runs now, and one that is running has exported what it has exported by now.
JSObject* Modules::capturedExports(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    captureExports(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    LoadedModule& loaded = ensureLoaded(module);
    if (!loaded.capturedExports && loaded.capturedFields)
        makeTableOfCapturedFields(loaded);
    RELEASE_ASSERT(loaded.capturedExports);
    return loaded.capturedExports;
}

JSValue Modules::capturedValue(ModuleID module, const Identifier& name)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    captureExports(module);
    RETURN_IF_EXCEPTION(scope, { });
    LoadedModule& loaded = ensureLoaded(module);
    JSObject* captured = loaded.capturedExports;
    if (!captured && loaded.capturedFields)
        return capturedField(loaded, name.impl());
    RELEASE_ASSERT(captured);
    JSValue value = storedValue(realm, captured, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (value != JSValue(captured))
        return value;
    value = loaded.deferredExportsSource->get(realm, name);
    RETURN_IF_EXCEPTION(scope, { });
    // The getter may have come back here and filled the export. A variable filled in the meantime holds that value.
    JSValue filledMeanwhile = storedValue(realm, captured, name);
    if (filledMeanwhile != JSValue(captured))
        return filledMeanwhile;
    captured->putDirectMayBeIndex(realm, name, value);
    RETURN_IF_EXCEPTION(scope, { });
    return value;
}

bool Modules::hasCapturedExport(ModuleID module, const Identifier& name)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    captureExports(module);
    RETURN_IF_EXCEPTION(scope, false);
    LoadedModule& loaded = ensureLoaded(module);
    if (!loaded.capturedExports && loaded.capturedFields)
        return !!capturedField(loaded, name.impl());
    RELEASE_ASSERT(loaded.capturedExports);
    return !!storedValue(m_instance.globalObject, loaded.capturedExports, name);
}

bool Modules::isExportAmbiguous(ModuleID module, unsigned index) const
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->ambiguousExports.get(index);
}

// No code of the program sees it, so it has one Structure of its own in place of one for each name.
JSObject* Modules::createTableOfExports()
{
    VM& vm = *m_instance.vm;
    JSObject* table = constructEmptyObject(vm, m_instance.globalObject->nullPrototypeObjectStructure());
    table->convertToDictionary(vm);
    return table;
}

JSObject* Modules::namespaceIfCreated(ModuleID module, ModulePhase phase) const
{
    if (phase == ModulePhase::Evaluation && data(module).holdsNamespaceInValue() && !m_modulesWithEarlierNamespaceInValue.get(module.index())) {
        JSValue held = value(module);
        return held ? asObject(held) : nullptr;
    }
    LoadedModule* loaded = loadedIfExists(module);
    return loaded ? loaded->namespaceObjectFor(phase).get() : nullptr;
}

void Modules::setNamespace(ModuleID module, ModulePhase phase, JSObject* namespaceObject)
{
    if (phase == ModulePhase::Evaluation && data(module).holdsNamespaceInValue() && !m_modulesWithEarlierNamespaceInValue.get(module.index()))
        setValue(module, namespaceObject);
    else
        ensureLoaded(module).namespaceObjectFor(phase).set(*this, namespaceObject);
}

JSObject* Modules::createNamespace(ModuleID module, ModulePhase phase)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);
    if (m_state.statics->fillsVariablesWithFiles() && (status(module) == ModuleStatus::Linking || status(module) == ModuleStatus::Evaluating))
        m_state.statics->fillVariablesOfAllFiles();

    if (moduleData.isUncompiledCode()) {
        JSObject* loaderNamespace = uncompiledNamespace(module, phase);
        RETURN_IF_EXCEPTION(scope, nullptr);
        setNamespace(module, phase, loaderNamespace);
        return loaderNamespace;
    }
    // The names of a file are taken once, for whoever imports it and for the host.
    if (moduleData.kind == ModuleKind::CommonJs) {
        if (std::optional<ModuleID> face = m_table.faceOfFile(module))
            RELEASE_AND_RETURN(scope, moduleNamespace(*face, phase));
    }

    bool ranCode = false;
    if (status(module) == ModuleStatus::Unlinked) {
        link(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        ranCode = true;
    }
    // The names are those of what the module exports: it is evaluated first.
    if ((moduleData.capturesExports() || moduleData.kind == ModuleKind::CommonJs) && !didCaptureExports(module)) {
        captureExports(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        ranCode = true;
    }
    if (JSObject* createdMeanwhile = ranCode ? namespaceIfCreated(module, phase) : nullptr)
        return createdMeanwhile;

    Vector<DynamicExport> dynamicExports = collectDynamicExports(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ModuleNamespace::DynamicExports exportObjects;
    if (!dynamicExports.isEmpty()) {
        exportObjects.capturedValues = createTableOfExports();
        exportObjects.names = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, dynamicExports.size());
    }
    for (unsigned i = 0; i < dynamicExports.size(); ++i) {
        const DynamicExport& dynamicExport = dynamicExports[i];
        if (dynamicExport.liveSource) {
            if (!exportObjects.liveSources)
                exportObjects.liveSources = createTableOfExports();
            exportObjects.liveSources->putDirectMayBeIndex(realm, dynamicExport.name, dynamicExport.liveSource);
        } else if (dynamicExport.capturedValue)
            exportObjects.capturedValues->putDirectMayBeIndex(realm, dynamicExport.name, dynamicExport.capturedValue);
        RETURN_IF_EXCEPTION(scope, nullptr);
        JSString* text = jsString(vm, dynamicExport.name.string());
        text->toIdentifier(realm);
        RETURN_IF_EXCEPTION(scope, nullptr);
        exportObjects.names->setIndex(vm, i, text);
    }

    if (!m_namespaceStructure) {
        m_namespaceStructure = ModuleNamespace::createStructure(vm, realm, realm->moduleNamespaceObjectStructure()->storedPrototype());
        m_storedReferences.recordAll();
    }
    ModuleNamespace* namespaceObject = ModuleNamespace::create(vm, m_namespaceStructure, module, phase, exportObjects);
    if (phase == ModulePhase::Evaluation && m_modulesWithMarkOfRequire.get(module.index()) && !namespaceObject->hasExport(vm.propertyNames->__esModule.impl()))
        namespaceObject->setESModuleMarker(true);
    setNamespace(module, phase, namespaceObject);
    return namespaceObject;
}

JSObject* Modules::moduleNamespace(ModuleID module, ModulePhase phase)
{
    if (JSObject* namespaceObject = namespaceIfCreated(module, phase))
        return namespaceObject;
    return createNamespace(module, phase);
}

JSObject* Modules::importMeta(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.importMeta)
        return loaded.importMeta;
    JSObject* meta = loader()->createImportMetaProperties(m_instance.globalObject, pathString(module), nullptr, nullptr);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_ASSERT(meta);
    loaded.importMeta.set(*this, meta);
    return meta;
}

JSCell* Modules::builtinCell(BuiltinID builtin, ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const Builtin& entry = BuiltinTable::singleton().at(builtin);
    RELEASE_ASSERT(entry.scope == BuiltinScope::Module);

    if (entry.holder) {
        JSCell* holder = builtinCell(entry.holder, module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!holder || !holder->isObject())
            return nullptr;
        JSCell* member = m_state.builtinCells->readMember(m_instance.globalObject, asObject(holder), builtin);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (builtin != Builtins::ModuleRequireResolve || !host().resolveFunctionOf)
            return member;
        RELEASE_AND_RETURN(scope, host().resolveFunctionOf(m_instance.globalObject, ensureLoaded(module).requirer, member));
    }
    if (builtin == Builtins::ImportMeta)
        RELEASE_AND_RETURN(scope, importMeta(module));
    JSObject* requirer = moduleObject(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (builtin == Builtins::ModuleObject)
        return requirer;
    RELEASE_ASSERT(builtin == Builtins::ModuleRequire);
    JSValue function = ensureLoaded(module).requireFunction;
    RELEASE_ASSERT(function.isCell());
    return function.asCell();
}

JSString* Modules::resolve(ModuleID module, ResolvedSpecifierForm form)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isURL = form == ResolvedSpecifierForm::URL;
    if (m_table.hasFilePath(module) && isURL)
        return jsString(vm, URL::fileURLWithFileSystemPath(m_table.pathText(module)).string());
    JSString* specifier = pathString(module);
    if (data(module).kind != ModuleKind::External)
        return specifier;

    Identifier key;
    {
        DisallowVMEntry disallowVMEntry(vm);
        key = loader()->resolve(m_instance.globalObject, specifier, pathString(*data(module).referrerModule()), nullptr, true);
    }
    if (scope.exception()) [[unlikely]] {
        TRY_CLEAR_EXCEPTION(scope, specifier);
        return specifier;
    }
    if (isURL && isAbsoluteFileSystemPath(key.string()))
        return jsString(vm, URL::fileURLWithFileSystemPath(key.string()).string());
    return jsString(vm, key.string());
}

const SourceOrigin& Modules::sourceOrigin(ModuleID module)
{
    if (std::optional<ModuleID> evaluator = data(module).evaluatorModule())
        return sourceOrigin(*evaluator);
    LoadedModule& loaded = ensureLoaded(module);
    if (!loaded.sourceOrigin) {
        String path = m_table.pathText(module);
        loaded.sourceOrigin = makeUniqueWithoutFastMallocCheck<SourceOrigin>(m_table.hasFilePath(module) ? URL::fileURLWithFileSystemPath(path) : URL(path));
    }
    return *loaded.sourceOrigin;
}

void Modules::throwFailure(ModuleFailureID id, bool isForRequire)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleFailureData& failure = m_table.failure(id);
    String specifier = ProgramStrings::text(m_table.programTables(), failure.specifierString());
    if (failure.isAbsentExport()) {
        JSObject* error = failure.isAskedByExportStatement()
            ? createExportStatementFindsNothingError(realm, specifier, ProgramStrings::text(m_table.programTables(), failure.exportSpecifierString()))
            : createExportNotFoundError(realm, specifier, m_table.pathText(failure.importerModule()));
        // A link finds the name missing, and no code runs while modules are linked.
        uncheckedDowncast<ErrorInstance>(error)->setStackFrames(vm, { });
        throwException(realm, scope, error);
        return;
    }
    String importer = m_table.pathText(failure.importerModule());
    Identifier key;
    {
        DisallowVMEntry disallowVMEntry(vm);
        key = loader()->resolve(realm, m_state.strings->string(vm, failure.specifierString()), pathString(failure.importerModule()), nullptr, true);
    }
    RETURN_IF_EXCEPTION(scope, void());
    if (host().createLoadError) {
        JSValue error = host().createLoadError(realm, key.string(), isForRequire);
        RETURN_IF_EXCEPTION(scope, void());
        if (error) {
            throwException(realm, scope, error);
            return;
        }
    }
    throwException(realm, scope, createModuleMissingAtBuildError(realm, specifier, importer, isForRequire ? "MODULE_NOT_FOUND"_s : "ERR_MODULE_NOT_FOUND"_s));
}

static void queueImportJob(JSGlobalObject* realm, JSPromise* importPromise, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    constexpr uint8_t unusedPayload = 0;
    realm->queueMicrotask(realm->vm(), InternalMicrotask::CompiledModuleImport, unusedPayload, importPromise, jsNumber(module.index()), referrerAsyncOrder ? jsNumber(static_cast<double>(*referrerAsyncOrder)) : jsUndefined());
}

// A synchronous module load runs the jobs it diverts in its own realm, so the realm of a job is the one its import promise was created in.
void runCompiledModuleImport(JSGlobalObject*, JSValue importPromise, JSValue module, JSValue referrerAsyncOrder)
{
    auto* promise = uncheckedDowncast<JSPromise>(importPromise);
    Modules* modules = modulesOf(promise->realm());
    if (modules->continueCountedImport(promise, module, referrerAsyncOrder))
        return;
    modules->continueImport(promise, ModuleID(static_cast<uint32_t>(module.asInt32())), referrerAsyncOrder.isUndefined() ? std::nullopt : std::optional(static_cast<int64_t>(referrerAsyncOrder.asNumber())));
}

// The jobs, one behind the other, in which the loader of `bun run` hands on the namespace object of a module that it has evaluated.
static constexpr InternalMicrotask jobsOfImportOfEvaluatedModule[] = {
    InternalMicrotask::ModuleLoadTopSettled,
    InternalMicrotask::DynamicImportLoadSettled,
    InternalMicrotask::DynamicImportEvaluateSettled,
    InternalMicrotask::ModuleLoadCombinedStateSettled,
    InternalMicrotask::PromiseFulfillWithoutHandlerJob,
    InternalMicrotask::ModuleLoadTopRejected,
    InternalMicrotask::ImportModuleNamespace,
};

// Behind the fetch of the root, the making of its record and the entry of the record in the registry.
static constexpr uint32_t jobInWhichLoaderAsksRoot = 4;
static constexpr uint32_t jobsFromRequestToGenerator = 2;
// The first two find the module loaded and run what is left to run of it.
static constexpr size_t numberOfJobsBehindEvaluation = std::size(jobsOfImportOfEvaluatedModule) - 2;

// The error is empty where the evaluation has not failed.
static void queueStepOfImport(JSPromise* importPromise, JSValue module, uint8_t stepsLeft, JSValue error = { })
{
    JSGlobalObject* realm = importPromise->realm();
    realm->queueMicrotask(realm->vm(), InternalMicrotask::CompiledModuleImportStep, stepsLeft, importPromise, module, error);
}

void runCompiledModuleImportStep(JSValue importPromise, JSValue module, JSValue error, uint8_t stepsLeft)
{
    auto* promise = uncheckedDowncast<JSPromise>(importPromise);
    if (--stepsLeft)
        queueStepOfImport(promise, module, stepsLeft, error);
    else
        modulesOf(promise->realm())->finishImportOfEvaluatedModule(promise, ModuleID(static_cast<uint32_t>(module.asInt32())), error);
}

void runCompiledModuleImportSettled(JSGlobalObject*, JSValue importPromise, JSValue resolution, JSValue module, JSPromise::Status status)
{
    auto* promise = uncheckedDowncast<JSPromise>(importPromise);
    RELEASE_ASSERT(status != JSPromise::Status::Pending);
    Modules* modules = modulesOf(promise->realm());
    ModuleID imported(static_cast<uint32_t>(module.asInt32()));
    if (modules->isImportedByLoader(imported))
        queueStepOfImport(promise, module, numberOfJobsBehindEvaluation - 1, status == JSPromise::Status::Rejected ? resolution : JSValue());
    else if (status == JSPromise::Status::Rejected)
        promise->reject(promise->vm(), resolution);
    else
        modules->resolveWithNamespace(promise, imported);
}

std::optional<int64_t> Modules::asyncEvaluationOrder(ModuleID module) const
{
    ModuleRecord* record = recordIfExists(module);
    if (!record || !record->asyncEvaluationOrder().hasOrder())
        return std::nullopt;
    return record->asyncEvaluationOrder().order();
}

JSPromise* Modules::import(ModuleID module, ModuleID referrer, JSValue options)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    auto fetchParameters = JSModuleLoader::fetchParametersFromImportOptions(realm, options);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (!fetchParameters)
        return fetchParameters.error();
    if (!data(module).isUncompiledCode())
        RELEASE_AND_RETURN(scope, startImport(module, asyncEvaluationOrder(referrer)));

    const Identifier& key = resolvedKey(module);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (std::optional<ModuleID> found = findWithAttributes(key, fetchParameters->get()))
        RELEASE_AND_RETURN(scope, startImport(*found, asyncEvaluationOrder(referrer)));
    RELEASE_AND_RETURN(scope, importUncompiled(key, *data(module).referrerModule(), WTF::move(*fetchParameters)));
}

std::optional<ModuleID> Modules::findByName(const Identifier& key)
{
    std::optional<ModuleID> found = m_table.findByPath(key.string());
    if (found && data(*found).kind == ModuleKind::Runtime && host().replacesNativeModule && host().replacesNativeModule(m_instance.globalObject, key.string()))
        return std::nullopt;
    return found;
}

std::optional<ModuleID> Modules::findWithAttributes(const Identifier& key, const ScriptFetchParameters* fetchParameters)
{
    if (!fetchParameters)
        return std::nullopt;
    // A module of the library has one form.
    if (std::optional<ModuleID> module = findByName(key); module && data(*module).registryID())
        return module;
    if (!host().findModuleWithAttributes)
        return std::nullopt;
    // The loader of `bun run` keeps one module for a name and a kind of type, and every type of the host's own is of one kind: whichever is asked for first answers for all of them.
    ModuleMapKey keyOfLoader { key.impl(), fetchParameters->type() };
    if (loader()->moduleMap().contains(keyOfLoader))
        return std::nullopt;
    if (auto first = m_formsAnsweredFirst.find(keyOfLoader); first != m_formsAnsweredFirst.end())
        return first->value.module;
    std::optional<ModuleID> module = host().findModuleWithAttributes(m_instance.globalObject, key.string(), *fetchParameters);
    if (module)
        m_formsAnsweredFirst.add(keyOfLoader, FormAnsweredFirst { key, *module });
    return module;
}

// The program gets a promise of its own, as from import() in code that is not compiled.
static JSPromise* createPromiseOfProgram(JSGlobalObject* realm, JSPromise* loaded)
{
    VM& vm = realm->vm();
    JSPromise* promise = JSPromise::create(vm, realm->promiseStructure());
    if (loaded->status() == JSPromise::Status::Fulfilled)
        promise->fulfill(vm, loaded->result());
    else
        promise->resolve(realm, vm, loaded);
    return promise;
}

JSPromise* Modules::startImport(ModuleID module, std::optional<int64_t> referrerAsyncOrder, ImportCaller caller)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    bool isFile = isImportedByLoader(module);
    bool isBuiltinModule = !isFile && isBuiltinModuleOfLoader(module);
    noteRequestByLoader(module);
    noteLoadOfGraph(module);
    if (isFile || isBuiltinModule) {
        VM& vm = *m_instance.vm;
        bool isBuiltinModuleThatLoaderHas = isBuiltinModule && !isBuiltinModuleNewToLoader(module);
        bool isCalledByProgram = caller == ImportCaller::CompiledCode;
        if (isCalledByProgram && (!isBuiltinModule || isBuiltinModuleThatLoaderHas) && loaderOnlyHandsOverNamespace(module)) {
            JSPromise* promise = JSPromise::create(vm, realm->promiseStructure());
            if (promise->isThenFastAndNonObservable()) [[likely]] {
                queueStepOfImport(promise, jsNumber(module.index()), std::size(jobsOfImportOfEvaluatedModule));
                return promise;
            }
        }
        if (isCalledByProgram && isFile && canCountJobsOfImport(module)) {
            // What the graph needs of the library is loaded at the call, as where the loader is asked.
            std::unique_ptr<ModuleLinkWalk> linkWalk = takeLinkWalk(module);
            {
                SetForScope stackPointerOfImport(vm.aotState().stackPointerOfImport, currentStackPointer());
                loadRuntimeModules(*linkWalk, NativeModuleLoading::Deferred);
            }
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
            keepLinkWalk(WTF::move(linkWalk));
            JSPromise* promise = JSPromise::create(vm, realm->promiseStructure());
            if (promise->isThenFastAndNonObservable()) [[likely]] {
                uint32_t serial = m_serialOfNextCountedImport++;
                m_countedImports.append(CountedImport { Strong<JSPromise>(vm, promise), module, referrerAsyncOrder, serial });
                ++m_numberOfCountedImports;
                if (!m_orderOfCountedJobs.isEmpty()) {
                    m_orderOfCountedJobs.append(serial);
                    m_modulesOfCountedImports.set(module.index());
                }
                queueImportJob(realm, promise, module, referrerAsyncOrder);
                return promise;
            }
        }
        handCountedImportsToLoader();
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (asyncLoadMustWaitFor(module) && !loader()->registryEntry(moduleKey(module))) [[unlikely]] {
            JSPromise* promise = JSPromise::create(vm, realm->promiseStructure());
            m_deferredImports.append(DeferredImport { fileOf(module), Strong<JSPromise>(vm, promise) });
            return promise;
        }
        // `bun run` has loaded a module with the graph that first came to it, and its loader knows the module from then on.
        if (isBuiltinModuleThatLoaderHas || (data(module).kind == ModuleKind::SourceText && !isNotLoaded(module))) {
            ModuleRecord* record = registerWithLoader(module, RecordHolder::Loader);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
            if (ModuleRegistryEntry* entry = loader()->registryEntry(record->moduleKey()); entry && entry->record() == record)
                entry->markLoaded();
        }
        JSPromise* loaded = askLoaderForImport(module, referrerAsyncOrder);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (!isCalledByProgram)
            return loaded;
        RELEASE_AND_RETURN(scope, createPromiseOfProgram(realm, loaded));
    }

    if (data(module).reachesUncompiledCode() && status(module) == ModuleStatus::Unlinked) {
        handCountedImportsToLoader();
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        ModuleRecord* record = registerWithLoader(module);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        JSPromise* promise = loader()->requestImportModuleForKey(realm, record->moduleKey(), Identifier(), nullptr, nullptr, false, referrerAsyncOrder.value_or(-1));
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        didStartLoad(promise, module);
        return promise;
    }

    JSPromise* importPromise = JSPromise::create(*m_instance.vm, realm->promiseStructure());
    queueImportJob(realm, importPromise, module, referrerAsyncOrder);
    return importPromise;
}

void Modules::resumeDeferredImports(ModuleID file, uint8_t jobsUntilLoaded)
{
    m_deferredImports.removeAllMatching([&](DeferredImport& import) {
        if (import.file != file)
            return false;
        JSValue error;
        for (ModuleID evaluated : { file, m_table.faceOfFile(file).value_or(file) }) {
            if (status(evaluated) == ModuleStatus::Failed)
                error = recordIfExists(evaluated)->evaluationError();
        }
        // A failed load skips ModuleLoadingContext::Step::Requested and DynamicImportEvaluateSettled.
        uint8_t skippedJobs = error ? 2 : 0;
        queueStepOfImport(import.promise.get(), jsNumber(file.index()), jobsUntilLoaded + std::size(jobsOfImportOfEvaluatedModule) - 1 - skippedJobs, error);
        return true;
    });
}

JSPromise* Modules::askLoaderForImport(ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    bool wasAskingHost = setIsAskingHost(true);
    // What the host would answer when it is asked to fetch the module.
    ModuleRecord* record = nullptr;
    {
        SetForScope stackPointerOfImport(m_instance.vm->aotState().stackPointerOfImport, currentStackPointer());
        record = isCommonJSFileOrFace(module) ? recordOfCommonJSFile(module) : exposeRecord(module, RecordHolder::Loader);
    }
    JSPromise* loaded = scope.exception() ? nullptr : loader()->requestImportModuleForKey(m_instance.globalObject, record->moduleKey(), Identifier(), nullptr, nullptr, false, referrerAsyncOrder.value_or(-1), record);
    setIsAskingHost(wasAskingHost);
    RETURN_IF_EXCEPTION(scope, nullptr);
    didStartLoad(loaded, module);
    return loaded;
}

bool Modules::isAnythingBeingLoaded() const
{
    if (m_instance.vm->m_synchronousModuleQueue || m_rootWhoseFilesRun || m_rootWhoseFilesRanAhead || m_fileWhoseJobsWaitForLoader || !m_levelsOfLoads.isEmpty())
        return true;
    // The host loads the graph of the entry point.
    if (!m_firstModuleRequestedByHost || status(*m_firstModuleRequestedByHost) < ModuleStatus::Evaluating)
        return true;
    return m_loadsInFlight.containsIf([](const LoadInFlight& load) { return load.promise->status() == JSPromise::Status::Pending; });
}

// The steps in which the loader of `bun run` loads a graph run no code of the program but the files of the graph. Where those have run, a program sees of the steps only how many jobs they take,
// as long as it asks for no other module: so long one job counts them, and nothing else is made for the import(). Graphs that share nothing that is left to load are loaded side by side, each in its own steps.
bool Modules::canCountJobsOfImport(ModuleID root)
{
    const ModuleData& moduleData = data(root);
    if (storedStatus(root) != StoredModuleStatus::Unlinked || !isLoadedWithRequests(root) || isCommonJSFileOrFace(root))
        return false;
    if (moduleData.isLoadedByLoader() || moduleData.reachesUncompiledCode() || !moduleData.isSynchronous())
        return false;
    if (isAnythingBeingLoaded() || !isNotLoaded(root))
        return false;
    if (m_numberOfCountedImports == 1)
        noteOrderAndModulesOfOnlyCountedImport();
    return !m_numberOfCountedImports || !m_modulesOfCountedImports.get(root.index());
}

void Modules::forEachCountedImport(const Invocable<void(CountedImport&)> auto& functor)
{
    for (CountedImport* import = m_innermostCountedImportInOwnJob; import; import = import->outerInOwnJob)
        functor(*import);
    for (CountedImport& import : m_countedImports)
        functor(import);
}

// While the jobs of one import() alone are counted, their number says in which order they ran, and nobody asks what its graph holds.
void Modules::noteOrderAndModulesOfOnlyCountedImport()
{
    if (!m_orderOfCountedJobs.isEmpty())
        return;
    forEachCountedImport([&](CountedImport& import) {
        if (import.jobsOfLoader)
            return;
        m_orderOfCountedJobs.appendUsingFunctor(1 + import.jobsRun, [&](size_t) { return import.serial; });
        m_modulesOfCountedImports.set(import.module.index());
        if (!import.plan)
            return;
        for (const PlanOfLoad::Found& found : import.plan->found)
            m_modulesOfCountedImports.set(found.module.index());
    });
}

static void runJobsThatAreDue(VM& vm, MicrotaskQueue& jobs)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    // What they queue goes to the queue of the realm. As in MicrotaskQueue::drain(), only a termination stops the remaining jobs.
    jobs.allOf([&](const QueuedTask& job) {
        auto arguments = job.arguments();
        runInternalMicrotask(job.globalObject(), vm, job.job(), job.payload(), arguments[0], arguments[1], arguments[2], arguments[3]);
        return !scope.exception() || scope.tryClearException();
    });
    jobs.clear();
}

// As comesToLoadInFlight(), of the graphs whose jobs are counted. A graph that shares nothing with them that is left to load is evaluated beside them, and they are none the wiser.
bool Modules::comesToCountedImport(ModuleID required)
{
    if (!m_numberOfCountedImports)
        return false;
    // No graph leads to a root that no module imports.
    bool mayComeToOne = false;
    forEachCountedImport([&](CountedImport& import) {
        if (!import.jobsOfLoader)
            mayComeToOne |= import.module == required || data(import.module).isImportedByModule() || (import.plan && import.plan->found.size() > 1);
    });
    if (!mayComeToOne)
        return false;
    noteOrderAndModulesOfOnlyCountedImport();
    const uint32_t isSeen = takeMarkOfSearch();
    std::span<uint32_t> placesSeen = m_placesSeenByPlans.mutableSpan();
    placesSeen[required.index()] = isSeen;
    Vector<ModuleID, 16> modules { required };
    while (!modules.isEmpty()) {
        ModuleID module = modules.takeLast();
        if (m_modulesOfCountedImports.get(module.index()))
            return true;
        // The loader asks a module that it has loaded for no requests.
        if (!isNotLoaded(module))
            continue;
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (request.isHoisted || request.isRequire)
                continue;
            ModuleID target = request.targetModule();
            if (std::exchange(placesSeen[target.index()], isSeen) != isSeen && !isLoadedModuleWithoutRecord(target))
                modules.append(target);
        }
    }
    return false;
}

// The loader is asked for each graph and takes the steps that it would have taken in the jobs that were counted, in the order in which those ran. What it queues in the last step for a graph is due
// where the job of that import() stands in the queue. From here on they are loads in flight like any other. All go together: the loader decides by what else is being loaded.
void Modules::handCountedImportsToLoader()
{
    if (!m_numberOfCountedImports)
        return;
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Every counted import reaches the loader. A termination requested meanwhile is thrown at the caller's exception check.
    DeferTerminationForAWhile deferTermination(vm);
    MicrotaskQueue& queue = vm.defaultMicrotaskQueue();
    size_t numberOfJobsOfOthers = queue.size();
    noteOrderAndModulesOfOnlyCountedImport();
    m_numberOfCountedImports = 0;
    m_modulesOfCountedImports.clearAll();
    auto find = [&](uint32_t serial) -> CountedImport& {
        CountedImport* found = nullptr;
        forEachCountedImport([&](CountedImport& import) {
            if (import.serial == serial)
                found = &import;
        });
        RELEASE_ASSERT(found);
        return *found;
    };
    // The steps run no code of the program, so nothing joins or leaves the list.
    for (uint32_t serial : std::exchange(m_orderOfCountedJobs, { })) {
        CountedImport& import = find(serial);
        if (!import.jobsOfLoader) {
            import.jobsOfLoader = MicrotaskQueue::create(vm);
            JSPromise* loaded = askLoaderForImport(import.module, import.referrerAsyncOrder);
            if (scope.exception()) [[unlikely]] {
                import.promise->rejectWithCaughtException(vm, scope);
                // The reactions of the rejected promise are jobs of the program, not steps of the loader.
                numberOfJobsOfOthers = queue.size();
                continue;
            }
            import.promise->resolve(m_instance.globalObject, vm, loaded);
        } else {
            // Other jobs waited when the loader would have asked the root for its requests.
            SetForScope rootKnownToBeWatched(m_rootKnownToBeWatched, import.jobsRun >= jobInWhichLoaderAsksRoot && !import.isLoaded ? std::optional(import.module) : std::nullopt);
            SetForScope planOfRootKnownToBeWatched(m_planOfRootKnownToBeWatched, import.plan.get());
            SetForScope rootWhoseGraphIsLoaded(m_rootWhoseGraphIsLoaded, import.isLoaded ? std::optional(import.module) : std::nullopt);
            runJobsThatAreDue(vm, *import.jobsOfLoader);
        }
        scope.assertNoException();
        queue.moveTasksBehind(numberOfJobsOfOthers, *import.jobsOfLoader);
    }
}

bool Modules::hasImportedFileLeftToRun(ModuleID root) const
{
    // The files stand in front of everything else that the root requests.
    for (const ModuleRequestData& request : m_table.requests(root)) {
        // Most of the files below a root of a large program ran with an earlier graph.
        if ((request.isHoisted && m_storedStatuses[request.target()] == static_cast<uint8_t>(StoredModuleStatus::Evaluated)) || request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (request.isDeferred || !isCommonJSFileOrFace(imported)) {
            if (request.isHoisted)
                continue;
            return false;
        }
        if (storedStatus(imported) != StoredModuleStatus::Evaluated)
            return true;
    }
    return false;
}

bool Modules::continueCountedImport(JSPromise* importPromise, JSValue module, JSValue referrerAsyncOrder)
{
    size_t index = m_countedImports.findIf([&](const CountedImport& import) { return import.promise.get() == importPromise; });
    if (index == notFound)
        return false;
    CountedImport& counted = m_countedImports[index];
    uint32_t job = counted.jobsRun + 1;
    // Most of the jobs only pass the turn on.
    if (job > 1 && job != jobInWhichLoaderAsksRoot && job != counted.jobOfEvaluation && job != counted.jobOfNextNativeModule && !counted.jobsOfLoader && !m_instance.vm->defaultMicrotaskQueue().isEmpty()) {
        counted.jobsRun = job;
        if (!m_orderOfCountedJobs.isEmpty())
            m_orderOfCountedJobs.append(counted.serial);
        constexpr uint8_t unusedPayload = 0;
        m_instance.globalObject->queueMicrotask(*m_instance.vm, InternalMicrotask::CompiledModuleImport, unusedPayload, importPromise, module, referrerAsyncOrder);
        return true;
    }
    CountedImport import = WTF::move(counted);
    m_countedImports.removeAt(index);
    decideHowCountedImportGoesOn(import);
    return true;
}

// Each level of a graph is requested four jobs after the level above it.
uint32_t Modules::findJobOfNextNativeModule(PlanOfLoad& plan, uint32_t currentJob) const
{
    for (uint32_t level = 1; level < plan.levels.size(); ++level) {
        uint32_t job = level * jobInWhichLoaderAsksRoot + jobsFromRequestToGenerator;
        if (job <= currentJob)
            continue;
        for (const PlanOfLoad::Found& found : plan.level(level)) {
            if (isNativeModule(found.module) && !didCaptureExports(found.module))
                return job;
        }
    }
    return 0;
}

void Modules::forgetCountedImport(const CountedImport& import)
{
    ASSERT(m_numberOfCountedImports);
    --m_numberOfCountedImports;
    if (m_orderOfCountedJobs.isEmpty())
        return;
    // The order of the jobs of the last one says no more than their number.
    if (m_numberOfCountedImports <= 1) {
        m_orderOfCountedJobs.shrink(0);
        m_modulesOfCountedImports.clearAll();
        return;
    }
    m_orderOfCountedJobs.removeAllMatching([&](uint32_t serial) { return serial == import.serial; });
    m_modulesOfCountedImports.clear(import.module.index());
    if (!import.plan)
        return;
    for (const PlanOfLoad::Found& found : import.plan->found)
        m_modulesOfCountedImports.clear(found.module.index());
}

void Modules::decideHowCountedImportGoesOn(CountedImport& import)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    MicrotaskQueue& queue = vm.defaultMicrotaskQueue();
    JSPromise* importPromise = import.promise.get();
    ModuleID module = import.module;

    // Whoever asks about loads while the job runs finds the import() among those that are counted.
    import.outerInOwnJob = std::exchange(m_innermostCountedImportInOwnJob, &import);
    auto didLeaveJob = makeScopeExit([&] {
        m_innermostCountedImportInOwnJob = import.outerInOwnJob;
        // Only a termination leaves the job with the import still counted.
        if (!import.jobsOfLoader)
            forgetCountedImport(import);
    });
    auto goOnWithLoader = [&] {
        handCountedImportsToLoader();
        RETURN_IF_EXCEPTION(scope, void());
        scope.release();
        runJobsThatAreDue(vm, *import.jobsOfLoader);
    };
    if (import.jobsOfLoader)
        return goOnWithLoader();

    // Where no job waits none will run before the graph has run: nobody can tell in how many jobs it is loaded, unless the host does something of its own behind a job.
    bool isWatched = !queue.isEmpty() || vm.hostObservesMicrotaskBoundaries();
    if (!import.jobsRun) {
        // Whatever began to load a graph since the call has handed this one to the loader.
        if (m_innermostEvaluation || storedStatus(module) != StoredModuleStatus::Unlinked || importsFileBeingEvaluated(module))
            return goOnWithLoader();
        if (hasImportedFileLeftToRun(module)) {
            // Where somebody can tell, each file runs when the loader comes to it.
            if (isWatched)
                return goOnWithLoader();
            // Nobody sees in which step a file runs, until one of them queues a job or throws. The loader runs them where it asks the root for its requests: what a file asks for finds the load there.
            import.jobsRun = jobInWhichLoaderAsksRoot - 1;
            if (!m_orderOfCountedJobs.isEmpty())
                m_orderOfCountedJobs.appendUsingFunctor(import.jobsRun, [&](size_t) { return import.serial; });
            evaluateImportedCommonJSFiles(module, JobsOfImportedFiles::WaitForLoader, ErrorOfImportedFile::IsLeftToLoader);
            RETURN_IF_EXCEPTION(scope, void());
            if (import.jobsOfLoader || m_rootWhoseFilesRanAhead == module || storedStatus(module) != StoredModuleStatus::Unlinked)
                return goOnWithLoader();
        }
    }

    // A job counts when it has passed the turn on: the loader never catches up with the job in which the graph runs.
    uint32_t job = import.jobsRun + 1;
    bool isEvaluatedNow = !isWatched;
    if (!isEvaluatedNow && job == jobInWhichLoaderAsksRoot) {
        RefPtr<PlanOfLoad> plan;
        if (!planLevelsOfLoad(module, &plan))
            return goOnWithLoader();
        // Another graph whose jobs are counted comes to one of the modules too.
        if (!m_orderOfCountedJobs.isEmpty()) {
            auto belowRoot = plan->found.span().subspan(1);
            if (std::ranges::any_of(belowRoot, [&](const PlanOfLoad::Found& found) { return m_modulesOfCountedImports.get(found.module.index()); }))
                return goOnWithLoader();
            for (const PlanOfLoad::Found& found : belowRoot)
                m_modulesOfCountedImports.set(found.module.index());
        }
        uint32_t jobsOfLevels = plan->stepsOfWholeGraph;
        for (uint32_t level = 1; !plan->stepsOfWholeGraph && level < plan->levels.size(); ++level)
            jobsOfLevels = std::max(jobsOfLevels, level * PlanOfLoad::jobsOfLevel + plan->levels[level].laterRequests);
        // The loader hears that the root is loaded in the next job, and links and evaluates in the one behind that.
        import.jobOfEvaluation = jobInWhichLoaderAsksRoot + jobsOfLevels + 2;
        import.plan = WTF::move(plan);
        import.jobOfNextNativeModule = findJobOfNextNativeModule(*import.plan, job);
    }
    if (!isEvaluatedNow && job == import.jobOfNextNativeModule) {
        Vector<ModuleID, 4> nativeModules;
        for (const PlanOfLoad::Found& found : import.plan->level((job - jobsFromRequestToGenerator) / jobInWhichLoaderAsksRoot))
            nativeModules.append(found.module);
        for (ModuleID nativeModule : nativeModules) {
            generateNativeModuleForLoader(nativeModule);
            Exception* exception = scope.exception();
            if (!exception) [[likely]]
                continue;
            if (vm.isTerminationException(exception))
                return;
            // The link throws the error again.
            TRY_CLEAR_EXCEPTION(scope, void());
        }
        if (import.jobsOfLoader || storedStatus(module) != StoredModuleStatus::Unlinked)
            return goOnWithLoader();
        import.jobOfNextNativeModule = findJobOfNextNativeModule(*import.plan, job);
    }
    isEvaluatedNow |= job == import.jobOfEvaluation;
    if (!isEvaluatedNow) {
        import.jobsRun = job;
        if (!m_orderOfCountedJobs.isEmpty())
            m_orderOfCountedJobs.append(import.serial);
        queueImportJob(m_instance.globalObject, importPromise, module, import.referrerAsyncOrder);
        didLeaveJob.release();
        m_innermostCountedImportInOwnJob = std::exchange(import.outerInOwnJob, nullptr);
        m_countedImports.append(WTF::move(import));
        return;
    }
    if (m_innermostEvaluation || importsFileBeingEvaluated(module)) [[unlikely]]
        return goOnWithLoader();

    auto noteFilesThatLoaderHasComeTo = [&] {
        if (!import.plan)
            return;
        for (const PlanOfLoad::Found& found : import.plan->found) {
            if (found.isFile)
                m_filesLoadedWithoutLoader.set(found.module.index());
        }
    };
    std::unique_ptr<ModuleLinkWalk> linkWalk = takeLinkWalk(module);
    link(*linkWalk);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        if (vm.isTerminationException(exception))
            return;
        // The loader remembers what does not link, and reports it to whoever imports it again. It stands where it has loaded the graph and heard so of the root: it links in this job.
        TRY_CLEAR_EXCEPTION(scope, void());
        keepLinkWalk(WTF::move(linkWalk));
        if (import.jobsOfLoader) [[unlikely]]
            return goOnWithLoader();
        noteFilesThatLoaderHasComeTo();
        import.isLoaded = true;
        import.jobsRun = jobInWhichLoaderAsksRoot + 1;
        if (!m_orderOfCountedJobs.isEmpty()) {
            m_orderOfCountedJobs.removeAllMatching([&](uint32_t serial) { return serial == import.serial; });
            m_orderOfCountedJobs.appendUsingFunctor(1 + import.jobsRun, [&](size_t) { return import.serial; });
        }
        return goOnWithLoader();
    }
    if (import.jobsOfLoader) [[unlikely]]
        return goOnWithLoader();

    forgetCountedImport(import);
    didLeaveJob.release();
    m_innermostCountedImportInOwnJob = import.outerInOwnJob;
    noteFilesThatLoaderHasComeTo();
    constexpr bool isLinked = true;
    JSPromise* evaluation = evaluate(module, import.referrerAsyncOrder, importPromise, nullptr, isLinked);
    RETURN_IF_EXCEPTION(scope, void());
    switch (evaluation->status()) {
    case JSPromise::Status::Fulfilled:
        queueStepOfImport(importPromise, jsNumber(module.index()), numberOfJobsBehindEvaluation);
        return;
    case JSPromise::Status::Rejected:
        evaluation->markAsHandled();
        queueStepOfImport(importPromise, jsNumber(module.index()), numberOfJobsBehindEvaluation, evaluation->result());
        return;
    case JSPromise::Status::Pending:
        evaluation->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledModuleImportSettled, importPromise, jsNumber(module.index()));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The steps of the loader find nothing to load, to link or to run, and no other load meets them: the module has no step of its own left that a synchronous load could take over.
bool Modules::loaderOnlyHandsOverNamespace(ModuleID module) const
{
    if (status(module) != ModuleStatus::Evaluated || isCommonJSFileOrFace(module) || m_instance.vm->m_synchronousModuleQueue || m_fileWhoseJobsWaitForLoader || m_rootWhoseFilesRanAhead)
        return false;
    // An import() of a module of a cycle settles as the evaluation of the root of the cycle does, which may wait or have failed behind an await when the module itself has run.
    // Such a module has a record.
    ModuleRecord* record = data(module).isSynchronous() ? nullptr : recordIfExists(module);
    if (!record)
        return true;
    CyclicModuleRecord* root = record->cycleRoot();
    return root && root->status() == CyclicModuleRecord::Status::Evaluated && !root->evaluationError();
}

// The last step of the loader resolves its own promise, and the promise of the program follows that one.
void Modules::finishImportOfEvaluatedModule(JSPromise* importPromise, ModuleID module, JSValue error)
{
    VM& vm = *m_instance.vm;
    JSPromise* loaded = JSPromise::create(vm, m_instance.globalObject->promiseStructure());
    loaded->markAsHandled();
    if (error) [[unlikely]]
        loaded->reject(vm, error);
    else
        resolveWithNamespace(loaded, module);
    loaded->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, importPromise, jsUndefined());
}

// A load asks for the walk of its root when it begins and when it links.
std::unique_ptr<ModuleLinkWalk> Modules::takeLinkWalk(ModuleID root)
{
    std::unique_ptr<ModuleLinkWalk> walk;
    size_t index = m_linkWalksOfLoads.findIf([&](const std::unique_ptr<ModuleLinkWalk>& kept) { return kept->root() == root; });
    if (index != notFound) {
        walk = WTF::move(m_linkWalksOfLoads[index]);
        m_linkWalksOfLoads.removeAt(index);
    }
    if (!walk || !walk->bringUpToDate(*this))
        walk = makeUnique<ModuleLinkWalk>(*this, root);
    return walk;
}

// Several graphs are loaded at a time. A load that fails before it links leaves its walk behind.
void Modules::keepLinkWalk(std::unique_ptr<ModuleLinkWalk>&& walk)
{
    AOT_THRESHOLD(size_t, maxWalksKept, 8);
    if (m_linkWalksOfLoads.size() == maxWalksKept)
        m_linkWalksOfLoads.removeAt(0);
    m_linkWalksOfLoads.append(WTF::move(walk));
}

void Modules::continueImport(JSPromise* importPromise, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (status(module) == ModuleStatus::Evaluating) {
        // The body of the module is on the stack below this job, so a job queued now would run, and queue itself again, before the body can go on.
        vm.whenIdle([realm = m_instance.globalObject, importPromise = Strong<JSPromise>(vm, importPromise), module, referrerAsyncOrder] {
            queueImportJob(realm, importPromise.get(), module, referrerAsyncOrder);
        });
        return;
    }

    if (m_importThatWaitsForQueuedJobs == importPromise) {
        if (!vm.defaultMicrotaskQueue().isEmpty()) {
            queueImportJob(m_instance.globalObject, importPromise, module, referrerAsyncOrder);
            return;
        }
        m_importThatWaitsForQueuedJobs = nullptr;
    }

    std::unique_ptr<ModuleLinkWalk> linkWalk = takeLinkWalk(module);
    bool didRunModule = loadRuntimeModules(*linkWalk);
    if (scope.exception()) [[unlikely]] {
        importPromise->rejectWithCaughtException(vm, scope);
        return;
    }
    // The loader links and evaluates a graph in a later job than the ones that load it, and the jobs that a file of the graph has queued run in between.
    bool didRunFile = !didRunModule && evaluateImportedCommonJSFiles(module);
    if (scope.exception()) [[unlikely]] {
        importPromise->rejectWithCaughtException(vm, scope);
        return;
    }
    if (didRunFile)
        m_importThatWaitsForQueuedJobs = importPromise;
    if (didRunModule || didRunFile) {
        keepLinkWalk(WTF::move(linkWalk));
        queueImportJob(m_instance.globalObject, importPromise, module, referrerAsyncOrder);
        return;
    }

    JSPromise* evaluation = evaluate(module, referrerAsyncOrder, importPromise, linkWalk.get());
    RETURN_IF_EXCEPTION(scope, void());
    switch (evaluation->status()) {
    case JSPromise::Status::Pending:
        evaluation->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledModuleImportSettled, importPromise, jsNumber(module.index()));
        return;
    case JSPromise::Status::Fulfilled:
        RELEASE_AND_RETURN(scope, resolveWithNamespace(importPromise, module));
    case JSPromise::Status::Rejected:
        evaluation->markAsHandled();
        importPromise->reject(vm, evaluation->result());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Modules::resolveWithNamespace(JSPromise* importPromise, ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* namespaceObject = moduleNamespace(module, ModulePhase::Evaluation);
    if (scope.exception()) [[unlikely]] {
        importPromise->rejectWithCaughtException(vm, scope);
        return;
    }
    scope.release();
    importPromise->resolve(m_instance.globalObject, vm, namespaceObject);
}

JSPromise* Modules::importDynamic(JSValue specifier, JSValue options, ModuleID referrer)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    JSString* text = specifier.toString(realm);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    willLoadByName(realm, text, LoadByName::Import);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    auto fetchParameters = JSModuleLoader::fetchParametersFromImportOptions(realm, options);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (!fetchParameters)
        return fetchParameters.error();
    Identifier key = loader()->resolve(realm, text, pathString(referrer), nullptr, true);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (std::optional<ModuleID> found = *fetchParameters ? findWithAttributes(key, fetchParameters->get()) : findByName(key)) {
        willAnswerWithModule(realm, *found, LoadByName::Import);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        // An import() that names a file in its text names the face of the file.
        RELEASE_AND_RETURN(scope, import(m_table.faceOfFile(*found).value_or(*found), referrer));
    }
    RELEASE_AND_RETURN(scope, importUncompiled(key, referrer, WTF::move(*fetchParameters)));
}

JSPromise* Modules::importUncompiled(const Identifier& key, ModuleID referrer, RefPtr<ScriptFetchParameters>&& fetchParameters)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    handCountedImportsToLoader();
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    JSPromise* loaded = loader()->requestImportModuleForKey(realm, key, moduleKey(referrer), WTF::move(fetchParameters), nullptr, false, asyncEvaluationOrder(referrer).value_or(-1));
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    RELEASE_AND_RETURN(scope, createPromiseOfProgram(realm, loaded));
}

static bool isText(StringView path)
{
    return path.startsWith("data:"_s);
}

bool Modules::isTextNamedByProgram(StringView url) const
{
    return isText(url) && std::ranges::any_of(m_table.modulesOfUncompiledCode(), [&](uint32_t module) {
        return url == m_table.pathText(ModuleID(module));
    });
}

// A package loads its own files at any later time, so the first one the build named as external admits all. A text has no files of its own.
const Identifier& Modules::resolvedKey(ModuleID external)
{
    ASSERT(data(external).isUncompiledCode());
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    m_runsUncompiledCode = true;
    m_buildHasSeenWhoNamesModules = false;
    if (!isText(m_table.pathText(external)))
        m_admitsCodeOutsideImage = true;
    LoadedModule& loaded = ensureLoaded(external);
    if (!loaded.resolvedKey.isNull())
        return loaded.resolvedKey;
    Identifier key = loader()->resolve(m_instance.globalObject, pathString(external), pathString(*data(external).referrerModule()), nullptr, true);
    RETURN_IF_EXCEPTION(scope, loaded.resolvedKey);
    loaded.resolvedKey = WTF::move(key);
    return loaded.resolvedKey;
}

void Modules::admitCodeNamedBy(JSValue specifier)
{
    if (m_admitsCodeOutsideImage || !specifier.isString())
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto text = asString(specifier)->view(m_instance.globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    m_admitsCodeOutsideImage = std::ranges::any_of(m_table.modulesOfUncompiledCode(), [&](uint32_t module) {
        String name = m_table.pathText(ModuleID(module));
        return !isText(name) && (text == name || (text->startsWith(name) && text[name.length()] == '/'));
    });
}

ModuleRecord* Modules::exposeRecord(ModuleID module, RecordHolder holder)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The walk of the root has loaded what a module below it needs, and is kept for the link.
    if ((status(module) == ModuleStatus::Unlinked || data(module).capturesExports()) && !m_isWalkingRequests && !m_levelsOfLoads.contains(module)) {
        std::unique_ptr<ModuleLinkWalk> linkWalk = takeLinkWalk(module);
        loadRuntimeModules(*linkWalk, NativeModuleLoading::Deferred);
        RETURN_IF_EXCEPTION(scope, nullptr);
        keepLinkWalk(WTF::move(linkWalk));
    }
    ModuleRecord* record = ensureRecord(module);
    if (holder == RecordHolder::Anyone)
        record->retain();
    else if (!record->isRetained()) {
        record->setIsHeldByLoader(true);
        m_modulesWhoseRecordsLoaderHolds.append(module);
    }
    if (record->status() != CyclicModuleRecord::Status::New || !record->requestedModules().isEmpty())
        return record;

    // The loader asks for the requests of a file of the program when it comes to them.
    if (isLoadedWithRequests(module)) {
        if (m_isWalkingRequests)
            record->setIsRequestedByWalkedModule();
        return record;
    }
    RELEASE_AND_RETURN(scope, appendRequestsOfUncompiledCode(module, record));
}

// `bun run` loads a graph module by module, in jobs. A program can tell how many they are only if other jobs wait, if its own code runs between them, if another graph is being loaded, or if another one has run and the steps that settle the promise of its import() wait.
bool Modules::canTellHowGraphIsLoaded(ModuleID root)
{
    VM& vm = *m_instance.vm;
    if (m_rootWhoseGraphIsLoaded == root) [[unlikely]]
        return false;
    if (data(root).isLoadedByLoader() || m_fileWhoseJobsWaitForLoader || m_rootWhoseFilesRanAhead == root || m_rootKnownToBeWatched == root)
        return true;
    // No code of the program has run when the host asks for the module that it starts with.
    if (m_firstModuleRequestedByHost == root)
        return false;
    if (vm.m_synchronousModuleQueue || numberOfGraphsBeingLoaded() > 1)
        return true;
    // No step of the loader waits for a graph whose root has not run to its end: it goes on with a job of the program.
    if (m_loadsInFlight.containsIf([&](const LoadInFlight& load) { return load.isLoaded && status(load.root) != ModuleStatus::Evaluating && status(load.root) != ModuleStatus::EvaluatingAsync; }))
        return true;
    // With one graph being loaded the steps of the loader that wait are its own, or come behind the evaluation of an entry point. None of them runs code of the program before the graph is loaded.
    return !vm.defaultMicrotaskQueue().allOf([&](const QueuedTask& task) {
        if (task.job() >= InternalMicrotask::ModuleRegistryFetchSettled && task.job() <= InternalMicrotask::ImportModuleNamespace)
            return true;
        return task.job() == InternalMicrotask::PromiseResolveThenableJobFast && m_loadsInFlight.containsIf([&](const LoadInFlight& load) {
            return !load.isLoaded && task.arguments()[0] == JSValue(load.promise.get());
        });
    });
}

bool Modules::isBuiltinModuleOfLoader(ModuleID module) const
{
    const ModuleData& moduleData = data(module);
    return moduleData.exportRule == SyntheticExportRule::RuntimeModule && moduleData.registryID();
}

// The loader of `bun run` fetches a built-in module for the first graph that imports it, in steps of its own, and has it from then on. A require() does not ask the loader.
bool Modules::isBuiltinModuleNewToLoader(ModuleID module) const
{
    return !isSettledForLinks(module) && !m_filesLoadedWithoutLoader.get(module.index()) && isBuiltinModuleOfLoader(module);
}

// `bun run` rejects the load of a module where the first of its requests fails, and the importer of that module hears of it two jobs later, and so on up to the root.
ModuleGraphLoadingState* Modules::loadThatWaitsFor(ModuleID requester, const Identifier& key, ModuleGraphLoadingState* loadOfRequester, unsigned& jobsUntilLoadHearsOfFailure)
{
    if (m_filesAskedForOnBehalfOfRoot.isEmpty()) [[likely]]
        return loadOfRequester;
    size_t index = m_filesAskedForOnBehalfOfRoot.findIf([&](const FileAskedForOnBehalfOfRoot& file) { return file.requester == requester && file.key == key; });
    if (index == notFound)
        return loadOfRequester;
    FileAskedForOnBehalfOfRoot file = WTF::move(m_filesAskedForOnBehalfOfRoot[index]);
    m_filesAskedForOnBehalfOfRoot.removeAt(index);
    auto* loadOfRoot = uncheckedDowncast<ModuleGraphLoadingState>(file.plan->load.get());
    loadOfRequester->setPendingModulesCount(loadOfRequester->pendingModulesCount() - 1);
    loadOfRoot->setPendingModulesCount(loadOfRoot->pendingModulesCount() + 1);
    jobsUntilLoadHearsOfFailure = 2 * (file.depth - 1);
    return loadOfRoot;
}

void Modules::appendRequestsForLoader(ModuleID module, ModuleRecord* record, ModuleGraphLoadingState& load)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!isLoadedWithRequests(module) || !record->requestedModules().isEmpty())
        return;
    auto appendNextOnWay = [&] {
        std::unique_ptr<LevelOfLoad> level = m_levelsOfLoads.take(module);
        if (!level)
            return false;
        if (level->next && level->plan && (!m_levelsOfLoads.contains(*level->next) || !isStillToBeLoaded(*level->next)))
            layWayOfLoadAgain(*level);
        // Nobody sees how many steps are taken before require() returns.
        if (level->next && level->plan && !m_instance.vm->m_synchronousModuleQueue)
            countStepsOfLevel(*level->plan, level->depth + 1);
        if (m_showsWaysBetweenModuleObjects && level->next && level->plan && level->depth + 1 < level->plan->levels.size()) {
            for (const PlanOfLoad::Found& found : level->plan->level(level->depth + 1))
                noteRequestByLoader(found.module);
        }
        auto append = [&](ModuleID target) {
            Identifier key = keyOfRequest(target);
            record->appendRequestedModule(key, nullptr, ModulePhase::Evaluation);
            record->didRequestCompiledModule(key, target);
            return key;
        };
        auto appendFiles = [&](const Vector<LevelOfLoad::FileOfNextLevel, 4>& files) {
            for (const LevelOfLoad::FileOfNextLevel& file : files) {
                // The loader has the file once it has run, and takes no step for it again.
                m_filesLoadedWithoutLoader.set(file.module.index());
                // A require() has come between.
                if (status(file.module) == ModuleStatus::Evaluated)
                    continue;
                noteLoadOfFile(file.module);
                RETURN_IF_EXCEPTION(scope, void());
                Identifier key = append(file.module);
                if (!file.isImportedByModuleOnWay)
                    m_filesAskedForOnBehalfOfRoot.append(FileAskedForOnBehalfOfRoot { module, key, level->plan, level->depth + 1 });
            }
        };
        appendFiles(level->filesInFrontOfNext);
        RETURN_IF_EXCEPTION(scope, true);
        if (level->next) {
            if (auto asked = m_levelsOfLoads.find(*level->next); asked != m_levelsOfLoads.end())
                asked->value->isAskedFor = true;
            append(*level->next);
        }
        appendFiles(level->filesBehindNext);
        return true;
    };
    if (!m_levelsOfLoads.isEmpty() && appendNextOnWay())
        return;
    if (!record->isRequestedByWalkedModule()) {
        bool mustWaitForFile = importsFileBeingEvaluated(module);
        bool isWalked = mustWaitForFile || canTellHowGraphIsLoaded(module);
        if (!isWalked) {
            // Nobody sees in which step a file runs, until one of them queues a job or throws.
            evaluateImportedCommonJSFiles(module, JobsOfImportedFiles::WaitForLoader, ErrorOfImportedFile::IsLeftToLoader);
            RETURN_IF_EXCEPTION(scope, void());
            isWalked = m_rootWhoseFilesRanAhead == module;
        }
        if (!isWalked) {
            scope.release();
            appendRequestsOfUncompiledCode(module, record);
            return;
        }
        for (LoadInFlight& load : m_loadsInFlight)
            load.isWalked |= load.root == module;
        if (!data(module).isLoadedByLoader() && m_rootWhoseFilesRanAhead != module && !mustWaitForFile && planLevelsOfLoad(module)) {
            m_levelsOfLoads.get(module)->plan->load.set(*m_instance.vm, &load);
            appendNextOnWay();
            return;
        }
    }

    OrderedHashSet<UniquedStringImpl*> keys;
    WTF::Vector<Identifier, 8> keptAlive;
    for (const ModuleRequestData& request : m_table.imports(module)) {
        if (request.isHoisted)
            continue;
        ModuleID target = request.targetModule();
        // The loader of `bun run` has such a module, and takes no step for it.
        if (wasLoadedWithEarlierGraph(target))
            continue;
        const ModuleData& targetData = data(target);
        if (targetData.isUncompiledCode()) {
            keptAlive.append(resolvedKey(target));
            if (Exception* exception = scope.exception()) [[unlikely]] {
                // The loader reports the error when it comes to the request, and asks for nothing behind it.
                VM& vm = *m_instance.vm;
                TRY_CLEAR_EXCEPTION(scope, void());
                Identifier name = Identifier::fromString(vm, m_table.pathText(target));
                JSModuleLoader::attachErrorInfo(m_instance.globalObject, exception, nullptr, name, ScriptFetchParameters::Type::JavaScript, JSModuleLoader::ModuleFailure::Kind::Instantiation);
                loader()->addResolutionFailure(vm, { record->moduleKey().impl(), name.impl() }, exception->value());
                record->appendRequestedModule(name, nullptr, request.isDeferred ? ModulePhase::Defer : ModulePhase::Evaluation);
                return;
            }
        } else if (isCommonJSFileOrFace(target)) {
            // The loader may have the file, and then asks nobody for its record.
            noteLoadOfFile(target);
            RETURN_IF_EXCEPTION(scope, void());
            noteRequestByLoader(target);
            keptAlive.append(keyOfRequest(target));
        } else if (targetData.kind == ModuleKind::Data || isFileOfProgram(target) || isBuiltinModuleNewToLoader(target))
            keptAlive.append(keyOfRequest(target));
        else
            continue;
        if (!keys.add(keptAlive.last().impl()).isNewEntry)
            continue;
        record->appendRequestedModule(keptAlive.last(), nullptr, request.isDeferred ? ModulePhase::Defer : ModulePhase::Evaluation);
        if (!targetData.isUncompiledCode())
            record->didRequestCompiledModule(keptAlive.last(), target);
    }
}

// Throws where a name does not resolve.
void Modules::collectKeysOfUncompiledCode(ModuleID module, OrderedHashSet<UniquedStringImpl*>& keys)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    BitVector visited(m_table.size());
    visited.quickSet(module.index());
    Vector<std::span<const ModuleRequestData>, 16> path { m_table.requests(module) };
    while (!path.isEmpty()) {
        std::span<const ModuleRequestData>& requests = path.last();
        if (requests.empty()) {
            path.removeLast();
            continue;
        }
        ModuleID target = requests.front().targetModule();
        requests = requests.subspan(1);
        if (!data(target).reachesUncompiledCode())
            continue;
        if (data(target).isUncompiledCode()) {
            const Identifier& key = resolvedKey(target);
            RETURN_IF_EXCEPTION(scope, void());
            keys.add(key.impl());
        } else if (status(target) == ModuleStatus::Unlinked && !visited.quickSet(target.index()))
            path.append(m_table.requests(target));
    }
}

// The load ends where the loader comes to such code, and the files that stand behind it never run.
bool Modules::comesToCodeThatDoesNotLoad(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    OrderedHashSet<UniquedStringImpl*> keys;
    collectKeysOfUncompiledCode(root, keys);
    if (scope.exception()) [[unlikely]] {
        TRY_CLEAR_EXCEPTION(scope, true);
        return true;
    }
    for (UniquedStringImpl* key : keys) {
        if (ModuleRegistryEntry* entry = loader()->registryEntry(Identifier::fromUid(*m_instance.vm, key)); entry && entry->fetchError())
            return true;
    }
    return false;
}

ModuleRecord* Modules::appendRequestsOfUncompiledCode(ModuleID module, ModuleRecord* record)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!data(module).reachesUncompiledCode())
        return record;
    OrderedHashSet<UniquedStringImpl*> keys;
    collectKeysOfUncompiledCode(module, keys);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (UniquedStringImpl* key : keys)
        record->appendRequestedModule(Identifier::fromUid(vm, key), nullptr, ModulePhase::Evaluation);
    return record;
}

ModuleRecord* Modules::registerWithLoader(ModuleID module, RecordHolder holder)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleRecord* record = exposeRecord(module, holder);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ModuleRegistryEntry* entry = loader()->ensureRegistered(m_instance.globalObject, record->moduleKey(), ScriptFetchParameters::Type::JavaScript);
    if (!entry->record() && entry->status() == ModuleRegistryEntry::Status::New)
        entry->provideModule(*m_instance.vm, record);
    return record;
}

AbstractModuleRecord* Modules::uncompiledRecord(ModuleID external)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(external);
    if (loaded.uncompiledRecord)
        return loaded.uncompiledRecord;
    const Identifier& key = resolvedKey(external);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ModuleRegistryEntry* entry = loader()->registryEntry(key);
    AbstractModuleRecord* record = entry ? entry->record() : nullptr;
    if (!record) {
        throwException(m_instance.globalObject, scope, createExternalModuleNotLoadedError(m_instance.globalObject, m_table.pathText(external)));
        return nullptr;
    }
    loaded.uncompiledRecord.set(*this, record);
    return record;
}

AbstractModuleRecord* Modules::recordForResolution(ModuleID module)
{
    if (data(module).isUncompiledCode())
        return uncompiledRecord(module);
    ModuleRecord* record = ensureRecordWithoutStatus(module);
    record->retain();
    return record;
}

JSObject* Modules::uncompiledNamespace(ModuleID external, ModulePhase phase)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    AbstractModuleRecord* record = uncompiledRecord(external);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, record->moduleNamespaceObject(m_instance.globalObject, phase));
}

JSC_DEFINE_JIT_OPERATION(operationAOTModuleBuiltinCell, JSCell*, (Instance* instance, uint32_t builtin, uint32_t module))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTModuleBuiltinCell);
    OPERATION_RETURN(scope, modulesOf(*instance).builtinCell(BuiltinID(builtin), ModuleID(module)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetModuleNamespace, JSCell*, (Instance* instance, uint32_t module, uint32_t phase))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetModuleNamespace);
    RELEASE_ASSERT(static_cast<ImportPhase>(phase) == ImportPhase::Evaluation || static_cast<ImportPhase>(phase) == ImportPhase::Defer);
    OPERATION_RETURN(scope, modulesOf(*instance).moduleNamespace(ModuleID(module), static_cast<ImportPhase>(phase) == ImportPhase::Defer ? Modules::ModulePhase::Defer : Modules::ModulePhase::Evaluation));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsNamespaceOfModule, size_t, (JSCell* cell, uint32_t module))
{
    auto* moduleNamespace = dynamicDowncast<ModuleNamespace>(cell);
    return moduleNamespace && moduleNamespace->hasNamespaceLayoutOf(ModuleID(module));
}

JSC_DEFINE_JIT_OPERATION(operationAOTImportModule, EncodedJSValue, (Instance* instance, uint32_t module, EncodedJSValue options, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTImportModule);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).import(ModuleID(module), ModuleID(referrer), JSValue::decode(options))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRequireModule, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireModule);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).require(ModuleID(module), ModuleID(referrer))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRecordRequire, void, (Instance* instance, uint32_t module, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRecordRequire);
    modulesOf(*instance).recordRequire(ModuleID(module), ModuleID(referrer));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolveModule, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t, uint32_t, uint32_t isURL))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTResolveModule);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).resolve(ModuleID(module), isURL ? ResolvedSpecifierForm::URL : ResolvedSpecifierForm::Path)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowModuleFailure, void, (Instance* instance, uint32_t failure, uint32_t isForRequire))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowModuleFailure);
    modulesOf(*instance).throwFailure(ModuleFailureID(failure), isForRequire);
    OPERATION_RETURN(scope);
}

// For a module whose status is in a record: one that failed, one that JavaScriptCore's loader knows, and one whose exports are captured behind its evaluation. Nothing is left for the caller to run.
JSC_DEFINE_JIT_OPERATION(operationAOTBeginModuleEvaluation, size_t, (Instance* instance, uint32_t module))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTBeginModuleEvaluation);
    OPERATION_RETURN(scope, modulesOf(*instance).beginModuleInRecord(ModuleID(module)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStopLoadingBehindFile, size_t, (Instance* instance, uint32_t module))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStopLoadingBehindFile);
    OPERATION_RETURN(scope, modulesOf(*instance).stopLoadingBehindFile(ModuleID(module)));
}

// From the handler around the evaluation of a module, with what it has just caught and is about to throw again.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAbandonModuleEvaluation, void, (Instance* instance, uint32_t module, JSCell* exception))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAbandonModuleEvaluation);
    DeferTerminationForAWhile deferTermination(vm);
    modulesOf(*instance).abandonEvaluation(ModuleID(module), cellArgument<Exception>(exception, Entry::operationAOTAbandonModuleEvaluation)->value());
    scope.assertNoException();
}

JSC_DEFINE_JIT_OPERATION(operationAOTImportDynamic, EncodedJSValue, (Instance* instance, EncodedJSValue specifier, EncodedJSValue options, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTImportDynamic);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).importDynamic(JSValue::decode(specifier), JSValue::decode(options), ModuleID(referrer))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRequireDynamic, EncodedJSValue, (Instance* instance, EncodedJSValue specifier, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireDynamic);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).requireSpecifier(JSValue::decode(specifier), ModuleID(referrer), false)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolveDynamic, EncodedJSValue, (Instance* instance, EncodedJSValue specifier, uint32_t referrer, uint32_t isURL, uint32_t isForRequire))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTResolveDynamic);
    ResolvedSpecifierForm form = isForRequire ? ResolvedSpecifierForm::PathForRequire : isURL ? ResolvedSpecifierForm::URL : ResolvedSpecifierForm::Path;
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).resolveDynamic(JSValue::decode(specifier), ModuleID(referrer), form)));
}

static bool s_logsEvaluations;

const ModuleHost& moduleHostOf(JSGlobalObject* realm)
{
    const ModuleHost* host = realm->globalObjectMethodTable()->moduleHost;
    RELEASE_ASSERT(host);
    return *host;
}

static std::optional<ModuleTable> currentModuleTable()
{
    const Image* image = Image::current();
    return image ? std::optional(ModuleTable(image->programTables())) : std::nullopt;
}

static Modules* modulesThatShowWaysBetweenModuleObjects(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules && Image::current()->showsWaysBetweenModuleObjects() ? modules : nullptr;
}

std::optional<ModuleID> findModule(StringView publicPath)
{
    struct PathsNotInImage {
        Lock lock;
        std::array<std::pair<const Image*, String>, 32> paths WTF_GUARDED_BY_LOCK(lock);
    };
    static NeverDestroyed<PathsNotInImage> pathsNotInImage;

    std::optional<ModuleTable> table = currentModuleTable();
    if (!table)
        return std::nullopt;
    if (publicPath.startsWith('#')) {
        std::optional<uint32_t> number = parseInteger<uint32_t>(publicPath.substring(1));
        if (number && *number < table->size() && table->pathText(ModuleID(*number)) == publicPath)
            return ModuleID(*number);
        return std::nullopt;
    }
    PathsNotInImage& notFound = pathsNotInImage.get();
    const Image* image = Image::current();
    size_t place = publicPath.hash() % notFound.paths.size();
    {
        Locker locker { notFound.lock };
        if (notFound.paths[place].first == image && notFound.paths[place].second == publicPath)
            return std::nullopt;
    }
    std::optional<ModuleID> found = table->findByPath(publicPath);
    if (!found) {
        String copy = publicPath.toString().isolatedCopy();
        Locker locker { notFound.lock };
        notFound.paths[place] = { image, WTF::move(copy) };
    }
    return found;
}

bool holdsFormThatAttributeAsksFor(StringView publicPath)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->holdsFormThatAttributeAsksFor(publicPath);
}

// Each frame of each stack trace asks.
struct PathOfModule {
    // A static string: every thread may hold it.
    StringImpl* path;
    bool isEvaluatedText;
};

static PathOfModule pathOfModule(ModuleID module)
{
    struct PathsOfModules {
        Lock lock;
        UncheckedKeyHashMap<uint64_t, PathOfModule> byImageAndModule WTF_GUARDED_BY_LOCK(lock);
    };
    static NeverDestroyed<PathsOfModules> pathsOfModules;
    PathsOfModules& paths = pathsOfModules.get();
    Locker locker { paths.lock };
    uint64_t key = static_cast<uint64_t>(Image::current()->number()) << 32 | (module.index() + 1);
    return paths.byImageAndModule.ensure(key, [&] {
        std::optional<ModuleTable> table = currentModuleTable();
        RELEASE_ASSERT(table);
        // A text that the program evaluates is shown under the URL of the file that evaluates it, as the engine shows the text of `new Function(text)`.
        std::optional<ModuleID> evaluator = table->data(module).evaluatorModule();
        String path = evaluator ? URL::fileURLWithFileSystemPath(table->pathText(*evaluator)).string() : table->data(module).registryID() ? table->displayPathText(module) : table->pathText(module);
        return PathOfModule { &(path.is8Bit() ? StringImpl::createStaticStringImpl(path.span8()) : StringImpl::createStaticStringImpl(path.span16())).leakRef(), !!evaluator };
    }).iterator->value;
}

String modulePath(ModuleID module)
{
    return pathOfModule(module).path;
}

String sourceProviderURL(ModuleID module)
{
    PathOfModule found = pathOfModule(module);
    return found.isEvaluatedText ? emptyString() : String(found.path);
}

bool entryPointIsCommonJS(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    std::optional<ModuleID> entryPoint = modules ? modules->firstModuleRequestedByHost() : std::nullopt;
    return entryPoint && modules->data(*entryPoint).kind == ModuleKind::CommonJs;
}

bool isESModule(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::SourceText;
}

bool isNativeModule(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::Runtime;
}

bool leavesModuleToHost(JSGlobalObject* realm, ModuleID module)
{
    if (modulesOf(realm))
        return false;
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).isOfLibrary();
}

void forEachModule(const ScopedLambda<IterationStatus(ModuleID)>& functor)
{
    std::optional<ModuleTable> table = currentModuleTable();
    for (unsigned index = 0; table && index < table->size(); ++index) {
        if (table->hasFilePath(ModuleID(index)) && functor(ModuleID(index)) == IterationStatus::Done)
            return;
    }
}

bool refusesCodeOutsideImage(JSGlobalObject* realm, StringView path)
{
    if (Modules* modules = modulesOf(realm))
        return modules->refusesCodeOutsideImage() && !modules->isTextNamedByProgram(path);
    const Image* image = Image::current();
    return image && !hasFlag(image->header().options.admitted(), AdmittedCode::Inspector) && !image->loadsCodeOutsideImage();
}

bool refusesHostObjectsOutsideImage(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return refusesCodeOutsideImage(realm, { }) && !(modules && modules->isAskingHost());
}

JSObject* mainModuleObject(JSGlobalObject* realm)
{
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules ? modules->mainModuleObject() : nullptr;
}

JSObject* parentModuleObject(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules ? modules->parentModuleObject(module) : nullptr;
}

bool forEachNewChild(JSGlobalObject* realm, ModuleID module, const ScopedLambda<void(JSObject*)>& functor)
{
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules && modules->forEachNewChild(module, functor);
}

JSValue moduleExports(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->commonJSExports(module) : jsUndefined();
}

void setModuleExports(JSGlobalObject* realm, ModuleID module, JSValue exports)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    ASSERT(modules->data(module).kind == ModuleKind::CommonJs);
    modules->setCommonJSExports(module, exports);
}

static void throwLoadByNameRefused(JSGlobalObject* realm, StringView name, LoadByName form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwException(realm, scope, createLoadByNameRefusedError(realm, name, form == LoadByName::Import ? "ERR_MODULE_NOT_FOUND"_s : "MODULE_NOT_FOUND"_s));
}

void willLoadByName(JSGlobalObject* realm, JSValue specifier, LoadByName form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    Modules* modules = modulesOf(realm);
    // Code from outside the image has a loader of its own: its names pass, and one that leads to a file of the image is refused there.
    if (modules ? modules->refusesLoadsByName() && modules->refusesCodeOutsideImage() : !Image::current()->loadsModulesByName()) {
        String name = specifier.isString() ? asString(specifier)->value(realm) : String();
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, throwLoadByNameRefused(realm, name, form));
    }
    if (modules)
        RELEASE_AND_RETURN(scope, modules->admitCodeNamedBy(specifier));
}

static Modules* modulesOrThrow(JSGlobalObject* realm, ModuleID module, LoadByName form = LoadByName::Import)
{
    Modules* modules = modulesOf(realm);
    if (modules && !modules->refusesRequestFromHost(module)) [[likely]]
        return modules;
    if (modules) {
        throwLoadByNameRefused(realm, modulePath(module), form);
        return nullptr;
    }
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
#if ENABLE(SHADOW_REALM)
    throwException(realm, scope, createModuleInShadowRealmError(realm, modulePath(module)));
#else
    throwException(realm, scope, createModuleInOtherRealmError(realm, modulePath(module)));
#endif
    return nullptr;
}

void willAnswerWithModule(JSGlobalObject* realm, ModuleID module, LoadByName form)
{
    modulesOrThrow(realm, module, form);
}

void setVerbose(bool isVerbose)
{
    s_logsEvaluations = isVerbose;
}

static Modules* modulesForEvaluation(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOrThrow(realm, module);
    if (!modules)
        return nullptr;
    modules->didRequestFromHost(module);
    if (s_logsEvaluations && modules->status(module) < ModuleStatus::Evaluating) [[unlikely]]
        writeToStandardError(makeString("[aot] module: "_s, modulePath(module), '\n'));
    return modules;
}

AbstractModuleRecord* moduleRecord(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesForEvaluation(realm, module);
    if (!modules)
        return nullptr;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    // The loader asks for a module when it loads it. Where it goes on to what the module imports, each file runs when the loader comes to it.
    if (!modules->isAskingHost()) {
        modules->handCountedImportsToLoader();
        RETURN_IF_EXCEPTION(scope, nullptr);
        modules->evaluateImportedCommonJSFiles(module, JobsOfImportedFiles::WaitForLoader, ErrorOfImportedFile::IsLeftToLoader);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (modules->isCommonJSFileOrFace(module))
        RELEASE_AND_RETURN(scope, modules->recordOfCommonJSFile(module));
    // A module of the image asks through the loader. Code that is not compiled binds what it imports to the record.
    RELEASE_AND_RETURN(scope, modules->exposeRecord(module, modules->isAskingHost() ? Modules::RecordHolder::Loader : Modules::RecordHolder::Anyone));
}

bool isCompiledModuleRecord(AbstractModuleRecord* record)
{
    return record->inherits<ModuleRecord>();
}

JSPromise* importModule(JSGlobalObject* realm, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    Modules* modules = modulesForEvaluation(realm, module);
    return modules ? modules->startImport(module, referrerAsyncOrder, Modules::ImportCaller::Host) : nullptr;
}

void evaluateModuleSynchronously(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesForEvaluation(realm, module))
        modules->evaluateSynchronously(module, SynchronousEvaluationReason::Require);
}

JSObject* moduleNamespace(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOrThrow(realm, module);
    return modules ? modules->moduleNamespace(module, Modules::ModulePhase::Evaluation) : nullptr;
}

JSValue requireModule(JSGlobalObject* realm, ModuleID module, std::optional<ModuleID> referrer)
{
    Modules* modules = modulesForEvaluation(realm, module);
    return modules ? modules->require(module, referrer.value_or(ModuleID())) : JSValue();
}

static void throwLoaderChangeRefused(JSGlobalObject* realm, LoaderChange change, StringView changed)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwException(realm, scope, createLoaderChangeRefusedError(realm, change, changed));
}

// The host's require() takes no entry that is falsy.
void willSetRequireCacheEntry(JSGlobalObject* realm, ModuleID module, JSValue entry)
{
    Modules* modules = modulesOf(realm);
    if (modules && entry.toBoolean(realm))
        throwLoaderChangeRefused(realm, LoaderChange::ReplaceInRequireCache, modules->table().pathText(module));
}

// A request that is resolved at run time asks the other two functions, as under plain Bun.
void willChangeRequireOverride(JSGlobalObject* realm, RequireOverride location, bool isOverridden)
{
    if (modulesOf(realm) && isOverridden && location == RequireOverride::Extensions)
        throwLoaderChangeRefused(realm, LoaderChange::ReplaceLoaderFunction, "a loader in Module._extensions"_s);
}

bool hasFileWithExtension(StringView extension)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->hasFileWithExtension(extension);
}

bool isModuleNamespace(JSCell* cell)
{
    return cell->inherits<ModuleNamespace>();
}

bool hasESModuleMarker(JSCell* moduleNamespace)
{
    return uncheckedDowncast<ModuleNamespace>(moduleNamespace)->hasESModuleMarker();
}

void setESModuleMarker(JSCell* moduleNamespace, bool hasMarker)
{
    uncheckedDowncast<ModuleNamespace>(moduleNamespace)->setESModuleMarker(hasMarker);
}

static constexpr unsigned invalidIndex = static_cast<unsigned>(-1);

const ClassInfo ModuleRecord::s_info = { "CompiledModuleRecord"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleRecord) };

Structure* ModuleRecord::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

ModuleRecord::ModuleRecord(Instance& instance, Structure* structure, JSModuleLoader* loader, const Identifier& moduleKey, ModuleID module)
    : Base(*instance.vm, structure, loader, moduleKey, SourceProviderSourceType::Module)
    , m_instance(instance)
    , m_module(module)
{
}

ModuleRecord* ModuleRecord::create(Instance& instance, Structure* structure, JSModuleLoader* loader, const Identifier& moduleKey, ModuleID module)
{
    auto* record = new (NotNull, allocateCell<ModuleRecord>(*instance.vm)) ModuleRecord(instance, structure, loader, moduleKey, module);
    record->finishCreation(instance.globalObject, *instance.vm);
    return record;
}

void ModuleRecord::destroy(JSCell* cell)
{
    static_cast<ModuleRecord*>(cell)->ModuleRecord::~ModuleRecord();
}

JSGlobalObject* ModuleRecord::instanceRealm() const
{
    return m_instance.globalObject;
}

Modules& ModuleRecord::modules() const
{
    return modulesOf(m_instance);
}

unsigned ModuleRecord::innerModuleEvaluation(JSGlobalObject* globalObject, ModuleEvaluationStack& stack, unsigned index, int64_t referrerAsyncOrder, JSPromise* dynamicImportPromise)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    EvaluationContext context { stack, index, referrerAsyncOrder < 0 ? std::nullopt : std::optional(referrerAsyncOrder), dynamicImportPromise };
    modules().innerModuleEvaluation(m_module, context);
    // The module may be evaluated and off the stack by now, so a request to terminate waits for the next check.
    RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, invalidIndex);
    return context.index;
}

void ModuleRecord::willLoadRequests(ModuleGraphLoadingState& load)
{
    modules().appendRequestsForLoader(m_module, this, load);
}

ModuleGraphLoadingState* ModuleRecord::loadThatWaitsFor(const Identifier& key, ModuleGraphLoadingState* loadOfRequester, unsigned& jobsUntilLoadHearsOfFailure)
{
    return modules().loadThatWaitsFor(m_module, key, loadOfRequester, jobsUntilLoadHearsOfFailure);
}

void ModuleRecord::didLoad()
{
    modules().evaluateFilesOfLevel(m_module, m_runsWhenLoaded);
}

void ModuleRecord::didFailToLoad(JSGlobalObject* globalObject, const ModuleGraphLoadingState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Exception* exception = scope.exception();
    if (exception)
        TRY_CLEAR_EXCEPTION(scope, void());
    state.iterateVisited([&](CyclicModuleRecord* visited) {
        auto* compiled = dynamicDowncast<ModuleRecord>(visited);
        if (!compiled || scope.exception())
            return;
        compiled->modules().didFailToLoadGraph(compiled->m_module);
        if (!scope.exception() && compiled->modules().hasDeferredLoads()) [[unlikely]]
            compiled->modules().didSettleLoad(state);
    });
    if (!scope.exception() && exception)
        throwException(globalObject, scope, exception);
}

std::optional<ModuleID> ModuleRecord::fileToWaitFor(JSGlobalObject* globalObject, const Identifier& key)
{
    Modules* modules = modulesOf(globalObject);
    return modules ? modules->fileToWaitFor(key) : std::nullopt;
}

JSPromise* ModuleRecord::deferRequest(JSGlobalObject* globalObject, ModuleID file, ModuleLoadingContext* context, bool useImportMap)
{
    return modulesOf(globalObject)->deferRequest(file, context, useImportMap);
}

void ModuleRecord::didRequestModule(JSGlobalObject* globalObject, const Identifier& key, JSCell* payload)
{
    Modules* modules = modulesOf(globalObject);
    if (modules && modules->hasDeferredLoads()) [[unlikely]]
        modules->didRequestModule(key, payload);
}

void ModuleRecord::didSettleLoad(JSGlobalObject* globalObject, const ModuleGraphLoadingState& load)
{
    Modules* modules = modulesOf(globalObject);
    if (modules && modules->hasDeferredLoads()) [[unlikely]]
        modules->didSettleLoad(load);
}

void ModuleRecord::didFailToMakeModule(JSGlobalObject* globalObject, const Identifier& key)
{
    Modules* modules = modulesOf(globalObject);
    if (modules && modules->hasDeferredLoads()) [[unlikely]]
        modules->didFailToMakeModule(key);
}

void ModuleRecord::didReplaceRecordInRegistry()
{
    modules().captureExportsAgain(m_module);
}

void ModuleRecord::willBeLoadedByImport()
{
    modules().willBeLoadedByImport(m_module);
}

bool ModuleRecord::takeStepOfMainModule()
{
    return !requestedModules().isEmpty() && modules().takeStepOfMainModule(m_module);
}

bool ModuleRecord::takeLoadPromiseBelowMainModule()
{
    return !requestedModules().isEmpty() && modules().takeLoadPromiseBelowMainModule(m_module);
}

void ModuleRecord::takeStatus()
{
    if (!m_holdsStatus)
        modules().ensureRecord(m_module);
}

bool ModuleRecord::isRunningCommonJSFile()
{
    return modules().isCommonJSFileOrFace(m_module) && modules().isImportedByLoader(m_module) && modules().status(m_module) == ModuleStatus::Evaluating;
}

uint8_t ModuleRecord::willAskHostForRequest()
{
    bool wasWalkingRequests = modules().setIsWalkingRequests(true);
    bool wasAskingHost = modules().setIsAskingHost(true);
    return static_cast<uint8_t>(wasAskingHost) | static_cast<uint8_t>(wasWalkingRequests) << 1;
}

void ModuleRecord::didAskHostForRequest(uint8_t flagsBefore)
{
    modules().setIsAskingHost(flagsBefore & 1);
    modules().setIsWalkingRequests(flagsBefore & 2);
}

AbstractModuleRecord* ModuleRecord::recordOfRequest(JSGlobalObject* globalObject, const Identifier& key)
{
    for (unsigned index = m_nextCompiledRequest; index < m_compiledRequests.size(); ++index) {
        if (m_compiledRequests[index].first != key.impl())
            continue;
        m_nextCompiledRequest = index + 1;
        return moduleRecord(globalObject, m_compiledRequests[index].second);
    }
    return nullptr;
}

void ModuleRecord::generateDeferredSyntheticModules(JSGlobalObject*, VisitedModuleRecords& visited)
{
    modules().captureSyntheticExports(m_module, visited);
}

unsigned ModuleRecord::innerModuleLinking(JSGlobalObject* globalObject, ModuleLinkingStack& stack, unsigned index, RefPtr<ScriptFetcher> scriptFetcher)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    LinkContext context { stack, index, WTF::move(scriptFetcher) };
    modules().queueJobsThatWaitForLoader();
    modules().didLoadGraph(m_module);
    RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, invalidIndex);
    modules().finishLink(m_module, context);
    if (!scope.exception() && status() == Status::Unlinked && !modules().data(m_module).needsLink())
        setStatus(Status::Linked);
    // The module may be linked by now, so a request to terminate waits for the next check.
    RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, invalidIndex);
    return context.index;
}

void ModuleRecord::execute(JSGlobalObject*, JSPromise* capability)
{
    RELEASE_ASSERT(!capability);
    modules().callBodyOfModuleEnteredByLoader(m_module);
}

void ModuleRecord::executeAsync(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(status() == Status::Evaluating || status() == Status::EvaluatingAsync);
    ASSERT(hasTLA());

    JSValue result = modules().callBodyOfModuleEnteredByLoader(m_module);
    RETURN_IF_EXCEPTION(scope, void());
    auto* promise = dynamicDowncast<JSPromise>(result);
    if (!promise) {
        promise = JSPromise::resolvedPromise(globalObject, result);
        RETURN_IF_EXCEPTION(scope, void());
    }
    promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::AsyncModuleExecutionDone, nullptr, this);
}

void ModuleRecord::appendDynamicStarTargets(StarExportTargets& targets)
{
    auto scope = DECLARE_THROW_SCOPE(vm());
    Modules& modules = this->modules();
    if (!modules.data(m_module).hasDynamicStarExport())
        return;
    for (const ModuleRequestData& request : modules.table().requests(m_module)) {
        if (!request.isDynamicStarExport)
            continue;
        AbstractModuleRecord* target = modules.recordForResolution(request.targetModule());
        RETURN_IF_EXCEPTION(scope, void());
        targets.append(target);
    }
}

auto ModuleRecord::resolveOwnExport(JSGlobalObject* globalObject, const Identifier& exportName, StarExportTargets& starTargets) -> Resolution
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Modules& modules = this->modules();
    const ModuleData& data = modules.data(m_module);
    bool isDefault = exportName == vm.propertyNames->defaultKeyword;
    Resolution absent = isDefault ? Resolution::error() : Resolution::notFound();

    retain();
    if (data.capturesExports()) {
        bool hasExport = modules.hasCapturedExport(m_module, exportName);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        return hasExport ? Resolution { Resolution::Type::Resolved, this, exportName } : absent;
    }

    // The search below for an indirect export has a resolveSet of its own, so a circular request is found here.
    if (m_resolveSet.contains(exportName.impl()))
        return Resolution::notFound();

    std::optional<unsigned> index = modules.table().findExport(m_module, exportName.impl());
    const ModuleExportData* entry = index ? &modules.table().exports(m_module)[*index] : nullptr;
    if (!isDefault && (!entry || entry->isFromStarExport)) {
        appendDynamicStarTargets(starTargets);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
    }
    if (!entry)
        return absent;

    // A Resolution has no phase, and its readers take *namespace* as the namespace object of phase Evaluation.
    if (entry->kind() == ModuleExportKind::Variable || (entry->kind() == ModuleExportKind::Namespace && entry->isDeferred))
        return { Resolution::Type::Resolved, this, exportName, *index };
    if (entry->kind() == ModuleExportKind::Namespace) {
        AbstractModuleRecord* record = modules.recordForResolution(entry->targetModule());
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        return { Resolution::Type::Resolved, record, vm.propertyNames->starNamespacePrivateName };
    }
    RELEASE_ASSERT(entry->kind() == ModuleExportKind::Indirect);
    const IndirectExportData& indirectExport = modules.table().indirectExport(*entry);
    AbstractModuleRecord* record = modules.recordForResolution(indirectExport.targetModule());
    RETURN_IF_EXCEPTION(scope, Resolution::error());
    if (!vm.isSafeToRecurse()) [[unlikely]] {
        throwException(globalObject, scope, createModuleGraphTooDeepError(globalObject));
        return Resolution::error();
    }
    m_resolveSet.append(exportName.impl());
    auto removeFromResolveSet = makeScopeExit([&] {
        m_resolveSet.removeLast();
    });
    RELEASE_AND_RETURN(scope, record->resolveExport(globalObject, modules.identifier(indirectExport.importName)));
}

void ModuleRecord::collectExportedNames(JSGlobalObject* globalObject, WTF::Vector<Identifier>& names, StarExportTargets& starTargets)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Modules& modules = this->modules();

    if (modules.data(m_module).capturesExports()) {
        RELEASE_AND_RETURN(scope, modules.forEachCapturedExport(m_module, [&](const Identifier& name, JSValue) {
            names.append(name);
        }));
    }
    for (const ModuleExportData& entry : modules.table().exports(m_module))
        names.append(modules.identifier(entry.name));
    RELEASE_AND_RETURN(scope, appendDynamicStarTargets(starTargets));
}

JSValue ModuleRecord::readExport(JSGlobalObject* globalObject, const Identifier& exportName, std::optional<unsigned> exportIndex)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (exportIndex)
        RELEASE_AND_RETURN(scope, modules().readExport(m_module, *exportIndex));
    // The namespace object holds the same value, and to make it reads every deferred export.
    if (modules().data(m_module).capturesExports()) {
        JSValue value = modules().capturedValue(m_module, exportName);
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            return value;
    }
    JSObject* namespaceObject = modules().moduleNamespace(m_module, ModulePhase::Evaluation);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, namespaceObject->get(globalObject, exportName));
}

JSObject* ModuleRecord::moduleNamespaceObject(JSGlobalObject*, ModulePhase phase)
{
    return modules().moduleNamespace(m_module, phase);
}

// Each importer of a host value has an External record of its own. The value says which of them are one module.
static const void* identityOfModule(const Modules& modules, ModuleID module)
{
    JSValue value = modules.data(module).isHostValue() ? modules.value(module) : JSValue();
    return value && value.isObject() ? static_cast<const void*>(value.asCell()) : &modules.data(module);
}

// The variable, or the module and the name in it. A deferred namespace object has no name.
using IdentityOfBinding = std::tuple<std::optional<SlotData>, const void*, UniquedStringImpl*>;

static IdentityOfBinding identityOfBinding(const Modules& modules, const AbstractModuleRecord::Resolution& resolution)
{
    ModuleID module = uncheckedDowncast<ModuleRecord>(resolution.moduleRecord)->module();
    UniquedStringImpl* name = resolution.localName.impl();
    if (resolution.compiledExportIndex) {
        const ModuleExportData& entry = modules.table().exports(module)[*resolution.compiledExportIndex];
        if (entry.kind() == ModuleExportKind::Variable)
            return { entry.variable, nullptr, nullptr };
        RELEASE_ASSERT(entry.kind() == ModuleExportKind::Namespace && entry.isDeferred);
        return { std::nullopt, identityOfModule(modules, entry.targetModule()), nullptr };
    }
    // export { x } from "./c.cjs" and export * from "./c.cjs" end at one binding. An export entry names it by the variable that holds the captured value.
    for (const SyntheticExportData& syntheticExport : modules.table().syntheticExports(module)) {
        if (syntheticExport.hasVariable && modules.identifier(syntheticExport.name).impl() == name)
            return { syntheticExport.variable, nullptr, nullptr };
    }
    return { std::nullopt, identityOfModule(modules, module), name };
}

bool ModuleRecord::isSameBinding(const Resolution& first, const Resolution& second)
{
    auto* firstRecord = uncheckedDowncast<ModuleRecord>(first.moduleRecord);
    if (&firstRecord->m_instance != &uncheckedDowncast<ModuleRecord>(second.moduleRecord)->m_instance)
        return false;
    Modules& modules = firstRecord->modules();
    return identityOfBinding(modules, first) == identityOfBinding(modules, second);
}

void ModuleRecord::gatherAsynchronousTransitiveDependencies(OrderedHashSet<AbstractModuleRecord*>& result, VisitedModuleRecords& seen)
{
    modules().gatherAsynchronousTransitiveDependencies(m_module, result, seen);
}

bool ModuleRecord::readyForSyncExecution(VisitedModuleRecords& seen)
{
    return modules().readyForSyncExecution(m_module, seen);
}

const ClassInfo ModuleNamespace::s_info = { "Module"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleNamespace) };

Structure* ModuleNamespace::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::preventExtensionsTransition(vm, Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info()));
}

ModuleNamespace::ModuleNamespace(VM& vm, Structure* structure, ModuleID module, ModulePhase phase, const DynamicExports& dynamicExports)
    : Base(vm, structure)
    , m_dynamicExportNames(dynamicExports.names, WriteBarrierEarlyInit)
    , m_capturedValues(dynamicExports.capturedValues, WriteBarrierEarlyInit)
    , m_liveSources(dynamicExports.liveSources, WriteBarrierEarlyInit)
    , m_module(module)
    , m_isDeferred(phase == ModulePhase::Defer)
    , m_mayNeedEvaluation(m_isDeferred && modules().data(module).hasExportTable())
{
}

ModuleNamespace* ModuleNamespace::create(VM& vm, Structure* structure, ModuleID module, ModulePhase phase, const DynamicExports& dynamicExports)
{
    ModuleNamespace* object = new (NotNull, allocateCell<ModuleNamespace>(vm)) ModuleNamespace(vm, structure, module, phase, dynamicExports);
    object->finishCreation(vm);
    return object;
}

Modules& ModuleNamespace::modules() const
{
    return *modulesOf(realm());
}

std::span<const ModuleExportData> ModuleNamespace::exports() const
{
    return modules().table().exports(m_module);
}

bool ModuleNamespace::isAmbiguousExport(unsigned index) const
{
    return exports()[index].isFromStarExport && modules().isExportAmbiguous(m_module, index);
}

void ModuleNamespace::ensureOwnStructure(VM& vm)
{
    // A Structure with properties may be shared with another namespace object.
    if (hasOwnStructure() || isValidOffset(structure()->maxOffset()))
        return;
    setStructure(vm, createStructure(vm, realm(), structure()->storedPrototype()));
    m_hasOwnStructure = true;
}

template<typename Visitor>
void ModuleNamespace::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_dynamicExportNames);
    visitor.append(thisObject->m_capturedValues);
    visitor.append(thisObject->m_liveSources);
}

DEFINE_VISIT_CHILDREN(ModuleNamespace);

std::optional<unsigned> ModuleNamespace::find(UniquedStringImpl* name) const
{
    if (isSymbolLikeKey(name))
        return std::nullopt;
    std::optional<unsigned> index = modules().table().findExport(m_module, name);
    return index && !isAmbiguousExport(*index) ? index : std::nullopt;
}

bool ModuleNamespace::tryReadExports(JSGlobalObject* globalObject, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values)
{
    if (m_isDeferred || m_mayNeedEvaluation || m_dynamicExportNames)
        return false;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Modules& modules = this->modules();
    std::span<const ModuleExportData> entries = exports();
    for (unsigned index = 0; index < entries.size(); ++index) {
        if (isAmbiguousExport(index))
            continue;
        JSValue value = modules.readExport(m_module, index);
        RETURN_IF_EXCEPTION(scope, true);
        names.append(modules.identifier(entries[index].name).impl());
        values.appendWithCrashOnOverflow(value);
    }
    return true;
}

bool ModuleNamespace::isSymbolLikeKey(UniquedStringImpl* name) const
{
    return name->isSymbol() || (m_isDeferred && name == vm().propertyNames->then.impl());
}

bool ModuleNamespace::evaluateIfNeeded(JSGlobalObject* globalObject)
{
    if (!m_mayNeedEvaluation)
        return true;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    modules().evaluateSynchronously(m_module, SynchronousEvaluationReason::DeferredNamespace);
    RETURN_IF_EXCEPTION(scope, false);
    m_mayNeedEvaluation = false;
    return true;
}

bool ModuleNamespace::isDynamicExport(UniquedStringImpl* name) const
{
    JSCellButterfly* names = m_dynamicExportNames.get();
    if (!names || isSymbolLikeKey(name))
        return false;
    unsigned from = 0;
    unsigned to = names->length();
    while (from < to) {
        unsigned middle = from + (to - from) / 2;
        const StringImpl* candidate = asString(names->get(middle))->tryGetValueImpl();
        if (candidate == name)
            return true;
        if (codePointCompare(StringView(*candidate), StringView(*name)) < 0)
            from = middle + 1;
        else
            to = middle;
    }
    return false;
}

JSValue ModuleNamespace::readDynamicExport(JSGlobalObject* globalObject, PropertyName name)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Identifier identifier = Identifier::fromUid(globalObject->vm(), name.uid());
    if (JSObject* liveSources = m_liveSources.get()) {
        JSValue source = Modules::storedValue(globalObject, liveSources, identifier);
        RETURN_IF_EXCEPTION(scope, { });
        if (source)
            RELEASE_AND_RETURN(scope, asObject(source)->get(globalObject, name));
    }
    if (JSValue captured = Modules::storedValue(globalObject, m_capturedValues.get(), identifier))
        return captured;
    // A deferred export: the host makes the value when somebody first asks for it, as it does in plain Bun, and the module keeps it from then on.
    RELEASE_AND_RETURN(scope, modules().capturedValue(m_module, identifier));
}

bool ModuleNamespace::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    using InternalMethodType = PropertySlot::InternalMethodType;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    InternalMethodType method = slot.internalMethodType();
    slot.setIsTaintedByOpaqueObject();
    if (method != InternalMethodType::VMInquiry && !thisObject->isSymbolLikeKey(name.uid())) {
        thisObject->evaluateIfNeeded(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    }
    std::optional<unsigned> index = thisObject->find(name.uid());
    if (!index && !thisObject->isDynamicExport(name.uid())) {
        if (name != vm.propertyNames->toStringTagSymbol)
            return false;
        slot.setValue(thisObject, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete, jsNontrivialString(vm, thisObject->m_isDeferred ? "Deferred Module"_s : "Module"_s));
        return true;
    }
    if (method == InternalMethodType::VMInquiry) {
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::None), jsUndefined());
        return false;
    }
    JSValue value = jsUndefined();
    if (method != InternalMethodType::HasProperty) {
        value = index ? thisObject->modules().readExport(thisObject->m_module, *index) : thisObject->readDynamicExport(globalObject, name);
        RETURN_IF_EXCEPTION(scope, false);
    }
    slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), value);
    return true;
}

bool ModuleNamespace::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    return getOwnPropertySlot(object, globalObject, Identifier::from(globalObject->vm(), index), slot);
}

void ModuleNamespace::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    thisObject->evaluateIfNeeded(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    Modules& modules = thisObject->modules();
    std::span<const ModuleExportData> entries = thisObject->exports();
    JSCellButterfly* dynamicNames = thisObject->m_dynamicExportNames.get();
    unsigned dynamicNameCount = dynamicNames ? dynamicNames->length() : 0;
    for (unsigned ownIndex = 0, dynamicIndex = 0; ownIndex < entries.size() || dynamicIndex < dynamicNameCount;) {
        if (ownIndex < entries.size() && thisObject->isAmbiguousExport(ownIndex)) {
            ++ownIndex;
            continue;
        }
        Identifier name = ownIndex < entries.size() ? modules.identifier(entries[ownIndex].name) : Identifier();
        if (dynamicIndex == dynamicNameCount || (!name.isNull() && codePointCompare(StringView(name.string()), StringView(*asString(dynamicNames->get(dynamicIndex))->tryGetValueImpl())) < 0))
            ++ownIndex;
        else {
            name = asString(dynamicNames->get(dynamicIndex++))->toIdentifier(globalObject);
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (mode == DontEnumPropertiesMode::Exclude) {
            PropertySlot slot(thisObject, PropertySlot::InternalMethodType::GetOwnProperty);
            bool hasProperty = getOwnPropertySlot(thisObject, globalObject, name, slot);
            RETURN_IF_EXCEPTION(scope, void());
            if (!hasProperty)
                continue;
        }
        names.add(name);
    }
    if (!names.includeSymbolProperties())
        return;
    // The order of JSModuleNamespaceObject::getOwnPropertyNames, where Symbol.toStringTag is the first property of the Structure.
    unsigned index = 0;
    std::optional<PropertyNameArrayBuilder> structureNames;
    if (isValidOffset(thisObject->structure()->maxOffset())) [[unlikely]] {
        structureNames.emplace(vm, PropertyNameMode::StringsAndSymbols, names.privateSymbolMode());
        thisObject->structure()->getPropertyNamesFromStructure(vm, *structureNames, mode);
        for (; index < structureNames->size() && !(*structureNames)[index].isSymbol(); ++index)
            names.add((*structureNames)[index]);
    }
    if (mode == DontEnumPropertiesMode::Include)
        names.add(vm.propertyNames->toStringTagSymbol);
    for (; structureNames && index < structureNames->size(); ++index)
        names.add((*structureNames)[index]);
}

bool ModuleNamespace::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (name == vm.propertyNames->__esModule && !uncheckedDowncast<ModuleNamespace>(cell)->hasExport(name.uid()))
        RELEASE_AND_RETURN(scope, Base::put(cell, globalObject, name, value, slot));
    return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
}

bool ModuleNamespace::putByIndex(JSCell*, JSGlobalObject* globalObject, unsigned, JSValue, bool shouldThrow)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
}

bool ModuleNamespace::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot&)
{
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    if (!thisObject->isSymbolLikeKey(name.uid()) && !thisObject->evaluateIfNeeded(globalObject))
        return false;
    return !thisObject->hasExport(name.uid()) && name != globalObject->vm().propertyNames->toStringTagSymbol;
}

bool ModuleNamespace::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    DeletePropertySlot slot;
    return deleteProperty(cell, globalObject, Identifier::from(globalObject->vm(), index), slot);
}

bool ModuleNamespace::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    if (name == vm.propertyNames->__esModule && descriptor.value().isTrue() && !thisObject->hasExport(name.uid())) {
        thisObject->setESModuleMarker(true);
        return true;
    }
    PropertyDescriptor current;
    bool isCurrentDefined = object->getOwnPropertyDescriptor(globalObject, name, current);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isCurrentDefined || !thisObject->hasExport(name.uid()))
        RELEASE_AND_RETURN(scope, validateAndApplyPropertyDescriptor(globalObject, nullptr, name, false, descriptor, isCurrentDefined, current, shouldThrow));

    // The steps of JSModuleNamespaceObject::defineOwnProperty.
    if (descriptor.isAccessorDescriptor())
        return typeError(globalObject, scope, shouldThrow, NamespaceBindingToAccessorError);
    if (descriptor.writablePresent() && !descriptor.writable())
        return typeError(globalObject, scope, shouldThrow, NamespaceBindingToNonWritableError);
    if (descriptor.enumerablePresent() && !descriptor.enumerable())
        return typeError(globalObject, scope, shouldThrow, NamespaceBindingToNonEnumerableError);
    if (descriptor.configurablePresent() && descriptor.configurable())
        return typeError(globalObject, scope, shouldThrow, NamespaceBindingToConfigurableError);
    if (!descriptor.value())
        return true;
    bool isSame = sameValue(globalObject, descriptor.value(), current.value());
    RETURN_IF_EXCEPTION(scope, false);
    return isSame || typeError(globalObject, scope, shouldThrow, NamespaceBindingValueError);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
