/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleHost.h"
#include "AOTModuleRecord.h"
#include "AOTModuleTable.h"
#include "AOTModuleWords.h"
#include "JSPromise.h"
#include "MicrotaskQueue.h"
#include "ModuleMap.h"
#include "RememberedSlot.h"
#include "ScriptFetcher.h"
#include "SourceOrigin.h"
#include "Strong.h"
#include <array>
#include <wtf/BitVector.h>
#include <wtf/Box.h>
#include <wtf/Deque.h>
#include <wtf/HashCountedSet.h>
#include <wtf/HashMap.h>
#include <wtf/RefCounted.h>
#include <wtf/RefPtr.h>
#include <wtf/SentinelLinkedList.h>
#include <wtf/SetForScope.h>
#include <wtf/StackPointer.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class JSCellButterfly;
class JSModuleLoader;
class ModuleLoadingContext;
class ModuleRegistryEntry;
class ScriptFetchParameters;
class ThrowScope;

namespace AOT {

class Environment;
class ModuleNamespace;
class ModuleLinkWalk;
struct LinkWalks;
struct EvaluationInProgress;
struct Instance;
struct State;

enum class ModuleStatus : uint8_t {
    Unlinked = static_cast<uint8_t>(CyclicModuleRecord::Status::Unlinked),
    Linking,
    Linked,
    Evaluating,
    EvaluatingAsync,
    Evaluated,
    Failed,
};
static_assert(static_cast<uint8_t>(ModuleStatus::Evaluated) == static_cast<uint8_t>(CyclicModuleRecord::Status::Evaluated));

enum class SynchronousEvaluationReason : uint8_t { Require, DeferredNamespace };
enum class EagerLoadJobPolicy : uint8_t { KeepQueued, DeferJobs, DeferLoaderSteps };
enum class ErrorOfImportedFile : uint8_t { IsThrown, IsLeftToLoader };
enum class PendingFactoryModules : uint8_t { Evaluate, Skip };

struct LinkContext {
    ModuleLinkingStack& stack;
    unsigned index { 0 };
    RefPtr<ScriptFetcher> scriptFetcher { };
};

#define FOR_EACH_REFERENCE_OF_LOADED_MODULE(macro) \
    macro(JSObject*, namespaceObject) \
    macro(JSObject*, deferredNamespaceObject) \
    macro(JSObject*, importMeta) \
    macro(JSObject*, requirer) \
    macro(JSObject*, moduleObjectBoundToExports) \
    macro(JSValue, exportsWhenVariableAndModuleObjectAgreed) \
    macro(Structure*, structureOfModuleObjectLastLookedAt) \
    macro(JSObject*, moduleObjectFromRequire) \
    macro(JSObject*, standInForModuleObject) \
    macro(JSObject*, failedModuleObject) \
    macro(JSObject*, displacedModuleObject) \
    macro(JSValue, exportsGivenToObjectMadeByRequire) \
    macro(JSObject*, firstUncompiledRequirer) \
    macro(JSValue, requireFunction) \
    macro(JSValue, requiredValue) \
    macro(JSValue, valueInRequireCache) \
    macro(JSValue, valueInRequireCacheBeforeMock) \
    macro(JSValue, cachedMockError) \
    macro(JSValue, errorOfModuleThatRanBesideMock) \
    macro(JSObject*, exportsOfMock) \
    macro(JSObject*, exportsBeforeMock) \
    macro(JSObject*, variablesBeforeMock) \
    macro(JSValue, valueBeforeMock) \
    macro(JSValue, retainedOriginalExports) \
    macro(JSValue, stashedActualKeyExports) \
    macro(JSObject*, retainedOriginalModuleObject) \
    macro(JSObject*, activeMock) \
    macro(JSObject*, recordBeforeMockInPlace) \
    macro(JSObject*, namespaceBeforeMockInPlace) \
    macro(JSObject*, namespaceToRestore) \
    macro(JSObject*, capturedExportsBeforeMockInPlace) \
    macro(JSValue, requiredValueBeforeMockInPlace) \
    macro(JSValue, requiredValueBeforeRefusal) \
    macro(JSValue, answerOfMockWhileOriginalRuns) \
    macro(JSValue, exportsOfOriginalBesideMock) \
    macro(JSValue, exportsFromOwnFactory) \
    macro(JSObject*, exportsAddedByMock) \
    macro(JSObject*, endedMocks) \
    macro(JSObject*, namespacesOfEndedMocks) \
    macro(JSObject*, preloadMock) \
    macro(JSObject*, mockWord) \
    macro(JSObject*, loadWord) \
    macro(JSObject*, importedPatch) \
    macro(JSObject*, patchWord) \
    macro(JSObject*, pendingMockWord) \
    macro(JSObject*, reloadedWord) \
    macro(JSObject*, preloadMockWord) \
    macro(JSObject*, earlierPreloadMockState) \
    macro(JSValue, preloadInstance) \
    macro(AbstractModuleRecord*, releasedFailedRecord) \
    macro(JSValue, instanceBesideMock) \
    macro(JSValue, preloadMockRequireValue) \
    macro(JSValue, handedOnInPlaceOfNamespace) \
    macro(JSObject*, handedOnByMocks) \
    macro(JSValue, cachedLoadError) \
    macro(JSValue, errorOfImportThatFailedUnderMock) \
    macro(JSObject*, exportsWhileRequired) \
    macro(JSObject*, capturedExports) \
    macro(JSObject*, deferredExportsSource) \
    macro(JSCellButterfly*, capturedFields) \
    macro(JSCellButterfly*, capturedStars) \
    macro(JSObject*, sourceOfCapturedFields) \
    macro(JSObject*, earlyCapturedExports) \
    macro(JSObject*, earlyDeferredExportsSource) \
    macro(JSCellButterfly*, earlyCapturedFields) \
    macro(JSObject*, earlySourceOfCapturedFields) \
    macro(AbstractModuleRecord*, uncompiledRecord)

struct EvaluationContext {
    ModuleEvaluationStack& stack;
    unsigned index { 0 };
    std::optional<int64_t> referrerAsyncOrder { };
    JSPromise* importPromise { nullptr };
};

struct MockedModules {
    static constexpr uint32_t notBelow = std::numeric_limits<uint32_t>::max();

    explicit MockedModules(const ImportersOfModules& importers)
        : importers(&importers)
    {
    }

    const ImportersOfModules* importers;
    BitVector modules;
    std::optional<uint32_t> positionBelow(ModuleID module) const
    {
        uint32_t position = (*positionsBelow)[module.index()];
        return position < numberBelow ? std::optional(position) : std::nullopt;
    }
    void ensurePositionsBelow(const ModuleTable&);
    void addModulesBelow(const ModuleTable&, Vector<ModuleID, 32>&&);

    Box<Vector<uint32_t>> positionsBelow;
    uint32_t numberBelow { 0 };
    std::optional<BitVector> comeToMock;
    UncheckedKeyHashMap<ModuleID, Box<BitVector>> reachedFromRoots;

    struct Hub {
        BitVector reached;
        BitVector comeToHub;
        BitVector reachedBelow;
    };
    std::optional<Hub> hub;
};

class Modules {
    WTF_MAKE_NONCOPYABLE(Modules);
    WTF_MAKE_TZONE_ALLOCATED(Modules);
public:
    using ModulePhase = AbstractModuleRecord::ModulePhase;

    explicit Modules(State&);
    ~Modules();
    void forgetUnfinishedLoads();

    Instance& instance() { return m_instance; }
    const ModuleTable& table() const { return m_table; }
    const Instance& instance() const { return m_instance; }
    unsigned linkEpoch() const { return m_linkEpoch; }
    bool loadsModulesIndividually() const { return m_loadsModulesIndividually; }
    bool linkGoesByAllRequestsOf(ModuleID module) const { return m_loadsModulesIndividually || m_leadersWhoseLinkGoesByAllRequests.get(module.index()); }
    void didGetStateOfItsOwn(ModuleID) const;
    std::span<const ModuleID> modulesLinkedAlone() const { return m_modulesLinkedAlone.span(); }
    bool isSettledForLinks(ModuleID module) const { return (m_modulesSettledForLinks[module.index() / 64] >> (module.index() % 64) & 1) || checkIsSettledForLinks(module); }
    std::span<const uint64_t> modulesSettledForLinks() const { return m_modulesSettledForLinks.span(); }
    const ModuleData& data(ModuleID module) const { return m_table.data(module); }
    const ModuleHost& host() const;
    Identifier identifier(uint32_t number) const;
    ModuleStatus status(ModuleID) const;
    ModuleRecord* recordIfExists(ModuleID) const;
    ModuleRecord* ensureRecord(ModuleID);
    JSValue value(ModuleID) const;
    bool hasInstances(ModuleID module) const { return m_table.anyModuleHasInstances() && m_table.instanceVariable(module); }
    Environment* currentInstance(ModuleID) const;
    Environment* ensureInstance(ModuleID);
    Environment* lastInstance(ModuleID) const;
    void noteCachedForeignInstance(ModuleID reexporter) { m_modulesCachingInstances.set(reexporter.index()); }
    enum class MockMode : bool { Use, Bypass };
    JSValue readExport(ModuleID, unsigned index, MockMode = MockMode::Use, Environment* instance = nullptr);
    JSValue readOffNamespaceThatListsName(ModuleID reader, JSObject* namespaceOfReader, JSObject* lister, const Identifier&);
    bool isBypassingMocks() const { return m_isBypassingMocks; }
    JSValue readExportIfInitialized(ModuleID, unsigned index, Environment* instance = nullptr) const;
    bool isExportAmbiguous(ModuleID, unsigned index) const;
    bool tryReplaceExport(ModuleID, UniquedStringImpl* name, JSValue, Environment* instance = nullptr);
    // Where an export keeps the value that tryReplaceExport() replaces. To read a place or to store into one resolves no name and runs no code of the program.
    struct ReplaceablePlace {
        enum class Kind : uint8_t { None, Variable, Field, CapturedTable, AddedTable, NamespaceHandedOn };
        Kind kind { Kind::None };
        SlotData slot { }; // Variable, Field. CapturedTable: the variable that holds a copy, if `hasVariable`.
        Environment* instance { nullptr }; // Field.
        ModuleID module; // CapturedTable, AddedTable: the module of the table. NamespaceHandedOn: the module of the namespace.
        UniquedStringImpl* name { nullptr }; // CapturedTable, AddedTable.
        JSObject* table { nullptr }; // CapturedTable, AddedTable.
        bool hasVariable { false };
        explicit operator bool() const { return kind != Kind::None; }
        bool isTable() const { return kind == Kind::CapturedTable || kind == Kind::AddedTable; }
    };
    // The place that tryReplaceExport() stores into. None where it stores nothing.
    ReplaceablePlace findPlaceToReplaceExport(ModuleID, UniquedStringImpl* name, Environment* instance = nullptr);
    bool storeIntoReplaceablePlace(const ReplaceablePlace&, JSValue);
    // Empty for a place that has no value yet.
    JSValue readReplaceablePlace(const ReplaceablePlace&);
    // A host keeps what its stores replace in a table of its own, by export name: `kept`. The table stands for an export that had no value yet.
    // It also remembers, under a private name, the place of each value that it got here, so that a later store goes there whatever the name resolves to by then.
    bool keepExportToReplace(ModuleID, UniquedStringImpl* name, Environment* instance, JSObject* kept, UniquedStringImpl* keptUnder);
    bool storeIntoKeptPlace(JSObject* kept, UniquedStringImpl* keptUnder, JSValue);
    JSValue readKeptPlace(JSObject* kept, UniquedStringImpl* keptUnder);
    bool putBackKeptExport(JSObject* kept, UniquedStringImpl* keptUnder);
    void fillExportsOfBodyThatNeverRuns(ModuleID);
    bool wasLoadedWithImporter(ModuleID) const;
    ReplaceablePlace findPlaceToReplaceExport(ModuleID, UniquedStringImpl* name, BitVector& seenBehindStars, Environment* instance = nullptr);
    ReplaceablePlace findPlaceToReplaceExportBehindStars(ModuleID, UniquedStringImpl* name, BitVector& seenBehindStars);
    ReplaceablePlace findPlaceToReplaceCapturedExport(ModuleID, UniquedStringImpl* name);
    ReplaceablePlace keptPlace(JSObject* kept, UniquedStringImpl* keptUnder, Identifier& nameInTable, bool& isGone);
    bool tryReplaceCapturedExport(ModuleID, UniquedStringImpl* name, JSValue);
    struct SyntheticExportPlace {
        ModuleID module;
        uint32_t name;
    };
    const SyntheticExportPlace* syntheticExportKeptIn(SlotData variable);
    bool mayBeMocked(ModuleID) const;
    ModuleID moduleWithExportsOf(ModuleID) const;
    ModuleID moduleWithNamesOf(ModuleID) const;
    ModuleID moduleLoadedBy(const ModuleRequestData&) const;
    ModuleID moduleLoadedUnder(ModuleID named) const;
    void willMock(ModuleID);
    void startLoadingModulesIndividually();
    void didRemoveMock(ModuleID);
    void didFinishTests();
    void markFactoryAbandoned(ModuleID module) { m_modulesWithAbandonedFactory.set(module.index()); }
    void markMockPassthrough(ModuleID module) { m_modulesWithPassthroughMock.set(fileOf(module).index()); }
    void didBeginTestFile()
    {
        m_filesThatRanBeforeTestFile = filesThatHaveRun();
        m_dataModulesLoadedBeforeTestFile = dataFilesThatHaveRun();
        m_restoredModules.clearAll();
        m_entriesOwnedByTestFile.clearAll();
        m_entriesRemovedForMock.clearAll();
        noteLoadsOfEndedTestFile();
        m_modulesWithOrphanedFetch.clearAll();
        m_mocksCalledInTestFile.clearAll();
        m_factoriesCalledWhenNamed.clear();
        if (!std::exchange(m_hasBegunTestFile, true)) {
            m_filesThatRanInPreload = m_filesThatRanBeforeTestFile;
            recordVariantsRegisteredByPreload();
            recordPreloadedMocks();
        }
        runScheduledVariantRestores();
        m_variantsFetchedByTestFile.clearAll();
        BitVector evictedFailures = m_modulesReleasedByLoader;
        evictedFailures.filter(m_loadedInPreload);
        for (BitVector* letGo : { &m_modulesReleasedByLoader, &m_importersOfMockThatEnded }) {
            letGo->exclude(m_loadedInPreload);
            letGo->exclude(m_filesThatRanInPreload);
        }
        for (size_t module : evictedFailures)
            releaseFailedESModule(ModuleID(module));
        m_factoryBlockedImportRoots.clear();
        m_factoryBlockedImports.clear();
        m_failuresOfFactories.clear();
        m_missingMockFailures.clear();
        m_didFinishTests = false;
        m_pendingActualKeyLoads.clear();
        m_factoryBlockedFetches.clear();
        m_modulesWithAbandonedFactory.clearAll();
        m_factoryBlockedRootsSnapshot.clearAll();
        m_mockImportTargetsSnapshot.clearAll();
        m_modulesWithPassthroughMock.clearAll();
        m_rootsOfImportsThatHaveGoneOn.clear();
        m_detachedLoads.clear();
        m_rootsDueBehindImports.clear();
        m_originalsThatImportTheirImporters.clearAll();
        m_filesThatRanDuringMockThatOnlyWaited.clearAll();
        m_blockedRequiresWithError.clearAll();
        m_rootsOfImportsThatFailedUnderMocks.clearAll();
        m_mocksWhoseEntryProgramDeleted.clearAll();
        didChangeLinkState();
        m_mocksOfPreloadThatCameBackBeforeAnyImport.clearAll();
        m_restoredPreloadMocks.clearAll();
        m_modulesWithNoNamesOwedBelow.clearAll();
        forgetEndedMocksOfEarlierTestFiles();
        m_linksOfLoadsThatMayFail.clear();
    }
    const Vector<ModuleID>& modulesOfKind(ModuleKind, std::optional<Vector<ModuleID>>&);
    BitVector filesThatHaveRun();
    BitVector dataFilesThatHaveRun();
    bool wasLoadedBeforeTestFile(ModuleID);
    void clearLoadedBeforeTestFile(ModuleID module) { m_dataModulesLoadedBeforeTestFile.clear(module.index()); }
    void markEntryOwnedByTestFile(ModuleID module) { m_entriesOwnedByTestFile.set(module.index()); }
    bool isEntryOwnedByTestFile(ModuleID module) const { return m_entriesOwnedByTestFile.get(module.index()) || m_entriesRemovedAfterTestFile.get(module.index()); }
    void didRemoveEntryAfterTestFile(ModuleID file) { m_entriesRemovedAfterTestFile.set(file.index()); }
    void didRestoreEntryForPreload(ModuleID file) { m_entriesRemovedAfterTestFile.clear(file.index()); }
    void markVariantEvictedByMock(ModuleID form)
    {
        m_variantsEvictedByMock.set(form.index());
        markImportersEvictedWithVariant(form);
    }
    bool wasVariantEvictedByMock(ModuleID form) const { return m_variantsEvictedByMock.get(form.index()); }
    void recordVariantRequest(ModuleID moduleOrFacade)
    {
        ModuleID form = fileOf(moduleOrFacade);
        if (!isMocked(moduleOrFacade) && !isMocked(form) && m_variantsEvictedByMock.clear(form.index()))
            m_variantsFetchedByTestFile.set(form.index());
        if (m_importersEvictedWithVariants.clear(moduleOrFacade.index()))
            recordStaticImportRequests(moduleOrFacade);
    }
    bool wasVariantFetchedByTestFile(ModuleID form) const { return m_variantsFetchedByTestFile.get(form.index()); }
    void markImportersEvictedWithVariant(ModuleID form);
    void recordStaticImportRequests(ModuleID importer);
    bool wasVariantRegisteredByPreload(ModuleID form) const { return m_variantsRegisteredByPreload.get(form.index()); }
    void scheduleVariantRestore(ModuleID restoredVariant, const Vector<ModuleID, 2>& forms) { m_scheduledVariantRestores.append({ restoredVariant, forms }); }
    void recordVariantsRegisteredByPreload();
    void runScheduledVariantRestores();
    void didRemoveEntryForMock(ModuleID file) { m_entriesRemovedForMock.set(file.index()); }
    void didRestoreEntryAfterMock(ModuleID file) { m_entriesRemovedForMock.clear(file.index()); }
    void markFetchOrphaned(ModuleID file);
    bool hasOrphanedFetch(ModuleID file) const { return m_modulesWithOrphanedFetch.get(file.index()); }
    void recordEntryRequest(ModuleID moduleOrFacade, ModuleID asker, LoadKind way)
    {
        ModuleID file = fileOf(moduleOrFacade);
        if (m_entriesRemovedForMock.get(file.index()) && !isMockBypassedFor(file, asker, way))
            markEntryOwnedByTestFile(file);
        if (m_entriesRemovedAfterTestFile.get(file.index()) && !isMocked(file)) {
            m_entriesRemovedAfterTestFile.clear(file.index());
            markEntryOwnedByTestFile(file);
        }
        if (!isMocked(file) && !m_modulesWithPendingMock.get(file.index()))
            m_modulesWithOrphanedFetch.clear(file.index());
    }
    bool isMockBypassedFor(ModuleID file, ModuleID asker, LoadKind);
    void markEvaluated(ModuleID module) { setStatus(module, ModuleStatus::Evaluated); }
    bool isMocked(ModuleID module) const { return m_mocks && m_mocks->modules.get(module.index()); }
    bool hasPendingMock(ModuleID file) const { return m_modulesWithDeferredMock.get(file.index()) && !m_modulesWithActiveMock.get(file.index()); }
    JSObject* activeMock(ModuleID module) const
    {
        if (!m_activeMockCount) [[likely]]
            return nullptr;
        const LoadedModule* loaded = loadedIfExists(module);
        return loaded ? loaded->activeMock.get() : nullptr;
    }
    bool hasAnyActiveMock() const { return m_activeMockCount; }
    bool someRetainedRecordHasLeft() const { return m_someRetainedRecordHasLeft; }
    struct StoredResolutions {
        uint32_t activeMocksVersion;
        bool isShut { false };
        bool isPassedBy { false };
        UncheckedKeyHashMap<std::pair<AbstractModuleRecord*, RefPtr<UniquedStringImpl>>, AbstractModuleRecord::Resolution> resolutions { };
        UncheckedKeyHashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> isLeftOut { };
        BitVector namesBehindStarsAreInTables { };
    };
    StoredResolutions* answersKept() const
    {
        if (!m_answersKept) [[likely]]
            return nullptr;
        if (m_answersKept->activeMocksVersion != m_activeMockCount.timesWritten()) [[unlikely]]
            dropAnswersKept();
        return m_answersKept->isShut || m_answersKept->isPassedBy ? nullptr : m_answersKept;
    }
    void dropAnswersKept(bool shuts = true) const
    {
        didChangeLinkState();
        if (m_answersKept) [[unlikely]]
            *m_answersKept = { m_activeMockCount.timesWritten(), shuts || m_answersKept->isShut, m_answersKept->isPassedBy };
    }
    class KeepsAnswers {
    public:
        explicit KeepsAnswers(Modules& modules)
            : m_answers { modules.m_activeMockCount.timesWritten() }
            , m_scope(modules.m_answersKept, modules.m_answersKept || (!modules.m_mocks && !modules.m_someRetainedRecordHasLeft) ? modules.m_answersKept : &m_answers)
        {
        }

    private:
        StoredResolutions m_answers;
        SetForScope<StoredResolutions*> m_scope;
    };
    class AsksAnew {
    public:
        explicit AsksAnew(Modules& modules)
            : m_scope(modules.m_answersKept ? modules.m_answersKept->isPassedBy : m_nothingIsKept, true)
        {
        }

