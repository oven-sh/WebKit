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
#include "AOTRealmStartup.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "DeferredWorkTimer.h"
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
#include "VMInlines.h"
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
    , m_storesPathOfMainModule(state.instance.vm->aotState().image->storesPathOfMainModule())
    , m_loadsModulesByName(state.instance.vm->aotState().image->loadsModulesByName())
    , m_findsDataFilesByName(state.instance.vm->aotState().image->findsDataFilesByName())
    , m_refusesOriginalsBesideMocks(state.instance.vm->aotState().image->refusesOriginalsBesideMocks())
    , m_mockOperations(host().mockOperations ? host().mockOperations() : nullptr)
{
    m_instance.modules.status = state.tableArena.allocate<uint8_t>(m_table.size());
    m_storedStatuses = unsafeMakeSpan(m_instance.modules.status, m_table.size());
    m_instance.modules.evaluatingMark = static_cast<uint32_t>(StoredModuleStatus::Evaluating);
    m_instance.modules.runsImportedFilesAlone = 0;
    m_modulesSettledForLinks.fill(0, m_table.size() / 64 + 1);
    state.caches->add(CacheKind::StrongRoots, "modules"_s, *this, RootVolatility::GreyedByExecution);
}

Modules::~Modules()
{
    forgetUnfinishedLoads();
}

void Modules::forgetUnfinishedLoads()
{
    m_synchronousLoadsHandedToLoader.clear();
    m_evaluationOfImportOfMainModule.clear();
    m_capabilityOfMainModule.clear();
    m_loadsInFlight.clear();
    m_levelsOfLoads.clear();
    m_filesAskedForOnBehalfOfRoot.clear();
    m_deferredRequests.clear();
    m_deferredImports.clear();
    m_importsBehindLoads.clear();
    m_importsBehindEvaluations.clear();
    m_textsOnTheirWay.clear();
    forgetTextsThatWait();
}

void forgetUnfinishedLoadsOfRetiredRealms(VM& vm)
{
    VMState& state = vm.aotState();
    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances) {
        if (!instance->globalObject || instance->globalObject->microtaskRunnability() != QueuedTaskResult::Discard)
            continue;
        if (instance->state && instance->state->modules)
            instance->state->modules->forgetUnfinishedLoads();
    }
}

Modules& modulesOf(Instance& instance)
{
    return *instance.state->modules;
}

Modules* modulesOf(JSGlobalObject* realm)
{
    Instance* instance = Instance::of(realm);
    return instance ? instance->state->modules.get() : nullptr;
}

void Modules::noteFirstRequestFromHost(ModuleID module)
{
    m_firstModuleRequestedByHost = module;
}

const ModuleHost& Modules::host() const
{
    return moduleHostOf(m_instance.globalObject);
}

Identifier Modules::identifier(uint32_t number) const
{
    return identifiersOf(m_instance).identifier(number);
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
    return (status(file) == ModuleStatus::Evaluating && data(file).kind == ModuleKind::CommonJs) || hasRun(file);
}

void Modules::keepObjectMadeByRequire(ModuleID module, JSObject* moduleObject)
{
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.objectMadeByRequire || loaded.isBeingRequired)
        return;
    loaded.objectMadeByRequire.set(*this, moduleObject);
    JSValue exports = moduleObject->getDirect(*m_instance.vm, m_instance.vm->propertyNames->exports);
    loaded.exportsGivenToObjectMadeByRequire.set(*this, exports);
    loaded.exportsWhileRequired.set(*this, exports && exports.isObject() ? asObject(exports) : nullptr);
}

void Modules::forgetObjectMadeByRequire(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->isKeptByRequire)
        return;
    loaded->isKeptByRequire = false;
    loaded->objectMadeByRequire.set(*this, nullptr);
}

// ES modules import a CommonJS file through its face, which require() leaves alone. The loader has what a module imports before the module begins.
bool Modules::wasLoadedByImport(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (data(file).kind == ModuleKind::SourceText)
        return true;
    ModuleID imported = moduleWithExportsOf(file);
    if (imported != file && status(imported) >= ModuleStatus::Evaluating)
        return true;
    BitVector reached(m_table.size());
    Vector<ModuleID, 64> toFollow;
    for (unsigned index = 0; index < m_table.size(); ++index) {
        if (status(ModuleID(index)) >= ModuleStatus::Evaluating)
            toFollow.append(ModuleID(index));
    }
    while (!toFollow.isEmpty()) {
        ModuleID importer = toFollow.takeLast();
        // What only a mocked module imports is not loaded.
        if (standsForWholeModule(importer))
            continue;
        for (const ModuleRequestData& request : m_table.requests(importer)) {
            // The list of a root holds the files of its whole graph, and says nothing of who imports them.
            if (request.isDeferred || request.isRequire || request.isHoisted)
                continue;
            if (request.targetModule() == imported)
                return true;
            // A statement of a test file that the loader has linked. It knows the file by its path.
            if (data(request.targetModule()).isUncompiledCode()) {
                LoadedModule* linked = loadedIfExists(request.targetModule());
                if (linked && linked->uncompiledRecord && linked->keyOfLatestLoad.string() == m_table.pathText(file))
                    return true;
                continue;
            }
            if (!reached.set(request.target()))
                toFollow.append(request.targetModule());
        }
    }
    return false;
}

// The loader has the record of a file that it linked for a test file, or walked. `delete require.cache[path]` takes the file out of the loader too: whoever asks it for the
// path from then on gets the file from disk. A store leaves the loader as it is.
void Modules::takeOutOfLoader(ModuleID module)
{
    ModuleID file = fileOf(module);
    for (ModuleID known : { file, moduleWithExportsOf(file) }) {
        if (ModuleRecord* record = m_records.get(known))
            loader()->removeEntryOfRecord(record);
    }
}

void Modules::evict(ModuleID module, bool isUndoneForKeptFiles)
{
    ModuleID file = fileOf(module);
    // The loader keeps the error of a CommonJS file that threw while an import loaded it, until the file is taken out of require.cache: didThrowInBody(). That of an ES module, until takeOutFilesThatFailed().
    bool hasThrown = status(file) == ModuleStatus::Failed || status(moduleWithExportsOf(file)) == ModuleStatus::Failed;
    if (!wouldBeEvicted(file) && !hasThrown)
        return;
    if (isUndoneForKeptFiles && !isEvicted(file) && m_keptFiles.get(file.index()))
        m_filesToBringBack.set(file.index());
    leaveToHost(file);
    m_admitsCodeOutsideImage = true;
    if (data(file).kind != ModuleKind::CommonJs)
        return;
    // The module object of the file reads `exports` from a variable of the program, which will hold what the next load exports.
    // `const entry = require.cache[path]; delete require.cache[path]`: whoever holds the object goes on seeing the exports of this load. A body that still runs may yet store others.
    VM& vm = *m_instance.vm;
    // The variable of a file that comes back stays the file's.
    if (LoadedModule* loaded = loadedIfExists(file); loaded && loaded->requirer && data(file).hasValue() && status(file) == ModuleStatus::Evaluated && !m_filesToBringBack.get(file.index()))
        loaded->requirer->putDirect(vm, vm.propertyNames->exports, commonJSExports(file));
    // A file that is still running keeps the mark when it ends.
    m_filesToLoadAgain.set(file.index());
    setStoredStatus(file, StoredModuleStatus::InRecord);
}

// False: the build has seen nothing that resets the modules.
bool Modules::holdsCodeThatMayBeEvicted()
{
    if (!m_holdsCodeThatMayBeEvicted) {
        m_holdsCodeThatMayBeEvicted = false;
        for (unsigned index = 0; index < m_table.size() && !*m_holdsCodeThatMayBeEvicted; ++index) {
            const ModuleData& file = data(ModuleID(index));
            m_holdsCodeThatMayBeEvicted = !file.isOfLibrary() && (file.kind == ModuleKind::SourceText || file.kind == ModuleKind::CommonJs) && file.mayBeEvicted();
        }
    }
    return *m_holdsCodeThatMayBeEvicted;
}

// The two are called round every hook of a setup file, once or more for each test: where nothing has run since the last question, this compares the stored statuses and copies two sets.
BitVector Modules::modulesThatHaveRun()
{
    followImportsOfModulesThatHaveRun();
    BitVector modules = m_modulesSeenToHaveRun;
    modules.merge(m_modulesThatRanWithImporters);
    return modules;
}

void Modules::willRunCodeThatKeepsFiles()
{
    if (holdsCodeThatMayBeEvicted())
        m_modulesThatRanBeforeCodeThatKeepsFiles = modulesThatHaveRun();
}

void Modules::keepFilesThatHaveRun()
{
    if (!holdsCodeThatMayBeEvicted())
        return;
    BitVector modules = modulesThatHaveRun();
    modules.exclude(std::exchange(m_modulesThatRanBeforeCodeThatKeepsFiles, { }));
    for (size_t index : modules) {
        ModuleID module(index);
        if (!data(module).isOfLibrary() && fileOf(module) == module && mayBeEvicted(module) && !isEvicted(module))
            m_keptFiles.set(index);
    }
}

