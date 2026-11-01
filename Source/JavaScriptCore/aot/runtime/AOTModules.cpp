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
#include "AOTModuleEnvironment.h"
#include "AOTModuleMicrotasks.h"
#include "AOTModuleNamespace.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeCounters.h"
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
#include "HeapAnalyzer.h"
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
        visitor.appendUnbarriered(m_environmentStructure);
        for (ModuleEnvironment* environment : m_environments)
            visitor.appendUnbarriered(environment);
    });
}

template void Modules::visitRoots(SlotVisitor&);
template void Modules::visitRoots(AbstractSlotVisitor&);

const ClassInfo ModuleEnvironment::s_info = { "JSModuleEnvironment"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleEnvironment) };

ModuleEnvironment::ModuleEnvironment(VM& vm, Structure* structure, const Modules& modules, uint32_t index)
    : Base(vm, structure)
    , m_modules(modules)
    , m_index(index)
{
}

Structure* ModuleEnvironment::createStructure(VM& vm, JSGlobalObject* globalObject)
{
    return Structure::create(vm, globalObject, jsNull(), TypeInfo(CellType, StructureFlags), info());
}

ModuleEnvironment* ModuleEnvironment::create(VM& vm, Structure* structure, const Modules& modules, uint32_t index)
{
    auto* environment = new (NotNull, allocateCell<ModuleEnvironment>(vm)) ModuleEnvironment(vm, structure, modules, index);
    environment->finishCreation(vm);
    return environment;
}

void ModuleEnvironment::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Base::analyzeHeap(cell, analyzer);
    auto* environment = uncheckedDowncast<ModuleEnvironment>(cell);
    environment->m_modules.analyzeVariables(environment, environment->m_index, analyzer);
}

std::span<const NamedVariableData> Modules::namedVariablesOf(uint32_t indexAmongModulesWithNamedVariables) const
{
    uint32_t first = m_firstNamedVariables[indexAmongModulesWithNamedVariables];
    return m_namedVariables.subspan(first, m_firstNamedVariables[indexAmongModulesWithNamedVariables + 1] - first);
}

void Modules::willAnalyzeHeap(VM& vm)
{
    const ProgramTables& tables = m_table.programTables();
    if (tables.namedVariables().empty())
        return;
    if (m_namedVariables.isEmpty()) {
        bool endsWhereAModuleEnds = forEachNamedVariable(tables.namedVariables(), [&](size_t) {
            m_firstNamedVariables.append(m_namedVariables.size());
        }, [&](const NamedVariableData& variable) {
            m_namedVariables.append(variable);
        });
        RELEASE_ASSERT(endsWhereAModuleEnds);
        m_firstNamedVariables.append(m_namedVariables.size());
        m_namesOfVariables.grow(m_namedVariables.size());
    }
    uint32_t numberOfModules = m_firstNamedVariables.size() - 1;

    // No collection may start between here and the one that takes the snapshot.
    DeferGCForAWhile deferGC(vm);
    Structure* structure = m_environmentStructure ? m_environmentStructure : ModuleEnvironment::createStructure(vm, m_instance.globalObject);
    Vector<std::pair<uint32_t, ModuleEnvironment*>> created;
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (!m_environments.isEmpty() && m_environments[index])
            continue;
        std::span<const NamedVariableData> variables = namedVariablesOf(index);
        bool holdsCell = std::ranges::any_of(variables, [&](const NamedVariableData& variable) {
            return m_state.statics->cellIn(variable);
        });
        if (!holdsCell)
            continue;
        size_t first = m_firstNamedVariables[index];
        for (size_t offset = 0; offset < variables.size(); ++offset)
            m_namesOfVariables[first + offset] = AtomString(ProgramStrings::text(tables, variables[offset].nameString()));
        created.append({ index, ModuleEnvironment::create(vm, structure, *this, index) });
    }

    Locker locker { m_state.caches->lock() };
    m_environmentStructure = structure;
    if (m_environments.isEmpty())
        m_environments.fill(nullptr, numberOfModules);
    for (auto [index, environment] : created)
        m_environments[index] = environment;
    m_storedReferences.recordAll();
}

void Modules::analyzeVariables(ModuleEnvironment* environment, uint32_t indexAmongModulesWithNamedVariables, HeapAnalyzer& analyzer) const
{
    std::span<const NamedVariableData> variables = namedVariablesOf(indexAmongModulesWithNamedVariables);
    size_t first = m_firstNamedVariables[indexAmongModulesWithNamedVariables];
    for (size_t offset = 0; offset < variables.size(); ++offset) {
        if (JSCell* cell = m_state.statics->cellIn(variables[offset]))
            analyzer.analyzeVariableNameEdge(environment, cell, m_namesOfVariables[first + offset].impl());
    }
}

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
    , m_buildHasSeenWhoNamesModules(!m_admitsInspector)
    , m_mockOperations(host().mockOperations ? host().mockOperations() : nullptr)
{
    m_instance.modules.status = state.tableArena.allocate<uint8_t>(m_table.size());
    m_storedStatuses = unsafeMakeSpan(m_instance.modules.status, m_table.size());
    m_instance.modules.evaluatingMark = static_cast<uint32_t>(StoredModuleStatus::Evaluating);
    m_modulesSettledForLinks.fill(0, m_table.size() / 64 + 1);
    m_loadedGroups.fill(0, m_table.size() / 64 + 1);
    state.caches->add(CacheKind::StrongRoots, "modules"_s, *this, RootVolatility::GreyedByExecution);
}

Modules::~Modules()
{
    forgetUnfinishedLoads();
}