    private:
        bool m_nothingIsKept { false };
        SetForScope<bool> m_scope;
    };
    Vector<std::pair<ModuleID, ModuleRecord*>, 2>& originalsBeingAsked() { return m_originalsBeingAsked; }
    struct OriginalAnswered {
        unsigned depth;
        ModuleID module;
        UniquedStringImpl* name;
        bool isShown;
    };
    Vector<OriginalAnswered, 4>& originalsAnswered() { return m_originalsAnswered; }
    bool someMockIsSeen() const { return m_activeMockCount || !m_removedMocksInUse.isEmpty(); }
    bool setActiveMock(ModuleID, JSObject* mock, JSValue requireReturnValue, bool keepsEarlierMockForImporters = false);
    bool setMockInPlaceOfLibraryModule(ModuleID, JSObject* mock, JSValue requireReturnValue);
    JSObject* namespaceOfRecordSetAside(ModuleRecord*);
    ModuleRecord* displacedRecord(ModuleID);
    ModuleRecord* ensureDisplacedRecord(ModuleID);
    void forgetAnswersThatName(ModuleRecord*);
    JSPromise* displacedRecordEvaluation(ModuleID);
    bool bodySetAsideWaitsForItsMock(ModuleID, std::optional<ModuleID> factoryCallerLoad);
    bool bodySetAsideCallsImportNow(ModuleID);
    JSPromise* evaluationOnceBodySetAsideHasGoneOn(ModuleID, std::optional<ModuleID> factoryCallerLoad);
    void decideWhetherBodySetAsideWaits(JSArray* asked);
    void recordImportForOriginalBodies(ModuleID imported, std::optional<int64_t> orderOfCaller, JSPromise*);
    void releaseSettledImports();
    struct ImportCalledByBodySetAside {
        ModuleID imported;
        std::optional<int64_t> orderOfCaller;
        Strong<JSPromise> promise;
    };
    Vector<ImportCalledByBodySetAside, 2> m_importsCalledByBodiesSetAside;
    size_t m_settledImportsReleaseThreshold { 0 };
    std::optional<std::pair<ModuleID, ModuleID>> m_importThatBodySetAsideCallsNow;
    bool tryKeepOriginalIntact(ModuleID, bool onlyIfLoadedInPreload, std::optional<ModuleID> askingAsHostNamesIt, bool evenIfEvaluationFailed);
    bool isGraphRunning() const { return m_innermostEvaluation || m_rootWhoseFilesRun || m_rootAwaitingLoader || m_instance.vm->synchronousModuleQueue(); }
    bool hasUnevaluatedLinkedImporter(ModuleID, std::optional<ModuleID> askingAsHostNamesIt, bool onlyOfProject = false) const;
    bool keepsStatusOfRunBeforeLoadThatRuns(ModuleID) const;
    BitVector modulesThatWerePassedWithoutStatus() const;
    uint32_t momentOfFirstMockInTestFile(ModuleID);
    void noteEndOfLoadedMock(ModuleID);
    void noteEndOfMockWhoseFactoryWasNeverCalled(ModuleID);
    bool ranDuringMockThatOnlyWaited(ModuleID file) const { return m_filesThatRanDuringMockThatOnlyWaited.get(file.index()); }
    void noteEndOfMockThatOnlyWaited(ModuleID);
    bool takeWhetherFactoryWasCalled(ModuleID module) { return m_modulesWhoseFactoryWasCalled.clear(module.index()); }
    void countMocksAnewBehindLoad(ModuleID);
    bool hasIntactOriginal(ModuleID moduleOrFacade) const { return !m_mocksWithIntactOriginal.isEmpty() && m_mocksWithIntactOriginal.contains(m_table.moduleBehindFacade(moduleOrFacade).value_or(moduleOrFacade)); }
    void didDisplaceIntactOriginal(ModuleID module, bool evenIfLoadedEarlier)
    {
        m_mocksWithIntactOriginal.find(module)->value.isDisplaced = evenIfLoadedEarlier || wasFileLoadedByTest(module);
    }
    bool isDisplacedByMock(ModuleID module) const
    {
        auto mock = m_mocksWithIntactOriginal.find(module);
        return mock != m_mocksWithIntactOriginal.end() && mock->value.isDisplaced;
    }
    bool setMockWithIntactOriginal(ModuleID, JSObject* mock, JSValue requireReturnValue, bool keepsEarlierMockForImporters = false);
    bool reachesStarCycle(ModuleID) const;
    void removeMockWithIntactOriginal(ModuleID);
    bool seesMockOf(ModuleID mocked, ModuleID importer) const;
    bool keepsMockThatStandsWhileAnotherWaits(ModuleID mocked, ModuleID importer) const;
    ActiveMockExports retainedActiveMockExports(ModuleID fileOrModule);
    bool isLinkedToRetainedMock(ModuleID fileOrModule, ModuleID importer) const;
    bool seesEndedMockLaidInPreload(ModuleID fileOrModule, ModuleID importer) const;
    bool isImportedByModuleLinkedInThisTestFile(ModuleID fileOrModule, ModuleID module) const;
    bool hasImporterLinkedSince(ModuleID fileOrModule, uint32_t moment) const;
    bool isPreloadMockDisplaced(ModuleID fileOrModule);
    bool tryRestorePreloadMock(ModuleID fileOrModule);
    JSObject* mockSeenBy(ModuleID mocked, ModuleID importer, ModuleID reader = { }) const;
    bool seesMockKeepingOriginalNames(ModuleID mocked, ModuleID importer, ModuleID reader) const;
    JSObject* endedMockSeenBy(ModuleID mocked, ModuleID importer) const;
    bool hasRemovedMockInUseFor(ModuleID) const;
    bool keepsSomeEndedMock() const { return !m_removedMocksInUse.isEmpty(); }
    bool isNamedByEndedMockBelow(ModuleID, const Identifier&);
    bool hasRemovedMockDuringStarCollection(ModuleID module) const { return m_collectionOfNamesBehindStars && hasRemovedMockInUseFor(module); }
    std::optional<bool> endedMockHasNameAsSeenBy(ModuleID mocked, ModuleID holder, const Identifier&);
    std::optional<bool> isLeftOutInViewOf(ModuleID importer, ModuleID mocked, unsigned exportIndex) const;
    JSObject* namespaceSeenBy(ModuleID module, ModuleID importer, ModuleID reader = { });
    std::optional<unsigned> indexOfEndedMockSeenAt(ModuleID fileOrModule, uint32_t moment) const;
    std::optional<unsigned> indexOfEndedMockSeenBy(ModuleID fileOrModule, ModuleID importer, ModuleID reader = { }) const;
    uint32_t momentLinkedAsOf(ModuleID importer, ModuleID reader) const;
    JSObject* endedMock(ModuleID fileOrModule, std::optional<unsigned> index) const;
    uint32_t momentLinked(ModuleID importer) const;
    void forgetEndedMocksOfEarlierTestFiles();
    void noteLoadedAnew(ModuleID, ModuleID root);
    void forgetLinksInRefusedLoadsThatCameTo(ModuleID mocked);
    void bringBackLinksOfLoadThatFailed(ModuleID root);
    void letLinksOfLoadStand(ModuleID root);
    void linkAnewWhoeverReadsEndedMockBelow(ModuleID root);
    JSValue handedOnInPlaceOfNamespace(const ModuleExportData&) const;
    bool tryReplaceNamespaceHandedOn(ModuleID reexportedModule, JSValue);
    void didReplaceNamespaceHandedOn(ModuleID reexportedModule);
    std::span<const ModuleID> modulesThatHandOnNamespaceOf(ModuleID reexportedModule);
    void releaseMockAtNextLoad(ModuleID);
    void markMockLingering(ModuleID module)
    {
        m_modulesWithLingeringMock.set(module.index());
        m_hasImportsBlockedOnFactories |= !m_modulesWithRunningFactory.get(module.index());
    }
    bool isMockLingering(ModuleID module) const { return m_modulesWithLingeringMock.get(module.index()); }
    bool isRequestedByDetachedLoad(ModuleID mocked) const { return m_resumingDetachedLoad && staticImportClosure(*m_resumingDetachedLoad, MockTraversal::Stop).get(mocked.index()); }
    bool isHeldByDetachedLoad(ModuleID mocked) const;
    void retryMockInDetachedLoads(ModuleID mocked);
    uint64_t factoryCallOfResumedLoad(ModuleID mocked) const;
    bool needsRetryInDetachedLoad(ModuleID mocked) const { return m_detachedLoads.containsIf([&](const auto& left) { return left.wait->status() == JSPromise::Status::Pending && left.modulesToRetry.get(mocked.index()); }); }
    void didResumeAfterFactories() { m_hasImportsBlockedOnFactories = false; }
    bool tryMarkMockLingering(ModuleID module, bool wasInstalledOnPreloadMock)
    {
        if (m_isRemovingLingeringMocks || isCurrentImportTarget(module))
            return false;
        if (m_modulesWithRunningFactory.get(module.index()) && hasIntactOriginal(module))
            markMockLingering(module);
        else if (activeMock(module) && isHeldByDetachedLoad(module))
            m_modulesWithLingeringMock.set(module.index());
        else
            return false;
        m_modulesWhoseEndPutOffBringsNoMockOfPreloadBack.set(module.index(), !wasInstalledOnPreloadMock);
        return true;
    }
    void removeLingeringMocks();
    void removeLingeringMock(ModuleID);
    bool isResolvedForInFlightLoads(ModuleID mocked) const;
    void removeUnusedLingeringMocks(ModuleID requested = ModuleID());
    void removeUnusedLingeringMocksSlow(ModuleID requested);
    void noteImportUnderMockInPlace(ModuleID);
    bool wasLoadedUnderMockInPlaceByThisTestFile(ModuleID fileOrModule);
    void forgetWhoLoadedItUnderMockInPlace(ModuleID fileOrModule) { ensureLoaded(moduleWithExportsOf(fileOrModule)).momentOfTestFileThatLoadedItUnderMockInPlace = 0; }
    void bringBackMockOfPreloadThatWasPatched(ModuleID fileOrModule);
    void releaseRemovedMock(ModuleID);
    void releaseRemovedMockSlow(ModuleID);
    bool releaseLibraryMockOnLoad(ModuleID);
    void releaseRemovedMocks();
    void releaseRemovedMocksFrom(ModuleID root);
    bool someMockThatEndedWaits() const { return !m_modulesWithRemovedMock.isEmpty(); }
    bool hasRemovedMock(ModuleID module) const { return m_modulesWithRemovedMock.get(module.index()); }
    bool willReloadAfterMockRemoval(ModuleID) const;
    bool hasMockOfLibraryModule(ModuleID module) const { return m_libraryModulesWithMock.get(module.index()); }
    bool wasLoadedByTestFile(ModuleID) const;
    bool wasFileLoadedByTest(ModuleID) const;
    enum class ReleasedImporters : uint8_t { All, AllButThoseThatThisTestFileLinkedToModule, WithInstances };
    void releaseMockedCycleMember(ModuleID);
    bool releaseImportersOf(ModuleID mocked, ReleasedImporters, std::optional<ModuleID> patched = std::nullopt);
    void releaseDisplacedMockImporters(ModuleID mocked);
    void endStaleDetachedLoads(ModuleID mocked);
    void evictRequiredImporters(ModuleID mocked)
    {
        m_requiredImportersToEvict.removeAllMatching([&](const auto& pair) {
            if (pair.second != mocked)
                return false;
            if (wouldBeEvicted(fileOf(pair.first)))
                evict(pair.first);
            else
                releaseImportersOf(mocked, ReleasedImporters::All);
            return true;
        });
    }
    Vector<std::pair<ModuleID, ModuleID>> m_requiredImportersToEvict;
    bool isReachedByBlockedLoad(ModuleID, ModuleID mocked);
    enum class FactoryOutcome : uint8_t { Threw, Settled };
    void retainFactoryLoadedImporters(ModuleID mocked, FactoryOutcome outcome)
    {
        m_importsWithRefusedRequires.removeAllMatching([&](const auto& pair) {
            if (pair.second != mocked)
                return false;
            if (outcome == FactoryOutcome::Settled && status(pair.first) <= ModuleStatus::Linked && m_table.hasFilePath(pair.first) && host().didRunOriginal)
                host().didRunOriginal(m_instance.globalObject, m_table.fullPath(pair.first), OriginalRun::PrivateCopy);
            return true;
        });
        m_filesVisibleToOwnFactories.clear(mocked.index());
        bool keepsImportersOfOriginal = !m_importerLoadingOriginals.isEmpty() && (outcome == FactoryOutcome::Threw || !m_mocks || isOnCycleWithOtherMock(mocked));
        m_factoryLoadedImporters.removeAllMatching([&](const auto& pair) {
            if (pair.second != mocked)
                return false;
            if (auto original = m_importerLoadingOriginals.find(pair.first); original != m_importerLoadingOriginals.end() && original->value == mocked) {
                m_importerLoadingOriginals.remove(original);
                if (keepsImportersOfOriginal)
                    return true;
            }
            bool isCopy = m_modulesWithPrivateCopy.get(pair.first.index()) && outcome == FactoryOutcome::Settled && !isReachedByBlockedLoad(pair.first, mocked);
            if (isCopy && tryLeaveUnderActualKey(pair.first))
                return true;
            if (m_modulesSharedWithFactories.clear(pair.first.index()) && outcome == FactoryOutcome::Settled && mayReleaseModule(pair.first)) {
                m_modulesToEvictOnceLoaded.append(pair.first);
                return true;
            }
            if ((outcome == FactoryOutcome::Threw || isReachedByBlockedLoad(pair.first, mocked) || !tryReleaseModule(pair.first)) && !m_importersRetainedAboveMock.set(pair.first.index()))
                m_retainedFactoryImporters.set(pair.first.index());
            return true;
        });
    }
    Vector<std::pair<ModuleID, ModuleID>> m_factoryLoadedImporters;
    Vector<std::pair<ModuleID, ModuleID>> m_importsWithRefusedRequires;
    UncheckedKeyHashMap<ModuleID, ModuleID> m_importerLoadingOriginals;
    bool isOnCycleWithOtherMock(ModuleID mocked) const;
    BitVector m_retainedFactoryImporters;
    void tellThatFileDidNotRunAgain(ModuleID);
    bool isImportedByStatement(ModuleID);
    void reportRetainedFailure(ModuleID);
    bool wasDisplacedAfterFailure(ModuleID module) const
    {
        const LoadedModule* loaded = loadedIfExists(fileOf(module));
        return loaded && loaded->wasDisplacedAfterFailure;
    }
    void tellOfFilesThatStayedLoadedBelow(ModuleID root);
    void letGoOfModuleLoadedAsMock(ModuleID module, bool endBringsBackWhatWasSetAside)
    {
        if (auto mock = m_mocksWithIntactOriginal.find(module); mock != m_mocksWithIntactOriginal.end() && (mock->value.didFileRunBesideMock || ((mock->value.wasLoadedByTestFile || mock->value.replacesRequiredValue || hasInstances(module) || (isOfPackage(module) && !m_entriesRemovedAfterTestFile.get(module.index()))) && endBringsBackWhatWasSetAside)))
            return;
        m_modulesReleasedByLoader.set(module.index());
        m_importersOfMockThatEnded.set(module.index());
    }
    void releaseAtNextLoad(ModuleID module)
    {
        if (currentInstance(module) && status(module) == ModuleStatus::Evaluated)
            m_modulesReleasedByLoader.set(module.index());
    }
    void releaseAfterEvaluation(ModuleID awaited);
    void evictOnceLoaded(ModuleID mocked);
    void evictAbandonedModules();
    bool isEvictedModuleImported();
    void ensureUninitializedMock();
    JSObject* uninitializedMock() const { return m_uninitializedMock.get(); }
    Strong<JSObject> m_uninitializedMock;
    BitVector m_filesVisibleToOwnFactories;
    void exposeOriginalUntilMocked(ModuleID);
    bool hasRootsDueBehindImports() const { return !m_rootsDueBehindImports.isEmpty(); }
    void didEvaluateForLoader(ModuleID imported);
    void evaluateRootsDueBehind(ModuleID imported);
    Vector<ModuleID, 1> m_rootsDueBehindImports;
    void tellIfOriginalImports(ModuleID mocked, ModuleID importer);
    BitVector m_originalsThatImportTheirImporters;
    void didPatchPendingMock(ModuleID mocked);
    BitVector modulesRequestedByImports(bool wasMockReplaced);
    void evictLoadedModules(bool wasMockReplaced = false);
    void evictAtNextLoad(ModuleID);
    bool wasEvictedOnceLoaded(ModuleID module) const { return m_modulesEvictedOnceLoaded.get(module.index()); }
    void finishDetachedLoadsOfRemovedMocks(ModuleID required);
    Vector<ModuleID, 2> m_modulesToEvictOnceLoaded;
    BitVector m_modulesEvictedOnceLoaded;
    BitVector m_modulesToReleaseAfterEvaluation;
    BitVector m_modulesToReleaseAtNextTestFile;
    bool wasReleasedByLoader(ModuleID module) const { return m_modulesReleasedByLoader.get(module.index()); }
    void releaseFailedESModules(std::optional<ModuleID> patched);
    void clearFailedCommonJSFiles();
    void willFetchESModules(ModuleID root);
    void evictFailedRecords();
    bool hasFailedESModule(ModuleID module) const { return isFailedESModule(module) || errorOfModuleThatRanBesideMock(module); }
    void releaseFailedESModule(ModuleID module)
    {
        m_loadRoots.clear(module.index());
        if (hasFailedESModule(module))
            m_modulesReleasedByLoader.set(module.index());
    }
    void forgetFailuresOfExportsThatBuildFoundAbsent();
    void bringBackWhatLoaderLetGoBelow(ModuleID root);
    void evaluateDueDependencies(ModuleID);
    bool hasMockEndedThatWalksPassedOver() const
    {
        for (ModuleID module : m_mockedModulesPassedOverByWalks) {
            if (!isMocked(module) && !activeMock(module))
                return true;
        }
        return false;
    }
    JSObject* originalNamespace(ModuleID);
    bool isHiddenInModuleItself(ModuleID, unsigned index) const;
    JSValue readExportOfModuleItself(ModuleID, unsigned index, Environment* instance = nullptr);
    bool isPatchedByMock(ModuleID) const;
    bool wasPatchedAfterEvaluation(ModuleID) const;
    LoadedBy evaluatedBy(ModuleID);
    LoadedBy fetchedBy(ModuleID);
    void didRemoveActiveMockEntry(ModuleID fileOrModule);
    void noteAskUnderMockInPlace(ModuleID fileOrModule);
    void noteRequireOfMockLaidInPreload(ModuleID moduleOrFacade);
    bool wasRequiredUnderPreloadMock(ModuleID file);
    bool wasImportedUnderPreloadMock(ModuleID file);
    LoadedBy activeMockReloadedBy(ModuleID fileOrModule);
    bool wasActiveMockEntryRemoved(ModuleID fileOrModule) { return activeMock(fileOrModule) && ensureLoaded(moduleWithExportsOf(fileOrModule)).nextRequestReloadsActiveMock; }
    void willRunBodyOfRecord(ModuleID, ModuleRecord*);
    void retainOriginalExports(ModuleID);
    void stashActualKeyExports(ModuleID file);
    bool isImportedByTestFile(ModuleID file);
    bool mayTakeRunUnderActualKey(ModuleID file);
    ActualKeyExports restoreActualKeyExports(ModuleID file, bool keepsSuperseded);
    void clearRetainedOriginalExports(ModuleID);
    JSValue retainedOriginalExports(ModuleID) const;
    JSValue revalidateRetainedOriginalExports(ModuleID);
    JSObject* retainedOriginalModuleObject(ModuleID) const;
    bool isUnderActualKey(ModuleID file) const { return m_modulesUnderActualKey.get(file.index()); }
    void setIsUnderActualKey(ModuleID file, bool ran) { m_modulesUnderActualKey.set(file.index(), ran); }
    void restoreOwnFactoryOriginal(ModuleID file);
    bool wasOriginalRestoredToOwnKey(ModuleID file) const { return m_modulesWithOriginalRestored.get(file.index()); }
    void setWasOriginalRestoredToOwnKey(ModuleID file, bool cameBack) { m_modulesWithOriginalRestored.set(file.index(), cameBack); }
    void setHasNoTestFile(ModuleID file) { m_modulesWithoutTestFile.set(file.index()); }
    JSValue exportsBeforePatch(ModuleID file) const;
    void takeNamesOfMockInPlace(ModuleID, JSObject* mock);
    void letRecordAnswerByMock(ModuleRecord*, JSObject* mock);
    void releaseMockRecord(ModuleID);
    struct MockedReexport {
        JSObject* mock;
        Identifier name;
        ModuleID mocked;
        bool lacksName { false };
    };
    std::optional<MockedReexport> findMockForReexport(ModuleID reexportingModule, const ModuleExportData& reexportEntry, MockMode reexportingModuleMockMode = MockMode::Use) const;
    std::optional<MockedReexport> findMockForReexportSlow(ModuleID reexportingModule, const ModuleExportData& reexportEntry, MockMode reexportingModuleMockMode) const;
    const Vector<ModuleID, 4>& reexportPathModules(ModuleID reexportingModule, unsigned index) const;
    bool canMockResolveReexport(ModuleID reexportingModule, const ModuleExportData& reexportEntry) const;
    struct PlaceOfExport {
        ModuleID module;
        unsigned index;
    };
    const Vector<Vector<PlaceOfExport, 1>>& starExportProviders(ModuleID) const;
    struct StarTarget {
        ModuleID module;
        bool isFollowed { true };
    };
    struct StarTargetsByName {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(StarTargetsByName);

        Vector<StarTarget> targets;
        Vector<unsigned> targetsWithUnlistedNames;
        UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, Vector<unsigned, 1>> targetsListingName;

        template<typename Functor>
        bool visitCandidates(UniquedStringImpl* name, const Functor& functor) const
        {
            auto found = targetsListingName.find(name);
            std::span<const unsigned> listing = found == targetsListingName.end() ? std::span<const unsigned>() : found->value.span();
            std::span<const unsigned> others = targetsWithUnlistedNames.span();
            while (!listing.empty() || !others.empty()) {
                bool takesListing = others.empty() || (!listing.empty() && listing.front() <= others.front());
                unsigned place = takesListing ? listing.front() : others.front();
                if (!listing.empty() && listing.front() == place)
                    listing = listing.subspan(1);
                if (!others.empty() && others.front() == place)
                    others = others.subspan(1);
                if (!functor(place))
                    return false;
            }
            return true;
        }
    };
    const StarTargetsByName& starTargetsByName(ModuleID) const;
    enum class MockFilter : uint8_t { All, AddingNames };
    bool isMockedThroughStarExports(ModuleID, MockFilter = MockFilter::All);
    bool isMockedThroughStarExportsSlow(ModuleID, MockFilter);
    void leaveOutWhatMockLacks(ModuleID, JSObject* exportsOfMock);
    void setMockHasCommonJSExports(ModuleID, bool);
    void setMockHasFactory(ModuleID module, bool hasFactory) { m_mocksWithoutFactory.set(module.index(), !hasFactory); }
    void takeWhatMockAdds(ModuleID, bool comesAheadOfLoad);
    bool isAddedByMock(ModuleID, const Identifier&);
    void appendNamesAddedByMock(ModuleID, WTF::Vector<Identifier>&);
    void appendNamesBehindStarsOf(ModuleID, WTF::Vector<Identifier>&);
    WTF::Vector<Identifier> namesOfRecord(AbstractModuleRecord&) const;
    bool hasNameBehindItsStars(ModuleID, UniquedStringImpl*);
    bool standsForWholeModule(ModuleID) const;
    struct SearchBehindStars {
        BitVector isOnPath;
        BitVector namesAreInTables;
        BitVector isAnswered;
        BitVector isLeftOut;
        unsigned ringsMet { 0 };
        static constexpr unsigned noRing = std::numeric_limits<unsigned>::max();
        Vector<ModuleID, 16> path;
        Vector<Vector<size_t, 2>, 16> answersThatHangOnPlaceOfPath;
        Vector<unsigned> outermostRingOfAnswer;
        unsigned outermostRing { noRing };
    };
    bool namesBehindStarsAreInTables(ModuleID module) const
    {
        SearchBehindStars search;
        return data(module).hasDynamicStarExport() && namesBehindStarsAreInTables(module, search);
    }
    enum class TablesBehindStars : bool { MayLackName, HaveName };
    bool leavesNameToStars(ModuleID module, unsigned exportIndex) const
    {
        const ModuleExportData& moduleExport = m_table.exports(module)[exportIndex];
        return moduleExport.isHiddenUntilInitialized && isVariableOfExportEmpty(module, exportIndex) && !findMockForReexport(module, moduleExport);
    }
    bool isLeftOut(ModuleID module, unsigned exportIndex, TablesBehindStars tables) const
    {
        if (leavesNameToStars(module, exportIndex)) [[unlikely]]
            return true;
        if (m_exportsLeftOut.isEmpty() && m_removedMocksInUse.isEmpty()) [[likely]]
            return false;
        SearchBehindStars search;
        return isLeftOut(module, exportIndex, tables, search);
    }
    class MocksInGraph {
    public:
        MocksInGraph(const ModuleTable& table, Box<MockedModules> mocks, ModuleID root, bool loadsRootOriginal)
            : m_table(table)
            , m_mocks(WTF::move(mocks))
            , m_root(root)
            , m_loadsRootOriginal(loadsRootOriginal)
        {
        }

        bool loadsRootOriginal() const { return m_loadsRootOriginal; }

        bool hides(ModuleID module) const
        {
            if (!m_mocks || module == m_root) [[likely]]
                return false;
            if (m_mocks->importers->namedByListsAlone.quickGet(module.index()))
                return true;
            m_mocks->ensurePositionsBelow(m_table);
            std::optional<uint32_t> position = m_mocks->positionBelow(module);
            if (!position)
                return false;
            if (!rootComesToMock())
                return m_mocks->modules.quickGet(module.index());
            return !reachedFromRoot().quickGet(*position);
        }

    private:
        const BitVector& reachedFromRoot() const;
        bool rootComesToMock() const;

