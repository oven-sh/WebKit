/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTImageOfCellScope.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTErrorMessages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTImagesBelow.h"
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
#include "JSModuleNamespaceObject.h"
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
        visitor.appendUnbarriered(m_extensibleNamespaceStructure);
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

Modules* Modules::modulesWithEntryPointOfRealm()
{
    for (Instance* instance = m_instance.globalObject->aotInstance(); instance; instance = instance->above) {
        Modules* modules = instance->state->modules.get();
        if (modules && modules->m_firstModuleRequestedByHost && !modules->m_otherImageHoldsEntryPointOfRealm)
            return modules;
    }
    return nullptr;
}

void Modules::noteFirstRequestFromHost(ModuleID module)
{
    m_otherImageHoldsEntryPointOfRealm = !!modulesWithEntryPointOfRealm();
    m_firstModuleRequestedByHost = module;
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
    if (!record && m_filesToLoadAgain.get(module.index()))
        return ModuleStatus::Linked;
    RELEASE_ASSERT(record);
    ModuleStatus status = statusOf(*record);
    return status == ModuleStatus::Unlinked && !data(module).needsLink() ? ModuleStatus::Linked : status;
}

// Code that requires a file reads its stored status, and asks the run time where that says the status is kept elsewhere.
bool Modules::wouldBeEvicted(ModuleID module)
{
    ModuleID file = fileOf(module);
    return status(file) == ModuleStatus::Evaluating || hasRun(file);
}

void Modules::evict(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (!wouldBeEvicted(file))
        return;
    leaveToHost(file);
    m_admitsCodeOutsideImage = true;
    if (data(file).kind != ModuleKind::CommonJs)
        return;
    // A file that is still running keeps the mark when it ends.
    m_filesToLoadAgain.set(file.index());
    setStoredStatus(file, StoredModuleStatus::InRecord);
}

void Modules::leaveToHostUnlessItHasRun(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (status(file) >= ModuleStatus::Evaluating || isEvicted(file) || hasRun(file))
        return;
    leaveToHost(file);
    m_admitsCodeOutsideImage = true;
    if (data(file).kind != ModuleKind::CommonJs)
        return;
    m_filesToLoadAgain.set(file.index());
    setStoredStatus(file, StoredModuleStatus::InRecord);
}

// A copy from disk names the files of the program as the compiled file does, and by import() any of them.
void Modules::leaveToHost(ModuleID file)
{
    m_evictedFiles.set(file.index());
    m_runsCopyFromDisk = true;
}

bool Modules::importsEvictedFile(ModuleID module, BitVector& seen)
{
    ModuleID file = fileOf(module);
    if (m_evictedFiles.get(file.index()))
        return true;
    if (m_otherImageHasEvictedFiles && data(file).isUncompiledCode() && namesFileEvictedFromOtherImage(file))
        return true;
    if (seen.set(file.index()) || !isLoadedWithRequests(file) || status(file) >= ModuleStatus::Evaluating || hasRun(file))
        return false;
    return std::ranges::any_of(m_table.imports(file), [&](const ModuleRequestData& request) {
        return importsEvictedFile(request.targetModule(), seen);
    });
}

// To the build of this image a file of another image is code that it has not seen, under the path that the other image has it under.
bool Modules::namesFileEvictedFromOtherImage(ModuleID external)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    Identifier key = resolvedKey(external);
    // Whoever loads the module hears that the name stands for nothing.
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return false;
    }
    for (Instance* instance = m_instance.globalObject->aotInstance(); instance; instance = instance->above) {
        Modules* modules = instance->state->modules.get();
        if (!modules || modules == this || modules->m_evictedFiles.isEmpty())
            continue;
        if (std::optional<ModuleID> found = modules->m_table.findByPath(key.string()); found && modules->isEvicted(*found))
            return true;
    }
    return false;
}

// A module that has not run yet is bound to what its imports name when it runs. Behind an eviction that is an instance from disk, which only code from disk imports.
bool Modules::isLeftToHost(ModuleID module)
{
    if (m_evictedFiles.isEmpty() && !m_otherImageHasEvictedFiles)
        return false;
    // The host has it anyway.
    if (data(module).isUncompiledCode())
        return false;
    BitVector seen;
    if (!importsEvictedFile(module, seen))
        return false;
    leaveToHost(fileOf(module));
    return true;
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
    if (m_instance.vm->synchronousModuleQueue() || m_innermostEvaluation || m_rootWhoseFilesRun || m_rootWhoseFilesRanAhead || m_fileWhoseJobsWaitForLoader || !m_levelsOfLoads.isEmpty())
        return;
    // The host loads the graph of the entry point.
    if (!m_firstModuleRequestedByHost || status(*m_firstModuleRequestedByHost) < ModuleStatus::Evaluating)
        return;
    auto hasLoadInFlight = [](const Modules& modules) {
        return modules.m_loadsInFlight.containsIf([](const LoadInFlight& load) { return load.promise->status() == JSPromise::Status::Pending; });
    };
    if (hasLoadInFlight(*this))
        return;
    // A load of another image of the realm looks for the records of this one in the registry of the loader, and a record that is retired leaves the registry.
    if (m_instance.program->sharesRealms()) [[unlikely]] {
        for (Instance* instance = m_instance.globalObject->aotInstance(); instance; instance = instance->above) {
            if (Modules* modules = instance->state->modules.get(); modules && modules != this && hasLoadInFlight(*modules))
                return;
        }
    }
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
    JSGlobalObject* realm = settled->realm();
    // Whichever image runs now, the load may be of another, and may be what the others waited for.
    if (Instance* first = realm->aotInstance(); first && first->above) [[unlikely]] {
        Instance::forEachOf(realm, [](Instance& instance) {
            if (Modules* modules = instance.state->modules.get())
                modules->retireRecordsOfSettledLoads();
        });
        return;
    }
    if (Modules* modules = modulesOf(realm))
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
    Identifier name = identifier(requiredName.name);
    bool isDefault = name == m_instance.vm->propertyNames->defaultKeyword;
    if (const ExportStatementData* statement = m_table.findExportStatement(requiredName)) {
        // As CyclicModuleRecord::initializeEnvironment() says it.
        if (isDefault)
            throwSyntaxError(realm, scope, "export default cannot be used with export *"_s);
        else
            throwException(realm, scope, createExportStatementFindsNothingError(realm, ProgramStrings::text(m_table.programTables(), statement->exportNameString()), ProgramStrings::text(m_table.programTables(), statement->specifierString())));
        return;
    }
    String path = m_table.pathText(m_table.fileBehindFace(requiredName.exporterModule()).value_or(requiredName.exporterModule()));
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

// Compiled code reads an export off the object too, where the build did not bind the read: `require("./list.js").LIST`. So does a built-in function that it calls: `Object.values(list)`.
static bool isReadForCompiledCode(VM& vm)
{
    if (isTopFrameCompiled(vm))
        return true;
    CallFrame* top = vm.topCallFrame;
    return top && !top->isNativeCalleeFrame() && !top->codeBlock() && isCallerCompiled(vm, top, vm.topEntryFrame);
}

// For whoever is not compiled code of the image: a binding of a file from disk, a read off the namespace object.
JSValue Modules::readExportFromOutside(ModuleID module, unsigned index)
{
    const ModuleExportData& moduleExport = m_table.exports(module)[index];
    if (moduleExport.isClosedToOutside && (moduleExport.variable == ModuleExportData::noSlot || !isReadForCompiledCode(*m_instance.vm))) [[unlikely]] {
        JSGlobalObject* realm = m_instance.globalObject;
        auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
        throwException(realm, scope, createExportClosedToOutsideError(realm, identifier(moduleExport.name).string(), modulePath(module), askerOfModule()));
        return { };
    }
    return readExport(module, index);
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

// The body of a module that is mocked before it has run never runs. What the mock does not give reads as undefined, as a name does that the mock's own module lacks in plain Bun.
void Modules::fillExportsOfBodyThatNeverRuns(ModuleID module)
{
    for (const ModuleExportData& moduleExport : m_table.exports(module)) {
        if (moduleExport.kind() == ModuleExportKind::Variable && moduleExport.isReplaceable && !ProgramStatics::readVariableWithoutAllocating(m_instance, moduleExport.variable))
            ProgramStatics::writeVariable(m_instance, moduleExport.variable, jsUndefined());
    }
}

bool Modules::tryReplaceExport(ModuleID module, UniquedStringImpl* name, JSValue value)
{
    BitVector seenBehindStars;
    return tryReplaceExport(module, name, value, seenBehindStars);
}

bool Modules::tryReplaceExport(ModuleID module, UniquedStringImpl* name, JSValue value, BitVector& seenBehindStars)
{
    // A chain of re-exports ends at the module that declares the variable. A ring of them names nothing.
    for (unsigned steps = 0; steps <= m_table.size(); ++steps) {
        if (m_instance.state->sharesRealm && data(module).isUncompiledCode()) [[unlikely]] {
            if (std::optional<bool> isReplaced = tryReplaceExportInImageBelow(module, name, value))
                return *isReplaced;
            return tryReplaceExportOfUncompiledModule(module, name, value);
        }
        std::optional<unsigned> index = m_table.findExport(module, name);
        if (!index) {
            if (data(module).hasDynamicStarExport() && tryReplaceExportBehindStars(module, name, value, seenBehindStars))
                return true;
            return tryReplaceCapturedExport(module, name, value);
        }
        const ModuleExportData& moduleExport = m_table.exports(module)[*index];
        switch (moduleExport.kind()) {
        case ModuleExportKind::Variable:
            if (!moduleExport.isReplaceable)
                return false;
            // `export { add } from "a-commonjs-package"` exports the variable that the name of the package is kept in, and the name stands in two more places.
            if (const SyntheticExportPlace* origin = syntheticExportKeptIn(moduleExport.variable))
                return tryReplaceCapturedExport(origin->module, identifier(origin->name).impl(), value);
            ProgramStatics::writeVariable(m_instance, moduleExport.variable, value);
            return true;
        case ModuleExportKind::Namespace:
            return false;
        case ModuleExportKind::Indirect: {
            const IndirectExportData& indirectExport = m_table.indirectExport(moduleExport);
            module = indirectExport.targetModule();
            name = identifier(indirectExport.importName).impl();
            break;
        }
        }
    }
    return false;
}

// `export * from` a module whose names the build did not see, as a package of an image below. The table of the module that says so lists none of them.
bool Modules::tryReplaceExportBehindStars(ModuleID module, UniquedStringImpl* name, JSValue value, BitVector& seenBehindStars)
{
    if (name == m_instance.vm->propertyNames->defaultKeyword.impl() || seenBehindStars.set(module.index()))
        return false;
    for (const ModuleRequestData& request : m_table.requests(module)) {
        if (request.isDynamicStarExport && tryReplaceExport(request.targetModule(), name, value, seenBehindStars))
            return true;
    }
    return false;
}

// The chain goes on in the image that holds the file. Nothing where the module stands for no file of an image below.
std::optional<bool> Modules::tryReplaceExportInImageBelow(ModuleID external, UniquedStringImpl* name, JSValue value)
{
    String path = m_table.pathText(external);
    for (Instance* below = m_instance.globalObject->aotInstance(); below != &m_instance; below = below->above) {
        ModuleTable table(below->tables());
        std::optional<ModuleID> file = table.findByPath(path);
        if (!file)
            continue;
        RunningImageScope imageOfFile(m_instance.vm->aotState(), below->program);
        // Whoever imports a CommonJS file that has a face reads what the face has captured.
        return below->state->modules->tryReplaceExport(table.faceOfFile(*file).value_or(*file), name, value);
    }
    return std::nullopt;
}

// A module of the library, which the first image holds and the loader hands out, or code that no image holds. Every image reads its exports off the record of the engine, as plain Bun does.
bool Modules::tryReplaceExportOfUncompiledModule(ModuleID external, UniquedStringImpl* name, JSValue value)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* namespaceObject = uncompiledNamespace(external, ModulePhase::Evaluation);
    RETURN_IF_EXCEPTION(scope, false);
    if (auto* ofImage = dynamicDowncast<ModuleNamespace>(namespaceObject))
        RELEASE_AND_RETURN(scope, AOT::tryReplaceExport(ofImage, name, value));
    auto* ofEngine = dynamicDowncast<JSModuleNamespaceObject>(namespaceObject);
    if (!ofEngine)
        return false;
    RELEASE_AND_RETURN(scope, ofEngine->overrideExportValue(realm, Identifier::fromUid(vm, name), value));
}

bool Modules::hasBegunToLoad(ModuleID module) const
{
    // Compiled code requires a CommonJS file without a word to the run time: no mock can wait for that.
    if (data(module).kind == ModuleKind::CommonJs)
        return true;
    ModuleStatus current = status(module);
    if ((current != ModuleStatus::Unlinked && current != ModuleStatus::Linked) || didCaptureExports(module))
        return true;
    return data(module).isESModuleOfProgram() && wasLoadedWithGraph(module);
}

// A mock names a path. The loader of `bun run` looks there as JSModuleLoader::registryEntry() does: for the file as no attribute asks for it, then as a type of the host's own, then as JSON. It holds only what has been loaded.
std::optional<ModuleID> Modules::formThatMockFinds(StringView path, std::optional<ModuleID> file) const
{
    if (file && hasBegunToLoad(*file))
        return file;
    Vector<ModuleID, 2> forms = m_table.formsThatAttributesAskFor(path);
    std::optional<ModuleID> json;
    for (ModuleID form : forms) {
        if (!hasBegunToLoad(form))
            continue;
        if (!m_table.pathText(form).endsWith("?json"_s))
            return form;
        json = form;
    }
    if (json || file || forms.isEmpty())
        return json ? json : file;
    return forms[0];
}

// What the loader holds when the first mock of the module is made decides. The mock itself makes the namespace object of the image.
void Modules::willMockLibraryModule(ModuleID module)
{
    if (m_libraryModulesWithMock.get(module.index()))
        return;
    m_libraryModulesWithMock.set(module.index());
    m_libraryModulesThatMockReplaces.set(module.index(), !isHeldForImport(module));
}

// In plain Bun an ES module that was required has a module object in require.cache, and a mock stores there what its factory gave: the next require() answers that very object.
// One that was only imported has none, and require() goes on answering the namespace object.
void Modules::leaveOutWhatMockLacks(ModuleID module, JSObject* exportsOfMock)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    std::span<const ModuleExportData> entries = m_table.exports(module);
    BitVector leftOut(entries.size());
    for (unsigned index = 0; index < entries.size(); ++index) {
        bool isAnswered = exportsOfMock->hasOwnProperty(m_instance.globalObject, identifier(entries[index].name));
        RETURN_IF_EXCEPTION(scope, void());
        if (!isAnswered)
            leftOut.quickSet(index);
    }
    m_exportsLeftOutByMocks.set(module, WTF::move(leftOut));
}