void Modules::forgetUnfinishedLoads()
{
    VMState& state = m_instance.vm->aotState();
    for (const TextThatWaits& text : textsThatWait()) {
        if (text.waitsApart)
            --state.numberOfCallsThatWaitApart;
        else
            --state.numberOfTextsThatWait;
    }
    m_textsThatWait.clear();
    m_numberOfTextsThatHaveCome = 0;
    m_synchronousLoadsHandedToLoader.clear();
    m_evaluationOfImportOfMainModule.clear();
    m_capabilityOfMainModule.clear();
    m_loadsInFlight.clear();
    m_countedImports.clear();
    m_levelsOfLoads.clear();
    m_filesAskedForOnBehalfOfRoot.clear();
    m_deferredRequests.clear();
    m_deferredImports.clear();
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
                if (linked && linked->uncompiledRecord && linked->resolvedKey.string() == m_table.pathText(file))
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
    // The loader keeps the error of a CommonJS file that threw while an import loaded it, until the file is taken out of require.cache: didThrowInBody().
    bool hasThrown = data(file).kind == ModuleKind::CommonJs && (status(file) == ModuleStatus::Failed || status(moduleWithExportsOf(file)) == ModuleStatus::Failed);
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
    if (LoadedModule* loaded = loadedIfExists(module))
        loaded->importMeta.set(*this, nullptr);
    // See isLeftToHost().
    if (m_evictedFiles.isEmpty() && !m_otherImageHasEvictedFiles)
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

bool Modules::isChangedOnDisk(ModuleID file)
{
    std::optional<uint64_t> stampOfBuild = m_table.sourceStamp(file);
    if (!stampOfBuild || !host().stampOfFile)
        return false;
    count(m_instance, RuntimeCounterID::FilesComparedWithDisk);
    return host().stampOfFile(m_pathsForSystem.pathOf(m_table, file)) != *stampOfBuild;
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
    if (m_otherImageHasEvictedFiles && data(file).isUncompiledCode()) {
        // To resolve a name may run code of the program.
        walk.importsAreFollowed = false;
        if (namesFileEvictedFromOtherImage(file))
            return true;
    }
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
        leaveToHostUnlessItHasRun(file);
        for (Instance* instance = m_instance.globalObject->aotInstance(); instance; instance = instance->above) {
            if (Modules* modules = instance->state->modules.get(); modules && modules != this)
                modules->didEvictFromOtherImage();
        }
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
    if (m_evictedFiles.isEmpty() && !m_otherImageHasEvictedFiles && !m_table.hasSourceStamps())
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
    // The face of a file is `#` and its number in its image. A realm has one registry for all its images, so there the number says the image too.
    if (m_instance.program->sharesRealms() && isNamedByNumber(path, module)) [[unlikely]]
        path = makeString('#', moduleIDForHost(*m_instance.program, module).index());
    return Identifier::fromString(*m_instance.vm, path);
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
        if (JSObject* linkedWith = namespaceThatCompiledCodeHolds(moduleExport.targetModule(), targetPhaseOf(moduleExport)))
            return linkedWith;
        if (isLeftAloneByMock(moduleExport.targetModule())) [[unlikely]]
            return { };
        JSObject* namespaceObject = namespaceIfCreated(moduleExport.targetModule(), targetPhaseOf(moduleExport));
        m_namespacesThatOnlyImagesAboveHold.clear(moduleExport.targetModule().index());
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
    if (JSObject* mock = m_moduleReadPastItsMock == module ? nullptr : mockInPlace(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, mock->get(realm, identifier(moduleExport.name)));
    // The caches answer for the namespace object of an evaluated module only.
    if (status(module) == ModuleStatus::Linked) [[unlikely]]
        recordEvaluationIfNothingIsLeftToRun(module);
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable: {
        if (m_numberOfMocksInPlace) [[unlikely]] {
            if (std::optional<AnswerOfMock> answer = mockThatAnswersFor(module, moduleExport)) {
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
        if (JSObject* linkedWith = namespaceThatCompiledCodeHolds(moduleExport.targetModule(), targetPhaseOf(moduleExport)))
            return linkedWith;
        if (targetPhaseOf(moduleExport) == ModulePhase::Evaluation && !seesMockOf(moduleExport.targetModule(), module)) [[unlikely]]
            RELEASE_AND_RETURN(scope, namespaceOfModuleItself(moduleExport.targetModule()));
        RELEASE_AND_RETURN(scope, moduleNamespace(moduleExport.targetModule(), targetPhaseOf(moduleExport)));
    case ModuleExportKind::Indirect: {
        const IndirectExportData& indirectExport = m_table.indirectExport(moduleExport);
        // The namespace object of the module between has the names that it was made with. A later mock may give another one, and the search for the name ends at the mock.
        // A mock of a module of another image is known there alone.
        if (m_mocks || (vm.aotState().hasSeveralImages && data(indirectExport.targetModule()).isUncompiledCode())) [[unlikely]] {
            AbstractModuleRecord* record = recordForResolution(indirectExport.targetModule());
            RETURN_IF_EXCEPTION(scope, { });
            StarExportResolution resolution = record->resolveExport(realm, identifier(indirectExport.importName));
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

Vector<ModuleID, 8> Modules::filesImportedByImagesAbove(ImportersAbove which) const
{
    Vector<ModuleID, 8> files;
    if (!m_instance.vm->aotState().hasSeveralImages) [[likely]]
        return files;
    Vector<const Modules*, 4> imagesAbove;
    for (Instance* above = m_instance.above; above; above = above->above) {
        if (const Modules* modules = above->state ? above->state->modules.get() : nullptr)
            imagesAbove.append(modules);
    }
    // From the topmost image down: a file of an image between, which an image above it imports, goes on importing there.
    Vector<Vector<ModuleID, 8>, 4> importedFromAbove(imagesAbove.size());
    for (size_t place = imagesAbove.size(); place--;) {
        const Modules* modules = imagesAbove[place];
        const ModuleTable& table = modules->m_table;
        BitVector seen(table.size());
        Vector<ModuleID, 32> work;
        for (ModuleID file : importedFromAbove[place]) {
            if (!seen.quickSet(file.index()))
                work.append(file);
        }
        for (uint32_t index = 0; index < table.size(); ++index) {
            ModuleStatus current = modules->status(ModuleID(index));
            bool isImporter = which == ImportersAbove::HaveBegun ? current > ModuleStatus::Linked : current == ModuleStatus::Evaluating || current == ModuleStatus::EvaluatingAsync;
            if (isImporter && !seen.quickSet(index))
                work.append(ModuleID(index));
        }
        while (!work.isEmpty()) {
            ModuleID importer = work.takeLast();
            // What only a mocked module imports is not loaded.
            if (modules->isMocked(importer))
                continue;
            if (modules->data(importer).isUncompiledCode()) {
                // The loader has resolved what it has fetched.
                const Identifier* key = modules->resolvedKeyIfKnown(importer);
                if (!key)
                    continue;
                auto fileOf = [&](const Modules& image) -> std::optional<ModuleID> {
                    std::optional<ModuleID> file = image.findByKey(*key);
                    if (!file || image.data(*file).isUncompiledCode())
                        return std::nullopt;
                    return image.m_table.faceOfFile(*file).value_or(*file);
                };
                std::optional<ModuleID> file;
                for (size_t between = place; between-- && !file;) {
                    if ((file = fileOf(*imagesAbove[between])))
                        importedFromAbove[between].append(*file);
                }
                if (!file && (file = fileOf(*this)))
                    files.append(*file);
                continue;
            }
            for (const ModuleRequestData& request : table.requests(importer)) {
                if (request.isHoisted || request.isRequire || request.isDeferred)
                    continue;
                if (!seen.quickSet(request.target()))
                    work.append(request.targetModule());
            }
        }
    }
    return files;
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
    for (ModuleID file : filesImportedByImagesAbove(ImportersAbove::HaveBegun)) {
        if (file == module)
            return true;
        if (!seen.quickSet(file.index()))
            work.append(file);
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

bool Modules::wasLoadedWhenMockWasMade(ModuleID module)
{
    if (!m_modulesWithMockThatWaited.get(module.index()))
        return wasLoadedWithImporter(module);
    m_modulesWithMockThatWaited.clear(module.index());
    return m_modulesLoadedBeforeMockThatWaited.get(module.index());
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
        if (m_instance.state->sharesRealm && data(module).isUncompiledCode()) [[unlikely]] {
            auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
            keepExportOfOtherImageBeforeMock(module, name);
            RETURN_IF_EXCEPTION(scope, false);
            std::optional<bool> isReplaced = tryReplaceExportInImageBelow(module, name, value);
            RETURN_IF_EXCEPTION(scope, false);
            if (isReplaced)
                return *isReplaced;
            RELEASE_AND_RETURN(scope, tryReplaceExportOfUncompiledModule(module, name, value));
        }
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

// The variable lies in another image, whose modules know nothing of the mock that is being laid in this one. The module that stands for the file here keeps the value, and the file
// under the mock gives it back by the same way: endMockLaidOverFile().
void Modules::keepExportOfOtherImageBeforeMock(ModuleID external, UniquedStringImpl* name)
{
    if (!m_moduleWithMockBeingLaid)
        return;
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* namespaceObject = uncompiledNamespace(external, ModulePhase::Evaluation, NamespaceUse::CarriesBindings);
    RETURN_IF_EXCEPTION(scope, void());
    if (!dynamicDowncast<ModuleNamespace>(namespaceObject))
        return;
    Vector<ModuleID>& behindStars = ensureLoaded(*m_moduleWithMockBeingLaid).modulesBehindStarsUnderMock;
    if (!behindStars.contains(external))
        behindStars.append(external);
    Identifier key = Identifier::fromUid(vm, name);
    LoadedModule& held = ensureLoaded(external);
    if (held.exportsBeforeMock && storedValue(realm, held.exportsBeforeMock, key))
        return;
    JSValue before = namespaceObject->get(realm, key);
    // The body of the file has yet to fill the name, on a ring through the file under the mock: there is no value to come back to.
    if (scope.exception()) [[unlikely]] {
        scope.tryClearException();
        return;
    }
    if (!held.exportsBeforeMock)
        held.exportsBeforeMock.set(*this, createTableOfExports());
    held.exportsBeforeMock.get()->putDirectMayBeIndex(realm, key, before);
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
    JSObject* namespaceObject = uncompiledNamespace(external, ModulePhase::Evaluation, NamespaceUse::CarriesBindings);
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

// A graph is loaded before a module of it begins, so a helper that mocks finds what its importer imports beside it linked. The body does not run ahead of an import whose own body is up the stack.
// True: the module is linked and no load has come to it: whatever imports it met a mock in its own place.
bool Modules::runBodyOfLinkedModuleAheadOfMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto hasYetToRun = [&](ModuleID each) {
        ModuleStatus current = status(each);
        return current == ModuleStatus::Unlinked || current == ModuleStatus::Linked;
    };
    if (!data(module).isESModuleOfProgram() || data(module).isFaceOfFile() || !hasYetToRun(module) || isMocked(module) || m_modulesWithMockToRun.get(module.index()))
        return false;
    // Only a graph whose evaluation is under way, or was ended by an error, has linked what it did not come to. A load that met a mock in place of the module never fetched it.
    auto isImportedByGraphUnderEvaluation = [&] {
        BitVector isReached(m_table.size());
        Vector<ModuleID, 16> importers;
        for (unsigned index = 0; index < m_table.size(); ++index) {
            ModuleStatus current = status(ModuleID(index));
            if (current == ModuleStatus::Evaluating || current == ModuleStatus::EvaluatingAsync || current == ModuleStatus::Failed)
                importers.append(ModuleID(index));
        }
        for (ModuleID file : filesImportedByImagesAbove(ImportersAbove::AreUnderEvaluation)) {
            if (file == module)
                return true;
            if (hasYetToRun(file) && !isMocked(file) && !m_modulesWithMockToRun.get(file.index()))
                importers.append(file);
        }
        while (!importers.isEmpty()) {
            ModuleID importer = importers.takeLast();
            if (isReached.quickSet(importer.index()))
                continue;
            for (const ModuleRequestData& request : m_table.imports(importer)) {
                if (request.isHoisted || request.isDeferred)
                    continue;
                ModuleID imported = request.targetModule();
                if (imported == module)
                    return true;
                if (hasYetToRun(imported) && !isMocked(imported) && !m_modulesWithMockToRun.get(imported.index()))
                    importers.append(imported);
            }
        }
        return false;
    };
    if (!isImportedByGraphUnderEvaluation())
        return !hasRun(module) && !didCaptureExports(module);
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 16> toVisit { module };
    while (!toVisit.isEmpty()) {
        ModuleID importer = toVisit.takeLast();
        if (isSeen.quickSet(importer.index()))
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (request.isDeferred)
                continue;
            if (status(request.targetModule()) == ModuleStatus::Evaluating)
                return false;
            if (hasYetToRun(request.targetModule()))
                toVisit.append(request.targetModule());
        }
    }
    JSPromise* evaluation = evaluate(module, std::nullopt, nullptr);
    RETURN_IF_EXCEPTION(scope, false);
    if (evaluation->status() == JSPromise::Status::Rejected) {
        evaluation->markAsHandled();
        throwException(m_instance.globalObject, scope, evaluation->result());
    }
    return false;
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
        loaded->firstRequirerOfOtherImage.set(*this, nullptr);
        m_libraryModulesInRequireCache.removeFirst(module);
        return;
    }
    if (JSObject* entry = loaded->objectMadeByRequire)
        entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, before);
}

// `export { sides } from "./shapes"`, `export * from "./shapes"`, `import { sides } from "./shapes"; export { sides as again }`: the entry of the module that passes an export on names the variable
// of the module that declares it. While a mock stands in place of that one nothing fills the variable, and the mock answers under the name that the declaring module exports it by.
auto Modules::mockThatAnswersForSlow(ModuleID passingOn, const ModuleExportData& passedOn) const -> std::optional<AnswerOfMock>
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
                if (request.isRequire || seen.quickSet(request.target()))
                    continue;
                work.append(request.targetModule());
                for (const ModuleExportData& entry : m_table.exports(request.targetModule())) {
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
                ModuleID named = request.targetModule();
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
                if (request.targetModule() == to)
                    return true;
                if (!request.isRequire && !seen.quickSet(request.target()))
                    work.append(request.targetModule());
            }
        }
        return false;
    };
    for (size_t index : m_modulesWithMockInPlace) {
        JSObject* mock = mockInPlace(ModuleID(index));
        if (!mock || !seesMockOf(ModuleID(index), passingOn))
            continue;
        ModuleID declaring = moduleWithExportsOf(ModuleID(index));
        for (const ModuleExportData& entry : m_table.exports(declaring)) {
            if (entry.kind() != ModuleExportKind::Variable || entry.variable != passedOn.variable)
                continue;
            // The mocked module has the name by a star from the module that asks, which declares it or has it by name.
            if (entry.isFromStarExport && !passedOn.isFromStarExport && reaches(declaring, passingOn))
                continue;
            if (takesFromAnotherModule(declaring) && !comesThroughMock(passingOn, ModuleID(index), declaring))
                break;
            return AnswerOfMock { mock, identifier(entry.name), ModuleID(index) };
        }
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

bool Modules::setMockInPlace(ModuleID fileOrModule, JSObject* mock, JSValue whatRequireAnswers)
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
    // Compiled code that imports a name of the module looks here first.
    if (std::optional<SlotData> word = m_table.wordOfMock(fileOrModule))
        ProgramStatics::writeVariable(m_instance, *word, mock ? JSValue(mock) : JSValue());

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
    // An evaluation that came by has marked the module as evaluated. Its body has yet to run, behind a link of its own.
    for (ModuleID each : { fileOrModule, module }) {
        if (ranBesideMock) [[unlikely]]
            continue;
        if (status(each) == ModuleStatus::Evaluated && (!original || each != fileOrModule))
            setStatus(each, ModuleStatus::Unlinked);
        m_modulesOwedToImportersThatRan.set(each.index());
        m_modulesWithNothingOwedBelow.clearAll();
        m_mockedModulesPassedOverByWalks.clear();
        // A walk made before passes over what has work again.
        m_modulesSettledForLinks[each.index() / 64] &= ~(1ull << (each.index() % 64));
    }
    ++m_linkEpoch;
    // The namespace object was made with the names of the mock. The module that loads next has one of its own, as in `bun test`. So has a module whose `export *` took its names from a run.
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID each(index);
        if (each == fileOrModule || each == module || data(each).hasDynamicStarExport())
            letGoOfNamespaceMadeUnderMock(each);
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

// The loader has asked for a module and has yet to run it.
bool Modules::isLoaderInMiddleOfGraph() const
{
    for (auto& entry : loader()->moduleMap().values()) {
        switch (entry->status()) {
        case ModuleRegistryEntry::Status::New:
        case ModuleRegistryEntry::Status::Fetching:
            return true;
        case ModuleRegistryEntry::Status::Fetched:
            if (auto* record = dynamicDowncast<CyclicModuleRecord>(entry->record()); record && record->status() != CyclicModuleRecord::Status::Evaluated)
                return true;
            break;
        case ModuleRegistryEntry::Status::FetchFailed:
        case ModuleRegistryEntry::Status::InstantiationFailed:
        case ModuleRegistryEntry::Status::EvaluationFailed:
            break;
        }
    }
    return false;
}

// Compiled code that requires a module again reads the status of that module and asks nobody where it has run: no load would come by.
bool Modules::isImportedByModuleThatRequireAnswered(ModuleID imported) const
{
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        const LoadedModule* loaded = loadedIfExists(ModuleID(index));
        if (!loaded || !loaded->requiredValue || index == imported.index())
            continue;
        BitVector seen(m_table.size());
        Vector<ModuleID, 16> work { ModuleID(index) };
        while (!work.isEmpty()) {
            for (const ModuleRequestData& request : m_table.imports(work.takeLast())) {
                if (request.targetModule() == imported)
                    return true;
                if (!seen.quickSet(request.target()))
                    work.append(request.targetModule());
            }
        }
    }
    return false;
}

// The code of a CommonJS file whose mock was let go asks the run time for its status again.
bool Modules::loadComesToModuleWhoseMockEnded(ModuleID module) const
{
    return hasMockThatEnded(module) || (data(module).kind == ModuleKind::CommonJs && !isImportedByModuleThatRequireAnswered(module));
}

// Compiled code calls the body of a CommonJS file by itself once the status says so, and asks nobody.
void Modules::letGoOfMockAtNextLoad(ModuleID module)
{
    // A graph that is being loaded holds the record of the module, and goes on by what the table says of the module.
    if (!data(module).isESModuleOfProgram() || data(module).isFaceOfFile() || isAnythingBeingLoaded() || isLoaderInMiddleOfGraph() || isImportedByModuleThatRequireAnswered(module)) {
        setMockInPlace(module, nullptr, { });
        return;
    }
    m_modulesWithMockToLetGo.set(module.index());
    // The loader asks for the module anew, and so comes by.
    if (ModuleRecord* record = recordIfExists(module); record && record->hasLiveExports())
        loader()->removeEntryOfRecord(record);
}

bool Modules::letGoOfMockOfLibraryModuleAtNextLoad(ModuleID module)
{
    // A mock that was laid over the names of a loaded module goes at once, as it came. A graph that is being loaded goes on by what the table says of the module.
    if (!m_libraryModulesWithMock.get(module.index()) || !exportsWhatMockNamesAlone(module) || isAnythingBeingLoaded() || isLoaderInMiddleOfGraph())
        return false;
    m_modulesWithMockToLetGo.set(module.index());
    return true;
}

void Modules::letGoOfMockThatEndedSlow(ModuleID module)
{
    if (data(module).isOfLibrary() || !m_table.hasFilePath(module)) {
        auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
        m_modulesWithMockToLetGo.clear(module.index());
        if (mockInPlace(module)) {
            setMockInPlace(module, nullptr, { });
            RETURN_IF_EXCEPTION(scope, void());
        }
        RELEASE_AND_RETURN(scope, endMockOfLibraryModule(module));
    }
    // Nothing is asked of a mock that is taken away.
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    setMockInPlace(module, nullptr, { });
    scope.assertNoExceptionExceptTermination();
}

void Modules::letGoOfMocksThatEnded()
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    while (!m_modulesWithMockToLetGo.isEmpty()) {
        letGoOfMockThatEnded(ModuleID(*m_modulesWithMockToLetGo.begin()));
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (m_libraryModulesOwedToImagesAbove.isEmpty()) [[likely]]
        return;
    for (size_t module : std::exchange(m_libraryModulesOwedToImagesAbove, { })) {
        handLibraryModuleToImagesAbove(ModuleID(module));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

uint32_t Modules::placeOfMomentOfLoad(ModuleID module) const
{
    // A CommonJS file and its face are in no load group.
    if (!data(module).isESModuleOfProgram())
        return module.index();
    uint32_t head = m_table.headOfLoadGroup(module).index();
    return m_momentsOfLoads[head] ? head : module.index();
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

void Modules::letGoOfImportersOf(ModuleID mocked, ImportersLetGo which)
{
    bool keepsImportersOfTestFile = which == ImportersLetGo::AllButThoseThatThisTestFileLinkedToModule;
    if (keepsImportersOfTestFile && !m_momentsOfLoads.isEmpty() && m_momentsOfLoads[placeOfMomentOfLoad(mocked)] > m_instance.vm->aotState().momentOfTestFile)
        return;
    BitVector asImported(m_table.size());
    asImported.quickSet(mocked.index());
    asImported.quickSet(moduleWithExportsOf(mocked).index());
    letGoOfImportersOf(asImported, BitVector(m_table.size()), keepsImportersOfTestFile);
}

// `gone`: what has left the loader with the mocked module in the images below, as this image names it.
void Modules::letGoOfImportersOf(const BitVector& mocked, BitVector&& gone, bool keepsImportersOfTestFile)
{
    bool hasSeveralImages = m_instance.vm->aotState().hasSeveralImages;
    if (!m_momentsOfLoads.isEmpty()) {
        uint32_t momentOfTestFile = m_instance.vm->aotState().momentOfTestFile;
        if (m_importersByStatement.isEmpty()) {
            m_importersByStatement.grow(m_table.size());
            for (uint32_t importer = 0; importer < m_table.size(); ++importer) {
                for (const ModuleRequestData& request : m_table.imports(ModuleID(importer))) {
                    if (!request.isHoisted && !request.isDeferred)
                        m_importersByStatement[request.target()].append(importer);
                }
            }
        }
        // An image above finds a file of this one with the loader, which the host searches for importers by what the loader has resolved: nothing, where this image names a module of an image below.
        bool leavesRegistry = hasSeveralImages && !isAnythingBeingLoaded() && !isLoaderInMiddleOfGraph();
        BitVector seen = mocked;
        seen.merge(gone);
        Vector<uint32_t, 32> work;
        for (size_t imported : seen)
            work.append(imported);
        while (!work.isEmpty()) {
            uint32_t imported = work.takeLast();
            for (uint32_t index : m_importersByStatement[imported]) {
                ModuleID importer(index);
                uint32_t place = placeOfMomentOfLoad(importer);
                if (mocked.quickGet(imported) && keepsImportersOfTestFile && m_momentsOfLoads[place] > momentOfTestFile)
                    continue;
                if (seen.quickSet(index))
                    continue;
                // A module with nothing to run stays as the loader left it.
                bool isLoaded = status(importer) > ModuleStatus::Linked || (data(importer).isESModuleOfProgram() && wasLoadedWithGraph(importer));
                // Nothing would load a module of a preload again. A mock that stands in place of a body that never ran imports nothing.
                if (!isLoaded || m_loadedInPreload.get(place) || (mockInPlace(importer) && !isLeftAloneByMock(importer)))
                    continue;
                m_modulesThatLoaderLetGo.set(index);
                // `bun test` lets go of what was linked to the mock. Whoever kept the module under a mock that left it alone stays as it is.
                if (mocked.quickGet(imported) ? !isLeftAloneByMock(ModuleID(imported)) || seesMockOf(ModuleID(imported), importer) : m_importersOfMockThatEnded.get(imported))
                    m_importersOfMockThatEnded.set(index);
                gone.quickSet(index);
                work.append(index);
                if (ModuleRecord* record = leavesRegistry ? recordIfExists(importer) : nullptr)
                    loader()->removeEntryOfRecord(record);
            }
        }
    }
    if (!hasSeveralImages) [[likely]]
        return;
    for (Instance* above = m_instance.above; above; above = above->above) {
        Modules* modules = above->state ? above->state->modules.get() : nullptr;
        if (!modules)
            continue;
        const ModuleTable& table = modules->m_table;
        BitVector mockedAbove(table.size());
        BitVector goneAbove(table.size());
        for (const UncompiledModuleData& entry : table.modulesOfUncompiledCode()) {
            std::optional<ModuleID> file = m_table.findByPath(table.pathText(ModuleID(entry.module)));
            if (!file)
                continue;
            uint32_t withExports = moduleWithExportsOf(*file).index();
            if (mocked.quickGet(file->index()) || mocked.quickGet(withExports))
                mockedAbove.quickSet(entry.module);
            else if (gone.quickGet(file->index()) || gone.quickGet(withExports))
                goneAbove.quickSet(entry.module);
        }
        if (!mockedAbove.isEmpty() || !goneAbove.isEmpty())
            modules->letGoOfImportersOf(mockedAbove, WTF::move(goneAbove), keepsImportersOfTestFile);
    }
}

bool Modules::hasLinkedImporterYetToRun(ModuleID module, const String& keyOfFileInImageBelow, std::optional<ModuleID> askingAsHostNamesIt) const
{
    auto isModule = [&](ModuleID target) {
        if (keyOfFileInImageBelow.isNull())
            return target == module || target == moduleWithExportsOf(module);
        const Identifier* key = data(target).isUncompiledCode() ? resolvedKeyIfKnown(target) : nullptr;
        return key && key->string() == keyOfFileInImageBelow;
    };
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID importer(index);
        if (!data(importer).isESModuleOfProgram() || data(importer).isFaceOfFile() || status(importer) > ModuleStatus::Evaluating || !wasLoadedWithGraph(importer))
            continue;
        if (askingAsHostNamesIt && moduleIDForHost(*m_instance.program, importer) == *askingAsHostNamesIt)
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (isModule(request.targetModule()))
                return true;
        }
    }
    return false;
}

bool Modules::willLeaveModuleAlone(ModuleID module, bool onlyIfLoadedInPreload, std::optional<ModuleID> askingAsHostNamesIt)
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
    if (onlyIfLoadedInPreload && !m_loadedInPreload.get(placeOfMomentOfLoad(module)))
        return false;
    if (LoadedModule* loaded = loadedIfExists(module); loaded && loaded->objectMadeByRequire && !isFile)
        return false;
    // `bun test` lays the mock over a module that a linked module has yet to read: only so does the mock reach it. A module is linked and yet to run only while its graph runs:
    // what the evaluation has not come to, and what lies between its root and the body that runs. The module that asks has read what it has read.
    // So too an importer in an image above, which knows the module by the key that the loader has it under. The graph that runs may be that of any image: it goes down through the others.
    Vector<const Modules*, 4> imagesAbove;
    for (Instance* above = m_instance.above; above; above = above->above) {
        if (const Modules* modules = above->state ? above->state->modules.get() : nullptr)
            imagesAbove.append(modules);
    }
    if (isGraphRunning() || std::ranges::any_of(imagesAbove, &Modules::isGraphRunning)) {
        if (hasLinkedImporterYetToRun(module, nullString(), askingAsHostNamesIt))
            return false;
        for (const Modules* modules : imagesAbove) {
            if (modules->hasLinkedImporterYetToRun(module, m_table.pathText(module), askingAsHostNamesIt))
                return false;
        }
    }
    VMState& state = m_instance.vm->aotState();
    auto laid = m_mocksOverModulesThatHaveRun.set(module, MockOverModuleThatHasRun { momentOfFirstMockInTestFile(module), state.momentOfTestFile, !!namespaceIfCreated(withExports, ModulePhase::Evaluation) });
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

// The host's own loader gives a package the module itself where a test mocks it as Vitest does.
bool Modules::importsOriginalBesideMock(ModuleID mocked, ModuleID importer) const
{
    if (!isMocked(mocked) || !host().givesOriginalBesideMock || !importer || !m_table.hasFilePath(mocked) || !m_table.hasFilePath(importer))
        return false;
    // TODO(bun): a CommonJS file has one variable for its exports, and what an import reads of it is taken by the mock.
    if (!data(mocked).isESModuleOfProgram() || data(mocked).isFaceOfFile())
        return false;
    auto asked = m_importersAskedAboutOriginal.add(static_cast<uint64_t>(mocked.index()) << 32 | importer.index(), false);
    if (asked.isNewEntry)
        asked.iterator->value = host().givesOriginalBesideMock(m_instance.globalObject, m_table.pathText(mocked), m_table.pathText(importer), true);
    return asked.iterator->value;
}

bool Modules::seesMockOf(ModuleID mocked, ModuleID importer) const
{
    if (importsOriginalBesideMock(mocked, importer)) [[unlikely]]
        return false;
    if (m_mocksOverModulesThatHaveRun.isEmpty()) [[likely]]
        return true;
    auto mock = m_mocksOverModulesThatHaveRun.find(m_table.fileBehindFace(mocked).value_or(mocked));
    if (mock == m_mocksOverModulesThatHaveRun.end())
        return true;
    // Whoever comes to the face of the file has been asked.
    if (m_table.fileBehindFace(importer) == std::optional(mock->key))
        return true;
    if (auto anew = m_momentsLoadedAnew.find(importer); anew != m_momentsLoadedAnew.end() && anew->value.momentOfTestFile == mock->value.momentOfTestFile && anew->value.moment > mock->value.momentThisWasLaid) [[unlikely]]
        return true;
    uint32_t place = placeOfMomentOfLoad(importer);
    uint32_t momentLoaded = m_momentsOfLoads[place];
    if (!momentLoaded || momentLoaded > mock->value.momentLaid)
        return true;
    return !m_loadedInPreload.get(place) && momentLoaded < mock->value.momentOfTestFile;
}

// What setMockInPlace() does for a module of which nothing was filled, for one that has run. Its variables, its status, its namespace object and its record stay what they are.
bool Modules::setMockOverModuleThatHasRun(ModuleID module, JSObject* mock, JSValue whatRequireAnswers)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID withExports = moduleWithExportsOf(module);
    bool isFile = withExports != module;
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
    setAsideNamespaceUnderMock(withExports);
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
    // The body has run: whoever gets the original beside the mock gets what it exported.
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
    loaded.mockInPlace.set(*this, nullptr);
    exporting.mockInPlace.set(*this, nullptr);
    --m_numberOfMocksInPlace;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithMockInPlace.clear(module.index());
    ProgramStatics::writeVariable(m_instance, *m_table.wordOfMock(module), JSValue());
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
    // An object that was made under the mock is the mock's, and whoever holds it keeps it. The object of the module is the one from before, as in `bun test`.
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID each(index);
        if (each == withExports || data(each).hasDynamicStarExport())
            letGoOfNamespaceMadeUnderMock(each);
    }
    if (JSObject* own = exporting.namespaceBeforeMockInPlace.get()) {
        uncheckedDowncast<ModuleNamespace>(own)->setKeepsToModule(false);
        exporting.namespaceBeforeMockInPlace.set(*this, nullptr);
        setNamespace(withExports, ModulePhase::Evaluation, own);
    }
}

// The object in the slot of the module goes to whoever asks by name. The first that a mock finds there is the module's own; a later one is the object of the mock before, which keeps that mock.
void Modules::setAsideNamespaceUnderMock(ModuleID module)
{
    auto* object = dynamicDowncast<ModuleNamespace>(namespaceIfCreated(module, ModulePhase::Evaluation));
    if (!object)
        return;
    LoadedModule& loaded = ensureLoaded(module);
    if (!loaded.namespaceBeforeMockInPlace) {
        object->setKeepsToModule(true);
        loaded.namespaceBeforeMockInPlace.set(*this, object);
    }
    setNamespace(module, ModulePhase::Evaluation, nullptr);
}

JSObject* Modules::namespaceOfModuleItself(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto mock = m_mocksOverModulesThatHaveRun.find(m_table.fileBehindFace(module).value_or(module));
    if (mock == m_mocksOverModulesThatHaveRun.end())
        RELEASE_AND_RETURN(scope, moduleNamespace(module, ModulePhase::Evaluation));
    m_namespacesThatOnlyImagesAboveHold.clear(module.index());
    // The mock waits for the first load: what the module has is its own. This is no load.
    if (!mock->value.isInPlace) {
        if (JSObject* own = namespaceIfCreated(module, ModulePhase::Evaluation))
            return own;
        RELEASE_AND_RETURN(scope, createNamespace(module, ModulePhase::Evaluation, true));
    }
    LoadedModule& loaded = ensureLoaded(module);
    if (JSObject* own = loaded.namespaceBeforeMockInPlace.get())
        return own;
    // Nobody had asked for it when the mock came.
    JSObject* ofMock = namespaceIfCreated(module, ModulePhase::Evaluation);
    setNamespace(module, ModulePhase::Evaluation, nullptr);
    JSObject* own = nullptr;
    {
        SetForScope makesIt(m_makesNamespaceOfModuleItself, true);
        // The names of a face are those of its table.
        JSObject* namesOfMock = loaded.capturedExports.get();
        JSObject* namesOfFile = loaded.capturedExportsBeforeMockInPlace.get();
        if (namesOfFile) {
            loaded.capturedExports.set(*this, namesOfFile);
            loaded.exportsWhatMockNamesAlone = false;
        }
        own = createNamespace(module, ModulePhase::Evaluation, true);
        if (namesOfFile) {
            loaded.capturedExports.set(*this, namesOfMock);
            loaded.exportsWhatMockNamesAlone = true;
        }
    }
    setNamespace(module, ModulePhase::Evaluation, ofMock);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (auto* object = dynamicDowncast<ModuleNamespace>(own)) {
        object->setKeepsToModule(true);
        loaded.namespaceBeforeMockInPlace.set(*this, object);
    }
    return own;
}

bool Modules::isHiddenInModuleItself(ModuleID module, unsigned index) const
{
    auto mock = m_mocksOverModulesThatHaveRun.find(m_table.fileBehindFace(module).value_or(module));
    if (mock != m_mocksOverModulesThatHaveRun.end() && mock->value.isInPlace)
        return mock->value.exportsHiddenBefore.get(index);
    return (m_table.exports(module)[index].isFromStarExport && isExportAmbiguous(module, index)) || isLeftOut(module, index, TablesBehindStars::MayLackName);
}

JSValue Modules::readExportOfModuleItself(ModuleID module, unsigned index)
{
    SetForScope readsPast(m_moduleReadPastItsMock, module);
    return readExportFromOutside(module, index);
}

auto Modules::mockState(ModuleID module) -> MockState
{
    LoadedModule* loaded = loadedIfExists(module);
    bool waits = m_modulesWithMockToRun.get(module.index());
    bool isInPlace = m_modulesWithMockInPlace.get(module.index());
    bool isLaidOver = loaded && loaded->hasMockLaidOver;
    bool isOverModuleThatHasRun = isLeftAloneByMock(module);
    bool isToBeLetGo = m_modulesWithMockToLetGo.get(module.index());
    bool hasWaited = m_modulesWithMockThatWaited.get(module.index());
    bool hasKeptError = m_modulesWithLoadEndedByMock.get(module.index()) || m_mockedModulesWithErrorKeptByLoader.get(module.index());
    bool isOfLibrary = data(module).isOfLibrary();
    ModuleStatus body = status(module);
    bool hasBegun = body != ModuleStatus::Unlinked && body != ModuleStatus::Linking && body != ModuleStatus::Linked;
    if (isLaidOver && (waits || isInPlace))
        return MockState::NoneOfThem;
    if (isToBeLetGo)
        return isInPlace && !waits ? MockState::Ended : MockState::NoneOfThem;
    if (waits && isInPlace && !isOverModuleThatHasRun)
        return isMocked(module) ? MockState::InPlaceWhileAnotherWaits : MockState::NoneOfThem;
    if (waits) {
        if ((!isMocked(module) && !isOfLibrary) || (hasBegun && !isOverModuleThatHasRun && !isOfLibrary && data(module).kind != ModuleKind::CommonJs))
            return MockState::NoneOfThem;
        return hasKeptError ? MockState::Failed : MockState::Waits;
    }
    if (isInPlace) {
        if (isOverModuleThatHasRun)
            return MockState::InPlaceOverModuleThatHasRun;
        return body == ModuleStatus::Evaluating || body == ModuleStatus::EvaluatingAsync ? MockState::NoneOfThem : MockState::InPlace;
    }
    if (isLaidOver)
        return hasBegun ? MockState::LaidOver : MockState::NoneOfThem;
    if (isOverModuleThatHasRun)
        return MockState::Runs;
    // willMockModule() ahead of the load: the names of the module hold what the mock gave, and its body never runs.
    if (isMocked(module))
        return hasWaited || isOfLibrary || m_modulesWhoseFactoryRuns.get(module.index()) ? MockState::Runs : MockState::Fills;
    if (hasKeptError)
        return MockState::Failed;
    return hasWaited ? MockState::NoneOfThem : MockState::None;
}

String Modules::describeMockState(ModuleID module)
{
    static constexpr std::array<ASCIILiteral, 11> names { "none"_s, "waits"_s, "runs"_s, "in place"_s, "in place while another waits"_s, "in place over a module that has run"_s, "fills"_s, "laid over"_s, "failed"_s, "ended"_s, "NONE OF THEM"_s };
    static constexpr std::array<ASCIILiteral, 7> bodies { "unlinked"_s, "linking"_s, "linked"_s, "evaluating"_s, "awaiting"_s, "evaluated"_s, "failed"_s };
    MockState state = mockState(module);
    ASCIILiteral kind = data(module).isOfLibrary() ? "library"_s : data(module).kind == ModuleKind::CommonJs ? "CommonJS"_s : data(module).isFaceOfFile() ? "face"_s : data(module).isESModuleOfProgram() ? "ES"_s : "other"_s;
    StringBuilder text;
    text.append(kind, ' ', names[static_cast<size_t>(state)], " (body "_s, bodies[static_cast<size_t>(status(module)) - static_cast<size_t>(ModuleStatus::Unlinked)], isLeftToHost(module) ? ", left to the host)"_s : ")"_s);
    if (state == MockState::NoneOfThem) {
        LoadedModule* loaded = loadedIfExists(module);
        auto say = [&](bool isSet, ASCIILiteral what) {
            if (isSet)
                text.append(' ', what);
        };
        text.append(" ["_s);
        say(isMocked(module), "mocked"_s);
        say(m_modulesWithMockToRun.get(module.index()), "to-run"_s);
        say(m_modulesWithMockThatWaited.get(module.index()), "waited"_s);
        say(m_modulesWithMockThatAsksAgain.get(module.index()), "asks-again"_s);
        say(m_modulesWithMockInPlace.get(module.index()), "in-place"_s);
        say(m_modulesWithMockToLetGo.get(module.index()), "to-let-go"_s);
        say(loaded && loaded->hasMockLaidOver, "laid-over"_s);
        say(isLeftAloneByMock(module), "over-run"_s);
        say(m_modulesWithLoadEndedByMock.get(module.index()), "load-ended"_s);
        say(m_mockedModulesWithErrorKeptByLoader.get(module.index()), "error-kept"_s);
        text.append(" ]"_s);
    }
    return text.toString();
}

void Modules::countMockStates(ASCIILiteral door, const String& before, ModuleID module)
{
    countMockTransition(*m_instance.vm, makeString(before, " > "_s, door, " > "_s, describeMockState(module)));
}

// Counts what a door between the host and the engine does to the state of a mock, where the counts are asked for.
class MockDoor {
public:
    MockDoor(Modules* modules, ModuleID module, ASCIILiteral door)
        : m_modules(Options::aotCountsPath() ? modules : nullptr)
        , m_module(module)
        , m_door(door)
    {
        if (m_modules) [[unlikely]]
            m_before = m_modules->describeMockState(module);
    }

    ~MockDoor()
    {
        if (m_modules) [[unlikely]]
            m_modules->countMockStates(m_door, m_before, m_module);
    }

private:
    Modules* m_modules;
    ModuleID m_module;
    ASCIILiteral m_door;
    String m_before;
};

void Modules::deferMock(ModuleID module, AfterFailure afterFailure)
{
    // Whoever was linked against the earlier mock reads it until the load that this one waits for.
    m_modulesWithMockToRun.set(module.index());
    m_modulesWithMockThatAsksAgain.set(module.index(), afterFailure == AfterFailure::AsksAgain);
    dropErrorsOfLoadsEndedByMockOf(module);
    if (data(module).kind == ModuleKind::CommonJs)
        ensureRecord(module);
    if (data(module).isOfLibrary())
        willMockLibraryModule(module);
    if (!data(module).isOfLibrary() && m_table.hasFilePath(module)) {
        m_modulesWithMockThatWaited.set(module.index());
        m_modulesLoadedBeforeMockThatWaited.set(module.index(), wasLoadedWithImporter(module));
        willMock(module);
    }
}

// What deferMock() did is taken back. The record of a CommonJS file stays: its code goes on asking the run time for its status.
void Modules::forgetMockThatWaits(ModuleID module)
{
    m_modulesWithMockToRun.clear(module.index());
    if (!m_modulesWithMockThatWaited.get(module.index()))
        return;
    m_modulesWithMockThatWaited.clear(module.index());
    m_modulesLoadedBeforeMockThatWaited.clear(module.index());
    didEndMock(module);
}

// It does not wait while its factory runs, which may load the module. A factory that fails leaves nothing in place of the module: the next load asks the mock again, as under `bun test`.
bool Modules::runMockThatWaitsSlow(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    MockDoor door(this, module, "runMock"_s);
    m_modulesWithMockToRun.clear(module.index());
    bool ranAlready = m_modulesWhoseFactoryRuns.set(module.index());
    bool factoryHasNotSettled = host().runMock(m_instance.globalObject, moduleIDForHost(*m_instance.program, module));
    if (!ranAlready)
        m_modulesWhoseFactoryRuns.clear(module.index());
    if (Exception* exception = scope.exception()) [[unlikely]] {
        m_modulesWithMockToRun.set(module.index());
        if (m_importsMadeByOwnFactory.contains(module) && !m_instance.vm->isTerminationException(exception)) {
            m_mockedModulesWithErrorKeptByLoader.set(module.index());
            ensureLoaded(module).errorOfMockKeptByLoader.set(*this, exception->value());
        }
        return false;
    }
    m_importsMadeByOwnFactory.removeAll(module);
    return factoryHasNotSettled;
}

void Modules::keepErrorOfLoad(ModuleID root, JSValue error, const ScopedLambda<bool(ModuleID)>& endedLoad)
{
    UncheckedKeyHashMap<ModuleID, Vector<ModuleID, 2>> importers;
    BitVector seen(m_table.size());
    BitVector ended(m_table.size());
    seen.quickSet(root.index());
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (endedLoad(module)) {
            ended.quickSet(module.index());
            continue;
        }
        if (isMocked(module) || hasBegunToLoad(module))
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (request.isHoisted)
                continue;
            importers.add(request.targetModule(), Vector<ModuleID, 2>()).iterator->value.append(module);
            if (!seen.quickSet(request.target()))
                work.append(request.targetModule());
        }
    }
    BitVector above(m_table.size());
    for (size_t module : ended)
        work.append(ModuleID(module));
    while (!work.isEmpty()) {
        ModuleID imported = work.takeLast();
        auto found = importers.find(imported);
        if (found == importers.end())
            continue;
        for (ModuleID importer : found->value) {
            if (above.quickSet(importer.index()))
                continue;
            work.append(importer);
            if (m_modulesWithLoadEndedByMock.set(importer.index()))
                continue;
            ensureLoaded(importer).errorKeptByLoader.set(*this, error);
        }
    }
}

// The loader knows the requester alone where it does not walk the graph below it. Whether the requester began the load its entry says.
void Modules::didFailToFetchRequest(ModuleID requester, const Identifier& key, JSValue error)
{
    keepErrorOfLoad(requester, error, [&](ModuleID module) { return standsForFileWithKey(module, key); });
}

bool Modules::standsForFileWithKey(ModuleID module, const Identifier& key)
{
    if (!data(module).isUncompiledCode())
        return false;
    const Vector<Identifier>& keysOfFilesBelow = m_instance.vm->aotState().parts[m_instance.program->position()].keysOfFilesInImagesBelow;
    if (module.index() < keysOfFilesBelow.size() && keysOfFilesBelow[module.index()] == key)
        return true;
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->resolvedKey == key;
}

void Modules::dropErrorOfLoadEndedByMock(ModuleID module)
{
    if (m_modulesWithLoadEndedByMock.clear(module.index()))
        ensureLoaded(module).errorKeptByLoader.set(*this, JSValue());
    dropErrorOfMockKeptByLoader(module);
}

void Modules::dropErrorOfMockKeptByLoader(ModuleID module)
{
    if (m_mockedModulesWithErrorKeptByLoader.clear(module.index())) [[unlikely]]
        ensureLoaded(module).errorOfMockKeptByLoader.set(*this, JSValue());
}

// A new mock takes the record of the module out of the loader, and with it the records of all that import the module.
void Modules::dropErrorsOfLoadsEndedByMockOf(ModuleID mocked)
{
    dropErrorOfMockKeptByLoader(mocked);
    dropErrorsOfLoadsThatCameTo([&](ModuleID module) { return module == mocked; });
    if (!m_instance.vm->aotState().hasSeveralImages) [[likely]]
        return;
    Identifier key = moduleKey(mocked);
    for (Instance* instance = m_instance.above; instance; instance = instance->above) {
        if (Modules* modules = instance->state->modules.get())
            modules->dropErrorsOfLoadsThatCameTo([&](ModuleID module) { return modules->standsForFileWithKey(module, key); });
    }
}

void Modules::dropErrorsOfLoadsThatCameTo(const ScopedLambda<bool(ModuleID)>& isMocked)
{
    if (m_modulesWithLoadEndedByMock.isEmpty()) [[likely]]
        return;
    Vector<ModuleID, 8> above;
    for (size_t kept : m_modulesWithLoadEndedByMock) {
        BitVector seen(m_table.size());
        Vector<ModuleID, 16> work { ModuleID(kept) };
        while (!work.isEmpty()) {
            ModuleID module = work.takeLast();
            if (isMocked(module)) {
                above.append(ModuleID(kept));
                break;
            }
            for (const ModuleRequestData& request : m_table.imports(module)) {
                if (!request.isHoisted && !seen.quickSet(request.target()))
                    work.append(request.targetModule());
            }
        }
    }
    for (ModuleID module : above)
        dropErrorOfLoadEndedByMock(module);
}

// The loader has made an entry for what a statement asks for, and none yet for what an import() or a require() asks for.
bool Modules::isFetchedForImportStatement(ModuleID module)
{
    ModuleRegistryEntry* entry = loader()->registryEntry(moduleKey(module));
    return entry && entry->status() == ModuleRegistryEntry::Status::New;
}

bool Modules::runMocksBelowModuleThatLoaderFetchesSlow(ModuleID module)
{
    MockDoor door(this, module, "the loader fetches it"_s);
    if (m_modulesWithMockToRun.isEmpty()) [[likely]] {
        evaluateModulesOwedBelow(module);
        return false;
    }
    // The entry that a statement left when the factory failed is still there.
    bool isAskedForByImportCall = m_instance.vm->aotState().frameOfImportThatAsksLoader;
    if (isAskedForByImportCall)
        dropErrorOfMockKeptByLoader(module);
    return runMocksBelow(module, !isAskedForByImportCall && isFetchedForImportStatement(module) ? RootOfLoad::IsImportedByStatement : RootOfLoad::BeginsLoad);
}

bool Modules::mockFailedBehindImportStatement(ModuleID module)
{
    return m_modulesWithMockToRun.get(module.index()) && isFetchedForImportStatement(module);
}

const MockOperations& Modules::mockOperations()
{
    static constexpr MockOperations operations {
        .runMocksBelow = [](Modules& modules, ModuleID root, RootOfLoad rootOfLoad) { return modules.runMocksBelowSlow(root, rootOfLoad); },
        .runMocksBelowModuleThatLoaderFetches = [](Modules& modules, ModuleID module) { return modules.runMocksBelowModuleThatLoaderFetchesSlow(module); },
        .evaluateModulesOwedBelow = [](Modules& modules, ModuleID root, bool hasImporterAboveThatRan) { modules.evaluateModulesOwedBelowSlow(root, hasImporterAboveThatRan); },
        .letGoOfMockThatEnded = [](Modules& modules, ModuleID module) { modules.letGoOfMockThatEndedSlow(module); },
        .runMockThatWaits = [](Modules& modules, ModuleID module) { return modules.runMockThatWaitsSlow(module); },
        .mockThatAnswersFor = [](const Modules& modules, ModuleID passingOn, const ModuleExportData& passedOn) { return modules.mockThatAnswersForSlow(passingOn, passedOn); },
        .isMockedInItsImage = [](Modules& modules, ModuleID module, MocksAskedFor which) { return modules.isMockedInItsImageSlow(module, which); },
        .answerRequireWithOriginal = [](Modules& modules, ModuleID file) { return modules.answerRequireWithOriginalSlow(file); },
    };
    return operations;
}

const MockOperations* mockOperations()
{
    return &Modules::mockOperations();
}

bool Modules::runMocksBelowSlow(ModuleID root, RootOfLoad rootOfLoad)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!m_modulesOwedToImportersThatRan.isEmpty() || !m_modulesWithMockToLetGo.isEmpty() || !m_modulesThatLoaderLetGo.isEmpty() || vm.aotState().hasSeveralImages) [[unlikely]] {
        evaluateModulesOwedBelow(root);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (m_modulesWithMockToRun.isEmpty()) [[likely]]
        return false;
    // The walk does nothing where it comes to none of them.
    MocksBelowRoot mocks = mocksBelow(root);
    bool mayComeToOne = false;
    for (size_t module : m_modulesWithMockToRun)
        mayComeToOne |= !isMocked(ModuleID(module)) || !mocks.hides(ModuleID(module));
    if (!mayComeToOne)
        return false;
    // An import() of a CommonJS file begins at the face of the file.
    std::optional<ModuleID> fileOfRoot = m_table.fileBehindFace(root);
    BitVector reached(m_table.size());
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (reached.quickSet(module.index()))
            continue;
        bool isImportedByStatement = (module != root && module != fileOfRoot) || rootOfLoad == RootOfLoad::IsImportedByStatement;
        JSValue kept = errorOfLoadEndedByMock(module);
        if (!kept && isImportedByStatement && m_mockedModulesWithErrorKeptByLoader.get(module.index()))
            kept = loadedIfExists(module)->errorOfMockKeptByLoader.get();
        if (!kept && !isImportedByStatement && m_mockedModulesWithErrorKeptByLoader.get(module.index()) && m_importsMadeByOwnFactory.removeFirst(module)) [[unlikely]]
            kept = loadedIfExists(module)->errorOfMockKeptByLoader.get();
        if (kept) [[unlikely]] {
            keepErrorOfLoad(root, kept, [&](ModuleID each) { return each == module; });
            throwException(m_instance.globalObject, scope, kept);
            return false;
        }
        if (m_modulesWithMockToRun.get(module.index())) {
            bool factoryHasNotSettled = runMockThatWaits(module);
            if (Exception* exception = scope.exception()) [[unlikely]] {
                if (vm.isTerminationException(exception) || m_modulesWithMockThatAsksAgain.get(module.index()))
                    return false;
                keepErrorOfLoad(root, exception->value(), [&](ModuleID each) { return each == module; });
                if (isImportedByStatement) {
                    m_mockedModulesWithErrorKeptByLoader.set(module.index());
                    ensureLoaded(module).errorOfMockKeptByLoader.set(*this, exception->value());
                }
                return false;
            }
            if (factoryHasNotSettled)
                return true;
        }
        if (isMocked(module))
            continue;
        // Depth first, in the order of the text: what is taken first is appended last.
        for (const ModuleRequestData& request : m_table.requests(module) | std::views::reverse) {
            // The load of a module that keeps the module itself does not come to the mock. A require() runs the factory of its own file where it stands; the face of a CommonJS file asks for the file as an import does.
            if (!request.isHoisted && (!request.isRequire || data(module).isFaceOfFile()) && seesMockOf(request.targetModule(), module))
                work.append(request.targetModule());
        }
    }
    return false;
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
        if (m_moduleWithMockBeingLaid && moduleWithExportsOf(*m_moduleWithMockBeingLaid) != module) {
            Vector<ModuleID>& behindStars = ensureLoaded(*m_moduleWithMockBeingLaid).modulesBehindStarsUnderMock;
            if (!behindStars.contains(module))
                behindStars.append(module);
        }
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
    JSGlobalObject* realm = m_instance.globalObject;
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

    ModuleNamespace::DynamicExports exportObjects;
    createDynamicExports(module, exportObjects.names, exportObjects.capturedValues, exportObjects.liveSources);
    RETURN_IF_EXCEPTION(scope, nullptr);

    bool isRequired = phase == ModulePhase::Evaluation && m_modulesWithMarkOfRequire.get(module.index());
    if (phase == ModulePhase::Evaluation && m_modulesWithExtensibleNamespace.get(module.index()))
        shouldPreventExtensions = false;
    ModuleNamespace* namespaceObject = ModuleNamespace::create(vm, namespaceStructure(shouldPreventExtensions), module, phase, exportObjects);
    if (isRequired && !namespaceObject->hasExport(vm.propertyNames->__esModule.impl()))
        namespaceObject->setESModuleMarker(true);
    if (JSObject* mock = phase == ModulePhase::Evaluation && !m_makesNamespaceOfModuleItself && isLeftAloneByMock(module) ? mockInPlace(module) : nullptr) [[unlikely]]
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
    m_namespacesThatOnlyImagesAboveHold.clear(module.index());
    if (JSObject* namespaceObject = namespaceIfCreated(module, phase))
        return namespaceObject;
    return createNamespace(module, phase, shouldPreventExtensions);
}

JSObject* Modules::moduleNamespaceForImageAbove(ModuleID module, ModulePhase phase)
{
    bool wasHeldByImagesAboveAlone = m_namespacesThatOnlyImagesAboveHold.get(module.index());
    bool isCreatedNow = !namespaceIfCreated(module, phase);
    JSObject* namespaceObject = moduleNamespace(module, phase);
    auto* ofModule = namespaceObject ? dynamicDowncast<ModuleNamespace>(namespaceObject) : nullptr;
    if (phase == ModulePhase::Evaluation && (isCreatedNow || wasHeldByImagesAboveAlone) && ofModule && ofModule == namespaceIfCreated(module, phase))
        m_namespacesThatOnlyImagesAboveHold.set(module.index());
    return namespaceObject;
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

static void tellHostOfImport(JSGlobalObject* realm, JSPromise* promise, const String& path)
{
    if (auto* didStartImport = moduleHostOf(realm).didStartImport; didStartImport && promise->status() == JSPromise::Status::Pending)
        didStartImport(realm, promise, path);
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
        scope.release();
        tellHostOfImport(realm, promise, data(file).mayBeMocked() ? keyWithoutForm(file).string() : String());
        return promise;
    };

    if (m_modulesWithLoadEndedByMock.get(module.index())) [[unlikely]]
        RELEASE_AND_RETURN(scope, rejectImportWithErrorOfLoad(module, ImportCaller::CompiledCode));
    if (m_modulesWhoseFactoryRuns.get(module.index())) [[unlikely]]
        m_importsMadeByOwnFactory.append(module);
    else if (!m_importsMadeByOwnFactory.contains(module)) [[likely]]
        dropErrorOfMockKeptByLoader(module);
    walkStopped = runMocksBelow(module);
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

    const Identifier& key = resolvedKey(module);
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    // The build has left the call to the loader, with its type.
    if (std::optional<ModuleID> found = findWithAttributes(key, fetchParameters->get(), FormWithoutAttributes::IsAnotherModule))
        return start(*found);
    if (m_instance.vm->aotState().hasSeveralImages) [[unlikely]] {
        Instance::forEachOf(realm, [&](Instance& instance) {
            Modules* modules = instance.state ? instance.state->modules.get() : nullptr;
            if (!modules || instance.program->position() >= m_instance.program->position() || scope.exception())
                return;
            std::optional<ModuleID> file = modules->findByKey(key);
            if (!file)
                return;
            if (!modules->m_modulesWhoseFactoryRuns.get(file->index()) && !modules->m_importsMadeByOwnFactory.contains(*file))
                modules->dropErrorOfMockKeptByLoader(*file);
            modules->runMocksBelow(*file);
        });
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    }
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
        loaded.firstRequirerOfOtherImage.set(*this, nullptr);
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
            if (request.isDeferred || request.isRequire || isSeen.quickSet(request.target()))
                continue;
            ModuleID target = request.targetModule();
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
    countModulesSoughtForRequireCache(*m_instance.vm, m_table.size() + toFollow.size());
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
    host().createModuleObject(m_instance.globalObject, pathString(module), moduleIDForHost(*m_instance.program, module), traits, objects);
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
    // The first image of a realm holds Bun's own modules for every image, and a mock is laid on those. What an image above holds of them is for its own library code alone.
    if (found && m_instance.program->position() && (data(*found).kind == ModuleKind::RuntimeSource || data(*found).kind == ModuleKind::Runtime || data(*found).isOfLibrary()))
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
    std::optional<ModuleID> module = moduleOfImage(*m_instance.program, host().findModuleWithAttributes(m_instance.globalObject, key.string(), *fetchParameters));
    if (module && formWithoutAttributes == FormWithoutAttributes::IsAnotherModule && m_table.pathText(*module) == key.string())
        module = std::nullopt;
    if (std::optional<ModuleID> loaded = findFormThatStatementLoaded(key, *fetchParameters, module))
        module = loaded;
    if (module)
        m_formsAnsweredFirst.add(keyOfLoader, FormAnsweredFirst { key, *module });
    return module;
}

// As the host decides where it is asked to fetch a file for an import. While a require() loads a graph it reads every text itself, also for an import() that is called meanwhile.
bool Modules::isTextReadOnOtherThread(ModuleID file)
{
    return host().callInLaterTurn && isFileOfProgram(file) && !m_instance.vm->synchronousModuleQueue() && !hostLoadsSynchronously(file);
}

bool Modules::textOfImportComesInLaterTurn(ModuleID module, AskedBy askedBy)
{
    // The loader has the text of whatever it has come to, and what a require() has begun is not begun by this call.
    ModuleID file = fileOf(module);
    if (isCommonJSFileOrFace(module)) {
        for (ModuleID fileOrFace : { module, file }) {
            if (status(fileOrFace) != ModuleStatus::Unlinked && status(fileOrFace) != ModuleStatus::Linked)
                return false;
        }
    } else if (!isNotLoaded(module))
        return false;
    if (askedBy == AskedBy::Import) {
        for (ModuleID fileOrFace : { module, file }) {
            if (loader()->registryEntry(moduleKey(fileOrFace)))
                return false;
        }
    }
    return isTextReadOnOtherThread(file);
}

void Modules::holdText(JSPromise* promise, ModuleID module, JSCell* requestedRecord, std::optional<int64_t> referrerAsyncOrder, AskedBy askedBy, ImportCaller caller)
{
    VM& vm = *m_instance.vm;
    VMState& state = vm.aotState();
    uint64_t place = static_cast<uint64_t>(state.turnOfTexts) << 33 | static_cast<uint64_t>(askedBy == AskedBy::Import) << 32 | state.numberOfTextsAskedFor++;
    bool waitsApart = askedBy == AskedBy::Import && !m_innermostFilesBeingRun && !state.numberOfTextsThatWait;
    uint32_t levelsAddedToPlace = 0;
    bool comesInFrontOfCallsAtItsPlace = false;
    bool comesWithCallInFront = false;
    // A place says where a call stands among those that wait. A call that waits alone comes first wherever it stands, so the levels below it are counted once another call finds it waiting.
    bool waitsAlone = waitsApart && !state.numberOfCallsThatWaitApart;
    if (waitsApart) {
        if (state.numberOfCallsThatWaitApart == 1) {
            for (Instance* instance : state.instances) {
                if (Modules* modules = instance->state ? instance->state->modules.get() : nullptr)
                    modules->countLevelsBelowCallThatWaitedAlone();
            }
        }
        ++state.numberOfCallsThatWaitApart;
        // The texts of a level come a turn behind those of the level above, so a shallow graph runs ahead of a deep one that was asked for first.
        // Two calls for one file settle together.
        auto waiting = textsThatWait();
        auto earlier = std::ranges::find_if(waiting, [&](const TextThatWaits& text) {
            return text.waitsApart && text.module == module && !text.requestedRecord && !requestedRecord;
        });
        if (earlier != waiting.end()) {
            place = earlier->place;
            levelsAddedToPlace = earlier->levelsAddedToPlace;
        } else if (!waitsAlone) {
            bool comesToEarlierCall = false;
            levelsAddedToPlace = countLevelsBelowRoot(module, comesToEarlierCall);
            bool hasPlaceOfEarlierCall = false;
            if (comesToEarlierCall) {
                // What it shares with them comes when the first to ask for it has it.
                LastTextBelowCall last = findLastTextBelowCall(module);
                levelsAddedToPlace = last.turn - state.turnOfTexts;
                if (last.earlierCall) {
                    hasPlaceOfEarlierCall = true;
                    // The news of the text goes up a level a job: the root that is nearer to it runs first, and of two that are as near the one that asked first.
                    comesInFrontOfCallsAtItsPlace = last.levelsBelowRoot < last.levelsBelowRootOfEarlierCall;
                    comesWithCallInFront = !comesInFrontOfCallsAtItsPlace;
                    uint64_t placeOfEarlierCall = waiting[*last.earlierCall].place;
                    uint32_t turnsSooner = static_cast<uint32_t>(placeOfEarlierCall >> 33) - last.turn;
                    place = placeOfEarlierCall - (static_cast<uint64_t>(turnsSooner) << 33);
                    levelsAddedToPlace = waiting[*last.earlierCall].levelsAddedToPlace - turnsSooner;
                    if (turnsSooner) {
                        for (TextThatWaits& text : waiting) {
                            if (text.place != placeOfEarlierCall)
                                continue;
                            text.place = place;
                            text.levelsAddedToPlace = levelsAddedToPlace;
                        }
                        std::ranges::stable_sort(waiting, { }, &TextThatWaits::place);
                    }
                }
            }
            if (!hasPlaceOfEarlierCall)
                place += static_cast<uint64_t>(levelsAddedToPlace) << 33;
        }
    } else {
        if (state.numberOfCallsThatWaitApart) {
            for (Instance* instance : state.instances) {
                if (Modules* modules = instance->state ? instance->state->modules.get() : nullptr)
                    modules->putCallsThatWaitApartInLine();
            }
        }
        ++state.numberOfTextsThatWait;
    }
    size_t index = m_textsThatWait.size();
    while (index > m_numberOfTextsThatHaveCome && (m_textsThatWait[index - 1].place > place || (comesInFrontOfCallsAtItsPlace && m_textsThatWait[index - 1].place == place)))
        --index;
    m_textsThatWait.insert(index, TextThatWaits { place, Strong<JSPromise>(vm, promise), module, Strong<JSCell>(vm, requestedRecord), referrerAsyncOrder, waitsApart, levelsAddedToPlace, caller, waitsAlone, comesWithCallInFront });
    if (comesInFrontOfCallsAtItsPlace)
        m_textsThatWait[index + 1].comesWithCallInFront = true;
    host().callInLaterTurn(m_instance.globalObject);
}

void Modules::countLevelsBelowCallThatWaitedAlone()
{
    for (TextThatWaits& text : textsThatWait()) {
        if (!std::exchange(text.levelsAreLeftToCount, false))
            continue;
        RunningImageScope imageOfModules(m_instance.vm->aotState(), m_instance.program);
        m_modulesBelowCallsThatWait.clearAll();
        bool comesToEarlierCall = false;
        text.levelsAddedToPlace = countLevelsBelowRoot(text.module, comesToEarlierCall);
        text.place += static_cast<uint64_t>(text.levelsAddedToPlace) << 33;
    }
}

void Modules::putCallsThatWaitApartInLine()
{
    VMState& state = m_instance.vm->aotState();
    for (TextThatWaits& text : textsThatWait()) {
        if (!std::exchange(text.waitsApart, false))
            continue;
        text.levelsAreLeftToCount = false;
        text.comesWithCallInFront = false;
        text.place -= static_cast<uint64_t>(std::exchange(text.levelsAddedToPlace, 0)) << 33;
        --state.numberOfCallsThatWaitApart;
        ++state.numberOfTextsThatWait;
    }
    std::ranges::stable_sort(textsThatWait(), { }, &TextThatWaits::place);
}

bool Modules::holdTextOfRequest(JSPromise* fetched, ModuleRecord* requested, ModuleID module)
{
    if (!isTextReadOnOtherThread(fileOf(module)))
        return false;
    holdText(fetched, module, requested, std::nullopt, AskedBy::Loader);
    return true;
}

void Modules::holdTextOfImportFromOtherImage(JSPromise* fetched, AbstractModuleRecord* root, ModuleID module)
{
    holdText(fetched, module, root, std::nullopt, AskedBy::Import);
}

bool Modules::deliverFirstTextThatWaits(TextOfImport textOfImport)
{
    VMState& state = m_instance.vm->aotState();
    TextThatWaits text = WTF::move(m_textsThatWait[m_numberOfTextsThatHaveCome]);
    if (++m_numberOfTextsThatHaveCome == m_textsThatWait.size()) {
        m_textsThatWait.clear();
        m_numberOfTextsThatHaveCome = 0;
    }
    if (textOfImport == TextOfImport::HasCome)
        m_promiseOfCallThatOthersComeWith = !m_textsThatWait.isEmpty() && m_textsThatWait[m_numberOfTextsThatHaveCome].comesWithCallInFront ? text.promise.get() : nullptr;
    if (text.waitsApart)
        --state.numberOfCallsThatWaitApart;
    else
        --state.numberOfTextsThatWait;
    if (m_instance.globalObject->microtaskRunnability() == QueuedTaskResult::Discard)
        return false;
    RunningImageScope imageOfModules(state, m_instance.program);
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSPromise* promise = text.promise.get();
    if (text.requestedRecord) {
        promise->fulfill(vm, text.requestedRecord.get());
        return true;
    }
    bool isMadeAgain = text.waitsApart || text.caller == ImportCaller::Host;
    JSPromise* loaded = startImport(text.module, text.referrerAsyncOrder, isMadeAgain ? text.caller : ImportCaller::Host, textOfImport, isMadeAgain ? promise : nullptr);
    if (scope.exception()) [[unlikely]] {
        promise->rejectWithCaughtException(vm, scope);
        return true;
    }
    scope.release();
    if (loaded != promise)
        settleAsLoaderDoes(m_instance.globalObject, promise, loaded);
    return true;
}

// In the job in which the graph of the call in front has run. Where that call is left to the loader they come in turns of their own.
void Modules::deliverCallsThatComeWith(JSPromise* promiseOfCallInFront)
{
    if (m_promiseOfCallThatOthersComeWith != promiseOfCallInFront) [[likely]]
        return;
    m_promiseOfCallThatOthersComeWith = nullptr;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    while (!m_textsThatWait.isEmpty() && m_textsThatWait[m_numberOfTextsThatHaveCome].comesWithCallInFront) {
        deliverFirstTextThatWaits(TextOfImport::HasComeWithCallInFront);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void deliverTextThatWaits(VM& vm)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    VMState& state = vm.aotState();
    ++state.turnOfTexts;
    bool isDelivered = false;
    while (!isDelivered && (state.numberOfTextsThatWait || state.numberOfCallsThatWaitApart)) {
        Modules* first = nullptr;
        for (Instance* instance : state.instances) {
            Modules* modules = instance->state ? instance->state->modules.get() : nullptr;
            if (modules && modules->placeOfFirstTextThatWaits() && (!first || *modules->placeOfFirstTextThatWaits() < *first->placeOfFirstTextThatWaits()))
                first = modules;
        }
        if (!first)
            return;
        isDelivered = first->deliverFirstTextThatWaits();
        // Only a request to terminate is left behind, and the jobs find it.
        if (scope.exception()) [[unlikely]]
            return;
    }
}

static JSC_DECLARE_HOST_FUNCTION(importBehindFactories);
static JSC_DECLARE_HOST_FUNCTION(importFromOtherImageBehindFactories);

// Another load may have come to the mock first: `await Promise.all([import("./a"), import("./b")])`.
JSPromise* Modules::factoryThatWaitsBelow(ModuleID root, FactoryAskedForBy askedBy, JSValue& state)
{
    if (!host().factoryThatWaits)
        return nullptr;
    std::optional<BitVector> reached;
    return host().factoryThatWaits(m_instance.globalObject, [&](ModuleID moduleOfHost) {
        std::optional<ModuleID> module = moduleOfImage(*m_instance.program, moduleOfHost);
        if (!module)
            return false;
        if (!reached) {
            reached.emplace(m_table.size());
            Vector<ModuleID, 32> work { root };
            while (!work.isEmpty()) {
                ModuleID next = work.takeLast();
                if (reached->quickSet(next.index()) || isMocked(next))
                    continue;
                for (const ModuleRequestData& request : m_table.requests(next)) {
                    if (!request.isHoisted)
                        work.append(request.targetModule());
                }
            }
        }
        return reached->quickGet(module->index());
    }, askedBy, state);
}

// `stateOfWait`: ModuleHost::factoryThatWaits' state, where a call that was held behind a factory goes on.
JSPromise* Modules::startImport(ModuleID module, std::optional<int64_t> referrerAsyncOrder, ImportCaller caller, TextOfImport text, JSPromise* promiseOfCaller, JSValue stateOfWait, std::optional<bool> answerOfWalkAlreadyMade)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto promiseForCaller = [&] {
        return promiseOfCaller ? promiseOfCaller : JSPromise::create(*m_instance.vm, realm->promiseStructure());
    };

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
        call.append(jsNumber(moduleIDForHost(*m_instance.program, module).index()));
        call.append(referrerAsyncOrder ? jsNumber(static_cast<double>(*referrerAsyncOrder)) : jsUndefined());
        call.append(jsBoolean(caller == ImportCaller::Host));
        call.append(jsNumber(static_cast<uint32_t>(text)));
        call.append(stateOfWait);
        JSArray* heldCall = constructArray(realm, static_cast<ArrayAllocationProfile*>(nullptr), call);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        factories->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), promise, heldCall);
        return promise;
    }
    // `bun build --compile` reads no text: what calls of one turn ask for runs in the order of the calls. What a file asks for while a graph is loaded still runs behind that graph.
    bool hasTextAtHand = !m_innermostFilesBeingRun && !m_instance.vm->aotState().numberOfTextsThatWait && host().holdsTextsOfProgram && host().holdsTextsOfProgram(realm);
    if (text == TextOfImport::MayComeLater && !hasTextAtHand && textOfImportComesInLaterTurn(module, AskedBy::Import)) {
        JSPromise* promise = JSPromise::create(*m_instance.vm, realm->promiseStructure());
        if (caller == ImportCaller::Host)
            promise->markAsHandled();
        holdText(promise, module, nullptr, referrerAsyncOrder, AskedBy::Import, caller);
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
        if (isCalledByProgram && (!isBuiltinModule || isBuiltinModuleThatLoaderHas) && loaderOnlyHandsOverNamespace(module)) {
            JSPromise* promise = promiseForCaller();
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
            JSPromise* promise = promiseForCaller();
            if (promise->isThenFastAndNonObservable()) [[likely]] {
                uint32_t serial = m_serialOfNextCountedImport++;
                m_countedImports.append(CountedImport { Strong<JSPromise>(vm, promise), module, referrerAsyncOrder, serial });
                ++m_numberOfCountedImports;
                if (!m_orderOfCountedJobs.isEmpty()) {
                    m_orderOfCountedJobs.append(serial);
                    m_modulesOfCountedImports.set(module.index());
                }
                if (text == TextOfImport::HasComeWithCallInFront) {
                    CountedImport import = m_countedImports.takeLast();
                    import.runsInJobOfCallInFront = true;
                    scope.release();
                    decideHowCountedImportGoesOn(import);
                    return promise;
                }
                queueImportJob(realm, promise, module, referrerAsyncOrder);
                return promise;
            }
        }
        handCountedImportsToLoader();
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (asyncLoadMustWaitFor(module) && !loader()->registryEntry(moduleKey(module))) [[unlikely]] {
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
    m_modulesOfQueuedImports.set(module.index());
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

bool Modules::isAnythingBeingLoaded() const
{
    if (m_instance.vm->synchronousModuleQueue() || m_rootWhoseFilesRun || m_rootWhoseFilesRanAhead || m_fileWhoseJobsWaitForLoader || !m_levelsOfLoads.isEmpty() || m_instance.vm->aotState().numberOfTextsThatWait)
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
    if (statementThatEndsLoad(root)) [[unlikely]]
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
    if (import.jobsOfLoader || isLeftToHost(module))
        return goOnWithLoader();

    // Where no job waits none will run before the graph has run: nobody can tell in how many jobs it is loaded, unless the host does something of its own behind a job.
    bool isWatched = !import.runsInJobOfCallInFront && (!queue.isEmpty() || vm.hostObservesMicrotaskBoundaries());
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
        break;
    case JSPromise::Status::Rejected:
        evaluation->markAsHandled();
        queueStepOfImport(importPromise, moduleForJob(module), numberOfJobsBehindEvaluation, evaluation->result());
        break;
    case JSPromise::Status::Pending:
        evaluation->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledModuleImportSettled, importPromise, moduleForJob(module));
        break;
    }
    RELEASE_AND_RETURN(scope, deliverCallsThatComeWith(importPromise));
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
    std::unique_ptr<ModuleLinkWalk> walk = takeKeptLinkWalk(root);
    if (!walk) {
        countLinkWalkMade(*m_instance.vm);
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
    AOT_THRESHOLD(size_t, maxWalksKept, 8);
    if (m_linkWalksOfLoads.size() == maxWalksKept)
        m_linkWalksOfLoads.removeAt(0);
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

    if (m_importThatWaitsForQueuedJobs == importPromise) {
        if (!vm.defaultMicrotaskQueue().isEmpty()) {
            queueImportJob(m_instance.globalObject, importPromise, module, referrerAsyncOrder);
            return;
        }
        m_importThatWaitsForQueuedJobs = nullptr;
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
    handCountedImportsToLoader();
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    VM& vm = *m_instance.vm;
    auto askLoader = [&] {
        SetForScope frameOfImportThatAsksLoader(vm.aotState().frameOfImportThatAsksLoader, vm.topCallFrame);
        return loader()->requestImportModuleForKey(realm, key, moduleKey(referrer), WTF::move(fetchParameters), nullptr, false, asyncEvaluationOrder(referrer).value_or(-1));
    };
    for (Instance* instance = fetchParameters ? nullptr : realm->aotInstance(); instance; instance = instance->above) {
        Modules* modules = instance->state->modules.get();
        if (!modules || modules->m_modulesWithLoadEndedByMock.isEmpty()) [[likely]]
            continue;
        if (std::optional<ModuleID> found = modules->m_table.findByPath(key.string()); found && modules->errorOfLoadEndedByMock(*found))
            RELEASE_AND_RETURN(scope, modules->rejectImportWithErrorOfLoad(*found, ImportCaller::CompiledCode));
    }
    // A fetch of the loader waits for the factories that it starts alone: nothing says there who called. Here it shows, so the call waits as one for a file of its own image does in startImport().
    for (Instance* instance = fetchParameters || !vm.aotState().hasSeveralImages ? nullptr : realm->aotInstance(); instance; instance = instance->above) {
        Modules* modules = instance->state->modules.get();
        std::optional<ModuleID> found = modules ? modules->m_table.findByPath(key.string()) : std::nullopt;
        if (!found)
            continue;
        JSPromise* factories = modules->factoryThatWaitsBelow(*found, FactoryAskedForBy::ImportCall, stateOfWait);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (!factories)
            break;
        JSPromise* promise = JSPromise::create(vm, realm->promiseStructure());
        JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), importFromOtherImageBehindFactories, ImplementationVisibility::Private);
        MarkedArgumentBuffer call;
        call.append(jsString(vm, key.string()));
        call.append(jsNumber(moduleIDForHost(*m_instance.program, referrer).index()));
        call.append(stateOfWait);
        JSArray* heldCall = constructArray(realm, static_cast<ArrayAllocationProfile*>(nullptr), call);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        factories->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), promise, heldCall);
        return promise;
    }
    SetForScope askerLoadsCompiledFilesAlone(vm.aotState().askerLoadsCompiledFilesAlone, loadsCompiledFilesAlone(referrer));
    JSPromise* loaded = askLoader();
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    tellHostOfImport(realm, loaded, key.string());
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