        const ModuleTable& m_table;
        Box<MockedModules> m_mocks;
        ModuleID m_root;
        bool m_loadsRootOriginal;
        mutable Box<BitVector> m_reached;
    };
    MocksInGraph mocksInGraph(ModuleID root) const { return { m_table, m_mocks, root, isMocked(root) && (m_originalsEvaluatedUnderMock.get(root.index()) || runsBesideMockFor(root, std::nullopt)) }; }
    bool isLinkedBelow(ModuleID module, ModuleID root) const { return !reachesDisplacedRun(module) && !mocksInGraph(root).hides(module) && (!isMocked(module) || mocksInGraph(module).loadsRootOriginal()); }
    bool hasStartedLoading(ModuleID) const;
    bool hasOnlyBeenLinked(ModuleID);
    bool wasMockedAfterFailure(ModuleID) const;
    bool isActiveMockRegistered(ModuleID) const;
    void registerEvaluatedModule(ModuleID);
    void setModuleWithSettledFactory(std::optional<ModuleID> module) { m_moduleWithSettledFactory = module; }
    void reportImportersWithoutInstances(ModuleID);
    void reportKeptImporters(ModuleID);
    bool isMockLoadedInPreload(ModuleID module) const { return m_preloadedMocks.get(fileOf(module).index()); }
    bool wasLoadedInPreload(ModuleID module) const { return m_loadedInPreload.get(module.index()); }
    bool wasLoadedByPreloadHook(ModuleID module) const { return m_loadedByPreloadHooks.get(module.index()); }
    void forgetLoadByPreloadHook(ModuleID module)
    {
        m_loadedByPreloadHooks.clear(module.index());
        m_releasedLoadsOfPreloadHooks.clear(module.index());
    }
    bool takeFailureBeforeMock(ModuleID);
    bool isReplacedByHost(ModuleID, LoadKind);
    bool isKeptInRequireCache(ModuleID) const;
    bool isInRequireCache(ModuleID) const;
    bool isListedOnceStatementReaches(ModuleID) const;
    void takeOutOfRequireCache(ModuleID);
    Vector<ModuleID> libraryModulesInRequireCache();
    bool isLibraryModuleInRequireCache(ModuleID);
    std::optional<ModuleID> findVariantToMock(StringView path, std::optional<ModuleID> file) const;
    void deferMock(ModuleID, FactoryFailurePolicy, bool isFromPreload = false);
    void forgetMockThatWaits(ModuleID);
    void leaveImportersAbove(ModuleID mocked);
    void endMockOfLibraryModule(ModuleID);
    void createDynamicExports(ModuleID, JSCellButterfly*& names, JSObject*& capturedValues, JSObject*& liveSources, Environment* instance = nullptr);
    void takeNamesBehindFollowedStarsAnew();
    void noteWhetherFileHasThrownWhereMockIsMade(ModuleID file, bool displacesEarlierMock, bool displacesModule);
    bool isFailedESModule(ModuleID) const;
    void saveExportsBeforePatch(ModuleID);
    void didInstallMock() { m_moduleBeingPatched = std::nullopt; }
    void endMockLaidOverFile(ModuleID);
    void restorePatchedExports(ModuleID file);
    void restoreCapturedExports(ModuleID holder);
    enum class OwedBelowOriginal : bool { IsPaid, StaysOwed };
    JSValue exportsBeforeMock(ModuleID, bool evenIfEvaluationFailed = false, OwedBelowOriginal = OwedBelowOriginal::IsPaid, bool* didExportReadThrow = nullptr);
    JSObject* namespaceBeforeMock(ModuleID);
    void releaseNamespaceBeforeMock(ModuleID);
    bool runPendingMock(ModuleID);
    bool runPendingMockSlow(ModuleID);
    void forgetWhatNamesResolvedTo();
    void didEvictLibraryEntry(ModuleID);
    void willMockLibraryModule(ModuleID);
    bool letMockStandInPlaceOfLibraryModule(ModuleID);
    enum class LibraryMock : bool { Replaces, Patches };
    void displaceLibraryOriginal(ModuleID, LibraryMock);
    bool isReloadedLibraryModule(ModuleID) const;
    bool isLinkedToLibraryModuleBeforeReset(ModuleID importer, ModuleID target) const;
    void detachImportersLinkedBeforeReset(ModuleID);
    bool isReloadedLibraryModuleRead(ModuleID) const;
    ModuleNamespace* reloadedLibraryWord(ModuleID);
    void showPatchToReloadedImporters(ModuleID);
    void leaveStructuresOfReloadedImporters(ModuleID);
    bool wasLoadedBeforeReset(ModuleID) const;
    void noteLibraryModuleLoaded(ModuleID);
    void separateReloadedNamespace(ModuleID);
    bool hasSeparateNamespace(ModuleID) const;
    void restoreLibraryOriginal(ModuleID, LibraryMock);
    bool doesMockReplaceLibraryModule(ModuleID module) const { return m_libraryModulesThatMockReplaces.get(module.index()); }
    bool isLibraryOriginalDisplaced(ModuleID module) const { return m_displacedLibraryModules.get(module.index()); }
    enum class LoadRootKind : bool { StartsLoad, ImportStatement };
    bool runMocksInGraph(ModuleID root, LoadRootKind = LoadRootKind::StartsLoad);
    bool runMocksInGraphSlow(ModuleID root, LoadRootKind);
    bool wasLoadedWhenMockWasMade(ModuleID);
    bool forgetLoadThatFailed(ModuleID file);
    bool importsModuleWithPendingMock(ModuleID root);
    bool evaluateLinkedModuleBeforeMock(ModuleID, bool keepsOriginalIntact);
    JSPromise* pendingEvaluation(ModuleID, bool hasAlreadyWaited);
    JSPromise* pendingEvaluationInGraph(ModuleID file);
    bool isImportedByGraphUnderEvaluation(ModuleID, bool forIntactOriginal = false);
    bool isLinkedByDeferredImport(ModuleID) const;
    void forgetLoadsRefusedByCheckOfNames(ModuleID mocked);
    bool wasRegisteredByFailedLoad(ModuleID) const;
    bool wasLinkedByFailedGraph(ModuleID, bool byEarlierTestFile);
    bool isImportedByFailedRoot(ModuleID, bool ofEarlierTestFile = false) const;
    bool wasReachedByFailedGraph(ModuleID) const;
    bool isFailedLoadRoot(ModuleID module) const { return m_loadRoots.get(module.index()) && status(module) == ModuleStatus::Failed; }
    std::optional<ModuleID> findUnlinkableModule(ModuleID root) const;
    void resetChecksOfNames();
    bool hasLinkFailures() const { return m_errorsOfExportsThatBuildFoundAbsent || !m_linkFailures.isEmpty(); }
    bool wasLinkedAheadOfFailure(ModuleID root, ModuleID) const;
    bool wasFetchedByFailedLoad(ModuleID) const;
    bool wasLinkedBeforeLinkFailure(ModuleID) const;
    bool hasImporterLinkedBeforeFailure(ModuleID);
    bool isLinkedForPendingImporter(ModuleID, std::optional<ModuleID> asking) const;
    std::optional<ModuleFailureID> failureByAbsentExport(ModuleID) const;
    bool tryEvaluateESModuleBeforeMock(ModuleID);
    bool startDetachedOriginalLoad(ModuleID);
    bool tryStartAsyncOriginal(ModuleID);
    JSObject* evaluateOriginalUnderMock(ModuleID);
    void evaluateCommonJSModuleBeforeMock(ModuleID);
    bool isLoaderInMiddleOfGraph() const;
    bool isImportedByModuleThatRequireAnswered(ModuleID) const;
    void keepErrorOfLoad(ModuleID root, JSValue error, const ScopedLambda<bool(ModuleID)>& endedLoad);
    void cacheDetachedFactoryError(ModuleID required, JSValue error);
    void didSettleDetachedFactory(unsigned number);
    void evaluateDetachedOriginal(size_t index);
    void didFailDetachedFactory(unsigned number, JSValue error);
    void didFinishLoaderWait(unsigned number);
    void didEvaluateDetachedLoad(unsigned number);
    void finishEvictedLoad(unsigned number);
    void finishRemovedMockLoad(unsigned number);
    void finishLoadsAwaitingLoader(std::optional<ModuleID> changing);
    unsigned subscribeToPendingFactories(ModuleID required, unsigned number, bool isForOriginal = false);
    bool hasErrorCachingDetachedLoad(ModuleID required) const { return m_detachedLoads.containsIf([&](const auto& left) { return left.required == required && left.keepsError; }); }
    void clearMockLoadError(ModuleID);
    void clearCachedMockError(ModuleID);
    bool hidesFailureOfLoad(ModuleID mocked, ModuleID root);
    void forgetFailedLoadsAt(ModuleID mocked);
    void clearLoadErrorsForMock(ModuleID mocked);
    void noteImportFailedUnderMock(ModuleID root, JSValue error);
    bool hasFailedUnderMockThatStands(ModuleID root);
    void letRootsOfFailedImportsGoWithWhatIsReset();
    void forgetImportFailedUnderMock(ModuleID root);
    enum class FailedImports : bool { Evict, MakeUnevictable };
    void settleFailedImportsUnder(ModuleID mocked, FailedImports);
    void didFailMockImportRoot(ModuleID mocked);
    void dropErrorsOfLoadsThatCameTo(const ScopedLambda<bool(ModuleID)>& isMocked);
    Vector<ModuleID, 8> evictableFailedLoads(const ScopedLambda<bool(ModuleID)>& isMocked);
    void clearMockLoadErrors();
    void clearEvictableFailures()
    {
        pruneActualKeyLoads();
        clearMockLoadErrors();
        m_missingMockFailures.clear();
    }
    void discardDetachedLoadErrorsReaching(ModuleID);
    void discardDetachedLoadErrors()
    {
        for (DetachedLoad& left : m_detachedLoads)
            left.keepsError = false;
    }
    void takeFailedLoadsOutOfLoader(const ScopedLambda<bool(ModuleID)>& isMocked);
    JSValue errorOfModuleThatRanBesideMock(ModuleID module) const
    {
        const LoadedModule* loaded = loadedIfExists(module);
        return loaded ? loaded->errorOfModuleThatRanBesideMock.get() : JSValue();
    }
    JSValue mockLoadError(ModuleID module) const { return m_modulesWithMockLoadError.get(module.index()) ? loadedIfExists(module)->cachedLoadError.get() : JSValue(); }
    bool didMockFailForImportStatement(ModuleID);
    void willFetchAfterTick(ModuleID);
    std::optional<ModuleID> loadOfFetch(ModuleID fetched);
    JSPromise* pendingFactoryForFetch(ModuleID fetched, JSValue& state);
    std::optional<ModuleID> currentLoadTarget() const { return m_currentImportTarget ? std::optional(fileOf(m_currentImportTarget)) : m_moduleBeingFetched; }
    std::optional<CurrentImportCall> currentImportCall() const { return m_currentImportCallerModule ? std::optional(CurrentImportCall { fileOf(m_currentImportCallerModule), fileOf(m_currentImportRequestedModule), m_currentImportSequenceNumber }) : std::nullopt; }
    bool runMocksInFetchedGraph(ModuleID);
    bool runMocksInFetchedGraphSlow(ModuleID);
    bool isFetchedForImportStatement(ModuleID) const;
    JSPromise* pendingFactoryInGraph(ModuleID root, PendingFactoryRequester, JSValue& state);
    bool tellHostOfRequire(ModuleID required);
    void setNeedsMockReinstall(ModuleID module, bool waits)
    {
        m_modulesNeedingMockReinstall.set(module.index(), waits);
        if (waits)
            m_modulesWithNothingOwedBelow.clearAll();
    }
    void reinstallBodyMocks(ModuleID);
    bool hasMocksToReinstall() const;
    enum class MockTraversal : bool { Continue, Stop };
    BitVector staticImportClosure(ModuleID root, MockTraversal) const;
    void listImportersAndRequirers();
    void noteRequireThatRunTimeAnswers(ModuleID target, ModuleID referrer);
    bool noteEntryParent(ModuleID target, ModuleID referrer);
    std::optional<ModuleID> forgetEntryParent(ModuleID target);
    void didDisplaceOriginal(ModuleID);
    void didEndMockOfEntry(ModuleID, bool restoresOriginal);
    std::optional<ModuleID> entryParentToRelease(ModuleID target);
    std::optional<ModuleID> retainedEntryParent(ModuleID target);
    BitVector transitiveImportersOf(std::span<const ModuleID> files);
    bool wasEvictedFromRequireCache(ModuleID testFile);
    void noteMockEnd(ModuleID);
    BitVector transitiveImportersOf(ModuleID file) { return transitiveImportersOf(std::span(&file, 1)); }
    bool importsStatically(ModuleID importer, ModuleID imported) const;
    enum class MockState : uint8_t { None, Waits, Runs, InPlace, InPlaceWhileAnotherWaits, InPlaceOverModuleThatHasRun, Fills, LaidOver, Failed, Ended, EndedWhileAnotherWaits, NoneOfThem };
    MockState mockState(ModuleID);
    String describeMockState(ModuleID);
    void countMockStates(ASCIILiteral door, const String& before, ModuleID);
    void noteCallOfMock(ModuleID fileOrModule, bool keepsActiveMock = false);
    void deferMockAfterAsyncFailure(ModuleID);
    bool isCurrentRequireTarget(ModuleID module) const { return m_currentRequireTarget == module; }
    void restoreOriginalForRequire(ModuleID);
    bool hasPendingMocks() const { return !m_modulesWithPendingMock.isEmpty(); }
    void noteHostImportInFlight(JSPromise*, ModuleID, bool isFromOwnFactory);
    void didTakeFirstHostImportStep(JSPromise*);
    bool isMockUnseen(ModuleID module) const { return m_unseenMocks.get(fileOf(module).index()); }
    bool isEvictedWithOriginal(ModuleID) const;
    void willEndMocksOfTestFile() { m_originalsRestoredAfterPass.emplace(); }
    void didEndMocksOfTestFile() { m_originalsRestoredAfterPass = std::nullopt; }
    void willRestoreOriginal(ModuleID module)
    {
        if (m_originalsRestoredAfterPass && hasIntactOriginal(module) && !isEvictedWithOriginal(module))
            m_originalsRestoredAfterPass->set(module.index());
    }
    bool takeUnseenMock(ModuleID module) { return m_unseenMocks.clear(fileOf(module).index()); }
    bool takeReleaseUnderPendingPatch(ModuleID);
    void dropPatch(ModuleID);
    void oweToEvaluatedImporters(ModuleID);
    bool isOriginalEvicted(ModuleID module) const { return m_evictedOriginals.get(fileOf(module).index()) && hasIntactOriginal(module); }
    void didRequireOwnOriginal(ModuleID module)
    {
        if (!m_originalsRequiredByFactories.get(module.index()))
            m_requirersOfOwnOriginals.set(module.index());
    }
    bool tryEvictRequirerOfOriginal(ModuleID);
    bool takeRequirerOfOwnOriginal(ModuleID module) { return m_requirersOfOwnOriginals.clear(module.index()); }
    bool takeEvictedOriginal(ModuleID module) { return m_evictedOriginals.clear(fileOf(module).index()); }
    void forgetMockRegisteredAfter(ModuleID, uint32_t moment);
    bool hasBatchedFetches() const { return !m_batchedFetches.isEmpty(); }
    bool wasRequestedByFactoryCaller(ModuleID mocked);
    bool awaitsGeneratedModule(ModuleID mocked);
    bool isRequiringAnotherModule(ModuleID module) const { return m_currentRequireTarget && fileOf(m_currentRequireTarget) != fileOf(module); }
    String requirerPath(ModuleID required) const { return isCurrentRequireTarget(required) && m_requiringModule && m_table.hasFilePath(m_requiringModule) ? m_table.fullPath(m_requiringModule) : String(); }
    bool isCurrentImportTarget(ModuleID module) const { return m_currentImportTarget && fileOf(m_currentImportTarget) == fileOf(module); }
    BitVector modulesInLoadGraph(ModuleID root, LoadGraphMode = LoadGraphMode::RespectMocks) const;
    bool isCurrentImportTargetAfterWait(ModuleID module) const { return m_currentImportWaitState && isCurrentImportTarget(module); }
    JSValue currentImportWaitState() const { return m_currentImportWaitState; }
    void tellIfMockedBelowImport(ModuleID file, std::optional<ModuleID> caller);
    void noteHostImportCall(ModuleID, JSPromise*);
    static constexpr size_t minimumHostImportCallLimit = 16;
    size_t m_hostImportCallLimit { minimumHostImportCallLimit };
    Vector<std::pair<ModuleID, Strong<JSPromise>>, 2> m_hostImportCalls;
    void tellIfFactoryImportsAbsentExport(ModuleID requested);
    void forgetCallOfMock(ModuleID fileOrModule);
    bool isBeingEvaluated(ModuleID module) const { return status(module) == ModuleStatus::Evaluating || status(module) == ModuleStatus::EvaluatingAsync; }
    static const MockOperations& mockOperations();
    JSObject* capturedExports(ModuleID);
    JSValue capturedValue(ModuleID, const Identifier&, MockMode = MockMode::Use);
    bool hasCapturedExport(ModuleID, const Identifier&);
    bool isNativeModule(ModuleID) const;
    void forEachCapturedExport(ModuleID, const ScopedLambda<void(const Identifier&, JSValue)>&);
    static JSValue storedValue(JSGlobalObject*, JSObject* table, const Identifier& name);
    static void storeValue(JSGlobalObject*, JSObject* table, unsigned number, JSValue);
    static void removeValue(JSGlobalObject*, JSObject* table, unsigned number);
    JSObject* moduleNamespace(ModuleID, ModulePhase, bool shouldPreventExtensions = true);
    bool mayExport(ModuleID, UniquedStringImpl* name) const;
    JSObject* importMeta(ModuleID);
    JSObject* createImportMeta(JSString* key);
    JSObject* instanceImportMeta(ModuleID, Environment* instance);
    JSCell* builtinCell(BuiltinID, ModuleID);
    JSString* pathString(ModuleID);
    JSString* resolve(ModuleID, ResolvedSpecifierForm);
    const SourceOrigin& sourceOrigin(ModuleID);
    bool hasMadeOrigin(const SourceOrigin&) const;
    void throwFailure(ModuleFailureID, bool isForRequire);
    void throwNewFailure(ModuleFailureID, bool isForRequire);
    std::optional<ModuleFailureID> loadFailure(ModuleID module) const { return m_table.loadFailure(module); }
    std::optional<ModuleFailureID> findUnmockedFileFailure(ModuleID root) const;
    bool hasUnresolvableRequestInGraph(ModuleID root);
    void throwExportNotFound(const RequiredNameData&);
    bool dropExportThatFindsNothing(ModuleID importer, const RequiredNameData&);
    void throwExportAmbiguous(const RequiredNameData&);
    bool isRequestedByStarExport(const RequiredNameData&) const;
    String pathOfExporterAsLoaderNamesIt(const RequiredNameData&);
    void didRequestFromHost(ModuleID module)
    {
        if (!m_firstModuleRequestedByHost) [[unlikely]]
            noteFirstRequestFromHost(module);
        if (!m_mainEntryPoint && !m_isRunningPreloads) [[unlikely]]
            m_mainEntryPoint = module;
    }
    void setIsRunningPreloads(bool runsPreloads) { m_isRunningPreloads = runsPreloads; }
    std::optional<ModuleID> firstModuleRequestedByHost() const { return m_firstModuleRequestedByHost; }
    bool hasBeenImported(ModuleID) const;
    bool hasBeenImportedSinceReset(ModuleID) const;
    std::optional<uint32_t> firstImportSinceReset(ModuleID) const;
    bool hasPendingMockAfterReset(ModuleID) const;
    bool isPatchedLibraryModule(ModuleID module) const { return m_libraryModulesWithMock.get(module.index()) && !activeMock(module) && !m_modulesWithPendingMock.get(module.index()); }
    bool isUnevaluatedTextForm(ModuleID) const;
    bool isBeingFetched(ModuleID) const;
    bool isFetchedSynchronously(ModuleID) const;
    bool isImportResolvedByLoader(ModuleID) const;
    bool hasPlannedLoadReached(ModuleID importer) const;
    enum class ImportResolver : bool { Build, Loader };
    void runDataFileAgainForImport(ModuleID, ImportResolver);
    void reevaluateDataFile(ModuleID);
    void didImportFromUncompiledCode(ModuleID module, bool isRootOfImportCall)
    {
        if (data(module).kind != ModuleKind::Data)
            return;
        if (isRootOfImportCall && !recordIfExists(module))
            return;
        m_dataFilesImportedByUncompiledCode.set(module.index());
    }
    void didImportFileFromUncompiledCode(ModuleID moduleOrFacade);
    bool isEntryPointOfRealm(ModuleID module) const { return m_firstModuleRequestedByHost == module; }
    bool takeMainModuleLoadStep(ModuleID module) { return isEntryPointOfRealm(module) && !std::exchange(m_didTakeMainModuleLoadStep, true); }
    void willBeLoadedByImport(ModuleID module) { m_didTakeMainModuleLoadStep |= isEntryPointOfRealm(module); }
    bool isLoadedByLoader(ModuleID module) const { return data(module).isLoadedByLoader(); }
    bool takeMainPromiseSplit(ModuleID module) { return isEntryPointOfRealm(module) && !m_didTakeMainModuleLoadStep && !std::exchange(m_didSplitMainLoadPromise, true); }
    JSPromise* evaluateMainModule(ModuleID entryPoint, JSPromise* evaluation);
    bool mainModuleWaitsForItsImport() const { return !!m_capabilityOfMainModule; }
    void runMainModuleThatWaited();
    void failMainModuleThatWaited();
    bool rejectsUncompiledCode() const { return !m_admitsCodeOutsideImage || m_instance.vm->aotState().requesterRejectsUncompiledCode; }
    bool moduleRejectsUncompiledCode(ModuleID) const;
    const String* exchangeModuleRequester(const String* asker) { return std::exchange(m_moduleRequester, asker); }
    const String& requesterForRegistryKey(const String& key) const { return key.isEmpty() || m_table.findByPath(key) ? compiledCodeRequester() : key; }
    bool hasUncompiledRequester() const { return m_moduleRequester && m_moduleRequester->impl() != compiledCodeRequester().impl() && !isInHostRequest(); }
    void forEachTLASuspendedModule(const WTF::Function<void(int64_t turn, const String& key)>&);
    void evict(ModuleID, bool shouldRestoreIfRetained = false);
    void resetModulesAfterEvaluation(const WTF::Function<bool(const String&)>& isKept);
    void resetPendingModulesIfNeeded()
    {
        if (!m_modulesPendingReset.isEmpty()) [[unlikely]]
            resetPendingModules();
    }
    void forEachFailedModule(const WTF::Function<void(ModuleID)>&) const;
    void snapshotEvaluatedModules();
    void retainNewlyEvaluatedModules();
    void restoreRetainedModules(const WTF::Function<void(const String&)>&);
    void resetEntryPointEvaluation(ModuleID);
    bool hasResettablePackageFailure(ModuleID) const;
    bool hasFailedWithoutMock(ModuleID module) const { return hasResettablePackageFailure(module) && !m_originalsEvaluatedUnderMock.get(module.index()) && !errorOfModuleThatRanBesideMock(module); }
    bool hasPermanentFailureInPackage(ModuleID root);
    bool hasPermanentFailureInProject(ModuleID root);
    void resetPackageFailureIfNeeded(ModuleID);
    void resetFailedEvaluations();
    void makeESModuleReadyToRunAgain(ModuleID);
    void releaseInstance(ModuleID);
    bool mayReleaseModule(ModuleID);
    bool mayReleaseCommonJSFile(ModuleID file);
    bool isReexportedByUnlistedName(ModuleID facade);
    bool isHandedOnOverDynamicStar(ModuleID, uint32_t name) const;
    void releaseCommonJSFile(ModuleID file);
    bool tryReleaseCommonJSFileDroppedByLoader(ModuleID);
    bool keepCommonJSFileDroppedByLoader(ModuleID);
    void releaseExportsOfLoad(ModuleID facade);
    void setImportedPatch(ModuleID file, JSObject* mock);
    ModuleNamespace* ensurePatchWord(ModuleID target);
    bool endImportedPatch(ModuleID file);
    void forgetLoadOfEntry(ModuleID fileOrFacade);
    void noteDisplacedRunImporters(ModuleID file);
    void releaseDisplacedRunImporters(ModuleID file);
    bool isDisplacedRunImporter(ModuleID file, ModuleID importer) const;
    bool keepsDisplacedRun(ModuleID importer) const;
    bool reachesDisplacedRun(ModuleID module) const;
    bool isReloadable(ModuleID module) const { return hasInstances(module) || data(fileOf(module)).isReloadable(); }
    bool requiresOriginalUnderMock(ModuleID file, ModuleID requirer) const;
    using ModuleIndexSet = UncheckedKeyHashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    template<typename Value> using ModuleIndexMap = UncheckedKeyHashMap<uint32_t, Value, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    uint32_t requirerIndexOfOriginal(ModuleID file) const { return m_table.size() + file.index(); }
    uint32_t requirerIndex(ModuleID requirer) const;
    void noteRequireEdge(ModuleID target, ModuleID requirer);
    void forgetRequireEdge(ModuleID target, ModuleID requirer);
    JSValue answerOfActiveMock(ModuleID file) const;
    void didDeleteEntry(ModuleID);
    void noteRequireOfOriginal(ModuleID target, ModuleID requirer);
    bool hasRequiredOriginal(ModuleID requirer, ModuleID file) const;
    void forgetEdgesOfRequirer(uint32_t requirerIndex);
    void forgetCommonJSOriginal(ModuleID file);
    void didEvaluateAsOriginal(ModuleID file);
    const ModuleIndexSet* requirersOfEntry(ModuleID file) const;
    bool recordsRequireEdges(ModuleID requirer) const { return data(requirer).kind == ModuleKind::CommonJs && (data(requirer).isReloadable() || (m_hasBegunTestFile && isStartedByHost(requirer))); }
    bool isStartedByHost(ModuleID) const;
    void noteAnsweredRequire(ModuleID target, ModuleID requirer, JSValue answered);
    void noteRequestOfActiveMock(ModuleID target);
    bool runsTestFilesAgain() const { return host().runsTestFilesAgain && host().runsTestFilesAgain(); }
    void tellOfReleaseWhereTestFilesRunAgain(ModuleID file)
    {
        if (data(file).isReloadable() && status(file) == ModuleStatus::Evaluated && runsTestFilesAgain())
            tellOfKeptRun(file);
    }
    bool isOnCycleOfLoads(ModuleID file);
    void tellIfMockedFileWasKept(ModuleID target);
    void tellOfKeptRun(ModuleID file);
    void tellIfReexportedFileWasKept(ModuleID module);
    void noteRequireDuringPatch(ModuleID file);
    void reportReloadableFileKept(ModuleID module)
    {
        if (data(fileOf(module)).isReloadable())
            tellOfKeptRun(fileOf(module));
    }
    void didRepeatMockOfKeptFile(ModuleID module)
    {
        if (m_table.hasSourceStamps() && !hasInstances(module) && !isEvicted(module) && !isStartedByHost(module))
            tellThatFileDidNotRunAgain(module);
    }
    void noteKeptOriginal(ModuleID module, bool isAutomock)
    {
        ModuleID file = fileOf(module);
        if (data(file).isReloadable()) {
            if (isAutomock)
                tellOfKeptRun(file);
        } else if (m_table.hasSourceStamps())
            m_originalsKeptUnderMock.set(file.index());
    }
    void reportIfFileWasReleased(ModuleID module)
    {
        if (data(fileOf(module)).isReloadable() && m_mockedFilesWithReleasedRequirers.contains(fileOf(module))) [[unlikely]]
            tellOfKeptRun(fileOf(module));
    }
    void reportMockedFileKept(ModuleID module)
    {
        if (data(fileOf(module)).isReloadable() && hasIntactOriginal(fileOf(module)))
            tellThatFileDidNotRunAgain(fileOf(module));
    }
    void noteRefusedOriginal(ModuleID);
    void forgetRequireEdges(ModuleID target, bool areToMock);
    bool mayReleasePatchedModule(ModuleID);
    void releaseLoadedModule(ModuleID);
    void forgetPatch(ModuleID);
    bool isPatchedInPreload(ModuleID);
    bool tryReleaseModule(ModuleID);
    bool isReloadedByPendingImport(ModuleID);
    Vector<String> takeModulesReleasedSincePass();
    bool tryReleasePatchedModule(ModuleID);
    void releasePatchedFileAtMockEnd(ModuleID);
    void releasePatchedModuleOfCycle(ModuleID, bool isFactoryPending);
    void restoreReleasedPatch(ModuleID);
    void forgetUnchangedFilesAbove(ModuleID mocked);
    void releaseImportersOfEvictedMock(ModuleID);
    void forgetLoad(ModuleID);
    void noteImportInFlight(JSPromise*, ModuleID);
    bool hasMockRegisteredAfter(ModuleID, uint32_t moment) const;
    JSObject* takeReleasedImportNamespace(JSPromise*, ModuleID);
    bool tryRestartReleasedImport(JSPromise*);
    void restorePreloadInstances();
    void noteRestoredModule(ModuleID module) { m_restoredModules.set(module.index()); }
    Vector<ModuleID, 16> linkInstances(ModuleID root);
    void unlinkInstances(ModuleID root, std::span<const ModuleID>);
    BitVector findModulesLinkedBefore(ModuleID root, ModuleID importer) const;
    void noteLinkFailure(ModuleID root, ModuleID importer);
    void tellOfFailedLoad(ModuleID module, ModuleID target);
    bool wasLinkedBeforeTestFile(ModuleID) const;
    void linkFailedLoads();
    void tryLinkFailedLoads();
    void noteLinkedModules(ModuleID root);
    void noteFailedEvaluation(ModuleID root, ModuleID failed);
    void noteFailedLoaderLink();
    void linkInstances(std::span<const ModuleID>);
    struct EndOfLinkPath {
        Environment* instance { nullptr };
        ModuleID module;
    };
    using EndsOfLinkPaths = UncheckedKeyHashMap<std::pair<Environment*, uint32_t>, EndOfLinkPath>;
    EndOfLinkPath followLinkPath(ModuleID, Environment*, uint32_t firstStep, EndsOfLinkPaths&) const;
    void resolveLinks(std::span<const ModuleID>);
    void finishLinkForLoader(ModuleID root, LinkContext&);
    std::optional<ModuleID> m_rootLinkedByLoader;
    bool isReplacedByMock(ModuleID root) const;
    bool isLinkedToReleasedInstance(ModuleID importer, ModuleID target) const;
    bool isEvaluatedWithOriginal(ModuleID importer, ModuleID mocked) const;
    JSValue releasedInstanceError(ModuleID importer, ModuleID target) const;
    void noteReleasedFailure(ModuleID, ModuleRecord*);
    void restoreInstance(ModuleID, Environment*);
    struct CurrentInstance {
        Environment* instance { nullptr };
        bool isLinked { false };
        uint32_t linkMoment { 0 };
        JSValue guard { jsBoolean(true) };
    };
    CurrentInstance exchangeCurrentInstance(ModuleID, const CurrentInstance&);
    std::optional<ModuleID> originalNamedBy(const Identifier&) const;
    std::optional<ModuleID> compiledOriginalNamedBy(const Identifier& key);
    bool tryRestoreCompiledOriginal(JSModuleRecord*);
    void restoreCompiledOriginal(JSModuleRecord*);
    bool tryRestoreCompiledOriginalUnderMock(ModuleID);
    void tryBindOriginal(JSModuleRecord*);
    void noteOriginalsRequestedBySource(JSModuleRecord* importer);
    void bindOriginal(JSModuleRecord*, ModuleID);
    void linkOriginal(JSModuleRecord*);
    AbstractModuleRecord* findRecordRequestedByOriginal(JSModuleRecord*, ModuleID target) const;
    AbstractModuleRecord* findHostRecordRequestedByOriginal(JSModuleRecord*, ModuleID target) const;
    JSValue evaluateOriginal(JSModuleRecord*);
    void didFailOriginal(JSModuleRecord*);
    void noteSecondInstanceOfOriginal(ModuleID);
    void noteCompiledOriginal(ModuleID, OriginalRun);
    void noteOriginalsOfFailedLink(ModuleID importer);
    void didRejectRequire(const String& path);
    void willEndMockOfBuiltin(ModuleID builtin, MockEnd);
    void willImportOriginal(ModuleID file);
    void bindNamesRequiredByOriginal(ModuleID);
    void finishLinksOfOriginal(JSModuleRecord*);
    bool isOriginalLoadedAlone(ModuleID);
    bool importsMock(ModuleID) const;
    bool hasAnotherStarExport(ModuleID) const;
    bool isLoadedBelow(ModuleID, ModuleID root) const;
    void restoreUnmockedInstance(ModuleID);
    void restoreOriginalLeftByEarlierMock(ModuleID);
    void displaceUnmockedInstance(ModuleID);
    bool tryLeaveUnderActualKey(ModuleID);
    void clearUnmockedInstance(ModuleID);
    bool tryEvictUnderActualKey(ModuleID);
    void releaseImportersOfInstance(ModuleID, JSValue original);
    void releaseEndedMockImporters();
    JSObject* absentModuleNamespace(ModuleID external);
    bool releaseAbsentModuleImporters(const String& key, ReleasedImporters);
    bool isLinkedAndUnevaluated(ModuleID) const;
    void resetLinkedInstances(const WTF::Function<bool(const String&)>& isKept);
    bool releaseModuleWithImporters(ModuleID);
    void setReplacedTestFile(std::optional<ModuleID> testFile) { m_replacedTestFile = testFile; }
    void setRestoredAtUnmock(ModuleID module, bool isRestored) { m_originalsRestoredAtUnmock.set(module.index(), isRestored); }
    bool isLinkedInstance(ModuleID, const Environment*) const;
    bool reexportsFrom(ModuleID, ModuleID importer, ModuleID imported) const;
    bool hasEvaluatedImporterWithoutInstances(ModuleID);
    bool areReadByImportersWithoutInstances(std::span<const ModuleID>);
    bool isAwaitedByDetachedLoad(ModuleID) const;
    void noteLoadsOfEndedTestFile();
    bool hasFixedNamespace(ModuleID module) const { return m_linkedInstances.get(module.index()) && hasInstances(module); }
    std::optional<bool> isLinkedToCurrentEntry(ModuleID importer, ModuleID target) const;
    std::optional<bool> isLinkedToCurrentEntry(ModuleID importer, ModuleID target, Environment* instance) const;
    Environment* originalBesideMock(ModuleID) const;
    void forgetUnmockedOriginal(ModuleID);
    bool tryDisplaceFailedOriginal(ModuleID);
    JSObject* instanceNamespace(ModuleID, Environment*, ModulePhase);
    bool keepsOriginalNames(const ModuleNamespace&) const;
    struct ResolvedField {
        Environment* instance { nullptr };
        SlotData slot;
        ModuleNamespace* mockWord { nullptr };
        uint32_t nameInMock { 0 };
        bool mayChange { false };
        bool isFromStarExport { false };
        std::optional<std::pair<ModuleID, unsigned>> exportWithoutLinks { };
    };
    JSValue readField(const Environment* instance, SlotData) const;
    ResolvedField resolveField(ModuleID, unsigned exportIndex, Environment* instance) const;
    struct ResolvedNamespace {
        std::optional<ModuleID> target;
        Environment* instance { nullptr };
        ModuleNamespace* mockWord { nullptr };
        std::optional<uint32_t> nameInMock;
        bool isFromStarExport { false };
        std::optional<std::pair<ModuleID, unsigned>> original { };
        JSObject* absent { nullptr };
    };
    ResolvedNamespace resolveNamespace(ModuleID, unsigned exportIndex, Environment* instance) const;
    struct ResolvedVariable {
        bool isFollowed { false };
        ModuleNamespace* mockWord { nullptr };
        uint32_t nameInMock { 0 };
        bool isFromStarExport { false };
        std::optional<std::pair<ModuleID, unsigned>> original { };
        Environment* starHolder { nullptr };
        ModuleID holderModule { };
        uint32_t nameInHolder { 0 };
        bool operator==(const ResolvedVariable&) const = default;
    };
    ResolvedVariable resolveVariable(ModuleID, unsigned exportIndex, Environment* instance) const;
    bool isLinkedReexport(ModuleID module, unsigned exportIndex) const { return hasInstances(module) && m_table.findReexportedField(module, exportIndex); }
    JSValue readLinkedMock(ModuleNamespace* word, uint32_t name) { return readLinkedMock(word, identifier(name)); }
    JSValue readLinkedMock(ModuleNamespace* word, const Identifier& name);
    bool hasOutdatedLinkForName(ModuleID, const Identifier& name, Environment* instance) const;
    bool isReadThroughStars(ModuleID, unsigned exportIndex, Environment* instance) const;
    JSValue readThroughStars(ModuleID, uint32_t name, Environment* instance);
    JSValue readMockOrStar(ModuleID, unsigned exportIndex, Environment* instance, ModuleNamespace* word, uint32_t nameInMock, bool isFromStarExport);
    struct StarResolution {
        JSObject* source { nullptr };
        ModuleNamespace* mockWord { nullptr };
        ModuleID module;
        unsigned exportIndex { 0 };
        Environment* instance { nullptr };
        bool isInStarCycle { false };
    };
    void fixMockNames(ModuleNamespace* word);
    bool addStarExportNames(ModuleID, const BitVector& onCycle, PropertyNameArrayBuilder&, BitVector& seen);
    BitVector modulesOnImportCycle(ModuleID) const;
    bool hasNameInMock(ModuleNamespace* word, const Identifier& name);
    bool hasNameInMockOutsideCycles(ModuleNamespace* word, const Identifier& name);
    std::optional<StarResolution> resolveThroughStars(ModuleID, const Identifier& name, Environment* instance, BitVector& seen);
    std::optional<StarResolution> followStars(ModuleID, const Identifier& name, Environment* instance);
    bool hasOutdatedStarLink(ModuleID, Environment* instance) const;
    void didChangeLinkState() const { ++m_linkStateVersion; }
    bool reexportsNameOf(ModuleID, const Identifier& name, ModuleID source) const;
    void didReplaceActiveMock(ModuleID file, JSObject* previous, JSObject* current, bool isSameEntry);
    void clearMockWord(ModuleID fileOrModule);
    void clearPendingMockState(ModuleID file);
    void discardFailedEvaluation(ModuleID);
    void saveFailedModuleObject(ModuleID);
    void detachFailedModuleObject(ModuleID);
    void forgetWhatFileRequired(ModuleID);
    void didMakeTemplateObject(ModuleID, uint32_t offsetOfCell);
    void forgetRunOfLibraryModule(ModuleID);
    JSValue executeForLoader(ModuleID);
    bool wouldBeEvicted(ModuleID);
    bool isLoadInFlight(ModuleID module) const { return status(fileOf(module)) == ModuleStatus::EvaluatingAsync; }
    bool wasLoadedByImport(ModuleID);
    bool mayBeEvicted(ModuleID module) const
    {
        const ModuleData& file = data(fileOf(module));
        return (file.kind != ModuleKind::SourceText && file.kind != ModuleKind::CommonJs) || file.mayBeEvicted();
    }
    enum class DelegatedBy : bool { Host, RequireCache };
    void delegateToHostUnlessEvaluated(ModuleID, DelegatedBy = DelegatedBy::Host);
    void takeOutOfLoader(ModuleID);
    bool hasNoEntryBehindMock(ModuleID);
    void retainMockForLinkedImporters(ModuleID);
    void noteEndOfMockOverFileThatNeverRan(ModuleID);
    void noteWhetherProgramStoredFalsyEntry(ModuleID module, bool didStore) { m_keysWithFalsyEntryOfProgram.set(fileOf(module).index(), didStore); }
    void forgetFormAnsweredFirst(ModuleID file);
    JSObject* requireCacheEntry(ModuleID);
    bool isEvicted(ModuleID module) const { return m_evictedFiles.get(fileOf(module).index()); }
    bool hasRun(ModuleID);
    bool hasRun(ModuleID, bool& importsAreFollowed);
    bool isListedBeforeItHasRun(ModuleID);
    bool hasModuleObjectInRequireCache(ModuleID) const;
    struct FirstRequire {
        ModuleID requirer;
        ModuleID required;
    };
    std::span<const FirstRequire> firstRequires() const LIFETIME_BOUND { return m_firstRequires.span(); }
    void forgetRequestByLoader(ModuleID file);
    void retainModuleObjectFromRequire(ModuleID, JSObject* moduleObject);
    void releaseModuleObjectFromRequire(ModuleID);
    void answerRequireWithNamespaceOfFile(ModuleID module)
    {
        if (data(module).kind != ModuleKind::CommonJs || data(module).isOfLibrary())
            return;
        LoadedModule& loaded = ensureLoaded(module);
        loaded.isAnsweredWithNamespace = true;
        loaded.requiredValue.set(*this, JSValue());
        loaded.moduleObjectFromRequire.set(*this, nullptr);
        loaded.exportsGivenToObjectMadeByRequire.set(*this, JSValue());
    }
    void answerRequireWithExportsOfFile(ModuleID module)
    {
        if (LoadedModule* loaded = loadedIfExists(module))
            loaded->isAnsweredWithNamespace = false;
    }
    bool holdsCodeThatMayBeEvicted();
    BitVector collectEvaluatedModules();
    bool delegatesToHost(ModuleID);
    bool delegatesWithoutPlugins(ModuleID);
    bool isRequiredModulePatchPending(ModuleID);
    std::optional<JSPromise*> importOriginalUnderPatch(ModuleID, ModuleID referrer, JSValue options);
    bool isPatchPending(ModuleID file) const;
    bool isEvaluatedUnderPendingPatch(ModuleID);
    void tellIfFactoryIsRunning(ModuleID requested, LoadKind);
    void tellIfPatchIsPending(ModuleID required);
    bool isRequireUnderPendingMock(ModuleID required, ModuleID requirer);
    bool requiresPatch(ModuleID, JSObject*& exports) const;
    bool wasModifiedSinceBuild(StringView path) const;
    bool wasWrittenSinceBuild(ModuleID file) const;
    bool wasDelegatedByHost(ModuleID file) const { return m_filesDelegatedByHost.get(file.index()); }
    bool isImportedDataURL(StringView url) const;
    bool rejectsLoadsByName() const { return !m_loadsModulesByName && !m_runsUncompiledCode && !m_hostRunsCopiesFromDisk && !isInHostRequest(); }
    bool isInHostRequest() const { return m_currentHostRequest && !isHostRequestStale(); }
    bool isHostRequestStale() const;
    std::optional<ModuleID> callingModule() const;
    bool isCompiledModuleHostRequest() const { return m_currentHostRequest.isForCompiledModule() && isInHostRequest(); }
    bool isMainEntryPoint(ModuleID module) const { return data(module).isEntryPoint() && (!m_mainEntryPoint || m_mainEntryPoint == module); }
    bool refusesRequestFromHost(ModuleID module) const { return (data(module).hasUnescapedExports() && !isMainEntryPoint(module)) || (valueOfLibraryFileStaysWithCompiledCode(module) && !isCompiledModuleHostRequest()) || refusesRequestFromHostOutsideWindows(module); }
    bool valueOfLibraryFileStaysWithCompiledCode(ModuleID module) const { return data(module).registryID() && m_table.valueOfRuntimeSourceStaysWithCompiledCode(*data(module).registryID()); }
    bool refusesRequestFromHostOutsideWindows(ModuleID module) const { return !m_admitsInspector && !data(module).isEntryPoint() && data(module).kind != ModuleKind::Data && !data(module).bodyThrowsAtLoad() && m_table.hasFilePath(module) && !data(module).isLibraryCodeWithFilePath() && !m_runsCopyFromDisk && !isCompiledModuleHostRequest(); }
    bool isRootLoadedByCompiledCode(ModuleID module) const { return m_rootLoadedByCompiledCode && fileOf(*m_rootLoadedByCompiledCode) == fileOf(module); }
    bool refusesOriginalsBesideMocks() const { return m_refusesOriginalsBesideMocks; }
    void refuseToShowExportsOf(ModuleID);
    void refuseToHandOutExportOf(ModuleID, StringView name);
    std::optional<std::pair<ModuleID, unsigned>> exportThatStaysInBehindIndirectEntries(ModuleID, unsigned index);
    enum class OriginalRequester : uint8_t { Statement, ImportCall };
    bool importsOriginalDespiteMock(ModuleID mocked, ModuleID importer, OriginalRequester = OriginalRequester::Statement) const;
    bool keepsOriginalAfterRemoval(ModuleID mocked, ModuleID importer) const;
    bool leadsToWhatIsImported(ModuleID imported, ModuleID importer) const;
    bool mayGetNameFromMock(ModuleID exporter, ModuleID importer) const { return !data(exporter).isESModuleOfProgram() || data(exporter).isFacade() || data(exporter).hasDynamicStarExport() || seesMockOf(exporter, importer); }
    bool isOfPackage(ModuleID) const;
    mutable BitVector m_modulesRequestedAboutPackage;
    mutable BitVector m_modulesOfPackages;
    bool isExternalizedPackage(ModuleID) const;
    bool wasLoadedUnderActualKey(ModuleID fileOrModule) const;
    bool keepsOriginalAfterReplacement(ModuleID fileOrModule, ModuleID importer) const;
    bool isLinkedToOriginalOf(ModuleID fileOrModule, ModuleID importer) const;
    void keepUnderActualKey(ModuleID fileOrModule);
    void noteWhereOriginalLiesBehindEndOfMock(ModuleID fileOrModule);
    bool hasBypassingImporterBelow(ModuleID root, ModuleID moduleOrFacade) const;
    bool hasUnevaluatedImporterInGraph(ModuleID root, ModuleID file) const;
    bool isDisplacedRunKept(ModuleID facade, ModuleID root) const;
    bool wasOriginalEvaluated(ModuleID mocked) const;
    bool wasOriginalEvaluatedUnderMock(ModuleID module) const { return m_originalsEvaluatedUnderMock.get(module.index()); }
    bool isMockedByFactoryThatTakesOriginal(ModuleID) const;
    std::optional<ModuleID> commonJSOriginalNamedBy(const Identifier& key);
    bool hasLoadedCommonJSFile();
    bool tryEvaluateCommonJSOriginal(const Identifier& key, const ArgList&);
    void noteOriginalsWithLoadedImporter(ModuleID imported, ModuleID referrer);
    void willReplaceMock(ModuleID);
    JSValue takeArgumentOfOriginal(ModuleID, unsigned index);
    bool createsOwnNamespace(ModuleID module) const { return m_moduleWhoseOwnNamespaceIsMade == module; }
    bool getsOriginalBesideMock(ModuleID, ModuleID referrer);
    bool requireBypassesMock(ModuleID mocked, ModuleID requirer);
    void captureEvaluatingRequirer(ModuleID requirer, ModuleID target);
    void swapEarlyCapture(ModuleID facade);
    void setMockInPlace(ModuleID file, JSObject* mock);
    JSValue answerRequireWithOriginal(ModuleID file, ModuleID referrer);
    void runFileBesideMock(ModuleID file);
    void runOriginalsOwedTo(ModuleID importer);
    void evaluatePastPendingMock(ModuleID facade);
    void keepOriginalIntact(ModuleID file);
    JSValue requireOriginalUnderMock(ModuleID);
    void detachRefusedOriginalLoad(ModuleID);
    BitVector importClosureOfOriginal(ModuleID) const;
    JSPromise* pendingFactoryInOriginalGraph(ModuleID, JSValue& state);
    JSValue answerOwnFactoryRequire(ModuleID);
    bool hasOnlyBeenRequired(ModuleID file);
    void willReplaceRequiredDataFile(ModuleID);
    void willImportEvictedDataOriginal(ModuleID);
    bool wasOriginalRequiredFirst(ModuleID file)
    {
        if (data(file).requireResult != RequireResult::NamespaceIfImported)
            return false;
        if (m_evictedDataOriginals.clear(file.index()) || (isEvictedDataFile(file) && !m_importedDataOriginals.get(file.index()))) [[unlikely]]
            m_originalsEvaluatedByRequire.set(file.index());
        return m_originalsEvaluatedByRequire.get(file.index());
    }
    String pathOfRefusedRequire(ModuleID) const;
    uint64_t pendingFactoryCallID(ModuleID, bool* shouldRejectEveryRequire = nullptr) const;
    bool hasPendingFactoryFromLoad(ModuleID) const;
    void resolveUncompiledImports(ModuleID required);
    void recordRejectedRequire(ModuleID required, bool wasRejected = true);
    JSValue answerRequireWhileFactoryHasNotSettled(ModuleID target, ModuleID requirer);
    void collectImportersBetween(ModuleID root, ModuleID mocked, Vector<std::pair<ModuleID, ModuleID>>& importers);
    bool isFactoryChainImport(ModuleID, ModuleID referrer);
    JSPromise* deferFactoryChainImport(ModuleID, ModuleID referrer);
    void markOriginalDue(ModuleID);
    void retainRecordForInFlightLoads(ModuleID);
    bool isRetainedByInFlightLoads(ModuleID module) const { return m_modulesRetainedByInFlightLoads.get(module.index()); }
    void evaluateModuleItself(ModuleID);
    bool isRequestedByOwnFactory(ModuleID);
    bool isRequestedDuringOwnPatch(ModuleID);
    void reportPendingOwnPatches(ModuleID root);
    void reportPendingFactoriesInGraph(ModuleID root);
    void tellOfFactoriesInCycle(ModuleID);
    bool tellIfMockedDuringFetch(ModuleID);
    BitVector awaitedMocksBelow(ModuleID, bool inCycleAlone);
    bool mayImportAwaitItself(ModuleID);
    bool hasPendingPatchInGraph(ModuleID root);
    bool isImportedByOwnFactory(ModuleID) const;
    bool tryDeferOwnPatch(ModuleID);
    bool isFileRequestedByOwnFactory(ModuleID file);
    JSPromise* importLoadedOriginal(ModuleID file);
    void noteOriginalRequiredFromSource(ModuleID file) { m_originalsRequiredFromSource.set(file.index()); }
    void noteLoadBegunByFactories(ModuleID root);
    void notePrivateCopy(ModuleID, ModuleID mocked);
    BitVector m_modulesWithPrivateCopy;
    BitVector m_modulesSharedWithFactories;
    BitVector m_mocksCalledInTestFile;
    UncheckedKeyHashMap<ModuleID, uint32_t> m_factoriesCalledWhenNamed;
    uint32_t m_factoriesCalledByLoads { 0 };
    BitVector m_mocksCalledAgain;
    bool runsBesideMockFor(ModuleID mocked, std::optional<ModuleID> importer) const;
    void setAsideErrorOfModuleThatRanBesideMock(ModuleID);
    void bringBackErrorOfModuleThatRanBesideMock(ModuleID);
    JSValue importModuleItselfBesideMock(ModuleID, JSValue answerOfImport);
    JSValue resolveImportWithOriginal(ModuleID);
    JSValue resumeFactoryChainImport(ModuleID, ModuleID referrer);
    JSObject* namespaceOfModuleThatRanBesideMock(ModuleID);
    void restoreNamespaceOfOriginal(ModuleID);
    void willRunCopyFromDisk()
    {
        m_runsCopyFromDisk = true;
        m_hostRunsCopiesFromDisk = true;
    }
    void admitCodeNamedBy(JSValue specifier);
    void abandonEvaluation(ModuleID, JSValue error);
    template<typename Visitor> void visitRoots(Visitor&);