// The reverse of evict(). A file that has not run yet and was left to the host for what it imports is the program's again, unless it still imports an evicted file.
void Modules::bringBackKeptFiles(const WTF::Function<void(const String&)>& functor)
{
    if (m_filesToBringBack.isEmpty())
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    Vector<ModuleID, 16> back;
    for (size_t index : std::exchange(m_filesToBringBack, { }))
        back.append(ModuleID(index));
    for (ModuleID file : back)
        m_evictedFiles.clear(file.index());
    BitVector evictedWithImports = std::exchange(m_filesEvictedWithImports, { });
    m_evictedFiles.exclude(evictedWithImports);
    m_filesAboveEvictedFile.clearAll();
    m_filesAboveNoEvictedFile.clearAll();
    ++m_linkEpoch;
    for (size_t index : evictedWithImports)
        isLeftToHost(ModuleID(index));
    for (ModuleID file : back) {
        functor(m_table.pathText(file));
        if (data(file).kind != ModuleKind::CommonJs)
            continue;
        bool wasLoadedAgain = !m_filesToLoadAgain.get(file.index());
        m_filesToLoadAgain.clear(file.index());
        if (!recordIfExists(file))
            setStoredStatus(file, StoredModuleStatus::Evaluated);
        if (wasLoadedAgain) {
            captureExportsAgain(file);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
}

// The next load of the module runs its body again. Its code keeps what the body declares in objects that each run makes anew, so whatever an earlier run left behind goes on with its own.
// Nothing of the image is linked again: what the module imports is what it was, but for a file that was taken out of require.cache since.
// What the loader links for it is linked again, to what the loader has by then.
void Modules::forgetEvaluation(ModuleID module)
{
    const ModuleData& moduleData = data(module);
    if (!m_instance.program->runsEntryPointsAgain() || !moduleData.isEntryPoint() || !moduleData.isESModuleOfProgram() || moduleData.isFaceOfFile())
        return;
    if (isEvicted(module) || isMocked(module))
        return;
    ModuleStatus current = status(module);
    if (current != ModuleStatus::Evaluated && current != ModuleStatus::Failed)
        return;
    RELEASE_ASSERT(!m_innermostEvaluation);
    if (ModuleRecord* record = m_records.get(module)) {
        loader()->removeEntryOfRecord(record);
        Locker locker { m_state.caches->lock() };
        m_records.remove(module);
    }
    bool importsByLoader = false;
    for (const ModuleRequestData& request : m_table.imports(module)) {
        if (!data(request.targetModule()).isUncompiledCode())
            continue;
        importsByLoader = true;
        if (LoadedModule* external = loadedIfExists(request.targetModule()))
            external->uncompiledRecord.set(*this, nullptr);
    }
    setStoredStatus(module, importsByLoader ? StoredModuleStatus::Unlinked : StoredModuleStatus::Linked);
    forgetTemplateObjects(module);
    if (LoadedModule* loaded = loadedIfExists(module))
        loaded->importMeta.set(*this, nullptr);
    // See isLeftToHost().
    if (m_evictedFiles.isEmpty())
        return;
    WalkForEvictedFiles walk;
    walk.visitOrder.add(module.index(), 0);
    uint32_t earliestVisitReached = 0;
    for (const ModuleRequestData& request : m_table.imports(module)) {
        if (importsEvictedFile(request.targetModule(), walk, earliestVisitReached))
            return leaveToHost(module);
    }
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
    m_filesAboveNoEvictedFile.clearAll();
    m_runsCopyFromDisk = true;
}

// `writeFileSync("fixture.ts", ..); await import("./fixture.ts")`: a test run finds what lies on disk at the first load, as plain Bun does. The host reads the file, and the interpreter runs it.
bool Modules::isChangedOnDisk(ModuleID file)
{
    std::optional<uint64_t> stampOfBuild = m_table.sourceStamp(file);
    if (!stampOfBuild || !host().stampOfFile)
        return false;
    countEvent(*m_instance.vm, RuntimeCounterID::FilesComparedWithDisk);
    CString path = m_table.pathText(file).utf8();
    return host().stampOfFile(byteCast<char>(path.span())) != *stampOfBuild;
}

bool Modules::isChangedOutOfSightOfBuild(StringView path) const
{
    if (m_filesChangedOutOfSightOfBuild.isEmpty())
        return false;
    std::optional<ModuleID> found = m_table.findByPath(path);
    return found && m_filesChangedOutOfSightOfBuild.get(fileOf(*found).index());
}

bool Modules::importsEvictedFile(ModuleID module)
{
    WalkForEvictedFiles walk;
    uint32_t earliestVisitReached = 0;
    if (!importsEvictedFile(module, walk, earliestVisitReached))
        return false;
    // Each of these imports a file whose visit had not ended, and all of those import the evicted file.
    for (uint32_t file : walk.unsettledFiles)
        m_filesAboveEvictedFile.set(file);
    return true;
}

bool Modules::importsEvictedFile(ModuleID module, WalkForEvictedFiles& walk, uint32_t& earliestVisitReached)
{
    ModuleID file = fileOf(module);
    if (m_evictedFiles.get(file.index()))
        return true;
    if (m_filesAboveNoEvictedFile.get(file.index()))
        return false;
    uint32_t visit = walk.visitOrder.size();
    if (auto added = walk.visitOrder.add(file.index(), visit); !added.isNewEntry) {
        earliestVisitReached = std::min(earliestVisitReached, added.iterator->value);
        return false;
    }
    if (status(file) >= ModuleStatus::Evaluating || hasRun(file, walk.importsAreFollowed)) {
        m_filesAboveNoEvictedFile.set(file.index());
        return false;
    }
    if (m_filesAboveEvictedFile.get(file.index()))
        return true;
    if (isChangedOnDisk(file)) [[unlikely]] {
        // The host is asked for the file all the same, and fails the load: isChangedOutOfSightOfBuild().
        if (!mayBeEvicted(file))
            m_filesChangedOutOfSightOfBuild.set(file.index());
        leaveToHostUnlessItHasRun(file);
        return true;
    }
    size_t numberOfUnsettledFiles = walk.unsettledFiles.size();
    uint32_t earliestVisitReachedFromHere = visit;
    if (isLoadedWithRequests(file)) {
        for (const ModuleRequestData& request : m_table.imports(file)) {
            if (importsEvictedFile(request.targetModule(), walk, earliestVisitReachedFromHere)) {
                m_filesAboveEvictedFile.set(file.index());
                return true;
            }
        }
    }
    if (earliestVisitReachedFromHere < visit) {
        walk.unsettledFiles.append(file.index());
        earliestVisitReached = std::min(earliestVisitReached, earliestVisitReachedFromHere);
        return false;
    }
    m_filesAboveNoEvictedFile.set(file.index());
    for (uint32_t settled : walk.unsettledFiles.span().subspan(numberOfUnsettledFiles))
        m_filesAboveNoEvictedFile.set(settled);
    walk.unsettledFiles.shrink(numberOfUnsettledFiles);
    return false;
}

// A module that has not run yet is bound to what its imports name when it runs. Behind an eviction that is an instance from disk, which only code from disk imports.
bool Modules::isLeftToHost(ModuleID module)
{
    if (m_evictedFiles.isEmpty() && !m_table.hasSourceStamps())
        return false;
    // The host has it anyway.
    if (data(module).isUncompiledCode())
        return false;
    if (!importsEvictedFile(module))
        return false;
    // No file that was settled since the last eviction imports this one: its walk would have come to the same evicted file.
    if (!m_evictedFiles.set(fileOf(module).index()))
        m_filesEvictedWithImports.set(fileOf(module).index());
    m_runsCopyFromDisk = true;
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
    if (JSObject* mock = mockInPlace(module)) [[unlikely]]
        letRecordAnswerByMock(record, mock);
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

void Modules::retireRecordOfModuleThatHasNotBegun(ModuleRecord* record)
{
    ASSERT(!record->isRetained());
    ASSERT(status(record->module()) == ModuleStatus::Linked);
    ASSERT(!record->evaluationError());
    setStoredStatus(record->module(), StoredModuleStatus::Linked);
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
    if (m_instance.vm->synchronousModuleQueue() || m_innermostEvaluation || m_rootWhoseFilesRun || m_rootWhoseFilesRanAhead || m_fileWhoseJobsWaitForLoader || !m_levelsOfLoads.isEmpty() || !m_textsOnTheirWay.isEmpty())
        return;
    // The host loads the graph of the entry point.
    if (!m_firstModuleRequestedByHost || status(*m_firstModuleRequestedByHost) < ModuleStatus::Evaluating)
        return;
    if (m_loadsInFlight.hasPendingPromise())
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
        // The load has failed, and the loader keeps the error with the record.
        if (record->status() == CyclicModuleRecord::Status::New)
            continue;
        if (isLeftToStartsOfImporters(module))
            retireRecordOfModuleThatHasNotBegun(record);
        else if (hasRunToItsEndWithItsCycle(*record))
            retireRecord(record);
    }
}

void didSettleLoad(JSPromise* settled)
{
    JSGlobalObject* realm = settled->realm();
    if (Modules* modules = modulesOf(realm)) {
        modules->didSettlePromiseOfImport(settled);
        modules->resumeImportsBehindSettledLoads();
        modules->retireRecordsOfSettledLoads();
    }
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

static bool isNamedByNumber(StringView path, ModuleID module)
{
    return path.startsWith('#') && parseInteger<uint32_t>(path.substring(1)) == module.index();
}

Identifier Modules::moduleKey(ModuleID module)
{
    // Every importer of a module that the loader goes through asks for the key.
    if (ModuleRecord* record = m_records.get(module))
        return record->moduleKey();
    String path = m_table.pathText(module);
    return Identifier::fromString(*m_instance.vm, path);
}

std::optional<ModuleID> Modules::findByKey(const Identifier& key) const
{
    return m_table.findByPath(key.string());
}

// The loader hands the key to the host, which asks for the module under it. Every importer of the module asks for the key, and the record that the host will ask for keeps it.
Identifier Modules::keyOfRequest(ModuleID target)
{
    return ensureRecordWithoutStatus(target)->moduleKey();
}

JSString* Modules::pathString(ModuleID module)
{
    if (data(module).directory == Number24::max)
        return m_state.strings->string(*m_instance.vm, StringID(data(module).publicPathOrName));
    return jsString(*m_instance.vm, m_table.pathText(module));
}

String Modules::pathOfExporterAsLoaderNamesIt(const RequiredNameData& requiredName)
{
    ModuleID exporter = m_table.fileBehindFace(requiredName.exporterModule()).value_or(requiredName.exporterModule());
    // The table has the specifier of a module that is not of this image. The loader names it by its key.
    return data(exporter).isUncompiledCode() ? resolvedKey(exporter).string() : m_table.pathText(exporter);
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
    String path = pathOfExporterAsLoaderNamesIt(requiredName);
    RETURN_IF_EXCEPTION(scope, void());
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
    String path = pathOfExporterAsLoaderNamesIt(requiredName);
    RETURN_IF_EXCEPTION(scope, void());
    throwException(realm, scope, createAmbiguousExportError(realm, identifier(requiredName.name).string(), path));
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
    // No cache answers for what is asked for anew at each read.
    if (m_numberOfMocksInPlace && (mockInPlace(module) || mockThatAnswersFor(module, moduleExport))) [[unlikely]]
        return { };
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable:
        return ProgramStatics::readVariableWithoutAllocating(m_instance, moduleExport.variable);
    case ModuleExportKind::Namespace: {
        if (handedOnInPlaceOfNamespace(moduleExport)) [[unlikely]]
            return { };
        if (JSObject* linkedWith = namespaceThatCompiledCodeHolds(moduleExport.targetModule(), targetPhaseOf(moduleExport)))
            return linkedWith;
        if (isLeftAloneByMock(moduleExport.targetModule())) [[unlikely]]
            return { };
        JSObject* namespaceObject = namespaceIfCreated(moduleExport.targetModule(), targetPhaseOf(moduleExport));
        return namespaceObject ? JSValue(namespaceObject) : JSValue();
    }
    case ModuleExportKind::Indirect:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue Modules::readExport(ModuleID module, unsigned index, MockOfModule mockOfModule)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const ModuleExportData> exports = m_table.exports(module);
    RELEASE_ASSERT(index < exports.size());
    const ModuleExportData& moduleExport = exports[index];
    if (JSObject* mock = mockOfModule == MockOfModule::IsReadPast ? nullptr : mockInPlace(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, mock->get(realm, identifier(moduleExport.name)));
    // The caches answer for the namespace object of an evaluated module only.
    if (status(module) == ModuleStatus::Linked) [[unlikely]]
        recordEvaluationIfNothingIsLeftToRun(module);
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable: {
        if (m_numberOfMocksInPlace) [[unlikely]] {
            if (std::optional<AnswerOfMock> answer = mockThatAnswersFor(module, moduleExport, mockOfModule)) {
                // AbstractModuleRecord::getReexportedLiveExport(): through another module a name that the mock lacks is undefined, whatever the mock's own [[Get]] makes of it.
                bool hasExport = answer->mock->hasProperty(realm, answer->name);
                RETURN_IF_EXCEPTION(scope, { });
                if (!hasExport)
                    return jsUndefined();
                RELEASE_AND_RETURN(scope, answer->mock->get(realm, answer->name));
            }
        }
        JSValue value = ProgramStatics::readVariable(m_instance, moduleExport.variable);
        if (!value) [[unlikely]] {
            DeferGCForAWhile deferGC(vm);
            throwException(realm, scope, createTDZError(realm, StringView(identifier(moduleExport.name).string())));
        }
        return value;
    }
    case ModuleExportKind::Namespace:
        if (JSValue ofMock = handedOnInPlaceOfNamespace(moduleExport)) [[unlikely]]
            return ofMock;
        if (JSObject* linkedWith = namespaceThatCompiledCodeHolds(moduleExport.targetModule(), targetPhaseOf(moduleExport)))
            return linkedWith;
        if (targetPhaseOf(moduleExport) == ModulePhase::Evaluation && !seesMockOf(moduleExport.targetModule(), module)) [[unlikely]]
            RELEASE_AND_RETURN(scope, namespaceOfModuleItself(moduleExport.targetModule()));
        RELEASE_AND_RETURN(scope, moduleNamespace(moduleExport.targetModule(), targetPhaseOf(moduleExport)));
    case ModuleExportKind::Indirect: {
        const IndirectExportData& indirectExport = m_table.indirectExport(moduleExport);
        // The namespace object of the module between has the names that it was made with. A later mock may give another one, and the search for the name ends at the mock.
        if (m_mocks) [[unlikely]] {
            AbstractModuleRecord* record = recordForResolution(indirectExport.targetModule());
            RETURN_IF_EXCEPTION(scope, { });
            AbstractModuleRecord::Resolution resolution = record->resolveExport(realm, identifier(indirectExport.importName));
            RETURN_IF_EXCEPTION(scope, { });
            if (resolution.isLiveExport(vm))
                RELEASE_AND_RETURN(scope, resolution.moduleRecord->getReexportedLiveExport(realm, resolution.localName));
        }
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
    // `export { b } from "./b.js"` exports the variable of the other module, whose body runs for whoever imports that module itself.
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> variablesOfOtherModules;
    BitVector seen(m_table.size());
    seen.quickSet(module.index());
    Vector<ModuleID, 32> work { module };
    while (!work.isEmpty()) {
        for (const ModuleRequestData& request : m_table.requests(work.takeLast())) {
            if (request.isRequire || seen.quickSet(request.target()))
                continue;
            work.append(request.targetModule());
            for (const ModuleExportData& moduleExport : m_table.exports(request.targetModule())) {
                if (moduleExport.kind() == ModuleExportKind::Variable)
                    variablesOfOtherModules.add(std::bit_cast<uint32_t>(moduleExport.variable));
            }
        }
    }
    for (const ModuleExportData& moduleExport : m_table.exports(module)) {
        if (moduleExport.kind() != ModuleExportKind::Variable || !moduleExport.isReplaceable || variablesOfOtherModules.contains(std::bit_cast<uint32_t>(moduleExport.variable)))
            continue;
        if (!ProgramStatics::readVariableWithoutAllocating(m_instance, moduleExport.variable))
            ProgramStatics::writeVariable(m_instance, moduleExport.variable, jsUndefined());
    }
}

bool Modules::wasLoadedWithImporter(ModuleID module) const
{
    BitVector seen(m_table.size());
    Vector<ModuleID, 32> work;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        // A program links modules ahead of their load, so only a body that runs or has run says that plain Bun has loaded what the module imports.
        if (ModuleID(index) != module && status(ModuleID(index)) > ModuleStatus::Linked) {
            seen.quickSet(index);
            work.append(ModuleID(index));
        }
    }
    while (!work.isEmpty()) {
        ModuleID importer = work.takeLast();
        // What only a mocked module imports is not loaded.
        if (isMocked(importer))
            continue;
        for (const ModuleRequestData& request : m_table.requests(importer)) {
            if (request.isHoisted || request.isRequire || request.isDeferred)
                continue;
            if (request.targetModule() == module)
                return true;
            if (!seen.quickSet(request.target()))
                work.append(request.targetModule());
        }
    }
    return false;
}

bool Modules::tryReplaceExport(ModuleID module, UniquedStringImpl* name, JSValue value)
{
    BitVector seenBehindStars;
    return tryReplaceExport(module, name, value, seenBehindStars);
}

bool Modules::tryReplaceExport(ModuleID module, UniquedStringImpl* name, JSValue value, BitVector& seenBehindStars)
{
    // A scope stands in the two arms that call what may throw, and each leaves the function. One over the whole function would owe the callers a look where an export is merely not replaceable.
    // A chain of re-exports ends at the module that declares the variable. A ring of them names nothing.
    for (unsigned steps = 0; steps <= m_table.size(); ++steps) {
        std::optional<unsigned> index = m_table.findExport(module, name);
        if (!index) {
            auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
            if (data(module).hasDynamicStarExport() && !standsForWholeModule(module)) {
                bool isReplaced = tryReplaceExportBehindStars(module, name, value, seenBehindStars);
                RETURN_IF_EXCEPTION(scope, false);
                if (isReplaced)
                    return true;
            }
            RELEASE_AND_RETURN(scope, tryReplaceCapturedExport(module, name, value));
        }
        const ModuleExportData& moduleExport = m_table.exports(module)[*index];
        switch (moduleExport.kind()) {
        case ModuleExportKind::Variable:
            if (!moduleExport.isReplaceable)
                return false;
            // `export { add } from "a-commonjs-package"` exports the variable that the name of the package is kept in, and the name stands in two more places.
            if (const SyntheticExportPlace* origin = syntheticExportKeptIn(moduleExport.variable))
                return tryReplaceCapturedExport(origin->module, identifier(origin->name).impl(), value);
            if (m_moduleWithMockBeingLaid) {
                LoadedModule& mocked = ensureLoaded(*m_moduleWithMockBeingLaid);
                if (!mocked.variablesBeforeMock)
                    mocked.variablesBeforeMock.set(*this, createTableOfExports());
                Identifier place = Identifier::from(*m_instance.vm, moduleExport.variable.encoded());
                JSValue replaced = ProgramStatics::readVariable(m_instance, moduleExport.variable);
                if (replaced && !storedValue(m_instance.globalObject, mocked.variablesBeforeMock, place))
                    mocked.variablesBeforeMock.get()->putDirectMayBeIndex(m_instance.globalObject, place, replaced);
            }
            ProgramStatics::writeVariable(m_instance, moduleExport.variable, value);
            return true;
        case ModuleExportKind::Namespace:
            return tryReplaceNamespaceHandedOn(moduleExport.targetModule(), value);
        case ModuleExportKind::Indirect: {
            const IndirectExportData& indirectExport = m_table.indirectExport(moduleExport);
            // The statement may be one that is left to the loader, which has found a file of the image under the name.
            module = moduleLoadedUnder(indirectExport.targetModule());
            name = identifier(indirectExport.importName).impl();
            break;
        }
        }
    }
    return false;
}

// `export * from` a module whose names the build did not see. The table of the module that says so lists none of them.
bool Modules::tryReplaceExportBehindStars(ModuleID module, UniquedStringImpl* name, JSValue value, BitVector& seenBehindStars)
{
    if (name == m_instance.vm->propertyNames->defaultKeyword.impl() || seenBehindStars.set(module.index()))
        return false;
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (const ModuleRequestData& request : m_table.requests(module)) {
        if (!request.isDynamicStarExport)
            continue;
        ModuleID target = request.targetModule();
        // A module that may be mocked takes any name among what it has captured. Only the one that has the name passes it on.
        bool hasName = true;
        if (data(target).capturesExports())
            hasName = hasCapturedExport(target, Identifier::fromUid(vm, name));
        else if (data(target).isUncompiledCode()) {
            JSObject* namespaceObject = uncompiledNamespace(target, ModulePhase::Evaluation, NamespaceUse::CarriesBindings);
            RETURN_IF_EXCEPTION(scope, false);
            hasName = namespaceObject->hasProperty(m_instance.globalObject, Identifier::fromUid(vm, name));
        }
        RETURN_IF_EXCEPTION(scope, false);
        if (!hasName)
            continue;
        bool isReplaced = tryReplaceExport(target, name, value, seenBehindStars);
        RETURN_IF_EXCEPTION(scope, false);
        if (isReplaced)
            return true;
    }
    return false;
}

bool Modules::hasBegunToLoad(ModuleID module) const
{
    if (hasMockThatEnded(module)) [[unlikely]]
        return false;
    ModuleStatus current = status(module);
    // The code of a CommonJS file that may be mocked reads its stored status first: deferMock() has that send it to the run time.
    if (data(module).kind == ModuleKind::CommonJs)
        return !mayBeMocked(module) || (current != ModuleStatus::Unlinked && current != ModuleStatus::Linked);
    if ((current != ModuleStatus::Unlinked && current != ModuleStatus::Linked) || didCaptureExports(module))
        return true;
    // `bun test` takes a module out of its loader with the mock that stood in its place.
    if (m_modulesOwedToImportersThatRan.get(module.index()) || wasLetGoByLoader(module))
        return false;
    return data(module).isESModuleOfProgram() && wasLoadedWithGraph(module);
}

// A mock names a path. The loader of `bun run` looks there as JSModuleLoader::registryEntry() does: for the file as no attribute asks for it, then as a type of the host's own, then as JSON. It holds only what has been loaded.
std::optional<ModuleID> Modules::formThatMockFinds(StringView path, std::optional<ModuleID> file) const
{
    Vector<ModuleID, 2> forms = m_table.formsThatAttributesAskFor(path);
    // A patch would reach one of several that are loaded, so the host patches none.
    size_t numberLoaded = std::ranges::count_if(forms, [&](ModuleID form) { return hasBegunToLoad(form); }) + (file && hasBegunToLoad(*file));
    if (numberLoaded > 1)
        return std::nullopt;
    if (file && hasBegunToLoad(*file))
        return file;
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

// As under `bun run`, no import runs the body of a module that a mock stands in place of: importers get the names of the mock alone.
// The library gets the module from the registry. The body runs for it. It stores no name that an importer reads: the loader does that, once.
bool Modules::letMockStandInPlaceOfLibraryModule(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (data(module).kind != ModuleKind::RuntimeSource || !m_libraryModulesThatMockReplaces.get(module.index()))
        return false;
    // An earlier mock that still waits for the first load comes first: code outside the image may have loaded it from the host. This one is laid over it.
    if (m_modulesWithMockToRun.get(module.index())) {
        runMockThatWaits(module);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (exportsWhatMockNamesAlone(module))
        return true;
    exportWhatMockNamesAlone(module, true);
    if (!exportsWhatMockNamesAlone(module))
        return false;
    // A name that the build knows the module to have is read without a test.
    fillCapturedExports(module);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
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
    m_exportsLeftOut.set(module, WTF::move(leftOut));
}

void Modules::takeWhatMockAdds(ModuleID module, bool comesAheadOfLoad)
{
    if (!comesAheadOfLoad) {
        if (LoadedModule* loaded = loadedIfExists(module))
            loaded->takesNamesThatMockAdds = false;
        return;
    }
    LoadedModule& loaded = ensureLoaded(module);
    loaded.takesNamesThatMockAdds = true;
    loaded.exportsAddedByMock.set(*this, createTableOfExports());
}

bool Modules::isAddedByMock(ModuleID module, const Identifier& name)
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->exportsAddedByMock && !!storedValue(m_instance.globalObject, loaded->exportsAddedByMock, name);
}

bool Modules::standsForWholeModule(ModuleID module) const
{
    if (!isMocked(module) && !mockInPlace(module))
        return false;
    const LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->exportsAddedByMock;
}

void Modules::appendNamesAddedByMock(ModuleID module, WTF::Vector<Identifier>& names)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->exportsAddedByMock)
        return;
    PropertyNameArrayBuilder added(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    JSObject::getOwnPropertyNames(loaded->exportsAddedByMock, m_instance.globalObject, added, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    for (const Identifier& name : added)
        names.append(name);
}

bool Modules::tryReplaceExportAddedByMock(ModuleID module, UniquedStringImpl* name, JSValue value)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule& loaded = ensureLoaded(module);
    JSObject* added = loaded.exportsAddedByMock;
    if (!added)
        return false;
    Identifier key = Identifier::fromUid(vm, name);
    if (!loaded.takesNamesThatMockAdds && !storedValue(realm, added, key))
        return false;
    added->putDirectMayBeIndex(realm, key, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (JSObject* namespaceObject = namespaceIfCreated(module, ModulePhase::Evaluation)) {
        uncheckedDowncast<ModuleNamespace>(namespaceObject)->replaceCapturedValue(realm, key, value);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

bool Modules::dropExportThatFindsNothing(ModuleID importer, const RequiredNameData& requiredName)
{
    const ExportStatementData* statement = m_table.findExportStatement(requiredName);
    if (!statement || !statement->dropsExportIfAbsent)
        return false;
    Identifier exportName = Identifier::fromString(*m_instance.vm, ProgramStrings::text(m_table.programTables(), statement->exportNameString()));
    if (std::optional<unsigned> index = m_table.findExport(importer, exportName.impl())) {
        m_exportsLeftOut.ensure(importer, [&] {
            return BitVector(m_table.exports(importer).size());
        }).iterator->value.quickSet(*index);
    }
    return true;
}

bool Modules::isVariableOfExportEmpty(ModuleID module, unsigned index) const
{
    return ProgramStatics::isVariableEmpty(m_instance, m_table.exports(module)[index].variable);
}

static bool exportSameBinding(const ModuleTable& table, const ModuleExportData& a, const ModuleExportData& b)
{
    if (a.kind() != b.kind())
        return false;
    switch (a.kind()) {
    case ModuleExportKind::Variable:
        return a.variable == b.variable;
    case ModuleExportKind::Namespace:
        return a.module == b.module;
    case ModuleExportKind::Indirect:
        return table.indirectExport(a).module == table.indirectExport(b).module && table.indirectExport(a).importName == table.indirectExport(b).importName;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool Modules::namesBehindStarsAreInTables(ModuleID module, const SearchBehindStars& search) const
{
    BitVector isSeen(m_table.size());
    for (ModuleID onPath : search.path)
        isSeen.quickSet(onPath.index());
    Vector<ModuleID, 8> toAsk { module };
    while (!toAsk.isEmpty()) {
        ModuleID asked = toAsk.takeLast();
        if (!data(asked).hasDynamicStarExport() || isSeen.quickSet(asked.index()) || standsForWholeModule(asked))
            continue;
        for (const ModuleRequestData& request : m_table.requests(asked)) {
            if (!request.isDynamicStarExport)
                continue;
            const ModuleData& source = data(request.targetModule());
            if (source.capturesExports() || source.isUncompiledCode() || !source.hasExportTable())
                return false;
            toAsk.append(request.targetModule());
        }
    }
    return true;
}

// `export *` keeps the name and the binding, so the module got the entry from a module that it requests and that has the same one. The name is found as long as one of them has it.
bool Modules::isLeftOut(ModuleID module, unsigned index, TablesBehindStars tables, SearchBehindStars& search) const
{
    if (auto found = m_exportsLeftOut.find(module); found != m_exportsLeftOut.end())
        return found->value.get(index);
    const ModuleExportData& entry = m_table.exports(module)[index];
    if (!entry.isFromStarExport)
        return false;
    size_t place = module.index() * 2 + (tables == TablesBehindStars::HaveName);
    if (search.isAnswered.get(place))
        return search.isLeftOut.get(place);
    unsigned ringsMet = search.ringsMet;
    // Another `export *` of the module, whose names only the run knows, may have the name too. The table of a module behind a star that is followed keeps every name.
    if (data(module).hasDynamicStarExport()) {
        if (!namesBehindStarsAreInTables(module, search))
            return false;
        tables = TablesBehindStars::HaveName;
    }
    Identifier name = identifier(entry.name);
    bool isMissing = false;
    bool isAnyAsked = false;
    bool comesRoundInCircle = false;
    search.path.append(module);
    for (const ModuleRequestData& request : m_table.requests(module)) {
        ModuleID target = request.targetModule();
        if (request.isRequire || !data(target).hasExportTable())
            continue;
        std::optional<unsigned> handedOn = m_table.findExport(target, name.impl());
        bool handsOnBinding = handedOn && exportSameBinding(m_table, entry, m_table.exports(target)[*handedOn]);
        if (search.path.contains(target)) {
            ++search.ringsMet;
            comesRoundInCircle |= handsOnBinding;
            continue;
        }
        if (handsOnBinding)
            isMissing = isLeftOut(target, *handedOn, tables, search);
        else if (!handedOn && tables == TablesBehindStars::MayLackName)
            isMissing = false;
        else
            continue;
        isAnyAsked = true;
        if (!isMissing)
            break;
    }
    search.path.removeLast();
    // Stars that lead back to where the search came from give the name to nobody.
    bool answer = isAnyAsked ? isMissing : comesRoundInCircle;
    if (ringsMet == search.ringsMet) {
        search.isAnswered.set(place);
        search.isLeftOut.set(place, answer);
    }
    return answer;
}

void Modules::setExportsOfMockAreOfCommonJSModule(ModuleID module, bool areOfCommonJSModule)
{
    ensureLoaded(module).exportsOfMockAreOfCommonJSModule = areOfCommonJSModule;
}

void Modules::mockWhatRequireAnswered(ModuleID module, JSObject* exports)
{
    LoadedModule* loaded = &ensureLoaded(module);
    loaded->exportsOfMock.set(*this, exports);
    if (!loaded->requiredValue)
        return;
    loaded->requiredValue.set(*this, exports);
    // In plain Bun the mock stores what its factory gave in the entry of the file.
    if (JSObject* entry = loaded->objectMadeByRequire) {
        entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, exports);
        loaded->exportsGivenToObjectMadeByRequire.set(*this, exports);
    }
}

void Modules::mockWhatRequireCacheHolds(ModuleID module, JSObject* exports)
{
    LoadedModule& loaded = ensureLoaded(module);
    bool isFirstMock = !loaded.exportsOfMock;
    loaded.exportsOfMock.set(*this, exports);
    if (!loaded.valueInRequireCache)
        return;
    if (isFirstMock)
        loaded.valueInRequireCacheBeforeMock.set(*this, loaded.valueInRequireCache.get());
    loaded.valueInRequireCache.set(*this, exports);
    if (JSObject* entry = loaded.objectMadeByRequire)
        entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, exports);
}

// The mock has ended. An entry that stood in require.cache before it holds again what it held. One that a require() behind the mock made is gone.
void Modules::letGoOfMockInRequireCache(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->valueInRequireCache)
        return;
    JSValue before = loaded->valueInRequireCacheBeforeMock;
    loaded->valueInRequireCacheBeforeMock.set(*this, JSValue());
    loaded->valueInRequireCache.set(*this, before);
    setRequireCacheValue(module, before);
    if (!before) {
        loaded->objectMadeByRequire.set(*this, nullptr);
        loaded->firstRequirer = std::nullopt;
        loaded->firstRequirerOutsideImage.set(*this, nullptr);
        m_libraryModulesInRequireCache.removeFirst(module);
        return;
    }
    if (JSObject* entry = loaded->objectMadeByRequire)
        entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, before);
}

// `export { sides } from "./shapes"`, `export * from "./shapes"`, `import { sides } from "./shapes"; export { sides as again }`: the entry of the module that passes an export on names the variable
// of the module that declares it. While a mock stands in place of that one nothing fills the variable, and the mock answers under the name that the declaring module exports it by.
auto Modules::mockThatAnswersForSlow(ModuleID passingOn, const ModuleExportData& passedOn, MockOfModule ofPassingOn) const -> std::optional<AnswerOfMock>
{
    if (!m_numberOfMocksInPlace || passedOn.kind() != ModuleExportKind::Variable)
        return std::nullopt;
    // The mocked module may itself pass on what another module declares: `export { b } from "./b.js"`. That module is as it was.
    auto takesFromAnotherModule = [&](ModuleID module) {
        BitVector seen(m_table.size());
        seen.quickSet(module.index());
        Vector<ModuleID, 16> work { module };
        while (!work.isEmpty()) {
            for (const ModuleRequestData& request : m_table.requests(work.takeLast())) {
                if (request.isRequire || seen.quickSet(moduleLoadedBy(request).index()))
                    continue;
                work.append(moduleLoadedBy(request));
                for (const ModuleExportData& entry : m_table.exports(moduleLoadedBy(request))) {
                    if (entry.kind() == ModuleExportKind::Variable && entry.variable == passedOn.variable)
                        return true;
                }
            }
        }
        return false;
    };
    // The tables name the variable, not the module that a statement names: of what a module names itself, the one that exports the variable is where it has it from. None: it declares it. Two: nobody knows.
    auto comesThroughMock = [&](ModuleID module, ModuleID mocked, ModuleID faceOfMocked) {
        BitVector seen(m_table.size());
        auto exportsVariable = [&](const ModuleExportData& entry) { return entry.kind() == ModuleExportKind::Variable && entry.variable == passedOn.variable; };
        bool mayDeclare = !passedOn.isFromStarExport;
        while (!seen.quickSet(module.index())) {
            std::optional<ModuleID> from;
            std::optional<ModuleID> onlyOther;
            unsigned others = 0;
            for (const ModuleRequestData& request : m_table.requests(module)) {
                ModuleID named = moduleLoadedBy(request);
                // In a ring a module that the walk came by has the variable from here.
                if (request.isRequire || seen.quickGet(named.index()) || named == from || named == onlyOther)
                    continue;
                if (named != mocked && named != faceOfMocked && !std::ranges::any_of(m_table.exports(named), exportsVariable)) {
                    onlyOther = named;
                    ++others;
                    continue;
                }
                if (from)
                    return false;
                from = named;
            }
            // A table lists what somebody reads off the object of its module: a module on the way may lack the name.
            if (!from && !mayDeclare && others == 1)
                from = onlyOther;
            if (!from)
                return false;
            mayDeclare = std::ranges::any_of(m_table.exports(*from), exportsVariable);
            if (from == mocked || from == faceOfMocked)
                return true;
            module = *from;
        }
        return false;
    };
    auto reaches = [&](ModuleID from, ModuleID to) {
        BitVector seen(m_table.size());
        seen.quickSet(from.index());
        Vector<ModuleID, 16> work { from };
        while (!work.isEmpty()) {
            for (const ModuleRequestData& request : m_table.requests(work.takeLast())) {
                if (moduleLoadedBy(request) == to)
                    return true;
                if (!request.isRequire && !seen.quickSet(moduleLoadedBy(request).index()))
                    work.append(moduleLoadedBy(request));
            }
        }
        return false;
    };
    // `export default function greet() {}` beside `export { greet }`: one variable under two names. The name that the export statement asks the module for goes first, then the name that is passed on.
    auto nameAskedOf = [&](ModuleID declaring, const Vector<uint32_t, 2>& names) -> uint32_t {
        if (names.size() == 1)
            return names[0];
        String exportName = identifier(passedOn.name).string();
        for (const RequiredNameData& requiredName : m_table.requiredNames(passingOn)) {
            if (requiredName.exporterModule() != declaring || !names.contains(requiredName.name))
                continue;
            const ExportStatementData* statement = m_table.findExportStatement(requiredName);
            if (statement && ProgramStrings::text(m_table.programTables(), statement->exportNameString()) == exportName)
                return requiredName.name;
        }
        uint32_t passedOnName = passedOn.name;
        return names.contains(passedOnName) ? passedOnName : names[0];
    };
    for (size_t index : m_modulesWithMockInPlace) {
        JSObject* mock = mockInPlace(ModuleID(index));
        // Only whoever does not see the mock holds the object that reads past it, whenever the module itself was loaded.
        if (ofPassingOn == MockOfModule::IsReadPast && (ModuleID(index) == passingOn || moduleWithExportsOf(ModuleID(index)) == passingOn))
            continue;
        if (!mock || !seesMockOf(ModuleID(index), passingOn))
            continue;
        ModuleID declaring = moduleWithExportsOf(ModuleID(index));
        Vector<uint32_t, 2> names;
        for (const ModuleExportData& entry : m_table.exports(declaring)) {
            if (entry.kind() != ModuleExportKind::Variable || entry.variable != passedOn.variable)
                continue;
            // The mocked module has the name by a star from the module that asks, which declares it or has it by name.
            if (entry.isFromStarExport && !passedOn.isFromStarExport && reaches(declaring, passingOn))
                continue;
            names.append(static_cast<uint32_t>(entry.name));
        }
        if (!names.isEmpty() && (!takesFromAnotherModule(declaring) || comesThroughMock(passingOn, ModuleID(index), declaring)))
            return AnswerOfMock { mock, identifier(nameAskedOf(declaring, names)), ModuleID(index) };
        for (const SyntheticExportData& entry : m_table.syntheticExports(declaring)) {
            if (entry.hasVariable && entry.variable == passedOn.variable)
                return AnswerOfMock { mock, identifier(entry.name), ModuleID(index) };
        }
    }
    return std::nullopt;
}

// JavaScriptCore's own search for a name, and code outside the image that links to the module, go by the rules of a record with live exports (AbstractModuleRecord::hasLiveExports()), as for the
// module that `bun test` makes of a mock: whoever asks the module itself finds any name, `export *` passes on what the mock had when it came, and no name gets ambiguous by it.
void Modules::letRecordAnswerByMock(ModuleRecord* record, JSObject* mock)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!record->hasLiveExports()) {
        record->setHasLiveExports();
        ModuleID module = moduleWithExportsOf(record->module());
        auto declare = [&](const Identifier& name) {
            record->addExportEntry(AbstractModuleRecord::ExportEntry::createLocal(name, name));
        };
        std::span<const ModuleExportData> entries = m_table.exports(module);
        for (unsigned index = 0; index < entries.size(); ++index) {
            if (!isLeftOut(module, index, TablesBehindStars::HaveName))
                declare(identifier(entries[index].name));
        }
        LoadedModule& loaded = ensureLoaded(module);
        if (JSObject* table = loaded.exportsWhatMockNamesAlone ? loaded.capturedExports.get() : loaded.exportsAddedByMock.get()) {
            PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
            // An object of the run time's own, with plain values: to list it runs no code.
            JSObject::getOwnPropertyNames(table, m_instance.globalObject, names, DontEnumPropertiesMode::Include);
            scope.assertNoExceptionExceptTermination();
            for (const Identifier& name : names)
                declare(name);
        }
    }
    record->setLiveExportsSource(vm, mock);
}

// A record cannot go back to what it was. Whoever holds it keeps the mock, as whoever holds the module of a mock in `bun test`, and the next question for the module makes another.
void Modules::letGoOfRecordThatAnswersByMock(ModuleID module)
{
    // A record that was made for a search alone holds no status, and answers by the mock all the same.
    ModuleRecord* record = m_records.get(module);
    if (!record || !record->hasLiveExports())
        return;
    // Nothing of the module has run or was linked.
    if (storedStatus(module) == StoredModuleStatus::InRecord)
        setStoredStatus(module, StoredModuleStatus::Unlinked);
    loader()->removeEntryOfRecord(record);
    Locker locker { m_state.caches->lock() };
    m_records.remove(module);
}

// Which names the module has for whoever comes through `export *` or lists a module that passes them on: those that the mock has as it comes. No value is read: every read asks the mock.
void Modules::takeNamesOfMockInPlace(ModuleID module, JSObject* mock)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule& loaded = ensureLoaded(module);
    bool hasTable = data(module).hasExportTable() && !data(module).capturesExports();
    if (!mock) {
        m_exportsLeftOut.remove(module);
        loaded.exportsAddedByMock.set(*this, nullptr);
        if (loaded.exportsWhatMockNamesAlone)
            loaded.capturedExports.set(*this, nullptr);
        loaded.exportsWhatMockNamesAlone = false;
        loaded.wasLoadedBehindMock = false;
        loaded.wasRequiredBehindMock = false;
        return;
    }
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    mock->methodTable()->getOwnPropertyNames(mock, realm, names, DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, void());
    JSObject* table = createTableOfExports();
    BitVector leftOut(hasTable ? m_table.exports(module).size() : 0);
    for (unsigned index = 0; hasTable && index < m_table.exports(module).size(); ++index)
        leftOut.quickSet(index);
    for (const Identifier& name : names) {
        if (std::optional<unsigned> index = hasTable ? m_table.findExport(module, name.impl()) : std::nullopt) {
            leftOut.quickClear(*index);
            continue;
        }
        table->putDirectMayBeIndex(realm, name, jsUndefined());
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (hasTable) {
        m_exportsLeftOut.set(module, WTF::move(leftOut));
        loaded.exportsAddedByMock.set(*this, table);
        loaded.takesNamesThatMockAdds = false;
        return;
    }
    loaded.capturedExports.set(*this, table);
    loaded.exportsWhatMockNamesAlone = true;
    // An importer looks behind the file whether the variable of a name that it binds was filled, and refuses to run where it was not.
    RELEASE_AND_RETURN(scope, fillCapturedExports(module));
}

// The importers of the program ask the mock. The library has the module from its registry: the body runs for it, and no mock reaches it. So the module does not join the mocked files.
bool Modules::setMockInPlaceOfLibraryModule(ModuleID module, JSObject* mock, JSValue whatRequireAnswers)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    bool stoodInPlace = !!mockInPlace(module);
    if (mock) {
        // What the loader holds when the first mock of the module is made decides.
        willMockLibraryModule(module);
        if (!m_libraryModulesThatMockReplaces.get(module.index()))
            return false;
        takeNamesOfMockInPlace(module, mock);
        RETURN_IF_EXCEPTION(scope, false);
        if (whatRequireAnswers.isObject())
            mockWhatRequireCacheHolds(module, asObject(whatRequireAnswers));
        // The loader of the host answers an import() of the module by itself, so an earlier mock may still wait here for the first load by compiled code. This one has taken its place.
        m_modulesWithMockToRun.clear(module.index());
    } else if (!stoodInPlace)
        return false;
    m_numberOfMocksInPlace += !!mock - stoodInPlace;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithMockInPlace.set(module.index(), !!mock);
    ensureLoaded(module).mockInPlace.set(*this, mock);
    if (!mock)
        letGoOfRecordThatAnswersByMock(module);
    else if (ModuleRecord* record = m_records.get(module))
        letRecordAnswerByMock(record, mock);
    if (std::optional<SlotData> word = m_table.wordOfMock(module))
        ProgramStatics::writeVariable(m_instance, *word, mock ? JSValue(mock) : JSValue());
    // endMockOfLibraryModule() takes the names from the module anew.
    return true;
}

bool Modules::setMockInPlace(ModuleID fileOrModule, JSObject* mock, JSValue whatRequireAnswers, bool leavesEarlierMockToItsImporters)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (data(fileOrModule).isOfLibrary() || !m_table.hasFilePath(fileOrModule))
        RELEASE_AND_RETURN(scope, setMockInPlaceOfLibraryModule(fileOrModule, mock, whatRequireAnswers));
    // The next mock is laid as over a module that nothing stands in place of.
    if (mock && hasMockThatEnded(fileOrModule)) [[unlikely]] {
        setMockInPlace(fileOrModule, nullptr, { });
        RETURN_IF_EXCEPTION(scope, false);
    }
    ModuleID module = moduleWithExportsOf(fileOrModule);
    bool isFile = data(fileOrModule).kind == ModuleKind::CommonJs;
    bool stoodInPlace = !!mockInPlace(fileOrModule);
    m_modulesWithMockToLetGo.clear(fileOrModule.index());
    if (!mock && !stoodInPlace)
        return false;
    JSObject* endedMock = mock ? nullptr : mockInPlace(fileOrModule);
    if (mock)
        noteMomentMockComesInPlace(fileOrModule, mock, stoodInPlace, leavesEarlierMockToItsImporters);
    takeNamesOfMockInPlace(module, mock);
    RETURN_IF_EXCEPTION(scope, false);
    m_numberOfMocksInPlace += !!mock - stoodInPlace;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithMockInPlace.set(fileOrModule.index(), !!mock);
    if (mock)
        willMock(fileOrModule);
    ensureLoaded(fileOrModule).mockInPlace.set(*this, mock);
    ensureLoaded(module).mockInPlace.set(*this, mock);
    for (ModuleID each : { fileOrModule, module }) {
        if (!mock)
            letGoOfRecordThatAnswersByMock(each);
        else if (ModuleRecord* record = m_records.get(each))
            letRecordAnswerByMock(record, mock);
    }
    if (endedMock)
        keepMockThatStoodInPlace(fileOrModule, module, endedMock);
    // Compiled code that imports a name of the module looks here first.
    if (std::optional<SlotData> word = m_table.wordOfMock(fileOrModule))
        ProgramStatics::writeVariable(m_instance, *word, mock ? JSValue(mock) : wordWhereNoMockStands(fileOrModule));

    LoadedModule& loaded = ensureLoaded(fileOrModule);
    bool originalRuns = isFile && m_filesWhoseOriginalRuns.get(fileOrModule.index());
    if (mock) {
        if (originalRuns) [[unlikely]]
            loaded.answerOfMockWhileOriginalRuns.set(*this, whatRequireAnswers);
        else if (isFile) {
            // Compiled code calls the body of a CommonJS file itself, and asks for the status first.
            markEvaluated(fileOrModule);
            setCommonJSExports(fileOrModule, whatRequireAnswers);
        } else
            loaded.requiredValue.set(*this, whatRequireAnswers);
        // The mock of a preload stands again behind a test file that had one of its own. The end of that one has marked the module as not loaded. The preload has loaded it under this mock, and the loader of
        // `bun test` holds that load: the mock.module() or jest.mock() of a later test file finds the module loaded and comes into its place at once, whether or not that file imports anything.
        if (!isFile && loaded.mockLaidInPreload.get() == mock && m_loadedInPreload.get(fileOrModule.index())) {
            for (ModuleID each : { fileOrModule, module }) {
                if (status(each) == ModuleStatus::Unlinked || status(each) == ModuleStatus::Linked)
                    setStatus(each, ModuleStatus::Evaluated);
            }
        }
        if (JSObject* entry = loaded.objectMadeByRequire) {
            entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, whatRequireAnswers);
            loaded.exportsGivenToObjectMadeByRequire.set(*this, whatRequireAnswers);
        }
        return true;
    }

    didEndMock(fileOrModule);
    // The body ran beside the mock, for an importer that got the original. What it exported is the file's from here on, and it does not run again.
    JSValue original = isFile ? loaded.exportsOfOriginalBesideMock.get() : JSValue();
    loaded.exportsOfOriginalBesideMock.set(*this, JSValue());
    // The same for a module.
    bool ranBesideMock = m_modulesThatRanBesideMock.get(module.index());
    m_modulesThatRanBesideMock.clear(module.index());
    m_modulesOwedBesideMock.clear(module.index());
    // An evaluation that came by has marked the module as evaluated. Its body has yet to run, behind a link of its own.
    for (ModuleID each : { fileOrModule, module }) {
        if (ranBesideMock) [[unlikely]] {
            // A record that answered by the mock held the status, and is gone.
            if (status(each) < ModuleStatus::Evaluating)
                setStatus(each, ModuleStatus::Evaluated);
            continue;
        }
        if (status(each) == ModuleStatus::Evaluated && (!original || each != fileOrModule))
            setStatus(each, ModuleStatus::Unlinked);
        // `bun test` takes a module out of its loader with the mock that stood in its place.
        m_modulesLoadedBesideGroups.clear(each.index());
        m_modulesOwedToImportersThatRan.set(each.index());
        m_modulesWithNothingOwedBelow.clearAll();
        m_mockedModulesPassedOverByWalks.clear();
        // A walk made before passes over what has work again.
        m_modulesSettledForLinks[each.index() / 64] &= ~(1ull << (each.index() % 64));
    }
    followImportsAnewAtNextQuestion();
    ++m_linkEpoch;
    // The namespace object was made with the names of the mock. The module that loads next has one of its own, as in `bun test`. So has a module whose `export *` took its names from a run.
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID each(index);
        if (each == fileOrModule || each == module || data(each).hasDynamicStarExport())
            letGoOfNamespaceMadeUnderMock(each);
    }
    // Whoever got the module itself beside the mock holds its object, which is the module's from here on.
    if (JSObject* own = ensureLoaded(module).namespaceBeforeMockInPlace.get()) [[unlikely]] {
        uncheckedDowncast<ModuleNamespace>(own)->setKeepsToModule(false);
        ensureLoaded(module).namespaceBeforeMockInPlace.set(*this, nullptr);
        setNamespace(module, ModulePhase::Evaluation, own);
    }
    if (originalRuns) [[unlikely]]
        loaded.answerOfMockWhileOriginalRuns.set(*this, JSValue());
    else if (isFile)
        setCommonJSExports(fileOrModule, original ? original : jsUndefined());
    else
        loaded.requiredValue.set(*this, JSValue());
    // An entry of require.cache that stood while the mock did holds what the mock answered. The next require() makes one of its own.
    loaded.objectMadeByRequire.set(*this, nullptr);
    loaded.exportsGivenToObjectMadeByRequire.set(*this, JSValue());
    return true;
}