const Identifier* Modules::resolvedKeyIfKnown(ModuleID external) const
{
    const Vector<Identifier>& keysOfFilesBelow = m_instance.vm->aotState().parts[m_instance.program->position()].keysOfFilesInImagesBelow;
    if (external.index() < keysOfFilesBelow.size() && !keysOfFilesBelow[external.index()].isNull())
        return &keysOfFilesBelow[external.index()];
    const LoadedModule* loaded = loadedIfExists(external);
    return loaded && !loaded->resolvedKey.isNull() ? &loaded->resolvedKey : nullptr;
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
    if ((status(module) == ModuleStatus::Unlinked || data(module).capturesExports()) && !m_isWalkingRequests && !m_levelsOfLoads.contains(module) && !isOnWayBelowRootOfItsImage(module)) {
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
    if (data(root).isLoadedByLoader() || m_fileWhoseJobsWaitForLoader || m_rootWhoseFilesRanAhead == root || m_rootKnownToBeWatched == root || vm.aotState().numberOfTextsThatWait)
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
    if (!m_levelsOfRealm.isEmpty()) [[unlikely]] {
        if (std::unique_ptr<LevelOfRealm> level = m_levelsOfRealm.take(module)) {
            scope.release();
            appendRequestsOfUncompiledCode(module, record, level->requests, level.get());
            return;
        }
    }
    if (!record->isRequestedByWalkedModule()) {
        bool mustWaitForFile = importsFileBeingEvaluated(module);
        bool endsAtStatement = statementThatEndsLoad(module).has_value();
        bool isWalked = mustWaitForFile || endsAtStatement || canTellHowGraphIsLoaded(module);
        FilesLeftToLoader leftToLoader;
        if (!isWalked) {
            // Nobody sees in which step a file runs, until one of them queues a job or throws.
            evaluateImportedCommonJSFiles(module, JobsOfImportedFiles::WaitForLoader, ErrorOfImportedFile::IsLeftToLoader, &load, &leftToLoader);
            RETURN_IF_EXCEPTION(scope, void());
            // One of the files had the loader go on from here.
            if (!record->requestedModules().isEmpty())
                return;
            isWalked = m_rootWhoseFilesRanAhead == module;
        }
        if (!isWalked) {
            LevelOfRealm levelOfRoot;
            bool hasWay = layWayThroughRealm(module, leftToLoader, levelOfRoot);
            scope.release();
            appendRequestsOfUncompiledCode(module, record, leftToLoader.requests, hasWay ? &levelOfRoot : nullptr);
            return;
        }
        // The host has asked for the graph, and no import(). A job that runs between the steps of the load may require a part of it. A root whose files ran ahead counts as it is.
        if (!m_instance.vm->synchronousModuleQueue() && m_rootWhoseFilesRanAhead != module && !m_loadsInFlight.containsIf([&](const LoadInFlight& known) { return known.root == module; }))
            didStartLoad(load.promise(), module);
        for (LoadInFlight& load : m_loadsInFlight)
            load.isWalked |= load.root == module;
        if (!data(module).isLoadedByLoader() && m_rootWhoseFilesRanAhead != module && !mustWaitForFile && !endsAtStatement && planLevelsOfLoad(module)) {
            m_levelsOfLoads.get(module)->plan->load.set(*m_instance.vm, &load);
            appendNextOnWay();
            return;
        }
    }

    count(m_instance, RuntimeCounterID::CompiledModulesWalkedByLoader);
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

ModuleRecord* Modules::appendRequestsOfUncompiledCode(ModuleID module, ModuleRecord* record, const ModulesOfRealm& inFront, const LevelOfRealm* level)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!level && !data(module).reachesUncompiledCode())
        return record;
    RequestsOfUncompiledCode requests;
    for (const ModuleOfRealm& each : inFront) {
        Modules* modules = each.modules;
        ModuleID requested = each.module;
        if (modules->isCommonJSFileOrFace(requested) && !modules->isFileLeftToRun(requested))
            continue;
        Identifier key = modules == this ? keyOfRequest(requested) : modules->moduleKey(requested);
        if (!requests.seen.add(placeInLoader(key, nullptr)).isNewEntry)
            continue;
        if (modules == this && isCommonJSFileOrFace(requested)) {
            noteLoadOfFile(requested, loader()->registryEntry(key) ? LoadOfFile::HasComeToFile : LoadOfFile::IsRequested);
            RETURN_IF_EXCEPTION(scope, nullptr);
            noteRequestByLoader(requested);
        }
        record->appendRequestedModule(key, nullptr, ModulePhase::Evaluation);
        if (level && modules != level->modulesOfRoot)
            level->made->modules.append(each);
        if (modules == this)
            record->didRequestCompiledModule(key, requested);
        else if (modules->data(requested).isFaceOfFile())
            record->didRequestCompiledModule(key, moduleIDForHost(*modules->m_instance.program, requested));
    }
    if (level) {
        if (!level->importersOfUncompiledCode.isEmpty()) {
            for (const ModuleOfRealm& made : level->made->modules)
                requests.seen.add(placeInLoader(made.modules->moduleKey(made.module), nullptr));
        }
        for (const ModuleOfRealm& importer : level->importersOfUncompiledCode) {
            importer.modules->collectRequestsOfUncompiledCode(importer.module, requests);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
    } else {
        collectRequestsOfUncompiledCode(module, requests);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
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
    // A module of another image that has run needs no record, and a record that is retired leaves the loader: retireRecord().
    if (!record && m_instance.program->sharesRealms()) [[unlikely]] {
        Instance::forEachOf(m_instance.globalObject, [&](Instance& instance) {
            Modules& modules = modulesOf(instance);
            if (record || &modules == this)
                return;
            if (std::optional<ModuleID> compiled = modules.m_table.findByPath(key.string()); compiled && !modules.isEvicted(*compiled))
                record = modules.recordForResolution(modules.moduleWithExportsOf(modules.fileOf(*compiled)));
        });
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
    if (auto* ofImage = dynamicDowncast<ModuleRecord>(record))
        RELEASE_AND_RETURN(scope, use == NamespaceUse::CarriesBindings ? ofImage->moduleNamespaceObjectForImageAbove(phase) : record->moduleNamespaceObject(m_instance.globalObject, phase));
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

// Undefined: no import has loaded the file, and require() reads it in its own way.
JSC_DEFINE_JIT_OPERATION(operationAOTRequireIfHeldForImport, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireIfHeldForImport);
    Modules& modules = modulesOf(*instance);
    if (!modules.isHeldForImport(ModuleID(module)))
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    OPERATION_RETURN(scope, JSValue::encode(modules.require(ModuleID(module), ModuleID(referrer))));
}

static NEVER_INLINE RuntimeCounters::CountedFile* findCountedFile(Instance& instance, uint32_t module, StringID description)
{
    RuntimeCounters& counters = *instance.vm->aotState().counters;
    unsigned position = instance.program->position();
    while (counters.countedFilesOfImages.size() <= position)
        counters.countedFilesOfImages.append({ });
    Vector<RuntimeCounters::CountedFile*>& files = counters.countedFilesOfImages[position];
    while (files.size() <= module)
        files.append(nullptr);
    files[module] = counters.countedFiles.ensure(ProgramStrings::atomOf(*instance.program, description).string(), [] {
        return makeUnique<RuntimeCounters::CountedFile>();
    }).iterator->value.get();
    return files[module];
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAreLoaderFunctionsStored, size_t, (Instance* instance))
{
    return modulesOf(*instance).areLoaderFunctionsStored();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSeesMockOf, size_t, (Instance* instance, uint32_t module, uint32_t referrer))
{
    return modulesOf(*instance).seesMockOf(ModuleID(module), ModuleID(referrer));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWasLoadedSynchronously, size_t, (Instance* instance, uint32_t module))
{
    return modulesOf(*instance).wasLoadedSynchronously(ModuleID(module));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCountExecution, void, (Instance* instance, uint32_t module, uint32_t description, uint32_t counter))
{
    const auto& images = instance->vm->aotState().counters->countedFilesOfImages;
    unsigned position = instance->program->position();
    RuntimeCounters::CountedFile* file = position < images.size() && module < images[position].size() ? images[position][module] : nullptr;
    if (!file) [[unlikely]]
        file = findCountedFile(*instance, module, StringID(description));
    while (file->size() <= counter)
        file->append(0);
    ++file->at(counter);
}

void forEachCountedFile(VM& vm, const ScopedLambda<void(const String& description, std::span<const uint64_t> counts)>& each)
{
    RuntimeCounters* counters = vm.aotState().counters.get();
    if (!counters)
        return;
    for (auto& [description, file] : counters->countedFiles)
        each(description, file->span());
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

// Where a realm has several images, a number that the host holds names the image too: it counts on from the modules of the images in front, Image::modulesBelow(), and a bit says that it does.
// A number without the bit is of the image that runs. With the bit it is still an int32 that is not negative: a job holds it as one.
static constexpr uint32_t countsThroughImages = 1u << 30;

static bool namesImage(ModuleID fromHost)
{
    return fromHost && fromHost.index() & countsThroughImages;
}

ModuleID moduleIDForHost(const Image& image, ModuleID module)
{
    if (!image.sharesRealms()) [[likely]]
        return module;
    uint32_t number = WTF::checkedSum<uint32_t>(image.modulesBelow(), module.index());
    RELEASE_ASSERT(number < countsThroughImages);
    return ModuleID(number | countsThroughImages);
}

// Nothing where the host names a module of another image.
std::optional<ModuleID> moduleOfImage(const Image& image, std::optional<ModuleID> fromHost)
{
    if (!fromHost || !namesImage(*fromHost)) [[likely]]
        return fromHost;
    uint32_t index = (fromHost->index() & ~countsThroughImages) - image.modulesBelow();
    if (index >= ModuleTable(image.programTables()).size())
        return std::nullopt;
    return ModuleID(index);
}

// Of a number that names its image. No image: none of the list has so many modules below it and of its own.
static std::pair<const Image*, ModuleID> findImageOfModule(const Image& imageOfList, ModuleID fromHost)
{
    ASSERT(namesImage(fromHost));
    for (const Image* image = &imageOfList.first(); image; image = image->above()) {
        if (std::optional<ModuleID> module = moduleOfImage(*image, fromHost))
            return { image, *module };
    }
    return { nullptr, ModuleID() };
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
        if (!namesImage(m_module)) [[likely]]
            return nullptr;
        const Image* image;
        std::tie(image, m_module) = findImageOfModule(*Image::current(), m_module);
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

// The module of this image that moduleKey() spells so.
std::optional<ModuleID> Modules::findByKey(const Identifier& key) const
{
    StringView text = key.string();
    if (!m_instance.program->sharesRealms() || !text.startsWith('#')) [[likely]]
        return m_table.findByPath(text);
    std::optional<uint32_t> number = parseInteger<uint32_t>(text.substring(1));
    if (!number || !namesImage(ModuleID(*number)))
        return std::nullopt;
    std::optional<ModuleID> module = moduleOfImage(*m_instance.program, ModuleID(*number));
    if (!module || !isNamedByNumber(m_table.pathText(*module), *module))
        return std::nullopt;
    return module;
}

// As Modules::moduleKey() spells it where images share realms.
static std::optional<ModuleID> findModuleNamedByNumber(const Image& running, StringView publicPath)
{
    std::optional<uint32_t> number = parseInteger<uint32_t>(publicPath.substring(1));
    if (!number || !namesImage(ModuleID(*number)))
        return std::nullopt;
    auto [image, module] = findImageOfModule(running, ModuleID(*number));
    if (!image || !isNamedByNumber(ModuleTable(image->programTables()).pathText(module), module))
        return std::nullopt;
    return ModuleID(*number);
}

std::optional<ModuleID> findModule(StringView publicPath, unsigned hashOfPath)
{
    const Image* running = Image::current();
    if (running && running->sharesRealms() && publicPath.startsWith('#'))
        return findModuleNamedByNumber(*running, publicPath);
    std::optional<ModuleID> found;
    forEachImageOfThread([&](const Image& image) {
        if (std::optional<ModuleID> module = findModuleInRunningImage(publicPath, hashOfPath))
            found = moduleIDForHost(image, *module);
        return found ? IterationStatus::Done : IterationStatus::Continue;
    });
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

bool isLeftToCodeOutsideImage(StringView specifier, StringView directoryOfImporter)
{
    const Image* image = Image::current();
    // Where images share realms, a program names the files of the images below it this way.
    if (!image || image->sharesRealms())
        return false;
    ModuleTable table(image->programTables());
    return std::ranges::any_of(table.modulesOfUncompiledCode(), [&](const UncompiledModuleData& entry) {
        ModuleID module(entry.module);
        String name = table.pathText(module);
        if (name.startsWith('.')) {
            std::optional<ModuleID> referrer = table.data(module).referrerModule();
            return specifier == name && referrer && withoutSeparatorAtEnd(directoryOfImporter) == withoutSeparatorAtEnd(directoryOf(table.pathText(*referrer)));
        }
        return specifier == name || (!name.startsWith('/') && specifier.startsWith(name) && specifier[name.length()] == '/');
    });
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

String modulePath(ModuleID moduleOfHost)
{
    ModuleFromHost module(moduleOfHost);
    return pathOfModule(module).path;
}

String pathOfFileOfModule(ModuleID moduleOfHost)
{
    ModuleFromHost module(moduleOfHost);
    return pathOfModule(module).pathOfFile;
}

String sourceProviderURL(ModuleID moduleOfHost)
{
    ModuleFromHost module(moduleOfHost);
    PathOfModule found = pathOfModule(module);
    return found.isEvaluatedText ? emptyString() : String(found.pathOfFile);
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

std::optional<bool> findsBuiltinsReadOnly(ModuleID moduleOfHost)
{
    ModuleFromHost module(moduleOfHost);
    std::optional<ModuleTable> table = currentModuleTable();
    if (!table || table->data(module).isOfLibrary())
        return std::nullopt;
    return std::ranges::binary_search(Image::current()->programTables().modulesThatFindBuiltinsReadOnly(), ModuleID(module).index());
}

bool isReachedUnder(ModuleID moduleOfHost, OtherNameOfModule name)
{
    ModuleFromHost module(moduleOfHost);
    if (!currentModuleTable())
        return false;
    return std::ranges::binary_search(Image::current()->programTables().modulesReachedUnderOtherNames(), moduleReachedUnder(ModuleID(module).index(), name));
}

bool isDataFile(ModuleID moduleOfHost)
{
    ModuleFromHost module(moduleOfHost);
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::Data;
}

bool isHeldForImport(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->isHeldForImport(module);
}

bool isNativeModule(ModuleID moduleOfHost)
{
    ModuleFromHost module(moduleOfHost);
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::Runtime;
}

bool isOfLibrary(ModuleID moduleOfHost)
{
    ModuleFromHost module(moduleOfHost);
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).isOfLibrary();
}

bool mayHoldEntryOfProgramInRequireCache(ModuleID moduleOfHost)
{
    ModuleFromHost module(moduleOfHost);
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

static void evictFromItsImage(JSGlobalObject* realm, ModuleID moduleOfHost, TakenOutBy takenOutBy)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    if (modules->data(module).isOfLibrary()) {
        String path = modules->table().pathText(module);
        bool programsAboveAskHost = realm->vm().aotState().hasSeveralImages;
        if (takenOutBy == TakenOutBy::EntryOfProgram && modules->data(module).isHeldUnderNameWithoutPrefix() && !path.startsWith("node:"_s) && !programsAboveAskHost)
            return throwLoaderChangeRefused(realm, LoaderChange::StoreUnderNameOfModuleInRequireCache, path);
        return modules->takeOutOfRequireCache(module);
    }
    modules->forgetObjectMadeByRequire(module);
    bool isDeleted = takenOutBy == TakenOutBy::Deletion || takenOutBy == TakenOutBy::Reset;
    if (isDeleted) {
        modules->forgetFirstLoad(module);
        modules->dropErrorOfLoadEndedByMock(module);
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

// A store leaves the loader as it is, and the host's require() looks there behind a falsy entry: it finds a module of the library, and a file that an import has loaded.
static bool isFoundBehindFalsyEntry(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && (modules->data(module).isOfLibrary() || modules->wasLoadedByImport(module));
}

// A file of another image may import the file, and goes to the host with it: isLeftToHost().
static void tellImagesOfEviction(JSGlobalObject* realm)
{
    const Image* running = Image::current();
    if (!running || !running->sharesRealms()) [[likely]]
        return;
    for (Instance* instance = realm->aotInstance(); instance; instance = instance->above) {
        if (Modules* modules = instance->state->modules.get())
            modules->didEvictFromOtherImage();
    }
}

// The cache knows a file by its path. Several images may hold the file, the image of a test file and one that it shares with others, and the copy that has run may be in any of them.
static void evictFromEveryImage(JSGlobalObject* realm, ModuleID moduleOfHost, TakenOutBy takenOutBy)
{
    const Image* running = Image::current();
    if (!running || !running->sharesRealms()) [[likely]] {
        if (takenOutBy == TakenOutBy::FalsyStore && isFoundBehindFalsyEntry(realm, moduleOfHost))
            return;
        return evictFromItsImage(realm, moduleOfHost, takenOutBy);
    }
    String path;
    {
        ModuleFromHost module(moduleOfHost);
        path = currentModuleTable()->pathText(module);
    }
    Vector<ModuleID, 4> copies;
    forEachImageOfThread([&](const Image& image) {
        if (std::optional<ModuleID> copy = findModuleInRunningImage(path, path.hash()))
            copies.append(moduleIDForHost(image, *copy));
        return IterationStatus::Continue;
    });
    if (takenOutBy == TakenOutBy::FalsyStore && std::ranges::any_of(copies, [&](ModuleID copy) { return isFoundBehindFalsyEntry(realm, copy); }))
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    for (ModuleID copy : copies) {
        evictFromItsImage(realm, copy, takenOutBy);
        RETURN_IF_EXCEPTION(scope, );
    }
    tellImagesOfEviction(realm);
}

void willRunEntryPointAgain(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    if (Modules* modules = modulesOf(realm))
        modules->forgetEvaluation(module);
}

void didDeleteRequireCacheEntry(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    evictFromEveryImage(realm, moduleOfHost, TakenOutBy::Deletion);
}

void didResetModule(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    evictFromEveryImage(realm, moduleOfHost, TakenOutBy::Reset);
}

bool isEvictedFile(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->isEvicted(module);
}

void willRunCodeThatKeepsFiles(JSGlobalObject* realm)
{
    if (!realm->aotInstance())
        return;
    Instance::forEachOf(realm, [&](Instance&) {
        if (Modules* modules = modulesOf(realm))
            modules->willRunCodeThatKeepsFiles();
    });
}

void keepFilesThatHaveRun(JSGlobalObject* realm)
{
    if (!realm->aotInstance())
        return;
    Instance::forEachOf(realm, [&](Instance&) {
        if (Modules* modules = modulesOf(realm))
            modules->keepFilesThatHaveRun();
    });
}

void bringBackKeptFiles(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    if (!realm->aotInstance())
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    Instance::forEachOf(realm, [&](Instance&) {
        if (scope.exception()) [[unlikely]]
            return;
        if (Modules* modules = modulesOf(realm))
            modules->bringBackKeptFiles(functor);
    });
    RETURN_IF_EXCEPTION(scope, void());
}

bool refusesToLoadAgain(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    auto refuses = [&](ModuleID copy) {
        ModuleFromHost module(realm, copy);
        Modules* modules = modulesOf(realm);
        return modules && !modules->data(module).isOfLibrary() && !modules->mayBeEvicted(module) && modules->wouldBeEvicted(module);
    };
    const Image* running = Image::current();
    if (!running || !running->sharesRealms()) [[likely]]
        return refuses(moduleOfHost);
    String path;
    {
        ModuleFromHost module(moduleOfHost);
        path = currentModuleTable()->pathText(module);
    }
    Vector<ModuleID, 4> copies;
    forEachImageOfThread([&](const Image& image) {
        if (std::optional<ModuleID> copy = findModuleInRunningImage(path, path.hash()))
            copies.append(moduleIDForHost(image, *copy));
        return IterationStatus::Continue;
    });
    return std::ranges::any_of(copies, refuses);
}

void willClearRequireCacheEntry(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    evictFromEveryImage(realm, moduleOfHost, TakenOutBy::FalsyStore);
}

void willReplaceRequireCacheEntry(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    evictFromEveryImage(realm, moduleOfHost, TakenOutBy::EntryOfProgram);
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
    tellImagesOfEviction(realm);
    return true;
}

// A module with nothing to run keeps no state, and neither does one that compiled code runs as a part of the only module that imports it.
// What a module imports has run before the module has.
bool Modules::hasRun(ModuleID module)
{
    bool importsAreFollowed = false;
    return hasRun(module, importsAreFollowed);
}

bool Modules::hasRun(ModuleID module, bool& importsAreFollowed)
{
    if (status(module) == ModuleStatus::Evaluated)
        return true;
    if (!std::exchange(importsAreFollowed, true))
        followImportsOfModulesThatHaveRun();
    return m_modulesThatRanWithImporters.get(module.index());
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
        bool hasRun = status(module) == ModuleStatus::Evaluated;
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
    constexpr size_t blockSize = 256;
    for (size_t start = 0; start < numberOfModules; start += blockSize) {
        size_t end = std::min(start + blockSize, numberOfModules);
        if (equalSpans(std::span<const uint8_t>(before.subspan(start, end - start)), now.subspan(start, end - start)))
            continue;
        numberLookedAt += end - start;
        for (size_t index = start; index < end; ++index) {
            if (std::exchange(before[index], now[index]) != now[index])
                lookAt(ModuleID(index));
        }
    }
    if (!numberLookedAt)
        return;
    numberLookedAt += m_records.size();
    for (ModuleID module : m_records.keys())
        lookAt(module);

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
            if (!m_modulesThatRanWithImporters.quickSet(request.target()))
                toFollow.append(request.targetModule());
        }
    }
    countModulesSoughtForWhatHasRun(*m_instance.vm, numberLookedAt);
}

// `bun run` puts a module object into require.cache before the file runs, and takes it out if the file throws: for a CommonJS file whoever loads it, for an ES module a require() alone.
bool Modules::isListedWhileItRuns(ModuleID module) const
{
    if (data(module).kind == ModuleKind::CommonJs)
        return status(module) == ModuleStatus::Evaluating;
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->isKeptByRequire && (loaded->requiredValue || status(module) != ModuleStatus::Failed);
}

bool isEvaluatedFile(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (!modules)
        return false;
    const ModuleData& data = modules->data(module);
    if (data.isOfLibrary())
        return modules->isLibraryModuleInRequireCache(module);
    if (data.isFaceOfFile() || !modules->table().hasFilePath(module) || modules->isEvicted(module))
        return false;
    if (!modules->hasRun(module) && !modules->isListedWhileItRuns(module))
        return false;
    // A file may have a record beside its own, as the one that checks the names which a required graph imports from CommonJS files. It is no file.
    return !isNamedByNumber(modules->table().pathText(module), module);
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

void Modules::forEachFileAwaitingAtTopLevel(const WTF::Function<void(int64_t, const String&)>& functor)
{
    for (ModuleRecord* record : m_records.values()) {
        if (record->status() == CyclicModuleRecord::Status::EvaluatingAsync && record->hasTLA() && !record->pendingAsyncDependencies().value_or(0) && record->asyncEvaluationOrder().hasOrder())
            functor(record->asyncEvaluationOrder().order(), record->moduleKey().string());
    }
}

void forEachFileAwaitingAtTopLevel(JSGlobalObject* realm, const WTF::Function<void(int64_t, const String&)>& functor)
{
    if (!realm->aotInstance())
        return;
    Instance::forEachOf(realm, [&](Instance&) {
        if (Modules* modules = modulesOf(realm))
            modules->forEachFileAwaitingAtTopLevel(functor);
    });
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
        // `bun run` lists a file from when it begins, so a file stands in front of what it asks for. The table has it behind that.
        // It lists the CommonJS files first: they are in the map of `require`, and what the loader of ES modules holds comes behind that.
        BitVector isSeen(table.size());
        Vector<ModuleID, 64> toVisit;
        Vector<ModuleID, 64> inOrder;
        auto listFrom = [&](ModuleID root) {
            toVisit.append(root);
            while (!toVisit.isEmpty()) {
                ModuleID module = toVisit.takeLast();
                if (isSeen.quickSet(module.index()))
                    continue;
                if (!table.data(module).isOfLibrary() && isEvaluatedFile(realm, module))
                    inOrder.append(module);
                auto requests = table.requests(module);
                for (size_t index = requests.size(); index--;)
                    toVisit.append(requests[index].targetModule());
            }
        };
        for (unsigned index = 0; index < table.size(); ++index) {
            if (table.data(ModuleID(index)).isEntryPoint())
                listFrom(ModuleID(index));
        }
        for (unsigned index = 0; index < table.size(); ++index)
            listFrom(ModuleID(index));
        for (bool listsCommonJS : { true, false }) {
            for (ModuleID module : inOrder) {
                if ((table.data(module).kind == ModuleKind::CommonJs) == listsCommonJS)
                    functor(table.pathText(module));
            }
        }
    });
}

void forEachLibraryModuleInRequireCache(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    if (!realm->aotInstance())
        return;
    Instance::forEachOf(realm, [&](Instance&) {
        Modules* modules = modulesOf(realm);
        if (!modules)
            return;
        for (ModuleID module : modules->libraryModulesInRequireCache())
            functor(modules->table().pathText(module));
    });
}

std::optional<ModuleID> findLibraryModuleInRequireCache(JSGlobalObject* realm, const String& name)
{
    std::optional<ModuleID> found;
    if (!realm->aotInstance())
        return found;
    Instance::forEachOf(realm, [&](Instance& instance) {
        Modules* modules = modulesOf(realm);
        if (!modules || found)
            return;
        // Nearly every key is the path of a file.
        std::optional<ModuleID> module = findModuleInRunningImage(name, name.hash());
        if (module && modules->data(*module).isOfLibrary() && modules->isLibraryModuleInRequireCache(*module))
            found = moduleIDForHost(*instance.program, *module);
    });
    return found;
}

bool hostAnswersInPlaceOf(JSGlobalObject* realm, ModuleID moduleOfHost, LoadByName way)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->hostAnswersInPlaceOf(module, way);
}

void willLoadCopyFromDisk(JSGlobalObject* realm, ModuleID moduleOfHost, LoadByName way)
{
    ModuleFromHost module(realm, moduleOfHost);
    if (Modules* modules = modulesOf(realm))
        modules->willLoadCopyFromDisk(module, way);
}

void forEachFirstLoad(VM& vm, const ScopedLambda<void(const String& path, bool wasSynchronous)>& functor)
{
    for (const VMState::PartsOfImage& parts : vm.aotState().parts) {
        if (parts.filesWithKnownFirstLoad.isEmpty())
            continue;
        ModuleTable table(parts.image->programTables());
        for (size_t file : parts.filesWithKnownFirstLoad)
            functor(table.pathText(ModuleID(file)), parts.filesFirstLoadedSynchronously.get(file));
    }
}

void noteFirstLoadInEarlierProcess(VM& vm, const String& path, bool wasSynchronous)
{
    for (VMState::PartsOfImage& parts : vm.aotState().parts) {
        ModuleTable table(parts.image->programTables());
        std::optional<ModuleID> found = table.findByPath(path);
        if (!found)
            continue;
        ModuleID file = table.fileBehindFace(*found).value_or(*found);
        if (!parts.filesWithKnownFirstLoad.set(file.index()))
            parts.filesFirstLoadedSynchronously.set(file.index(), wasSynchronous);
    }
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

bool willRunCopyFromDisk(JSGlobalObject* realm)
{
    if (!realm->aotInstance())
        return true;
    bool isAdmitted = true;
    Instance::forEachOf(realm, [&](Instance&) {
        if (Modules* modules = modulesOf(realm))
            isAdmitted &= !modules->refusesOriginalsBesideMocks();
    });
    if (!isAdmitted)
        return false;
    Instance::forEachOf(realm, [&](Instance&) {
        if (Modules* modules = modulesOf(realm))
            modules->willRunCopyFromDisk();
    });
    return true;
}

bool refusesHostObjectsOutsideImage(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return refusesCodeOutsideImage(realm, { }) && !(modules && modules->isAskingHost());
}

JSObject* mainModuleObject(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    // Asked of the image that runs, not of the one that holds the file.
    if (!modulesThatShowWaysBetweenModuleObjects(realm))
        return nullptr;
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules ? modules->mainModuleObject(module) : nullptr;
}

void willStorePathOfMainModule(JSGlobalObject* realm)
{
    bool isDecidedInCode = false;
    for (Instance* instance = realm->aotInstance(); instance; instance = instance->above) {
        // In a test run the runner itself stores there, for each test file in its turn.
        if (hasFlag(instance->options().admitted(), AdmittedCode::FilesRunByHost) || hasFlag(instance->options().admitted(), AdmittedCode::ReplacedBuiltins))
            return;
        isDecidedInCode |= !instance->program->holdsLibraryAlone() && !instance->program->storesPathOfMainModule();
    }
    if (!isDecidedInCode)
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwTypeError(realm, scope, assignmentThatBuildDidNotSeeMessage("Bun.main"_s));
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
    if (Modules* modules = modulesOf(realm); modules && modules->data(module).isOfLibrary())
        return true;
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules && modules->forEachNewChild(module, functor);
}

void dropNewChildren(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    if (Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm))
        modules->dropNewChildren(module);
}

JSValue moduleExports(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (!modules)
        return jsUndefined();
    modules->noteLoadBehindMock(module, true);
    return modules->commonJSExports(module);
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
    // A realm that holds nothing compiled runs the library from its text: the host answers there.
    if (!modulesOf(realm) && isOfLibrary(moduleOfHost))
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

AbstractModuleRecord* moduleRecordOfNamespace(JSGlobalObject* realm, JSCell* cell)
{
    auto* moduleNamespace = dynamicDowncast<ModuleNamespace>(cell);
    if (!moduleNamespace)
        return nullptr;
    ImageOfCellScope imageOfCell(realm->vm(), cell);
    return moduleNamespace->recordForResolution();
}

AbstractModuleRecord* moduleRecord(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesForEvaluation(realm, module);
    if (!modules)
        return nullptr;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    modules->letGoOfMockThatEnded(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    // An import() of an image above has asked the loader, which asks for the module inside that call. What this runs of the library `bun run` runs from a job, as in askLoaderForImport().
    // A mark further in than the import() is the one that was set here for it.
    VMState& state = realm->vm().aotState();
    bool isFirstAskedForByImport = state.frameOfImportThatAsksLoader && (!state.stackPointerOfImport || state.stackPointerOfImport > static_cast<void*>(state.frameOfImportThatAsksLoader));
    SetForScope stackPointerOfImport(state.stackPointerOfImport, isFirstAskedForByImport ? currentStackPointer() : state.stackPointerOfImport);
    // The loader asks for a module when it loads it. Where it goes on to what the module imports, each file runs when the loader comes to it.
    if (!modules->isAskingHost()) {
        modules->handCountedImportsToLoader();
        RETURN_IF_EXCEPTION(scope, nullptr);
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
    // A module of the image asks through the loader. Code that is not compiled binds what it imports to the record.
    if (!modules->isAskingHost())
        modules->didImportFromUncompiledCode(module, isFirstAskedForByImport);
    RELEASE_AND_RETURN(scope, modules->exposeRecord(module, modules->isAskingHost() ? Modules::RecordHolder::Loader : Modules::RecordHolder::Anyone));
}

bool textOfFetchedModuleComesInLaterTurn(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    if (!realm->vm().aotState().frameOfImportThatAsksLoader) [[likely]]
        return false;
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->textOfImportComesInLaterTurn(module, Modules::AskedBy::Loader);
}

void holdTextOfFetchedModule(JSGlobalObject* realm, ModuleID moduleOfHost, JSPromise* fetched, AbstractModuleRecord* record)
{
    ModuleFromHost module(realm, moduleOfHost);
    modulesOf(realm)->holdTextOfImportFromOtherImage(fetched, record, module);
}

bool isCompiledModuleRecord(AbstractModuleRecord* record)
{
    return record->inherits<ModuleRecord>();
}

JSC_DEFINE_HOST_FUNCTION(importBehindFactories, (JSGlobalObject* realm, CallFrame* callFrame))
{
    JSArray* heldCall = asArray(callFrame->argument(1));
    ModuleFromHost module(realm, ModuleID(heldCall->getIndexQuickly(0).asUInt32()));
    JSValue order = heldCall->getIndexQuickly(1);
    std::optional<int64_t> referrerAsyncOrder = order.isUndefined() ? std::nullopt : std::optional(static_cast<int64_t>(order.asNumber()));
    auto caller = heldCall->getIndexQuickly(2).asBoolean() ? Modules::ImportCaller::Host : Modules::ImportCaller::CompiledCode;
    return JSValue::encode(modulesOf(realm)->startImport(module, referrerAsyncOrder, caller, static_cast<Modules::TextOfImport>(heldCall->getIndexQuickly(3).asUInt32()), nullptr, heldCall->getIndexQuickly(4)));
}

JSC_DEFINE_HOST_FUNCTION(importFromOtherImageBehindFactories, (JSGlobalObject* realm, CallFrame* callFrame))
{
    JSArray* heldCall = asArray(callFrame->argument(1));
    ModuleFromHost referrer(realm, ModuleID(heldCall->getIndexQuickly(1).asUInt32()));
    Identifier key = Identifier::fromString(realm->vm(), asString(heldCall->getIndexQuickly(0))->tryGetValue());
    return JSValue::encode(modulesOf(realm)->importUncompiled(key, referrer, nullptr, heldCall->getIndexQuickly(2)));
}

JSPromise* importModule(JSGlobalObject* realm, ModuleID moduleOfHost, std::optional<int64_t> referrerAsyncOrder)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesForEvaluation(realm, module);
    if (modules && modules->errorOfLoadEndedByMock(module)) [[unlikely]]
        return modules->rejectImportWithErrorOfLoad(module, Modules::ImportCaller::Host);
    if (modules)
        modules->dropErrorOfMockKeptByLoader(module);
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
    modules->noteLoadBehindMock(module, true);
    // The loader asks for code of the program, which may lie in another image or on disk. The library gets its own modules from the registry.
    return modules->require(module, moduleOfImage(*Image::current(), referrer).value_or(ModuleID()), Modules::RequiredBy::Program);
}

bool isInRequireCache(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->isInRequireCache(module);
}

void willRequireModule(JSGlobalObject* realm, ModuleID moduleOfHost, std::optional<ModuleID> referrer, JSObject* moduleObjectOfReferrer)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (modules && !moduleOfImage(*Image::current(), referrer))
        modules->recordRequireFromOtherImage(module, moduleObjectOfReferrer);
}

void didMakeModuleObjectForRequire(JSGlobalObject* realm, ModuleID moduleOfHost, JSObject* moduleObject)
{
    ModuleFromHost module(realm, moduleOfHost);
    if (Modules* modules = modulesOf(realm))
        modules->keepObjectMadeByRequire(module, moduleObject);
}

void didRequireModule(JSGlobalObject* realm, ModuleID moduleOfHost, ModuleID referrerOfHost)
{
    if (!namesImage(moduleOfHost) && !namesImage(referrerOfHost)) [[likely]]
        return;
    if (namesImage(moduleOfHost) && namesImage(referrerOfHost) && findImageOfModule(*Image::current(), moduleOfHost).first == findImageOfModule(*Image::current(), referrerOfHost).first)
        return;
    ModuleFromHost referrer(realm, referrerOfHost);
    if (Modules* modules = modulesOf(realm))
        modules->recordRequireOfOtherImage(referrer, moduleOfHost);
}

std::pair<Modules*, ModuleID> Modules::ownerOfRequiredModule(ModuleID required)
{
    if (!namesImage(required)) [[likely]]
        return { this, required };
    for (Instance* instance = m_instance.globalObject->aotInstance(); instance; instance = instance->above) {
        if (std::optional<ModuleID> module = moduleOfImage(*instance->program, required))
            return { instance->state->modules.get(), *module };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

AbstractModuleRecord* Modules::recordOfRequestedModule(ModuleID requested)
{
    SetForScope isAskingHost(ownerOfRequiredModule(requested).first->m_isAskingHost, true);
    return moduleRecord(m_instance.globalObject, requested);
}

// The host's require() takes no entry that is falsy: willClearRequireCacheEntry().
void willSetRequireCacheEntry(JSGlobalObject* realm, ModuleID moduleOfHost, JSValue entry)
{
    ModuleFromHost module(realm, moduleOfHost);
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

void willSetRequireCacheEntryUnderName(JSGlobalObject* realm, ModuleID moduleOfHost, const String& name)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (modules && modules->data(module).isHeldUnderNameWithoutPrefix())
        throwLoaderChangeRefused(realm, LoaderChange::StoreUnderNameOfModuleInRequireCache, name);
}

// A request that is resolved at run time asks the other two functions, as under plain Bun.
void willChangeRequireOverride(JSGlobalObject* realm, RequireOverride location, bool isOverridden)
{
    for (Instance* instance = realm->aotInstance(); instance; instance = instance->above) {
        if (Modules* modules = instance->state->modules.get())
            modules->noteLoaderFunction(location, isOverridden);
    }
}

void willStoreLoaderOfExtension(JSGlobalObject* realm, StringView extension)
{
    for (Instance* instance = realm->aotInstance(); instance; instance = instance->above) {
        Modules* modules = instance->state->modules.get();
        if (!modules)
            continue;
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

bool setMock(JSGlobalObject* realm, ModuleID moduleOfHost, JSObject* mock, JSValue whatRequireAnswers)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, mock ? "setMock"_s : "setMock, none"_s);
    if (modules && mock && modules->isLeftAloneByMock(module))
        return modules->setMockOverModuleThatHasRun(module, mock, whatRequireAnswers);
    return modules && modules->setMockInPlace(module, mock, whatRequireAnswers);
}

bool willLeaveModuleAlone(JSGlobalObject* realm, ModuleID moduleOfHost, bool onlyIfLoadedInPreload, std::optional<ModuleID> asking)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->willLeaveModuleAlone(module, onlyIfLoadedInPreload, asking);
}

bool isLeftAloneByMock(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->isLeftAloneByMock(module);
}

bool wasLetGoByLoader(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->wasLetGoByLoader(module);
}

void didBeginTestFile(JSGlobalObject* realm)
{
    VMState& state = realm->vm().aotState();
    state.momentOfTestFile = ++state.momentOfLoads;
}

JSObject* mockInPlaceBehind(JSCell* moduleNamespace)
{
    auto* object = dynamicDowncast<ModuleNamespace>(moduleNamespace);
    return object ? object->mockInPlace() : nullptr;
}

bool loadComesToModuleWhoseMockEnded(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->loadComesToModuleWhoseMockEnded(module);
}

bool hasMockInPlace(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->mockInPlace(module) && !modules->hasMockThatEnded(module);
}

void letGoOfMocksThatEnded(JSGlobalObject* realm)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    Instance::forEachOf(realm, [&](Instance& instance) {
        if (scope.exception()) [[unlikely]]
            return;
        if (Modules* modules = instance.state ? instance.state->modules.get() : nullptr)
            modules->letGoOfMocksThatEnded();
    });
    RETURN_IF_EXCEPTION(scope, void());
}

bool hasWordOfMock(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->table().wordOfMock(module);
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
        // No image holds the file as no attribute asks for it. Whoever mocks may be a test file of the last image, and what attributes make of the file lies where its importers do.
        std::optional<ModuleID> found;
        Instance::forEachOf(realm, [&](Instance& instance) {
            Modules* modules = instance.state ? instance.state->modules.get() : nullptr;
            if (found || !modules)
                return;
            if (std::optional<ModuleID> form = modules->formThatMockFinds(path, std::nullopt))
                found = moduleIDForHost(*instance.program, *form);
        });
        return found;
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

void runBodyOfRequiredFileAheadOfMock(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    MockDoor door(modules, module, "runBodyOfRequiredFileAheadOfMock"_s);
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    modules->forgetMockThatWaits(module);
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    modules->require(module, ModuleID(), Modules::RequiredBy::Program);
}

bool runBodyOfLinkedModuleAheadOfMock(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "the mock call"_s);
    return modules && modules->runBodyOfLinkedModuleAheadOfMock(module);
}

void deferMock(JSGlobalObject* realm, ModuleID moduleOfHost, AfterFailure afterFailure)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, afterFailure == AfterFailure::AsksAgain ? "deferMock, asks again"_s : "deferMock"_s);
    if (modules)
        modules->deferMock(module, afterFailure);
}

JSValue exportsBeforeMock(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "exportsBeforeMock"_s);
    return modules ? modules->exportsBeforeMock(module) : JSValue();
}

void didEndMock(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    MockDoor door(modules, module, "didEndMock"_s);
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    bool wasLoadedAsMock = modules->mockInPlace(module) && !modules->hasMockThatEnded(module);
    if (wasLoadedAsMock || modules->hasMockLaidOver(module))
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
        modules->oweLibraryModuleToImagesAbove(module);
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
    if (isLibraryModule) {
        modules->oweLibraryModuleToImagesAbove(module);
        RELEASE_AND_RETURN(scope, modules->endMockOfLibraryModule(module));
    }
    if (!stoodInPlace)
        RELEASE_AND_RETURN(scope, modules->endMockLaidOverFile(module));
}

void didChangePlugins(JSGlobalObject* realm)
{
    for (Instance* instance = realm->aotInstance(); instance; instance = instance->above) {
        if (Modules* modules = instance->state->modules.get())
            modules->forgetWhatNamesResolvedTo();
    }
}

JSPromise* factoryThatWaitsBelow(JSGlobalObject* realm, ModuleID moduleOfHost, JSValue& state)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules ? modules->factoryThatWaitsBelow(module, FactoryAskedForBy::Loader, state) : nullptr;
}

bool runMocksBelow(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->runMocksBelowModuleThatLoaderFetches(module);
}

bool mockFailedBehindImportStatement(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    return modules && modules->mockFailedBehindImportStatement(module);
}

void willMockModule(JSGlobalObject* realm, ModuleID moduleOfHost, JSObject* exports)
{
    ModuleFromHost module(realm, moduleOfHost);
    // Bun finds the module in the image, whatever the realm.
    Modules* found = modulesOf(realm);
    if (!found)
        return;
    MockDoor door(found, module, "willMockModule"_s);
    Modules& modules = *found;
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

void didMockModule(JSGlobalObject* realm, ModuleID moduleOfHost)
{
    ModuleFromHost module(realm, moduleOfHost);
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    MockDoor door(modules, module, "didMockModule"_s);
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    // letGoOfRecordThatAnswersByMock(): the record stands for the mock alone, for whoever holds it, and what the table does for the module does not reach it. A mock has nothing to run.
    if (hasLiveExports() && modules().recordIfExists(m_module) != this) [[unlikely]] {
        if (status() != Status::Evaluated) {
            setStatus(Status::Evaluated);
            setCycleRoot(globalObject->vm(), this);
        }
        return index;
    }
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

BitVector ModuleRecord::letAllImagesAskHost(JSGlobalObject* globalObject)
{
    BitVector wereAsking;
    size_t image = 0;
    Instance::forEachOf(globalObject, [&](Instance& instance) {
        if (modulesOf(instance).setIsAskingHost(true))
            wereAsking.set(image);
        ++image;
    });
    return wereAsking;
}

// Each image goes back to what it was: one that asks ends that itself, and nothing would end it for the others. An image that has joined the realm meanwhile stands above them.
void ModuleRecord::restoreImagesThatAskHost(JSGlobalObject* globalObject, const BitVector& wereAsking)
{
    size_t image = 0;
    Instance::forEachOf(globalObject, [&](Instance& instance) {
        modulesOf(instance).setIsAskingHost(wereAsking.get(image++));
    });
}

uint8_t ModuleRecord::willAskHostForRequest()
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    // A module that the loader walks asks it for compiled modules. One that compiled code loads asks only for what the loader links, and a compiled file among that
    // is the root of a graph of its own.
    bool wasWalkingRequests = modules().setIsWalkingRequests(!m_compiledRequests.isEmpty());
    bool wasAskingHost = modules().setIsAskingHost(true);
    return static_cast<uint8_t>(wasAskingHost) | static_cast<uint8_t>(wasWalkingRequests) << 1;
}

void ModuleRecord::didAskHostForRequest(uint8_t flagsBefore)
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    modules().setIsAskingHost(flagsBefore & 1);
    modules().setIsWalkingRequests(flagsBefore & 2);
}

void ModuleRecord::didFailToFetchRequest(const Identifier& key, Exception* exception)
{
    if (m_instance.vm->isTerminationException(exception))
        return;
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    modules().didFailToFetchRequest(m_module, key, exception->value());
}

AbstractModuleRecord* ModuleRecord::recordOfRequest(JSGlobalObject*, const Identifier& key)
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
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
    if (!m_instance.vm->aotState().numberOfTextsThatWait) [[likely]]
        return false;
    auto* compiled = dynamicDowncast<ModuleRecord>(requested);
    if (!compiled)
        return false;
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), compiled->m_instance.program);
    return compiled->modules().holdTextOfRequest(fetched, compiled, compiled->m_module);
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
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
            if (!request.isDynamicStarExport || !modules.isMockedByFactoryThatTakesOriginal(request.targetModule()) || !modules.table().findExport(request.targetModule(), exportName.impl()))
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
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
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (JSObject* mock = modules().mockInPlace(m_module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, mock->get(globalObject, exportName));
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

JSObject* ModuleRecord::moduleNamespaceObjectForImageAbove(ModulePhase phase)
{
    RunningImageScope imageOfRecord(m_instance.vm->aotState(), m_instance.program);
    return modules().moduleNamespaceForImageAbove(m_module, phase);
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
    , m_imagePosition(vm.aotState().image->position())
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
    if (m_keepsToModule) [[unlikely]]
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

// The caches file what they know of an object with a Structure of its own under that Structure: classifyNamespaceRead(). Such an object never shares one again.
void ModuleNamespace::becomeExtensible(VM& vm, Structure* sharedByExtensibleObjects)
{
    setStructure(vm, m_hasOwnStructure ? createStructure(vm, realm(), structure()->storedPrototype(), false) : sharedByExtensibleObjects);
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
    ImageOfCellScope imageOfCell(globalObject->vm(), this);
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Modules& modules = this->modules();
    if (modules.mockInPlace(m_module) || m_mockOfView) [[unlikely]]
        return false;
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

AbstractModuleRecord* ModuleNamespace::recordForResolution()
{
    // The record that the module has now answers by the mock.
    if (m_keepsToModule) [[unlikely]]
        return nullptr;
    return modules().recordForResolution(m_module);
}

JSObject* ModuleNamespace::mockInPlace() const
{
    if (JSObject* ofView = m_mockOfView.get()) [[unlikely]]
        return ofView;
    return m_keepsToModule ? nullptr : modules().mockInPlace(m_module);
}

bool ModuleNamespace::isAnsweredByMock() const
{
    // The caches go by the state of the module, which has a mock while this object keeps to the module.
    return m_keepsToModule || !!mockInPlace();
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
    // The caches of every image file what they know of this object under its Structure: classifyNamespaceRead(). It never has that one again.
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

bool ModuleNamespace::readDeferredExport(JSGlobalObject* globalObject, const Identifier& name)
{
    if (modules().data(m_module).exportRule != SyntheticExportRule::RuntimeModule || !isDynamicExport(name.impl()))
        return false;
    ImageOfCellScope imageOfCell(globalObject->vm(), this);
    readDynamicExport(globalObject, name);
    return true;
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
        if (index && thisObject->m_keepsToModule) [[unlikely]]
            value = thisObject->modules().readExportOfModuleItself(thisObject->m_module, *index);
        else
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
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
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
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
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
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
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