    bool loadRuntimeModules(ModuleID root);
    void link(ModuleID root);
    void captureSyntheticExports(ModuleID root, VisitedModuleRecords&);
    void finishLink(ModuleID root, LinkContext&);

    JSPromise* evaluate(ModuleID root, std::optional<int64_t> referrerAsyncOrder, JSPromise* importPromise, const ModuleLinkWalk* = nullptr, bool isLinked = false);
    void evaluateSynchronously(ModuleID, SynchronousEvaluationReason);
    void markRefusedEvaluation(ModuleID, int64_t firstOrder);
    void leaveStepsOfLoaderToJobs();
    void startDetachedLoad(ModuleID required, bool isForOriginal = false);
    void willRejectRequireAgain(ModuleID asked, ModuleID above);
    BitVector modulesInRequireGraph(ModuleID root) const;
    bool isStillBlockedOnFactories(ModuleID required) const;
    void innerModuleEvaluation(ModuleID root, EvaluationContext&);
    void gatherAsynchronousTransitiveDependencies(ModuleID, OrderedHashSet<AbstractModuleRecord*>& result, VisitedModuleRecords& seen);
    bool readyForSyncExecution(ModuleID, VisitedModuleRecords& seen);
    bool isOnCycleOfImports(ModuleID module) const { return data(module).hasExportTable() && !data(module).isSimple(); }
    bool mayWaitForItsCycle(ModuleID module) const { return storedStatus(module) == StoredModuleStatus::Evaluated && isOnCycleOfImports(module) && m_instance.modules.evaluatingMark != static_cast<uint32_t>(StoredModuleStatus::Evaluating); }
    struct ModulesThatWaitForCycles {
        BitVector cyclesLookedAt;
        BitVector modules;
    };
    bool waitsForItsCycle(ModuleID, ModulesThatWaitForCycles&) const;
    void didChangeStatusInRecord(ModuleID module)
    {
        m_recordsNotSeenToHaveRun.set(module.index());
        m_recordsNotSeenEvaluated.set(module.index());
    }
    void forEachUnsettledModule(NOESCAPE const Invocable<void(ModuleID, ModuleStatus)> auto&) const;
    void lookAtCycleOf(ModuleID, ModulesThatWaitForCycles&) const;
    const CyclesOfImports& cyclesOfImports() const;
    const ImportersOfModules& importersOfModules() const;
    const StatementImporters& statementImporters() const;
    const StatementImporters& importersOfEveryImport() const;
    const Vector<ModuleID>& uncompiledModules() const;
    JSValue callBodyOfModuleEnteredByLoader(ModuleID module) { return callBodyUnderOwnMark(module, false); }
    void evaluateModuleInRecord(ModuleID);
    bool beginModuleInRecord(ModuleID);
    bool evaluateImportedCommonJSModules(ModuleID root, EagerLoadJobPolicy = EagerLoadJobPolicy::KeepQueued, ErrorOfImportedFile = ErrorOfImportedFile::IsThrown, ModuleGraphLoadingState* = nullptr, PendingFactoryModules = PendingFactoryModules::Evaluate);
    bool wasRequestedByOuterLoad(ModuleID);
    Vector<uint32_t, 16> placesOfFilesAsSynchronousLoadMakesThem(ModuleID root, const Vector<uint32_t, 16>& places);
    AbstractModuleRecord* recordForRequestedModule(ModuleID);
    void resumeDeferredJobs();
    void transferGraphsToLoader();
    void didLoadGraph(ModuleID root);
    void didFailToLoadGraph(ModuleID root);
    bool tryDeferForEarlierImports(ModuleRecord&, JSCell* payload, bool isDeferred);
    bool tryDeferForFactories(ModuleRecord&, JSCell* payload);
    void resumeGraphsDeferredForImports();
    void countLoaderStep() { ++m_loaderStepCount; }
    void didSettlePromiseOfImport(JSPromise* promise) { m_loadsInFlight.didSettle(promise); }
    void keepFailedImportError(JSPromise* settled);
    BitVector m_mockEntriesToEvict;
    void runFileThatWaitsForItsTurn(ModuleID);
    bool isAskedForBehindLoadInFlight(ModuleID moduleOrFacade);
    void recaptureExports(ModuleID);
    void takeCopiesAnewWhereMockCallAloneTookThem(ModuleID facade);
    void captureExportsAgainKeepingWhatGettersGave(ModuleID facade);
    JSObject* standInWhoseGettersAreTheirCopies(JSValue exports, JSObject* copies);
    bool forgetCapturedExports(ModuleID);
    void exportWhatMockNamesAlone(ModuleID file, bool hasRun);
    bool exportsWhatMockNamesAlone(ModuleID) const;
    void fillNamesThatMockLacks(ModuleID file);
    void answerFirstRequireBehindMock(ModuleID file);
    void noteLoadBehindMock(ModuleID moduleOrFacade, bool isRequired);
    bool wasRequiredUnderActiveMock(ModuleID file) const;
    bool wasImportedUnderActiveMock(ModuleID file) { return ensureLoaded(moduleWithExportsOf(file)).wasImportedUnderActiveMock; }
    ActiveMockUse activeMockUse(ModuleID) const;
    void keepActiveMockUntilPatch(ModuleID, ActiveMockUse);
    bool isRunningFilesOf(ModuleID root) const
    {
        for (const EagerLoad* files = m_innermostEagerLoad; files; files = files->outer) {
            if (files->root == root)
                return true;
        }
        return m_rootWhoseFilesRun == root;
    }
    bool isRunningFilesAheadOfLoader(ModuleID root) const
    {
        for (const EagerLoad* files = m_innermostEagerLoad; files; files = files->outer) {
            if (files->root == root)
                return !files->isTransferredSynchronousLoad();
        }
        return m_rootWhoseFilesRun == root;
    }
    bool isInTransferredSynchronousLoad() const
    {
        for (const EagerLoad* files = m_innermostEagerLoad; files; files = files->outer) {
            if (files->isTransferredSynchronousLoad())
                return true;
        }
        return false;
    }
    bool stopLoadingBehindFile(ModuleID file);
    void evaluateLoadedCommonJSFile(ModuleID);
    void generateNativeModuleForLoader(ModuleID);
    void evaluateFilesOfLevel(ModuleID moduleOnWay, bool runsWhenLoaded);
    bool isCommonJSModuleOrFacade(ModuleID module) const
    {
        const ModuleData& moduleData = data(module);
        return moduleData.kind == ModuleKind::CommonJs || moduleData.isFacade();
    }
    bool endsFilesThatRunAhead(const ModuleRequestData& request) const { return !request.isHoisted || data(request.targetModule()).kind == ModuleKind::SourceText; }
    bool isFileOfProgram(ModuleID module) const { return !data(module).isOfLibrary() && (data(module).kind == ModuleKind::SourceText || data(module).kind == ModuleKind::CommonJs); }
    bool isLoadedWithRequests(ModuleID module) const { return isFileOfProgram(module) && data(module).kind == ModuleKind::SourceText; }
    bool isImportedByLoader(ModuleID module) const { return isFileOfProgram(m_table.moduleBehindFacade(module).value_or(module)) || (data(module).kind == ModuleKind::Data && !data(module).isOfLibrary()); }
    bool isBuiltinModuleOfLoader(ModuleID) const;
    bool isBuiltinModuleNewToLoader(ModuleID) const;
    enum class LoaderForRequire : uint8_t { IsNotNeeded, ComesBehindImportedFiles, IsNeeded };
    LoaderForRequire loaderForRequire(ModuleID);
    bool canTellHowGraphIsLoaded(ModuleID root);
    void appendRequestsForLoader(ModuleID, ModuleRecord*, ModuleGraphLoadingState&);
    std::optional<ModuleFailureID> statementThatEndsLoad(ModuleID) const;
    bool wasAbandonedByFailedLoad(ModuleID) const;
    void appendUnresolvableRequest(ModuleFailureID statement, ModuleRecord*);
    void keepFailureOfRequest(ModuleID, ModuleRecord*, const Identifier& name);
    ModuleRecord* appendRequestsOfUncompiledCode(ModuleID, ModuleRecord*);
    struct RequestsOfUncompiledCode;
    void collectRequestsOfUncompiledCode(ModuleID, RequestsOfUncompiledCode&);
    void collectRequestsBelow(ModuleID, RequestsOfUncompiledCode&);
    bool importsUnloadedCode(ModuleID);
    RefPtr<ScriptFetchParameters> fetchParametersOf(ModuleID external) const;
    ModuleRegistryEntry* registryEntry(const Identifier& key, const ScriptFetchParameters*);
    bool comesToCodeThatDoesNotLoad(ModuleID root);
    bool setIsWalkingRequests(bool isWalkingRequests) { return std::exchange(m_isWalkingRequests, isWalkingRequests); }
    unsigned numberOfGraphsBeingLoaded();
    bool comesToLoadInFlight(ModuleID required);
    bool hasNotBegunAsFile(ModuleID) const;
    void markModulesBelowWalkedRoot(ModuleID root);
    enum class SettlingOfLoad : bool { IsToldByLoader, IsLookedFor };
    void didStartLoad(JSPromise*, ModuleID root, SettlingOfLoad = SettlingOfLoad::IsToldByLoader);
    enum class LoadOfFile : uint8_t { IsAskedFor, IsRequested, HasComeToFile, IsRootOfImport };
    ModuleRecord* recordOfCommonJSFile(ModuleID, LoadOfFile = LoadOfFile::HasComeToFile);
    void noteLoadOfFile(ModuleID moduleOrFacade, LoadOfFile = LoadOfFile::HasComeToFile);
    bool shouldDeferAsyncLoad(ModuleID moduleOrFacade) const;
    bool importsFileBeingEvaluated(ModuleID root) const;
    enum class EvaluatedModules : bool { Visit, Skip };
    bool anyImportedModule(ModuleID root, EvaluatedModules, const Invocable<bool(ModuleID)> auto&) const;
    bool importsEagerlyEvaluatingModule(ModuleID root) const;
    bool importsEvaluatingCommonJSModule(ModuleID root) const;
    bool isLinkStepBlocked(const VM::SynchronousModuleTask&, std::optional<ModuleID> finishedModule) const;
    bool tryDeferLinkStep(const VM::SynchronousModuleTask&);
    bool resumeDeferredLinkSteps(std::optional<ModuleID> finishedModule);
    void evaluateEagerLoadRoot(ModuleID root);
    std::optional<ModuleID> findEvaluatingCommonJSModule(const Identifier& key);
    bool isModuleBeingTransferred(const Identifier& key) { return m_moduleBeingTransferred && hasKey(*m_moduleBeingTransferred, key); }
    bool wasRequestedByEnclosingLoad(const Identifier& key);
    bool isBlockedOnModuleTransfer(const Identifier& key, bool& shouldRequeueStep);
    JSPromise* deferRequest(ModuleID file, ModuleLoadingContext*, bool useImportMap);
    bool hasDeferredLoads() const { return !m_deferredRequests.isEmpty() || !m_deferredImports.isEmpty(); }
    void didRequestModule(const Identifier& key, JSCell* payload);
    void didSettleLoad(const ModuleGraphLoadingState&);
    void didFailToMakeModule(const Identifier& key);
    bool isNotLoaded(ModuleID);
    bool isNotLoadedWithoutStatus(ModuleID, const ModuleData&);
    bool isLoadedModuleWithoutRecord(ModuleID) const;
    bool wasLoadedWithEarlierGraph(ModuleID);
    bool wasLoadedWithGraph(ModuleID) const;
    ALWAYS_INLINE void pushHostRequest(bool isInHostRequest) { m_savedHostRequests.append(std::exchange(m_currentHostRequest, HostRequest(isInHostRequest))); }
    void popHostRequest() { m_currentHostRequest = m_savedHostRequests.takeLast(); }
    bool isStatementHostRequest() const { return m_isStatementHostRequest; }
    bool setIsStatementHostRequest(bool isAsking) { return std::exchange(m_isStatementHostRequest, isAsking); }
    bool wasImportedDuringEvaluation(ModuleID file) const;
    bool loaderHasFileThatThrew(ModuleID file);
    bool isAwaitedByImport(ModuleID file);
    bool isAwaitedByImportFromBeforeItRan(ModuleID file);
    void dropErrorOfFile(ModuleID moduleOrFacade);
    void didDiscardModuleError(const Identifier& key);
    void evaluateCommonJSForLoader(ModuleID file);
    JSValue execute(ModuleID, ReadsExports = ReadsExports::Yes);