// Whoever was loaded behind the first mock of a module in a test file was linked to a mock, not to the module: it goes by the mock that stands, whichever that is.
uint32_t Modules::momentOfFirstMockInTestFile(ModuleID module)
{
    VMState& state = m_instance.vm->aotState();
    auto result = m_firstMocksInTestFile.add(module, FirstMockInTestFile { 0, 0 });
    if (result.isNewEntry || result.iterator->value.momentOfTestFile != state.momentOfTestFile)
        result.iterator->value = FirstMockInTestFile { ++state.momentOfLoads, state.momentOfTestFile };
    return result.iterator->value.moment;
}

bool Modules::hasMockLaidOver(ModuleID module) const
{
    const LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->hasMockLaidOver;
}

// A module that has nothing to run, and imports nothing that has, is never entered: its status stays Linked for good. The evaluation has passed it once it has come by a statement that
// imports it: every statement of a module that has run, and of a module that is entered those in front of the one in which the evaluation stands.
BitVector Modules::modulesWithNothingToRunThatWerePassed() const
{
    BitVector passed(m_table.size());
    Vector<ModuleID, 32> toFollow;
    auto pass = [&](ModuleID imported) {
        if (!data(imported).needsEvaluation() && !passed.quickSet(imported.index()))
            toFollow.append(imported);
    };
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID importer(index);
        // `bun test` has let go of what was linked to a mock that ended, and no load has come to this one since: the test file before. What it imported may be loaded again by now, and is yet to be passed.
        if (!data(importer).isESModuleOfProgram() || !data(importer).needsEvaluation() || status(importer) < ModuleStatus::Evaluating || m_importersOfMockThatEnded.get(index))
            continue;
        bool isEntered = status(importer) == ModuleStatus::Evaluating;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            ModuleID imported = moduleLoadedBy(request);
            if (isEntered && data(imported).needsEvaluation() && status(imported) == ModuleStatus::Evaluating)
                break;
            pass(imported);
        }
    }
    while (!toFollow.isEmpty()) {
        for (const ModuleRequestData& request : m_table.imports(toFollow.takeLast()))
            pass(moduleLoadedBy(request));
    }
    return passed;
}

bool Modules::hasLinkedImporterYetToRun(ModuleID module, std::optional<ModuleID> asking) const
{
    std::optional<BitVector> passed;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID importer(index);
        if (!data(importer).isESModuleOfProgram() || data(importer).isFaceOfFile() || status(importer) > ModuleStatus::Evaluating || !wasLoadedWithGraph(importer))
            continue;
        if (asking && importer == *asking)
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (moduleLoadedBy(request) != module && moduleLoadedBy(request) != moduleWithExportsOf(module))
                continue;
            if (data(importer).needsEvaluation())
                return true;
            if (!passed)
                passed = modulesWithNothingToRunThatWerePassed();
            if (!passed->quickGet(index))
                return true;
            break;
        }
    }
    return false;
}

bool Modules::willLeaveModuleAlone(ModuleID module, bool onlyIfLoadedInPreload, std::optional<ModuleID> asking)
{
    // Another mock of the module still waits for the load. This one takes its place.
    if (auto waiting = m_mocksOverModulesThatHaveRun.find(module); waiting != m_mocksOverModulesThatHaveRun.end() && !waiting->value.isInPlace && wasLetGoByLoader(module))
        return true;
    const ModuleData& moduleData = data(module);
    bool isFile = moduleData.kind == ModuleKind::CommonJs && !moduleData.isOfLibrary();
    if ((!isFile && (!moduleData.isESModuleOfProgram() || moduleData.isFaceOfFile())) || !m_table.hasFilePath(module) || !m_table.wordOfMock(module))
        return false;
    if (!hasRun(module) || isMocked(module) || mockInPlace(module) || isEvicted(module) || m_momentsOfLoads.isEmpty())
        return false;
    // ES modules read the copies that the face of a CommonJS file took, and those stay. Nobody has noted when the file itself was loaded.
    ModuleID withExports = moduleWithExportsOf(module);
    if (isFile && (onlyIfLoadedInPreload || withExports == module || status(withExports) != ModuleStatus::Evaluated || !didCaptureExports(withExports)))
        return false;
    if (onlyIfLoadedInPreload && !m_loadedInPreload.get(module.index()))
        return false;
    if (LoadedModule* loaded = loadedIfExists(module); loaded && loaded->objectMadeByRequire && !isFile)
        return false;
    // `bun test` lays the mock over a module that a linked module has yet to read: only so does the mock reach it. A module is linked and yet to run only while its graph runs:
    // what the evaluation has not come to, and what lies between its root and the body that runs. The module that asks has read what it has read.
    if (isGraphRunning() && hasLinkedImporterYetToRun(module, asking))
        return false;
    VMState& state = m_instance.vm->aotState();
    auto laid = m_mocksOverModulesThatHaveRun.set(module, MockOverModuleThatHasRun { momentOfFirstMockInTestFile(module), state.momentOfTestFile });
    laid.iterator->value.momentThisWasLaid = ++state.momentOfLoads;
    // Whoever holds the record of the module, code from disk that was linked to it, keeps it. The loader forgets it: a load by name from now on fetches the module anew, and so comes to the mock.
    auto setAsideRecord = [&](ModuleID each) {
        ModuleRecord* record = m_records.get(each);
        if (!record)
            return;
        loader()->removeEntryOfRecord(record);
        if (storedStatus(each) == StoredModuleStatus::InRecord)
            setStoredStatus(each, StoredModuleStatus::Evaluated);
        ensureLoaded(each).recordBeforeMockInPlace.set(*this, record);
        Locker locker { m_state.caches->lock() };
        m_records.remove(each);
    };
    setAsideRecord(module);
    if (isFile)
        setAsideRecord(withExports);
    return true;
}

// What setMockInPlace() does for a module of which nothing was filled, for one that has run. Its variables, its status, its namespace object and its record stay what they are.
bool Modules::setMockOverModuleThatHasRun(ModuleID module, JSObject* mock, JSValue whatRequireAnswers)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID withExports = moduleWithExportsOf(module);
    bool isFile = withExports != module;
    LoadedModule& exporting = ensureLoaded(withExports);
    // Compiled code asks for the object of a module where it first uses it, which may be under the mock. So the object is made now, while the module has its own names.
    // It stays out of the slot: nobody has asked for it.
    JSObject* ownNamespace = exporting.namespaceBeforeMockInPlace.get();
    if (!ownNamespace)
        ownNamespace = namespaceIfCreated(withExports, ModulePhase::Evaluation);
    bool namespaceIsHeld = !!ownNamespace;
    if (!ownNamespace) {
        ownNamespace = createNamespace(withExports, ModulePhase::Evaluation, true);
        RETURN_IF_EXCEPTION(scope, false);
        setNamespace(withExports, ModulePhase::Evaluation, nullptr);
    }
    std::optional<BitVector> exportsLeftOutBefore;
    if (auto leftOut = m_exportsLeftOut.find(withExports); leftOut != m_exportsLeftOut.end())
        exportsLeftOutBefore = leftOut->value;
    BitVector exportsHiddenBefore(m_table.exports(withExports).size());
    for (unsigned index = 0; index < m_table.exports(withExports).size(); ++index)
        exportsHiddenBefore.quickSet(index, (m_table.exports(withExports)[index].isFromStarExport && isExportAmbiguous(withExports, index)) || isLeftOut(withExports, index, TablesBehindStars::MayLackName));
    // The table of a face gives way to the names of the mock. The variables keep the copies: fillCapturedExports().
    if (isFile && !ensureLoaded(withExports).capturedExportsBeforeMockInPlace) {
        JSObject* own = capturedExports(withExports);
        RETURN_IF_EXCEPTION(scope, false);
        ensureLoaded(withExports).capturedExportsBeforeMockInPlace.set(*this, own);
    }
    takeNamesOfMockInPlace(withExports, mock);
    RETURN_IF_EXCEPTION(scope, false);
    // The names were asked of an object of the host.
    auto found = m_mocksOverModulesThatHaveRun.find(module);
    if (found == m_mocksOverModulesThatHaveRun.end())
        return false;
    MockOverModuleThatHasRun& kept = found->value;
    LoadedModule& loaded = ensureLoaded(module);
    loaded.mockInPlace.set(*this, mock);
    ensureLoaded(withExports).mockInPlace.set(*this, mock);
    ProgramStatics::writeVariable(m_instance, *m_table.wordOfMock(module), mock);
    // The object in the slot of the module goes to whoever asks by name. The first mock sets the module's own aside; what a later one finds in the slot is the object of the mock before.
    if (!kept.isInPlace) {
        // The code that listed the names of the mock may have asked for the object of the module. A mock of a module behind its stars sets the object aside too, and does not mark it.
        JSObject* askedFor = exporting.namespaceBeforeMockInPlace.get();
        if (!askedFor)
            askedFor = namespaceIfCreated(withExports, ModulePhase::Evaluation);
        if (askedFor) {
            ownNamespace = askedFor;
            namespaceIsHeld = true;
        }
        uncheckedDowncast<ModuleNamespace>(ownNamespace)->setKeepsToModule(true);
        exporting.namespaceBeforeMockInPlace.set(*this, ownNamespace);
        kept.namespaceIsHeld = namespaceIsHeld;
    }
    setNamespace(withExports, ModulePhase::Evaluation, nullptr);
    auto handsOnNamespace = [&](const ModuleExportData& entry) {
        return entry.kind() == ModuleExportKind::Namespace && moduleWithExportsOf(fileOf(entry.targetModule())) == withExports;
    };
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID each(index);
        if (each == withExports)
            continue;
        JSObject* made = namespaceIfCreated(each, ModulePhase::Evaluation);
        LoadedModule* other = loadedIfExists(each);
        JSObject* setAside = other ? other->namespaceBeforeMockInPlace.get() : nullptr;
        // `export * as inner from "./mocked"`: the caches have filed the object of the module under the name. From now on the link of the module between says which of the two objects it hands on.
        if ((made || setAside) && std::ranges::any_of(m_table.exports(each), handsOnNamespace)) {
            for (JSObject* object : { made, setAside }) {
                if (auto* filed = dynamicDowncast<ModuleNamespace>(object))
                    filed->leaveStructureThatCachesKnow(*m_instance.vm);
            }
        }
        // A module whose `export *` takes its names from a run listed those of before the mock. Whoever holds that object keeps it; whoever is linked under the mock gets one with the names of the mock.
        if (!made || !data(each).hasDynamicStarExport())
            continue;
        LoadedModule& behindStars = ensureLoaded(each);
        if (!behindStars.namespaceBeforeMockInPlace)
            behindStars.namespaceBeforeMockInPlace.set(*this, made);
        letGoOfNamespaceMadeUnderMock(each);
    }
    // Made since the mock was laid, for the load that has come to it.
    if (ModuleRecord* record = m_records.get(module))
        letRecordAnswerByMock(record, mock);
    if (ModuleRecord* record = isFile ? m_records.get(withExports) : nullptr)
        letRecordAnswerByMock(record, mock);
    auto answerRequire = [&] {
        loaded.requiredValue.set(*this, whatRequireAnswers);
        if (!isFile)
            return;
        setCommonJSExports(module, whatRequireAnswers);
        if (JSObject* entry = loaded.objectMadeByRequire) {
            entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, whatRequireAnswers);
            loaded.exportsGivenToObjectMadeByRequire.set(*this, whatRequireAnswers);
        }
    };
    if (kept.isInPlace) {
        answerRequire();
        return true;
    }
    kept.isInPlace = true;
    kept.exportsLeftOutBefore = WTF::move(exportsLeftOutBefore);
    kept.exportsHiddenBefore = WTF::move(exportsHiddenBefore);
    ++m_numberOfMocksInPlace;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithMockInPlace.set(module.index());
    loaded.requiredValueBeforeMockInPlace.set(*this, loaded.requiredValue.get());
    if (isFile)
        loaded.exportsOfOriginalBesideMock.set(*this, commonJSExports(module));
    answerRequire();
    return true;
}

void Modules::endMockOverModuleThatHasRun(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    MockOverModuleThatHasRun kept = m_mocksOverModulesThatHaveRun.take(module);
    m_modulesWithMockToRun.clear(module.index());
    m_modulesWithMockThatWaited.clear(module.index());
    m_modulesLoadedBeforeMockThatWaited.clear(module.index());
    didEndMock(module);
    ModuleID withExports = moduleWithExportsOf(module);
    bool isFile = withExports != module;
    LoadedModule& loaded = ensureLoaded(module);
    auto bringBackRecord = [&](ModuleID each) {
        if (ModuleRecord* recordOfMock = m_records.get(each)) {
            loader()->removeEntryOfRecord(recordOfMock);
            if (storedStatus(each) == StoredModuleStatus::InRecord)
                setStoredStatus(each, StoredModuleStatus::Evaluated);
            Locker locker { m_state.caches->lock() };
            m_records.remove(each);
        }
        LoadedModule& held = ensureLoaded(each);
        JSObject* before = held.recordBeforeMockInPlace.get();
        if (!before)
            return;
        auto* record = uncheckedDowncast<ModuleRecord>(before);
        held.recordBeforeMockInPlace.set(*this, nullptr);
        {
            Locker locker { m_state.caches->lock() };
            m_records.add(each, record);
            m_storedReferences.recordAll();
        }
        if (record->holdsStatus())
            setStoredStatus(each, StoredModuleStatus::InRecord);
    };
    bringBackRecord(module);
    if (isFile)
        bringBackRecord(withExports);
    if (!kept.isInPlace)
        return;
    LoadedModule& exporting = ensureLoaded(withExports);
    takeNamesOfMockInPlace(withExports, nullptr);
    scope.assertNoExceptionExceptTermination();
    if (kept.exportsLeftOutBefore)
        m_exportsLeftOut.set(withExports, WTF::move(*kept.exportsLeftOutBefore));
    if (JSObject* own = exporting.capturedExportsBeforeMockInPlace.get()) {
        exporting.capturedExports.set(*this, own);
        exporting.capturedExportsBeforeMockInPlace.set(*this, nullptr);
    }
    m_momentsWaitingMocksWereLaid.remove(module);
    keepEndedMock(module, loaded.mockInPlace.get(), { kept.momentThisWasLaid, kept.momentOfTestFile });
    loaded.mockInPlace.set(*this, nullptr);
    exporting.mockInPlace.set(*this, nullptr);
    --m_numberOfMocksInPlace;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithMockInPlace.clear(module.index());
    ProgramStatics::writeVariable(m_instance, *m_table.wordOfMock(module), wordWhereNoMockStands(module));
    loaded.requiredValue.set(*this, loaded.requiredValueBeforeMockInPlace.get());
    loaded.requiredValueBeforeMockInPlace.set(*this, JSValue());
    if (isFile) {
        JSValue original = loaded.exportsOfOriginalBesideMock.get();
        loaded.exportsOfOriginalBesideMock.set(*this, JSValue());
        setCommonJSExports(module, original);
        if (JSObject* entry = loaded.objectMadeByRequire) {
            entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, original);
            loaded.exportsGivenToObjectMadeByRequire.set(*this, original);
        }
    } else {
        // There was none when the mock came: willLeaveModuleAlone().
        loaded.objectMadeByRequire.set(*this, nullptr);
        loaded.exportsGivenToObjectMadeByRequire.set(*this, JSValue());
    }
    // One that waits for its load has set nothing aside.
    bool anotherMockStands = false;
    for (const MockOverModuleThatHasRun& other : m_mocksOverModulesThatHaveRun.values())
        anotherMockStands |= other.isInPlace;
    // An object that was made under the mock is the mock's, and whoever holds it keeps it. The object of the module is the one from before, as in `bun test`.
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID each(index);
        if (each == withExports || data(each).hasDynamicStarExport())
            letGoOfNamespaceMadeUnderMock(each);
        if (each == withExports || !data(each).hasDynamicStarExport() || anotherMockStands)
            continue;
        LoadedModule* behindStars = loadedIfExists(each);
        if (JSObject* before = behindStars ? behindStars->namespaceBeforeMockInPlace.get() : nullptr) {
            behindStars->namespaceBeforeMockInPlace.set(*this, nullptr);
            setNamespace(each, ModulePhase::Evaluation, before);
        }
    }
    if (JSObject* own = exporting.namespaceBeforeMockInPlace.get()) {
        uncheckedDowncast<ModuleNamespace>(own)->setKeepsToModule(false);
        // The end of a mock that still stands empties the slot of every module with `export *` from a run, and the last of them brings back what is set aside.
        if (!kept.namespaceIsHeld || !data(withExports).hasDynamicStarExport() || !anotherMockStands)
            exporting.namespaceBeforeMockInPlace.set(*this, nullptr);
        // Whoever asks first makes the object, and says whether it takes new properties: willRequireESModule().
        if (kept.namespaceIsHeld)
            setNamespace(withExports, ModulePhase::Evaluation, own);
    }
}

JSObject* Modules::namespaceOfModuleItself(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto mock = m_mocksOverModulesThatHaveRun.find(fileOf(module));
    // The body ran beside a mock that stands in place of the module.
    if (mock == m_mocksOverModulesThatHaveRun.end() && hasRunItself(module) && mockInPlace(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, namespaceOfModuleThatRanBesideMock(module));
    if (mock == m_mocksOverModulesThatHaveRun.end())
        RELEASE_AND_RETURN(scope, moduleNamespace(module, ModulePhase::Evaluation));
    // The mock waits for the first load: what the module has is its own. This is no load.
    if (!mock->value.isInPlace) {
        if (JSObject* own = namespaceIfCreated(module, ModulePhase::Evaluation))
            return own;
        RELEASE_AND_RETURN(scope, createNamespace(module, ModulePhase::Evaluation, true));
    }
    // The object of a CommonJS file is that of its face, whichever of the two is asked for.
    JSObject* own = ensureLoaded(moduleWithExportsOf(mock->key)).namespaceBeforeMockInPlace.get();
    RELEASE_ASSERT(own);
    mock->value.namespaceIsHeld = true;
    return own;
}

bool Modules::isHiddenInModuleItself(ModuleID module, unsigned index) const
{
    auto mock = m_mocksOverModulesThatHaveRun.find(fileOf(module));
    if (mock != m_mocksOverModulesThatHaveRun.end() && mock->value.isInPlace)
        return mock->value.exportsHiddenBefore.get(index);
    // What is left out are the names that the mock lacks.
    if (hasRunItself(module) && mockInPlace(module)) [[unlikely]]
        return m_table.exports(module)[index].isFromStarExport && isExportAmbiguous(module, index);
    return (m_table.exports(module)[index].isFromStarExport && isExportAmbiguous(module, index)) || isLeftOut(module, index, TablesBehindStars::MayLackName);
}

JSValue Modules::readExportOfModuleItself(ModuleID module, unsigned index)
{
    return readExport(module, index, MockOfModule::IsReadPast);
}






const MockOperations& Modules::mockOperations()
{
    static constexpr MockOperations operations {
        .runMocksBelow = [](Modules& modules, ModuleID root, RootOfLoad rootOfLoad) { return modules.runMocksBelowSlow(root, rootOfLoad); },
        .runMocksBelowModuleThatLoaderFetches = [](Modules& modules, ModuleID module) { return modules.runMocksBelowModuleThatLoaderFetchesSlow(module); },
        .letGoOfMockThatEnded = [](Modules& modules, ModuleID module) { modules.letGoOfMockThatEndedSlow(module); },
        .runMockThatWaits = [](Modules& modules, ModuleID module) { return modules.runMockThatWaitsSlow(module); },
        .mockThatAnswersFor = [](const Modules& modules, ModuleID passingOn, const ModuleExportData& passedOn, MockOfModule ofPassingOn) { return modules.mockThatAnswersForSlow(passingOn, passedOn, ofPassingOn); },
        .isMockedInItsImage = [](Modules& modules, ModuleID module, MocksAskedFor which) { return modules.isMockedInItsImageSlow(module, which); },
    };
    return operations;
}

const MockOperations* mockOperations()
{
    return &Modules::mockOperations();
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
    // An ES module captures nothing: a name that it does not export stays out of it, unless a mock stands in place of the module.
    if (!isListed && data(module).hasExportTable() && !m_table.fileBehindFace(module))
        RELEASE_AND_RETURN(scope, tryReplaceExportAddedByMock(module, name, value));

    Identifier key = Identifier::fromUid(vm, name);
    JSObject* captured = capturedExports(module);
    RETURN_IF_EXCEPTION(scope, false);
    // What an import had loaded has its names. A mock stands in place of any other module, with its own.
    LoadedModule& loaded = ensureLoaded(module);
    bool hasItsNames = loaded.exportsWhatMockNamesAlone ? loaded.wasLoadedBehindMock : !m_libraryModulesThatMockReplaces.get(module.index());
    if (hasItsNames && !storedValue(realm, captured, key))
        return false;
    // What a mock is laid over comes back when the mock ends: endMockOfLibraryModule(). The table itself stands for a deferred export, as in what was captured.
    if ((m_libraryModulesWithMock.get(module.index()) || m_moduleWithMockBeingLaid) && !loaded.exportsWhatMockNamesAlone) {
        if (!loaded.exportsBeforeMock)
            loaded.exportsBeforeMock.set(*this, createTableOfExports());
        JSObject* before = loaded.exportsBeforeMock;
        JSValue replaced = storedValue(realm, captured, key);
        if (replaced && !storedValue(realm, before, key)) {
            before->putDirectMayBeIndex(realm, key, replaced == JSValue(captured) ? JSValue(before) : replaced);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }
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

// `export * as space from "./file.cjs"`, read off the namespace object of the exporter. A module that has run keeps what it was linked with, and its code reads that here.
// Whoever asks for the file behind a second load gets another object: captureExportsAgain().
JSObject* Modules::namespaceThatCompiledCodeHolds(ModuleID module, ModulePhase phase) const
{
    // As createNamespace() goes from a file to its face.
    if (data(module).kind == ModuleKind::CommonJs) {
        if (std::optional<ModuleID> face = m_table.faceOfFile(module); face && data(*face).capturesExports())
            module = *face;
    }
    if (phase != ModulePhase::Evaluation || !m_modulesWithEarlierNamespaceInValue.get(module.index()))
        return nullptr;
    JSValue held = value(module);
    return held ? asObject(held) : nullptr;
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

// require() of an ES module answers its namespace object, and keeps the answer.
void Modules::letGoOfNamespaceMadeUnderMock(ModuleID module)
{
    JSObject* made = namespaceIfCreated(module, ModulePhase::Evaluation);
    if (!made)
        return;
    setNamespace(module, ModulePhase::Evaluation, nullptr);
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || loaded->requiredValue.get() != JSValue(made))
        return;
    loaded->requiredValue.set(*this, JSValue());
    loaded->objectMadeByRequire.set(*this, nullptr);
    loaded->exportsGivenToObjectMadeByRequire.set(*this, JSValue());
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
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);
    if (m_state.statics->fillsVariablesWithFiles() && (status(module) == ModuleStatus::Linking || status(module) == ModuleStatus::Evaluating))
        m_state.statics->fillVariablesOfAllFiles();

    if (moduleData.isUncompiledCode()) {
        JSObject* loaderNamespace = uncompiledNamespace(module, phase, NamespaceUse::IsShownToProgram, shouldPreventExtensions);
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

    bool isRequired = phase == ModulePhase::Evaluation && m_modulesWithMarkOfRequire.get(module.index());
    if (phase == ModulePhase::Evaluation && m_modulesWithExtensibleNamespace.get(module.index()))
        shouldPreventExtensions = false;
    ModuleNamespace* namespaceObject = ModuleNamespace::create(vm, namespaceStructure(shouldPreventExtensions), module, phase, { });
    // A ring of stars comes back to this module: the others read its names through this very object.
    if (phase == ModulePhase::Evaluation)
        m_namespacesBeingMade.append({ module, namespaceObject });
    ModuleNamespace::DynamicExports exportObjects;
    createDynamicExports(module, exportObjects.names, exportObjects.capturedValues, exportObjects.liveSources);
    if (phase == ModulePhase::Evaluation)
        m_namespacesBeingMade.removeLast();
    RETURN_IF_EXCEPTION(scope, nullptr);
    namespaceObject->replaceDynamicExports(vm, exportObjects);
    if (isRequired && !namespaceObject->hasExport(vm.propertyNames->__esModule.impl()))
        namespaceObject->setESModuleMarker(true);
    if (JSObject* mock = phase == ModulePhase::Evaluation && isLeftAloneByMock(module) ? mockInPlace(module) : nullptr) [[unlikely]]
        namespaceObject->setMockOfView(vm, mock);
    setNamespace(module, phase, namespaceObject);
    return namespaceObject;
}

void Modules::createDynamicExports(ModuleID module, JSCellButterfly*& names, JSObject*& capturedValues, JSObject*& liveSources)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<DynamicExport> dynamicExports = collectDynamicExports(module);
    RETURN_IF_EXCEPTION(scope, void());
    if (!dynamicExports.isEmpty()) {
        capturedValues = createTableOfExports();
        names = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, dynamicExports.size());
    }
    for (unsigned i = 0; i < dynamicExports.size(); ++i) {
        const DynamicExport& dynamicExport = dynamicExports[i];
        if (dynamicExport.liveSource) {
            if (!liveSources)
                liveSources = createTableOfExports();
            liveSources->putDirectMayBeIndex(realm, dynamicExport.name, dynamicExport.liveSource);
        } else if (dynamicExport.capturedValue)
            capturedValues->putDirectMayBeIndex(realm, dynamicExport.name, dynamicExport.capturedValue);
        RETURN_IF_EXCEPTION(scope, void());
        JSString* text = jsString(vm, dynamicExport.name.string());
        text->toIdentifier(realm);
        RETURN_IF_EXCEPTION(scope, void());
        names->setIndex(vm, i, text);
    }
}

Structure* Modules::namespaceStructure(bool shouldPreventExtensions)
{
    JSGlobalObject* realm = m_instance.globalObject;
    Structure*& structure = shouldPreventExtensions ? m_namespaceStructure : m_extensibleNamespaceStructure;
    if (!structure) {
        structure = ModuleNamespace::createStructure(*m_instance.vm, realm, realm->moduleNamespaceObjectStructure()->storedPrototype(), shouldPreventExtensions);
        m_storedReferences.recordAll();
    }
    return structure;
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
    if (m_mocks) [[unlikely]]
        noteLoadBehindMock(module, false);
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

// The loader of `bun run` keeps what an import statement threw. Every load that comes to the statement gets that object: of the module, of one above it, by import() or by require().
// A call of require() or import() that finds nothing throws anew each time.
void Modules::throwFailure(ModuleFailureID id, bool isForRequire)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleFailureData& failure = m_table.failure(id);
    if (failure.isAbsentExport())
        RELEASE_AND_RETURN(scope, throwNewFailure(id, isForRequire));
    // The table has the statement once for the code that throws in the module, and once for each module whose load it ends. The module that holds a call loads.
    ModuleID importer = failure.importerModule();
    std::optional<ModuleFailureID> endOfLoad = m_table.loadFailure(importer);
    if (!endOfLoad || m_table.failure(*endOfLoad).importer != failure.importer || m_table.failure(*endOfLoad).specifier != failure.specifier)
        RELEASE_AND_RETURN(scope, throwNewFailure(id, isForRequire));
    if (JSValue error = ensureLoaded(importer).errorKeptByLoader) {
        throwException(m_instance.globalObject, scope, error);
        return;
    }
    throwNewFailure(id, isForRequire);
    if (Exception* exception = scope.exception(); exception && !vm.isTerminationException(exception))
        ensureLoaded(importer).errorKeptByLoader.set(*this, exception->value());
}

void Modules::throwNewFailure(ModuleFailureID id, bool isForRequire)
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

static JSValue moduleForJob(ModuleID module)
{
    return jsNumber(module.index());
}

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
        if (JSValue error = modules.errorOfLoadEndedByMock(imported)) [[unlikely]]
            promise->reject(promise->vm(), error);
        else if (modules.isImportedByLoader(imported))
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

static void settleAsLoaderDoes(JSGlobalObject* realm, JSPromise* promiseOfProgram, JSPromise* loaded)
{
    VM& vm = realm->vm();
    if (loaded->status() == JSPromise::Status::Fulfilled)
        promiseOfProgram->fulfill(vm, loaded->result());
    else
        promiseOfProgram->resolve(realm, vm, loaded);
}

// The program gets a promise of its own, as from import() in code that is not compiled.
static JSPromise* createPromiseOfProgram(JSGlobalObject* realm, JSPromise* loaded)
{
    JSPromise* promise = JSPromise::create(realm->vm(), realm->promiseStructure());
    settleAsLoaderDoes(realm, promise, loaded);
    return promise;
}

// The loader of `bun run` rejects a promise of its own, and import() resolves the promise of the program with that one: the program hears of the failure two jobs later.
static JSPromise* importPromiseRejectedByLoader(JSGlobalObject* realm, ThrowScope& scope)
{
    JSPromise* rejected = JSPromise::rejectedPromiseWithCaughtException(realm, scope);
    if (!rejected)
        return JSPromise::create(realm->vm(), realm->promiseStructure());
    return createPromiseOfProgram(realm, rejected);
}

JSPromise* Modules::rejectImportWithErrorOfLoad(ModuleID module, ImportCaller caller)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    throwException(realm, scope, errorOfLoadEndedByMock(module));
    return caller == ImportCaller::CompiledCode ? importPromiseRejectedByLoader(realm, scope) : rejectedPromiseWithPendingException(realm, scope);
}