void Modules::mockWhatRequireAnswered(ModuleID module, JSObject* exports)
{
    if (LoadedModule* loaded = loadedIfExists(module); loaded && loaded->requiredValue)
        loaded->requiredValue.set(*this, exports);
}

void Modules::deferMock(ModuleID module)
{
    m_modulesWithMockToRun.set(module.index());
    if (data(module).isOfLibrary())
        willMockLibraryModule(module);
    if (!data(module).isOfLibrary() && m_table.hasFilePath(module))
        willMock(module);
}

void Modules::runMocksBelow(ModuleID root)
{
    if (m_modulesWithMockToRun.isEmpty()) [[likely]]
        return;
    // The walk does nothing where it comes to none of them.
    MocksBelowRoot mocks = mocksBelow(root);
    bool mayComeToOne = false;
    for (size_t module : m_modulesWithMockToRun)
        mayComeToOne |= !isMocked(ModuleID(module)) || !mocks.hides(ModuleID(module));
    if (!mayComeToOne)
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    BitVector reached(m_table.size());
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (reached.quickSet(module.index()))
            continue;
        if (m_modulesWithMockToRun.get(module.index())) {
            m_modulesWithMockToRun.clear(module.index());
            host().runMock(m_instance.globalObject, moduleIDForHost(*m_instance.program, module));
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (isMocked(module))
            continue;
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (!request.isHoisted)
                work.append(request.targetModule());
        }
    }
}

// A module that has not run gets what the files that it imports export once they are loaded anew. Its code asks nothing of a face that has run.
void Modules::loadEvictedFilesBelow(ModuleID root)
{
    if (m_filesToLoadAgain.isEmpty()) [[likely]]
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    BitVector reached(m_table.size());
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (reached.quickSet(module.index()))
            continue;
        if (ModuleID file = fileOf(module); m_filesToLoadAgain.get(file.index())) {
            loadEvictedFile(file);
            RETURN_IF_EXCEPTION(scope, void());
            continue;
        }
        // What has run keeps what it was linked with.
        if (status(module) > ModuleStatus::Linked)
            continue;
        std::span<const ModuleRequestData> requests = m_table.requests(module);
        for (size_t index = requests.size(); index--;) {
            if (!requests[index].isRequire)
                work.append(requests[index].targetModule());
        }
    }
}

// Asked where a test replaces an export, which few realms do: the list is made at the first one.
auto Modules::syntheticExportKeptIn(SlotData variable) -> const SyntheticExportPlace*
{
    if (!m_syntheticExportsByVariable) {
        m_syntheticExportsByVariable.emplace();
        for (unsigned index = 0; index < m_table.size(); ++index) {
            for (const SyntheticExportData& syntheticExport : m_table.syntheticExports(ModuleID(index))) {
                if (syntheticExport.hasVariable && syntheticExport.isReplaceable)
                    m_syntheticExportsByVariable->add(syntheticExport.variable.encoded(), SyntheticExportPlace { ModuleID(index), syntheticExport.name });
            }
        }
    }
    auto found = m_syntheticExportsByVariable->find(variable.encoded());
    return found == m_syntheticExportsByVariable->end() ? nullptr : &found->value;
}

// ES modules import a CommonJS file through its face.
ModuleID Modules::moduleWithExportsOf(ModuleID module) const
{
    std::optional<ModuleID> face = data(module).kind == ModuleKind::CommonJs ? m_table.faceOfFile(module) : std::nullopt;
    return face.value_or(module);
}

bool Modules::mayBeMocked(ModuleID module) const
{
    return data(module).mayBeMocked();
}