    enum class RequiredBy : uint8_t { Referrer, Program };
    JSValue require(ModuleID, ModuleID referrer = { }, RequiredBy = RequiredBy::Referrer, JSObject* staleRequirerObject = nullptr, ReadsExports = ReadsExports::Yes);
    ModuleID formThatRequireFinds(ModuleID, bool& answersNamespace);
    bool wasImportedByStatement(ModuleID) const;
    bool hasEvaluationPassedImport(ModuleID importer, ModuleID target) const;
    void didMockEvictDataModule(ModuleID);
    void didEvictDataFile(ModuleID);
    void setMockedAfterEviction(ModuleID file, bool isMockedAfterEviction) { m_mockedEvictedDataFiles.set(file.index(), isMockedAfterEviction); }
    bool isEvictedDataFile(ModuleID file) const { return m_evictedDataFiles.get(file.index()); }
    void didLoadDataFile(ModuleID file, LoadKind way)
    {
        if (m_hasEvictedDataFiles) [[unlikely]]
            didLoadDataFileSlow(file, way);
    }
    void didLoadDataFileSlow(ModuleID, LoadKind);
    bool wasEvictedByRemovedMock(ModuleID) const;
    void evaluateCommonJS(ModuleID);
    JSValue commonJSExports(ModuleID) const;
    JSValue exportsGivenBackToCompiledCode(ModuleID) const;
    JSValue exportsSeenByLoader(ModuleID);
    void stopWhereExportsLeaveCompiledCode(ModuleID) const;
    void setCommonJSExports(ModuleID module, JSValue exports)
    {
        stopWhereExportsLeaveCompiledCode(module);
        setValue(module, exports);
    }
    void mockWhatRequireAnswered(ModuleID, JSObject* exports);
    void mockWhatRequireCacheHolds(ModuleID, JSObject* exports);
    void releaseMockInRequireCache(ModuleID);
    enum class RequireResolver : bool { CompiledCode, Runtime };
    template<RequireResolver> void recordRequire(ModuleID, ModuleID referrer, bool isFromCreatedRequire = false, JSObject* staleRequirerObject = nullptr, bool isAfterLoad = false);
    JSObject* findStaleRequirerObject(ModuleID file, JSValue moduleObjectOrRequire);
    static bool isCreatedRequireFunction(JSValue);
    void didMaterializeModuleObject(ModuleID);
    void noteExportsStoredThroughModuleObject(ModuleID, JSValue exports);
    bool findWhetherExportsVariableIsTakenOffModuleObject(ModuleID);
    void lookAtExportsOfModuleObject(ModuleID, RequireResolver);
    void readExportsOffModuleObjectIntoVariable(ModuleID);
    void recordRequireFromUncompiledCode(ModuleID, JSObject* requirerModuleObject);
    bool wasLoadedSynchronously(ModuleID file);
    bool areLoaderFunctionsStored() const { return !m_storedLoaderFunctions.isEmpty(); }
    void recordRequireOverride(RequireOverride function, bool isStored) { m_storedLoaderFunctions.set(function, isStored); }
    bool wasFirstLoadedSynchronously(ModuleID file);
    bool hostLoadsSynchronously(ModuleID file) const;
    bool isFetchedInsideCall(ModuleID file) const;
    bool isOfKindWhoseTextHostKeeps(ModuleID file) const;
    bool delegatesToStartingHost(ModuleID);
    void noteTextsThatHostKeeps();
    void forgetFirstLoad(ModuleID);
    void willLoadCopyFromDisk(ModuleID, LoadKind);
    JSObject* moduleObject(ModuleID);
    std::optional<OptionSet<ModuleObjectTrait>> newModuleObjectTraits(ModuleID);
    void adoptModuleObject(ModuleID, const ModuleObjects&);
    void releaseModuleObject(ModuleID);
    JSObject* moduleObjectWithExportsVariable(ModuleID) const;
    void didBindExportsVariable(ModuleID);
    JSObject* mainModuleObject(ModuleID);
    JSObject* parentModuleObject(ModuleID);
    JSObject* childModuleObject(ModuleID);
    std::optional<ModuleID> moduleFormOf(ModuleID file) const;
    std::optional<ModuleID> loadedModuleForm(ModuleID file);
    bool canExposeModuleObject(ModuleID module) const { return data(module).kind != ModuleKind::CommonJs || data(module).hasValue(); }
    bool hidesModuleObject(ModuleID module) const { return !canExposeModuleObject(module) || data(module).hasUnescapedExports(); }
    bool forEachNewChild(ModuleID, const ScopedLambda<void(JSObject*)>&);
    JSObject* prototypeOfModuleObjects() const { return m_prototypeOfModuleObjects; }
    void clearNewChildren(ModuleID);
    enum class NamedByBuild : uint8_t { No, CompiledModule, UncompiledModule };
    JSValue requireSpecifier(JSValue specifier, ModuleID referrer, NamedByBuild, JSObject* staleRequirerObject = nullptr);
    JSObject* takeHostRequestRequirerObject() { return std::exchange(m_hostRequestRequirerObject, nullptr); }
    JSValue resolveDynamic(JSValue specifier, ModuleID referrer, ResolvedSpecifierForm);

    JSPromise* import(ModuleID, ModuleID referrer, JSValue options = jsUndefined());
    void resumeDelayedImport(JSArray* deferredCall);
    bool m_isResumingDelayedImport { false };
    bool m_wasHeldForBody { false };
    Vector<ModuleID, 2> m_delayedImportTargets;
    Vector<ModuleID, 2> m_factoryBlockedImportRoots;
    struct FactoryBlockedImport {
        ModuleID root;
        Strong<JSPromise> promise;
        size_t followingCallCount;
    };
    Vector<FactoryBlockedImport, 2> m_factoryBlockedImports;
    const FactoryBlockedImport* lastFactoryBlockedImport(ModuleID root) const
    {
        for (size_t index = m_factoryBlockedImports.size(); index--;) {
            if (m_factoryBlockedImports[index].root == root && m_factoryBlockedImports[index].promise->status() == JSPromise::Status::Pending)
                return &m_factoryBlockedImports[index];
        }
        return nullptr;
    }
    Vector<ModuleID, 2> m_rootsOfImportsThatHaveGoneOn;
    void didResumeAfterFactories(ModuleID root);
    JSValue errorOfFactoryThatFailedWhileCallWaited(ModuleID root, uint32_t momentCallBeganToWait) const;
    JSValue takeErrorOfFactoryThatFailedWhileCallWasHeldForItsText(ModuleID asked);
    void recordFactoryFailure(ModuleID mocked, JSValue error);
    bool hasInFlightMockImport(ModuleID asked) const { return m_importsAwaitingSource.containsIf([&](const ImportAwaitingSource& each) { return each.isMockImport && !each.missingMockFailure && fileOf(each.module) == fileOf(asked); }); }
    bool hasInFlightOriginalRequest(ModuleID asked) const { return m_importsAwaitingSource.containsIf([&](const ImportAwaitingSource& each) { return each.isMockImport && each.hasRequestedOriginal && !each.missingMockFailure && fileOf(each.module) == fileOf(asked); }); }
    bool hasMockImportsAwaitingSource() { return hasUnstartedActualKeyLoad() || hasLateImportAwaitingSource() || m_importsAwaitingSource.containsIf([&](const ImportAwaitingSource& each) { return each.isMockImport && !hasOriginalGraphStartedEvaluating(fileOf(each.module)); }); }
    bool hasDeferredFetchOf(ModuleID file);
    bool hasDeferredFetchToEvict();
    void willRunTestFileAgain(ModuleID file);
    BitVector m_filesToEvictOnceFetched;
    bool isSomeCallHeldForItsText() const { return hasBatchedFetches() || !m_queuedFetches.isEmpty(); }
    void didSettleImportBehindFactories(ModuleID root);
    void holdMockThatStandsUntilImportSettles(ModuleID root, JSPromise*);
    bool hasDelayedImport(ModuleID module) const
    {
        return m_delayedImportTargets.containsIf([&](ModuleID held) { return fileOf(held) == fileOf(module); });
    }
    enum class ImportCaller : bool { CompiledCode, Host };
    enum class ImportSourceState : uint8_t { MayArriveLater, Arrived, ArrivedWithDeferredMocks };
    JSPromise* startImport(ModuleID, std::optional<int64_t> referrerAsyncOrder, ImportCaller = ImportCaller::CompiledCode, ImportSourceState = ImportSourceState::MayArriveLater, JSPromise* promiseOfCaller = nullptr, JSValue importWaitState = JSValue(), std::optional<bool> priorRunMocksResult = std::nullopt);
    JSPromise* rejectImportWithErrorOfLoad(ModuleID, ImportCaller);
    void startImportWhoseTextHasCome(JSPromise* importPromise, ModuleID, std::optional<int64_t> referrerAsyncOrder, JSPromise* loaderPromise = nullptr);
    bool tryDeferFetchFulfillment(JSPromise* fetched, ModuleRecord* requested, ModuleID requester);
    void fulfillFirstQueuedFetch();
    void fulfillQueuedFetch(JSPromise*);
    void fulfillQueuedFetchAfterJobs(JSPromise*);
    bool isOriginalLoaded(ModuleID);
    enum class AskedBy : bool { Loader, Import };
    bool textOfImportComesInLaterTurn(ModuleID, AskedBy);
    bool hasTextAtHand() const;
    bool isFetchedBesideFactory(ModuleID);
    void didDrainDeferredFetchJobs()
    {
        m_isInJobsAfterArrival = false;
        m_batchBeingFulfilled = std::nullopt;
    }
    bool hostHasProgramSources() const;
    bool isTestRun() const;
    void holdTextOfCall(JSPromise*, ModuleID, ModuleRecord* requested, std::optional<int64_t> referrerAsyncOrder, ImportCaller);
    void willFulfillDeferredFetches() { ++m_currentBatchNumber; }
    bool hasDeferredFetches() const { return !m_batchedFetches.isEmpty() || !m_queuedFetches.isEmpty(); }
    std::optional<uint32_t> firstBatchNumber() const { return m_batchedFetches.isEmpty() ? std::nullopt : std::optional(m_batchedFetches.first().batchNumber); }
    bool isFirstFetchDue(uint64_t completedDiskReads) const { return (m_batchedFetches.isEmpty() ? m_queuedFetches.first().precedingDiskReads : m_batchedFetches.first().precedingDiskReads) <= completedDiskReads; }
    bool tryFulfillFirstBatchedFetch();
    bool tryFulfillFirstQueuedFetch();
    void tellHostOfImport(JSPromise*, const String& path, std::optional<ModuleID> file = std::nullopt);
    JSPromise* importDynamic(JSValue specifier, JSValue options, ModuleID referrer);
    JSPromise* importUncompiled(const Identifier& key, ModuleID referrer, RefPtr<ScriptFetchParameters>&&, JSValue importWaitState = { });
    void continueImport(JSPromise* importPromise, ModuleID, std::optional<int64_t> referrerAsyncOrder);
    void resolveWithNamespace(JSPromise* importPromise, ModuleID);
    void finishImportOfEvaluatedModule(JSPromise* importPromise, ModuleID, JSValue error);
    JSPromise* behindStepsOfImportThatWaitsFor(JSPromise* evaluation, ModuleID);
    void updateRetainedImportNamespaces(ModuleID);
    void recordMockedImport(JSPromise*, ModuleID);
    void didRunFirstImportStepSlow(JSPromise*);
    enum class DeedOnMock : bool { Ends, LaysAnother };
    void noteDeedOnMockUnderCallsOfImportSlow(ModuleID, DeedOnMock);
    enum class MockChange : bool { Removed, Replaced };
    void keepMockForPendingImports(ModuleID, MockChange);
    void scheduleImportBinding(JSPromise*);
    void didBindImportToMock(JSPromise*);
    void advanceMockedImportsThrough(unsigned serial);
    void didRunFirstImportStep(JSPromise* promise)
    {
        if (!m_importsAwaitingFirstStep.isEmpty()) [[unlikely]]
            didRunFirstImportStepSlow(promise);
    }
    void noteDeedOnMockUnderCallsOfImport(ModuleID module, DeedOnMock deed)
    {
        if (!m_importsAwaitingFirstStep.isEmpty()) [[unlikely]]
            noteDeedOnMockUnderCallsOfImportSlow(module, deed);
    }
    void clearRetainedImportNamespaces();
    JSObject* takeRetainedImportNamespace(JSPromise*);

    enum class RecordHolder : uint8_t { Anyone, Loader };
    ModuleRecord* exposeRecord(ModuleID, RecordHolder = RecordHolder::Anyone);
    void retireRecordsOfSettledLoads();
    AbstractModuleRecord* uncompiledRecord(ModuleID);
    AbstractModuleRecord* recordForResolution(ModuleID);
    enum class NamespaceUse : uint8_t { IsShownToProgram, CarriesBindings };
    JSObject* uncompiledNamespace(ModuleID, ModulePhase, NamespaceUse, bool shouldPreventExtensions = true);

private:
    friend class HostRequestScope;

    template<typename T>
    class Reference {
        WTF_MAKE_NONCOPYABLE(Reference);
    public:
        Reference() = default;

        operator T() const { return m_value; }
        explicit operator bool() const { return !!m_value; }
        T operator->() const { return m_value; }
        T get() const { return m_value; }

        void set(Modules& modules, T value)
        {
            modules.didChangeLinkState();
            setUnlinked(modules, value);
        }

        void setUnlinked(Modules& modules, T value)
        {
            m_value = value;
            modules.m_storedReferences.recordAll();
        }

    private:
        T m_value { };
    };

    struct LoadedModule {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LoadedModule);

        Reference<JSObject*>& namespaceObjectFor(ModulePhase phase) { return phase == ModulePhase::Defer ? deferredNamespaceObject : namespaceObject; }

#define AOT_DECLARE_REFERENCE(type, name) Reference<type> name;
        FOR_EACH_REFERENCE_OF_LOADED_MODULE(AOT_DECLARE_REFERENCE)
#undef AOT_DECLARE_REFERENCE
        uint32_t failureLinkMoment { 0 };
        uint32_t failureReleaseMoment { 0 };
        bool isBeingRequired { false };
        bool isExportsVariableTakenOffModuleObject { false };
        bool isOrderOfStoreAndDeedUnknown { false };
        bool wasModuleObjectMaterialized { false };
        StructureID idOfStructureOfModuleObjectLastLookedAt;
        bool isKeptByRequire { false };
        bool wasRequiredByHost { false };
        bool isPatchedByMock { false };
        bool isReleasedPatch { false };
        bool wasEvaluatedWhenPatched { false };
        uint32_t momentImportFailedUnderMock { 0 };
        bool hadThrownWhenMockWasMade { false };
        bool holdsValueOfMockThatEnded { false };
        bool hadThrownWhenPatched { false };
        bool wasDisplacedAfterFailure { false };
        UncheckedKeyHashMap<std::pair<Environment*, uint32_t>, JSValue> fieldsBeforeMock;
        Vector<ModuleID> namespacesReplacedByMock;
        Vector<ModuleID> mocksThatReplaceNamespace;
        bool capturedFieldsAreOfESModule { false };
        bool capturedFieldsAreOfRuntimeModule { false };
        Identifier resolvedKey;
        Identifier keyOfLatestLoad;
        bool wasLoadedDuringEvaluation { false };
        bool wasLoadedSynchronouslyDuringEvaluation { false };
        bool wasFetchedDuringEvaluation { false };
        bool isAnsweredWithNamespace { false };
    bool isCreatedByRequireCache { false };
    bool hasStaleFirstRequire { false };
        bool bindsEarlierCapture { false };
        bool didDropError { false };
        bool exportsWhatMockNamesAlone { false };
        bool hasLinkedImporter { false };
        bool holdsValuesOfMock { false };
        bool takesNamesThatMockAdds { false };
        bool isYetToBeRequiredBehindMock { false };
        bool hadRunAheadOfMocksThatNameAlone { false };
        bool wasRequiredUnderActiveMock { false };
        bool wasImportedUnderActiveMock { false };
        bool isPreloadMockPatched { false };
        bool wasRequiredUnderPreloadMock { false };
        bool wasImportedUnderPreloadMock { false };
        bool returnsPreloadMockNamespace { false };
        uint32_t momentOfTestFileThatLoadedItUnderMockInPlace { 0 };
        bool nextRequestReloadsActiveMock { false };
        uint32_t momentOfDisplacement { 0 };
        uint32_t momentFirstLoaded { 0 };
        uint32_t resetOfReloadedState { 0 };
        uint32_t resetOfNamespace { 0 };
        uint32_t activeMockReloadTestFile { 0 };
        bool exportsOfMockAreOfCommonJSModule { false };
        bool wasLoadedBehindMock { false };
        bool wasRequiredBehindMock { false };
        bool wasReachedBySecondLoad { false };
        bool isAwaitedByCallThatComesInLaterJob { false };
        bool isBeingLoadedByLoader { false };
        bool isLinkedWithoutNotice { false };
        BitVector ambiguousExports;
        std::unique_ptr<SourceOrigin> sourceOrigin;
        std::optional<uint32_t> synchronousLoadCountAtStart;
        bool beganInsideSynchronousLoad { false };
        uint32_t throwAfterFetchLoad { 0 };
    };

    struct WaysToModule {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(WaysToModule);

        Vector<ModuleID> requiredModules;
        unsigned numberOfChildrenShown { 0 };
        std::optional<ModuleID> firstRequirer;
    };
    WaysToModule& ensureWaysTo(ModuleID module)
    {
        return *m_waysToModules.ensure(module, [] {
            return makeUnique<WaysToModule>();
        }).iterator->value;
    }
    WaysToModule* waysToIfRecorded(ModuleID module) const { return m_waysToModules.get(module); }
    std::optional<ModuleID> firstRequirerOf(ModuleID module) const
    {
        WaysToModule* ways = waysToIfRecorded(module);
        return ways ? ways->firstRequirer : std::nullopt;
    }
    void forgetFirstRequirerOf(ModuleID module)
    {
        if (WaysToModule* ways = waysToIfRecorded(module))
            ways->firstRequirer = std::nullopt;
    }
    UncheckedKeyHashMap<ModuleID, std::unique_ptr<WaysToModule>> m_waysToModules;

    struct DynamicExport {
        Identifier name;
        JSValue capturedValue;
        JSObject* liveSource { nullptr };
    };
    Vector<DynamicExport> collectStarExports(ModuleID, Environment* instance);

    StoredModuleStatus storedStatus(ModuleID) const;
    bool isLinkedWithLeader(ModuleID) const;
    void setStoredStatus(ModuleID, StoredModuleStatus);
    void unsettleForLinks(ModuleID);
    ModuleStatus statusInRecord(ModuleID) const;
    void setStatus(ModuleID, ModuleStatus);
    void setFailed(ModuleID, JSValue error);
    ModuleRecord* ensureRecordWithoutStatus(ModuleID);
    void retireRecord(ModuleRecord*);
    void retireRecordOfModuleThatHasNotBegun(ModuleRecord*);
    bool hasRunToItsEndWithItsCycle(const ModuleRecord&) const;
    static bool isEvaluatingAsynchronouslyWithItsCycle(const ModuleRecord&);
    void setValue(ModuleID, JSValue);
    LoadedModule& ensureLoaded(ModuleID);
    LoadedModule* loadedIfExists(ModuleID) const;
    void throwEvaluationError(ModuleID);
    static JSPromise* rejectedPromiseWithPendingException(JSGlobalObject*, ThrowScope&);
    Identifier moduleKey(ModuleID) const;
    std::optional<ModuleID> findByKey(const Identifier&) const;
    Identifier keyOfRequest(ModuleID target);
    JSObject* namespaceIfCreated(ModuleID, ModulePhase) const;
    JSObject* namespaceThatCompiledCodeHolds(ModuleID, ModulePhase) const;
    void setNamespace(ModuleID, ModulePhase, JSObject*);
    void discardNamespace(ModuleID);
    JSObject* createNamespace(ModuleID, ModulePhase, bool shouldPreventExtensions);
    Structure* namespaceStructure(bool shouldPreventExtensions);
    JSObject* createTableOfExports();
    JSModuleLoader* loader() const;

    enum class NativeModuleLoading : bool { Eager, Deferred };
    bool loadRuntimeModules(const ModuleLinkWalk&, NativeModuleLoading = NativeModuleLoading::Eager);
    void link(const ModuleLinkWalk&);
    bool hasFileCapturedAfterEvaluation(const ModuleLinkWalk&) const;
    bool resolvesNamesOutsideLibrary(const ModuleLinkWalk&, ThrowScope&) const;
    void captureSyntheticExports(const ModuleLinkWalk&, VisitedModuleRecords&);
    void finishLink(const ModuleLinkWalk&, LinkContext&);
    void didLinkEntryPoint(ModuleID, const MocksInGraph&);
    bool didCaptureExports(ModuleID) const;
    void captureExports(ModuleID);
    void copyExportsOfFinishedFile(ModuleID facade);
    JSObject* generateNativeModuleExports(ModuleID, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values);
    JSValue requireNativeModule(ModuleID);
    void bindLatestCapture(ModuleID);
    void captureExportsBehindEvaluation(ModuleID);
    void captureExportsOfLibraryModules(ModuleID root);
    void captureExportsOfFilesBeingEvaluated(ModuleID root);
    void noteThatGraphBegins(ModuleID root);
    JSValue evaluateForCapture(ModuleID);
    bool tryCaptureFields(ModuleID, JSValue exports);
    JSValue capturedField(const LoadedModule&, UniquedStringImpl* name) const;
    void makeTableOfCapturedFields(LoadedModule&);
    void fillCapturedExports(ModuleID);
    void fillDeferredExports(ModuleID importer);
    AbstractModuleRecord::Resolution::Type resolveRequiredName(ModuleID exporter, const Identifier&);
    std::optional<AbstractModuleRecord::Resolution::Type> tryResolveRequiredNameInTables(const RequiredNameData&);
    bool reexportsFromMock(ModuleID);
    ReexportedNames& reexportedNames();
    enum class ModulesAsked : bool { ThoseNotLinked, ThoseLoadedAnew };
    void checkRequiredNames(std::span<const ModuleID> modules, ModulesAsked = ModulesAsked::ThoseNotLinked);
    void checkRequiredNamesOfModulesLoadedAnewBelow(ModuleID root);
    bool isLeftOut(ModuleID, unsigned exportIndex, TablesBehindStars, SearchBehindStars&) const;
    bool namesBehindStarsAreInTables(ModuleID, SearchBehindStars&) const;
    bool isVariableOfExportEmpty(ModuleID, unsigned exportIndex) const;
    void callInstanceAllocator(ModuleID);
    Environment* readLink(const Environment*, SlotData) const;
    const ModuleLinkData* findModuleLink(ModuleID, ModuleID target) const;
    ModuleNamespace* mockWord(const ModuleLinkData&, const Environment*) const;
    bool isMockPending(ModuleID file) const;
    bool isMockSeenByLoad(ModuleID target, ModuleID importer) const;
    ModuleNamespace* mockNamespaceForLoad(ModuleID target);
    ModuleNamespace* wordForLoad(ModuleID target, bool seesMock);
    ModuleNamespace* ensureLoadWord(ModuleID target);
    Vector<ModuleID, 16> collectUnlinkedInstances(ModuleID root);
    enum class StarMode : bool { Follow, Ignore };
    bool tryReadLinkedExport(ModuleID, unsigned index, Environment*, JSValue&, StarMode = StarMode::Follow);
    void fixHiddenExports(ModuleNamespace&);
    struct LinkedBinding {
        enum class Type : uint8_t { NotFound, Resolved, Ambiguous };
        enum class Kind : uint8_t { Variable, Field, Namespace, Name };

        bool isSameBinding(const LinkedBinding& other) const { return kind == other.kind && holder == other.holder && place == other.place && name == other.name; }

        Type type { Type::NotFound };
        Kind kind { Kind::Name };
        bool isOfMock { false };
        const void* holder { nullptr };
        uint64_t place { 0 };
        UniquedStringImpl* name { nullptr };
        bool isLackedByMock { false };
    };
    struct BindingSearch {
        using Query = std::tuple<uint32_t, const Environment*, UniquedStringImpl*>;
        static constexpr size_t noQuery = std::numeric_limits<size_t>::max();

        UncheckedKeyHashMap<Query, size_t> visited;
        Vector<Query, 8> open;
        size_t outermost { noQuery };
        size_t numberOfRevisits { 0 };
        size_t numberOfOrderedResults { 0 };
    };
    LinkedBinding resolveBinding(ModuleID, const Identifier& name, Environment* instance, BindingSearch&);
    JSValue readBinding(const LinkedBinding&, const Identifier& name);
    LinkedBinding resolveBindingAmongStars(ModuleID, const Identifier& name, Environment* instance, BindingSearch&);
    LinkedBinding bindingOfExport(ModuleID, unsigned index, Environment* instance, BindingSearch&);
    bool isProvidedByDynamicStar(ModuleID, unsigned exportIndex) const;
    bool isAmbiguousInLinks(ModuleID, const Identifier& name, Environment* instance, BindingSearch&);

    struct MockForExport {
        ModuleNamespace* word { nullptr };
        uint32_t nameInMock { 0 };
        bool isFromStarExport { false };
        std::optional<std::pair<ModuleID, unsigned>> exportWithoutLinks { };
        bool operator==(const MockForExport&) const = default;
    };
    MockForExport findMockForField(ModuleID, unsigned exportIndex, Environment* instance) const;
    struct NamespaceBehindExport {
        enum class End : uint8_t { Broken, WithoutLink, WithoutInstances, Mock, Instance, Original };
        End end { End::Broken };
        bool isFollowed { false };
        bool mayBeMocked { false };
        bool isFromStarExport { false };
        ModuleNamespace* word { nullptr };
        std::optional<uint32_t> nameInMock { };
        Environment* instance { nullptr };
        std::pair<ModuleID, unsigned> original { };
        std::optional<Environment*> holder { };
        JSObject* absent { nullptr };
        bool operator==(const NamespaceBehindExport&) const = default;
    };
    NamespaceBehindExport followNamespace(ModuleID, unsigned exportIndex, Environment* instance) const;

    struct LinkState {
        uint32_t version { 0 };
        uint32_t activeMocksVersion { 0 };
        unsigned linkEpoch { 0 };
        uint32_t momentOfLoads { 0 };
        uint32_t momentOfTestFile { 0 };
        bool operator==(const LinkState&) const = default;
    };
    struct AnswersByLinks {
        LinkState state { };
        UncheckedKeyHashMap<Environment*, bool> hasOutdatedStarLink { };
        UncheckedKeyHashMap<std::pair<Environment*, unsigned>, MockForExport> mocksBehindFields { };
        UncheckedKeyHashMap<std::pair<Environment*, unsigned>, ResolvedVariable> variables { };
        UncheckedKeyHashMap<std::pair<Environment*, unsigned>, NamespaceBehindExport> namespaces { };
        UncheckedKeyHashMap<std::pair<Environment*, RefPtr<UniquedStringImpl>>, StarResolution> starResolutions { };
        struct KeptBinding {
            LinkedBinding binding;
            RefPtr<UniquedStringImpl> name;
        };
        UncheckedKeyHashMap<std::pair<const Environment*, RefPtr<UniquedStringImpl>>, KeptBinding> bindings { };
    };
    AnswersByLinks& answersByLinks() const;
#if ASSERT_ENABLED
    template<typename Function> auto answerAnew(const Function&) const;
    mutable bool m_isAnsweringAnew { false };
#endif
    struct OutdatedStarSearch {
        struct Visit {
            Environment* instance { nullptr };
            unsigned outermost { 0 };
        };
        UncheckedKeyHashMap<ModuleID, Visit> visits { };
        bool hasOtherInstances { false };
    };
    bool findOutdatedStarLink(ModuleID, Environment* instance, OutdatedStarSearch&, unsigned depth, unsigned& outermost) const;
    mutable uint32_t m_linkStateVersion { 0 };
    mutable std::array<AnswersByLinks, 2> m_answersByLinks;
    void storeInInstance(ModuleNamespace&);
    JSObject* namespaceInInstance(ModuleID, ModulePhase) const;
    void markAsRequired(ModuleNamespace&, bool isRequired);
    JSValue readInitializedLinkedExport(ModuleID, unsigned index, Environment*) const;
    void writeField(Environment* instance, SlotData, JSValue);
    bool isFoundInTextOfFile(const RequiredNameData& requiredName) const { return requiredName.mayBeAbsent && isCommonJSModuleOrFacade(requiredName.exporterModule()); }
    bool isDeferredExport(const LoadedModule&, const Identifier&) const;
    bool findsItsNamesInLibraryAlone(ModuleID) const;
    Vector<DynamicExport> collectDynamicExports(ModuleID);

    void evaluateWithOwnStack(ModuleID root);
    Vector<ModuleID, 1> m_modulesAskedForWhileTheyRan;
    void settleImportsOfModulesThatHaveRun(ModuleID rootOfEvaluation);
    BitVector evaluatingModuleDependencies(Vector<ModuleID>& topmost) const;
    Vector<ModuleID> m_modulesPendingReset;
    Vector<ModuleID> m_modulesBlockingReset;
    void resetPendingModules();
    size_t queuedJobCount(EagerLoadJobPolicy) const;
    void holdJobsBehind(size_t numberOfJobs, ModuleID file, EagerLoadJobPolicy = EagerLoadJobPolicy::DeferJobs);
    bool hasQueuedTicks() const { return host().hasQueuedTicks && host().hasQueuedTicks(m_instance.globalObject); }
    struct LevelOfLoad;
    struct PlanOfLoad;
    bool tryFindAliasedCompiledModules(ModuleID root, UncheckedKeyHashMap<ModuleID, ModuleID>&);
    bool planLevelsOfLoad(ModuleID root);
    bool isLeftAsLoaded(ModuleID) const;
    bool isModuleWithoutRecord(const ModuleData&, StoredModuleStatus) const;
    bool isNeitherOfProgramNorData(const ModuleData&, StoredModuleStatus) const;
    bool isFoundWithoutRecord(ModuleID, const ModuleData&, StoredModuleStatus);
    bool isStillToBeLoaded(ModuleID);
    void layWayOfLoad(PlanOfLoad&, LevelOfLoad* above);
    void layWayOfLoadAgain(LevelOfLoad&);
    void countStepsOfLevel(PlanOfLoad&, size_t depth);
    uint32_t countStepsOfWholeGraph(const PlanOfLoad&, std::span<uint32_t> firstRequestOfPlace, std::span<uint32_t> allRequestedPlaces);
    void takeUpWaysOfLoads();
    void markModulesOfPlan(const PlanOfLoad&);
    uint32_t takeMarkOfSearch();
    JSValue callBody(ModuleID);
    JSValue enterBody(ModuleID, FunctionID body);
    JSValue callBodyUnderOwnMark(ModuleID, bool bodyStoresStatus, bool runsImportedFilesAlone = false);
    struct ModuleThatMayBegin {
        ModuleID module;
        uint32_t markBefore;
    };
    Vector<ModuleThatMayBegin, 8> noteModulesThatMayBeginUnder(uint32_t mark, ModuleID root);
    bool hasNotBegun(ModuleID module) const
    {
        ModuleStatus current = status(module);
        return current == ModuleStatus::Unlinked || current == ModuleStatus::Linked;
    }
    uint32_t markOfEvaluationThatBegan(ModuleID) const;
    class EvaluationMarkScope {
        WTF_MAKE_NONCOPYABLE(EvaluationMarkScope);
    public:
        explicit EvaluationMarkScope(Modules&);
        ~EvaluationMarkScope() { leave(); }