static JSC_DECLARE_HOST_FUNCTION(importBehindFactories);
static JSC_DECLARE_HOST_FUNCTION(importByKeyBehindFactories);

static JSC_DECLARE_HOST_FUNCTION(continueImportWithModuleItself);

JSPromise* Modules::import(ModuleID module, ModuleID referrer, JSValue options)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    ModuleID walkedBelow = module;
    bool walkStopped = false;
    auto start = [&](ModuleID started) -> JSPromise* {
        JSPromise* promise = startImport(started, asyncEvaluationOrder(referrer), ImportCaller::CompiledCode, TextOfImport::MayComeLater, nullptr, JSValue(), started == walkedBelow ? std::optional(walkStopped) : std::nullopt);
        RETURN_IF_EXCEPTION(scope, promise);
        ModuleID file = fileOf(started);
        // A package gets the module itself. Its body runs behind the steps of the import, and the object is the module's own.
        if (importsOriginalBesideMock(started, referrer)) [[unlikely]] {
            VM& vm = *m_instance.vm;
            JSPromise* ofModuleItself = JSPromise::create(vm, realm->promiseStructure());
            JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), continueImportWithModuleItself, ImplementationVisibility::Private);
            promise->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), ofModuleItself, jsNumber(started.index()));
            promise = ofModuleItself;
        }
        scope.release();
        tellHostOfImport(promise, data(file).mayBeMocked() ? keyWithoutForm(file).string() : String());
        return promise;
    };

    if (m_modulesWithLoadEndedByMock.get(module.index())) [[unlikely]]
        RELEASE_AND_RETURN(scope, rejectImportWithErrorOfLoad(module, ImportCaller::CompiledCode));
    if (m_modulesWhoseFactoryRuns.get(module.index())) [[unlikely]]
        m_importsMadeByOwnFactory.append(module);
    else if (!m_importsMadeByOwnFactory.contains(module)) [[likely]] {
        dropErrorOfMockKeptByLoader(module);
        m_modulesOwedBesideMock.clear(module.index());
    }
    {
        // The error that is kept is for the call that the factory made, which goes on in a later job.
        SetForScope moduleThatProgramImportsNow(m_moduleThatProgramImportsNow, m_modulesWhoseFactoryRuns.get(module.index()) ? std::nullopt : std::optional(module));
        walkStopped = runMocksBelow(module);
    }
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    auto fetchParameters = JSModuleLoader::fetchParametersFromImportOptions(realm, options);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (!fetchParameters)
        return fetchParameters.error();
    // The loader of the host comes to the files that wait to be loaded again by itself, and reads them as it reads whatever is imported.
    if (isLeftToHost(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, importUncompiled(keyWithoutForm(fileOf(module)), referrer, WTF::move(*fetchParameters)));
    if (hostAnswersInPlaceOf(module, LoadByName::Import)) [[unlikely]]
        RELEASE_AND_RETURN(scope, importUncompiled(moduleKey(module), referrer, WTF::move(*fetchParameters)));
    if (!data(module).isUncompiledCode()) {
        // The build has bound the call by what its text says of the attributes. The image may hold the file as what the options ask for.
        if (const ScriptFetchParameters* attributes = fetchParameters->get()) {
            ModuleID file = fileOf(module);
            Identifier key = keyWithoutForm(file);
            // A statement that the loader links, or code from disk, has asked for the file first: the loader has what that made, for every type of the same kind.
            if (registryEntry(key, attributes)) [[unlikely]]
                RELEASE_AND_RETURN(scope, importUncompiled(key, referrer, WTF::move(*fetchParameters)));
            if (std::optional<ModuleID> form = findWithAttributes(key, attributes))
                module = m_table.faceOfFile(*form).value_or(*form);
            else
                noteFormAnsweredFirst(key, *attributes, file);
        }
        return start(module);
    }

    const LoadedModule* loadedBefore = loadedIfExists(module);
    bool hasAskedLoader = loadedBefore && !loadedBefore->resolvedKey.isNull();
    Identifier key = resolvedKey(module);
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    // The loader resolves the name at every call: a plugin is asked each time, and may answer otherwise.
    if (hasAskedLoader) {
        key = loader()->resolve(realm, pathString(module), pathString(*data(module).referrerModule()), nullptr, true);
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    }
    // The build has left the call to the loader, with its type.
    if (std::optional<ModuleID> found = findWithAttributes(key, fetchParameters->get(), FormWithoutAttributes::IsAnotherModule))
        return start(*found);
    // The loader answers with the object that carries the bindings.
    if (m_uncompiledNamespacesNotShownYet.get(module.index())) [[unlikely]] {
        uncompiledNamespace(module, ModulePhase::Evaluation, NamespaceUse::IsShownToProgram);
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    }
    RELEASE_AND_RETURN(scope, importUncompiled(key, *data(module).referrerModule(), WTF::move(*fetchParameters)));
}

// `mock.module("node:util/types", factory)`: the image holds no code of the module, and the loader of the host has what stands in its place.
// `mock.module("undici", factory)`: the image holds the code. As under `bun run`, the mock stands in place of a module that no import has loaded, with its own names alone, and is laid over one that an import has.
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
    if (way == LoadByName::Require && (name == "bun"_s || isInRequireCache(module)))
        return false;
    // Over a module of native code that an import had loaded the mock is laid too: the namespace object of the image has it, with the names that the mock lacks.
    return m_libraryModulesThatMockReplaces.get(module.index());
}

// Under `bun run` the first require() of a module of the library from code of the program leaves a module object in require.cache, which answers the later ones.
// A mock stores what its factory gave into a module object that is there.
bool Modules::isKeptInRequireCache(ModuleID module) const
{
    if (!data(module).isOfLibrary())
        return false;
    if (!m_libraryModulesWithCheckedName.get(module.index())) {
        String name = m_table.pathText(module);
        m_libraryModulesWithCheckedName.set(module.index());
        m_libraryModulesKeptInRequireCache.set(module.index(), !name.startsWith("node:"_s) && name != "bun"_s && name != "bun:test"_s);
    }
    return m_libraryModulesKeptInRequireCache.get(module.index());
}

bool Modules::isInRequireCache(ModuleID module) const
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->valueInRequireCache;
}

// `delete require.cache[name]` takes the module from the loader too: a mock is all that stands under the name then.
void Modules::takeOutOfRequireCache(ModuleID module)
{
    if (isInRequireCache(module)) {
        LoadedModule& loaded = ensureLoaded(module);
        loaded.valueInRequireCache.set(*this, JSValue());
        loaded.objectMadeByRequire.set(*this, nullptr);
        loaded.firstRequirer = std::nullopt;
        loaded.firstRequirerOutsideImage.set(*this, nullptr);
        setRequireCacheValue(module, JSValue());
        m_libraryModulesInRequireCache.removeFirst(module);
    }
    m_libraryModulesTakenFromLoader.set(module.index());
    if (m_libraryModulesWithMock.get(module.index()))
        m_libraryModulesThatMockReplaces.set(module.index());
}

// Under `bun run` the loader has a record of each module that a statement imports, and require.cache also lists what the loader holds evaluated, unless the name begins
// with `node:` or `bun:`. So `import "ws"` lists "ws". What the library loads for itself does not go through the loader.
Vector<ModuleID> Modules::libraryModulesInRequireCache()
{
    Vector<ModuleID> listed = m_libraryModulesInRequireCache;
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 64> toFollow;
    for (unsigned index = 0; index < m_table.size(); ++index) {
        ModuleID module(index);
        if (data(module).isOfLibrary() || data(module).kind != ModuleKind::SourceText)
            continue;
        // The loader has evaluated a module of the library by the time that the first file of the graph runs.
        if (hasBegunToLoad(module) || isRunningFilesOf(module)) {
            isSeen.quickSet(index);
            toFollow.append(module);
        }
    }
    for (size_t next = 0; next < toFollow.size(); ++next) {
        // What only a mocked module imports is not loaded.
        if (standsForWholeModule(toFollow[next]))
            continue;
        for (const ModuleRequestData& request : m_table.requests(toFollow[next])) {
            ModuleID target = moduleLoadedBy(request);
            if (request.isDeferred || request.isRequire || isSeen.quickSet(target.index()))
                continue;
            if (!data(target).isOfLibrary()) {
                if (data(target).kind == ModuleKind::SourceText)
                    toFollow.append(target);
                continue;
            }
            m_libraryModulesThatStatementsReached.set(target.index());
            if (isListedOnceStatementReaches(target))
                listed.append(target);
        }
    }
    // The loader keeps a module when the file that imported it is taken out.
    for (size_t index : m_libraryModulesThatStatementsReached) {
        if (!isSeen.get(index) && isListedOnceStatementReaches(ModuleID(index)))
            listed.append(ModuleID(index));
    }
    countEvent(*m_instance.vm, RuntimeCounterID::ModulesForRequireCache, m_table.size() + toFollow.size());
    return listed;
}

bool Modules::isListedOnceStatementReaches(ModuleID module) const
{
    return isKeptInRequireCache(module) && !isInRequireCache(module) && !m_libraryModulesTakenFromLoader.get(module.index()) && !m_table.pathText(module).startsWith("bun:"_s);
}

// What a statement has reached stays reached, so at most the first question about a module that is listed walks.
bool Modules::isLibraryModuleInRequireCache(ModuleID module)
{
    if (m_libraryModulesInRequireCache.contains(module))
        return true;
    if (!isListedOnceStatementReaches(module))
        return false;
    if (!m_libraryModulesThatStatementsReached.get(module.index()))
        libraryModulesInRequireCache();
    return m_libraryModulesThatStatementsReached.get(module.index());
}

void Modules::addToRequireCache(ModuleID module, JSValue exports)
{
    ensureLoaded(module).valueInRequireCache.set(*this, exports);
    setRequireCacheValue(module, exports);
    m_libraryModulesInRequireCache.append(module);
}

void Modules::setRequireCacheValue(ModuleID module, JSValue exports)
{
    if (data(module).hasRequireCacheValue())
        ProgramStatics::writeVariable(m_instance, data(module).requireCacheValue(), exports);
}

// The program may store into `exports` of an entry that it holds: from here on every require() reads the entry.
JSObject* Modules::requireCacheEntry(ModuleID module)
{
    if (!data(module).isOfLibrary())
        return moduleObjectMadeByRequire(module);
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSObject* entry = ensureLoaded(module).objectMadeByRequire)
        return entry;
    // The entry of a module that is listed for a statement alone holds the namespace object, and from here on that is what require() answers.
    if (!isInRequireCache(module)) {
        JSObject* namespaceObject = moduleNamespace(module, ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, nullptr);
        addToRequireCache(module, namespaceObject);
    }
    OptionSet<ModuleObjectTrait> traits { ModuleObjectTrait::IsNoCommonJSFile };
    if (data(module).kind == ModuleKind::SourceText)
        traits.add(ModuleObjectTrait::IsESModule);
    ModuleObjects objects;
    host().createModuleObject(m_instance.globalObject, pathString(module), module, traits, objects);
    RETURN_IF_EXCEPTION(scope, nullptr);
    LoadedModule& loaded = ensureLoaded(module);
    objects.module->putDirect(vm, vm.propertyNames->exports, loaded.valueInRequireCache.get());
    loaded.objectMadeByRequire.set(*this, objects.module);
    setRequireCacheValue(module, JSValue());
    return objects.module;
}

std::optional<ModuleID> Modules::findByName(const Identifier& key)
{
    std::optional<ModuleID> found = m_table.findByPath(key.string());
    if (found && hostAnswersInPlaceOf(*found, LoadByName::Import))
        return std::nullopt;
    // What has answered once goes on answering, as an entry of the loader does.
    if (found && !data(*found).isOfLibrary() && host().loadsFileItself && host().loadsFileItself(m_instance.globalObject, key.string()) && !hasRun(*found) && !namespaceIfCreated(*found, ModulePhase::Evaluation))
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
        if (data(importer).kind != ModuleKind::SourceText || !wasLoaded || standsForWholeModule(importer))
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

std::optional<ModuleID> Modules::findWithAttributes(const Identifier& key, const ScriptFetchParameters* fetchParameters, FormWithoutAttributes formWithoutAttributes)
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
    if (module && formWithoutAttributes == FormWithoutAttributes::IsAnotherModule && m_table.pathText(*module) == key.string())
        module = std::nullopt;
    if (std::optional<ModuleID> loaded = findFormThatStatementLoaded(key, *fetchParameters, module))
        module = loaded;
    if (module)
        m_formsAnsweredFirst.add(keyOfLoader, FormAnsweredFirst { key, *module });
    return module;
}

bool Modules::doOtherJobsWait() const
{
    MicrotaskQueue& queue = m_instance.vm->defaultMicrotaskQueue();
    if (!m_isJobOfRoundQueued)
        return !queue.isEmpty();
    return !queue.allOf([&](const QueuedTask& task) {
        return task.arguments()[0] == JSValue(m_promiseOfJobOfRound.get());
    });
}

// The file runs, or it has returned and the graph has yet to run: a job or a tick that the file has queued may be what calls.
bool Modules::hasFileOfGraphBeingLoadedBegun() const
{
    return m_innermostFilesBeingRun || m_rootWhoseFilesRanAhead || m_loadsInFlight.hasSeenFileRunWhileBeingLoaded();
}

// `bun run` has the text of whatever its loader has come to, and reads every other on another thread. While a require() loads a graph it reads each text itself, also for an import() that is called meanwhile.
bool Modules::isTextLeftToRead(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (!isFileOfProgram(file) || m_instance.vm->synchronousModuleQueue() || hostLoadsSynchronously(file))
        return false;
    bool hasNotBegun = isCommonJSFileOrFace(module) ? hasNotBegunAsFile(module) && hasNotBegunAsFile(file) : isNotLoaded(module);
    return hasNotBegun && !loader()->registryEntry(moduleKey(module)) && !loader()->registryEntry(moduleKey(file));
}

void Modules::noteThatTextHasCome(ModuleID module)
{
    if (m_turnsInWhichTextsCame.isEmpty())
        m_turnsInWhichTextsCame.fill(0, m_table.size());
    m_turnsInWhichTextsCame[fileOf(module).index()] = ++m_turnOfTexts;
}

// One task for each text, and each task brings the text that stands first.
void Modules::holdText(JSPromise* promise, ModuleID module, ModuleRecord* requested, std::optional<int64_t> referrerAsyncOrder, uint32_t turnOfRequest)
{
    VM& vm = *m_instance.vm;
    putCallsThatWaitApartInLine();
    uint64_t place = static_cast<uint64_t>(turnOfRequest) << 32 | m_numberOfTextsAskedFor++;
    size_t index = m_textsOnTheirWay.size();
    while (index && m_textsOnTheirWay[index - 1].place > place)
        --index;
    m_textsOnTheirWay.insert(index, TextOnItsWay { place, Strong<JSPromise>(vm, promise), module, Strong<JSCell>(vm, requested), referrerAsyncOrder });
    // In a test run the host brings it, behind the `describe` callbacks of a test file that has just been loaded: deliverTextThatWaits().
    if (host().callInLaterTurn && isTestRun()) {
        host().callInLaterTurn(m_instance.globalObject);
        return;
    }
    auto ticket = vm.deferredWorkTimer->addPendingWork(DeferredWorkTimer::WorkType::ImminentlyScheduled, vm, promise, { });
    vm.deferredWorkTimer->scheduleWorkSoonIfActive(ticket, [](DeferredWorkTimer::Ticket& ticket) {
        if (Modules* modules = modulesOf(ticket.target()->realm()))
            modules->deliverFirstTextOnItsWay();
    });
}

void Modules::deliverFirstTextOnItsWay()
{
    if (m_textsOnTheirWay.isEmpty())
        return;
    TextOnItsWay text = WTF::move(m_textsOnTheirWay.first());
    m_textsOnTheirWay.removeAt(0);
    noteThatTextHasCome(text.module);
    if (text.requested)
        text.promise->fulfill(*m_instance.vm, text.requested.get());
    else
        startImportWhoseTextHasCome(text.promise.get(), text.module, text.referrerAsyncOrder);
}

// While no text is on its way nobody can tell when one comes: they come in the order in which they are asked for. While a require() loads a graph `bun run` reads each text itself.
bool Modules::holdTextOfRequest(JSPromise* fetched, ModuleRecord* requested, ModuleID requester)
{
    ModuleID file = fileOf(requested->module());
    if (m_textsOnTheirWay.isEmpty() || !isFileOfProgram(file) || m_instance.vm->synchronousModuleQueue() || hostLoadsSynchronously(file)) {
        noteThatTextHasCome(file);
        return false;
    }
    holdText(fetched, requested->module(), requested, std::nullopt, turnInWhichTextCame(requester));
    return true;
}

// The text comes a turn of the event loop behind the call: every job that waits has run by then, and so has a graph that has all its texts.
JSPromise* Modules::startImportInLaterTurn(ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    JSPromise* importPromise = JSPromise::create(*m_instance.vm, m_instance.globalObject->promiseStructure());
    m_modulesOfQueuedImports.set(module.index());
    // A job or a tick calls in the turn in which it runs.
    std::optional<ModuleID> fileThatCalls = m_innermostFilesBeingRun ? fileOrFaceEvaluatedIn(*m_innermostFilesBeingRun) : std::nullopt;
    holdText(importPromise, module, nullptr, referrerAsyncOrder, fileThatCalls ? turnInWhichTextCame(*fileThatCalls) : m_turnOfTexts);
    return importPromise;
}

// The promise that the caller holds follows the one of the loader: createPromiseOfProgram().
void Modules::startImportWhoseTextHasCome(JSPromise* importPromise, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    m_modulesOfQueuedImports.clear(module.index());
    JSPromise* loaded = startImport(module, referrerAsyncOrder, ImportCaller::Host, TextOfImport::HasCome);
    if (scope.exception()) [[unlikely]] {
        importPromise->rejectWithCaughtException(vm, scope);
        return;
    }
    scope.release();
    settleAsLoaderDoes(m_instance.globalObject, importPromise, loaded);
}

// `stateOfWait`: ModuleHost::factoryThatWaits' state, where a call that was held behind a factory goes on.
JSPromise* Modules::startImport(ModuleID module, std::optional<int64_t> referrerAsyncOrder, ImportCaller caller, TextOfImport text, JSPromise* promiseOfCaller, JSValue stateOfWait, std::optional<bool> answerOfWalkAlreadyMade)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto promiseForCaller = [&] {
        return promiseOfCaller ? promiseOfCaller : JSPromise::create(*m_instance.vm, realm->promiseStructure());
    };
    if (isCommonJSFileOrFace(module)) {
        if (ModuleID file = fileOf(module); data(file).runsAgainAfterThrow() && status(file) == ModuleStatus::Evaluating && !isAwaitedByImport(file)) [[unlikely]]
            ensureLoaded(file).wasFirstImportedByCallWhileItRan = true;
    }

    bool walkStopped = answerOfWalkAlreadyMade ? *answerOfWalkAlreadyMade : runMocksBelow(module);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (m_innermostFilesBeingRun) [[unlikely]] {
        handGraphsToLoader();
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (m_innermostFilesBeingRun->isAheadOfStepsOfLoader) {
            handSynchronousLoadToLoader(*m_innermostFilesBeingRun);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        }
    }
    // The loader of `bun run` waits for a factory before it goes on with the graph.
    FactoryAskedForBy askedBy = text == TextOfImport::MayComeLater || stateOfWait ? FactoryAskedForBy::ImportCall : FactoryAskedForBy::ImportCallBehindItsText;
    JSPromise* factories = factoryThatWaitsBelow(module, askedBy, stateOfWait);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    // The factory has settled meanwhile, or whoever asks was called from it.
    while (!factories && walkStopped) [[unlikely]] {
        walkStopped = runMocksBelow(module);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        factories = factoryThatWaitsBelow(module, askedBy, stateOfWait);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    }
    if (factories) [[unlikely]] {
        VM& vm = *m_instance.vm;
        JSPromise* promise = promiseForCaller();
        JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), importBehindFactories, ImplementationVisibility::Private);
        MarkedArgumentBuffer call;
        call.append(jsNumber(module.index()));
        call.append(referrerAsyncOrder ? jsNumber(static_cast<double>(*referrerAsyncOrder)) : jsUndefined());
        call.append(jsBoolean(caller == ImportCaller::Host));
        call.append(jsNumber(static_cast<uint32_t>(text)));
        call.append(stateOfWait);
        JSArray* heldCall = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), call);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        factories->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), promise, heldCall);
        return promise;
    }
    // An executable has every text, so a call finds its graph at once. What a file of a graph that is being loaded asks for, itself or in a job or a tick that it has queued, still comes a turn later,
    // and a later call does not overtake a text that is on its way.
    if (text == TextOfImport::MayComeLater && (!m_textsOnTheirWay.isEmpty() || hasFileOfGraphBeingLoadedBegun()) && isTextLeftToRead(module)) [[unlikely]]
        return startImportInLaterTurn(module, referrerAsyncOrder);
    // `bun test` reads the text on another thread. What the jobs behind its coming call for is at hand.
    if (text == TextOfImport::MayComeLater && !hasTextAtHand() && textOfImportComesInLaterTurn(module, AskedBy::Import)) [[unlikely]] {
        JSPromise* promise = JSPromise::create(*m_instance.vm, realm->promiseStructure());
        if (caller == ImportCaller::Host)
            promise->markAsHandled();
        holdTextOfCall(promise, module, nullptr, referrerAsyncOrder, caller);
        return promise;
    }
    bool isFile = isImportedByLoader(module);
    bool isBuiltinModule = !isFile && isBuiltinModuleOfLoader(module);
    noteRequestByLoader(module);
    noteLoadOfGraph(module);
    if (isFile || isBuiltinModule) {
        VM& vm = *m_instance.vm;
        bool isBuiltinModuleThatLoaderHas = isBuiltinModule && !isBuiltinModuleNewToLoader(module);
        bool isCalledByProgram = caller == ImportCaller::CompiledCode;
        // A call is no part of the graph that a require() loads: its steps are jobs, not steps of that load, and what it names runs when the require() has returned.
        // What has begun, and a file that may be imported while it runs, still go the way of `bun run`, inside the require().
        bool hasBegun = status(module) >= ModuleStatus::Evaluating || status(fileOf(module)) >= ModuleStatus::Evaluating;
        bool isNoPartOfLoad = isCalledByProgram && !hasBegun && !(isCommonJSFileOrFace(module) && data(fileOf(module)).mayBeImportedWhileItRuns());
        SetForScope noSynchronousLoad(vm.m_synchronousModuleQueueSetAside, isNoPartOfLoad ? vm.m_synchronousModuleQueue : vm.m_synchronousModuleQueueSetAside);
        if (isCalledByProgram && (!isBuiltinModule || isBuiltinModuleThatLoaderHas) && loaderOnlyHandsOverNamespace(module)) {
            JSPromise* promise = promiseForCaller();
            if (promise->isThenFastAndNonObservable()) [[likely]] {
                queueStepOfImport(promise, moduleForJob(module), std::size(jobsOfImportOfEvaluatedModule));
                return promise;
            }
        }
        // `import("./a.mjs")`, and in a later job `import("./b.mjs")`, where each imports the other: the graph that was asked for first runs first, so the cycle is entered at a.
        if (isCalledByProgram && isFile && !m_loadsInFlight.isEmpty() && !m_innermostFilesBeingRun && m_textsOnTheirWay.isEmpty()) {
            if (LoadInFlight* earlier = findEarlierLoadAbove(module)) {
                JSPromise* promise = promiseForCaller();
                m_importsBehindLoads.append(ImportBehindLoad { earlier->root, module, referrerAsyncOrder, Strong<JSPromise>(vm, promise) });
                return promise;
            }
        }
        // A call is no part of the graph that a require() loads: it waits for the file as it does where nothing is loaded synchronously.
        bool isCalledUnderRequire = vm.synchronousModuleQueue() && isCommonJSFileOrFace(module) && isEvaluatedByLoader(fileOf(module));
        if (isCalledUnderRequire || (asyncLoadMustWaitFor(module) && !loader()->registryEntry(moduleKey(module)))) [[unlikely]] {
            JSPromise* promise = promiseForCaller();
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
        JSPromise* loaded = askLoaderForImport(module, referrerAsyncOrder, isCalledByProgram ? nullptr : promiseOfCaller);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (!isCalledByProgram)
            return loaded;
        if (promiseOfCaller) {
            scope.release();
            settleAsLoaderDoes(realm, promiseOfCaller, loaded);
            return promiseOfCaller;
        }
        RELEASE_AND_RETURN(scope, createPromiseOfProgram(realm, loaded));
    }

    if (data(module).reachesUncompiledCode() && status(module) == ModuleStatus::Unlinked) {
        ModuleRecord* record = registerWithLoader(module);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        SetForScope rootThatCompiledCodeLoads(m_rootThatCompiledCodeLoads, std::optional(module));
        JSPromise* promise = loader()->requestImportModuleForKey(realm, record->moduleKey(), Identifier(), nullptr, nullptr, false, referrerAsyncOrder.value_or(-1));
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        didStartLoad(promise, module);
        return promise;
    }

    JSPromise* importPromise = JSPromise::create(*m_instance.vm, realm->promiseStructure());
    m_modulesOfQueuedImports.set(module.index());
    queueImportJob(realm, importPromise, module, referrerAsyncOrder);
    return importPromise;
}