// A name that the loader fills stands in three places: the variable that importers read, the table that a later link takes it from, and the namespace object.
bool Modules::tryReplaceCapturedExport(ModuleID module, UniquedStringImpl* name, JSValue value)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto isNamed = [&](const SyntheticExportData& syntheticExport) {
        return identifier(syntheticExport.name).impl() == name;
    };
    std::span<const SyntheticExportData> syntheticExports = m_table.syntheticExports(module);
    auto found = std::ranges::find_if(syntheticExports, isNamed);
    // No code binds a name that is not listed: it is read off the namespace object alone.
    bool isListed = found != syntheticExports.end();
    if (isListed ? !found->isReplaceable : !data(m_table.fileBehindFace(module).value_or(module)).mayBeMocked())
        return false;
    // An ES module captures nothing: a name that it does not export stays out of it.
    if (!isListed && data(module).hasExportTable() && !m_table.fileBehindFace(module))
        return false;

    Identifier key = Identifier::fromUid(vm, name);
    JSObject* captured = capturedExports(module);
    RETURN_IF_EXCEPTION(scope, false);
    captured->putDirectMayBeIndex(realm, key, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (JSObject* namespaceObject = namespaceIfCreated(module, ModulePhase::Evaluation)) {
        uncheckedDowncast<ModuleNamespace>(namespaceObject)->replaceCapturedValue(realm, key, value);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (isListed && found->hasVariable)
        ProgramStatics::writeVariable(m_instance, found->variable, value);
    return true;
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

JSObject* Modules::createNamespace(ModuleID module, ModulePhase phase, bool shouldPreventExtensions)
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
    // The names of a file are taken once, for whoever imports it and for the host. A face that captures nothing lists only what the importers of the image bind.
    if (moduleData.kind == ModuleKind::CommonJs) {
        if (std::optional<ModuleID> face = m_table.faceOfFile(module); face && data(*face).capturesExports())
            RELEASE_AND_RETURN(scope, moduleNamespace(*face, phase, shouldPreventExtensions));
    }
    // A load by a name that the build does not follow comes to a face that captures nothing. The file has the names.
    if (std::optional<ModuleID> file = m_table.fileBehindFace(module); file && !moduleData.capturesExports()) {
        JSObject* namespaceOfFile = moduleNamespace(*file, phase, shouldPreventExtensions);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (auto* compiled = dynamicDowncast<ModuleNamespace>(namespaceOfFile))
            compiled->setFace(module);
        return namespaceOfFile;
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

    bool isRequired = phase == ModulePhase::Evaluation && m_modulesWithMarkOfRequire.get(module.index());
    if (phase == ModulePhase::Evaluation && m_modulesWithExtensibleNamespace.get(module.index()))
        shouldPreventExtensions = false;
    Structure*& structure = shouldPreventExtensions ? m_namespaceStructure : m_extensibleNamespaceStructure;
    if (!structure) {
        structure = ModuleNamespace::createStructure(vm, realm, realm->moduleNamespaceObjectStructure()->storedPrototype(), shouldPreventExtensions);
        m_storedReferences.recordAll();
    }
    ModuleNamespace* namespaceObject = ModuleNamespace::create(vm, structure, module, phase, exportObjects);
    if (isRequired && !namespaceObject->hasExport(vm.propertyNames->__esModule.impl()))
        namespaceObject->setESModuleMarker(true);
    setNamespace(module, phase, namespaceObject);
    return namespaceObject;
}

bool Modules::mayExport(ModuleID module, UniquedStringImpl* name) const
{
    const ModuleData& moduleData = data(module);
    if (moduleData.kind != ModuleKind::SourceText || moduleData.capturesExports() || moduleData.hasDynamicStarExport())
        return true;
    return !!m_table.findExport(module, name);
}

JSObject* Modules::moduleNamespace(ModuleID module, ModulePhase phase, bool shouldPreventExtensions)
{
    if (JSObject* namespaceObject = namespaceIfCreated(module, phase))
        return namespaceObject;
    return createNamespace(module, phase, shouldPreventExtensions);
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
    // A package may name one file for require() and another for an import. `require.resolve` goes by the first, and the loader below by the second.
    if (!isURL && data(module).isUncompiledCode()) {
        JSValue resolved = resolveDynamic(specifier, *data(module).referrerModule(), form);
        RETURN_IF_EXCEPTION(scope, nullptr);
        return resolved.isString() ? asString(resolved) : specifier;
    }

    Identifier key;
    {
        DisallowVMEntry disallowVMEntry(vm);
        key = loader()->resolve(m_instance.globalObject, specifier, pathString(*data(module).referrerModule()), nullptr, true);
    }
    if (scope.exception()) [[unlikely]] {
        TRY_CLEAR_EXCEPTION(scope, specifier);
        if (!data(module).isUncompiledCode())
            return specifier;
        // The run would load the code from outside the image, so whether it is there is a question for the run. The resolver of the host words its error for whoever asks.
        JSValue resolved = resolveDynamic(specifier, *data(module).referrerModule(), form);
        RETURN_IF_EXCEPTION(scope, nullptr);
        return resolved.isString() ? asString(resolved) : specifier;
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
    if (!isForRequire) {
        // `import` passes the resolved key to the loader, which resolves it again.
        DisallowVMEntry disallowVMEntry(vm);
        key = loader()->resolve(realm, jsString(vm, key.string()), pathString(failure.importerModule()), nullptr, true);
    }
    RETURN_IF_EXCEPTION(scope, void());
    if (host().createLoadError) {
        JSValue error = host().createLoadError(realm, key.string());
        RETURN_IF_EXCEPTION(scope, void());
        if (error) {
            throwException(realm, scope, error);
            return;
        }
    }
    // The build did not find the module, so the program does not hold it, whatever the resolver finds now.
    if (host().createModuleNotFoundError) {
        JSValue error = host().createModuleNotFoundError(realm, specifier, importer, isForRequire);
        RETURN_IF_EXCEPTION(scope, void());
        throwException(realm, scope, error);
        return;
    }
    throwException(realm, scope, createModuleMissingAtBuildError(realm, specifier, importer, isForRequire ? "MODULE_NOT_FOUND"_s : "ERR_MODULE_NOT_FOUND"_s));
}

// A job runs whichever image ran last. Its number of a module names the image too, as a number that the host holds does.
static JSValue moduleForJob(ModuleID module)
{
    return jsNumber(moduleIDForHost(*Image::current(), module).index());
}

// Calls the functor while the image of the module is the one that runs.
static void withModuleOfJob(JSGlobalObject* realm, JSValue moduleForJob, const ScopedLambda<void(Modules&, ModuleID)>&);
static void continueImportOfJob(Modules&, JSPromise*, ModuleID, JSValue referrerAsyncOrder);

static void queueImportJob(JSGlobalObject* realm, JSPromise* importPromise, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    constexpr uint8_t unusedPayload = 0;
    realm->queueMicrotask(realm->vm(), InternalMicrotask::CompiledModuleImport, unusedPayload, importPromise, moduleForJob(module), referrerAsyncOrder ? jsNumber(static_cast<double>(*referrerAsyncOrder)) : jsUndefined());
}

// A synchronous module load runs the jobs it diverts in its own realm, so the realm of a job is the one its import promise was created in.
void runCompiledModuleImport(JSGlobalObject*, JSValue importPromise, JSValue module, JSValue referrerAsyncOrder)
{
    auto* promise = uncheckedDowncast<JSPromise>(importPromise);
    withModuleOfJob(promise->realm(), module, [&](Modules& modules, ModuleID imported) {
        if (!modules.continueCountedImport(promise, module, referrerAsyncOrder))
            continueImportOfJob(modules, promise, imported, referrerAsyncOrder);
    });
}

static void continueImportOfJob(Modules& modules, JSPromise* promise, ModuleID imported, JSValue referrerAsyncOrder)
{
    modules.continueImport(promise, imported, referrerAsyncOrder.isUndefined() ? std::nullopt : std::optional(static_cast<int64_t>(referrerAsyncOrder.asNumber())));
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
        withModuleOfJob(promise->realm(), module, [&](Modules& modules, ModuleID imported) {
            modules.finishImportOfEvaluatedModule(promise, imported, error);
        });
}

void runCompiledModuleImportSettled(JSGlobalObject*, JSValue importPromise, JSValue resolution, JSValue module, JSPromise::Status status)
{
    auto* promise = uncheckedDowncast<JSPromise>(importPromise);
    RELEASE_ASSERT(status != JSPromise::Status::Pending);
    withModuleOfJob(promise->realm(), module, [&](Modules& modules, ModuleID imported) {
        if (modules.isImportedByLoader(imported))
            queueStepOfImport(promise, module, numberOfJobsBehindEvaluation - 1, status == JSPromise::Status::Rejected ? resolution : JSValue());
        else if (status == JSPromise::Status::Rejected)
            promise->reject(promise->vm(), resolution);
        else
            modules.resolveWithNamespace(promise, imported);
    });
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

    runMocksBelow(module);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    loadEvictedFilesBelow(module);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    auto fetchParameters = JSModuleLoader::fetchParametersFromImportOptions(realm, options);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (!fetchParameters)
        return fetchParameters.error();
    if (isLeftToHost(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, importUncompiled(keyWithoutForm(fileOf(module)), referrer, WTF::move(*fetchParameters)));
    if (hostAnswersInPlaceOf(module, LoadByName::Import)) [[unlikely]]
        RELEASE_AND_RETURN(scope, importUncompiled(moduleKey(module), referrer, WTF::move(*fetchParameters)));
    if (!data(module).isUncompiledCode()) {
        // The build has bound the call by what its text says of the attributes. The image may hold the file as what the options ask for.
        if (const ScriptFetchParameters* attributes = fetchParameters->get()) {
            ModuleID file = fileOf(module);
            Identifier key = keyWithoutForm(file);
            if (std::optional<ModuleID> form = findWithAttributes(key, attributes))
                module = m_table.faceOfFile(*form).value_or(*form);
            else
                noteFormAnsweredFirst(key, *attributes, file);
        }
        RELEASE_AND_RETURN(scope, startImport(module, asyncEvaluationOrder(referrer)));
    }

    const Identifier& key = resolvedKey(module);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (std::optional<ModuleID> found = findWithAttributes(key, fetchParameters->get()))
        RELEASE_AND_RETURN(scope, startImport(*found, asyncEvaluationOrder(referrer)));
    RELEASE_AND_RETURN(scope, importUncompiled(key, *data(module).referrerModule(), WTF::move(*fetchParameters)));
}

// `mock.module("node:util/types", factory)`: the image holds no code of the module, and the loader of the host has what stands in its place.
// `mock.module("undici", factory)`: the image holds the code. As under `bun run`, the mock stands in place of a module that no import has loaded, with its own names alone, and is laid over one that an import has.
// There require() takes a name that starts with `node:` to the registry and asks no loader (overridableRequire in CommonJS.ts), so a mock of `node:os` is nothing to it.
bool Modules::hostAnswersInPlaceOf(ModuleID module, LoadByName way)
{
    const ModuleData& replaced = data(module);
    bool isNative = replaced.kind == ModuleKind::Runtime;
    // `bun:wrap` is an ES module.
    bool isCodeOfLibrary = replaced.kind == ModuleKind::RuntimeSource || (replaced.kind == ModuleKind::SourceText && replaced.isOfLibrary());
    if (!isNative && (!isCodeOfLibrary || !replaced.mayBeMocked()))
        return false;
    String name = m_table.pathText(module);
    if (!host().replacesNativeModule || !host().replacesNativeModule(m_instance.globalObject, name))
        return false;
    // And its transpiler writes the global `Bun` for `require("bun")`.
    if (way == LoadByName::Require && (name.startsWith("node:"_s) || name == "bun"_s))
        return false;
    return (isNative && way == LoadByName::Import) || m_libraryModulesThatMockReplaces.get(module.index());
}

// An import had loaded the module when it was mocked. There the loader holds it as an ES module, so a require() that asks the loader gets the namespace object, with the mock in it.
bool Modules::requireAnswersNamespaceWithMock(ModuleID module)
{
    if (!m_libraryModulesWithMock.get(module.index()) || m_libraryModulesThatMockReplaces.get(module.index()))
        return false;
    String name = m_table.pathText(module);
    return !name.startsWith("node:"_s) && name != "bun"_s && host().replacesNativeModule && host().replacesNativeModule(m_instance.globalObject, name);
}

std::optional<ModuleID> Modules::findByName(const Identifier& key)
{
    std::optional<ModuleID> found = m_table.findByPath(key.string());
    if (found && hostAnswersInPlaceOf(*found, LoadByName::Import))
        return std::nullopt;
    return found;
}

// The loader of `bun run` knows every form of a file by the path of the file.
Identifier Modules::keyWithoutForm(ModuleID file)
{
    if (data(file).kind != ModuleKind::Data)
        return moduleKey(file);
    String path = m_table.pathText(file);
    size_t mark = path.reverseFind('?');
    return mark == notFound ? moduleKey(file) : Identifier::fromString(*m_instance.vm, path.left(mark));
}

// An import statement has asked for its form before any import() runs. Only the types of the host's own share a place in the loader.
std::optional<ModuleID> Modules::findFormThatStatementLoaded(const Identifier& key, const ScriptFetchParameters& fetchParameters, std::optional<ModuleID> asked) const
{
    if (fetchParameters.type() != ScriptFetchParameters::Type::HostDefined)
        return std::nullopt;
    Vector<ModuleID, 2> forms = m_table.formsThatAttributesAskFor(key.string());
    forms.removeAllMatching([&](ModuleID form) {
        return data(form).kind != ModuleKind::Data || m_table.pathText(form).endsWith("?json"_s);
    });
    if (forms.isEmpty() || (forms.size() == 1 && asked == forms[0]))
        return std::nullopt;
    for (unsigned index = 0; index < m_table.size(); ++index) {
        ModuleID importer(index);
        // A module that needs no link counts as linked before anybody has asked for it.
        bool wasLoaded = status(importer) > ModuleStatus::Linked || (m_entryPointIsLinked && data(importer).isLinkedWithEntryPoint());
        if (data(importer).kind != ModuleKind::SourceText || !wasLoaded)
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (forms.contains(request.targetModule()))
                return request.targetModule();
        }
    }
    return std::nullopt;
}

// The host does not find every form by its type. The form that the build bound then answers, also for the types that are asked for later.
void Modules::noteFormAnsweredFirst(const Identifier& key, const ScriptFetchParameters& fetchParameters, ModuleID file)
{
    ModuleMapKey keyOfLoader { key.impl(), fetchParameters.type() };
    if (data(file).kind == ModuleKind::Data && !loader()->moduleMap().contains(keyOfLoader))
        m_formsAnsweredFirst.add(keyOfLoader, FormAnsweredFirst { key, file });
}

// The loader of `bun run` has forgotten the file, so the type that is asked for next answers.
void Modules::forgetFormAnsweredFirst(ModuleID file)
{
    if (m_formsAnsweredFirst.isEmpty())
        return;
    Identifier key = keyWithoutForm(file);
    m_formsAnsweredFirst.removeIf([&](auto& entry) {
        return entry.value.name == key;
    });
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
    std::optional<ModuleID> module = moduleOfImage(*m_instance.program, host().findModuleWithAttributes(m_instance.globalObject, key.string(), *fetchParameters));
    if (std::optional<ModuleID> loaded = findFormThatStatementLoaded(key, *fetchParameters, module))
        module = loaded;
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

    runMocksBelow(module);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (m_innermostFilesBeingRun) [[unlikely]] {
        handGraphsToLoader();
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (m_innermostFilesBeingRun->isAheadOfStepsOfLoader) {
            handSynchronousLoadToLoader(*m_innermostFilesBeingRun);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        }
    }
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
                queueStepOfImport(promise, moduleForJob(module), std::size(jobsOfImportOfEvaluatedModule));
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
        queueStepOfImport(import.promise.get(), jsNumber(moduleIDForHost(*m_instance.program, file).index()), jobsUntilLoaded + std::size(jobsOfImportOfEvaluatedModule) - 1 - skippedJobs, error);
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
        // Where the loader makes the record of a file inline, what the file throws rejects at once: no step of this import() comes to the file ahead of that.
        ModuleRecord* made = recordIfExists(module);
        bool isMadeInline = made && loader()->isMakingRecordInline(made->moduleKey());
        record = isCommonJSFileOrFace(module) ? recordOfCommonJSFile(module, isMadeInline ? LoadOfFile::IsAskedFor : LoadOfFile::IsRootOfImport) : exposeRecord(module, RecordHolder::Loader);
    }
    JSPromise* loaded = scope.exception() ? nullptr : loader()->requestImportModuleForKey(m_instance.globalObject, record->moduleKey(), Identifier(), nullptr, nullptr, false, referrerAsyncOrder.value_or(-1), record);
    setIsAskingHost(wasAskingHost);
    RETURN_IF_EXCEPTION(scope, nullptr);
    didStartLoad(loaded, module);
    return loaded;
}

bool Modules::isAnythingBeingLoaded() const
{
    if (m_instance.vm->synchronousModuleQueue() || m_rootWhoseFilesRun || m_rootWhoseFilesRanAhead || m_fileWhoseJobsWaitForLoader || !m_levelsOfLoads.isEmpty())
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

void Modules::runJobsThatAreDue(VM& vm, MicrotaskQueue& jobs)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    // What they queue goes to the queue of the realm. As in MicrotaskQueue::drain(), only a termination stops the remaining jobs.
    jobs.allOf([&](const QueuedTask& job) {
        if (job.globalObject()->microtaskRunnability() == QueuedTaskResult::Discard)
            return true;
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
    // The calls came ahead of any require() that is under way, so their steps are jobs, not steps of its load.
    SetForScope noSynchronousLoad(vm.m_synchronousModuleQueueSetAside, vm.m_synchronousModuleQueue);
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
        for (const PlanOfLoad::Found& found : import.plan->level((job - jobsFromRequestToGenerator) / jobInWhichLoaderAsksRoot)) {
            if (isNativeModule(found.module))
                nativeModules.append(found.module);
        }
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
        queueStepOfImport(importPromise, moduleForJob(module), numberOfJobsBehindEvaluation);
        return;
    case JSPromise::Status::Rejected:
        evaluation->markAsHandled();
        queueStepOfImport(importPromise, moduleForJob(module), numberOfJobsBehindEvaluation, evaluation->result());
        return;
    case JSPromise::Status::Pending:
        evaluation->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledModuleImportSettled, importPromise, moduleForJob(module));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The steps of the loader find nothing to load, to link or to run, and no other load meets them: the module has no step of its own left that a synchronous load could take over.
bool Modules::loaderOnlyHandsOverNamespace(ModuleID module) const
{
    if (status(module) != ModuleStatus::Evaluated || isCommonJSFileOrFace(module) || m_instance.vm->synchronousModuleQueue() || m_fileWhoseJobsWaitForLoader || m_rootWhoseFilesRanAhead)
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
        evaluation->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledModuleImportSettled, importPromise, moduleForJob(module));
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
    VM& vm = *m_instance.vm;
    SetForScope frameOfImportThatAsksLoader(vm.aotState().frameOfImportThatAsksLoader, vm.topCallFrame);
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
    // What the build found in an image below its own it names by the path that the image has it under. Nothing is left to resolve.
    Identifier* keyOfFileBelow = nullptr;
    if (VMState& state = m_instance.vm->aotState(); state.hasSeveralImages) [[unlikely]] {
        Vector<Identifier>& keys = state.parts[m_instance.program->position()].keysOfFilesInImagesBelow;
        if (keys.isEmpty())
            keys.grow(m_table.size());
        keyOfFileBelow = &keys[external.index()];
        if (!keyOfFileBelow->isNull()) {
            m_admitsCodeOutsideImage = true;
            return *keyOfFileBelow;
        }
    }
    if (!m_admitsCodeOutsideImage && !isText(m_table.pathText(external)))
        m_admitsCodeOutsideImage = true;
    LoadedModule& loaded = ensureLoaded(external);
    if (!loaded.resolvedKey.isNull())
        return loaded.resolvedKey;
    if (keyOfFileBelow) [[unlikely]] {
        String path = m_table.pathText(external);
        for (const Image* image = &m_instance.program->first(); image != m_instance.program; image = image->above()) {
            if (ModuleTable(image->programTables()).findByPath(path)) {
                *keyOfFileBelow = Identifier::fromString(*m_instance.vm, path);
                return *keyOfFileBelow;
            }
        }
    }
    Identifier key = loader()->resolve(m_instance.globalObject, pathString(external), pathString(*data(external).referrerModule()), nullptr, true);
    RETURN_IF_EXCEPTION(scope, loaded.resolvedKey);
    loaded.resolvedKey = WTF::move(key);
    // A file under a query is one more instance of the file.
    m_runsCopyFromDisk |= loaded.resolvedKey.string().contains('?');
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
    if (isEntryPointOfRealm(root))
        return false;
    if (vm.synchronousModuleQueue() || numberOfGraphsBeingLoaded() > 1)
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
        if (level->next && level->plan && !m_instance.vm->synchronousModuleQueue())
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
            evaluateImportedCommonJSFiles(module, JobsOfImportedFiles::WaitForLoader, ErrorOfImportedFile::IsLeftToLoader, &load);
            RETURN_IF_EXCEPTION(scope, void());
            // One of the files had the loader go on from here.
            if (!record->requestedModules().isEmpty())
                return;
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
            keptAlive.append(keyOfRequest(target));
            // The loader asks nobody for the record of a file that it has: one that it started itself, for an import or as the entry point. A file that a require() started is new to it.
            bool isKnownToLoader = data(fileOf(target)).isEntryPoint() || loader()->registryEntry(keptAlive.last());
            noteLoadOfFile(target, isKnownToLoader ? LoadOfFile::HasComeToFile : LoadOfFile::IsRequested);
            RETURN_IF_EXCEPTION(scope, void());
            noteRequestByLoader(target);
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

// Where a realm has several images, a number that the host holds names the image too: above the number stands the position of the image and one more.
// A number without that part is of the image that runs.
static constexpr unsigned shiftOfImageInModuleID = 24;

ModuleID moduleIDForHost(const Image& image, ModuleID module)
{
    if (!image.sharesRealms()) [[likely]]
        return module;
    RELEASE_ASSERT(!(module.index() >> shiftOfImageInModuleID));
    return ModuleID(module.index() | (image.position() + 1) << shiftOfImageInModuleID);
}

// Nothing where the host names a module of another image.
std::optional<ModuleID> moduleOfImage(const Image& image, std::optional<ModuleID> fromHost)
{
    unsigned positionAndOne = fromHost && *fromHost ? fromHost->index() >> shiftOfImageInModuleID : 0;
    if (!positionAndOne) [[likely]]
        return fromHost;
    if (positionAndOne - 1 != image.position())
        return std::nullopt;
    return ModuleID(fromHost->index() & ((1u << shiftOfImageInModuleID) - 1));
}

namespace {

// While it lives, the image of the module is the one that runs.
class ModuleFromHost {
    WTF_MAKE_NONCOPYABLE(ModuleFromHost);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    // Without a realm only the tables of the image can be asked.
    explicit ModuleFromHost(ModuleID fromHost)
        : m_module(fromHost)
    {
        if (const Image* image = takeImage()) {
            m_imageOfThread = Image::current();
            Image::setCurrentOfThread(image);
        }
    }
    ModuleFromHost(JSGlobalObject* realm, ModuleID fromHost)
        : m_module(fromHost)
    {
        if (const Image* image = takeImage())
            m_scope.emplace(realm->vm().aotState(), image);
    }
    ~ModuleFromHost()
    {
        if (m_imageOfThread)
            Image::setCurrentOfThread(m_imageOfThread);
    }

    operator ModuleID() const { return m_module; }

private:
    const Image* takeImage()
    {
        unsigned positionAndOne = m_module ? m_module.index() >> shiftOfImageInModuleID : 0;
        if (!positionAndOne) [[likely]]
            return nullptr;
        m_module = ModuleID(m_module.index() & ((1u << shiftOfImageInModuleID) - 1));
        const Image* image = Image::current()->atPosition(positionAndOne - 1);
        RELEASE_ASSERT(image);
        return image;
    }

    ModuleID m_module;
    const Image* m_imageOfThread { nullptr };
    std::optional<RunningImageScope> m_scope;
};

} // anonymous namespace

static void withModuleOfJob(JSGlobalObject* realm, JSValue moduleForJob, const ScopedLambda<void(Modules&, ModuleID)>& functor)
{
    ModuleFromHost module(realm, ModuleID(static_cast<uint32_t>(moduleForJob.asInt32())));
    functor(*modulesOf(realm), module);
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

static std::optional<ModuleID> findModuleInRunningImage(StringView publicPath)
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

// Calls the functor for each image of the thread's list, the first one first, with that image as the thread's.
// A module of the library may be in several. The first image has all of it: a later one has what its own code calls, and nobody else is to find that.
static void forEachImageOfThread(const Invocable<IterationStatus(const Image&)> auto& functor)
{
    const Image* running = Image::current();
    if (!running)
        return;
    if (!running->sharesRealms()) [[likely]] {
        functor(*running);
        return;
    }
    for (const Image* image = &running->first(); image; image = image->above()) {
        Image::setCurrentOfThread(image);
        IterationStatus status = functor(*image);
        Image::setCurrentOfThread(running);
        if (status == IterationStatus::Done)
            return;
    }
}

// Where images share realms, each realm asks for the same files anew, and a search goes through the tables of one image after the other.
// What is kept is checked against the image when it is read: another image may stand at that position by then.
namespace {
struct ModulesFoundInSeveralImages {
    Lock lock;
    UncheckedKeyHashMap<String, ModuleID> byPath WTF_GUARDED_BY_LOCK(lock);
};
}

std::optional<ModuleID> findModule(StringView publicPath)
{
    static NeverDestroyed<ModulesFoundInSeveralImages> modulesFound;
    const Image* running = Image::current();
    bool keepsWhatIsFound = running && running->sharesRealms();
    if (keepsWhatIsFound) {
        std::optional<ModuleID> kept;
        {
            Locker locker { modulesFound->lock };
            if (auto entry = modulesFound->byPath.find<StringViewHashTranslator>(publicPath); entry != modulesFound->byPath.end())
                kept = entry->value;
        }
        if (kept) {
            const Image* image = running->atPosition((kept->index() >> shiftOfImageInModuleID) - 1);
            ModuleID module(kept->index() & ((1u << shiftOfImageInModuleID) - 1));
            if (image) {
                ModuleTable table(image->programTables());
                if (module.index() < table.size()) {
                    table.willBeAskedForPath(module, publicPath.hash());
                    if (table.findByPath(publicPath) == module)
                        return kept;
                }
            }
        }
    }
    std::optional<ModuleID> found;
    forEachImageOfThread([&](const Image& image) {
        if (std::optional<ModuleID> module = findModuleInRunningImage(publicPath))
            found = moduleIDForHost(image, *module);
        return found ? IterationStatus::Done : IterationStatus::Continue;
    });
    if (found && keepsWhatIsFound && found->index() >> shiftOfImageInModuleID) {
        String copy = publicPath.toString().isolatedCopy();
        Locker locker { modulesFound->lock };
        modulesFound->byPath.set(WTF::move(copy), *found);
    }
    return found;
}

bool holdsFormThatAttributeAsksFor(StringView publicPath)
{
    bool holds = false;
    forEachImageOfThread([&](const Image& image) {
        holds = ModuleTable(image.programTables()).holdsFormThatAttributeAsksFor(publicPath);
        return holds ? IterationStatus::Done : IterationStatus::Continue;
    });
    return holds;
}

bool mayHoldPathStartingWith(StringView start)
{
    bool mayHold = false;
    forEachImageOfThread([&](const Image& image) {
        mayHold = start.isEmpty() || start.startsWith('#') || (image.mayHoldModulePathStartingWith(start) && ModuleTable(image.programTables()).holdsPathStartingWith(start));
        return mayHold ? IterationStatus::Done : IterationStatus::Continue;
    });
    return mayHold;
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
        while (std::optional<ModuleID> outer = evaluator ? table->data(*evaluator).evaluatorModule() : std::nullopt)
            evaluator = outer;
        String path = evaluator ? URL::fileURLWithFileSystemPath(table->pathText(*evaluator)).string() : table->data(module).registryID() ? table->displayPathText(module) : table->pathText(module);
        return PathOfModule { &(path.is8Bit() ? StringImpl::createStaticStringImpl(path.span8()) : StringImpl::createStaticStringImpl(path.span16())).leakRef(), !!evaluator };
    }).iterator->value;
}

String modulePath(ModuleID moduleOfHost)
{
    ModuleFromHost module(moduleOfHost);
    return pathOfModule(module).path;
}

String sourceProviderURL(ModuleID moduleOfHost)
{
    ModuleFromHost module(moduleOfHost);
    PathOfModule found = pathOfModule(module);
    return found.isEvaluatedText ? emptyString() : String(found.path);
}

bool entryPointIsCommonJS(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    modules = modules ? modules->modulesWithEntryPointOfRealm() : nullptr;
    return modules && modules->data(*modules->firstModuleRequestedByHost()).kind == ModuleKind::CommonJs;
}

bool isESModule(ModuleID moduleOfHost)
{
    ModuleFromHost module(moduleOfHost);
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::SourceText;
}

bool isNativeModule(ModuleID moduleOfHost)
{
    ModuleFromHost module(moduleOfHost);
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::Runtime;
}

static void throwLoaderChangeRefused(JSGlobalObject* realm, LoaderChange change, StringView changed)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwException(realm, scope, createLoaderChangeRefusedError(realm, change, changed));
}

static void evictFromItsImage(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (!modules || modules->data(module).isOfLibrary())
        return;
    if (!modules->mayBeEvicted(module)) {
        if (modules->wouldBeEvicted(module))
            throwLoaderChangeRefused(realm, LoaderChange::TakeOutOfRequireCache, modules->table().pathText(module));
        return;
    }
    modules->forgetFormAnsweredFirst(module);
    modules->evict(module);
}

// The cache knows a file by its path. Several images may hold the file, the image of a test file and one that it shares with others, and the copy that has run may be in any of them.
void didDeleteRequireCacheEntry(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    const Image* running = Image::current();
    if (!running || !running->sharesRealms()) [[likely]]
        return evictFromItsImage(realm, moduleOfHost);
    String path;
    {
        ModuleFromHost module(moduleOfHost);
        path = currentModuleTable()->pathText(module);
    }
    Vector<ModuleID, 4> copies;
    forEachImageOfThread([&](const Image& image) {
        if (std::optional<ModuleID> copy = findModuleInRunningImage(path))
            copies.append(moduleIDForHost(image, *copy));
        return IterationStatus::Continue;
    });
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    for (ModuleID copy : copies) {
        evictFromItsImage(realm, copy);
        RETURN_IF_EXCEPTION(scope, );
    }
    for (Instance* instance = realm->aotInstance(); instance; instance = instance->above) {
        if (Modules* modules = instance->state->modules.get())
            modules->didEvictFromOtherImage();
    }
}

bool willLoadFileInAnotherWay(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (!modules || modules->data(module).isOfLibrary())
        return true;
    if (!modules->mayBeEvicted(module))
        return false;
    modules->leaveToHostUnlessItHasRun(module);
    return true;
}

// A module with nothing to run keeps no state, and neither does one that compiled code runs as a part of the only module that imports it.
// What a module imports has run before the module has.
bool Modules::hasRun(ModuleID module)
{
    if (status(module) == ModuleStatus::Evaluated)
        return true;
    if (!equalSpans(m_statusesWhenImportsWereFollowed.span(), std::span<const uint8_t>(m_storedStatuses))) {
        m_statusesWhenImportsWereFollowed = Vector<uint8_t>(std::span<const uint8_t>(m_storedStatuses));
        m_modulesThatRanWithImporters = BitVector(m_table.size());
        Vector<ModuleID, 64> toFollow;
        for (unsigned index = 0; index < m_table.size(); ++index) {
            if (status(ModuleID(index)) == ModuleStatus::Evaluated)
                toFollow.append(ModuleID(index));
        }
        while (!toFollow.isEmpty()) {
            for (const ModuleRequestData& request : m_table.requests(toFollow.takeLast())) {
                if (request.isDeferred || request.isRequire)
                    continue;
                if (!m_modulesThatRanWithImporters.set(request.target()))
                    toFollow.append(request.targetModule());
            }
        }
    }
    return m_modulesThatRanWithImporters.get(module.index());
}

bool isEvaluatedFile(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (!modules)
        return false;
    const ModuleData& data = modules->data(module);
    return !data.isOfLibrary() && !data.isFaceOfFile() && modules->table().hasFilePath(module) && !modules->isEvicted(module) && modules->hasRun(module);
}

static void throwLoadByNameRefused(JSGlobalObject*, StringView name, LoadByName);

JSObject* requireCacheEntry(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    ASSERT(isEvaluatedFile(realm, module));
    Modules* modules = modulesOf(realm);
    // The entry of a CommonJS file hands out `module.exports` in one piece, and a build for files that the host runs has closed what is no entry point. An ES module shows its exports one by one: readExportFromOutside().
    if (modules->data(module).kind == ModuleKind::CommonJs && hasFlag(Image::current()->header().options.admitted(), AdmittedCode::FilesRunByHost) && modules->refusesRequestFromHost(module)) [[unlikely]] {
        throwLoadByNameRefused(realm, modulePath(module), LoadByName::Require);
        return nullptr;
    }
    return modules->requireCacheEntry(module);
}

void forEachEvaluatedFile(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    if (!realm->aotInstance())
        return;
    Instance::forEachOf(realm, [&](Instance&) {
        Modules* modules = modulesOf(realm);
        if (!modules)
            return;
        const ModuleTable& table = modules->table();
        for (unsigned index = 0; index < table.size(); ++index) {
            if (isEvaluatedFile(realm, ModuleID(index)))
                functor(table.pathText(ModuleID(index)));
        }
    });
}

bool hostAnswersInPlaceOf(JSGlobalObject* realm, ModuleID moduleOfHost, LoadByName way)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->hostAnswersInPlaceOf(module, way);
}

bool leavesModuleToHost(JSGlobalObject* realm, ModuleID moduleOfHost, StartedBy startedBy)
{
    ModuleFromHost module(realm, moduleOfHost);
    if (Modules* modules = modulesOf(realm))
        return startedBy == StartedBy::Host ? modules->isEvicted(module) : modules->isLeftToHost(module);
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).isOfLibrary();
}