        uint32_t mark() const { return m_outerMark + 1; }
        uint8_t storedMark() const { return storedMark(mark()); }
        bool isUnderAnotherEvaluation() const { return m_outerMark != static_cast<uint32_t>(StoredModuleStatus::Evaluating); }
        void stopMarking() { m_modules.m_instance.modules.evaluatingMark = storedMark(m_outerMark); }
        void leave();

    private:
        static uint8_t storedMark(uint32_t mark) { return static_cast<uint8_t>(std::min<uint32_t>(mark, UINT8_MAX)); }

        Modules& m_modules;
        uint32_t m_outerMark;
        bool m_hasLeft { false };
        Vector<ModuleID> m_outerModules;
    };
    uint32_t evaluationMarkOf(ModuleID) const;
    uint32_t m_evaluationMark { static_cast<uint32_t>(StoredModuleStatus::Evaluating) };
    UncheckedKeyHashMap<ModuleID, uint32_t> m_marksOfOuterModules;
    void evaluateByCallingBody(ModuleID module) { callBodyUnderOwnMark(module, true); }
    void didThrowInBody(ModuleID, JSValue error);
    void recordEvaluationIfNothingIsLeftToRun(ModuleID);
    bool isLeftToStartsOfImporters(ModuleID) const;

    std::optional<ModuleID> mainCommonJSModule();
    void decideMainCommonJSModule(std::optional<ModuleID>);
    void lookForMainCommonJSModule();
    bool isInRequireMap(ModuleID file);
    void resetFailedMainModule();
    bool isMainCommonJSModule(ModuleID file);
    void noteFirstRequestFromHost(ModuleID);
    void noteLoadOfGraph(ModuleID root);
    enum class ModuleObjectCreator : bool { Require, RequireCache };
    JSObject* moduleObjectMadeByRequire(ModuleID, ModuleObjectCreator = ModuleObjectCreator::Require);
    JSObject* moduleObjectOrPlaceholder(ModuleID);
    JSObject* moduleObjectIfCreated(ModuleID);
    void notePrototypeOfModuleObjects(JSObject* freshModuleObject);
    enum class FormThatAnswers : bool { TheOneAskedFor, TheOneThatLoaderFinds };
    JSValue answerRequire(ModuleID, ModuleID referrer = { }, RequiredBy = RequiredBy::Referrer, FormThatAnswers = FormThatAnswers::TheOneAskedFor, ReadsExports = ReadsExports::Yes);
    bool standsForFileOutsideImage(ModuleID) const;
    JSValue settledAnswerOfRequire(ModuleID, RequiredBy) const;
    void addToRequireCache(ModuleID, JSValue exports);
    void setRequireCacheValue(ModuleID, JSValue exports);
    JSValue requireThroughCache(ModuleID, ModuleID referrer);
    JSValue exportsOnObjectMadeByRequire(const LoadedModule&);
    JSValue answerRequireWithMock(ModuleID, ModuleID referrer);
    JSValue answerOfRequireToMock(ModuleID);
    JSValue requiredExportsOfMock(JSObject* exports);
    bool isMockedLibraryModule(ModuleID);
    JSValue loadEvictedFile(ModuleID, ModuleID referrer = { });
    JSValue readExportForRequire(JSObject* namespaceObject, const Identifier& name);
    JSObject* namespaceForRequire(ModuleID);
    JSValue whatRequireMakesOfNamespace(JSObject* namespaceObject);
    void willRequireESModule(ModuleID);
    void markNamespaceAsRequired(ModuleID);
    void clearDeferredRequireMark(ModuleID);
    void clearRejectedFirstRequire(ModuleID);

    std::optional<ModuleID> findByName(const Identifier& key);
    enum class FormWithoutAttributes : bool { MayAnswer, IsAnotherModule };
    std::optional<ModuleID> findWithAttributes(const Identifier& key, const ScriptFetchParameters*, FormWithoutAttributes = FormWithoutAttributes::MayAnswer, std::optional<ModuleID> foundUnderWrittenName = std::nullopt);
    std::optional<ModuleID> findFormUnderWrittenName(JSString* name, ModuleID referrer, const ScriptFetchParameters&);
    Identifier keyWithoutForm(ModuleID file);
    std::optional<ModuleID> findFormThatStatementLoaded(const Identifier& key, const ScriptFetchParameters&, std::optional<ModuleID> asked) const;
    bool wasImporterLoaded(ModuleID) const;
    bool isBelowRootWhoseFilesRun(ModuleID) const;
    void noteFormAnsweredFirst(const Identifier& key, const ScriptFetchParameters&, ModuleID file);
    void giveStatementsFormsThatLoaderHas(ModuleID importer);
    bool takeWhatFormAnsweredFirstHolds(ModuleID form);
    bool loaderOnlyHandsOverNamespace(ModuleID) const;
    JSPromise* askLoaderForImport(ModuleID, std::optional<int64_t> referrerAsyncOrder, JSPromise* resultPromise = nullptr);
    std::optional<int64_t> asyncEvaluationOrder(ModuleID) const;
    ModuleRecord* registerWithLoader(ModuleID, RecordHolder = RecordHolder::Anyone);
    const Identifier& resolvedKey(ModuleID);
    void willAnswerNameLeftToHostWith(const Identifier& key);

    State& m_state;
    Instance& m_instance;
    ModuleTable m_table;
    std::span<uint8_t> m_storedStatuses;
    UncheckedKeyHashMap<ModuleID, ModuleRecord*> m_records;
    bool m_someRetainedRecordHasLeft { false };
    void takeOutRecord(ModuleID);
    UncheckedKeyHashMap<ModuleID, std::unique_ptr<LoadedModule>> m_loaded;
    struct FormAnsweredFirst {
        Identifier name;
        ModuleID module;
    };
    ModuleMap<FormAnsweredFirst> m_formsAnsweredFirst;
    BitVector m_mockEvictedDataModules;
    BitVector m_evictedDataFiles;
    bool m_hasEvictedDataFiles { false };
    BitVector m_mockedEvictedDataFiles;
    BitVector m_importedDataOriginals;
    BitVector m_originalsEvaluatedByRequire;
    BitVector m_evictedDataOriginals;
    std::optional<ModuleID> m_originalRequiredByFactory;
    mutable LoadedModule* m_loadedFoundLast { nullptr };
    mutable ModuleID m_moduleOfLoadedFoundLast;
    RememberedSlotSet m_storedReferences;
    std::unique_ptr<ModuleLinkWalk> takeLinkWalk(ModuleID root);
    std::unique_ptr<ModuleLinkWalk> takeKeptLinkWalk(ModuleID root);
    void keepLinkWalk(std::unique_ptr<ModuleLinkWalk>&&);
    Vector<std::unique_ptr<ModuleLinkWalk>, 2> m_linkWalksOfLoads;
    Vector<ModuleID> m_unlinkedRecordsBelowLeaders;
    void linkRecordsWithLeaders();
    RefPtr<LinkWalks> m_walksOfLink;
    bool checkIsSettledForLinks(ModuleID) const;
    mutable unsigned m_linkEpoch { 0 };
    mutable BitVector m_leadersWhoseLinkGoesByAllRequests;
    mutable Vector<ModuleID> m_modulesLinkedAlone;
    mutable BitVector m_isLinkedAlone;
    class NumberOfMocksInPlace {
    public:
        operator bool() const { return m_number; }
        NumberOfMocksInPlace& operator+=(int change)
        {
            m_number += change;
            ++m_timesWritten;
            return *this;
        }
        NumberOfMocksInPlace& operator++() { return *this += 1; }
        NumberOfMocksInPlace& operator--() { return *this += -1; }
        uint32_t timesWritten() const { return m_timesWritten; }

    private:
        unsigned m_number { 0 };
        uint32_t m_timesWritten { 0 };
    };
    NumberOfMocksInPlace m_activeMockCount;
    StoredResolutions* m_answersKept { nullptr };
    Vector<std::pair<ModuleID, ModuleRecord*>, 2> m_originalsBeingAsked;
    Vector<OriginalAnswered, 4> m_originalsAnswered;
    BitVector m_modulesWithActiveMock;
    mutable Vector<uint64_t> m_modulesSettledForLinks;
    const EvaluationInProgress* m_innermostEvaluation { nullptr };
    uint32_t m_synchronousLoadCount { 0 };
    uint32_t m_activeSynchronousLoad { 0 };
    std::optional<ModuleID> m_innermostSyncLoadRoot;
    uint32_t m_outermostSynchronousLoad { 0 };
    Structure* m_namespaceStructure { nullptr };
    JSObject* m_prototypeOfModuleObjects { nullptr };
    JSObject* m_errorsOfExportsThatBuildFoundAbsent { nullptr };
    struct ImportInFlight {
        ModuleID module;
        Environment* instance;
        uint32_t moment;
        bool isFromOwnFactory { false };
        bool joinsImportCall { false };
    };
    UncheckedKeyHashMap<JSPromise*, ImportInFlight> m_importsInFlight;
    UncheckedKeyHashMap<ModuleID, ModuleIndexSet> m_patchesRequestedByOwnFile;
    ModuleIndexSet m_modulesReloadedUnderPatch;
    HashCountedSet<ModuleID> m_importsBeforeFirstStep;
    HashCountedSet<ModuleID> m_hostImportsBeforeFirstStep;
    std::optional<ModuleID> m_patchedModuleOfCycle;
    Vector<ModuleFailureID> m_failuresByExportsThatBuildFoundAbsent;
    UncheckedKeyHashMap<ModuleID, ModuleID> m_linkFailures;
    Structure* m_extensibleNamespaceStructure { nullptr };
    Identifier m_moduleExportsName;
    const Identifier& nameOfPlacesOfKeptExports();
    Identifier m_nameOfPlacesOfKeptExports;
    enum class RecordedRequire : uint8_t { Begun, BegunForEvictedModule, Returned, BegunAgain, BegunFirstESModuleRequire, ReturnedAndNotListed };
    UncheckedKeyHashMap<uint64_t, RecordedRequire> m_recordedRequires;
    struct RecordedRequireIndex {
        using Keys = UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
        ModuleIndexMap<Keys> byReferrer;
        ModuleIndexMap<Keys> byTarget;
        ModuleIndexMap<Vector<ModuleID, 2>> firstRequired;

        void addRequire(uint64_t key)
        {
            byReferrer.add(static_cast<uint32_t>((key << 1) >> 33), Keys()).iterator->value.add(key);
            byTarget.add(static_cast<uint32_t>(key), Keys()).iterator->value.add(key);
        }
        void addFirstRequire(ModuleID requirer, ModuleID required) { firstRequired.add(requirer.index(), Vector<ModuleID, 2>()).iterator->value.append(required); }
    };
    std::unique_ptr<RecordedRequireIndex> m_recordedRequireIndex;
    RecordedRequireIndex& recordedRequireIndex();
    Vector<ModuleID, 2> filesFirstRequiredBy(ModuleID requirer);
    std::optional<ModuleID> m_mainCommonJSModule;
    BitVector m_failedMainModules;
    bool m_isMainCommonJSModuleDecided { false };
    bool m_didSearchBelowEntryPoint { false };
    Vector<ModuleID, 4> m_pendingFirstRequireTargets;
    Vector<FirstRequire> m_firstRequires;
    JSObject* m_hostRequestRequirerObject { nullptr };
    Vector<ModuleID, 2> m_firstFilesOfLoads;
    BitVector m_filesRequestedByLoader;
    struct EagerLoad {
        ModuleID root;
        std::optional<ModuleID> moduleOrFacade;
        EagerLoad* outer;
        bool runsBeforeLoaderJobs { false };
        ModuleGraphLoadingState* load { nullptr };
        size_t jobCountBeforeModule { 0 };
        bool runsBeforeLoaderSteps { false };
        std::optional<ModuleID> transferredModule { };
        BitVector modulesOfLoad { };
        bool isTransferredSynchronousLoad() const { return transferredModule && !load; }
        BitVector requested { };
        Vector<ModuleID> nextLevelImporters { };
        BitVector requestedByOuterLoads { };
        bool hasCollectedOuterRequests { false };
        const VM::SynchronousModuleQueue* loadQueue { nullptr };
    };
    std::optional<ModuleID> evaluatingModuleOrFacade(const EagerLoad&) const;
    bool isBegunForStatement(ModuleID moduleOrFacade) const;
    bool wasRequestedByLoad(EagerLoad&, ModuleID file);
    void collectOuterRequests(EagerLoad&);
    bool hasSynchronousLoadFetched(ModuleID file);
    bool tryDiscardFailureDuringFetch(ModuleID file);
    bool takeThrowAfterFetch(ModuleID moduleOrFacade);
    void makeRecordWithoutExports(ModuleID facade);
    void transferGraphToLoader(EagerLoad&, std::optional<ModuleID> finishedFile = std::nullopt);
    void transferSynchronousLoad(EagerLoad&);
    bool reachesEagerLoadGraph(EagerLoad&, ModuleID required);
    void runSynchronousStepsInFrontOf(ModuleID file);
    struct TransferredLoad {
        ModuleID root;
        Strong<JSPromise> promise;
    };
    Vector<TransferredLoad, 1> m_transferredSynchronousLoads;
    struct DeferredLinkStep {
        InternalMicrotask task;
        uint8_t payload;
        std::array<Strong<Unknown>, 4> arguments;
    };
    Vector<DeferredLinkStep, 1> m_deferredLinkSteps;
    void captureNoExports(ModuleID file);
    void runStepsOfLoaderInFrontOf(ModuleID file);
    void finishStepInWhichFileRan(ModuleID moduleOrFacade, JSValue error);
    std::optional<ModuleID> m_moduleBeingTransferred;
    EagerLoad* m_innermostEagerLoad { nullptr };
    void noteRequestByLoader(ModuleID moduleOrFacade);
    bool wasRequestedByLoader(ModuleID file);
    void discardModuleObject(ModuleID file);
    void layValueInPlaceOfExportsVariable(ModuleID file);
    void forgetTemplateObjects(ModuleID file);
    struct TemplateObjectOfFile {
        ModuleID file;
        uint32_t offsetOfCell;
    };
    Vector<TemplateObjectOfFile> m_templateObjectsOfFiles;
    void noteRequireOfFile(ModuleID file);
    BitVector m_modulesMarkedAsRequired;
    BitVector m_modulesWithDeferredMark;
    BitVector m_modulesWithExtensibleNamespace;
    BitVector m_modulesWithEarlierNamespaceInValue;
    Vector<uint32_t> m_marksOfEvaluationsUnderOthers;
    Box<MockedModules> m_mocks;
    std::optional<ModuleID> m_moduleBeingPatched;
    OptionSet<RequireOverride> m_storedLoaderFunctions;
    std::optional<UncheckedKeyHashMap<uint32_t, SyntheticExportPlace, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> m_syntheticExportsByVariable;
    UncheckedKeyHashMap<ModuleID, BitVector> m_exportsLeftOut;
    BitVector exportsLeftOutOf(ModuleID module) const
    {
        auto found = m_exportsLeftOut.find(module);
        return found == m_exportsLeftOut.end() ? BitVector() : found->value;
    }
    BitVector m_evictedFiles;
    BitVector m_filesDelegatedByHost;
    BitVector m_filesModifiedSinceBuild;
    bool m_hasNotedTextsThatHostKeeps { false };
    BitVector m_dataFilesThatHostPrintsAsJavaScript;
    BitVector m_filesAboveNoEvictedFile;
    BitVector m_filesAboveEvictedFile;
    BitVector m_filesAskedAbout;
    BitVector m_filesWhoseGraphHasBegun;
    uint64_t m_numberOfGraphsBegun { 0 };
    bool m_skipsResolvePlugins { false };
    BitVector m_retainedModules;
    BitVector m_evaluatedModulesSnapshot;
    std::optional<bool> m_holdsCodeThatMayBeEvicted;
    BitVector m_retainedModulesToRestore;
    BitVector m_filesEvictedWithImports;
    bool m_runsCopyFromDisk { false };
    bool m_hostRunsCopiesFromDisk { false };
    BitVector m_filesToLoadAgain;
    BitVector m_uncompiledNamespacesAskedFor;
    BitVector m_uncompiledNamespacesNotShownYet;
    BitVector m_modulesThatRanWithImporters;
    BitVector m_modulesSeenToHaveRun;
    BitVector m_recordsNotSeenToHaveRun;
    mutable BitVector m_recordsNotSeenEvaluated;
    Vector<uint8_t> m_statusesWhenImportsWereFollowed;
    ModulesThatWaitForCycles m_modulesThatWaitedWhenImportsWereFollowed;
    BitVector m_modulesOfQueuedImports;
    BitVector m_modulesWithPendingMock;
    BitVector m_filesWithCompiledOriginal;
    const ArgList* m_argumentsOfOriginal { nullptr };
    std::optional<ModuleID> m_commonJSOriginal;
    ModuleIndexMap<uint32_t> m_testFilesOfCommonJSOriginals;
    struct OriginalInstance {
        Weak<Environment> instance;
        Identifier key;
    };
    UncheckedKeyHashMap<uint32_t, Vector<OriginalInstance, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_originalsByFile;
    BitVector m_filesWithRejectedRequire;
    BitVector m_originalsUnderEndedBuiltinMocks;
    Vector<std::pair<ModuleID, ModuleID>> m_originalsReadingHostMocks;
    std::optional<ModuleID> m_importDeferringMocks;
    struct ImportAwaitingSource {
        ModuleID module;
        bool isMockImport { false };
        bool didRunMocksBeforeSource { false };
        bool hasRequestedOriginal { false };
        bool wasOvertakenByRequire { false };
        Strong<JSPromise> missingMockFailure;
        uint32_t callSequenceNumber { 0 };
    };
    Vector<ImportAwaitingSource, 2> m_importsAwaitingSource;
    bool shouldRunMocksAfterSource(ModuleID);
    std::optional<ModuleID> requestedMockAheadOfCommonJS(ModuleID root) const;
    bool isNamedByImport(ModuleID file) const;
    void didCallFactoryOfLoad(ModuleID file);
    bool shouldRunMocksAtFetch(ModuleID);
    BitVector m_statementFetchesDelayedByTick;
    struct FactoryCaller {
        ModuleID root;
        ModuleID mocked;
        FactoryCaller* outer { nullptr };
        std::optional<BitVector> requested { };
    };
    FactoryCaller* m_innermostFactoryCaller { nullptr };
    const BitVector& modulesRequestedBefore(FactoryCaller&);
    BitVector m_awaitedGeneratedModules;
    std::optional<ModuleID> m_moduleBeingFetched;
    bool hasFactoryBlockedFetch(ModuleID);
    JSPromise* importModuleItselfWithoutLoader(ModuleID);
    std::optional<ModuleID> originalThatOwnFactoryAsksFor(const Identifier& key);
    BitVector m_mocksWhoseFactoriesRanInsideTheirFetch;
    Vector<ModuleID> m_libraryModulesInRequireCache;
    BitVector m_libraryModulesTakenFromLoader;
    BitVector m_libraryModulesThatStatementsReached;
    mutable BitVector m_libraryModulesWithCheckedName;
    mutable BitVector m_libraryModulesKeptInRequireCache;
    BitVector m_libraryModulesWithMock;
    BitVector m_libraryModulesThatMockReplaces;
    BitVector m_displacedLibraryModules;
    UncheckedKeyHashMap<ModuleID, uint32_t> m_momentsMocksInPlaceOfLibraryModulesEnded;
    UncheckedKeyHashMap<ModuleID, uint32_t> m_lastLinksOfReleasedImporters;
    uint32_t m_momentOfModuleReset { 0 };
    UncheckedKeyHashMap<ModuleID, uint32_t> m_momentsLibraryModulesWereExposed;
    BitVector m_libraryModulesLeftByThrow;
    std::optional<ModuleID> m_firstModuleRequestedByHost;
    std::optional<ModuleID> m_mainEntryPoint;
    bool m_isRunningPreloads { false };
    BitVector m_dataFilesImportedByUncompiledCode;
    bool m_didTakeMainModuleLoadStep { false };
    bool m_didSplitMainLoadPromise { false };
    ModuleID m_importOfMainModule;
    Strong<JSPromise> m_evaluationOfImportOfMainModule;
    Strong<JSPromise> m_capabilityOfMainModule;
    bool m_admitsCodeOutsideImage;
    const String* m_moduleRequester { nullptr };
    bool m_runsUncompiledCode { false };
    const bool m_admitsInspector;
    const bool m_showsWaysBetweenModuleObjects;
    const bool m_hasReloadableModules;
    const bool m_setsMainModulePath;
    bool m_loadsModulesByName;
    const bool m_refusesOriginalsBesideMocks;
    const bool m_showsEntriesOfRequireCache;
    class HostRequest {
    public:
        ALWAYS_INLINE HostRequest(bool isMade = false)
            : m_madeAt(isMade ? currentStackPointer() : nullptr)
        {
        }
        static ALWAYS_INLINE HostRequest forUncompiledModule()
        {
            HostRequest request(true);
            request.m_isForCompiledModule = false;
            return request;
        }
        explicit operator bool() const { return m_madeAt; }
        bool isForCompiledModule() const { return m_isForCompiledModule; }
        bool wasMadeBeforeEntering(const void* frame) const { return frame < m_madeAt; }

    private:
        const void* m_madeAt;
        uintptr_t m_isForCompiledModule { true };
    };
    static_assert(std::has_unique_object_representations_v<HostRequest>);
    HostRequest m_currentHostRequest;
    Vector<HostRequest, 4> m_savedHostRequests;
    std::optional<ModuleID> m_rootLoadedByCompiledCode;
    bool m_isStatementHostRequest { false };
    bool m_isWalkingRequests { false };
    struct LoadInFlight : BasicRawSentinelNode<LoadInFlight> {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LoadInFlight);

        bool isWaiting() const { return !isLoaded && !isWalked; }