bool Modules::leadsToModuleLeftToRun(const ModuleRequestData& request) const
{
    return !request.isHoisted && !request.isRequire && status(request.targetModule()) < ModuleStatus::Evaluating;
}

const BitVector& Modules::modulesLeftToRunBelow(LoadInFlight& load)
{
    if (load.modulesLeftToRun)
        return *load.modulesLeftToRun;
    load.modulesLeftToRun = makeUnique<BitVector>(m_table.size());
    BitVector& below = *load.modulesLeftToRun;
    below.quickSet(load.root.index());
    Vector<ModuleID, 16> modules { load.root };
    while (!modules.isEmpty()) {
        for (const ModuleRequestData& request : m_table.requests(modules.takeLast())) {
            if (leadsToModuleLeftToRun(request) && !below.quickSet(request.target()))
                modules.append(request.targetModule());
        }
    }
    return below;
}

bool Modules::leadsTo(ModuleID from, ModuleID to)
{
    if (from == to)
        return true;
    const uint32_t isSeen = takeMarkOfSearch();
    std::span<uint32_t> placesSeen = m_placesSeenByPlans.mutableSpan();
    placesSeen[from.index()] = isSeen;
    Vector<ModuleID, 16> modules { from };
    while (!modules.isEmpty()) {
        for (const ModuleRequestData& request : m_table.requests(modules.takeLast())) {
            if (!leadsToModuleLeftToRun(request) || std::exchange(placesSeen[request.target()], isSeen) == isSeen)
                continue;
            if (request.targetModule() == to)
                return true;
            modules.append(request.targetModule());
        }
    }
    return false;
}

// Every cycle closes with a request for a module that the search is going through.
bool Modules::isAboveCycleLeftToRun(LoadInFlight& load, ModuleID root)
{
    if (!load.modulesAboveNoCycle)
        load.modulesAboveNoCycle = makeUnique<BitVector>(m_table.size());
    BitVector& isAboveNone = *load.modulesAboveNoCycle;
    if (isAboveNone.quickGet(root.index()))
        return false;

    const uint32_t isBeingGoneThrough = takeMarkOfSearch();
    std::span<uint32_t> placesSeen = m_placesSeenByPlans.mutableSpan();
    struct Visit {
        ModuleID module;
        uint32_t nextRequest;
    };
    Vector<Visit, 16> visits { Visit { root, 0 } };
    placesSeen[root.index()] = isBeingGoneThrough;
    while (!visits.isEmpty()) {
        Visit& visit = visits.last();
        std::span<const ModuleRequestData> requests = m_table.requests(visit.module);
        if (visit.nextRequest == requests.size()) {
            isAboveNone.quickSet(visit.module.index());
            visits.removeLast();
            continue;
        }
        const ModuleRequestData& request = requests[visit.nextRequest++];
        if (!leadsToModuleLeftToRun(request) || isAboveNone.quickGet(request.target()))
            continue;
        if (std::exchange(placesSeen[request.target()], isBeingGoneThrough) == isBeingGoneThrough)
            return true;
        visits.append(Visit { request.targetModule(), 0 });
    }
    return false;
}

// The texts come in the order in which they are asked for: that of the root, that of the module, then what each text that has come imports. What a module asks for belongs to whoever asked for the module.
// A graph is loaded when the last of its texts has come, and of two graphs that end with one text the one that asked for it hears of it first.
bool Modules::asksForLastTextBelow(const LoadInFlight& load, ModuleID module)
{
    const uint32_t isAskedForByLoad = takeMarkOfSearch();
    const uint32_t isAskedForByCall = takeMarkOfSearch();
    std::span<uint32_t> placesSeen = m_placesSeenByPlans.mutableSpan();
    placesSeen[load.root.index()] = isAskedForByLoad;
    placesSeen[module.index()] = isAskedForByCall;
    Vector<ModuleID, 32> inOrder { load.root, module };
    for (size_t next = 0; next < inOrder.size(); ++next) {
        uint32_t asker = placesSeen[inOrder[next].index()];
        for (const ModuleRequestData& request : m_table.requests(inOrder[next])) {
            if (!leadsToModuleLeftToRun(request))
                continue;
            uint32_t& seen = placesSeen[request.target()];
            // The root has asked for the module when the text that the call asked for comes.
            if (!next && request.targetModule() == module)
                seen = isAskedForByLoad;
            if (seen == isAskedForByLoad || seen == isAskedForByCall)
                continue;
            seen = asker;
            inOrder.append(request.targetModule());
        }
    }
    // A graph that does not reach the last text is loaded sooner.
    return placesSeen[inOrder.last().index()] == isAskedForByLoad && leadsTo(module, inOrder.last());
}

// A cycle is entered by the graph that is loaded first. What two graphs share outside of a cycle runs the same whichever of them comes to it first, and the loader's own timing decides.
// An executable has every text, and its loader would come to the end of the smaller graph first, whichever `bun run` loads first.
Modules::LoadInFlight* Modules::findEarlierLoadAbove(ModuleID module)
{
    if (status(module) >= ModuleStatus::Evaluating)
        return nullptr;
    LoadInFlight* load = m_loadsInFlight.loadThatCameFirst();
    if (!load || load->stepsOfLoaderAtCall == m_stepsOfLoader || !m_loadsInFlight.isBeingLoaded(*load))
        return nullptr;
    if (load->root == module)
        return load;
    if (modulesLeftToRunBelow(*load).quickGet(module.index()) && isAboveCycleLeftToRun(*load, module) && asksForLastTextBelow(*load, module))
        return load;
    return nullptr;
}

void Modules::makeImportAgain(const ImportBehindLoad& import)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSPromise* promise = import.promise.get();
    JSPromise* loaded = startImport(import.module, import.referrerAsyncOrder, ImportCaller::CompiledCode, TextOfImport::HasCome, promise);
    if (scope.exception()) [[unlikely]] {
        if (!vm.isTerminationException(scope.exception()))
            promise->rejectWithCaughtException(vm, scope);
        return;
    }
    if (loaded != promise)
        settleAsLoaderDoes(m_instance.globalObject, promise, loaded);
}

// The loader evaluates the root in the job in which it links it, and a call that is made now comes to its graph later than that.
void Modules::resumeImportsBehindLoad(ModuleID root, bool graphRunsInThisJob)
{
    if (m_importsBehindLoads.isEmpty()) [[likely]]
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    Vector<ImportBehindLoad, 2> due;
    m_importsBehindLoads.removeAllMatching([&](ImportBehindLoad& import) {
        if (import.root != root)
            return false;
        due.append(WTF::move(import));
        return true;
    });
    for (ImportBehindLoad& import : due) {
        // The module runs with the graph: the call has nothing of its own to load, and settles right behind the call for the graph.
        if (graphRunsInThisJob && data(root).isSynchronous() && import.promise->isThenFastAndNonObservable()) {
            queueStepOfImport(import.promise.get(), moduleForJob(import.module), numberOfJobsBehindEvaluation + 1);
            m_importsBehindEvaluations.append(WTF::move(import));
            continue;
        }
        makeImportAgain(import);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// Compiled code may come to the root first, by a road on which the loader links nothing.
void Modules::resumeImportsBehindSettledLoads()
{
    if (m_importsBehindLoads.isEmpty()) [[likely]]
        return;
    Vector<ModuleID, 2> roots;
    for (const ImportBehindLoad& import : m_importsBehindLoads) {
        bool isBeingLoaded = false;
        m_loadsInFlight.forEachOfRoot(import.root, [&](LoadInFlight& load) {
            isBeingLoaded |= m_loadsInFlight.isBeingLoaded(load);
        });
        if (!isBeingLoaded && !roots.contains(import.root))
            roots.append(import.root);
    }
    for (ModuleID root : roots)
        resumeImportsBehindLoad(root);
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

JSPromise* Modules::askLoaderForImport(ModuleID module, std::optional<int64_t> referrerAsyncOrder, JSPromise* resultPromise)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    // A file of the graph was taken out of the program between the call and this step. The loader reads the graph from disk, as for a call that comes now: import().
    if (isLeftToHost(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, loader()->requestImportModuleForKey(m_instance.globalObject, keyWithoutForm(fileOf(module)), Identifier(), nullptr, nullptr, false, referrerAsyncOrder.value_or(-1), nullptr, resultPromise));
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
    JSPromise* loaded = scope.exception() ? nullptr : loader()->requestImportModuleForKey(m_instance.globalObject, record->moduleKey(), Identifier(), nullptr, nullptr, false, referrerAsyncOrder.value_or(-1), record, resultPromise);
    setIsAskingHost(wasAskingHost);
    RETURN_IF_EXCEPTION(scope, nullptr);
    didStartLoad(loaded, module);
    return loaded;
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
    if (importPromise == m_promiseOfJobOfRound.get()) [[unlikely]]
        return runJobOfRound(module);
    if (!m_callsToMakeAgain.isEmpty() && makeCallAgain(importPromise)) [[unlikely]]
        return;
    if (!m_importsBehindEvaluations.isEmpty()) [[unlikely]] {
        size_t index = m_importsBehindEvaluations.findIf([&](const ImportBehindLoad& import) { return import.promise.get() == importPromise; });
        if (index != notFound) {
            ImportBehindLoad import = WTF::move(m_importsBehindEvaluations[index]);
            m_importsBehindEvaluations.removeAt(index);
            // The graph has failed.
            if (!loaderOnlyHandsOverNamespace(module))
                return makeImportAgain(import);
        }
    }
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
    std::unique_ptr<ModuleLinkWalk> walk = takeKeptLinkWalk(root);
    if (!walk) {
        countEvent(*m_instance.vm, RuntimeCounterID::LinksBelowRoots);
        walk = makeUnique<ModuleLinkWalk>(*this, root);
    }
    return walk;
}

std::unique_ptr<ModuleLinkWalk> Modules::takeKeptLinkWalk(ModuleID root)
{
    size_t index = m_linkWalksOfLoads.findIf([&](const std::unique_ptr<ModuleLinkWalk>& kept) { return kept->root() == root; });
    if (index == notFound)
        return nullptr;
    std::unique_ptr<ModuleLinkWalk> walk = WTF::move(m_linkWalksOfLoads[index]);
    m_linkWalksOfLoads.removeAt(index);
    if (!walk->bringUpToDate(*this))
        return nullptr;
    return walk;
}

// Several graphs are loaded at a time. A load that fails before it links leaves its walk behind.
void Modules::keepLinkWalk(std::unique_ptr<ModuleLinkWalk>&& walk)
{
    m_linkWalksOfLoads.append(WTF::move(walk));
}

void Modules::continueImport(JSPromise* importPromise, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    // See askLoaderForImport().
    if (isLeftToHost(module)) [[unlikely]] {
        JSPromise* loaded = askLoaderForImport(module, referrerAsyncOrder);
        if (scope.exception()) [[unlikely]] {
            importPromise->rejectWithCaughtException(vm, scope);
            return;
        }
        scope.release();
        importPromise->resolve(m_instance.globalObject, vm, loaded);
        return;
    }

    if (status(module) == ModuleStatus::Evaluating) {
        // The body of the module is on the stack below this job, so a job queued now would run, and queue itself again, before the body can go on.
        vm.whenIdle([realm = m_instance.globalObject, importPromise = Strong<JSPromise>(vm, importPromise), module, referrerAsyncOrder] {
            queueImportJob(realm, importPromise.get(), module, referrerAsyncOrder);
        });
        return;
    }

    m_modulesOfQueuedImports.clear(module.index());
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
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    auto fetchParameters = JSModuleLoader::fetchParametersFromImportOptions(realm, options);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (!fetchParameters)
        return fetchParameters.error();
    Identifier key;
    {
        // The host tells a name that a program wrote in a call from one that stands in an import statement.
        SetForScope isResolvingImportCall(m_instance.vm->aotState().isResolvingImportCall, true);
        key = loader()->resolve(realm, text, pathString(referrer), nullptr, true);
    }
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    if (std::optional<ModuleID> found = *fetchParameters ? findWithAttributes(key, fetchParameters->get()) : findByName(key)) {
        willAnswerWithModule(realm, *found, LoadByName::Import);
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
        // An import() that names a file in its text names the face of the file.
        RELEASE_AND_RETURN(scope, import(m_table.faceOfFile(*found).value_or(*found), referrer));
    }
    RELEASE_AND_RETURN(scope, importUncompiled(key, referrer, WTF::move(*fetchParameters)));
}

JSPromise* Modules::importUncompiled(const Identifier& key, ModuleID referrer, RefPtr<ScriptFetchParameters>&& fetchParameters, JSValue stateOfWait)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    VM& vm = *m_instance.vm;
    auto askLoader = [&] {
        SetForScope frameOfImportThatAsksLoader(vm.aotState().frameOfImportThatAsksLoader, vm.topCallFrame);
        return loader()->requestImportModuleForKey(realm, key, moduleKey(referrer), WTF::move(fetchParameters), nullptr, false, asyncEvaluationOrder(referrer).value_or(-1));
    };
    std::optional<ModuleID> found = fetchParameters ? std::nullopt : m_table.findByPath(key.string());
    if (found && errorOfLoadEndedByMock(*found)) [[unlikely]]
        RELEASE_AND_RETURN(scope, rejectImportWithErrorOfLoad(*found, ImportCaller::CompiledCode));
    // A fetch of the loader waits for the factories that it starts alone: nothing says there who called. Here it shows, so the call waits as one that names the file does in startImport().
    if (found) {
        JSPromise* factories = factoryThatWaitsBelow(*found, FactoryAskedForBy::ImportCall, stateOfWait);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (factories) [[unlikely]] {
            JSPromise* promise = JSPromise::create(vm, realm->promiseStructure());
            JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), importByKeyBehindFactories, ImplementationVisibility::Private);
            MarkedArgumentBuffer call;
            call.append(jsString(vm, key.string()));
            call.append(jsNumber(referrer.index()));
            call.append(stateOfWait);
            JSArray* heldCall = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), call);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
            factories->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), promise, heldCall);
            return promise;
        }
    }
    SetForScope askerLoadsCompiledFilesAlone(vm.aotState().askerLoadsCompiledFilesAlone, loadsCompiledFilesAlone(referrer));
    JSPromise* loaded = askLoader();
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    tellHostOfImport(loaded, key.string());
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    RELEASE_AND_RETURN(scope, createPromiseOfProgram(realm, loaded));
}

bool Modules::loadsCompiledFilesAlone(ModuleID module) const
{
    return std::ranges::binary_search(m_instance.tables().modulesThatLoadCompiledFilesAlone(), module.index());
}

static bool isText(StringView path)
{
    return path.startsWith("data:"_s);
}

bool Modules::isTextNamedByProgram(StringView url) const
{
    return isText(url) && std::ranges::any_of(m_table.modulesOfUncompiledCode(), [&](const UncompiledModuleData& entry) {
        return url == m_table.pathText(ModuleID(entry.module));
    });
}

// A package loads its own files at any later time, so the first one the build named as external admits all. A text has no files of its own.
const Identifier& Modules::resolvedKey(ModuleID external)
{
    ASSERT(data(external).isUncompiledCode());
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    m_runsUncompiledCode = true;
    if (!m_admitsCodeOutsideImage && !isText(m_table.pathText(external)))
        m_admitsCodeOutsideImage = true;
    LoadedModule& loaded = ensureLoaded(external);
    if (!loaded.resolvedKey.isNull())
        return loaded.resolvedKey;
    Identifier key = loader()->resolve(m_instance.globalObject, pathString(external), pathString(*data(external).referrerModule()), nullptr, true);
    RETURN_IF_EXCEPTION(scope, loaded.resolvedKey);
    loaded.resolvedKey = WTF::move(key);
    loaded.keyOfLatestLoad = loaded.resolvedKey;
    // A file under a query is one more instance of the file. By the name as written, which is what the build went by.
    m_runsCopyFromDisk |= m_table.pathText(external).contains('?');
    return loaded.resolvedKey;
}

void Modules::forgetWhatNamesResolvedTo()
{
    for (const UncompiledModuleData& external : m_table.modulesOfUncompiledCode()) {
        if (LoadedModule* loaded = loadedIfExists(ModuleID(external.module)))
            loaded->resolvedKey = Identifier();
    }
}

void Modules::admitCodeNamedBy(JSValue specifier)
{
    if (m_admitsCodeOutsideImage || !specifier.isString())
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto text = asString(specifier)->view(m_instance.globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    m_admitsCodeOutsideImage = std::ranges::any_of(m_table.modulesOfUncompiledCode(), [&](const UncompiledModuleData& entry) {
        String name = m_table.pathText(ModuleID(entry.module));
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
    if (data(root).isLoadedByLoader() || m_fileWhoseJobsWaitForLoader || m_rootWhoseFilesRanAhead == root || !m_textsOnTheirWay.isEmpty())
        return true;
    // No code of the program has run when the host asks for the module that it starts with.
    if (isEntryPointOfRealm(root))
        return false;
    if (vm.synchronousModuleQueue() || numberOfGraphsBeingLoaded() > 1)
        return true;
    // No step of the loader waits for a graph whose root has not run to its end: it goes on with a job of the program.
    for (const LoadInFlight& load : m_loadsInFlight) {
        if (load.isLoaded && status(load.root) != ModuleStatus::Evaluating && status(load.root) != ModuleStatus::EvaluatingAsync)
            return true;
    }
    // With one graph being loaded the steps of the loader that wait are its own, or come behind the evaluation of an entry point. None of them runs code of the program before the graph is loaded.
    return !vm.defaultMicrotaskQueue().allOf([&](const QueuedTask& task) {
        if (task.job() >= InternalMicrotask::ModuleRegistryFetchSettled && task.job() <= InternalMicrotask::ImportModuleNamespace)
            return true;
        if (m_isJobOfRoundQueued && task.arguments()[0] == JSValue(m_promiseOfJobOfRound.get()))
            return true;
        if (task.job() != InternalMicrotask::PromiseResolveThenableJobFast)
            return false;
        auto* promise = dynamicDowncast<JSPromise>(task.arguments()[0]);
        const LoadInFlight* load = promise ? m_loadsInFlight.find(promise) : nullptr;
        return load && !load->isLoaded;
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

// `import data from "./data.txt" with { type: "toml" }`: as the parser makes them of the statement.
RefPtr<ScriptFetchParameters> Modules::fetchParametersOf(ModuleID external) const
{
    String type = m_table.typeAttribute(external);
    std::optional<ScriptFetchParameters::Type> kind = ScriptFetchParameters::parseType(type);
    if (!kind)
        return nullptr;
    if (*kind == ScriptFetchParameters::Type::HostDefined)
        return ScriptFetchParameters::create(type);
    return ScriptFetchParameters::create(*kind);
}

// The loader keeps one module for a name and a kind of type.
static ModuleMapKey placeInLoader(const Identifier& key, const ScriptFetchParameters* fetchParameters)
{
    return { key.impl(), fetchParameters ? fetchParameters->type() : ScriptFetchParameters::Type::JavaScript };
}

ModuleRegistryEntry* Modules::registryEntry(const Identifier& key, const ScriptFetchParameters* fetchParameters)
{
    if (!fetchParameters)
        return loader()->registryEntry(key);
    return loader()->moduleMap().get(placeInLoader(key, fetchParameters)).get();
}

void Modules::appendRequestsForLoader(ModuleID module, ModuleRecord* record, ModuleGraphLoadingState& load)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    noteStepOfLoader();
    if (!isLoadedWithRequests(module) || !record->requestedModules().isEmpty())
        return;
    // What only a mocked module imports is not loaded.
    if (isMocked(module) || m_modulesWithMockToRun.get(module.index()))
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
        bool endsAtStatement = statementThatEndsLoad(module).has_value();
        bool isWalked = mustWaitForFile || endsAtStatement || canTellHowGraphIsLoaded(module);
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
        // The host has asked for the graph, and no import(). A job that runs between the steps of the load may require a part of it. A root whose files ran ahead counts as it is.
        if (!m_instance.vm->synchronousModuleQueue() && m_rootWhoseFilesRanAhead != module && !m_loadsInFlight.hasRoot(module))
            didStartLoad(load.promise(), module, SettlingOfLoad::IsLookedFor);
        m_loadsInFlight.forEachOfRoot(module, [](LoadInFlight& load) {
            load.isWalked = true;
        });
        if (!data(module).isLoadedByLoader() && m_rootWhoseFilesRanAhead != module && !mustWaitForFile && !endsAtStatement && planLevelsOfLoad(module)) {
            m_levelsOfLoads.get(module)->plan->load.set(*m_instance.vm, &load);
            appendNextOnWay();
            return;
        }
    }

    countEvent(*m_instance.vm, RuntimeCounterID::CompiledModulesWalkedByLoader);
    UncheckedKeyHashSet<ModuleMapKey, ModuleMapHash> keys;
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
                record->setHasRequestThatFindsNothing();
                return;
            }
            if (std::optional<ModuleID> compiled = m_table.findByPath(keptAlive.last().string()); compiled && isCommonJSFileOrFace(*compiled)) {
                noteLoadOfFile(*compiled, loader()->registryEntry(keptAlive.last()) ? LoadOfFile::HasComeToFile : LoadOfFile::IsRequested);
                RETURN_IF_EXCEPTION(scope, void());
                noteRequestByLoader(*compiled);
                if (!m_isMainCommonJSModuleDecided)
                    m_firstFilesOfLoads.append(fileOf(*compiled));
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
        RefPtr<ScriptFetchParameters> fetchParameters = targetData.isUncompiledCode() ? fetchParametersOf(target) : nullptr;
        if (!keys.add(placeInLoader(keptAlive.last(), fetchParameters.get())).isNewEntry)
            continue;
        record->appendRequestedModule(keptAlive.last(), WTF::move(fetchParameters), request.isDeferred ? ModulePhase::Defer : ModulePhase::Evaluation);
        if (!targetData.isUncompiledCode())
            record->didRequestCompiledModule(keptAlive.last(), target);
    }
    if (std::optional<ModuleFailureID> statement = statementThatEndsLoad(module); statement && m_table.failure(*statement).importerModule() == module) [[unlikely]]
        RELEASE_AND_RETURN(scope, appendRequestThatFindsNothing(*statement, record));
}

// A file that is no program stands in the table as a statement of its own that names it: its load ends where it is fetched.
std::optional<ModuleFailureID> Modules::statementThatEndsLoad(ModuleID module) const
{
    std::optional<ModuleFailureID> endOfLoad = m_table.loadFailure(module);
    if (!endOfLoad) [[likely]]
        return std::nullopt;
    const ModuleFailureData& failure = m_table.failure(*endOfLoad);
    if (ProgramStrings::text(m_table.programTables(), failure.specifierString()) == m_table.pathText(failure.importerModule()))
        return std::nullopt;
    return endOfLoad;
}

void Modules::appendRequestThatFindsNothing(ModuleFailureID statement, ModuleRecord* record)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwFailure(statement, false);
    Exception* exception = scope.exception();
    RELEASE_ASSERT(exception);
    TRY_CLEAR_EXCEPTION(scope, void());
    Identifier name = Identifier::fromString(vm, ProgramStrings::text(m_table.programTables(), m_table.failure(statement).specifierString()));
    JSModuleLoader::attachErrorInfo(m_instance.globalObject, exception, nullptr, name, ScriptFetchParameters::Type::JavaScript, JSModuleLoader::ModuleFailure::Kind::Instantiation);
    loader()->addResolutionFailure(vm, { record->moduleKey().impl(), name.impl() }, exception->value());
    record->appendRequestedModule(name, nullptr, ModulePhase::Evaluation);
    record->setHasRequestThatFindsNothing();
}

// The loader forgets the failure with the entry of the module. The record stays, and names the request as before.
void Modules::keepFailureOfRequest(ModuleID module, ModuleRecord* record, const Identifier& name)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (loader()->hasResolutionFailure({ record->moduleKey().impl(), name.impl() }))
        return;
    std::optional<ModuleFailureID> statement = statementThatEndsLoad(module);
    if (statement && m_table.failure(*statement).importerModule() == module && name.string() == ProgramStrings::text(m_table.programTables(), m_table.failure(*statement).specifierString()))
        throwFailure(*statement, false);
    else {
        for (const ModuleRequestData& request : m_table.imports(module)) {
            ModuleID target = request.targetModule();
            if (!data(target).isUncompiledCode() || name.string() != m_table.pathText(target))
                continue;
            resolvedKey(target);
            break;
        }
    }
    Exception* exception = scope.exception();
    if (!exception)
        return;
    TRY_CLEAR_EXCEPTION(scope, void());
    JSModuleLoader::attachErrorInfo(m_instance.globalObject, exception, nullptr, name, ScriptFetchParameters::Type::JavaScript, JSModuleLoader::ModuleFailure::Kind::Instantiation);
    loader()->addResolutionFailure(vm, { record->moduleKey().impl(), name.impl() }, exception->value());
}

// In the order in which the graph comes to them. The modules keep the keys alive.
struct Modules::RequestsOfUncompiledCode {
    struct Request {
        UniquedStringImpl* key;
        RefPtr<ScriptFetchParameters> fetchParameters;
    };

    UncheckedKeyHashSet<ModuleMapKey, ModuleMapHash> seen;
    WTF::Vector<Request, 8> list;
};

// Throws where a name does not resolve.
void Modules::collectRequestsOfUncompiledCode(ModuleID module, RequestsOfUncompiledCode& requests)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    // What only a mocked module imports is not loaded.
    auto isOrWillBeMocked = [&](ModuleID importer) { return isMocked(importer) || m_modulesWithMockToRun.get(importer.index()); };
    if (isOrWillBeMocked(module))
        return;
    BitVector visited(m_table.size());
    visited.quickSet(module.index());
    Vector<std::span<const ModuleRequestData>, 16> path { m_table.requests(module) };
    while (!path.isEmpty()) {
        std::span<const ModuleRequestData>& left = path.last();
        if (left.empty()) {
            path.removeLast();
            continue;
        }
        ModuleID target = left.front().targetModule();
        left = left.subspan(1);
        if (!data(target).reachesUncompiledCode())
            continue;
        if (data(target).isUncompiledCode()) {
            const Identifier& key = resolvedKey(target);
            RETURN_IF_EXCEPTION(scope, void());
            RefPtr<ScriptFetchParameters> fetchParameters = fetchParametersOf(target);
            if (requests.seen.add(placeInLoader(key, fetchParameters.get())).isNewEntry)
                requests.list.append({ key.impl(), WTF::move(fetchParameters) });
        } else if (status(target) == ModuleStatus::Unlinked && !isOrWillBeMocked(target) && !visited.quickSet(target.index()))
            path.append(m_table.requests(target));
    }
}