void forEachModule(const ScopedLambda<IterationStatus(ModuleID)>& functor)
{
    forEachImageOfThread([&](const Image& image) {
        ModuleTable table(image.programTables());
        for (unsigned index = 0; index < table.size(); ++index) {
            if (table.hasFilePath(ModuleID(index)) && functor(moduleIDForHost(image, ModuleID(index))) == IterationStatus::Done)
                return IterationStatus::Done;
        }
        return IterationStatus::Continue;
    });
}

void forEachFilePathStartingWith(StringView start, const ScopedLambda<IterationStatus(const String&)>& functor)
{
    Image::forEachPublishedImage([&](const Image& image) {
        if (!image.mayHoldModulePathStartingWith(start))
            return IterationStatus::Continue;
        return ModuleTable(image.programTables()).forEachFilePathStartingWith(start, functor);
    });
}

bool imagesHoldFile(std::span<const std::span<const uint8_t>> images, StringView path)
{
    struct ImagesRead {
        Lock lock;
        const uint8_t* last WTF_GUARDED_BY_LOCK(lock) { nullptr };
        std::unique_ptr<ImagesBelow> images WTF_GUARDED_BY_LOCK(lock);
    };
    static NeverDestroyed<ImagesRead> imagesRead;
    if (images.empty())
        return false;
    Locker locker { imagesRead->lock };
    if (imagesRead->last != images.back().data()) {
        imagesRead->images = ImagesBelow::tryCreate(images);
        imagesRead->last = images.back().data();
    }
    return imagesRead->images && imagesRead->images->holdsFile(path);
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

JSObject* parentModuleObject(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules ? modules->parentModuleObject(module) : nullptr;
}

bool forEachNewChild(JSGlobalObject* realm, ModuleID moduleOfHost, const ScopedLambda<void(JSObject*)>& functor)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules && modules->forEachNewChild(module, functor);
}