        Strong<JSPromise> promise;
        ModuleID root;
        bool isWalked { false };
        bool hasMarksBelowRoot { false };
        bool isLoaded { false };
        bool isKnownToBeSettled { false };
        Strong<JSArray> callsThatSettleBehind;
        uint32_t loaderStepsAtCall { 0 };
        uint64_t numberOfFilesRunAtCall { 0 };
        uint64_t number { 0 };
        RefPtr<PlanOfLoad> planWithoutMarks;
    };
    class LoadsInFlight {
        WTF_MAKE_NONCOPYABLE(LoadsInFlight);
    public:
        LoadsInFlight() = default;

        class Iterator {
        public:
            Iterator(const Vector<std::unique_ptr<LoadInFlight>, 2>& loads, size_t place)
                : m_loads(loads)
                , m_place(place)
            {
                skipLoadsTakenOut();
            }

            LoadInFlight& operator*() const { return *m_loads[m_place]; }
            bool operator==(const Iterator& other) const { return m_place == other.m_place; }
            Iterator& operator++()
            {
                ++m_place;
                skipLoadsTakenOut();
                return *this;
            }

        private:
            void skipLoadsTakenOut()
            {
                while (m_place < m_loads.size() && !m_loads[m_place])
                    ++m_place;
            }

            const Vector<std::unique_ptr<LoadInFlight>, 2>& m_loads;
            size_t m_place;
        };
        Iterator begin() const { return { m_inOrder, m_firstPlace }; }
        Iterator end() const { return { m_inOrder, m_inOrder.size() }; }

        bool isEmpty() const { return !m_size; }
        bool hasRoot(ModuleID root) const { return m_byRoot.contains(root); }
        void forEachOfRoot(ModuleID root, NOESCAPE const Invocable<void(LoadInFlight&)> auto& functor) const
        {
            auto found = m_byRoot.find(root);
            if (found == m_byRoot.end())
                return;
            for (LoadInFlight* load : found->value.loads)
                functor(*load);
        }
        bool hasWaitingLoad() const { return m_numberWaiting; }
        bool hasWalkedLoad() const { return m_numberWalked; }
        bool hasWaitingRoot(ModuleID root) const
        {
            bool waits = false;
            forEachOfRoot(root, [&](LoadInFlight& load) {
                waits |= load.isWaiting();
            });
            return waits;
        }
        LoadInFlight* find(JSPromise* promise) const { return m_byPromise.get(promise); }
        LoadInFlight* withNumber(uint64_t number) const { return number >= m_numberAtPlaceZero && number < numberBehindLast() ? m_inOrder[static_cast<size_t>(number - m_numberAtPlaceZero)].get() : nullptr; }
        uint64_t numberBehindLast() const { return m_numberAtPlaceZero + m_inOrder.size(); }

        bool isBeingLoaded(const LoadInFlight& load) const { return !load.isLoaded && !load.isKnownToBeSettled && load.promise->status() == JSPromise::Status::Pending; }
        unsigned numberBeingLoaded() const
        {
            lookAtPromisesThatNobodyTellsOf();
            return m_numberBeingLoaded;
        }
        bool hasModuleEvaluatedDuringLoad() const
        {
            lookAtPromisesThatNobodyTellsOf();
            return m_numberBeingLoadedThatSawFileRun;
        }
        bool hasPendingPromise() const
        {
            lookAtPromisesThatNobodyTellsOf();
            return m_size != m_settled.size();
        }

        LoadInFlight& append(VM& vm, JSPromise* promise, ModuleID root, SettlingOfLoad settling)
        {
            auto owned = makeUnique<LoadInFlight>();
            LoadInFlight& load = *owned;
            load.promise.set(vm, promise);
            load.root = root;
            load.numberOfFilesRunAtCall = m_numberOfFilesRun;
            load.number = m_numberAtPlaceZero + m_inOrder.size();
            m_inOrder.append(WTF::move(owned));
            m_byRoot.add(root, LoadsOfRoot { }).iterator->value.loads.append(&load);
            if (!m_byPromise.add(promise, &load).isNewEntry || settling == SettlingOfLoad::IsLookedFor)
                m_promisesThatNobodyTellsOf.append(&load);
            ++m_numberBeingLoaded;
            ++m_numberWaiting;
            ++m_size;
            return load;
        }

        void didWalk(LoadInFlight& load)
        {
            if (load.isWalked)
                return;
            if (load.isLoaded) {
                load.isWalked = true;
                return;
            }
            --m_numberWaiting;
            ++m_numberWalked;
            load.isWalked = true;
            willMark(load);
        }

        void willMark(LoadInFlight& load)
        {
            if (load.isWalked && !load.isLoaded && !load.isOnList())
                m_walkedWithoutMarks.append(&load);
        }

        LoadInFlight* takeWalkedWithoutMarks()
        {
            if (m_walkedWithoutMarks.isEmpty())
                return nullptr;
            LoadInFlight& load = *m_walkedWithoutMarks.begin();
            load.remove();
            return &load;
        }

        void didLoad(ModuleID root)
        {
            auto found = m_byRoot.find(root);
            if (found == m_byRoot.end())
                return;
            LoadsOfRoot& ofRoot = found->value;
            for (; ofRoot.numberLoaded < ofRoot.loads.size(); ++ofRoot.numberLoaded) {
                LoadInFlight& load = *ofRoot.loads[ofRoot.numberLoaded];
                if (!load.isKnownToBeSettled)
                    didEndLoading(load);
                didEndWaiting(load);
                load.isLoaded = true;
            }
        }

        void didRunFile()
        {
            ++m_numberOfFilesRun;
            m_numberBeingLoadedThatSawFileRun = m_numberBeingLoaded;
        }

        void didSettle(JSPromise* promise)
        {
            LoadInFlight* load = find(promise);
            if (!load)
                return;
            if (promise->status() == JSPromise::Status::Pending) {
                if (!m_promisesThatNobodyTellsOf.contains(load))
                    m_promisesThatNobodyTellsOf.append(load);
                return;
            }
            noteSettled(*load);
        }

        void removeSettledIf(NOESCAPE const Invocable<bool(const LoadInFlight&)> auto& predicate)
        {
            lookAtPromisesThatNobodyTellsOf();
            m_settled.removeAllMatching([&](LoadInFlight* load) {
                if (!predicate(*load))
                    return false;
                takeOut(*load);
                return true;
            });
        }

        void clear()
        {
            while (takeWalkedWithoutMarks()) { }
            m_numberAtPlaceZero += m_inOrder.size();
            m_inOrder.clear();
            m_firstPlace = 0;
            m_byRoot.clear();
            m_byPromise.clear();
            m_promisesThatNobodyTellsOf.clear();
            m_settled.clear();
            m_size = 0;
            m_numberBeingLoaded = 0;
            m_numberBeingLoadedThatSawFileRun = 0;
            m_numberWaiting = 0;
            m_numberWalked = 0;
        }

    private:
        void didEndWaiting(LoadInFlight& load)
        {
            --(load.isWalked ? m_numberWalked : m_numberWaiting);
            if (load.isOnList())
                load.remove();
        }

        void didEndLoading(const LoadInFlight& load) const
        {
            --m_numberBeingLoaded;
            if (load.numberOfFilesRunAtCall != m_numberOfFilesRun)
                --m_numberBeingLoadedThatSawFileRun;
        }

        void noteSettled(LoadInFlight& load) const
        {
            if (std::exchange(load.isKnownToBeSettled, true))
                return;
            m_settled.append(&load);
            if (!load.isLoaded)
                didEndLoading(load);
        }

        void lookAtPromisesThatNobodyTellsOf() const
        {
            if (m_promisesThatNobodyTellsOf.isEmpty()) [[likely]]
                return;
            m_promisesThatNobodyTellsOf.removeAllMatching([&](LoadInFlight* load) {
                if (load->promise->status() == JSPromise::Status::Pending)
                    return false;
                noteSettled(*load);
                return true;
            });
        }

        void takeOut(LoadInFlight& load)
        {
            if (!load.isLoaded)
                didEndWaiting(load);
            auto ofRoot = m_byRoot.find(load.root);
            size_t place = ofRoot->value.loads.find(&load);
            ofRoot->value.loads.removeAt(place);
            if (place < ofRoot->value.numberLoaded)
                --ofRoot->value.numberLoaded;
            if (ofRoot->value.loads.isEmpty())
                m_byRoot.remove(ofRoot);
            if (auto ofPromise = m_byPromise.find(load.promise.get()); ofPromise != m_byPromise.end() && ofPromise->value == &load)
                m_byPromise.remove(ofPromise);
            --m_size;
            m_inOrder[static_cast<size_t>(load.number - m_numberAtPlaceZero)] = nullptr;
            while (m_firstPlace < m_inOrder.size() && !m_inOrder[m_firstPlace])
                ++m_firstPlace;
            if (m_firstPlace * 2 < m_inOrder.size())
                return;
            m_inOrder.removeAt(0, m_firstPlace);
            m_numberAtPlaceZero += m_firstPlace;
            m_firstPlace = 0;
        }

        Vector<std::unique_ptr<LoadInFlight>, 2> m_inOrder;
        size_t m_firstPlace { 0 };
        uint64_t m_numberAtPlaceZero { 0 };
        struct LoadsOfRoot {
            Vector<LoadInFlight*, 1> loads;
            size_t numberLoaded { 0 };
        };
        UncheckedKeyHashMap<ModuleID, LoadsOfRoot> m_byRoot;
        UncheckedKeyHashMap<JSPromise*, LoadInFlight*> m_byPromise;
        mutable Vector<LoadInFlight*, 2> m_promisesThatNobodyTellsOf;
        mutable Vector<LoadInFlight*, 2> m_settled;
        SentinelLinkedList<LoadInFlight, BasicRawSentinelNode<LoadInFlight>> m_walkedWithoutMarks;
        unsigned m_numberWaiting { 0 };
        unsigned m_numberWalked { 0 };
        unsigned m_size { 0 };
        mutable unsigned m_numberBeingLoaded { 0 };
        mutable unsigned m_numberBeingLoadedThatSawFileRun { 0 };
        uint64_t m_numberOfFilesRun { 0 };
    };
    LoadsInFlight m_loadsInFlight;
    BitVector m_modulesWithUncompiledRecord;
    Vector<ModuleID, 4> m_modulesWhoseRecordsLoaderHolds;
    Vector<ModuleID, 4> m_modulesToRetireAfterAsyncEvaluation;
    BitVector m_modulesBelowWalkedRoots;
    struct PlanOfLoad : RefCounted<PlanOfLoad> {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PlanOfLoad);

        struct Found {
            ModuleID module;
            uint32_t importer;
            uint32_t laterRequests : 26;
            uint32_t isFile : 1;
            uint32_t leadsToFile : 1;
            uint32_t isAliased : 1;
            uint32_t isFetchedByHost : 1;
            uint32_t wasLoadedSince : 1;
            uint32_t isKnownToBeLeft : 1;
        };
        struct Level {
            uint32_t first;
            uint32_t laterRequests;
            uint32_t placeOfMostLaterRequests;
            bool hasFiles;
        };
        static constexpr uint32_t jobsOfLevel = 5;

        Vector<Found, 64> found;
        Vector<Level, 32> levels;
        Vector<uint32_t> knownToBeLeft;
        BitVector modulesComeTo;
        uint32_t levelsReached { 0 };
        uint32_t levelsRequested { 0 };
        uint32_t steps { 0 };
        uint32_t stepsOfWholeGraph { 0 };
        bool asksAcrossLevels { false };
        bool isWhole { true };

        std::span<Found> level(size_t depth) { return found.mutableSpan().subspan(levels[depth].first, (depth + 1 == levels.size() ? found.size() : levels[depth + 1].first) - levels[depth].first); }
    };
    bool isLeftToLoad(PlanOfLoad&, PlanOfLoad::Found&);
    struct LevelOfLoad {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LevelOfLoad);

        std::optional<ModuleID> next;
        Vector<ModuleID, 4> filesWithoutCode;
        RefPtr<PlanOfLoad> plan;
        uint32_t depth { 0 };
        uint32_t place { 0 };
        bool isAskedFor { false };
    };
    UncheckedKeyHashMap<ModuleID, std::unique_ptr<LevelOfLoad>> m_levelsOfLoads;
    bool hasGraphRun(const LevelOfLoad& level) const { return status(level.plan->found[0].module) >= ModuleStatus::EvaluatingAsync; }
    PlacesSeenByPlans& m_placesSeenByPlans;
    Vector<ModuleID, 4> m_rootsLoadedWithoutLoader;
    BitVector m_filesLoadedWithoutLoader;
    BitVector m_filesLoadedByRequire;
    BitVector m_filesThatRanInPreload;
    std::optional<Vector<ModuleID>> m_commonJSModules;
    std::optional<Vector<ModuleID>> m_dataModules;
    BitVector m_filesThatRanBeforeTestFile;
    BitVector m_dataModulesLoadedBeforeTestFile;
    BitVector m_entriesOwnedByTestFile;
    BitVector m_entriesRemovedForMock;
    BitVector m_modulesWithOrphanedFetch;
    BitVector m_entriesRemovedAfterTestFile;
    BitVector m_variantsEvictedByMock;
    BitVector m_variantsRegisteredByPreload;
    BitVector m_variantsFetchedByTestFile;
    BitVector m_importersEvictedWithVariants;
    Vector<std::pair<ModuleID, Vector<ModuleID, 2>>> m_scheduledVariantRestores;
    BitVector m_filesWithKnownLoad;
    BitVector m_filesLoadedSynchronously;
    BitVector m_filesWhoseNextLoadIsFirst;
    BitVector m_filesWhoseTextHostDropped;
    void noteModulesLoadedWith(ModuleID root);
    void noteLoadOfEachModuleWith(ModuleID root);
    bool m_loadsModulesIndividually { false };
    const MockOperations* m_mockOperations;
    struct AnswersOfHostOnOriginal {
        bool isGiven { false };
        bool staysLinked { false };
    };
    mutable UncheckedKeyHashMap<uint64_t, AnswersOfHostOnOriginal, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_importersAskedAboutOriginal;
    struct GotOriginal {
        uint32_t moment;
        bool isGivenItNoMore;
        uint32_t timesMocked;
        bool staysUnderActualKey { false };
        bool isGivenByHost { false };
    };
    mutable UncheckedKeyHashMap<uint64_t, GotOriginal, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_importersLinkedToOriginal;
    mutable UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_importersLinkedToMock;
    UncheckedKeyHashMap<ModuleID, uint32_t> m_timesMocked;
    struct ReexportersOfMocks {
        uint32_t activeMocksVersion { 0 };
        uint32_t numberOfModulesMocked { 0 };
        uint32_t numberOfRemovedMocksInUse { 0 };
        uint32_t numberOfModulesWithExportsLeftOut { 0 };

        bool operator==(const ReexportersOfMocks&) const = default;
    };
    ReexportersOfMocks m_stateOfReexportersOfMocks;
    BitVector m_reexportersOfMocks;
    BitVector m_originalsEvaluatedUnderMock;
    BitVector m_modulesThatRanBesideMockAlone;
    BitVector m_modulesRetainedByInFlightLoads;
    struct ImportersEnteredAboveMock {
        ModuleID mocked;
        ModuleID root;
        BitVector entered;
        BitVector passed;
        Strong<JSPromise> original { };
        bool loadHasWaitedForOriginal { false };
        bool mockHasFactory { false };
        bool isLeftWhenSweepEnds { false };
    };
    Vector<ImportersEnteredAboveMock, 2> m_importersEnteredAboveMocks;
    std::optional<ModuleID> m_rootWhoseLoadIsSwept;
    std::optional<ModuleID> m_lastSweptRoot;
    BitVector m_modulesPendingAfterSweep;
    std::optional<ModuleID> m_originalBeingEvaluated;
    bool needsOriginalBeforeFactory(ModuleID mocked) const { return m_modulesWithPendingMock.get(mocked.index()) && m_mocksRetriedAfterFailure.get(mocked.index()); }
    bool sweepAheadOfFactories(ModuleID root);
    bool isOriginalDelegatedToHost(ModuleID mocked, const BitVector& held, const BitVector& isOnPath);
    bool isDataFile(ModuleID module) const { return data(module).kind == ModuleKind::Data && m_table.fullPath(module).find('?') == notFound; }
    enum class OriginalInSweep : uint8_t { Evaluated, DelegatedToHost, Failed };
    bool displacesRequiredMock(ModuleID mocked);
    OriginalInSweep runOriginalInSweep(ModuleID mocked);
    JSObject* actualKeyNamespace(ModuleID mocked);
    bool isDeferredByCurrentSweep(ModuleID) const;
    const Vector<uint32_t>& importDepthsInSweep();
    bool isOriginalLoadedBySweep(ModuleID mocked);
    void rejectRequireInSweep(ModuleID target);
    Vector<ModuleID, 4> commonJSOriginalsInGraph(ModuleID root);
    std::optional<ModuleID> m_commonJSModuleBeforeFactories;
    Vector<uint32_t> m_importDepthsInSweep;
    std::optional<bool> evaluateCommonJSModulesBeforeFactories(ModuleID root);
    void evaluatePrecedingCommonJSModules(std::span<const ModuleID>);
    bool loadWaitsForOriginalBelow(ModuleID root) const;
    JSPromise* originalThatLoadWaitsForBelow(ModuleID root);
    bool m_evaluatesImportersBehindModulesThatHaveRun { false };
    bool m_evaluatesSynchronously { false };
    void evaluateImportersBehindModuleThatHasRun(ModuleID, BitVector& seen);
    void evaluateImportersBehindModuleThatHasRun(ModuleID, ModuleID root, const ModuleEvaluationStack&, BitVector& seen);
    template<typename Matches> void leaveImportersWhere(const Matches&);
    void endPassing(ImportersEnteredAboveMock&);
    void didBeginWalkOnEmptyStack(ModuleID root);
    bool isWalkOfOriginal(ModuleID mocked, ModuleID root, const ModuleEvaluationStack&) const;
    bool passImporterEnteredAboveMock(ModuleID, ModuleID root, const ModuleEvaluationStack&);
    BitVector m_originalsRetainedAfterMock;
    bool hasRunBesideMockAlone(ModuleID module) const { return m_modulesThatRanBesideMockAlone.get(module.index()) && !wasLoadedWithGraph(module); }
    void noteCallThatComesToModuleItself(ModuleID module)
    {
        if (!isMocked(module))
            m_modulesThatRanBesideMockAlone.clear(module.index());
    }
    BitVector m_originalsDueUnderMock;
    BitVector m_failedInstancesBesideMocks;
    BitVector m_modulesWithResetFailure;
    BitVector m_modulesThatOwnFactoryMayLoad;
    BitVector m_originalsBeingEvaluated;
    std::optional<ModuleID> m_moduleWhoseOwnNamespaceIsMade;
    std::optional<ModuleID> m_moduleEvaluatedAsOriginal;
    Vector<uint64_t> m_loadedGroups;
    BitVector m_modulesLoadedBesideGroups;
    Vector<uint32_t> m_momentsOfLoads;
    BitVector m_loadedInPreload;
    BitVector m_loadedByPreloadHooks;
    BitVector m_releasedLoadsOfPreloadHooks;
    BitVector m_modulesLoadedAsMocksAlone;
    BitVector m_libraryModulesAskedForInPreload;
    bool m_hasBegunTestFile { false };
    struct MockWithIntactOriginal {
        uint32_t momentLaid;
        uint32_t momentOfTestFile;
        bool isInPlace { false };
        std::optional<BitVector> exportsLeftOutBefore { };
        BitVector exportsHiddenBefore { };
        uint32_t momentThisWasLaid { 0 };
        bool namespaceIsHeld { false };
        bool isDisplaced { false };
        bool didFileRunBesideMock { false };
        bool wasLoadedByTestFile { false };
        bool replacesRequiredValue { false };
        BitVector filesThatHadRun { };
        bool wasInstalledAfterFailure { false };
    };
    Vector<std::pair<ModuleID, JSObject*>, 4> m_namespacesBeingMade;
    size_t m_outermostNamespaceReadWhileBeingMade { notFound };
    struct NamespaceThatLacksNames {
        ModuleID module;
        JSObject* object;
        size_t outermostRead;
    };
    Vector<NamespaceThatLacksNames, 4> m_namespacesThatLackNames;
    Vector<std::pair<JSObject*, UniquedStringImpl*>, 4> m_namesBeingReadOffListers;
    bool m_isBypassingMocks { false };
    struct CollectionOfNamesBehindStars {
        Vector<std::pair<JSObject*, IdentifierSet>, 1> namesOfEndedMocks;
    };
    CollectionOfNamesBehindStars* m_collectionOfNamesBehindStars { nullptr };
    UncheckedKeyHashMap<ModuleID, MockWithIntactOriginal> m_mocksWithIntactOriginal;
    std::span<const uint32_t> requiredNamesOfStatementsThatExport(ModuleID, const String& exportName) const;
    mutable UncheckedKeyHashMap<ModuleID, UncheckedKeyHashMap<String, Vector<uint32_t, 1>>> m_requiredNamesOfStatementsByExportName;
    BitVector m_facadesCapturedForMock;
    JSObject* m_copiesThatGettersGave { nullptr };
    std::optional<ModuleID> m_facadeReusingGetterValues;
    struct FirstMockInTestFile {
        uint32_t moment;
        uint32_t momentOfTestFile;
        bool hasEnded { false };
    };
    UncheckedKeyHashMap<ModuleID, FirstMockInTestFile> m_firstMocksInTestFile;
    BitVector m_modulesWithMomentOfMock;
    uint32_t m_testFileOfMomentsOfMocks { 0 };
    BitVector m_modulesOnceMockedAfterFailure;
    mutable std::optional<Vector<ModuleID>> m_modulesWithDeferredRequest;
    mutable std::optional<Vector<ModuleID>> m_uncompiledModules;
    const Vector<ModuleID>& modulesWithDeferredRequest() const;
    struct MomentLoadedAnew {
        uint32_t moment;
        uint32_t momentOfTestFile;
        ModuleID rootOfLoad;
    };
    UncheckedKeyHashMap<ModuleID, MomentLoadedAnew> m_momentsLoadedAnew;
    UncheckedKeyHashMap<ModuleID, Vector<uint32_t, 2>> m_earlierMomentsLoadedAnew;
    struct LinksBeforeLoad {
        Vector<std::pair<ModuleID, std::optional<MomentLoadedAnew>>, 8> moments;
        BitVector modulesReleasedByLoader;
        BitVector importersOfMockThatEnded;
        LinksBeforeLoad* outer { nullptr };
    };
    LinksBeforeLoad* m_linksBeforeLoadInFlight { nullptr };
    UncheckedKeyHashMap<ModuleID, uint32_t> m_momentsLinkedInRefusedLoads;
    enum class LinksOfLoad : bool { AreForgotten, AreMetByLaterLoads };
    void bringBackLinks(const LinksBeforeLoad&, LinksOfLoad);
    struct LoadThatMayFail {
        ModuleID root;
        bool hadRun;
    };
    struct LinksOfLoadThatMayFail {
        ModuleID root;
        LinksBeforeLoad before;
        Vector<LoadThatMayFail, 2> loadsThatMayFail;
        bool isHeldBy(ModuleID load) const { return loadsThatMayFail.containsIf([&](const LoadThatMayFail& each) { return each.root == load; }); }
    };
    Vector<LinksOfLoadThatMayFail, 1> m_linksOfLoadsThatMayFail;
    LinksOfLoadThatMayFail* linksOfLoadWalkedFor(ModuleID root);
    bool reachesWhatLoadLinkedAnew(const LinksOfLoadThatMayFail&, ModuleID root);
    void meetLinksOfLoadsThatMayFail(ModuleID root);
    bool holdsLinksOfLoadThatMayFail(ModuleID root) const;
    void hearWhetherImportThatLinkedAnewFails(ModuleID root, JSPromise*);
    void hearWhetherImportUnderMockFails(ModuleID root, JSPromise*);
    struct EndedMock {
        uint32_t momentLaid;
        uint32_t momentEnded;
        uint32_t momentOfTestFile;
        bool wasLaidInPreload;
        BitVector exportsLeftOut;
        bool isRetainedForLinkedImporters { false };
        std::optional<unsigned> placeOfMockThatWasPatched { };
    };
    UncheckedKeyHashMap<ModuleID, Vector<EndedMock, 2>> m_removedMocksInUse;
    UncheckedKeyHashMap<ModuleID, Vector<unsigned, 2>> m_endedMocksToAskAbout;
    void didGiveUpMomentLinked(uint32_t moment);
    void didGiveUpLinksBeforeLoad(const LinksBeforeLoad&);
    UncheckedKeyHashMap<uint32_t, Vector<std::pair<ModuleID, unsigned>, 1>> m_endedMocksSeenByMomentLinked;
    struct MomentSomeModuleMayLookAt {
        uint32_t moment;
        uint32_t momentLinked;
    };
    static const MomentSomeModuleMayLookAt* firstBetween(std::span<const MomentSomeModuleMayLookAt> sorted, uint32_t from, uint32_t until);
    Vector<MomentSomeModuleMayLookAt, 16> momentsAtWhichSomeModuleMayLook(uint32_t since) const;
    void releaseUnusedRemovedMocks();
    struct MomentMockWasLaid {
        uint32_t moment { 0 };
        uint32_t momentOfTestFile { 0 };
        bool isInPreload { false };
        bool hadNamespace { false };
        bool keepsActiveMock { false };
        bool importsWrapWhatRequireMapHolds { false };
        bool wasStoredByImportedFile { false };
        bool wasStoredDuringImport { false };
        ActiveMockUse activeMockUse { ActiveMockUse::None };
    };
    UncheckedKeyHashMap<ModuleID, MomentMockWasLaid> m_pendingMockCalls;
    BitVector m_unseenMocks;
    BitVector m_evictedOriginals;
    BitVector m_originalsFetchedByTestFiles;
    void didEvaluateOriginalUnderMock(ModuleID module)
    {
        m_originalsEvaluatedUnderMock.set(module.index());
        if (m_hasBegunTestFile)
            m_originalsFetchedByTestFiles.set(fileOf(module).index());
    }
    std::optional<BitVector> m_originalsRestoredAfterPass;
    BitVector m_requirersOfOwnOriginals;
    BitVector m_originalsRequiredByFactories;
    UncheckedKeyHashMap<ModuleID, MomentMockWasLaid> m_activeMockCalls;
    UncheckedKeyHashMap<ModuleID, uint32_t> m_testFilesInWhichMocksEnded;
    BitVector m_modulesWhoseEndedMocksOverlap;
    UncheckedKeyHashMap<ModuleID, MomentMockWasLaid> m_preloadMockCalls;
    MomentMockWasLaid momentOfNow();
    std::optional<unsigned> keepEndedMock(ModuleID fileOrModule, JSObject* mock, MomentMockWasLaid, uint32_t momentEnded = 0, BitVector&& exportsLeftOut = { });
    JSObject* withNamesOfPatchedMock(JSObject* patched, JSObject* patch);
    void keepNamespacesMadeUnder(ModuleID fileOrModule, ModuleID module, JSObject* endedMock, std::optional<unsigned> ended);
    JSValue emptyMockSlotValue(ModuleID fileOrModule) const;
    void noteMomentMockComesInPlace(ModuleID fileOrModule, JSObject* mock, bool stoodInPlace, bool keepsEarlierMockForImporters);
    void releaseRemovedPreloadMocks(ModuleID fileOrModule);
    void keepMockThatStoodInPlace(ModuleID fileOrModule, ModuleID module, JSObject* endedMock, BitVector&& exportsLeftOut);
    void bringBackNamespaceKeptFor(ModuleID fileOrModule, JSObject* mock);
    void giveBackNamespacesHandedOn(LoadedModule& mocked);
    bool isSomeLoadUnderWay() const;
    BitVector m_namespacesReplacedByMock;
    std::optional<UncheckedKeyHashMap<ModuleID, Vector<ModuleID, 2>>> m_modulesThatHandOnNamespaces;
    BitVector m_modulesOwedToImportersThatRan;
    bool m_isDueModuleBlocked { false };
    std::optional<ModuleID> m_moduleWhoseBodyMockCallRan;
    std::optional<ModuleID> m_moduleWithSettledFactory;
    BitVector m_modulesWithNothingOwedBelow;
    Vector<ModuleID, 4> m_mockedModulesPassedOverByWalks;
    BitVector m_modulesWithRemovedMock;
    BitVector m_modulesWithLingeringMock;
    BitVector m_modulesWhoseEndPutOffBringsNoMockOfPreloadBack;
    bool m_hasImportsBlockedOnFactories { false };
    bool m_isRemovingLingeringMocks { false };
    BitVector m_modulesWhoseMockOfPreloadIsPatched;
    BitVector m_restoredPreloadMocks;
    BitVector m_preloadedMocks;
    void recordPreloadedMocks();
    BitVector m_mocksOfPreloadThatCameBackBeforeAnyImport;
    BitVector m_modulesReleasedByLoader;
    BitVector m_importersOfMockThatEnded;
    BitVector m_modulesLoadedAnewWhoseNamesAreOwed;
    BitVector m_modulesWithNoNamesOwedBelow;
    Vector<Vector<uint32_t, 2>> m_importersByStatement;
    Vector<Vector<uint32_t, 2>> m_requirers;
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_requirersThatRunTimeAnswered;
    UncheckedKeyHashSet<uint64_t> m_requiresThatRunTimeAnswered;
    struct Requirers {
        ModuleIndexSet ofLoad;
        ModuleIndexSet ofMock;
    };
    ModuleIndexMap<Requirers> m_requirersByFile;
    ModuleIndexMap<ModuleIndexSet> m_filesByRequirer;
    UncheckedKeyHashMap<ModuleID, bool> m_mockedFilesWithReleasedRequirers;
    std::optional<UncheckedKeyHashSet<uint64_t>> m_variablesReexportedByUnlistedNames;
    BitVector m_filesRequiredBesidePatch;
    BitVector m_runsLoadedUnderActualKey;
    BitVector m_requirersThatRunAgain;
    BitVector m_originalsOfReplacedMocks;
    UncheckedKeyHashMap<ModuleID, uint32_t> m_refusedOriginals;
    ModuleIndexMap<ModuleIndexSet> m_originalsByRequirer;
    BitVector m_requirersUnderPendingMocks;
    BitVector m_importedOriginals;
    std::optional<ModuleID> m_loadCallingFactory;
    BitVector m_mocksImportedByFactories;
    ModuleIndexMap<ModuleIndexSet> m_displacedRunImporters;
    UncheckedKeyHashMap<ModuleID, ModuleID> m_entryParents;
    UncheckedKeyHashMap<ModuleID, ModuleID> m_parentsOfPreloadMocks;
    UncheckedKeyHashMap<ModuleID, Vector<ModuleID, 1>> m_testFilesRequiringESModules;
    UncheckedKeyHashMap<ModuleID, uint32_t> m_startMoments;
    BitVector m_evictedTestFiles;
    UncheckedKeyHashMap<ModuleID, uint32_t> m_mockEndMoments;
    UncheckedKeyHashMap<ModuleID, ModuleID> m_displacedEntryParents;
    BitVector m_modulesWithKnownParent;
    std::optional<ModuleID> m_replacedTestFile;
    UncheckedKeyHashMap<uint64_t, unsigned, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_requirersBypassingMock;
    Vector<Vector<uint32_t, 2>> m_takersOfNames;
    BitVector m_importersRetainedAboveMock;
    BitVector m_originalsKeptUnderMock;
    mutable UncheckedKeyHashMap<ModuleID, Vector<Vector<PlaceOfExport, 1>>> m_starExportProviders;
    mutable UncheckedKeyHashMap<ModuleID, std::unique_ptr<StarTargetsByName>> m_starTargetsByName;
    mutable UncheckedKeyHashMap<uint64_t, Vector<ModuleID, 4>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_reexportWays;
    mutable std::optional<UncheckedKeyHashMap<uint32_t, Vector<ModuleID, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> m_modulesWithSyntheticExport;
    std::optional<ModuleID> m_moduleWithDeferredJobs;
    std::optional<ModuleID> m_rootWhoseFilesRun;
    struct DeferredRequest {
        ModuleID file;
        Strong<JSCell> context;
        Strong<JSPromise> promise;
        bool useImportMap;
        Strong<JSCell> importerLoad { };
    };
    Vector<DeferredRequest> m_deferredRequests;
    struct DeferredImport {
        ModuleID file;
        Strong<JSPromise> promise;
    };
    Vector<DeferredImport> m_deferredImports;
    struct MockedImport {
        Strong<JSPromise> promise;
        ModuleID module;
        Strong<JSObject> namespaceBeforeImportChangedMock;
        unsigned serial { 0 };
        bool isBoundToMock { false };
        bool hasProvisionalView { false };
    };
    UncheckedKeyHashMap<JSPromise*, MockedImport> m_mockedImports;
    unsigned m_retainedImportNamespaceCount { 0 };
    unsigned m_lastMockedImportSerial { 0 };
    Deque<std::pair<unsigned, JSPromise*>> m_importsAwaitingFirstStep;
    struct LastDeedOnMock {
        unsigned lastCallInFrontOfIt { 0 };
        bool bringsBackFileThatHadRun { false };
        Strong<JSObject> mock;
        Strong<JSObject> viewOfMock;
    };
    UncheckedKeyHashMap<ModuleID, LastDeedOnMock> m_lastDeedsOnMocksUnderCallsOfImport;
    BitVector m_originalsEvaluatedWhenMocked;
    BitVector m_modulesWithRunningFactory;
    BitVector m_linkedInstances;
    BitVector m_modulesLinkedAfterEntryPoint;
    BitVector m_modulesReleasedSincePass;
    Vector<uint32_t> m_linkTestFiles;
    Vector<uint32_t> m_linkMoments;
    uint32_t m_numberOfReleases { 0 };
    BitVector m_modulesCachingInstances;
    uint32_t m_numberOfLinkFailures { 0 };
    ModuleID m_moduleWithCheckedNames;
    ModuleID m_lastLinkFailureImporter;
    Vector<ModuleID, 4> m_modulesLinkedBeforeFailures;
    BitVector m_modulesBeforeLinkFailures;
    std::optional<Vector<ModuleID>> m_checksOfNames;
    BitVector m_linkedByFailedLoads;
    UncheckedKeyHashMap<ModuleID, uint32_t> m_failedLoadTestFiles;
    Vector<ModuleID, 16> m_instancesLinkedByLoader;
    BitVector m_modulesWithPreloadInstance;
    BitVector m_restoredModules;
    bool m_hasMockEnded { false };
    BitVector m_originalsRestoredAtUnmock;
    bool m_isDisplacingOriginal { false };
    bool m_hasReleasedInstances { false };
    BitVector m_modulesWhoseFactoryWasCalled;
    struct FailureOfFactory {
        ModuleID mocked;
        Strong<Unknown> error;
        uint32_t moment { 0 };
        Vector<ModuleID, 2> rootsAwaitingOriginal;
    };
    Vector<FailureOfFactory> m_failuresOfFactories;
    Vector<ModuleID, 2> m_importsMadeByOwnFactory;
    BitVector m_mocksRetriedAfterFailure;
    BitVector m_modulesNeedingMockReinstall;
    BitVector m_mocksWithoutFactory;
    BitVector m_modulesWithMockLoadError;
    ModuleID m_currentRequireTarget;
    bool m_shouldContinuePastPendingFactories { false };
    std::optional<ModuleID> m_resumingDetachedLoad;
    bool m_letsGoOfMocksAtLinkFromSource { false };
    ModuleID m_requiringModule;
    ModuleID m_prerecordedRequireTarget;
    ModuleID m_prerecordedRequirer;
    ModuleID m_completedRequireTarget;
    ModuleID m_currentRequirer;
    bool m_isResumingFactoryChainImport { false };
    bool m_hasFactoryChainImportSource { false };
    ModuleID m_currentImportTarget;
    ModuleID m_currentImportCallerModule;
    ModuleID m_currentImportRequestedModule;
    uint32_t m_currentImportSequenceNumber { 0 };
    uint32_t m_delayedImportSequenceNumber { 0 };
    bool m_isDelayedImportFromDependency { false };
    bool m_isCurrentImportFromDependency { false };
    ModuleID m_factoryCallImportTarget;
    JSValue m_currentImportWaitState;
    BitVector m_modulesWithCachedMockError;
    bool m_loaderMayHoldLoadsThatFactoryEnded { false };
    BitVector m_failedLoadModules;
    std::optional<ModuleID> m_moduleThatProgramImportsNow;
    BitVector m_modulesWithDeferredMock;
    struct RejectedRequire {
        ModuleID required;
        ModuleID mocked;
        uint64_t call;
        bool isAlwaysRejected;
        friend bool operator==(const RejectedRequire&, const RejectedRequire&) = default;
    };
    Vector<RejectedRequire> m_rejectedRequires;
    UncheckedKeyHashMap<uint64_t, uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_callsOfRejectedRequires;
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_requiresRejectedAgain;
    struct DetachedLoad {
        ModuleID required;
        Strong<JSPromise> wait;
        bool keepsError;
        unsigned number;
        unsigned pendingFactoryCount;
        BitVector unmockedModules;
        Vector<std::pair<ModuleID, uint64_t>, 1> awaitedFactoryCalls;
        BitVector modulesToRetry;
        bool wasEndedByLaterMock { false };
        ModuleID orphanedFetchModule;
        bool wasAwaitedMockRemoved { false };
        Vector<ModuleID, 1> mocksRemovedSince;
        Vector<ModuleID, 1> laterRootsBelow;
        bool isForOriginal { false };
        bool isRefusedOriginal { false };
        bool wasEvaluatedByRequire { false };
        bool wasEvicted { false };
        bool hasWaitedForLoader { false };
        BitVector timedMocks;
        unsigned pendingLoaderWaits { 0 };
        bool isEvaluationDue { false };
    };
    bool hasEnded(const DetachedLoad& left) const { return left.wasEndedByLaterMock || (left.orphanedFetchModule && !hasOrphanedFetch(left.orphanedFetchModule)); }
    bool hasOnlyStaleFactoryCalls(const DetachedLoad&);
    void restoreRequiredValue(ModuleID required);
    Vector<ModuleID, 4> unevaluatedEvictedImporters(const DetachedLoad&) const;
    bool hasUnheardSettlement(ModuleID required) const;
    Vector<ModuleID, 2> evaluationOrderedRoots(const DetachedLoad&) const;
    unsigned loaderStepsToMock(ModuleID root, ModuleID mock) const;
    void startLoaderWaits(unsigned number);
    void evaluateDetachedLoad(unsigned number);
    void finishDetachedLoad(unsigned number);
    void finishDetachedLoadsUnder(ModuleID required);
    bool isUnmockedInDetachedLoad(ModuleID root, ModuleID module) const;
    bool isFetchedByPendingFactoryCall(ModuleID root, ModuleID mocked) const;
    const BitVector* m_modulesBeingRetried { nullptr };
    Vector<DetachedLoad> m_detachedLoads;
    BitVector m_unsettledLoadModules;
    unsigned m_lastDetachedLoadNumber { 0 };
    BitVector m_blockedRequiresWithError;
    BitVector m_modulesLoadedBeforeMockThatWaited;
    BitVector m_modulesEvaluatedBeforeMock;
    BitVector m_filesEvaluatedByOwnFactory;
    BitVector m_originalsRequiredFromSource;
    BitVector m_filesThatRanDuringMockThatOnlyWaited;
    BitVector m_modulesAskedForOwnNames;
    BitVector m_modulesUnderActualKey;
    BitVector m_modulesWithOriginalRestored;
    BitVector m_modulesWithoutTestFile;
    uint32_t m_loaderStepCount { 0 };
    UncheckedKeyHashMap<ModuleID, uint64_t> m_loadsThatHoldModules;
    uint64_t m_numberOfNextLoadToGoThrough { 0 };
    void goThroughLoadsOfEarlierJobs(uint64_t endNumber);
    LoadInFlight* loadBeingLoadedThatHolds(ModuleID) const;
    bool leadsToModuleLeftToRun(const ModuleRequestData&) const;
    bool leadsTo(ModuleID from, ModuleID to);
    bool asksForLastTextBelow(const LoadInFlight&, ModuleID);
    bool runsFirstBelow(ModuleID root, ModuleID);
    Vector<ModuleID, 2> modulesThatCloseCyclesBelow(ModuleID root);
    std::optional<ModuleID> rootOfRunningFileThatHoldsCycleBelow(ModuleID required);
    LoadInFlight* loadToEvaluateFirst(const LoadInFlight&);
    LoadInFlight* loadToSettleFirst(ModuleID);
    struct GraphDeferredForImports {
        Strong<JSCell> root;
        Strong<JSCell> payload;
        bool isDeferred;
        uint64_t numberOfLoad;
        uint64_t numberOfEarlierLoad;
    };
    Vector<GraphDeferredForImports, 2> m_graphsDeferredForImports;
    bool hasFileOfGraphBeingLoadedBegun() const;
    bool hasUnreadSource(ModuleID);
    JSPromise* startImportInLaterTurn(ModuleID, std::optional<int64_t> referrerAsyncOrder);
    struct QueuedFetch {
        uint64_t place;
        Strong<JSPromise> promise;
        ModuleID module;
        Strong<JSCell> requested;
        std::optional<int64_t> referrerAsyncOrder;
        std::optional<uint32_t> loaderStepsAtCall;
        uint32_t callSequenceNumber;
        Strong<Unknown> asyncContextOfCallWhoseMocksRunNow { };
        ModuleID importCallerModule { };
        uint32_t importSequenceNumber { 0 };
        uint64_t precedingDiskReads { 0 };
    };
    Deque<QueuedFetch, 2> m_queuedFetches;
    bool m_canJobsOfFileTellWhenTextsCome { false };
    std::optional<uint32_t> m_resumingCallLoaderSteps;
    std::optional<uint32_t> m_resumingCallSequenceNumber;
    void forgetMockLaidBehindCallWhoseTextHasCome(ModuleID);
    void walkInPlaceOfCallsHeldForTextOf(ModuleID file);
    uint32_t m_sourceArrivalCount { 0 };
    uint32_t m_numberOfTextsAskedFor { 0 };
    UncheckedKeyHashMap<ModuleID, uint32_t> m_turnsInWhichTextsCame;
    void noteThatTextHasCome(ModuleID);
    uint32_t turnInWhichTextCame(ModuleID module) const
    {
        uint32_t turn = m_turnsInWhichTextsCame.get(fileOf(module));
        return turn ? turn : m_sourceArrivalCount;
    }
    void enqueueFetch(JSPromise*, ModuleID, ModuleRecord* requested, std::optional<int64_t> referrerAsyncOrder, uint32_t turnOfRequest);
    bool doOtherJobsWait() const;
    struct BatchedFetch : BasicRawSentinelNode<BatchedFetch> {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(BatchedFetch);

        uint64_t place { 0 };
        uint32_t batchNumber { 0 };
        Strong<JSPromise> promise;
        ModuleID module;
        ModuleID file;
        Strong<JSCell> requestedRecord;
        std::optional<int64_t> referrerAsyncOrder;
        ImportCaller caller { ImportCaller::CompiledCode };
        uint32_t callSequenceNumber { 0 };
        uint64_t precedingDiskReads { 0 };

        bool isCall() const { return !!promise; }
        unsigned missingImports { 0 };
        UncheckedKeyHashSet<ModuleID> importedFiles;
        bool joinsEarlierCall { false };
    };
    class BatchedFetches {
        WTF_MAKE_NONCOPYABLE(BatchedFetches);
    public:
        BatchedFetches() = default;
        ~BatchedFetches() { clear(); }

        bool isEmpty() const { return m_inOrder.begin() == m_inOrder.end(); }
        const BatchedFetch& first() const { return *m_inOrder.begin(); }
        const BatchedFetch& last() const { return *--m_inOrder.end(); }
        bool containsCallOf(ModuleID file) const { return m_callsByFile.contains(file); }
        bool containsCallOf(ModuleID module, ModuleID file) const;
        bool isFetchedForImporters(ModuleID file) const { return m_importersByFile.contains(file); }
        void forEachCall(NOESCAPE const Invocable<void(BatchedFetch&)> auto& functor) const
        {
            for (auto& calls : m_callsByFile.values()) {
                for (BatchedFetch* call : calls)
                    functor(*call);
            }
        }
        bool containsCallIf(NOESCAPE const Invocable<bool(const BatchedFetch&)> auto& predicate) const
        {
            return std::ranges::any_of(m_callsByFile.values(), [&](const auto& calls) {
                return calls.containsIf([&](const BatchedFetch* call) { return predicate(*call); });
            });
        }

        void addCall(std::unique_ptr<BatchedFetch>);
        void addImport(std::unique_ptr<BatchedFetch>);
        void addImporter(BatchedFetch&, ModuleID imported);
        std::unique_ptr<BatchedFetch> takeFirst();
        Vector<BatchedFetch*, 1> takeImportersOf(ModuleID imported) { return m_importersByFile.take(imported); }
        void letWait(std::unique_ptr<BatchedFetch>);
        std::unique_ptr<BatchedFetch> takeWaiting(BatchedFetch&);
        void didTakeCall(BatchedFetch&);
        Vector<std::unique_ptr<BatchedFetch>> takeCalls();
        void clear();

    private:
        SentinelLinkedList<BatchedFetch, BasicRawSentinelNode<BatchedFetch>> m_inOrder;
        SentinelLinkedList<BatchedFetch, BasicRawSentinelNode<BatchedFetch>> m_waiting;
        UncheckedKeyHashMap<ModuleID, Vector<BatchedFetch*, 1>> m_callsByFile;
        UncheckedKeyHashMap<ModuleID, Vector<BatchedFetch*, 1>> m_importersByFile;
    };
    BitVector m_filesWithSource;
    bool hasImportsToFetch(ModuleID);
    void fetchImports(BatchedFetch& call, ModuleID importer, const BatchedFetch& arrived);
    std::optional<uint32_t> m_batchBeingFulfilled;
    bool m_isInJobsAfterArrival { false };
    uint32_t m_currentBatchNumber { 0 };
    Strong<JSPromise> m_promiseOfJobInWhichLoadAsksForOriginal;
    void letLoadInFlightAskForOriginal(ModuleID);
    void addImportAwaitingSource(ModuleID);
    bool hasLoaderSettledOriginalOf(ModuleID mocked);
    JSPromise* takeMissingMockFailure(ModuleID);
    bool takeImportWithRemovedMock(ModuleID);
    bool isInAnotherLoadGraph(ModuleID file);
    bool needsOriginalDespiteMock(ModuleID);
    JSPromise* ensureActualKeyLoad(ModuleID file);
    Vector<std::pair<ModuleID, Strong<JSPromise>>, 1> m_pendingActualKeyLoads;
    Vector<std::pair<ModuleID, Strong<JSPromise>>, 1> m_factoryBlockedFetches;
    BitVector m_modulesWithAbandonedFactory;
    bool isBlockedOnAbandonedFactory(ModuleID file) const;
    BitVector m_factoryBlockedRootsSnapshot;
    BitVector m_mockImportTargetsSnapshot;
    BitVector m_modulesWithPassthroughMock;
    bool isUnevaluatedAndUnmocked(ModuleID module)
    {
        for (ModuleID moduleOrFile : { module, fileOf(module) }) {
            if (status(moduleOrFile) >= ModuleStatus::Evaluating || activeMock(moduleOrFile) || m_modulesWithPendingMock.get(moduleOrFile.index()))
                return false;
        }
        return true;
    }
    bool isImportCallAfterTests() const { return m_didFinishTests && !m_activeSynchronousLoad && !m_instance.vm->aotState().requireRunsMocks; }
    bool shouldLeaveImportPending(ModuleID module) { return isImportCallAfterTests() && (m_modulesWithPassthroughMock.get(fileOf(module).index()) || isUnevaluatedAndUnmocked(module)); }
    bool hasLateImportAwaitingSource();
    bool hasOriginalGraphStartedEvaluating(ModuleID file);
    bool hasUnstartedActualKeyLoad();
    void pruneActualKeyLoads();
    Identifier actualKey(ModuleID file);
    bool m_didFinishTests { false };
    Vector<std::pair<ModuleID, Strong<JSPromise>>, 1> m_missingMockFailures;
    JSPromise* findMissingMockFailure(ModuleID module) const
    {
        for (const auto& load : m_missingMockFailures) {
            if (load.first == module && load.second->status() == JSPromise::Status::Rejected)
                return load.second.get();
        }
        return nullptr;
    }
    void makeCallAgain(BatchedFetch&);
    JSArray* createLineBehindCall(JSPromise* earlierCall);
    void settleInLine(JSArray* line, JSPromise*, JSPromise* loaded);
    BatchedFetches m_batchedFetches;
    bool isTextReadOnOtherThread(ModuleID file);
    void putCallsThatWaitApartInLine();
    void clearBatchedFetches();
    ModuleID fileOf(ModuleID moduleOrFacade) const { return m_table.moduleBehindFacade(moduleOrFacade).value_or(moduleOrFacade); }
    struct WalkForEvictedFiles {
        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> visitOrder;
        Vector<uint32_t, 32> unsettledFiles;
        uint32_t numberOfFilesToAskHostAbout { 0 };
        std::optional<bool> pluginsAnswerForNames;
        bool importsAreFollowed { false };
    };
    bool importsEvictedFile(ModuleID, WalkForEvictedFiles&, uint32_t& earliestVisitReached);
    bool importsEvictedFile(ModuleID);
    bool walkForEvictedFiles(ModuleID, WalkForEvictedFiles&);
    bool askHostLevelByLevel(ModuleID root, WalkForEvictedFiles&, BitVector& filesAsked);
    void followImportsOfModulesThatHaveRun();
    void followImportsAnewAtNextQuestion();
    void evaluateModulesOwedBelow(ModuleID root, bool hasImporterThatRan = false);
    bool evaluateModuleOwed(ModuleID);
    void passModuleThatHasRun(ModuleID, BitVector& isPassed);
    bool isSomethingDueWhereModulesArePassed() const { return !m_modulesOwedToImportersThatRan.isEmpty() || !m_modulesWithRemovedMock.isEmpty() || !m_modulesNeedingMockReinstall.isEmpty(); }
    void releaseRemovedMocksBelow(ModuleID root);
    void letStartsAskWhereSomethingIsDueBelow(ModuleID root);
    void delegateToHost(ModuleID file);
    void takeBackFromHost(std::span<const ModuleID> files);
    BitVector m_keysWithoutEntryBehindMock;
    BitVector m_rootsOfImportsThatFailedUnderMocks;
    BitVector m_loadRoots;
    BitVector m_fetchedESModules;
    BitVector m_modulesWithUnevictableFailure;
    BitVector m_mocksWhoseEntryProgramDeleted;
    BitVector m_keysWithFalsyEntryOfProgram;
    bool isChangedOnDisk(ModuleID file);
    void evictHostCachedSource(ModuleID file);
    bool writesNameThatHostMayAnswerFor(ModuleID file, WalkForEvictedFiles&);
    struct AnswersForNamesOfFile {
        PluginResolveResult answer { PluginResolveResult::Declined };
        Vector<uint32_t, 4> filesThatAnswersLeadAwayFrom;
        Vector<uint32_t, 4> filesInFrontOfThrow;
    };
    AnswersForNamesOfFile askHostAboutNamesWrittenBy(ModuleID file);
    bool mayBeAskedAbout(ModuleID file) const { return !m_filesAskedAbout.get(file.index()) && !m_filesWhoseGraphHasBegun.get(file.index()); }
    void forEachEvaluatingCommonJSModule(const Invocable<void(ModuleID)> auto&) const;
    void forEachEvaluatingModuleOrFacade(const Invocable<void(ModuleID)> auto&) const;
    bool isEvaluatedByLoader(ModuleID file) const;
    bool hasDeferredLoads(ModuleID moduleOrFacade) const;
    void resumeDeferredRequests(ModuleID file);
    void resumeDeferredImports(ModuleID file, uint8_t jobsUntilLoaded);
    void resumeDeferredLoadsWithoutImporter(ModuleID moduleOrFacade);
    void resumeDeferredLoadsOfFinishedFiles();
    bool hasKey(ModuleID file, const Identifier& key);
    std::optional<ModuleID> fileWithDeferredLoads(const Identifier& key);
    std::optional<ModuleID> m_rootAwaitingLoader;
    RefPtr<MicrotaskQueue> m_deferredJobs;
    VM::SynchronousModuleQueue* m_deferredLoaderSteps { nullptr };
    bool m_isStartingRuntimeModule { false };
    bool m_entryPointIsLinked { false };
};