// The load ends where the loader comes to such code, and the files that stand behind it never run.
bool Modules::comesToCodeThatDoesNotLoad(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    RequestsOfUncompiledCode requests;
    collectRequestsOfUncompiledCode(root, requests);
    if (scope.exception()) [[unlikely]] {
        TRY_CLEAR_EXCEPTION(scope, true);
        return true;
    }
    for (const auto& request : requests.list) {
        if (ModuleRegistryEntry* entry = registryEntry(Identifier::fromUid(*m_instance.vm, request.key), request.fetchParameters.get()); entry && entry->fetchError())
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
    RequestsOfUncompiledCode requests;
    collectRequestsOfUncompiledCode(module, requests);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (auto& request : requests.list) {
        // A statement of a test file that the loader links: a CommonJS file that it names has been come to by the loader, as under `bun run`.
        if (std::optional<ModuleID> compiled = m_table.findByPath(String(request.key)); compiled && isCommonJSFileOrFace(*compiled)) {
            noteLoadOfFile(*compiled, loader()->registryEntry(Identifier::fromUid(vm, request.key)) ? LoadOfFile::HasComeToFile : LoadOfFile::IsRequested);
            RETURN_IF_EXCEPTION(scope, nullptr);
            noteRequestByLoader(*compiled);
            if (!m_isMainCommonJSModuleDecided)
                m_firstFilesOfLoads.append(fileOf(*compiled));
        }
        record->appendRequestedModule(Identifier::fromUid(vm, request.key), WTF::move(request.fetchParameters), ModulePhase::Evaluation);
    }
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
    ModuleRegistryEntry* entry = registryEntry(key, fetchParametersOf(external).get());
    AbstractModuleRecord* record = entry ? entry->record() : nullptr;
    // A statement of a test file that the loader links, for a file of this image that nothing has taken out: the loader does not keep an entry for what the image answers.
    if (!record) {
        if (std::optional<ModuleID> compiled = m_table.findByPath(key.string()); compiled && !isEvicted(*compiled))
            record = recordForResolution(moduleWithExportsOf(fileOf(*compiled)));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
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

JSObject* Modules::uncompiledNamespace(ModuleID external, ModulePhase phase, NamespaceUse use, bool shouldPreventExtensions)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    AbstractModuleRecord* record = uncompiledRecord(external);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (dynamicDowncast<ModuleRecord>(record))
        RELEASE_AND_RETURN(scope, record->moduleNamespaceObject(m_instance.globalObject, phase));
    // The engine binds a name without the object. Whoever shows the object first says whether it takes new properties: an import says no, require() yes.
    bool isShown = use == NamespaceUse::IsShownToProgram;
    JSModuleNamespaceObject* namespaceObject = record->getModuleNamespace(m_instance.globalObject, phase, isShown && shouldPreventExtensions);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (phase != ModulePhase::Evaluation)
        return namespaceObject;
    bool wasAskedFor = m_uncompiledNamespacesAskedFor.set(external.index());
    if (!isShown) {
        if (!wasAskedFor)
            m_uncompiledNamespacesNotShownYet.set(external.index());
        return namespaceObject;
    }
    if (m_uncompiledNamespacesNotShownYet.clear(external.index()) && shouldPreventExtensions) {
        JSObject::preventExtensions(namespaceObject, m_instance.globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return namespaceObject;
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

JSC_DEFINE_JIT_OPERATION(operationAOTGetNamespaceOfModuleItself, JSCell*, (Instance* instance, uint32_t module))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetNamespaceOfModuleItself);
    OPERATION_RETURN(scope, modulesOf(*instance).namespaceOfModuleItself(ModuleID(module)));
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

// What a require() or an import without a type has made of the file answers before what an import under a type has made of it.
static bool isLoadedWithoutType(const Modules& modules, ModuleID form)
{
    String path = modules.table().pathText(form);
    size_t mark = path.reverseFind('?');
    if (modules.data(form).kind != ModuleKind::Data || mark == notFound)
        return false;
    std::optional<ModuleID> file = modules.table().findByPath(StringView(path).left(mark));
    return file && modules.data(*file).kind == ModuleKind::Data && modules.status(*file) == ModuleStatus::Evaluated;
}

// Undefined: no import has loaded the file, and require() reads it in its own way.
JSC_DEFINE_JIT_OPERATION(operationAOTRequireIfHeldForImport, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireIfHeldForImport);
    Modules& modules = modulesOf(*instance);
    if (!modules.isHeldForImport(ModuleID(module)) || isLoadedWithoutType(modules, ModuleID(module)))
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    OPERATION_RETURN(scope, JSValue::encode(modules.require(ModuleID(module), ModuleID(referrer))));
}

static NEVER_INLINE Vector<uint64_t>* findCountedFile(Instance& instance, uint32_t module, StringID description)
{
    auto& files = instance.vm->aotState().countedFiles;
    while (files.byModule.size() <= module)
        files.byModule.append(nullptr);
    files.byModule[module] = files.byDescription.ensure(ProgramStrings::atomOf(*instance.program, description).string(), [] {
        return makeUnique<Vector<uint64_t>>();
    }).iterator->value.get();
    return files.byModule[module];
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAreLoaderFunctionsStored, size_t, (Instance* instance))
{
    return modulesOf(*instance).areLoaderFunctionsStored();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSeesMockOf, size_t, (Instance* instance, uint32_t module, uint32_t referrer))
{
    return !!modulesOf(*instance).mockSeenBy(ModuleID(module), ModuleID(referrer));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMockSeenBy, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer))
{
    return JSValue::encode(modulesOf(*instance).mockSeenBy(ModuleID(module), ModuleID(referrer)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWasLoadedSynchronously, size_t, (Instance* instance, uint32_t module))
{
    return modulesOf(*instance).wasLoadedSynchronously(ModuleID(module));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCountExecution, void, (Instance* instance, uint32_t module, uint32_t description, uint32_t counter))
{
    const auto& byModule = instance->vm->aotState().countedFiles.byModule;
    Vector<uint64_t>* counts = module < byModule.size() ? byModule[module] : nullptr;
    if (!counts) [[unlikely]]
        counts = findCountedFile(*instance, module, StringID(description));
    while (counts->size() <= counter)
        counts->append(0);
    ++counts->at(counter);
}

void forEachCountedFile(VM& vm, const ScopedLambda<void(const String& description, std::span<const uint64_t> counts)>& each)
{
    for (auto& [description, counts] : vm.aotState().countedFiles.byDescription)
        each(description, counts->span());
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

static void withModuleOfJob(JSGlobalObject* realm, JSValue moduleForJob, const ScopedLambda<void(Modules&, ModuleID)>& functor)
{
    functor(*modulesOf(realm), ModuleID(static_cast<uint32_t>(moduleForJob.asInt32())));
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

static std::optional<ModuleID> findModuleInRunningImage(StringView publicPath, unsigned hashOfPath)
{
    const Image* image = Image::current();
    if (!image)
        return std::nullopt;
    ModuleTable table(image->programTables());
    if (publicPath.startsWith('#')) {
        std::optional<uint32_t> number = parseInteger<uint32_t>(publicPath.substring(1));
        if (number && *number < table.size() && table.pathText(ModuleID(*number)) == publicPath)
            return ModuleID(*number);
        return std::nullopt;
    }
    return table.findByPath(publicPath, hashOfPath);
}

std::optional<ModuleID> findModule(StringView publicPath, unsigned hashOfPath)
{
    return findModuleInRunningImage(publicPath, hashOfPath);
}

bool holdsFormThatAttributeAsksFor(StringView publicPath)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->holdsFormThatAttributeAsksFor(publicPath);
}

bool mayHoldPathStartingWith(StringView start)
{
    const Image* image = Image::current();
    return image && (start.isEmpty() || start.startsWith('#') || (image->mayHoldModulePathStartingWith(start) && ModuleTable(image->programTables()).holdsPathStartingWith(start)));
}

static StringView withoutSeparatorAtEnd(StringView directory)
{
    while (directory.length() > 1 && (directory[directory.length() - 1] == '/' || directory[directory.length() - 1] == '\\'))
        directory = directory.left(directory.length() - 1);
    return directory;
}

static StringView directoryOf(StringView path)
{
    size_t separator = path.reverseFind('/');
    if (size_t backslash = path.reverseFind('\\'); backslash != notFound && (separator == notFound || backslash > separator))
        separator = backslash;
    return separator == notFound ? StringView() : path.left(separator);
}

struct NamesLeftOutside {
    struct Key {
        StringView name;
        // Of the file that names it, for a name that starts with a dot.
        StringView directory;
    };
    static bool isLess(const Key& left, const Key& right)
    {
        std::strong_ordering order = codePointCompare(left.name, right.name);
        return order != 0 ? order < 0 : codePointCompareLessThan(left.directory, right.directory);
    }
    Key keyAt(uint32_t place) const { return { names[place], directories[place] }; }

    Vector<String> names;
    Vector<String> directories;
    Vector<uint32_t> placesInOrder;
};

// Made once for an image: a run of many test files leaves tens of thousands of names outside, and every resolve at run time asks about one.
static const NamesLeftOutside& namesLeftOutside(const Image& image, const ModuleTable& table)
{
    struct NamesOfImages {
        Lock lock;
        UncheckedKeyHashMap<unsigned, std::unique_ptr<NamesLeftOutside>> byImage WTF_GUARDED_BY_LOCK(lock);
    };
    static NeverDestroyed<NamesOfImages> namesOfImages;
    NamesOfImages& ofImages = namesOfImages.get();
    Locker locker { ofImages.lock };
    return *ofImages.byImage.ensure(image.number() + 1, [&] {
        auto made = makeUniqueWithoutFastMallocCheck<NamesLeftOutside>();
        for (const UncompiledModuleData& entry : table.modulesOfUncompiledCode()) {
            ModuleID module(entry.module);
            String name = table.pathText(module);
            String directory;
            if (name.startsWith('.')) {
                std::optional<ModuleID> referrer = table.data(module).referrerModule();
                if (!referrer)
                    continue;
                directory = withoutSeparatorAtEnd(directoryOf(table.pathText(*referrer))).toString();
            }
            made->placesInOrder.append(made->names.size());
            made->names.append(WTF::move(name));
            made->directories.append(WTF::move(directory));
        }
        AOT::sort(made->placesInOrder, [&](uint32_t left, uint32_t right) {
            return NamesLeftOutside::isLess(made->keyAt(left), made->keyAt(right));
        });
        return made;
    }).iterator->value;
}

bool isLeftToCodeOutsideImage(StringView specifier, StringView directoryOfImporter)
{
    const Image* image = Image::current();
    if (!image)
        return false;
    ModuleTable table(image->programTables());
    if (table.modulesOfUncompiledCode().empty())
        return false;
    const NamesLeftOutside& left = namesLeftOutside(*image, table);
    auto holds = [&](StringView name, StringView directory) {
        return std::ranges::binary_search(left.placesInOrder, NamesLeftOutside::Key { name, directory }, NamesLeftOutside::isLess, [&](uint32_t place) {
            return left.keyAt(place);
        });
    };
    if (specifier.startsWith('.'))
        return holds(specifier, withoutSeparatorAtEnd(directoryOfImporter));
    if (holds(specifier, { }))
        return true;
    // `package/file`, where the build left `package` outside.
    for (size_t slash = specifier.find('/'); slash != notFound; slash = specifier.find('/', slash + 1)) {
        StringView package = specifier.left(slash);
        if (package.startsWith('/'))
            return false;
        if (holds(package, { }))
            return true;
    }
    return false;
}

// Each frame of each stack trace asks.
struct PathOfModule {
    // Static strings: every thread may hold them.
    StringImpl* path;
    StringImpl* pathOfFile;
    bool isEvaluatedText;
};

// `import "./a.mjs?fresh=1"`: the loader knows the module by the path of the file with the query behind it. The display path ends where the query begins.
static String withoutQuery(const ModuleTable& table, ModuleID module, const String& path)
{
    size_t query = path.find('?');
    if (query == notFound)
        return path;
    String displayPath = table.displayPathText(module);
    if (displayPath.isEmpty() || displayPath.length() > query)
        return path;
    // A display path has forward slashes. In an executable for Windows the path has that system's separators.
    StringView end = StringView(path).substring(query - displayPath.length(), displayPath.length());
    for (unsigned index = 0; index < end.length(); ++index) {
        if (end[index] != displayPath[index] && (end[index] != '\\' || displayPath[index] != '/'))
            return path;
    }
    return path.left(query);
}

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
        // A text that the program evaluates is shown under the URL of the file that evaluates it, as the engine shows the text of `new Function(text)`,
        // unless it has a `//# sourceURL=`: the build hands that on as its display path, which is its path otherwise.
        std::optional<ModuleID> evaluator = table->data(module).evaluatorModule();
        while (std::optional<ModuleID> outer = evaluator ? table->data(*evaluator).evaluatorModule() : std::nullopt)
            evaluator = outer;
        String ownURL = evaluator ? table->displayPathText(module) : String();
        bool hasOwnURL = !ownURL.isEmpty() && ownURL != table->pathText(module);
        String path = hasOwnURL ? ownURL : evaluator ? URL::fileURLWithFileSystemPath(table->pathText(*evaluator)).string() : table->data(module).registryID() ? table->displayPathText(module) : table->pathText(module);
        auto keep = [](const String& text) {
            return &(text.is8Bit() ? StringImpl::createStaticStringImpl(text.span8()) : StringImpl::createStaticStringImpl(text.span16())).leakRef();
        };
        StringImpl* kept = keep(path);
        String pathOfFile = evaluator || table->data(module).registryID() ? path : withoutQuery(*table, module, path);
        return PathOfModule { kept, pathOfFile.length() == path.length() ? kept : keep(pathOfFile), !!evaluator };
    }).iterator->value;
}

String modulePath(ModuleID module)
{
    return pathOfModule(module).path;
}

String pathOfFileOfModule(ModuleID module)
{
    return pathOfModule(module).pathOfFile;
}

String sourceProviderURL(ModuleID module)
{
    PathOfModule found = pathOfModule(module);
    return found.isEvaluatedText ? emptyString() : String(found.pathOfFile);
}

bool entryPointIsCommonJS(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->firstModuleRequestedByHost() && modules->data(*modules->firstModuleRequestedByHost()).kind == ModuleKind::CommonJs;
}

bool isESModule(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::SourceText;
}

std::optional<bool> findsBuiltinsReadOnly(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    if (!table || table->data(module).isOfLibrary())
        return std::nullopt;
    return std::ranges::binary_search(Image::current()->programTables().modulesThatFindBuiltinsReadOnly(), module.index());
}

bool isReachedUnder(ModuleID module, OtherNameOfModule name)
{
    if (!currentModuleTable())
        return false;
    return std::ranges::binary_search(Image::current()->programTables().modulesReachedUnderOtherNames(), moduleReachedUnder(module.index(), name));
}

bool isDataFile(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::Data;
}

bool isHeldForImport(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isHeldForImport(module);
}

bool isNativeModule(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::Runtime;
}

bool isOfLibrary(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).isOfLibrary();
}

bool mayHoldEntryOfProgramInRequireCache(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).isOfLibrary();
}

static void throwLoaderChangeRefused(JSGlobalObject* realm, LoaderChange change, StringView changed)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwException(realm, scope, createLoaderChangeRefusedError(realm, change, changed));
}

// Reset: a deletion that bringBackKeptFiles() undoes.
enum class TakenOutBy : uint8_t { Deletion, Reset, FalsyStore, EntryOfProgram };

// A store leaves the loader as it is, and the host's require() looks there behind a falsy entry: it finds a module of the library, and a file that an import has loaded.
static bool isFoundBehindFalsyEntry(Modules& modules, ModuleID module)
{
    return modules.data(module).isOfLibrary() || modules.wasLoadedByImport(module);
}

static void evictModule(JSGlobalObject* realm, ModuleID module, TakenOutBy takenOutBy)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    if (takenOutBy == TakenOutBy::FalsyStore && isFoundBehindFalsyEntry(*modules, module))
        return;
    if (modules->data(module).isOfLibrary()) {
        String path = modules->table().pathText(module);
        if (takenOutBy == TakenOutBy::EntryOfProgram && modules->data(module).isHeldUnderNameWithoutPrefix() && !path.startsWith("node:"_s))
            return throwLoaderChangeRefused(realm, LoaderChange::StoreUnderNameOfModuleInRequireCache, path);
        return modules->takeOutOfRequireCache(module);
    }
    modules->forgetObjectMadeByRequire(module);
    bool isDeleted = takenOutBy == TakenOutBy::Deletion || takenOutBy == TakenOutBy::Reset;
    if (isDeleted) {
        modules->forgetFirstLoad(module);
        modules->dropErrorOfLoadEndedByMock(module);
        modules->forgetRequestByLoader(module);
    }
    // The host's require() answers with an entry that the program has made, whether the file has run or not.
    bool standsInPlaceOfFile = takenOutBy == TakenOutBy::EntryOfProgram;
    if (!modules->mayBeEvicted(module)) {
        if (standsInPlaceOfFile)
            throwLoaderChangeRefused(realm, LoaderChange::ReplaceInRequireCache, modules->table().pathText(module));
        else if (modules->wouldBeEvicted(module))
            throwLoaderChangeRefused(realm, LoaderChange::TakeOutOfRequireCache, modules->table().pathText(module));
        return;
    }
    modules->forgetFormAnsweredFirst(module);
    modules->evict(module, takenOutBy == TakenOutBy::Reset);
    if (isDeleted && modules->isEvicted(module))
        modules->takeOutOfLoader(module);
    if (standsInPlaceOfFile)
        modules->leaveToHostUnlessItHasRun(module);
}

void willRunEntryPointAgain(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->forgetEvaluation(module);
}

void didDeleteRequireCacheEntry(JSGlobalObject* realm, ModuleID module)
{
    evictModule(realm, module, TakenOutBy::Deletion);
}

void didResetModule(JSGlobalObject* realm, ModuleID module)
{
    evictModule(realm, module, TakenOutBy::Reset);
}

// A failure is kept in a record, and few modules have one.
void Modules::forEachFileThatFailed(const WTF::Function<void(ModuleID)>& functor) const
{
    for (auto& [module, record] : m_records) {
        const ModuleData& moduleData = data(module);
        if (statusOf(*record) != ModuleStatus::Failed || moduleData.isOfLibrary() || !m_table.hasFilePath(module))
            continue;
        ModuleID file = m_table.fileBehindFace(module).value_or(module);
        if (mayBeEvicted(file) && !isEvicted(file))
            functor(file);
    }
}

void takeOutFilesThatFailed(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    if (!modules || !modules->holdsCodeThatMayBeEvicted())
        return;
    Vector<ModuleID, 4> failed;
    modules->forEachFileThatFailed([&](ModuleID file) {
        if (!failed.contains(file))
            failed.append(file);
    });
    if (failed.isEmpty()) [[likely]]
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    for (ModuleID file : failed) {
        evictModule(realm, file, TakenOutBy::Reset);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool isEvictedFile(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isEvicted(module);
}

void willRunCodeThatKeepsFiles(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->willRunCodeThatKeepsFiles();
}

void keepFilesThatHaveRun(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->keepFilesThatHaveRun();
}

void bringBackKeptFiles(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    if (Modules* modules = modulesOf(realm))
        modules->bringBackKeptFiles(functor);
}

bool refusesToLoadAgain(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && !modules->data(module).isOfLibrary() && !modules->mayBeEvicted(module) && modules->wouldBeEvicted(module);
}

void willClearRequireCacheEntry(JSGlobalObject* realm, ModuleID module)
{
    evictModule(realm, module, TakenOutBy::FalsyStore);
}

void willReplaceRequireCacheEntry(JSGlobalObject* realm, ModuleID module)
{
    evictModule(realm, module, TakenOutBy::EntryOfProgram);
}

bool willLoadFileInAnotherWay(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules || modules->data(module).isOfLibrary())
        return true;
    if (!modules->mayBeEvicted(module))
        return false;
    modules->leaveToHostUnlessItHasRun(module);
    return true;
}

// A module with nothing to run keeps no state, and neither does one that compiled code runs as a part of the only module that imports it.
// What a module imports has run before the module has. A module of a cycle of imports has run when its cycle has.
bool Modules::hasRun(ModuleID module)
{
    bool importsAreFollowed = false;
    return hasRun(module, importsAreFollowed);
}

bool Modules::hasRun(ModuleID module, bool& importsAreFollowed)
{
    if (status(module) == ModuleStatus::Evaluated && !mayWaitForItsCycle(module))
        return true;
    if (!std::exchange(importsAreFollowed, true))
        followImportsOfModulesThatHaveRun();
    return m_modulesSeenToHaveRun.get(module.index()) || m_modulesThatRanWithImporters.get(module.index());
}

// Compiled code stores that a module has run and tells nobody.
void Modules::followImportsOfModulesThatHaveRun()
{
    size_t numberOfModules = m_table.size();
    if (m_statusesWhenImportsWereFollowed.isEmpty()) {
        m_statusesWhenImportsWereFollowed.fill(static_cast<uint8_t>(StoredModuleStatus::Unlinked), numberOfModules);
        m_modulesThatRanWithImporters.ensureSize(numberOfModules);
        m_modulesSeenToHaveRun.ensureSize(numberOfModules);
    }
    size_t numberLookedAt = 0;
    bool hasStatusTakenBack = false;
    Vector<ModuleID, 64> toFollow;
    auto lookAt = [&](ModuleID module) {
        bool hasRun = status(module) == ModuleStatus::Evaluated && !waitsForItsCycle(module, m_modulesOnStacksWhenImportsWereFollowed);
        if (hasRun == m_modulesSeenToHaveRun.quickGet(module.index()))
            return;
        m_modulesSeenToHaveRun.quickSet(module.index(), hasRun);
        if (hasRun)
            toFollow.append(module);
        else
            hasStatusTakenBack = true;
    };

    std::span<const uint8_t> now = m_storedStatuses;
    std::span<uint8_t> before = m_statusesWhenImportsWereFollowed.mutableSpan();
    // When a cycle ends, the status of the module that was entered first changes, and that of no module that waited for it.
    BitVector waited;
    bool hasFoundChange = false;
    constexpr size_t blockSize = 256;
    for (size_t start = 0; start < numberOfModules; start += blockSize) {
        size_t end = std::min(start + blockSize, numberOfModules);
        if (equalSpans(std::span<const uint8_t>(before.subspan(start, end - start)), now.subspan(start, end - start)))
            continue;
        if (!std::exchange(hasFoundChange, true) && m_modulesOnStacksWhenImportsWereFollowed)
            waited = *std::exchange(m_modulesOnStacksWhenImportsWereFollowed, std::nullopt);
        numberLookedAt += end - start;
        for (size_t index = start; index < end; ++index) {
            if (std::exchange(before[index], now[index]) != now[index])
                lookAt(ModuleID(index));
        }
    }
    for (size_t index : waited)
        lookAt(ModuleID(index));
    // A record holds the status of its module, and the table says no more than that: the module runs and no byte of the table changes.
    numberLookedAt += m_records.size();
    for (ModuleID module : m_records.keys())
        lookAt(module);
    if (!numberLookedAt)
        return;

    if (hasStatusTakenBack) {
        m_modulesThatRanWithImporters.clearAll();
        toFollow.shrink(0);
        for (size_t index : m_modulesSeenToHaveRun)
            toFollow.append(ModuleID(index));
    }
    while (!toFollow.isEmpty()) {
        ++numberLookedAt;
        ModuleID importer = toFollow.takeLast();
        // What only a mocked module imports is not loaded.
        if (standsForWholeModule(importer))
            continue;
        for (const ModuleRequestData& request : m_table.requests(importer)) {
            if (request.isDeferred || request.isRequire)
                continue;
            // A mock stood in its place while the importer ran, and has ended: its body has yet to run, and so has what it alone imports.
            if (m_modulesOwedToImportersThatRan.get(request.target()) || m_modulesWithMockToLetGo.get(request.target()))
                continue;
            // The evaluation of the importer passed over what another one had begun.
            if (status(request.targetModule()) == ModuleStatus::Evaluating || waitsForItsCycle(request.targetModule(), m_modulesOnStacksWhenImportsWereFollowed))
                continue;
            if (!m_modulesThatRanWithImporters.quickSet(request.target()))
                toFollow.append(request.targetModule());
        }
    }
    countEvent(*m_instance.vm, RuntimeCounterID::ModulesForWhatHasRun, numberLookedAt);
}

void Modules::followImportsAnewAtNextQuestion()
{
    m_statusesWhenImportsWereFollowed.clear();
    m_modulesOnStacksWhenImportsWereFollowed = std::nullopt;
    m_modulesThatRanWithImporters.clearAll();
    m_modulesSeenToHaveRun.clearAll();
}

// `bun run` puts a module object into require.cache before the file runs, and takes it out if the file throws: for a CommonJS file whoever asks for it first, which the loader does
// for every file of a level before it runs the first of them, for an ES module a require() alone.
bool Modules::isListedBeforeItHasRun(ModuleID module)
{
    if (data(module).kind == ModuleKind::CommonJs)
        return status(module) == ModuleStatus::Evaluating || (status(module) <= ModuleStatus::Linked && wasRequestedByLoader(module));
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->isKeptByRequire && (loaded->requiredValue || status(module) != ModuleStatus::Failed);
}

bool Modules::hasModuleObjectInRequireCache(ModuleID module) const
{
    if (data(module).kind == ModuleKind::CommonJs)
        return true;
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->isKeptByRequire;
}

bool isEvaluatedFile(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return false;
    const ModuleData& data = modules->data(module);
    if (data.isOfLibrary())
        return modules->isLibraryModuleInRequireCache(module);
    if (data.isFaceOfFile() || !modules->table().hasFilePath(module) || modules->isEvicted(module))
        return false;
    if (!modules->hasRun(module) && !modules->isListedBeforeItHasRun(module))
        return false;
    // A file may have a record beside its own, as the one that checks the names which a required graph imports from CommonJS files. It is no file.
    return !isNamedByNumber(modules->table().pathText(module), module);
}

JSObject* requireCacheEntry(JSGlobalObject* realm, ModuleID module)
{
    ASSERT(isEvaluatedFile(realm, module));
    return modulesOf(realm)->requireCacheEntry(module);
}

void Modules::forEachFileAwaitingAtTopLevel(const WTF::Function<void(int64_t, const String&)>& functor)
{
    for (ModuleRecord* record : m_records.values()) {
        if (record->status() == CyclicModuleRecord::Status::EvaluatingAsync && record->hasTLA() && !record->pendingAsyncDependencies().value_or(0) && record->asyncEvaluationOrder().hasOrder())
            functor(record->asyncEvaluationOrder().order(), record->moduleKey().string());
    }
}

void forEachFileAwaitingAtTopLevel(JSGlobalObject* realm, const WTF::Function<void(int64_t, const String&)>& functor)
{
    if (Modules* modules = modulesOf(realm))
        modules->forEachFileAwaitingAtTopLevel(functor);
}

void forEachEvaluatedFile(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    const ModuleTable& table = modules->table();
    // `bun run` lists a file from when somebody asks for it. A require() runs what it asks for at once, so a file stands in front of what it requires. The loader asks for everything that the
    // ES modules of one level import before any of it runs, so the files of a level stand in front of what the first of them requires. The table has a file behind what it asks for.
    // It lists what has a module object first: that is in the map of `require`, and what the loader of ES modules alone holds comes behind that.
    BitVector isSeen(table.size());
    Vector<ModuleID, 64> inOrder;
    auto askFor = [&](ModuleID module) {
        if (isSeen.quickSet(module.index()))
            return false;
        if (!table.data(module).isOfLibrary() && isEvaluatedFile(realm, module))
            inOrder.append(module);
        return true;
    };
    // No list of requests holds a require() that is left to the loader, and none says which of two calls came first.
    BitVector isRequested(table.size());
    UncheckedKeyHashMap<ModuleID, Vector<ModuleID, 2>> modulesFirstRequiredBy;
    for (auto [requirer, required] : modules->firstRequires()) {
        modulesFirstRequiredBy.add(requirer, Vector<ModuleID, 2>()).iterator->value.append(required);
        isRequested.quickSet(required.index());
    }
    auto firstRequiredBy = [&](ModuleID requirer) {
        auto found = modulesFirstRequiredBy.find(requirer);
        return found == modulesFirstRequiredBy.end() ? std::span<const ModuleID>() : found->value.span();
    };
    // One module, or the ES modules of one level.
    struct Step {
        Vector<ModuleID, 1> askers;
        bool areAskedFor;
    };
    Vector<Step, 64> steps;
    auto listFrom = [&](ModuleID root) {
        steps.append(Step { { root }, false });
        while (!steps.isEmpty()) {
            Step step = steps.takeLast();
            if (!step.areAskedFor && !askFor(step.askers[0]))
                continue;
            if (!modules->isLoadedWithRequests(step.askers[0]) || table.data(step.askers[0]).isFaceOfFile()) {
                auto requests = table.requests(step.askers[0]);
                for (size_t index = requests.size(); index--;)
                    steps.append(Step { { modules->moduleLoadedBy(requests[index]) }, false });
                for (ModuleID required : firstRequiredBy(step.askers[0]) | std::views::reverse)
                    steps.append(Step { { required }, false });
                continue;
            }
            Vector<ModuleID, 1> nextLevel;
            Vector<ModuleID, 8> filesOfLevel;
            // What a module requires when it runs, and whatever the levels do not come to.
            Vector<ModuleID, 8> askedForLater;
            for (ModuleID importer : step.askers) {
                askedForLater.append(firstRequiredBy(importer));
                for (const ModuleRequestData& request : table.requests(importer)) {
                    ModuleID loaded = modules->moduleLoadedBy(request);
                    ModuleID target = table.fileBehindFace(loaded).value_or(loaded);
                    if (request.isHoisted || request.isRequire)
                        askedForLater.append(target);
                    else if (!askFor(target))
                        continue;
                    else if (modules->isLoadedWithRequests(target))
                        nextLevel.append(target);
                    else
                        filesOfLevel.append(target);
                }
            }
            for (ModuleID module : askedForLater | std::views::reverse)
                steps.append(Step { { module }, false });
            if (!nextLevel.isEmpty())
                steps.append(Step { WTF::move(nextLevel), true });
            for (ModuleID file : filesOfLevel | std::views::reverse)
                steps.append(Step { { file }, true });
        }
    };
    // What nobody asks for begins a load: an entry point, of which a program that finds its files by name has many more than it starts from, and what an import() names.
    for (unsigned index = 0; index < table.size(); ++index) {
        for (const ModuleRequestData& request : table.requests(ModuleID(index)))
            isRequested.quickSet(modules->moduleLoadedBy(request).index());
    }
    for (bool startsAtEntryPoints : { true, false }) {
        for (unsigned index = 0; index < table.size(); ++index) {
            if (!isRequested.quickGet(index) && (!startsAtEntryPoints || table.data(ModuleID(index)).isEntryPoint()))
                listFrom(ModuleID(index));
        }
    }
    for (unsigned index = 0; index < table.size(); ++index)
        listFrom(ModuleID(index));
    for (bool hasModuleObject : { true, false }) {
        for (ModuleID module : inOrder) {
            if (modules->hasModuleObjectInRequireCache(module) == hasModuleObject)
                functor(table.pathText(module));
        }
    }
}

void forEachLibraryModuleInRequireCache(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    for (ModuleID module : modules->libraryModulesInRequireCache())
        functor(modules->table().pathText(module));
}

std::optional<ModuleID> findLibraryModuleInRequireCache(JSGlobalObject* realm, const String& name)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return std::nullopt;
    // Nearly every key is the path of a file.
    std::optional<ModuleID> module = findModuleInRunningImage(name, name.hash());
    if (module && modules->data(*module).isOfLibrary() && modules->isLibraryModuleInRequireCache(*module))
        return module;
    return std::nullopt;
}