JSValue moduleExports(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules ? modules->commonJSExports(module) : jsUndefined();
}

void setModuleExports(JSGlobalObject* realm, ModuleID moduleOfHost, JSValue exports)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    ASSERT(modules->data(module).kind == ModuleKind::CommonJs);
    modules->setCommonJSExports(module, exports);
}

AskerOfModule::AskerOfModule(JSGlobalObject* realm, const String& path)
    : m_modules(modulesOf(realm))
    , m_outer(m_modules ? m_modules->exchangeAskerOfModule(&path) : nullptr)
{
}

AskerOfModule::~AskerOfModule()
{
    if (m_modules)
        m_modules->exchangeAskerOfModule(m_outer);
}

static void throwLoadByNameRefused(JSGlobalObject* realm, StringView name, LoadByName form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    Modules* modules = modulesOf(realm);
    throwException(realm, scope, createLoadByNameRefusedError(realm, name, form == LoadByName::Import ? "ERR_MODULE_NOT_FOUND"_s : "MODULE_NOT_FOUND"_s, modules ? modules->askerOfModule() : StringView()));
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
    throwException(realm, scope, createModuleInOtherRealmError(realm, modulePath(module)));
    return nullptr;
}

void willAnswerWithModule(JSGlobalObject* realm, ModuleID moduleOfHost, LoadByName form)
{
    ModuleFromHost module(realm, moduleOfHost);
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

AbstractModuleRecord* moduleRecord(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesForEvaluation(realm, module);
    if (!modules)
        return nullptr;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    // The loader asks for a module when it loads it. Where it goes on to what the module imports, each file runs when the loader comes to it.
    if (!modules->isAskingHost()) {
        modules->handCountedImportsToLoader();
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    // The loader asks for all that a module imports before it comes to the first of them. ModuleRecord::didLoad() hears where it comes to a file.
    if (modules->isCommonJSFileOrFace(module))
        RELEASE_AND_RETURN(scope, modules->recordOfCommonJSFile(module, Modules::LoadOfFile::IsRequested));
    // A module of the image asks through the loader. Code that is not compiled binds what it imports to the record.
    if (!modules->isAskingHost())
        modules->didImportFromUncompiledCode(module);
    RELEASE_AND_RETURN(scope, modules->exposeRecord(module, modules->isAskingHost() ? Modules::RecordHolder::Loader : Modules::RecordHolder::Anyone));
}

bool isCompiledModuleRecord(AbstractModuleRecord* record)
{
    return record->inherits<ModuleRecord>();
}

JSPromise* importModule(JSGlobalObject* realm, ModuleID moduleOfHost, std::optional<int64_t> referrerAsyncOrder)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesForEvaluation(realm, module);
    return modules ? modules->startImport(module, referrerAsyncOrder, Modules::ImportCaller::Host) : nullptr;
}

void evaluateModuleSynchronously(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    if (Modules* modules = modulesForEvaluation(realm, module))
        modules->evaluateSynchronously(module, SynchronousEvaluationReason::Require);
}

JSObject* moduleNamespace(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOrThrow(realm, module);
    return modules ? modules->moduleNamespace(module, Modules::ModulePhase::Evaluation) : nullptr;
}

bool mayExport(JSGlobalObject* realm, ModuleID moduleOfHost, UniquedStringImpl* name)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return !modules || modules->refusesRequestFromHost(module) || modules->mayExport(module, name);
}