class HostRequestScope {
public:
    ALWAYS_INLINE explicit HostRequestScope(Modules& modules)
        : m_currentHostRequest(modules.m_currentHostRequest, Modules::HostRequest::forUncompiledModule())
    {
    }

private:
    SetForScope<Modules::HostRequest> m_currentHostRequest;
};

Modules& modulesOf(Instance&);
Modules* modulesOf(JSGlobalObject*);
void forgetUnfinishedLoadsOfRetiredRealms(VM&);

class MockDoor {
    WTF_MAKE_NONCOPYABLE(MockDoor);
public:
    MockDoor(Modules*, ModuleID, ASCIILiteral door);
    ~MockDoor();

private:
    Modules* m_modulesOfMock;
    Modules* m_modules;
    ModuleID m_module;
    ASCIILiteral m_door;
    String m_before;
};

ALWAYS_INLINE StoredModuleStatus Modules::storedStatus(ModuleID module) const
{
    RELEASE_ASSERT(module.index() < m_storedStatuses.size());
    uint8_t stored = m_storedStatuses[module.index()];
    if (!stored && data(module).onlyPassesLinkOn() && isLinkedWithLeader(module))
        return StoredModuleStatus::Linked;
    return static_cast<StoredModuleStatus>(std::min(stored, static_cast<uint8_t>(StoredModuleStatus::Evaluating)));
}

ALWAYS_INLINE void Modules::unsettleForLinks(ModuleID module)
{
    m_modulesSettledForLinks[module.index() / 64] &= ~(1ull << (module.index() % 64));
    ++m_linkEpoch;
}

ALWAYS_INLINE void Modules::setStoredStatus(ModuleID module, StoredModuleStatus status)
{
    RELEASE_ASSERT(module.index() < m_storedStatuses.size());
    if (status == StoredModuleStatus::Evaluated && m_filesToLoadAgain.get(module.index())) [[unlikely]]
        return;
    m_storedStatuses[module.index()] = static_cast<uint8_t>(status);
    if (status == StoredModuleStatus::InRecord)
        didChangeStatusInRecord(module);
    else if (status == StoredModuleStatus::Unlinked)
        unsettleForLinks(module);
}

void Modules::forEachUnsettledModule(NOESCAPE const Invocable<void(ModuleID, ModuleStatus)> auto& functor) const
{
    std::span<const uint8_t> statuses = m_storedStatuses;
    static_assert(static_cast<uint8_t>(StoredModuleStatus::Evaluating) == 4);
    auto visitStored = [&](size_t start, size_t end) {
        for (size_t index = start; index < end; ++index) {
            if (statuses[index] >= static_cast<uint8_t>(StoredModuleStatus::Evaluating))
                functor(ModuleID(index), ModuleStatus::Evaluating);
        }
    };
    size_t start = 0;
    for (; start + 4 * sizeof(uint64_t) <= statuses.size(); start += 4 * sizeof(uint64_t)) {
        std::array<uint64_t, 4> words;
        memcpySpan(asMutableByteSpan(words), statuses.subspan(start, sizeof(words)));
        if ((words[0] | words[1] | words[2] | words[3]) & 0xfcfcfcfcfcfcfcfcull) [[unlikely]]
            visitStored(start, start + sizeof(words));
    }
    visitStored(start, statuses.size());
    Vector<size_t, 16> settled;
    for (size_t index : m_recordsNotSeenEvaluated) {
        ModuleID module(index);
        if (storedStatus(module) != StoredModuleStatus::InRecord) {
            settled.append(index);
            continue;
        }
        ModuleStatus current = status(module);
        if (current == ModuleStatus::Evaluated)
            settled.append(index);
        else
            functor(module, current);
    }
    for (size_t index : settled)
        m_recordsNotSeenEvaluated.quickClear(index);
}

ALWAYS_INLINE ModuleStatus Modules::status(ModuleID module) const
{
    RELEASE_ASSERT(module.index() < m_storedStatuses.size());
    switch (static_cast<StoredModuleStatus>(std::min(m_storedStatuses[module.index()], static_cast<uint8_t>(StoredModuleStatus::Evaluating)))) {
    case StoredModuleStatus::Unlinked: {
        const ModuleData& moduleData = data(module);
        if (!moduleData.needsLink() || (m_entryPointIsLinked && moduleData.isLinkedWithEntryPoint() && !m_modulesLinkedAfterEntryPoint.get(module.index())))
            return ModuleStatus::Linked;
        return moduleData.onlyPassesLinkOn() && isLinkedWithLeader(module) ? ModuleStatus::Linked : ModuleStatus::Unlinked;
    }
    case StoredModuleStatus::Linked:
        return ModuleStatus::Linked;
    case StoredModuleStatus::Evaluating:
        return ModuleStatus::Evaluating;
    case StoredModuleStatus::Evaluated:
        return ModuleStatus::Evaluated;
    case StoredModuleStatus::InRecord:
        return statusInRecord(module);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE auto Modules::loadedIfExists(ModuleID module) const -> LoadedModule*
{
    if (m_loadedFoundLast && m_moduleOfLoadedFoundLast == module)
        return m_loadedFoundLast;
    LoadedModule* loaded = m_loaded.get(module);
    if (loaded) {
        m_loadedFoundLast = loaded;
        m_moduleOfLoadedFoundLast = module;
    }
    return loaded;
}

ALWAYS_INLINE JSValue Modules::settledAnswerOfRequire(ModuleID target, RequiredBy requiredBy) const
{
    if (requiredBy != RequiredBy::Referrer || !m_modulesWithPendingMock.isEmpty() || (!m_modulesLoadedAnewWhoseNamesAreOwed.isEmpty() && !m_modulesWithNoNamesOwedBelow.get(target.index())) || ((!m_modulesWithRemovedMock.isEmpty() || !m_modulesOwedToImportersThatRan.isEmpty() || !m_modulesNeedingMockReinstall.isEmpty()) && (!m_modulesWithNothingOwedBelow.get(target.index()) || hasMockEndedThatWalksPassedOver())) || !m_evictedFiles.isEmpty())
        return { };
    const LoadedModule* loaded = loadedIfExists(target);
    if (!loaded || !loaded->requiredValue || loaded->isBeingRequired || loaded->moduleObjectFromRequire || storedStatus(target) != StoredModuleStatus::Evaluated || data(target).isOfLibrary())
        return { };
    if (m_importersOfMockThatEnded.get(target.index())) [[unlikely]]
        return { };
    if (loaded->activeMock && host().shouldBypassMock) [[unlikely]]
        return { };
    return loaded->requiredValue.get();
}

ALWAYS_INLINE bool Modules::isLoadedModuleWithoutRecord(ModuleID module) const
{
    StoredModuleStatus stored = storedStatus(module);
    if (stored == StoredModuleStatus::Unlinked || stored == StoredModuleStatus::InRecord)
        return false;
    const ModuleData& moduleData = data(module);
    return moduleData.kind == ModuleKind::SourceText && !moduleData.isOfLibrary() && !moduleData.isFacade();
}

ALWAYS_INLINE bool Modules::isNotLoadedWithoutStatus(ModuleID module, const ModuleData& moduleData)
{
    if (m_entryPointIsLinked && moduleData.isLinkedWithEntryPoint() && !m_modulesLinkedAfterEntryPoint.get(module.index()))
        return false;
    if (moduleData.needsLink())
        return true;
    return !wasLoadedWithGraph(module);
}

ALWAYS_INLINE bool Modules::wasLoadedWithGraph(ModuleID module) const
{
    if (!data(module).isESModuleOfProgram())
        return false;
    uint32_t head = m_table.headOfLoadGroup(module).index();
    return (m_loadedGroups[head / 64] >> (head % 64) & 1) || m_modulesLoadedBesideGroups.get(module.index());
}

ALWAYS_INLINE uint32_t Modules::markOfEvaluationThatBegan(ModuleID module) const
{
    uint32_t noted = m_marksOfEvaluationsUnderOthers.isEmpty() ? 0 : m_marksOfEvaluationsUnderOthers[module.index()];
    return noted ? noted : static_cast<uint32_t>(StoredModuleStatus::Evaluating) + 1;
}

ALWAYS_INLINE ModuleRecord* Modules::recordIfExists(ModuleID module) const
{
    return storedStatus(module) == StoredModuleStatus::InRecord ? m_records.get(module) : nullptr;
}

struct MockOperations {
    bool (*runMocksInGraph)(Modules&, ModuleID root, Modules::LoadRootKind);
    bool (*runMocksInFetchedGraph)(Modules&, ModuleID);
    void (*releaseRemovedMock)(Modules&, ModuleID);
    bool (*runPendingMock)(Modules&, ModuleID);
    void (*removeUnusedLingeringMocks)(Modules&, ModuleID requested);
    std::optional<Modules::MockedReexport> (*findMockForReexport)(const Modules&, ModuleID reexportingModule, const ModuleExportData& reexportEntry, Modules::MockMode reexportingModuleMockMode);
    bool (*isMockedThroughStarExports)(Modules&, ModuleID, Modules::MockFilter);
};

ALWAYS_INLINE bool Modules::runMocksInGraph(ModuleID root, LoadRootKind rootOfLoad)
{
    return m_mockOperations && m_mockOperations->runMocksInGraph(*this, root, rootOfLoad);
}

ALWAYS_INLINE bool Modules::runMocksInFetchedGraph(ModuleID module)
{
    return m_mockOperations && m_mockOperations->runMocksInFetchedGraph(*this, module);
}

ALWAYS_INLINE void Modules::removeUnusedLingeringMocks(ModuleID requested)
{
    if (!m_modulesWithLingeringMock.isEmpty()) [[unlikely]]
        m_mockOperations->removeUnusedLingeringMocks(*this, requested);
}

ALWAYS_INLINE void Modules::releaseRemovedMock(ModuleID module)
{
    if (hasRemovedMock(module)) [[unlikely]]
        m_mockOperations->releaseRemovedMock(*this, module);
}

ALWAYS_INLINE bool Modules::runPendingMock(ModuleID module)
{
    return m_mockOperations->runPendingMock(*this, module);
}

ALWAYS_INLINE auto Modules::findMockForReexport(ModuleID reexportingModule, const ModuleExportData& reexportEntry, MockMode reexportingModuleMockMode) const -> std::optional<MockedReexport>
{
    if (!someMockIsSeen()) [[likely]]
        return std::nullopt;
    return m_mockOperations->findMockForReexport(*this, reexportingModule, reexportEntry, reexportingModuleMockMode);
}

ALWAYS_INLINE bool Modules::isMockedThroughStarExports(ModuleID module, MockFilter which)
{
    return m_mockOperations && m_mockOperations->isMockedThroughStarExports(*this, module, which);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