bool hostAnswersInPlaceOf(JSGlobalObject* realm, ModuleID module, LoadByName way)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hostAnswersInPlaceOf(module, way);
}

void willLoadCopyFromDisk(JSGlobalObject* realm, ModuleID module, LoadByName way)
{
    if (Modules* modules = modulesOf(realm))
        modules->willLoadCopyFromDisk(module, way);
}

void forEachFirstLoad(VM& vm, const ScopedLambda<void(const String& path, bool wasSynchronous)>& functor)
{
    const VMState& state = vm.aotState();
    if (state.filesWithKnownFirstLoad.isEmpty())
        return;
    ModuleTable table(state.image->programTables());
    for (size_t file : state.filesWithKnownFirstLoad)
        functor(table.pathText(ModuleID(file)), state.filesFirstLoadedSynchronously.get(file));
}

void noteFirstLoadInEarlierProcess(VM& vm, const String& path, bool wasSynchronous)
{
    VMState& state = vm.aotState();
    if (!state.image)
        return;
    ModuleTable table(state.image->programTables());
    std::optional<ModuleID> found = table.findByPath(path);
    if (!found)
        return;
    ModuleID file = table.fileBehindFace(*found).value_or(*found);
    if (!state.filesWithKnownFirstLoad.set(file.index()))
        state.filesFirstLoadedSynchronously.set(file.index(), wasSynchronous);
}

bool leavesModuleToHost(JSGlobalObject* realm, ModuleID module, StartedBy startedBy)
{
    if (Modules* modules = modulesOf(realm))
        return startedBy == StartedBy::Host ? modules->isEvicted(module) : modules->isLeftToHost(module);
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).isOfLibrary();
}

void forEachModule(const ScopedLambda<IterationStatus(ModuleID)>& functor)
{
    std::optional<ModuleTable> table = currentModuleTable();
    if (!table)
        return;
    for (unsigned index = 0; index < table->size(); ++index) {
        if (table->hasFilePath(ModuleID(index)) && functor(ModuleID(index)) == IterationStatus::Done)
            return;
    }
}

void forEachFilePathStartingWith(StringView start, const ScopedLambda<IterationStatus(const String&)>& functor)
{
    Image::forEachPublishedImage([&](const Image& image) {
        if (!image.mayHoldModulePathStartingWith(start))
            return IterationStatus::Continue;
        return ModuleTable(image.programTables()).forEachFilePathStartingWith(start, functor);
    });
}

bool refusesCodeOutsideImage(JSGlobalObject* realm, StringView path)
{
    if (Modules* modules = modulesOf(realm))
        return modules->refusesCodeOutsideImage() && !modules->isTextNamedByProgram(path);
    const Image* image = Image::current();
    return image && !hasFlag(image->header().options.admitted(), AdmittedCode::Inspector) && !image->loadsCodeOutsideImage();
}

bool isChangedOutOfSightOfBuild(JSGlobalObject* realm, StringView path)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isChangedOutOfSightOfBuild(path);
}

bool willRunCopyFromDisk(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return true;
    if (modules->refusesOriginalsBesideMocks())
        return false;
    modules->willRunCopyFromDisk();
    return true;
}

bool refusesHostObjectsOutsideImage(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return refusesCodeOutsideImage(realm, { }) && !(modules && modules->isAskingHost());
}

JSObject* mainModuleObject(JSGlobalObject* realm, ModuleID module)
{
    if (!modulesThatShowWaysBetweenModuleObjects(realm))
        return nullptr;
    Modules* modules = modulesOf(realm);
    return modules ? modules->mainModuleObject(module) : nullptr;
}

void willStorePathOfMainModule(JSGlobalObject* realm)
{
    Instance* instance = realm->aotInstance();
    // In a test run the runner itself stores there, for each test file in its turn.
    if (!instance || hasFlag(instance->options().admitted(), AdmittedCode::ReplacedBuiltins) || instance->program->storesPathOfMainModule())
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwTypeError(realm, scope, assignmentThatBuildDidNotSeeMessage("Bun.main"_s));
}

JSObject* parentModuleObject(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules ? modules->parentModuleObject(module) : nullptr;
}

bool forEachNewChild(JSGlobalObject* realm, ModuleID module, const ScopedLambda<void(JSObject*)>& functor)
{
    if (Modules* modules = modulesOf(realm); modules && modules->data(module).isOfLibrary())
        return true;
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules && modules->forEachNewChild(module, functor);
}

void dropNewChildren(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm))
        modules->dropNewChildren(module);
}

JSValue moduleExports(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return jsUndefined();
    modules->noteLoadBehindMock(module, true);
    return modules->exportsShownByModuleObject(module);
}

void setModuleExports(JSGlobalObject* realm, ModuleID module, JSValue exports)
{
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

bool isAskedByCodeOutsideImage(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isAskedByCodeOutsideImage();
}

void throwFileOfImageRefused(JSGlobalObject* realm, ModuleID module, LoadByName form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    String path = modulePath(module);
    tellRefusedChangeListener(realm, makeString("Cannot reach '"_s, path, '\''));
    throwException(realm, scope, createFileOfImageRefusedError(realm, path, form == LoadByName::Import ? "ERR_MODULE_NOT_FOUND"_s : "MODULE_NOT_FOUND"_s));
}

static void throwLoadByNameRefused(JSGlobalObject* realm, StringView name, LoadByName form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    tellRefusedChangeListener(realm, makeString("Cannot reach '"_s, name, '\''));
    throwException(realm, scope, createLoadByNameRefusedError(realm, name, form == LoadByName::Import ? "ERR_MODULE_NOT_FOUND"_s : "MODULE_NOT_FOUND"_s));
}

bool isResolvingImportCall(VM& vm)
{
    return vm.aotState().isResolvingImportCall;
}

bool requireRunsMocks(VM& vm)
{
    return vm.aotState().requireRunsMocks;
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
        // Whoever wrote a file that is loaded from disk cannot pass another entry point to the build.
        if (modules->isAskedByCodeOutsideImage() && Image::current()->loadsCodeOutsideImage())
            throwFileOfImageRefused(realm, module, form);
        else
            throwLoadByNameRefused(realm, modulePath(module), form);
        return nullptr;
    }
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwException(realm, scope, createModuleInOtherRealmError(realm, modulePath(module)));
    return nullptr;
}

void willAnswerWithModule(JSGlobalObject* realm, ModuleID module, LoadByName form)
{
    // A realm that holds nothing compiled runs the library from its text: the host answers there.
    if (!modulesOf(realm) && isOfLibrary(module))
        return;
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

AbstractModuleRecord* moduleRecordOfNamespace(JSGlobalObject*, JSCell* cell)
{
    auto* moduleNamespace = dynamicDowncast<ModuleNamespace>(cell);
    if (!moduleNamespace)
        return nullptr;
    return moduleNamespace->recordForResolution();
}

AbstractModuleRecord* moduleRecord(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesForEvaluation(realm, module);
    if (!modules)
        return nullptr;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    modules->letGoOfMockThatEnded(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    // An import() has asked the loader, which asks for the module inside that call. What this runs of the library `bun run` runs from a job, as in askLoaderForImport().
    // A mark further in than the import() is the one that was set here for it.
    VMState& state = realm->vm().aotState();
    bool isFirstAskedForByImport = state.frameOfImportThatAsksLoader && (!state.stackPointerOfImport || state.stackPointerOfImport > static_cast<void*>(state.frameOfImportThatAsksLoader));
    SetForScope stackPointerOfImport(state.stackPointerOfImport, isFirstAskedForByImport ? currentStackPointer() : state.stackPointerOfImport);
    // The loader asks for a module when it loads it. Where it goes on to what the module imports, each file runs when the loader comes to it.
    if (!modules->isAskingHost()) {
        // A file that is no program fails where it is fetched. The loader comes to a statement that names nothing by itself: appendRequestsForLoader().
        if (std::optional<ModuleFailureID> failure = modules->loadFailure(module); failure && !modules->statementThatEndsLoad(module)) [[unlikely]] {
            scope.release();
            modules->throwFailure(*failure, false);
            return nullptr;
        }
    }
    // The loader asks for all that a module imports before it comes to the first of them. ModuleRecord::didLoad() hears where it comes to a file.
    if (modules->isCommonJSFileOrFace(module)) {
        if (!modules->isAskingHost())
            modules->didImportFileFromUncompiledCode(module);
        RELEASE_AND_RETURN(scope, modules->recordOfCommonJSFile(module, Modules::LoadOfFile::IsRequested));
    }
    // A module of the image asks through the loader. Code that is not compiled binds what it imports to the record, and so does a statement that the build has left to the loader.
    if (!modules->isAskingHost() || modules->isStatementAskingHost())
        modules->didImportFromUncompiledCode(module, isFirstAskedForByImport);
    RELEASE_AND_RETURN(scope, modules->exposeRecord(module, modules->isAskingHost() ? Modules::RecordHolder::Loader : Modules::RecordHolder::Anyone));
}

bool isCompiledModuleRecord(AbstractModuleRecord* record)
{
    return record->inherits<ModuleRecord>();
}

JSC_DEFINE_HOST_FUNCTION(importBehindFactories, (JSGlobalObject* realm, CallFrame* callFrame))
{
    JSArray* heldCall = asArray(callFrame->argument(1));
    ModuleID module(heldCall->getIndexQuickly(0).asUInt32());
    JSValue order = heldCall->getIndexQuickly(1);
    std::optional<int64_t> referrerAsyncOrder = order.isUndefined() ? std::nullopt : std::optional(static_cast<int64_t>(order.asNumber()));
    auto caller = heldCall->getIndexQuickly(2).asBoolean() ? Modules::ImportCaller::Host : Modules::ImportCaller::CompiledCode;
    return JSValue::encode(modulesOf(realm)->startImport(module, referrerAsyncOrder, caller, static_cast<Modules::TextOfImport>(heldCall->getIndexQuickly(3).asUInt32()), nullptr, heldCall->getIndexQuickly(4)));
}

JSC_DEFINE_HOST_FUNCTION(importByKeyBehindFactories, (JSGlobalObject* realm, CallFrame* callFrame))
{
    JSArray* heldCall = asArray(callFrame->argument(1));
    ModuleID referrer(heldCall->getIndexQuickly(1).asUInt32());
    Identifier key = Identifier::fromString(realm->vm(), asString(heldCall->getIndexQuickly(0))->tryGetValue());
    return JSValue::encode(modulesOf(realm)->importUncompiled(key, referrer, nullptr, heldCall->getIndexQuickly(2)));
}

JSC_DEFINE_HOST_FUNCTION(continueImportWithModuleItself, (JSGlobalObject* realm, CallFrame* callFrame))
{
    ModuleID module(callFrame->argument(1).asUInt32());
    return JSValue::encode(modulesOf(realm)->importModuleItselfBesideMock(module, callFrame->argument(0)));
}

JSPromise* importModule(JSGlobalObject* realm, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    Modules* modules = modulesForEvaluation(realm, module);
    if (JSValue error = modules ? modules->errorOfLoadEndedByMock(module) : JSValue()) [[unlikely]]
        return JSPromise::rejectedPromise(realm, error);
    if (modules)
        modules->dropErrorOfMockKeptByLoader(module);
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
    if (!modules)
        return nullptr;
    // A mock without a factory is made of what the module exports.
    if (modules->isLeftAloneByMock(module)) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(realm->vm());
        modules->evaluateModulesOwedBelowOriginal(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        RELEASE_AND_RETURN(scope, modules->moduleNamespace(module, Modules::ModulePhase::Evaluation));
    }
    return modules->moduleNamespace(module, Modules::ModulePhase::Evaluation);
}

bool mayExport(JSGlobalObject* realm, ModuleID module, UniquedStringImpl* name)
{
    Modules* modules = modulesOf(realm);
    return !modules || modules->refusesRequestFromHost(module) || modules->mayExport(module, name);
}

JSValue requireModule(JSGlobalObject* realm, ModuleID module, std::optional<ModuleID> referrer)
{
    Modules* modules = modulesForEvaluation(realm, module);
    if (!modules)
        return { };
    modules->noteLoadBehindMock(module, true);
    // The loader asks for code of the program, which may lie on disk. The library gets its own modules from the registry.
    return modules->require(module, referrer.value_or(ModuleID()), Modules::RequiredBy::Program);
}

bool isInRequireCache(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isInRequireCache(module);
}

void didMakeModuleObjectForRequire(JSGlobalObject* realm, ModuleID module, JSObject* moduleObject)
{
    if (Modules* modules = modulesOf(realm))
        modules->keepObjectMadeByRequire(module, moduleObject);
}

void willRequireModuleFromOutsideImage(JSGlobalObject* realm, ModuleID module, JSObject* moduleObjectOfRequirer)
{
    if (Modules* modules = modulesOf(realm))
        modules->recordRequireFromOutsideImage(module, moduleObjectOfRequirer);
}

AbstractModuleRecord* Modules::recordOfRequestedModule(ModuleID requested)
{
    SetForScope isAskingHost(m_isAskingHost, true);
    return moduleRecord(m_instance.globalObject, requested);
}

// The host's require() takes no entry that is falsy: willClearRequireCacheEntry().
void willSetRequireCacheEntry(JSGlobalObject* realm, ModuleID module, JSValue entry)
{
    Modules* modules = modulesOf(realm);
    if (!modules || !entry.toBoolean(realm))
        return;
    String path = modules->table().pathText(module);
    // require() takes a name that starts with `node:` to the registry and looks for no entry: the store changes no answer.
    if (modules->data(module).isOfLibrary()) {
        if (!path.startsWith("node:"_s) && modules->data(module).isHeldUnderNameWithoutPrefix())
            throwLoaderChangeRefused(realm, LoaderChange::StoreUnderNameOfModuleInRequireCache, path);
        return;
    }
    throwLoaderChangeRefused(realm, LoaderChange::ReplaceInRequireCache, path);
}

void willSetRequireCacheEntryUnderName(JSGlobalObject* realm, ModuleID module, const String& name)
{
    Modules* modules = modulesOf(realm);
    if (modules && modules->data(module).isHeldUnderNameWithoutPrefix())
        throwLoaderChangeRefused(realm, LoaderChange::StoreUnderNameOfModuleInRequireCache, name);
}

// A request that is resolved at run time asks the other two functions, as under plain Bun.
void willChangeRequireOverride(JSGlobalObject* realm, RequireOverride location, bool isOverridden)
{
    if (Modules* modules = modulesOf(realm))
        modules->noteLoaderFunction(location, isOverridden);
}

void willStoreLoaderOfExtension(JSGlobalObject* realm, StringView extension)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    const ModuleTable& table = modules->table();
    for (unsigned index = 0; index < table.size(); ++index) {
        ModuleID module(index);
        if (!table.hasFilePath(module) || modules->data(module).isOfLibrary())
            continue;
        // A form of a file stands under the path of the file, a question mark and the name of the form.
        String path = table.pathText(module);
        if (!StringView(path).left(path.reverseFind('?')).endsWith(extension))
            continue;
        if (!modules->mayBeEvicted(module)) {
            throwLoaderChangeRefused(realm, LoaderChange::ReplaceLoaderFunction, "a loader in Module._extensions"_s);
            return;
        }
        modules->leaveToHostUnlessItHasRun(module);
    }
}

bool isModuleNamespace(JSCell* cell)
{
    return cell->inherits<ModuleNamespace>();
}

bool tryReplaceExport(JSCell* moduleNamespace, UniquedStringImpl* name, JSValue value)
{
    auto* object = uncheckedDowncast<ModuleNamespace>(moduleNamespace);
    return modulesOf(object->realm())->tryReplaceExport(object->module(), name, value);
}

bool mayBeMocked(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->mayBeMocked(module);
}

bool setMock(JSGlobalObject* realm, ModuleID module, JSObject* mock, JSValue whatRequireAnswers, bool leavesEarlierMockToItsImporters)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, mock ? "setMock"_s : "setMock, none"_s);
    if (modules && mock && modules->isLeftAloneByMock(module))
        return modules->setMockOverModuleThatHasRun(module, mock, whatRequireAnswers);
    return modules && modules->setMockInPlace(module, mock, whatRequireAnswers, leavesEarlierMockToItsImporters);
}

bool willLeaveModuleAlone(JSGlobalObject* realm, ModuleID module, bool onlyIfLoadedInPreload, std::optional<ModuleID> asking)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->willLeaveModuleAlone(module, onlyIfLoadedInPreload, asking);
}

bool isLeftAloneByMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isLeftAloneByMock(module);
}

void didBeginTestFile(JSGlobalObject* realm)
{
    VMState& state = realm->vm().aotState();
    state.momentOfTestFile = ++state.momentOfLoads;
    if (Modules* modules = modulesOf(realm))
        modules->didBeginTestFile();
}

JSObject* mockInPlaceBehind(JSCell* moduleNamespace)
{
    auto* object = dynamicDowncast<ModuleNamespace>(moduleNamespace);
    return object ? object->mockInPlace() : nullptr;
}

bool hasMockInPlace(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->mockInPlace(module) && !modules->hasMockThatEnded(module);
}

bool hasWordOfMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->table().wordOfMock(module);
}

void mockExport(JSGlobalObject* realm, ModuleID module, UniquedStringImpl* name, JSValue value)
{
    if (Modules* modules = modulesOf(realm))
        modules->tryReplaceExport(modules->moduleWithExportsOf(module), name, value);
}

bool hasBegunToLoad(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return !modules || modules->hasBegunToLoad(module);
}

std::optional<ModuleID> formThatMockFinds(JSGlobalObject* realm, StringView path, std::optional<ModuleID> file)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return file;
    return modules->formThatMockFinds(path, file);
}

Vector<ModuleID, 2> formsThatAttributesAskFor(JSGlobalObject* realm, StringView path)
{
    Vector<ModuleID, 2> forms;
    if (Modules* modules = modulesOf(realm))
        forms = modules->table().formsThatAttributesAskFor(path);
    return forms;
}

bool hasYetToBegin(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->data(module).kind == ModuleKind::CommonJs && modules->hasYetToBegin(module);
}

bool isBeingEvaluated(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isBeingEvaluated(module);
}

void deferMock(JSGlobalObject* realm, ModuleID module, AfterFailure afterFailure)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, afterFailure == AfterFailure::AsksAgain ? "deferMock, asks again"_s : "deferMock"_s);
    if (modules)
        modules->deferMock(module, afterFailure);
}

JSValue exportsBeforeMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "exportsBeforeMock"_s);
    return modules ? modules->exportsBeforeMock(module) : JSValue();
}

bool runMocksBelow(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->runMocksBelowModuleThatLoaderFetches(module);
}

void didEndMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    MockDoor door(modules, module, "didEndMock"_s);
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    // The call of a mock that ends before it is laid counts for nothing that comes later.
    modules->forgetCallOfMock(module);
    bool wasLoadedAsMock = modules->mockInPlace(module) && !modules->hasMockThatEnded(module);
    // `bun test` takes whoever imports a built-in module out of its loader with the mock, whether the mock came ahead of the load or was laid over what an import had loaded.
    if (wasLoadedAsMock || modules->hasMockLaidOver(module) || modules->hasMockOfLibraryModule(module))
        modules->letGoOfImportersOf(module, Modules::ImportersLetGo::All);
    if (modules->isLeftAloneByMock(module)) {
        if (wasLoadedAsMock)
            modules->letGoOfModuleLoadedAsMock(module);
        modules->endMockOverModuleThatHasRun(module);
        RETURN_IF_EXCEPTION(scope, void());
        modules->dropErrorsOfLoadsEndedByMockOf(module);
        return;
    }
    // Whoever was linked against the mock of a module of the library reads it until a load comes to the module or the test file ends, as with a file.
    bool isLibraryModule = modules->data(module).isOfLibrary() || !modules->table().hasFilePath(module);
    if (isLibraryModule && modules->letGoOfMockOfLibraryModuleAtNextLoad(module)) {
        modules->forgetMockThatWaits(module);
        modules->dropErrorsOfLoadsEndedByMockOf(module);
        return;
    }
    // Nothing of a module that a mock stood in place of was filled or laid over.
    bool stoodInPlace = modules->mockInPlace(module);
    if (stoodInPlace) {
        modules->letGoOfMockAtNextLoad(module);
        RETURN_IF_EXCEPTION(scope, void());
    }
    modules->forgetMockThatWaits(module);
    // `bun test` takes the importers out of its loader with the mock, and with them the error of a factory that ended their load.
    modules->dropErrorsOfLoadsEndedByMockOf(module);
    if (isLibraryModule)
        RELEASE_AND_RETURN(scope, modules->endMockOfLibraryModule(module));
    if (!stoodInPlace)
        RELEASE_AND_RETURN(scope, modules->endMockLaidOverFile(module));
}

void didChangePlugins(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->forgetWhatNamesResolvedTo();
}