JSValue requireModule(JSGlobalObject* realm, ModuleID moduleOfHost, std::optional<ModuleID> referrer)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesForEvaluation(realm, module);
    if (!modules)
        return { };
    // The loader asks for code of the program, which may lie in another image or on disk. The library gets its own modules from the registry.
    if (modules->requireAnswersNamespaceWithMock(module)) [[unlikely]]
        return modules->moduleNamespace(module, ModulePhase::Evaluation);
    return modules->require(module, moduleOfImage(*Image::current(), referrer).value_or(ModuleID()));
}

void willRequireModule(JSGlobalObject* realm, ModuleID moduleOfHost, std::optional<ModuleID> referrer, JSObject* moduleObjectOfReferrer)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (modules && !moduleOfImage(*Image::current(), referrer))
        modules->recordRequireFromOtherImage(module, moduleObjectOfReferrer);
}

void didRequireModule(JSGlobalObject* realm, ModuleID moduleOfHost, ModuleID referrerOfHost)
{
    if (moduleOfHost.index() >> shiftOfImageInModuleID == referrerOfHost.index() >> shiftOfImageInModuleID) [[likely]]
        return;
    ModuleFromHost referrer(realm, referrerOfHost);
    if (Modules* modules = modulesOf(realm))
        modules->recordRequireOfOtherImage(referrer, moduleOfHost);
}