void willMockModule(JSGlobalObject* realm, ModuleID module, JSObject* exports, bool exportsAreOfCommonJSModule)
{
    // Bun finds the module in the image, whatever the realm.
    Modules* found = modulesOf(realm);
    if (!found)
        return;
    MockDoor door(found, module, "willMockModule"_s);
    Modules& modules = *found;
    modules.setExportsOfMockAreOfCommonJSModule(module, exportsAreOfCommonJSModule);
    if (modules.data(module).isOfLibrary() || !modules.table().hasFilePath(module)) {
        auto scope = DECLARE_THROW_SCOPE(realm->vm());
        modules.letGoOfMockThatEnded(module);
        RETURN_IF_EXCEPTION(scope, void());
        // An ES module of the library has a table of exports, as a file has: a mock that comes ahead of the load stands in its place, with its own names alone.
        ModuleStatus status = modules.status(module);
        bool comesAheadOfLoad = modules.data(module).kind == ModuleKind::SourceText && (status == ModuleStatus::Unlinked || status == ModuleStatus::Linked) && !modules.wasLoadedWhenMockWasMade(module);
        modules.willMockLibraryModule(module);
        bool standsInPlace = modules.letMockStandInPlaceOfLibraryModule(module);
        RETURN_IF_EXCEPTION(scope, void());
        if (comesAheadOfLoad) {
            modules.fillExportsOfBodyThatNeverRuns(module);
            modules.leaveOutWhatMockLacks(module, exports);
            RETURN_IF_EXCEPTION(scope, void());
            modules.takeWhatMockAdds(module, true);
        } else if (!standsInPlace) {
            // The mock is laid over the names of the module, which the loader takes from what the body made.
            modules.evaluateSynchronously(module, SynchronousEvaluationReason::Require);
            RETURN_IF_EXCEPTION(scope, void());
        }
        modules.mockWhatRequireCacheHolds(module, exports);
        return;
    }
    bool isFirstMock = !modules.isMocked(module);
    modules.willMock(module);
    if (modules.data(module).kind != ModuleKind::CommonJs) {
        ModuleStatus status = modules.status(module);
        bool comesAheadOfLoad = (status == ModuleStatus::Unlinked || status == ModuleStatus::Linked) && !modules.wasLoadedWhenMockWasMade(module);
        if (!comesAheadOfLoad)
            modules.keepWhatMockIsLaidOver(module);
        if (!comesAheadOfLoad && isFirstMock)
            modules.letGoOfImportersOf(module, Modules::ImportersLetGo::AllButThoseThatThisTestFileLinkedToModule);
        if (comesAheadOfLoad) {
            modules.fillExportsOfBodyThatNeverRuns(module);
            modules.leaveOutWhatMockLacks(module, exports);
        }
        modules.takeWhatMockAdds(module, comesAheadOfLoad);
        modules.mockWhatRequireAnswered(module, exports);
        return;
    }
    // Compiled code calls the body of a CommonJS file itself, and asks for the status first.
    ModuleStatus status = modules.status(module);
    bool hasRun = status != ModuleStatus::Unlinked && status != ModuleStatus::Linked;
    if (!hasRun)
        modules.markEvaluated(module);
    else
        modules.keepWhatMockIsLaidOver(module);
    if (hasRun && isFirstMock)
        modules.letGoOfImportersOf(module, Modules::ImportersLetGo::AllButThoseThatThisTestFileLinkedToModule);
    modules.setCommonJSExports(module, exports);
    modules.exportWhatMockNamesAlone(module, hasRun);
}

void didMockModule(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    MockDoor door(modules, module, "didMockModule"_s);
    modules->fillNamesThatMockLacks(module);
    RETURN_IF_EXCEPTION(scope, void());
    modules->answerFirstRequireBehindMock(module);
    RETURN_IF_EXCEPTION(scope, void());
    modules->takeWhatMockAdds(module, false);
    modules->didLayMock();
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

bool ModuleRecord::isMocked() const
{
    return modules().isMockedInItsImage(m_module);
}

unsigned ModuleRecord::innerModuleEvaluation(JSGlobalObject* globalObject, ModuleEvaluationStack& stack, unsigned index, int64_t referrerAsyncOrder, JSPromise* dynamicImportPromise)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    // letGoOfRecordThatAnswersByMock(): the record stands for the mock alone, for whoever holds it, and what the table does for the module does not reach it. A mock has nothing to run.
    if (hasLiveExports() && modules().recordIfExists(m_module) != this) [[unlikely]] {
        if (status() != Status::Evaluated) {
            setStatus(Status::Evaluated);
            setCycleRoot(globalObject->vm(), this);
        }
        return index;
    }
    if (takeRunsInItsTurn()) [[unlikely]] {
        modules().evaluateFilesOfLevel(m_module, true);
        RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, invalidIndex);
    }
    EvaluationContext context { stack, index, referrerAsyncOrder < 0 ? std::nullopt : std::optional(referrerAsyncOrder), dynamicImportPromise };
    modules().innerModuleEvaluation(m_module, context);
    // The module may be evaluated and off the stack by now, so a request to terminate waits for the next check.
    Exception* exception = scope.exception();
    if (!exception) [[likely]]
        return context.index;
    // evaluate() files what was thrown on the records of the stack. A request to terminate is raised behind the body too, where getters of the program run: the record has left the stack by then, or never stood on it, and takes the request here. Nothing else is filed: whoever reads an error takes the module for failed.
    if (globalObject->vm().isTerminationException(exception) && status() == Status::Evaluated && !evaluationError())
        setEvaluationError(globalObject->vm(), exception->value());
    return invalidIndex;
}

void ModuleRecord::willLoadRequests(ModuleGraphLoadingState& load)
{
    modules().appendRequestsForLoader(m_module, this, load);
}

ModuleGraphLoadingState* ModuleRecord::loadThatWaitsFor(const Identifier& key, ModuleGraphLoadingState* loadOfRequester, unsigned& jobsUntilLoadHearsOfFailure)
{
    if (m_hasRequestThatFindsNothing) [[unlikely]]
        modules().keepFailureOfRequest(m_module, this, key);
    return modules().loadThatWaitsFor(m_module, key, loadOfRequester, jobsUntilLoadHearsOfFailure);
}

void ModuleRecord::didLoad()
{
    m_runsInItsTurn &= !m_runsWhenLoaded;
    modules().evaluateFilesOfLevel(m_module, m_runsWhenLoaded);
}

void ModuleRecord::didFailToLoad(JSGlobalObject* globalObject, const ModuleGraphLoadingState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A request to terminate ends the load as an error does: the loader comes to nothing more of the graph. It is thrown again at the end.
    DeferTerminationForAWhile deferTermination(vm);
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
    if (!modules)
        return;
    modules->noteStepOfLoader();
    if (modules->hasDeferredLoads()) [[unlikely]]
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

void ModuleRecord::didLoseErrorOfModule(JSGlobalObject* globalObject, const Identifier& key)
{
    if (Modules* modules = modulesOf(globalObject))
        modules->didLoseErrorOfModule(key);
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

JSPromise* ModuleRecord::evaluateMainModule(JSPromise* evaluation)
{
    return modules().evaluateMainModule(m_module, evaluation);
}

void ModuleRecord::didEvaluateModulesInJob(JSGlobalObject* globalObject)
{
    Modules* modules = modulesOf(globalObject);
    if (modules && modules->mainModuleWaitsForItsImport()) [[unlikely]]
        modules->runMainModuleThatWaited();
}

void ModuleRecord::didFailModulesInJob(JSGlobalObject* globalObject)
{
    Modules* modules = modulesOf(globalObject);
    if (modules && modules->mainModuleWaitsForItsImport()) [[unlikely]]
        modules->failMainModuleThatWaited();
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

BitVector ModuleRecord::letAllImagesAskHost(JSGlobalObject* globalObject)
{
    BitVector wereAsking;
    if (Modules* modules = modulesOf(globalObject); modules && modules->setIsAskingHost(true))
        wereAsking.set(0);
    return wereAsking;
}

void ModuleRecord::restoreImagesThatAskHost(JSGlobalObject* globalObject, const BitVector& wereAsking)
{
    if (Modules* modules = modulesOf(globalObject))
        modules->setIsAskingHost(wereAsking.get(0));
}

uint8_t ModuleRecord::willAskHostForRequest()
{
    // A module that the loader walks asks it for compiled modules. One that compiled code loads asks only for what the loader links, and a compiled file among that
    // is the root of a graph of its own.
    bool wasWalkingRequests = modules().setIsWalkingRequests(!m_compiledRequests.isEmpty());
    bool wasAskingHost = modules().setIsAskingHost(true);
    bool wasStatementAskingHost = modules().setIsStatementAskingHost(true);
    return static_cast<uint8_t>(wasAskingHost) | static_cast<uint8_t>(wasWalkingRequests) << 1 | static_cast<uint8_t>(wasStatementAskingHost) << 2;
}

void ModuleRecord::didAskHostForRequest(uint8_t flagsBefore)
{
    modules().setIsAskingHost(flagsBefore & 1);
    modules().setIsWalkingRequests(flagsBefore & 2);
    modules().setIsStatementAskingHost(flagsBefore & 4);
}

uint8_t ModuleRecord::willAskHostForUncompiledModule(JSGlobalObject* globalObject)
{
    Modules* modules = modulesOf(globalObject);
    if (!modules)
        return 0;
    bool wasWalkingRequests = modules->setIsWalkingRequests(false);
    bool wasAskingHost = modules->setIsAskingHost(false);
    bool wasStatementAskingHost = modules->setIsStatementAskingHost(false);
    return static_cast<uint8_t>(wasAskingHost) | static_cast<uint8_t>(wasWalkingRequests) << 1 | static_cast<uint8_t>(wasStatementAskingHost) << 2;
}

void ModuleRecord::didAskHostForUncompiledModule(JSGlobalObject* globalObject, uint8_t flagsBefore)
{
    Modules* modules = modulesOf(globalObject);
    if (!modules)
        return;
    modules->setIsAskingHost(flagsBefore & 1);
    modules->setIsWalkingRequests(flagsBefore & 2);
    modules->setIsStatementAskingHost(flagsBefore & 4);
}

void ModuleRecord::willAnswerRequestOfUncompiledModule(JSGlobalObject* globalObject, const Identifier& key, const Identifier& keyOfAsker)
{
    Modules* modules = modulesOf(globalObject);
    if (!modules || key.isSymbol())
        return;
    std::optional<ModuleID> found = modules->table().findByPath(key.string());
    if (!found)
        return;
    AskerOfModule asker(globalObject, keyOfAsker.isSymbol() ? emptyString() : keyOfAsker.string());
    willAnswerWithModule(globalObject, *found, LoadByName::Import);
}

void ModuleRecord::willAnswerFromRegistry(JSGlobalObject* globalObject, const Identifier& key, const String& pathOfAsker)
{
    Modules* modules = modulesOf(globalObject);
    if (!modules || key.isSymbol())
        return;
    std::optional<ModuleID> found = modules->table().findByPath(key.string());
    if (!found || modules->isRootThatCompiledCodeLoads(*found))
        return;
    AskerOfModule asker(globalObject, pathOfAsker);
    willAnswerWithModule(globalObject, *found, LoadByName::Import);
}

AbstractModuleRecord* ModuleRecord::recordOfRequest(JSGlobalObject*, const Identifier& key)
{
    for (unsigned index = m_nextCompiledRequest; index < m_compiledRequests.size(); ++index) {
        if (m_compiledRequests[index].first != key.impl())
            continue;
        m_nextCompiledRequest = index + 1;
        return modules().recordOfRequestedModule(m_compiledRequests[index].second);
    }
    return nullptr;
}

bool ModuleRecord::holdTextOfRequest(JSPromise* fetched, AbstractModuleRecord* requested)
{
    auto* compiled = dynamicDowncast<ModuleRecord>(requested);
    return compiled && modules().holdTextOfRequest(fetched, compiled, m_module);
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

bool ModuleRecord::standsForMock() const
{
    // A mock that is laid over a module that has run replaces values: the names stay whose they were, as in `bun test`.
    return (modules().isMocked(m_module) || modules().mockInPlace(m_module)) && !modules().hasMockLaidOver(m_module);
}

bool ModuleRecord::someMockStandsInPlace() const
{
    return modules().someMockStandsInPlace();
}

void ModuleRecord::appendDynamicStarTargets(StarExportTargets& targets)
{
    auto scope = DECLARE_THROW_SCOPE(vm());
    Modules& modules = this->modules();
    if (!modules.data(m_module).hasDynamicStarExport() || modules.standsForWholeModule(m_module))
        return;
    for (const ModuleRequestData& request : modules.table().requests(m_module)) {
        // The tables have the names of the original, and what the mock adds stays behind.
        if (!request.isDynamicStarExport || modules.isMockedByFactoryThatTakesOriginal(request.targetModule()))
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
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Modules& modules = this->modules();
    const ModuleData& data = modules.data(m_module);
    bool isDefault = exportName == vm.propertyNames->defaultKeyword;
    Resolution absent = isDefault ? Resolution::error() : Resolution::notFound();

    retain();
    if (takesNamesFromValue(data)) {
        // A mock that stands for a CommonJS file lays its names where the ES modules of the image find them: on the face.
        ModuleID withExports = modules.moduleWithExportsOf(m_module);
        bool hasExport = modules.hasCapturedExport(modules.exportsWhatMockNamesAlone(withExports) ? withExports : m_module, exportName);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        return hasExport ? Resolution { Resolution::Type::Resolved, this, exportName } : absent;
    }

    // The search below for an indirect export has a resolveSet of its own, so a circular request is found here.
    if (m_resolveSet.contains(exportName.impl()))
        return Resolution::notFound();

    std::optional<unsigned> index = modules.table().findExport(m_module, exportName.impl());
    const ModuleExportData* entry = index ? &modules.table().exports(m_module)[*index] : nullptr;
    if (!entry && modules.isAddedByMock(m_module, exportName))
        return { Resolution::Type::Resolved, this, exportName };
    // A mock may have left out one of the two.
    if (!entry && modules.table().isAmbiguousExport(m_module, exportName.impl()) && !modules.namesBehindStarsAreInTables(m_module))
        return Resolution::ambiguous();
    if (!isDefault && (!entry || entry->isFromStarExport)) {
        appendDynamicStarTargets(starTargets);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
    }
    // The build leaves a name out of the table where a mock may answer ahead of the file that declares it. Behind a star such a mock passes on the names of the original, whatever it lacks or adds.
    if (!entry && !isDefault && modules.data(m_module).hasDynamicStarExport() && !modules.standsForWholeModule(m_module)) {
        for (const ModuleRequestData& request : modules.table().requests(m_module)) {
            if (!request.isDynamicStarExport || !modules.isMockedByFactoryThatTakesOriginal(request.targetModule()))
                continue;
            bool isNameOfOriginal = modules.table().findExport(request.targetModule(), exportName.impl()) || modules.hasNameBehindItsStars(request.targetModule(), exportName.impl());
            RETURN_IF_EXCEPTION(scope, Resolution::error());
            if (!isNameOfOriginal)
                continue;
            AbstractModuleRecord* record = modules.recordForResolution(request.targetModule());
            RETURN_IF_EXCEPTION(scope, Resolution::error());
            return { Resolution::Type::Resolved, record, exportName };
        }
    }
    if (!entry || modules.isLeftOut(m_module, *index, Modules::TablesBehindStars::MayLackName))
        return absent;
    // `export * from "./dep"`, `export { fn } from "./dep"`: while a mock stands in place of the module that declares the variable, the name is found where whoever asks that module finds it.
    // spyOn() so comes to the object that the factory returned.
    if (std::optional<Modules::AnswerOfMock> answer = modules.mockThatAnswersFor(m_module, *entry)) [[unlikely]] {
        AbstractModuleRecord* record = modules.recordForResolution(answer->mocked);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        if (record != this)
            RELEASE_AND_RETURN(scope, record->resolveExport(globalObject, answer->name));
    }
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

static constinit MissingExportListener missingExportListener = nullptr;

void setMissingExportListener(MissingExportListener listener)
{
    missingExportListener = listener;
}

bool ModuleRecord::throwIfExportWasLeftOut(JSGlobalObject* globalObject, AbstractModuleRecord* importer, AbstractModuleRecord* exporter, const Identifier& name)
{
    auto* compiled = dynamicDowncast<ModuleRecord>(exporter);
    if (!compiled || compiled->modules().data(compiled->m_module).isEntryPoint())
        return false;
    if (missingExportListener)
        missingExportListener(name.string(), exporter->moduleKey().string(), importer->moduleKey().string());
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    throwSyntaxError(globalObject, scope, makeString("Export named '"_s, name.string(), "' not found in module '"_s, exporter->moduleKey().string(), "'. The build keeps only the exports that something refers to: if the module exports it, this is a bug in Bun."_s));
    return true;
}

void ModuleRecord::collectExportedNames(JSGlobalObject* globalObject, WTF::Vector<Identifier>& names, StarExportTargets& starTargets)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Modules& modules = this->modules();

    if (takesNamesFromValue(modules.data(m_module))) {
        RELEASE_AND_RETURN(scope, modules.forEachCapturedExport(m_module, [&](const Identifier& name, JSValue) {
            names.append(name);
        }));
    }
    std::span<const ModuleExportData> entries = modules.table().exports(m_module);
    for (unsigned index = 0; index < entries.size(); ++index) {
        if (!hasLiveExports() || !modules.isLeftOut(m_module, index, Modules::TablesBehindStars::HaveName))
            names.append(modules.identifier(entries[index].name));
    }
    modules.appendNamesAddedByMock(m_module, names);
    RETURN_IF_EXCEPTION(scope, void());
    // Whoever exports all of this module has the name from nobody else either: `export * from "./stars"; export * from "./c"`, and c has it too.
    for (const AmbiguousExportData& entry : modules.table().ambiguousExports(m_module))
        names.append(Identifier::fromString(globalObject->vm(), ProgramStrings::text(modules.table().programTables(), entry.nameString())));
    RELEASE_AND_RETURN(scope, appendDynamicStarTargets(starTargets));
}

JSValue ModuleRecord::readExport(JSGlobalObject* globalObject, const Identifier& exportName, std::optional<unsigned> exportIndex)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (JSObject* mock = modules().mockInPlace(m_module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, mock->get(globalObject, exportName));
    if (exportIndex)
        RELEASE_AND_RETURN(scope, modules().readExport(m_module, *exportIndex));
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
    return modulesOf(*realm()->aotInstance());
}

std::span<const ModuleExportData> ModuleNamespace::exports() const
{
    return modules().table().exports(m_module);
}

bool ModuleNamespace::isHiddenExport(unsigned index) const
{
    if (keepsToModule()) [[unlikely]]
        return modules().isHiddenInModuleItself(m_module, index);
    return (exports()[index].isFromStarExport && modules().isExportAmbiguous(m_module, index)) || modules().isLeftOut(m_module, index, Modules::TablesBehindStars::MayLackName);
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
    visitor.append(thisObject->m_mockOfView);
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
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Modules& modules = this->modules();
    if (modules.mockInPlace(m_module) || m_mockOfView) [[unlikely]]
        return false;
    std::span<const ModuleExportData> entries = exports();
    for (unsigned index = 0; index < entries.size(); ++index) {
        if (isHiddenExport(index))
            continue;
        JSValue value = modules.readExport(m_module, index);
        RETURN_IF_EXCEPTION(scope, true);
        names.append(modules.identifier(entries[index].name).impl());
        values.appendWithCrashOnOverflow(value);
    }
    return true;
}

AbstractModuleRecord* ModuleNamespace::recordForResolution()
{
    // The record that the module has now answers by the mock.
    if (keepsToModule()) [[unlikely]]
        return nullptr;
    return modules().recordForResolution(m_module);
}

JSObject* ModuleNamespace::mockInPlace() const
{
    if (JSObject* ofView = m_mockOfView.get()) [[unlikely]]
        return ofView;
    return keepsToModule() ? nullptr : modules().mockInPlace(m_module);
}

bool ModuleNamespace::isAnsweredByMock() const
{
    // The caches go by the state of the module, which has a mock while this object keeps to the module.
    return keepsToModule() || !!mockInPlace();
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
    leaveStructureThatCachesKnow(globalObject->vm());
}

void ModuleNamespace::replaceDynamicExports(VM& vm, const DynamicExports& dynamicExports)
{
    m_dynamicExportNames.setMayBeNull(vm, this, dynamicExports.names);
    m_capturedValues.setMayBeNull(vm, this, dynamicExports.capturedValues);
    m_liveSources.setMayBeNull(vm, this, dynamicExports.liveSources);
    leaveStructureThatCachesKnow(vm);
}

void ModuleNamespace::leaveStructureThatCachesKnow(VM& vm)
{
    if (!m_hasOwnStructure)
        return;
    // The caches file what they know of this object under its Structure: classifyNamespaceRead(). It never has that one again.
    // A property that is deleted may bring an earlier Structure back. A dictionary has no earlier one, and nothing of the kind is filed under it.
    if (isValidOffset(structure()->maxOffset()))
        convertToDictionary(vm);
    else
        setStructure(vm, createStructure(vm, realm(), structure()->storedPrototype(), !structure()->isStructureExtensible()));
}

JSValue ModuleNamespace::capturedValueWithoutAllocating(UniquedStringImpl* name) const
{
    JSObject* table = m_capturedValues.get();
    if (!table || m_mayNeedEvaluation || parseIndex(*name) || !isDynamicExport(name) || modules().mockInPlace(m_module))
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

// What the factory gave may inherit from this object, or pass the question on to it, and that walk never pushes a frame of the program. As canForwardToLiveExportsSource() of JSModuleNamespaceObject.cpp.
static bool canAskMock(JSGlobalObject* globalObject, ThrowScope& scope)
{
    if (globalObject->vm().isSafeToRecurseSoft()) [[likely]]
        return true;
    throwStackOverflowError(globalObject, scope);
    return false;
}

// The steps of JSModuleNamespaceObject::getOwnPropertySlotForLiveExports().
bool ModuleNamespace::getOwnPropertySlotOfMock(JSGlobalObject* globalObject, JSObject* mock, PropertyName name, PropertySlot& slot)
{
    using InternalMethodType = PropertySlot::InternalMethodType;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (slot.internalMethodType() == InternalMethodType::VMInquiry) {
        slot.setValue(this, static_cast<unsigned>(PropertyAttribute::None), jsUndefined());
        return false;
    }
    if (!canAskMock(globalObject, scope)) [[unlikely]]
        return false;
    if (slot.internalMethodType() != InternalMethodType::Get || name == vm.propertyNames->__esModule) {
        bool hasExport = mock->hasProperty(globalObject, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (!hasExport) {
            if (name == vm.propertyNames->__esModule)
                RELEASE_AND_RETURN(scope, Base::getOwnPropertySlot(this, globalObject, name, slot));
            return false;
        }
        if (slot.internalMethodType() == InternalMethodType::HasProperty) {
            slot.setValue(this, static_cast<unsigned>(PropertyAttribute::None), jsUndefined());
            return true;
        }
    }
    JSValue value = mock->get(globalObject, name);
    RETURN_IF_EXCEPTION(scope, false);
    slot.setValue(this, static_cast<unsigned>(PropertyAttribute::None), value);
    return true;
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
    if (JSObject* mock = thisObject->isSymbolLikeKey(name.uid()) ? nullptr : thisObject->mockInPlace()) [[unlikely]]
        RELEASE_AND_RETURN(scope, thisObject->getOwnPropertySlotOfMock(globalObject, mock, name, slot));
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
        if (index && thisObject->keepsToModule()) [[unlikely]]
            value = thisObject->modules().readExportOfModuleItself(thisObject->m_module, *index);
        else
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
    // The names of the mock, in its order.
    if (JSObject* mock = thisObject->mockInPlace()) [[unlikely]] {
        if (!canAskMock(globalObject, scope)) [[unlikely]]
            return;
        PropertyNameArrayBuilder namesOfMock(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        mock->methodTable()->getOwnPropertyNames(mock, globalObject, namesOfMock, DontEnumPropertiesMode::Exclude);
        RETURN_IF_EXCEPTION(scope, void());
        for (const Identifier& name : namesOfMock)
            names.add(name);
        entries = { };
        dynamicNameCount = 0;
    }
    for (unsigned ownIndex = 0, dynamicIndex = 0; ownIndex < entries.size() || dynamicIndex < dynamicNameCount;) {
        if (ownIndex < entries.size() && thisObject->isHiddenExport(ownIndex)) {
            ++ownIndex;
            continue;
        }
        Identifier name = ownIndex < entries.size() ? modules.identifier(entries[ownIndex].name) : Identifier();
        bool isAnsweredByMock = false;
        if (dynamicIndex == dynamicNameCount || (!name.isNull() && codePointCompare(StringView(name.string()), StringView(*asString(dynamicNames->get(dynamicIndex))->tryGetValueImpl())) < 0))
            isAnsweredByMock = !!modules.mockThatAnswersFor(thisObject->m_module, entries[ownIndex++]);
        else {
            name = asString(dynamicNames->get(dynamicIndex++))->toIdentifier(globalObject);
            RETURN_IF_EXCEPTION(scope, void());
        }
        // What a mock answers is never uninitialized, and to read it may run an accessor of the mock.
        if (mode == DontEnumPropertiesMode::Exclude && !isAnsweredByMock) {
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
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    if (name == vm.propertyNames->__esModule && !thisObject->hasExport(name.uid()))
        RELEASE_AND_RETURN(scope, Base::put(cell, globalObject, name, value, slot));
    // A write goes through to what the factory gave, which importers read.
    if (JSObject* mock = thisObject->mockInPlace()) [[unlikely]] {
        if (!canAskMock(globalObject, scope)) [[unlikely]]
            return false;
        if (isThisValueAltered(slot, thisObject)) [[unlikely]]
            RELEASE_AND_RETURN(scope, ordinarySetSlow(globalObject, thisObject, name, value, slot.thisValue(), slot.isStrictMode()));
        PutPropertySlot slotOfMock(mock, slot.isStrictMode());
        RELEASE_AND_RETURN(scope, mock->methodTable()->put(mock, globalObject, name, value, slotOfMock));
    }
    return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
}

bool ModuleNamespace::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    if (JSObject* mock = thisObject->mockInPlace()) [[unlikely]] {
        if (!canAskMock(globalObject, scope)) [[unlikely]]
            return false;
        RELEASE_AND_RETURN(scope, mock->methodTable()->putByIndex(mock, globalObject, index, value, shouldThrow));
    }
    return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
}

bool ModuleNamespace::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    if (thisObject->isSymbolLikeKey(name.uid()) && name != globalObject->vm().propertyNames->toStringTagSymbol)
        RELEASE_AND_RETURN(scope, Base::deleteProperty(thisObject, globalObject, name, slot));
    if (!thisObject->isSymbolLikeKey(name.uid())) {
        bool isEvaluated = thisObject->evaluateIfNeeded(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (!isEvaluated)
            return false;
    }
    if (JSObject* mock = thisObject->isSymbolLikeKey(name.uid()) ? nullptr : thisObject->mockInPlace()) [[unlikely]] {
        if (!canAskMock(globalObject, scope)) [[unlikely]]
            return false;
        // Not the caller's slot: the mock would mark it cacheable for the Structure of this object, which never changes.
        DeletePropertySlot slotOfMock;
        RELEASE_AND_RETURN(scope, mock->methodTable()->deleteProperty(mock, globalObject, name, slotOfMock));
    }
    RELEASE_AND_RETURN(scope, !thisObject->hasExport(name.uid()) && name != globalObject->vm().propertyNames->toStringTagSymbol);
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
    if (thisObject->isOrdinaryKey(name))
        RELEASE_AND_RETURN(scope, Base::defineOwnProperty(thisObject, globalObject, name, descriptor, shouldThrow));
    if (JSObject* mock = thisObject->isSymbolLikeKey(name.uid()) ? nullptr : thisObject->mockInPlace()) [[unlikely]] {
        if (!canAskMock(globalObject, scope)) [[unlikely]]
            return false;
        RELEASE_AND_RETURN(scope, mock->methodTable()->defineOwnProperty(mock, globalObject, name, descriptor, shouldThrow));
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