std::pair<Modules*, ModuleID> Modules::ownerOfRequiredModule(ModuleID required)
{
    unsigned positionAndOne = required.index() >> shiftOfImageInModuleID;
    if (!positionAndOne) [[likely]]
        return { this, required };
    for (Instance* instance = m_instance.globalObject->aotInstance(); instance; instance = instance->above) {
        if (instance->program->position() == positionAndOne - 1)
            return { instance->state->modules.get(), ModuleID(required.index() & ((1u << shiftOfImageInModuleID) - 1)) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The host's require() takes no entry that is falsy.
void willSetRequireCacheEntry(JSGlobalObject* realm, ModuleID moduleOfHost, JSValue entry)
{
    ModuleFromHost module(realm, moduleOfHost);
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

bool tryReplaceExport(JSCell* moduleNamespace, UniquedStringImpl* name, JSValue value)
{
    auto* object = uncheckedDowncast<ModuleNamespace>(moduleNamespace);
    ImageOfCellScope imageOfCell(object);
    return modulesOf(object->realm())->tryReplaceExport(object->module(), name, value);
}

bool mayBeMocked(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->mayBeMocked(module);
}

void mockExport(JSGlobalObject* realm, ModuleID moduleOfHost, UniquedStringImpl* name, JSValue value)
{
    ModuleFromHost module(realm, moduleOfHost);
    if (Modules* modules = modulesOf(realm))
        modules->tryReplaceExport(modules->moduleWithExportsOf(module), name, value);
}

bool hasBegunToLoad(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return !modules || modules->hasBegunToLoad(module);
}

std::optional<ModuleID> formThatMockFinds(JSGlobalObject* realm, StringView path, std::optional<ModuleID> file)
{
    if (!file) {
        Modules* modules = modulesOf(realm);
        return modules ? modules->formThatMockFinds(path, file) : file;
    }
    ModuleFromHost fileInImage(realm, *file);
    Modules* modules = modulesOf(realm);
    if (!modules)
        return file;
    std::optional<ModuleID> found = modules->formThatMockFinds(path, ModuleID(fileInImage));
    return found ? std::optional(moduleIDForHost(*Image::current(), *found)) : std::nullopt;
}

Vector<ModuleID, 2> formsThatAttributesAskFor(JSGlobalObject* realm, StringView path)
{
    // Whoever asks may be code of the library, which runs in the first image, about a file of the last.
    Vector<ModuleID, 2> forms;
    if (!modulesOf(realm))
        return forms;
    forEachImageOfThread([&](const Image& image) {
        for (ModuleID form : ModuleTable(image.programTables()).formsThatAttributesAskFor(path))
            forms.append(moduleIDForHost(image, form));
        return IterationStatus::Continue;
    });
    return forms;
}

void deferMock(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    if (Modules* modules = modulesOf(realm))
        modules->deferMock(module);
}

void runMocksBelow(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    if (Modules* modules = modulesOf(realm))
        modules->runMocksBelow(module);
}

void willMockModule(JSGlobalObject* realm, ModuleID moduleOfHost, JSObject* exports)
{
    ModuleFromHost module(realm, moduleOfHost);
    // Bun finds the module in the image, whatever the realm.
    Modules* found = modulesOf(realm);
    if (!found)
        return;
    Modules& modules = *found;
    // Compiled code names objects of a module of the library, so its body runs: the mock stands under its names alone. The body runs now, or it would store over them.
    if (modules.data(module).isOfLibrary() || !modules.table().hasFilePath(module)) {
        modules.willMockLibraryModule(module);
        modules.evaluateSynchronously(module, SynchronousEvaluationReason::Require);
        return;
    }
    modules.willMock(module);
    if (modules.data(module).kind != ModuleKind::CommonJs) {
        if (ModuleStatus status = modules.status(module); status == ModuleStatus::Unlinked || status == ModuleStatus::Linked) {
            modules.fillExportsOfBodyThatNeverRuns(module);
            modules.leaveOutWhatMockLacks(module, exports);
        }
        modules.mockWhatRequireAnswered(module, exports);
        return;
    }
    // Compiled code calls the body of a CommonJS file itself, and asks for the status first.
    ModuleStatus status = modules.status(module);
    bool hasRun = status != ModuleStatus::Unlinked && status != ModuleStatus::Linked;
    if (!hasRun)
        modules.markEvaluated(module);
    modules.setCommonJSExports(module, exports);
    modules.exportWhatMockNamesAlone(module, hasRun);
}

void didMockModule(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    if (Modules* modules = modulesOf(realm))
        modules->answerRequireWithNamespaceOfMock(module);
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    EvaluationContext context { stack, index, referrerAsyncOrder < 0 ? std::nullopt : std::optional(referrerAsyncOrder), dynamicImportPromise };
    modules().innerModuleEvaluation(m_module, context);
    // The module may be evaluated and off the stack by now, so a request to terminate waits for the next check.
    RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, invalidIndex);
    return context.index;
}

void ModuleRecord::willLoadRequests(ModuleGraphLoadingState& load)
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    modules().appendRequestsForLoader(m_module, this, load);
}

ModuleGraphLoadingState* ModuleRecord::loadThatWaitsFor(const Identifier& key, ModuleGraphLoadingState* loadOfRequester, unsigned& jobsUntilLoadHearsOfFailure)
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    return modules().loadThatWaitsFor(m_module, key, loadOfRequester, jobsUntilLoadHearsOfFailure);
}

void ModuleRecord::didLoad()
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
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
        RunningImageScope imageOfRecord(vm.aotState(), compiled->m_instance.program);
        compiled->modules().didFailToLoadGraph(compiled->m_module);
        if (!scope.exception() && compiled->modules().hasDeferredLoads()) [[unlikely]]
            compiled->modules().didSettleLoad(state);
    });
    if (!scope.exception() && exception)
        throwException(globalObject, scope, exception);
}

std::optional<ModuleID> ModuleRecord::fileToWaitFor(JSGlobalObject* globalObject, const Identifier& key)
{
    std::optional<ModuleID> file;
    Instance::forEachOf(globalObject, [&](Instance& instance) {
        if (file)
            return;
        if (std::optional<ModuleID> found = modulesOf(instance).fileToWaitFor(key))
            file = moduleIDForHost(*instance.program, *found);
    });
    return file;
}

JSPromise* ModuleRecord::deferRequest(JSGlobalObject* globalObject, ModuleID fileOfSomeImage, ModuleLoadingContext* context, bool useImportMap)
{
    ModuleFromHost file(globalObject, fileOfSomeImage);
    return modulesOf(globalObject)->deferRequest(file, context, useImportMap);
}

void ModuleRecord::didRequestModule(JSGlobalObject* globalObject, const Identifier& key, JSCell* payload)
{
    Instance::forEachOf(globalObject, [&](Instance&) {
        Modules* modules = modulesOf(globalObject);
        if (modules && modules->hasDeferredLoads()) [[unlikely]]
            modules->didRequestModule(key, payload);
    });
}

void ModuleRecord::didSettleLoad(JSGlobalObject* globalObject, const ModuleGraphLoadingState& load)
{
    Instance::forEachOf(globalObject, [&](Instance&) {
        Modules* modules = modulesOf(globalObject);
        if (modules && modules->hasDeferredLoads()) [[unlikely]]
            modules->didSettleLoad(load);
    });
}

void ModuleRecord::didFailToMakeModule(JSGlobalObject* globalObject, const Identifier& key)
{
    Instance::forEachOf(globalObject, [&](Instance&) {
        Modules* modules = modulesOf(globalObject);
        if (modules && modules->hasDeferredLoads()) [[unlikely]]
            modules->didFailToMakeModule(key);
    });
}

void ModuleRecord::didLoseErrorOfModule(JSGlobalObject* globalObject, const Identifier& key)
{
    Instance::forEachOf(globalObject, [&](Instance&) {
        if (Modules* modules = modulesOf(globalObject))
            modules->didLoseErrorOfModule(key);
    });
}

void ModuleRecord::didReplaceRecordInRegistry()
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    modules().captureExportsAgain(m_module);
}

void ModuleRecord::willBeLoadedByImport()
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    modules().willBeLoadedByImport(m_module);
}

bool ModuleRecord::takeStepOfMainModule()
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    return !requestedModules().isEmpty() && modules().takeStepOfMainModule(m_module);
}

bool ModuleRecord::takeLoadPromiseBelowMainModule()
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    return !requestedModules().isEmpty() && modules().takeLoadPromiseBelowMainModule(m_module);
}

JSPromise* ModuleRecord::evaluateMainModule(JSPromise* evaluation)
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    return modules().evaluateMainModule(m_module, evaluation);
}

void ModuleRecord::didEvaluateModulesInJob(JSGlobalObject* globalObject)
{
    Instance::forEachOf(globalObject, [&](Instance&) {
        Modules* modules = modulesOf(globalObject);
        if (modules && modules->mainModuleWaitsForItsImport()) [[unlikely]]
            modules->runMainModuleThatWaited();
    });
}

void ModuleRecord::didFailModulesInJob(JSGlobalObject* globalObject)
{
    Instance::forEachOf(globalObject, [&](Instance&) {
        Modules* modules = modulesOf(globalObject);
        if (modules && modules->mainModuleWaitsForItsImport()) [[unlikely]]
            modules->failMainModuleThatWaited();
    });
}

void ModuleRecord::takeStatus()
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    if (!m_holdsStatus)
        modules().ensureRecord(m_module);
}

bool ModuleRecord::isRunningCommonJSFile()
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    return modules().isCommonJSFileOrFace(m_module) && modules().isImportedByLoader(m_module) && modules().status(m_module) == ModuleStatus::Evaluating;
}

bool ModuleRecord::setIsAskingHost(JSGlobalObject* globalObject, bool isAskingHost)
{
    bool wasAskingHost = false;
    Instance::forEachOf(globalObject, [&](Instance& instance) {
        wasAskingHost |= modulesOf(instance).setIsAskingHost(isAskingHost);
    });
    return wasAskingHost;
}

uint8_t ModuleRecord::willAskHostForRequest()
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    bool wasWalkingRequests = modules().setIsWalkingRequests(true);
    bool wasAskingHost = modules().setIsAskingHost(true);
    return static_cast<uint8_t>(wasAskingHost) | static_cast<uint8_t>(wasWalkingRequests) << 1;
}

void ModuleRecord::didAskHostForRequest(uint8_t flagsBefore)
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    modules().setIsAskingHost(flagsBefore & 1);
    modules().setIsWalkingRequests(flagsBefore & 2);
}

AbstractModuleRecord* ModuleRecord::recordOfRequest(JSGlobalObject* globalObject, const Identifier& key)
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    modules().captureSyntheticExports(m_module, visited);
}

unsigned ModuleRecord::innerModuleLinking(JSGlobalObject* globalObject, ModuleLinkingStack& stack, unsigned index, RefPtr<ScriptFetcher> scriptFetcher)
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    RELEASE_ASSERT(!capability);
    modules().callBodyOfModuleEnteredByLoader(m_module);
}

void ModuleRecord::executeAsync(JSGlobalObject* globalObject)
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
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

// An importer outside the image binds the names of a CommonJS file at run time, whatever the importers of the image do.
static bool takesNamesFromValue(const ModuleData& data)
{
    return data.capturesExports() || data.kind == ModuleKind::CommonJs;
}

auto ModuleRecord::resolveOwnExport(JSGlobalObject* globalObject, const Identifier& exportName, StarExportTargets& starTargets) -> Resolution
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Modules& modules = this->modules();
    const ModuleData& data = modules.data(m_module);
    bool isDefault = exportName == vm.propertyNames->defaultKeyword;
    Resolution absent = isDefault ? Resolution::error() : Resolution::notFound();

    retain();
    if (takesNamesFromValue(data)) {
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Modules& modules = this->modules();

    if (takesNamesFromValue(modules.data(m_module))) {
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (exportIndex)
        RELEASE_AND_RETURN(scope, modules().readExportFromOutside(m_module, *exportIndex));
    // The namespace object holds the same value, and to make it reads every deferred export.
    if (takesNamesFromValue(modules().data(m_module))) {
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    modules().gatherAsynchronousTransitiveDependencies(m_module, result, seen);
}

bool ModuleRecord::readyForSyncExecution(VisitedModuleRecords& seen)
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    return modules().readyForSyncExecution(m_module, seen);
}

const ClassInfo ModuleNamespace::s_info = { "Module"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleNamespace) };

Structure* ModuleNamespace::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype, bool shouldPreventExtensions)
{
    Structure* structure = Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    return shouldPreventExtensions ? Structure::preventExtensionsTransition(vm, structure) : structure;
}

ModuleNamespace::ModuleNamespace(VM& vm, Structure* structure, ModuleID module, ModulePhase phase, const DynamicExports& dynamicExports)
    : Base(vm, structure)
    , m_dynamicExportNames(dynamicExports.names, WriteBarrierEarlyInit)
    , m_capturedValues(dynamicExports.capturedValues, WriteBarrierEarlyInit)
    , m_liveSources(dynamicExports.liveSources, WriteBarrierEarlyInit)
    , m_module(module)
    , m_imagePosition(static_cast<uint8_t>(vm.aotState().image->position()))
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
    Instance* instance = realm()->aotInstance();
    for (unsigned position = m_imagePosition; position; --position)
        instance = instance->above;
    return modulesOf(*instance);
}

std::span<const ModuleExportData> ModuleNamespace::exports() const
{
    return modules().table().exports(m_module);
}

bool ModuleNamespace::isHiddenExport(unsigned index) const
{
    return (exports()[index].isFromStarExport && modules().isExportAmbiguous(m_module, index)) || modules().isLeftOutByMock(m_module, index);
}

void ModuleNamespace::ensureOwnStructure(VM& vm)
{
    // A Structure with properties may be shared with another namespace object.
    if (hasOwnStructure() || isValidOffset(structure()->maxOffset()))
        return;
    setStructure(vm, createStructure(vm, realm(), structure()->storedPrototype(), !structure()->isStructureExtensible()));
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
    return index && !isHiddenExport(*index) ? index : std::nullopt;
}

bool ModuleNamespace::tryReadExports(JSGlobalObject* globalObject, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values)
{
    if (m_isDeferred || m_mayNeedEvaluation || m_dynamicExportNames || isValidOffset(structure()->maxOffset()))
        return false;
    ImageOfCellScope imageOfCell(globalObject->vm(), this);
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Modules& modules = this->modules();
    std::span<const ModuleExportData> entries = exports();
    for (unsigned index = 0; index < entries.size(); ++index) {
        if (isHiddenExport(index))
            continue;
        JSValue value = modules.readExportFromOutside(m_module, index);
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

// JSModuleNamespaceObject keeps these keys as ordinary properties. Symbol.toStringTag is not stored here: getOwnPropertySlot() answers it.
bool ModuleNamespace::isOrdinaryKey(PropertyName name) const
{
    VM& vm = this->vm();
    if (name == vm.propertyNames->toStringTagSymbol)
        return false;
    return isSymbolLikeKey(name.uid()) || (name == vm.propertyNames->__esModule && !hasExport(name.uid()));
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

void ModuleNamespace::replaceCapturedValue(JSGlobalObject* globalObject, const Identifier& name, JSValue value)
{
    if (JSObject* capturedValues = m_capturedValues.get())
        capturedValues->putDirectMayBeIndex(globalObject, name, value);
    if (!m_hasOwnStructure)
        return;
    // The caches of every image file what they know of this object under its Structure: classifyNamespaceRead(). It never has that one again.
    // A property that is deleted may bring an earlier Structure back. A dictionary has no earlier one, and nothing of the kind is filed under it.
    VM& vm = globalObject->vm();
    if (isValidOffset(structure()->maxOffset()))
        convertToDictionary(vm);
    else
        setStructure(vm, createStructure(vm, realm(), structure()->storedPrototype(), !structure()->isStructureExtensible()));
}

JSValue ModuleNamespace::capturedValueWithoutAllocating(UniquedStringImpl* name) const
{
    JSObject* table = m_capturedValues.get();
    if (!table || m_mayNeedEvaluation || parseIndex(*name) || !isDynamicExport(name))
        return { };
    unsigned attributes;
    auto offsetIn = [&](JSObject* object) {
        return object->structure()->getWithoutMaterializing(name, attributes);
    };
    // readDynamicExport() asks the live sources first.
    if (JSObject* liveSources = m_liveSources.get()) {
        if (std::optional<PropertyOffset> offset = offsetIn(liveSources); !offset || isValidOffset(*offset))
            return { };
    }
    std::optional<PropertyOffset> offset = offsetIn(table);
    return offset && isValidOffset(*offset) ? table->getDirect(*offset) : JSValue();
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
    // So does this object: the next read finds it above, and capturedValueWithoutAllocating() finds it for the caches.
    JSValue made = modules().capturedValue(m_module, identifier);
    RETURN_IF_EXCEPTION(scope, { });
    if (made)
        m_capturedValues->putDirectMayBeIndex(globalObject, identifier, made);
    return made;
}

bool ModuleNamespace::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
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
        if (thisObject->isOrdinaryKey(name))
            RELEASE_AND_RETURN(scope, Base::getOwnPropertySlot(thisObject, globalObject, name, slot));
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
        value = index ? thisObject->modules().readExportFromOutside(thisObject->m_module, *index) : thisObject->readDynamicExport(globalObject, name);
        RETURN_IF_EXCEPTION(scope, false);
    }
    slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), value);
    return true;
}

bool ModuleNamespace::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    return getOwnPropertySlot(object, globalObject, Identifier::from(globalObject->vm(), index), slot);
}

void ModuleNamespace::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
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
        if (ownIndex < entries.size() && thisObject->isHiddenExport(ownIndex)) {
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
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
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

bool ModuleNamespace::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    if (thisObject->isSymbolLikeKey(name.uid()) && name != globalObject->vm().propertyNames->toStringTagSymbol)
        return Base::deleteProperty(thisObject, globalObject, name, slot);
    if (!thisObject->isSymbolLikeKey(name.uid()) && !thisObject->evaluateIfNeeded(globalObject))
        return false;
    return !thisObject->hasExport(name.uid()) && name != globalObject->vm().propertyNames->toStringTagSymbol;
}

bool ModuleNamespace::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
    DeletePropertySlot slot;
    return deleteProperty(cell, globalObject, Identifier::from(globalObject->vm(), index), slot);
}

bool ModuleNamespace::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    if (thisObject->isOrdinaryKey(name))
        RELEASE_AND_RETURN(scope, Base::defineOwnProperty(thisObject, globalObject, name, descriptor, shouldThrow));
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
