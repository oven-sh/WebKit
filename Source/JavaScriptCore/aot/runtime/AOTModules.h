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
#include <wtf/HashMap.h>
#include <wtf/RefCounted.h>
#include <wtf/RefPtr.h>
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

class ModuleLinkWalk;
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
enum class JobsOfImportedFiles : uint8_t { StayQueued, WaitForLoader, StepsOfLoaderWaitForLoader };
enum class ErrorOfImportedFile : uint8_t { IsThrown, IsLeftToLoader };

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
    macro(JSObject*, moduleObjectOverExportsVariable) \
    macro(JSObject*, objectMadeByRequire) \
    macro(JSValue, exportsGivenToObjectMadeByRequire) \
    macro(JSObject*, firstRequirerOutsideImage) \
    macro(JSValue, requireFunction) \
    macro(JSValue, requiredValue) \
    macro(JSValue, valueInRequireCache) \
    macro(JSValue, valueInRequireCacheBeforeMock) \
    macro(JSValue, errorOfMockKeptByLoader) \
    macro(JSValue, errorOfModuleThatRanBesideMock) \
    macro(JSObject*, exportsOfMock) \
    macro(JSObject*, exportsBeforeMock) \
    macro(JSObject*, variablesBeforeMock) \
    macro(JSValue, valueBeforeMock) \
    macro(JSObject*, mockInPlace) \
    macro(JSObject*, recordBeforeMockInPlace) \
    macro(JSObject*, namespaceBeforeMockInPlace) \
    macro(JSObject*, capturedExportsBeforeMockInPlace) \
    macro(JSValue, requiredValueBeforeMockInPlace) \
    macro(JSValue, answerOfMockWhileOriginalRuns) \
    macro(JSValue, exportsOfOriginalBesideMock) \
    macro(JSObject*, exportsAddedByMock) \
    macro(JSObject*, endedMocks) \
    macro(JSObject*, namespacesOfEndedMocks) \
    macro(JSObject*, mockLaidInPreload) \
    macro(JSValue, whatRequireAnswersUnderMockLaidInPreload) \
    macro(JSValue, handedOnInPlaceOfNamespace) \
    macro(JSObject*, handedOnByMocks) \
    macro(JSValue, errorKeptByLoader) \
    macro(JSObject*, exportsWhileRequired) \
    macro(JSObject*, capturedExports) \
    macro(JSObject*, deferredExportsSource) \
    macro(JSCellButterfly*, capturedFields) \
    macro(JSCellButterfly*, capturedStars) \
    macro(JSObject*, sourceOfCapturedFields) \
    macro(AbstractModuleRecord*, uncompiledRecord)

struct EvaluationContext {
    ModuleEvaluationStack& stack;
    unsigned index { 0 };
    std::optional<int64_t> referrerAsyncOrder { };
    JSPromise* importPromise { nullptr };
};

struct MockedModules {
    static constexpr uint32_t notBelow = std::numeric_limits<uint32_t>::max();

    BitVector modules;
    Vector<uint32_t> positionBelow;
    uint32_t numberBelow { 0 };
    BitVector namedByListsAlone;
    UncheckedKeyHashMap<ModuleID, Box<BitVector>> reachedFromRoots;
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
    bool goesModuleByModule() const { return m_goesModuleByModule; }
    bool linkGoesByAllRequestsOf(ModuleID module) const { return m_goesModuleByModule || m_leadersWhoseLinkGoesByAllRequests.get(module.index()); }
    void didGetStateOfItsOwn(ModuleID) const;
    bool isSettledForLinks(ModuleID module) const { return (m_modulesSettledForLinks[module.index() / 64] >> (module.index() % 64) & 1) || checkIsSettledForLinks(module); }
    std::span<const uint64_t> modulesSettledForLinks() const { return m_modulesSettledForLinks.span(); }
    const ModuleData& data(ModuleID module) const { return m_table.data(module); }
    const ModuleHost& host() const;
    Identifier identifier(uint32_t number) const;
    ModuleStatus status(ModuleID) const;
    ModuleRecord* recordIfExists(ModuleID) const;
    ModuleRecord* ensureRecord(ModuleID);
    JSValue value(ModuleID) const;
    enum class MockOfModule : bool { Answers, IsReadPast };
    JSValue readExport(ModuleID, unsigned index, MockOfModule = MockOfModule::Answers);
    JSValue readExportIfInitialized(ModuleID, unsigned index) const;
    bool isExportAmbiguous(ModuleID, unsigned index) const;
    bool tryReplaceExport(ModuleID, UniquedStringImpl* name, JSValue);
    void fillExportsOfBodyThatNeverRuns(ModuleID);
    bool wasLoadedWithImporter(ModuleID) const;
    bool tryReplaceExport(ModuleID, UniquedStringImpl* name, JSValue, BitVector& seenBehindStars);
    bool tryReplaceExportBehindStars(ModuleID, UniquedStringImpl* name, JSValue, BitVector& seenBehindStars);
    bool tryReplaceCapturedExport(ModuleID, UniquedStringImpl* name, JSValue);
    bool tryReplaceExportAddedByMock(ModuleID, UniquedStringImpl* name, JSValue);
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
    void goModuleByModuleFromNowOn();
    void didEndMock(ModuleID);
    void didBeginTestFile()
    {
        m_hasBegunTestFile = true;
        forgetEndedMocksOfEarlierTestFiles();
    }
    void markEvaluated(ModuleID module) { setStatus(module, ModuleStatus::Evaluated); }
    bool isMocked(ModuleID module) const { return m_mocks && m_mocks->modules.get(module.index()); }
    JSObject* mockInPlace(ModuleID module) const
    {
        if (!m_numberOfMocksInPlace) [[likely]]
            return nullptr;
        const LoadedModule* loaded = loadedIfExists(module);
        return loaded ? loaded->mockInPlace.get() : nullptr;
    }
    bool mockInPlaceWasLaidInPreload(ModuleID module) const
    {
        JSObject* mock = mockInPlace(module);
        return mock && loadedIfExists(module)->mockLaidInPreload.get() == mock;
    }
    bool someMockStandsInPlace() const { return m_numberOfMocksInPlace; }
    bool someMockIsSeen() const { return m_numberOfMocksInPlace || !m_endedMocks.isEmpty(); }
    bool setMockInPlace(ModuleID, JSObject* mock, JSValue whatRequireAnswers, bool leavesEarlierMockToItsImporters = false);
    bool setMockInPlaceOfLibraryModule(ModuleID, JSObject* mock, JSValue whatRequireAnswers);
    bool willLeaveModuleAlone(ModuleID, bool onlyIfLoadedInPreload, std::optional<ModuleID> askingAsHostNamesIt);
    bool isGraphRunning() const { return m_innermostEvaluation || m_rootWhoseFilesRun || m_rootWhoseFilesRanAhead || m_instance.vm->synchronousModuleQueue(); }
    bool hasLinkedImporterYetToRun(ModuleID, std::optional<ModuleID> askingAsHostNamesIt) const;
    bool keepsStatusOfRunBeforeLoadThatRuns(ModuleID) const;
    BitVector modulesThatWerePassedWithoutStatus() const;
    uint32_t momentOfFirstMockInTestFile(ModuleID);
    void noteEndOfLoadedMock(ModuleID);
    void countMocksAnewBehindLoad(ModuleID);
    bool isLeftAloneByMock(ModuleID fileOrFace) const { return !m_mocksOverModulesThatHaveRun.isEmpty() && m_mocksOverModulesThatHaveRun.contains(m_table.fileBehindFace(fileOrFace).value_or(fileOrFace)); }
    bool setMockOverModuleThatHasRun(ModuleID, JSObject* mock, JSValue whatRequireAnswers);
    void endMockOverModuleThatHasRun(ModuleID);
    bool seesMockOf(ModuleID mocked, ModuleID importer) const;
    JSObject* mockSeenBy(ModuleID mocked, ModuleID importer) const;
    std::optional<bool> isLeftOutInViewOf(ModuleID importer, ModuleID mocked, unsigned exportIndex) const;
    JSObject* namespaceSeenBy(ModuleID module, ModuleID importer);
    std::optional<unsigned> indexOfEndedMockSeenAt(ModuleID fileOrModule, uint32_t moment) const;
    std::optional<unsigned> indexOfEndedMockSeenBy(ModuleID fileOrModule, ModuleID importer) const;
    JSObject* endedMock(ModuleID fileOrModule, std::optional<unsigned> index) const;
    uint32_t momentLinked(ModuleID importer) const;
    void forgetEndedMocksOfEarlierTestFiles();
    void linkAnewWhoeverReadsEndedMockBelow(ModuleID root);
    JSValue handedOnInPlaceOfNamespace(const ModuleExportData&) const;
    bool tryReplaceNamespaceHandedOn(ModuleID handedOn, JSValue);
    void didReplaceNamespaceHandedOn(ModuleID handedOn);
    std::span<const ModuleID> modulesThatHandOnNamespaceOf(ModuleID handedOn);
    void letGoOfMockAtNextLoad(ModuleID);
    void bringBackMockOfPreloadThatWasPatched(ModuleID fileOrModule);
    void letGoOfMockThatEnded(ModuleID);
    void letGoOfMockThatEndedSlow(ModuleID);
    bool letGoOfMockOfLibraryModuleAtNextLoad(ModuleID);
    void letGoOfMocksThatEnded();
    bool hasMockThatEnded(ModuleID module) const { return m_modulesWithMockToLetGo.get(module.index()); }
    bool loadComesToModuleWhoseMockEnded(ModuleID) const;
    bool hasMockOfLibraryModule(ModuleID module) const { return m_libraryModulesWithMock.get(module.index()); }
    bool wasLoadedInThisTestFile(ModuleID) const;
    enum class ImportersLetGo : bool { All, AllButThoseThatThisTestFileLinkedToModule };
    void letGoOfImportersOf(ModuleID mocked, ImportersLetGo);
    void letGoOfModuleLoadedAsMock(ModuleID module)
    {
        if (auto mock = m_mocksOverModulesThatHaveRun.find(module); mock != m_mocksOverModulesThatHaveRun.end() && mock->value.didFileRunBesideMock)
            return;
        m_modulesThatLoaderLetGo.set(module.index());
        m_importersOfMockThatEnded.set(module.index());
    }
    bool wasLetGoByLoader(ModuleID module) const { return m_modulesThatLoaderLetGo.get(module.index()); }
    void bringBackWhatLoaderLetGoBelow(ModuleID root);
    void evaluateModulesOwedBelowOriginal(ModuleID);
    bool hasMockEndedThatWalksPassedOver() const
    {
        for (ModuleID module : m_mockedModulesPassedOverByWalks) {
            if (!isMocked(module) && !mockInPlace(module))
                return true;
        }
        return false;
    }
    JSObject* namespaceOfModuleItself(ModuleID);
    bool isHiddenInModuleItself(ModuleID, unsigned index) const;
    JSValue readExportOfModuleItself(ModuleID, unsigned index);
    bool hasMockLaidOver(ModuleID) const;
    void takeNamesOfMockInPlace(ModuleID, JSObject* mock);
    void letRecordAnswerByMock(ModuleRecord*, JSObject* mock);
    void letGoOfRecordThatAnswersByMock(ModuleID);
    struct AnswerOfMock {
        JSObject* mock;
        Identifier name;
        ModuleID mocked;
    };
    std::optional<AnswerOfMock> mockThatAnswersFor(ModuleID passingOn, const ModuleExportData& passedOn, MockOfModule ofPassingOn = MockOfModule::Answers) const;
    std::optional<AnswerOfMock> mockThatAnswersForSlow(ModuleID passingOn, const ModuleExportData& passedOn, MockOfModule ofPassingOn) const;
    enum class MocksAskedFor : uint8_t { All, ThoseThatAddNames };
    bool isMockedInItsImage(ModuleID, MocksAskedFor = MocksAskedFor::All);
    bool isMockedInItsImageSlow(ModuleID, MocksAskedFor);
    void leaveOutWhatMockLacks(ModuleID, JSObject* exportsOfMock);
    void setExportsOfMockAreOfCommonJSModule(ModuleID, bool);
    void takeWhatMockAdds(ModuleID, bool comesAheadOfLoad);
    bool isAddedByMock(ModuleID, const Identifier&);
    void appendNamesAddedByMock(ModuleID, WTF::Vector<Identifier>&);
    void appendNamesBehindStarsOf(ModuleID, WTF::Vector<Identifier>&);
    bool hasNameBehindItsStars(ModuleID, UniquedStringImpl*);
    bool standsForWholeModule(ModuleID) const;
    struct SearchBehindStars {
        Vector<ModuleID, 8> path;
        BitVector isAnswered;
        BitVector isLeftOut;
        unsigned ringsMet { 0 };
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
        return moduleExport.isDroppedWhileEmpty && isVariableOfExportEmpty(module, exportIndex) && !mockThatAnswersFor(module, moduleExport);
    }
    bool isLeftOut(ModuleID module, unsigned exportIndex, TablesBehindStars tables) const
    {
        if (leavesNameToStars(module, exportIndex)) [[unlikely]]
            return true;
        if (m_exportsLeftOut.isEmpty() && m_endedMocks.isEmpty()) [[likely]]
            return false;
        SearchBehindStars search;
        return isLeftOut(module, exportIndex, tables, search);
    }
    class MocksBelowRoot {
    public:
        MocksBelowRoot(const ModuleTable& table, Box<MockedModules> mocks, ModuleID root)
            : m_table(table)
            , m_mocks(WTF::move(mocks))
            , m_root(root)
        {
        }

        bool hides(ModuleID module) const
        {
            if (!m_mocks || module == m_root) [[likely]]
                return false;
            if (m_mocks->namedByListsAlone.quickGet(module.index()))
                return true;
            uint32_t position = m_mocks->positionBelow[module.index()];
            return position != MockedModules::notBelow && !reachedFromRoot().quickGet(position);
        }

    private:
        const BitVector& reachedFromRoot() const;

        const ModuleTable& m_table;
        Box<MockedModules> m_mocks;
        ModuleID m_root;
        mutable Box<BitVector> m_reached;
    };
    MocksBelowRoot mocksBelow(ModuleID root) const { return { m_table, m_mocks, root }; }
    bool hasBegunToLoad(ModuleID) const;
    bool hostAnswersInPlaceOf(ModuleID, LoadByName);
    bool isKeptInRequireCache(ModuleID) const;
    bool isInRequireCache(ModuleID) const;
    bool isListedOnceStatementReaches(ModuleID) const;
    void takeOutOfRequireCache(ModuleID);
    Vector<ModuleID> libraryModulesInRequireCache();
    bool isLibraryModuleInRequireCache(ModuleID);
    std::optional<ModuleID> formThatMockFinds(StringView path, std::optional<ModuleID> file) const;
    void deferMock(ModuleID, AfterFailure);
    void forgetMockThatWaits(ModuleID);
    void endMockOfLibraryModule(ModuleID);
    void createDynamicExports(ModuleID, JSCellButterfly*& names, JSObject*& capturedValues, JSObject*& liveSources);
    void takeNamesBehindFollowedStarsAnew();
    void keepWhatMockIsLaidOver(ModuleID);
    void didLayMock() { m_moduleWithMockBeingLaid = std::nullopt; }
    void endMockLaidOverFile(ModuleID);
    JSValue exportsBeforeMock(ModuleID);
    bool runMockThatWaits(ModuleID);
    bool runMockThatWaitsSlow(ModuleID);
    void forgetWhatNamesResolvedTo();
    void willMockLibraryModule(ModuleID);
    bool letMockStandInPlaceOfLibraryModule(ModuleID);
    enum class RootOfLoad : bool { BeginsLoad, IsImportedByStatement };
    bool runMocksBelow(ModuleID root, RootOfLoad = RootOfLoad::BeginsLoad);
    bool runMocksBelowSlow(ModuleID root, RootOfLoad);
    bool wasLoadedWhenMockWasMade(ModuleID);
    bool runBodyOfLinkedModuleAheadOfMock(ModuleID);
    bool runBodyOfImportedModuleAheadOfMock(ModuleID);
    void runBodyOfRequiredFileAheadOfMock(ModuleID);
    bool isLoaderInMiddleOfGraph() const;
    bool isImportedByModuleThatRequireAnswered(ModuleID) const;
    void keepErrorOfLoad(ModuleID root, JSValue error, const ScopedLambda<bool(ModuleID)>& endedLoad);
    void dropErrorOfLoadEndedByMock(ModuleID);
    void dropErrorOfMockKeptByLoader(ModuleID);
    void dropErrorsOfLoadsEndedByMockOf(ModuleID mocked);
    void dropErrorsOfLoadsThatCameTo(const ScopedLambda<bool(ModuleID)>& isMocked);
    void dropErrorsOfLoadsEndedByMocks();
    void takeFailedLoadsOutOfLoader(const ScopedLambda<bool(ModuleID)>& isMocked);
    JSValue errorOfLoadEndedByMock(ModuleID module) const { return m_modulesWithLoadEndedByMock.get(module.index()) ? loadedIfExists(module)->errorKeptByLoader.get() : JSValue(); }
    bool mockFailedBehindImportStatement(ModuleID);
    bool runMocksBelowModuleThatLoaderFetches(ModuleID);
    bool runMocksBelowModuleThatLoaderFetchesSlow(ModuleID);
    bool isFetchedForImportStatement(ModuleID) const;
    JSPromise* factoryThatWaitsBelow(ModuleID root, FactoryAskedForBy, JSValue& state);
    void layMocksAgainAtNextLoad(ModuleID module, bool waits)
    {
        m_modulesThatLayMocksAgain.set(module.index(), waits);
        if (waits)
            m_modulesWithNothingOwedBelow.clearAll();
    }
    void layMocksOfBodyAgain(ModuleID);
    bool mocksWaitToBeLaidAgain() const;
    enum class AtMock : bool { GoesOn, Stops };
    BitVector reachedByStatements(ModuleID root, AtMock) const;
    bool importsByStatements(ModuleID importer, ModuleID imported) const;
    enum class MockState : uint8_t { None, Waits, Runs, InPlace, InPlaceWhileAnotherWaits, InPlaceOverModuleThatHasRun, Fills, LaidOver, Failed, Ended, EndedWhileAnotherWaits, NoneOfThem };
    MockState mockState(ModuleID);
    String describeMockState(ModuleID);
    void countMockStates(ASCIILiteral door, const String& before, ModuleID);
    void noteCallOfMock(ModuleID fileOrModule);
    void letMockWaitBehindLateFailure(ModuleID module) { m_modulesWithMockToRun.set(module.index()); }
    bool requireAsksFor(ModuleID module) const { return m_moduleThatRequireAsksFor == module; }
    bool importAsksFor(ModuleID module) const { return m_moduleThatImportAsksFor && fileOf(m_moduleThatImportAsksFor) == fileOf(module); }
    bool importThatWaitedAsksFor(ModuleID module) const { return m_importThatAsksHasWaited && importAsksFor(module); }
    void forgetCallOfMock(ModuleID fileOrModule);
    bool isBeingEvaluated(ModuleID module) const { return status(module) == ModuleStatus::Evaluating || status(module) == ModuleStatus::EvaluatingAsync; }
    static const MockOperations& mockOperations();
    JSObject* capturedExports(ModuleID);
    JSValue capturedValue(ModuleID, const Identifier&);
    bool hasCapturedExport(ModuleID, const Identifier&);
    bool isNativeModule(ModuleID) const;
    void forEachCapturedExport(ModuleID, const ScopedLambda<void(const Identifier&, JSValue)>&);
    static JSValue storedValue(JSGlobalObject*, JSObject* table, const Identifier& name);
    static void storeValue(JSGlobalObject*, JSObject* table, unsigned number, JSValue);
    JSObject* moduleNamespace(ModuleID, ModulePhase, bool shouldPreventExtensions = true);
    bool mayExport(ModuleID, UniquedStringImpl* name) const;
    JSObject* importMeta(ModuleID);
    JSCell* builtinCell(BuiltinID, ModuleID);
    JSString* pathString(ModuleID);
    JSString* resolve(ModuleID, ResolvedSpecifierForm);
    const SourceOrigin& sourceOrigin(ModuleID);
    void throwFailure(ModuleFailureID, bool isForRequire);
    void throwNewFailure(ModuleFailureID, bool isForRequire);
    std::optional<ModuleFailureID> loadFailure(ModuleID module) const { return m_table.loadFailure(module); }
    void throwExportNotFound(const RequiredNameData&);
    bool dropExportThatFindsNothing(ModuleID importer, const RequiredNameData&);
    void throwExportAmbiguous(const RequiredNameData&);
    String pathOfExporterAsLoaderNamesIt(const RequiredNameData&);
    void didRequestFromHost(ModuleID module)
    {
        if (!m_firstModuleRequestedByHost) [[unlikely]]
            noteFirstRequestFromHost(module);
        if (!m_entryThatStartsRealm && !m_hostRunsPreloads) [[unlikely]]
            m_entryThatStartsRealm = module;
    }
    void setHostRunsPreloads(bool runsPreloads) { m_hostRunsPreloads = runsPreloads; }
    std::optional<ModuleID> firstModuleRequestedByHost() const { return m_firstModuleRequestedByHost; }
    bool isHeldForImport(ModuleID) const;
    enum class ImportAnsweredBy : bool { Build, Loader };
    void runDataFileAgainForImport(ModuleID, ImportAnsweredBy);
    void didImportFromUncompiledCode(ModuleID module, bool isRootOfImportCall)
    {
        if (data(module).kind != ModuleKind::Data)
            return;
        if (isRootOfImportCall && !recordIfExists(module))
            return;
        m_dataFilesImportedByUncompiledCode.set(module.index());
    }
    void didImportFileFromUncompiledCode(ModuleID fileOrFace);
    bool isEntryPointOfRealm(ModuleID module) const { return m_firstModuleRequestedByHost == module; }
    bool takeStepOfMainModule(ModuleID module) { return isEntryPointOfRealm(module) && !std::exchange(m_didTakeStepOfMainModule, true); }
    void willBeLoadedByImport(ModuleID module) { m_didTakeStepOfMainModule |= isEntryPointOfRealm(module); }
    bool takeLoadPromiseBelowMainModule(ModuleID module) { return isEntryPointOfRealm(module) && !m_didTakeStepOfMainModule && !std::exchange(m_didTakeLoadPromiseBelowMainModule, true); }
    JSPromise* evaluateMainModule(ModuleID entryPoint, JSPromise* evaluation);
    bool mainModuleWaitsForItsImport() const { return !!m_capabilityOfMainModule; }
    void runMainModuleThatWaited();
    void failMainModuleThatWaited();
    bool refusesCodeOutsideImage() const { return !m_admitsCodeOutsideImage || m_instance.vm->aotState().askerLoadsCompiledFilesAlone; }
    bool loadsCompiledFilesAlone(ModuleID) const;
    const String* exchangeAskerOfModule(const String* asker) { return std::exchange(m_askerOfModule, asker); }
    bool isAskedByCodeOutsideImage() const { return m_askerOfModule && !m_askerOfModule->isEmpty() && !m_table.findByPath(*m_askerOfModule) && !isAskingHost(); }
    void forEachFileAwaitingAtTopLevel(const WTF::Function<void(int64_t turn, const String& key)>&);
    void evict(ModuleID, bool isUndoneForKeptFiles = false);
    void forEachFileThatFailed(const WTF::Function<void(ModuleID)>&) const;
    void willRunCodeThatKeepsFiles();
    void keepFilesThatHaveRun();
    void bringBackKeptFiles(const WTF::Function<void(const String&)>&);
    void forgetEvaluation(ModuleID);
    void forgetRunThatThrew(ModuleID);
    void didMakeTemplateObject(ModuleID, uint32_t offsetOfCell);
    void forgetRunOfLibraryModule(ModuleID);
    JSValue executeForLoader(ModuleID);
    bool wouldBeEvicted(ModuleID);
    bool wasLoadedByImport(ModuleID);
    bool mayBeEvicted(ModuleID module) const
    {
        const ModuleData& file = data(fileOf(module));
        return (file.kind != ModuleKind::SourceText && file.kind != ModuleKind::CommonJs) || file.mayBeEvicted();
    }
    void leaveToHostUnlessItHasRun(ModuleID);
    void takeOutOfLoader(ModuleID);
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
    void keepObjectMadeByRequire(ModuleID, JSObject* moduleObject);
    void forgetObjectMadeByRequire(ModuleID);
    bool holdsCodeThatMayBeEvicted();
    BitVector modulesThatHaveRun();
    bool isLeftToHost(ModuleID);
    bool isLeftToHostAsFarAsIsKnown(ModuleID);
    bool isChangedOutOfSightOfBuild(StringView path) const;
    bool isTextNamedByProgram(StringView url) const;
    bool refusesLoadsByName() const { return !m_loadsModulesByName && !m_runsUncompiledCode && !m_hostRunsCopiesFromDisk && !isAskingHost(); }
    bool isAskingHost() const { return m_isAskingHost && !hasEnteredCodeOfProgramSinceAskingHost(); }
    bool hasEnteredCodeOfProgramSinceAskingHost() const;
    bool isAskingHostForFileOfImage() const { return m_isAskingHost.isForFileOfImage() && isAskingHost(); }
    bool startsRealm(ModuleID module) const { return data(module).isEntryPoint() && (!m_entryThatStartsRealm || m_entryThatStartsRealm == module); }
    bool refusesRequestFromHost(ModuleID module) const { return (data(module).exportsStayInCompiledCode() && !startsRealm(module)) || (valueOfLibraryFileStaysWithCompiledCode(module) && !isAskingHostForFileOfImage()) || refusesRequestFromHostOutsideWindows(module); }
    bool valueOfLibraryFileStaysWithCompiledCode(ModuleID module) const { return data(module).registryID() && m_table.valueOfRuntimeSourceStaysWithCompiledCode(*data(module).registryID()); }
    bool refusesRequestFromHostOutsideWindows(ModuleID module) const { return !m_admitsInspector && !data(module).isEntryPoint() && data(module).kind != ModuleKind::Data && m_table.hasFilePath(module) && !data(module).isLibraryCodeWithFilePath() && !m_runsCopyFromDisk && !isAskingHostForFileOfImage(); }
    bool isRootThatCompiledCodeLoads(ModuleID module) const { return m_rootThatCompiledCodeLoads && fileOf(*m_rootThatCompiledCodeLoads) == fileOf(module); }
    bool refusesOriginalsBesideMocks() const { return m_refusesOriginalsBesideMocks; }
    void refuseToShowExportsOf(ModuleID);
    bool importsOriginalBesideMock(ModuleID mocked, ModuleID importer) const;
    bool hasImporterBelowThatGetsOriginal(ModuleID root, ModuleID fileOrFace) const;
    bool hasRunItself(ModuleID mocked) const;
    bool isMockedByFactoryThatTakesOriginal(ModuleID) const;
    bool getsOriginalBesideMock(ModuleID, ModuleID referrer);
    JSValue answerRequireWithOriginal(ModuleID file, ModuleID referrer);
    void runFileBesideMock(ModuleID file);
    void runOriginalsOwedTo(ModuleID importer);
    JSValue requireModuleItselfBesideMock(ModuleID);
    void oweBodyBesideMock(ModuleID);
    bool isAskedForByOwnFactory(ModuleID);
    void noteLoadBegunByFactories(ModuleID root);
    bool runsBesideMockFor(ModuleID mocked, std::optional<ModuleID> importer) const;
    void setAsideErrorOfModuleThatRanBesideMock(ModuleID);
    void bringBackErrorOfModuleThatRanBesideMock(ModuleID);
    JSValue importModuleItselfBesideMock(ModuleID, JSValue answerOfImport);
    JSObject* namespaceOfModuleThatRanBesideMock(ModuleID);
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
    void didChangeStatusInRecord(ModuleID module) { m_recordsNotSeenToHaveRun.set(module.index()); }
    void lookAtCycleOf(ModuleID, ModulesThatWaitForCycles&) const;
    const CyclesOfImports& cyclesOfImports() const;
    JSValue callBodyOfModuleEnteredByLoader(ModuleID module) { return callBodyUnderOwnMark(module, false); }
    void evaluateModuleInRecord(ModuleID);
    bool beginModuleInRecord(ModuleID);
    bool evaluateImportedCommonJSFiles(ModuleID root, JobsOfImportedFiles = JobsOfImportedFiles::StayQueued, ErrorOfImportedFile = ErrorOfImportedFile::IsThrown, ModuleGraphLoadingState* = nullptr);
    Vector<uint32_t, 16> placesOfFilesAsSynchronousLoadMakesThem(ModuleID root, const Vector<uint32_t, 16>& places);
    AbstractModuleRecord* recordOfRequestedModule(ModuleID);
    void queueJobsThatWaitForLoader();
    void handGraphsToLoader();
    void didLoadGraph(ModuleID root);
    void didFailToLoadGraph(ModuleID root);
    bool holdGraphBehindEarlierCalls(ModuleRecord&, JSCell* payload, bool isDeferred);
    void letGoGraphsHeldBehindEarlierCalls();
    void noteStepOfLoader() { ++m_stepsOfLoader; }
    void didSettlePromiseOfImport(JSPromise* promise) { m_loadsInFlight.didSettle(promise); }
    void runFileThatWaitsForItsTurn(ModuleID);
    bool isAskedForBehindLoadInFlight(ModuleID fileOrFace);
    void captureExportsAgain(ModuleID);
    void exportWhatMockNamesAlone(ModuleID file, bool hasRun);
    bool exportsWhatMockNamesAlone(ModuleID) const;
    void fillNamesThatMockLacks(ModuleID file);
    void answerFirstRequireBehindMock(ModuleID file);
    void noteLoadBehindMock(ModuleID fileOrFace, bool isRequired);
    bool wasRequiredUnderMockInPlace(ModuleID file) const;
    bool isRunningFilesOf(ModuleID root) const
    {
        for (const FilesBeingRun* files = m_innermostFilesBeingRun; files; files = files->outer) {
            if (files->root == root)
                return true;
        }
        return m_rootWhoseFilesRun == root;
    }
    bool isRunningFilesAheadOfLoader(ModuleID root) const
    {
        for (const FilesBeingRun* files = m_innermostFilesBeingRun; files; files = files->outer) {
            if (files->root == root)
                return !files->fileHandedToLoader;
        }
        return m_rootWhoseFilesRun == root;
    }
    bool stopLoadingBehindFile(ModuleID file);
    void evaluateLoadedCommonJSFile(ModuleID);
    void generateNativeModuleForLoader(ModuleID);
    void evaluateFilesOfLevel(ModuleID moduleOnWay, bool runsWhenLoaded);
    bool isCommonJSFileOrFace(ModuleID module) const { return data(module).kind == ModuleKind::CommonJs || m_table.fileBehindFace(module); }
    bool endsFilesThatRunAhead(const ModuleRequestData& request) const { return !request.isHoisted || data(request.targetModule()).kind == ModuleKind::SourceText; }
    bool isFileOfProgram(ModuleID module) const { return !data(module).isOfLibrary() && (data(module).kind == ModuleKind::SourceText || data(module).kind == ModuleKind::CommonJs); }
    bool isLoadedWithRequests(ModuleID module) const { return isFileOfProgram(module) && data(module).kind == ModuleKind::SourceText; }
    bool isImportedByLoader(ModuleID module) const { return isFileOfProgram(m_table.fileBehindFace(module).value_or(module)) || (data(module).kind == ModuleKind::Data && !data(module).isOfLibrary()); }
    bool isBuiltinModuleOfLoader(ModuleID) const;
    bool isBuiltinModuleNewToLoader(ModuleID) const;
    enum class LoaderForRequire : uint8_t { IsNotNeeded, ComesBehindImportedFiles, IsNeeded };
    LoaderForRequire loaderForRequire(ModuleID);
    bool canTellHowGraphIsLoaded(ModuleID root);
    void appendRequestsForLoader(ModuleID, ModuleRecord*, ModuleGraphLoadingState&);
    std::optional<ModuleFailureID> statementThatEndsLoad(ModuleID) const;
    void appendRequestThatFindsNothing(ModuleFailureID statement, ModuleRecord*);
    void keepFailureOfRequest(ModuleID, ModuleRecord*, const Identifier& name);
    ModuleRecord* appendRequestsOfUncompiledCode(ModuleID, ModuleRecord*);
    struct RequestsOfUncompiledCode;
    void collectRequestsOfUncompiledCode(ModuleID, RequestsOfUncompiledCode&);
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
    void noteLoadOfFile(ModuleID fileOrFace, LoadOfFile = LoadOfFile::HasComeToFile);
    bool asyncLoadMustWaitFor(ModuleID fileOrFace) const;
    bool importsFileBeingEvaluated(ModuleID root) const;
    bool importsFileThatRunsAhead(ModuleID root) const;
    bool importsOneOf(ModuleID root, std::span<const ModuleID> files) const;
    bool loadFurtherOutWaitsForFileBeside(ModuleID);
    void evaluateRootThatWaitedForFile(ModuleID root);
    std::optional<ModuleID> fileToWaitFor(const Identifier& key);
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
    ALWAYS_INLINE void setRequestToHostAside(bool isAskingHost) { m_requestsToHostSetAside.append(std::exchange(m_isAskingHost, RequestToHost(isAskingHost))); }
    void takeUpRequestToHost() { m_isAskingHost = m_requestsToHostSetAside.takeLast(); }
    bool isStatementAskingHost() const { return m_isStatementAskingHost; }
    bool setIsStatementAskingHost(bool isAsking) { return std::exchange(m_isStatementAskingHost, isAsking); }
    bool wasImportedWhileItRan(ModuleID file) const;
    bool loaderHasFileThatThrew(ModuleID file);
    bool isAwaitedByImport(ModuleID file);
    bool isAwaitedByImportFromBeforeItRan(ModuleID file);
    void dropErrorOfFile(ModuleID fileOrFace);
    void didLoseErrorOfModule(const Identifier& key);
    void evaluateCommonJSForLoader(ModuleID file);
    JSValue execute(ModuleID);

    enum class RequiredBy : uint8_t { Referrer, Program };
    JSValue require(ModuleID, ModuleID referrer = { }, RequiredBy = RequiredBy::Referrer);
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
    void letGoOfMockInRequireCache(ModuleID);
    void recordRequire(ModuleID, ModuleID referrer);
    void recordRequireFromOutsideImage(ModuleID, JSObject* moduleObjectOfRequirer);
    bool wasLoadedSynchronously(ModuleID file);
    bool areLoaderFunctionsStored() const { return !m_storedLoaderFunctions.isEmpty(); }
    void noteLoaderFunction(RequireOverride function, bool isStored) { m_storedLoaderFunctions.set(function, isStored); }
    bool wasFirstLoadedSynchronously(ModuleID file);
    bool hostLoadsSynchronously(ModuleID file) const;
    bool isFetchedInsideCall(ModuleID file) const;
    void noteTextsThatHostKeeps();
    void forgetFirstLoad(ModuleID);
    void willLoadCopyFromDisk(ModuleID, LoadByName);
    JSObject* moduleObject(ModuleID);
    std::optional<OptionSet<ModuleObjectTrait>> traitsOfModuleObjectYetToBeMade(ModuleID);
    void takeModuleObject(ModuleID, const ModuleObjects&);
    void giveBackModuleObject(ModuleID);
    JSObject* moduleObjectOverExportsVariableIfMade(ModuleID) const;
    void didLayExportsVariableOnModuleObject(ModuleID);
    JSObject* mainModuleObject(ModuleID);
    JSObject* parentModuleObject(ModuleID);
    bool mayHandOutModuleObject(ModuleID module) const { return data(module).kind != ModuleKind::CommonJs || data(module).hasValue(); }
    bool forEachNewChild(ModuleID, const ScopedLambda<void(JSObject*)>&);
    void dropNewChildren(ModuleID);
    enum class NamedByBuild : uint8_t { No, FileOfImage, WhatLiesOutsideImage };
    JSValue requireSpecifier(JSValue specifier, ModuleID referrer, NamedByBuild);
    JSValue resolveDynamic(JSValue specifier, ModuleID referrer, ResolvedSpecifierForm);

    JSPromise* import(ModuleID, ModuleID referrer, JSValue options = jsUndefined());
    enum class ImportCaller : bool { CompiledCode, Host };
    enum class TextOfImport : bool { MayComeLater, HasCome };
    JSPromise* startImport(ModuleID, std::optional<int64_t> referrerAsyncOrder, ImportCaller = ImportCaller::CompiledCode, TextOfImport = TextOfImport::MayComeLater, JSPromise* promiseOfCaller = nullptr, JSValue stateOfWait = JSValue(), std::optional<bool> answerOfWalkAlreadyMade = std::nullopt);
    JSPromise* rejectImportWithErrorOfLoad(ModuleID, ImportCaller);
    void startImportWhoseTextHasCome(JSPromise* importPromise, ModuleID, std::optional<int64_t> referrerAsyncOrder);
    bool holdTextOfRequest(JSPromise* fetched, ModuleRecord* requested, ModuleID requester);
    void deliverFirstTextOnItsWay();
    enum class AskedBy : bool { Loader, Import };
    bool textOfImportComesInLaterTurn(ModuleID, AskedBy);
    bool hasTextAtHand() const;
    void didRunJobsBehindDelivery() { m_isInJobsBehindDelivery = false; }
    bool loadsAsExecutableDoes() const;
    bool isTestRun() const;
    void holdTextOfCall(JSPromise*, ModuleID, ModuleRecord* requested, std::optional<int64_t> referrerAsyncOrder, ImportCaller);
    void willDeliverText() { ++m_turnOfDeliveries; }
    bool hasTextThatWaits() const { return !m_textsThatWait.isEmpty() || !m_textsOnTheirWay.isEmpty(); }
    std::optional<uint32_t> turnOfFirstTextThatWaits() const { return m_textsThatWait.isEmpty() ? std::nullopt : std::optional(m_textsThatWait[m_numberOfTextsThatHaveCome].turnOfCall); }
    bool deliverFirstTextThatWaits();
    bool deliverFirstTextInLine();
    void tellHostOfImport(JSPromise*, const String& path);
    JSPromise* importDynamic(JSValue specifier, JSValue options, ModuleID referrer);
    JSPromise* importUncompiled(const Identifier& key, ModuleID referrer, RefPtr<ScriptFetchParameters>&&, JSValue stateOfWait = { });
    void continueImport(JSPromise* importPromise, ModuleID, std::optional<int64_t> referrerAsyncOrder);
    void resolveWithNamespace(JSPromise* importPromise, ModuleID);
    void finishImportOfEvaluatedModule(JSPromise* importPromise, ModuleID, JSValue error);

    enum class RecordHolder : uint8_t { Anyone, Loader };
    ModuleRecord* exposeRecord(ModuleID, RecordHolder = RecordHolder::Anyone);
    void retireRecordsOfSettledLoads();
    AbstractModuleRecord* uncompiledRecord(ModuleID);
    AbstractModuleRecord* recordForResolution(ModuleID);
    enum class NamespaceUse : uint8_t { IsShownToProgram, CarriesBindings };
    JSObject* uncompiledNamespace(ModuleID, ModulePhase, NamespaceUse, bool shouldPreventExtensions = true);

private:
    friend class RequestToHostScope;

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
        bool isBeingRequired { false };
        bool isKeptByRequire { false };
        bool wasRequiredByHost { false };
        bool hasMockLaidOver { false };
        bool hadRunWhenMockWasLaid { false };
        Vector<ModuleID> namespacesReplacedByMock;
        Vector<ModuleID> mocksThatReplaceNamespace;
        bool capturedFieldsAreOfESModule { false };
        bool capturedFieldsAreOfRuntimeModule { false };
        Identifier resolvedKey;
        Identifier keyOfLatestLoad;
        bool wasLoadedWhileItRan { false };
        bool wasLoadedSynchronouslyWhileItRan { false };
        bool wasFetchedWhileItRan { false };
        bool isAnsweredWithNamespace { false };
        bool bindsEarlierCapture { false };
        bool didDropError { false };
        bool exportsWhatMockNamesAlone { false };
        bool takesNamesThatMockAdds { false };
        bool isYetToBeRequiredBehindMock { false };
        bool hadRunAheadOfMocksThatNameAlone { false };
        bool wasRequiredUnderMockInPlace { false };
        bool wasImportedUnderMockInPlace { false };
        bool exportsOfMockAreOfCommonJSModule { false };
        bool wasLoadedBehindMock { false };
        bool wasRequiredBehindMock { false };
        bool wasReachedBySecondLoad { false };
        bool isAwaitedByCallThatComesInLaterJob { false };
        bool isBeingLoadedByLoader { false };
        BitVector ambiguousExports;
        std::unique_ptr<SourceOrigin> sourceOrigin;
        std::optional<uint32_t> synchronousLoadCountAtStart;
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

    StoredModuleStatus storedStatus(ModuleID) const;
    bool isLinkedWithLeader(ModuleID) const;
    void setStoredStatus(ModuleID, StoredModuleStatus);
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
    Identifier moduleKey(ModuleID);
    std::optional<ModuleID> findByKey(const Identifier&) const;
    Identifier keyOfRequest(ModuleID target);
    JSObject* namespaceIfCreated(ModuleID, ModulePhase) const;
    JSObject* namespaceThatCompiledCodeHolds(ModuleID, ModulePhase) const;
    void setNamespace(ModuleID, ModulePhase, JSObject*);
    void letGoOfNamespaceMadeUnderMock(ModuleID);
    JSObject* createNamespace(ModuleID, ModulePhase, bool shouldPreventExtensions);
    Structure* namespaceStructure(bool shouldPreventExtensions);
    JSObject* createTableOfExports();
    JSModuleLoader* loader() const;

    enum class NativeModuleLoading : bool { Eager, Deferred };
    bool loadRuntimeModules(const ModuleLinkWalk&, NativeModuleLoading = NativeModuleLoading::Eager);
    void link(const ModuleLinkWalk&);
    void captureSyntheticExports(const ModuleLinkWalk&, VisitedModuleRecords&);
    void finishLink(const ModuleLinkWalk&, LinkContext&);
    void didLinkEntryPoint();
    bool didCaptureExports(ModuleID) const;
    void captureExports(ModuleID);
    void copyExportsOfFinishedFile(ModuleID face);
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
    void checkRequiredNames(std::span<const ModuleID> modules);
    bool isLeftOut(ModuleID, unsigned exportIndex, TablesBehindStars, SearchBehindStars&) const;
    bool namesBehindStarsAreInTables(ModuleID, const SearchBehindStars&) const;
    bool isVariableOfExportEmpty(ModuleID, unsigned exportIndex) const;
    bool isFoundInTextOfFile(const RequiredNameData& requiredName) const { return requiredName.mayBeAbsent && isCommonJSFileOrFace(requiredName.exporterModule()); }
    bool isDeferredExport(const LoadedModule&, const Identifier&) const;
    bool findsItsNamesInLibraryAlone(ModuleID) const;
    Vector<DynamicExport> collectDynamicExports(ModuleID);

    void evaluateWithOwnStack(ModuleID root);
    Vector<ModuleID, 1> m_modulesAskedForWhileTheyRan;
    void settleImportsOfModulesThatHaveRun(ModuleID rootOfEvaluation);
    size_t numberOfJobsThatWouldWait(JobsOfImportedFiles) const;
    void holdJobsBehind(size_t numberOfJobs, ModuleID file, JobsOfImportedFiles = JobsOfImportedFiles::WaitForLoader);
    bool hasQueuedTicks() const { return host().hasQueuedTicks && host().hasQueuedTicks(m_instance.globalObject); }
    struct LevelOfLoad;
    struct PlanOfLoad;
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
    JSValue callBodyUnderOwnMark(ModuleID, bool bodyStoresStatus, bool runsImportedFilesAlone = false);
    struct ModuleThatMayBegin {
        ModuleID module;
        uint8_t markBefore;
    };
    Vector<ModuleThatMayBegin, 8> noteModulesThatMayBeginUnder(uint8_t mark, ModuleID root);
    bool hasNotBegun(ModuleID module) const { return status(module) == ModuleStatus::Unlinked || status(module) == ModuleStatus::Linked; }
    uint8_t markOfEvaluationThatBegan(ModuleID) const;
    void evaluateByCallingBody(ModuleID module) { callBodyUnderOwnMark(module, true); }
    void didThrowInBody(ModuleID, JSValue error);
    void recordEvaluationIfNothingIsLeftToRun(ModuleID);
    bool isLeftToStartsOfImporters(ModuleID) const;

    std::optional<ModuleID> mainCommonJSModule();
    void decideMainCommonJSModule(std::optional<ModuleID>);
    void lookForMainCommonJSModule();
    void noteFirstRequestFromHost(ModuleID);
    void noteLoadOfGraph(ModuleID root);
    JSObject* moduleObjectMadeByRequire(ModuleID);
    JSValue answerRequire(ModuleID, ModuleID referrer = { }, RequiredBy = RequiredBy::Referrer);
    JSValue settledAnswerOfRequire(ModuleID, RequiredBy) const;
    void addToRequireCache(ModuleID, JSValue exports);
    void setRequireCacheValue(ModuleID, JSValue exports);
    JSValue requireThroughCache(ModuleID, ModuleID referrer);
    JSValue exportsOnObjectMadeByRequire(const LoadedModule&);
    JSValue answerRequireWithMock(ModuleID, ModuleID referrer);
    JSValue answerOfRequireToMock(ModuleID);
    bool isMockedLibraryModule(ModuleID);
    JSValue loadEvictedFile(ModuleID);
    JSValue readExportForRequire(JSObject* namespaceObject, const Identifier& name);
    void willRequireESModule(ModuleID);
    void markNamespaceAsRequired(ModuleID);

    std::optional<ModuleID> findByName(const Identifier& key);
    enum class FormWithoutAttributes : bool { MayAnswer, IsAnotherModule };
    std::optional<ModuleID> findWithAttributes(const Identifier& key, const ScriptFetchParameters*, FormWithoutAttributes = FormWithoutAttributes::MayAnswer);
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
    UncheckedKeyHashMap<ModuleID, std::unique_ptr<LoadedModule>> m_loaded;
    struct FormAnsweredFirst {
        Identifier name;
        ModuleID module;
    };
    ModuleMap<FormAnsweredFirst> m_formsAnsweredFirst;
    mutable LoadedModule* m_loadedFoundLast { nullptr };
    mutable ModuleID m_moduleOfLoadedFoundLast;
    RememberedSlotSet m_storedReferences;
    std::unique_ptr<ModuleLinkWalk> takeLinkWalk(ModuleID root);
    std::unique_ptr<ModuleLinkWalk> takeKeptLinkWalk(ModuleID root);
    void keepLinkWalk(std::unique_ptr<ModuleLinkWalk>&&);
    Vector<std::unique_ptr<ModuleLinkWalk>, 2> m_linkWalksOfLoads;
    bool checkIsSettledForLinks(ModuleID) const;
    mutable unsigned m_linkEpoch { 0 };
    mutable BitVector m_leadersWhoseLinkGoesByAllRequests;
    unsigned m_numberOfMocksInPlace { 0 };
    BitVector m_modulesWithMockInPlace;
    mutable Vector<uint64_t> m_modulesSettledForLinks;
    const EvaluationInProgress* m_innermostEvaluation { nullptr };
    uint32_t m_synchronousLoadCount { 0 };
    uint32_t m_activeSynchronousLoad { 0 };
    Vector<ModuleID, 2> m_rootsOfSynchronousLoads;
    Structure* m_namespaceStructure { nullptr };
    Structure* m_extensibleNamespaceStructure { nullptr };
    Identifier m_moduleExportsName;
    enum class RecordedRequire : uint8_t { Begun, Returned, BegunAgain, BegunAndMadeObjectOfESModule, ReturnedAndNotListed };
    UncheckedKeyHashMap<uint64_t, RecordedRequire> m_recordedRequires;
    std::optional<ModuleID> m_mainCommonJSModule;
    bool m_isMainCommonJSModuleDecided { false };
    bool m_didSearchBelowEntryPoint { false };
    Vector<ModuleID, 4> m_modulesFirstRequiredByCallsInProgress;
    Vector<FirstRequire> m_firstRequires;
    Vector<ModuleID, 2> m_firstFilesOfLoads;
    BitVector m_filesRequestedByLoader;
    struct FilesBeingRun {
        ModuleID root;
        std::optional<ModuleID> fileOrFace;
        FilesBeingRun* outer;
        bool isAheadOfJobsOfLoader { false };
        ModuleGraphLoadingState* load { nullptr };
        size_t numberOfJobsInFrontOfFile { 0 };
        bool isAheadOfStepsOfLoader { false };
        std::optional<ModuleID> fileHandedToLoader { };
        BitVector modulesOfLoad { };
        BitVector requested { };
        Vector<ModuleID> importersOfNextLevel { };
    };
    std::optional<ModuleID> fileOrFaceEvaluatedIn(const FilesBeingRun&) const;
    bool hasLoaderAskedFor(FilesBeingRun&, ModuleID file);
    void handGraphToLoader(FilesBeingRun&, std::optional<ModuleID> finishedFile = std::nullopt);
    void handSynchronousLoadToLoader(FilesBeingRun&);
    bool comesToLoadOf(FilesBeingRun&, ModuleID required);
    void runSynchronousStepsInFrontOf(ModuleID file);
    struct LoadHandedToLoader {
        ModuleID root;
        Strong<JSPromise> promise;
    };
    Vector<LoadHandedToLoader, 1> m_synchronousLoadsHandedToLoader;
    void captureNoExports(ModuleID file);
    void runStepsOfLoaderInFrontOf(ModuleID file);
    void finishStepInWhichFileRan(ModuleID fileOrFace, JSValue error);
    std::optional<ModuleID> m_fileBeingHandedToLoader;
    FilesBeingRun* m_innermostFilesBeingRun { nullptr };
    void noteRequestByLoader(ModuleID fileOrFace);
    bool wasRequestedByLoader(ModuleID file);
    void letGoOfModuleObject(ModuleID file);
    void layValueInPlaceOfExportsVariable(ModuleID file);
    void forgetTemplateObjects(ModuleID file);
    struct TemplateObjectOfFile {
        ModuleID file;
        uint32_t offsetOfCell;
    };
    Vector<TemplateObjectOfFile> m_templateObjectsOfFiles;
    void noteRequireOfFile(ModuleID file);
    BitVector m_modulesWithMarkOfRequire;
    BitVector m_modulesWithExtensibleNamespace;
    BitVector m_modulesWithEarlierNamespaceInValue;
    Vector<uint8_t> m_marksOfEvaluationsUnderOthers;
    Box<MockedModules> m_mocks;
    std::optional<ModuleID> m_moduleWithMockBeingLaid;
    OptionSet<RequireOverride> m_storedLoaderFunctions;
    std::optional<UncheckedKeyHashMap<uint32_t, SyntheticExportPlace, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> m_syntheticExportsByVariable;
    UncheckedKeyHashMap<ModuleID, BitVector> m_exportsLeftOut;
    BitVector m_evictedFiles;
    BitVector m_filesChangedOutOfSightOfBuild;
    BitVector m_filesAboveNoEvictedFile;
    BitVector m_filesAboveEvictedFile;
    BitVector m_filesAskedAbout;
    BitVector m_filesWhoseGraphHasBegun;
    uint64_t m_numberOfGraphsBegun { 0 };
    bool m_listsWithoutAsking { false };
    BitVector m_keptFiles;
    BitVector m_modulesThatRanBeforeCodeThatKeepsFiles;
    std::optional<bool> m_holdsCodeThatMayBeEvicted;
    BitVector m_filesToBringBack;
    BitVector m_filesEvictedWithImports;
    bool m_runsCopyFromDisk { false };
    bool m_hostRunsCopiesFromDisk { false };
    BitVector m_filesToLoadAgain;
    BitVector m_uncompiledNamespacesAskedFor;
    BitVector m_uncompiledNamespacesNotShownYet;
    BitVector m_modulesThatRanWithImporters;
    BitVector m_modulesSeenToHaveRun;
    BitVector m_recordsNotSeenToHaveRun;
    Vector<uint8_t> m_statusesWhenImportsWereFollowed;
    ModulesThatWaitForCycles m_modulesThatWaitedWhenImportsWereFollowed;
    BitVector m_modulesOfQueuedImports;
    BitVector m_modulesWithMockToRun;
    Vector<ModuleID> m_libraryModulesInRequireCache;
    BitVector m_libraryModulesTakenFromLoader;
    BitVector m_libraryModulesThatStatementsReached;
    mutable BitVector m_libraryModulesWithCheckedName;
    mutable BitVector m_libraryModulesKeptInRequireCache;
    BitVector m_libraryModulesWithMock;
    BitVector m_libraryModulesThatMockReplaces;
    UncheckedKeyHashMap<ModuleID, uint32_t> m_momentsMocksInPlaceOfLibraryModulesEnded;
    BitVector m_libraryModulesLeftByThrow;
    std::optional<ModuleID> m_firstModuleRequestedByHost;
    std::optional<ModuleID> m_entryThatStartsRealm;
    bool m_hostRunsPreloads { false };
    BitVector m_dataFilesImportedByUncompiledCode;
    bool m_didTakeStepOfMainModule { false };
    bool m_didTakeLoadPromiseBelowMainModule { false };
    ModuleID m_importOfMainModule;
    Strong<JSPromise> m_evaluationOfImportOfMainModule;
    Strong<JSPromise> m_capabilityOfMainModule;
    bool m_admitsCodeOutsideImage;
    const String* m_askerOfModule { nullptr };
    bool m_runsUncompiledCode { false };
    const bool m_admitsInspector;
    const bool m_showsWaysBetweenModuleObjects;
    const bool m_storesPathOfMainModule;
    bool m_loadsModulesByName;
    const bool m_refusesOriginalsBesideMocks;
    const bool m_showsEntriesOfRequireCache;
    class RequestToHost {
    public:
        ALWAYS_INLINE RequestToHost(bool isMade = false)
            : m_madeAt(isMade ? currentStackPointer() : nullptr)
        {
        }
        static ALWAYS_INLINE RequestToHost forWhatLiesOutsideImage()
        {
            RequestToHost request(true);
            request.m_isForFileOfImage = false;
            return request;
        }
        explicit operator bool() const { return m_madeAt; }
        bool isForFileOfImage() const { return m_isForFileOfImage; }
        bool wasMadeBeforeEntering(const void* frame) const { return frame < m_madeAt; }

    private:
        const void* m_madeAt;
        bool m_isForFileOfImage { true };
    };
    RequestToHost m_isAskingHost;
    Vector<RequestToHost, 4> m_requestsToHostSetAside;
    std::optional<ModuleID> m_rootThatCompiledCodeLoads;
    bool m_isStatementAskingHost { false };
    bool m_isWalkingRequests { false };
    struct LoadInFlight {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LoadInFlight);

        Strong<JSPromise> promise;
        ModuleID root;
        bool isWalked { false };
        bool hasMarksBelowRoot { false };
        bool isLoaded { false };
        bool isKnownToBeSettled { false };
        Strong<JSArray> callsThatSettleBehind;
        uint32_t stepsOfLoaderAtCall { 0 };
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
            for (LoadInFlight* load : found->value)
                functor(*load);
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
        bool hasSeenFileRunWhileBeingLoaded() const
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
            m_byRoot.add(root, Vector<LoadInFlight*, 1> { }).iterator->value.append(&load);
            if (!m_byPromise.add(promise, &load).isNewEntry || settling == SettlingOfLoad::IsLookedFor)
                m_promisesThatNobodyTellsOf.append(&load);
            ++m_numberBeingLoaded;
            ++m_size;
            return load;
        }

        void didLoad(ModuleID root)
        {
            forEachOfRoot(root, [&](LoadInFlight& load) {
                if (!load.isLoaded && !load.isKnownToBeSettled)
                    didEndLoading(load);
                load.isLoaded = true;
            });
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
        }

    private:
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
            auto ofRoot = m_byRoot.find(load.root);
            ofRoot->value.removeFirst(&load);
            if (ofRoot->value.isEmpty())
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
        UncheckedKeyHashMap<ModuleID, Vector<LoadInFlight*, 1>> m_byRoot;
        UncheckedKeyHashMap<JSPromise*, LoadInFlight*> m_byPromise;
        mutable Vector<LoadInFlight*, 2> m_promisesThatNobodyTellsOf;
        mutable Vector<LoadInFlight*, 2> m_settled;
        unsigned m_size { 0 };
        mutable unsigned m_numberBeingLoaded { 0 };
        mutable unsigned m_numberBeingLoadedThatSawFileRun { 0 };
        uint64_t m_numberOfFilesRun { 0 };
    };
    LoadsInFlight m_loadsInFlight;
    Vector<ModuleID, 4> m_modulesWhoseRecordsLoaderHolds;
    Vector<ModuleID, 4> m_modulesToRetireAfterAsyncEvaluation;
    BitVector m_modulesBelowWalkedRoots;
    struct PlanOfLoad : RefCounted<PlanOfLoad> {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PlanOfLoad);

        struct Found {
            ModuleID module;
            uint32_t importer;
            uint32_t laterRequests : 28;
            uint32_t isFile : 1;
            uint32_t leadsToFile : 1;
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
    Vector<uint32_t> m_placesSeenByPlans;
    uint32_t m_firstPlaceOfNextPlan { 1 };
    Vector<ModuleID, 4> m_rootsLoadedWithoutLoader;
    BitVector m_filesLoadedWithoutLoader;
    BitVector m_filesLoadedByRequire;
    BitVector m_filesWithKnownLoad;
    BitVector m_filesLoadedSynchronously;
    BitVector m_filesWhoseNextLoadIsFirst;
    BitVector m_filesWhoseTextHostDropped;
    void noteModulesLoadedWith(ModuleID root);
    void noteLoadOfEachModuleWith(ModuleID root);
    bool m_goesModuleByModule { false };
    const MockOperations* m_mockOperations;
    mutable UncheckedKeyHashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_importersAskedAboutOriginal;
    mutable UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_importersThatGotOriginal;
    BitVector m_modulesThatRanBesideMock;
    BitVector m_modulesOwedBesideMock;
    BitVector m_modulesThatOwnFactoryMayLoad;
    BitVector m_filesWhoseOriginalRuns;
    bool m_makesNamespaceOfModuleItself { false };
    Vector<uint64_t> m_loadedGroups;
    BitVector m_modulesLoadedBesideGroups;
    Vector<uint32_t> m_momentsOfLoads;
    BitVector m_loadedInPreload;
    bool m_hasBegunTestFile { false };
    struct MockOverModuleThatHasRun {
        uint32_t momentLaid;
        uint32_t momentOfTestFile;
        bool isInPlace { false };
        std::optional<BitVector> exportsLeftOutBefore { };
        BitVector exportsHiddenBefore { };
        uint32_t momentThisWasLaid { 0 };
        bool namespaceIsHeld { false };
        bool didFileRunBesideMock { false };
    };
    Vector<std::pair<ModuleID, JSObject*>, 4> m_namespacesBeingMade;
    UncheckedKeyHashMap<ModuleID, MockOverModuleThatHasRun> m_mocksOverModulesThatHaveRun;
    BitVector m_facesThatTookCopiesForMock;
    struct FirstMockInTestFile {
        uint32_t moment;
        uint32_t momentOfTestFile;
        bool hasEnded { false };
    };
    UncheckedKeyHashMap<ModuleID, FirstMockInTestFile> m_firstMocksInTestFile;
    struct MomentLoadedAnew {
        uint32_t moment;
        uint32_t momentOfTestFile;
        ModuleID rootOfLoad;
    };
    UncheckedKeyHashMap<ModuleID, MomentLoadedAnew> m_momentsLoadedAnew;
    struct EndedMock {
        uint32_t momentLaid;
        uint32_t momentEnded;
        uint32_t momentOfTestFile;
        bool wasLaidInPreload;
        BitVector exportsLeftOut;
    };
    UncheckedKeyHashMap<ModuleID, Vector<EndedMock, 2>> m_endedMocks;
    struct MomentMockWasLaid {
        uint32_t moment { 0 };
        uint32_t momentOfTestFile { 0 };
        bool isInPreload { false };
        bool hadNamespace { false };
    };
    UncheckedKeyHashMap<ModuleID, MomentMockWasLaid> m_momentsWaitingMocksWereLaid;
    UncheckedKeyHashMap<ModuleID, MomentMockWasLaid> m_momentsStandingMocksWereLaid;
    UncheckedKeyHashMap<ModuleID, uint32_t> m_testFilesInWhichMocksEnded;
    BitVector m_modulesWhoseEndedMocksOverlap;
    UncheckedKeyHashMap<ModuleID, MomentMockWasLaid> m_momentsMocksOfPreloadWereLaid;
    MomentMockWasLaid momentOfNow();
    std::optional<unsigned> keepEndedMock(ModuleID fileOrModule, JSObject* mock, MomentMockWasLaid, uint32_t momentEnded = 0, BitVector&& exportsLeftOut = { });
    void keepNamespacesMadeUnder(ModuleID fileOrModule, ModuleID module, JSObject* endedMock, std::optional<unsigned> ended);
    JSValue wordWhereNoMockStands(ModuleID fileOrModule) const;
    void noteMomentMockComesInPlace(ModuleID fileOrModule, JSObject* mock, bool stoodInPlace, bool leavesEarlierMockToItsImporters);
    void keepMockThatStoodInPlace(ModuleID fileOrModule, ModuleID module, JSObject* endedMock, BitVector&& exportsLeftOut);
    void bringBackNamespaceKeptFor(ModuleID fileOrModule, JSObject* mock);
    void giveBackNamespacesHandedOn(LoadedModule& mocked);
    bool isSomeLoadUnderWay() const;
    BitVector m_namespacesReplacedByMock;
    std::optional<UncheckedKeyHashMap<ModuleID, Vector<ModuleID, 2>>> m_modulesThatHandOnNamespaces;
    BitVector m_modulesOwedToImportersThatRan;
    BitVector m_modulesWithNothingOwedBelow;
    Vector<ModuleID, 4> m_mockedModulesPassedOverByWalks;
    BitVector m_modulesWithMockToLetGo;
    BitVector m_modulesWhoseMockOfPreloadIsPatched;
    BitVector m_modulesThatLoaderLetGo;
    BitVector m_importersOfMockThatEnded;
    Vector<Vector<uint32_t, 2>> m_importersByStatement;
    std::optional<ModuleID> m_fileWhoseJobsWaitForLoader;
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
    BitVector m_modulesThatHadRunWhenMocked;
    BitVector m_modulesWhoseFactoryRuns;
    Vector<ModuleID, 2> m_importsMadeByOwnFactory;
    BitVector m_modulesWithMockThatAsksAgain;
    BitVector m_modulesThatLayMocksAgain;
    BitVector m_modulesWithLoadEndedByMock;
    ModuleID m_moduleThatRequireAsksFor;
    ModuleID m_moduleThatImportAsksFor;
    bool m_importThatAsksHasWaited { false };
    BitVector m_mockedModulesWithErrorKeptByLoader;
    bool m_loaderMayHoldLoadsThatFactoryEnded { false };
    std::optional<ModuleID> m_moduleThatProgramImportsNow;
    BitVector m_modulesWithMockThatWaited;
    BitVector m_modulesLoadedBeforeMockThatWaited;
    BitVector m_filesThatRanAheadOfMock;
    uint32_t m_stepsOfLoader { 0 };
    UncheckedKeyHashMap<ModuleID, uint64_t> m_loadsThatHoldModules;
    uint64_t m_numberOfNextLoadToGoThrough { 0 };
    void goThroughLoadsOfEarlierJobs();
    LoadInFlight* loadBeingLoadedThatHolds(ModuleID) const;
    bool leadsToModuleLeftToRun(const ModuleRequestData&) const;
    bool leadsTo(ModuleID from, ModuleID to);
    bool asksForLastTextBelow(const LoadInFlight&, ModuleID);
    bool runsFirstBelow(ModuleID root, ModuleID);
    Vector<ModuleID, 2> modulesThatCloseCyclesBelow(ModuleID root);
    std::optional<ModuleID> rootOfRunningFileThatHoldsCycleBelow(ModuleID required);
    LoadInFlight* earlierLoadToRunFirst(const LoadInFlight&);
    LoadInFlight* earlierLoadToSettleFirst(ModuleID);
    struct GraphHeldBehindEarlierCall {
        Strong<JSCell> root;
        Strong<JSCell> payload;
        bool isDeferred;
        uint64_t numberOfLoad;
        uint64_t numberOfEarlierLoad;
    };
    Vector<GraphHeldBehindEarlierCall, 2> m_graphsHeldBehindEarlierCalls;
    bool hasFileOfGraphBeingLoadedBegun() const;
    bool isTextLeftToRead(ModuleID);
    JSPromise* startImportInLaterTurn(ModuleID, std::optional<int64_t> referrerAsyncOrder);
    struct TextOnItsWay {
        uint64_t place;
        Strong<JSPromise> promise;
        ModuleID module;
        Strong<JSCell> requested;
        std::optional<int64_t> referrerAsyncOrder;
        std::optional<uint32_t> stepsOfLoaderAtCall;
    };
    Deque<TextOnItsWay, 2> m_textsOnTheirWay;
    bool m_canJobsOfFileTellWhenTextsCome { false };
    std::optional<uint32_t> m_stepsOfLoaderAtCallWhoseTextHasCome;
    uint32_t m_turnOfTexts { 0 };
    uint32_t m_numberOfTextsAskedFor { 0 };
    UncheckedKeyHashMap<ModuleID, uint32_t> m_turnsInWhichTextsCame;
    void noteThatTextHasCome(ModuleID);
    uint32_t turnInWhichTextCame(ModuleID module) const
    {
        uint32_t turn = m_turnsInWhichTextsCame.get(fileOf(module));
        return turn ? turn : m_turnOfTexts;
    }
    void holdText(JSPromise*, ModuleID, ModuleRecord* requested, std::optional<int64_t> referrerAsyncOrder, uint32_t turnOfRequest);
    bool doOtherJobsWait() const;
    struct TextThatWaits {
        uint64_t place;
        uint32_t turnOfCall;
        Strong<JSPromise> promise;
        ModuleID module;
        Strong<JSCell> requestedRecord { };
        std::optional<int64_t> referrerAsyncOrder { };
        ImportCaller caller { ImportCaller::CompiledCode };
        uint32_t jobsBehindFirstCall { 0 };
    };
    Strong<JSPromise> m_promiseOfJobOfRound;
    bool m_isJobOfRoundQueued { false };
    uint32_t m_jobsBehindFirstCall { 0 };
    bool m_isInJobsBehindDelivery { false };
    uint32_t m_turnOfDeliveries { 0 };
    void queueJobOfRound(ModuleID moduleOfJob);
    void runJobOfRound(ModuleID moduleOfJob);
    Deque<TextThatWaits, 2> m_callsToMakeAgain;
    Strong<JSPromise> m_promiseOfJobThatMakesCallsAgain;
    uint32_t m_jobsBehindDelivery { 0 };
    void runJobThatMakesCallsAgain(ModuleID moduleOfJob);
    void makeCallAgain(TextThatWaits&);
    Vector<TextThatWaits> m_textsThatWait;
    size_t m_numberOfTextsThatHaveCome { 0 };
    std::span<TextThatWaits> textsThatWait() { return m_textsThatWait.mutableSpan().subspan(m_numberOfTextsThatHaveCome); }
    bool isTextReadOnOtherThread(ModuleID file);
    void putCallsThatWaitApartInLine();
    void forgetTextsThatWait();
    ModuleID fileOf(ModuleID fileOrFace) const { return m_table.fileBehindFace(fileOrFace).value_or(fileOrFace); }
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
    void evaluateModuleOwed(ModuleID);
    void passModuleThatHasRun(ModuleID, BitVector& isPassed);
    bool isSomethingDueWhereModulesArePassed() const { return !m_modulesOwedToImportersThatRan.isEmpty() || !m_modulesWithMockToLetGo.isEmpty() || !m_modulesThatLayMocksAgain.isEmpty(); }
    void letGoOfMocksThatEndedBelow(ModuleID root);
    void letStartsAskWhereSomethingIsDueBelow(ModuleID root);
    void leaveToHost(ModuleID file);
    bool isChangedOnDisk(ModuleID file);
    bool writesNameThatHostMayAnswerFor(ModuleID file, WalkForEvictedFiles&);
    struct AnswersForNamesOfFile {
        AnswerForName answer { AnswerForName::Declined };
        Vector<uint32_t, 4> filesThatAnswersLeadAwayFrom;
        Vector<uint32_t, 4> filesInFrontOfThrow;
    };
    AnswersForNamesOfFile askHostAboutNamesWrittenBy(ModuleID file);
    bool mayBeAskedAbout(ModuleID file) const { return !m_filesAskedAbout.get(file.index()) && !m_filesWhoseGraphHasBegun.get(file.index()); }
    void forEachFileEvaluatedByLoader(const Invocable<void(ModuleID)> auto&) const;
    void forEachFileOrFaceEvaluatedByLoader(const Invocable<void(ModuleID)> auto&) const;
    bool isEvaluatedByLoader(ModuleID file) const;
    bool hasDeferredLoads(ModuleID fileOrFace) const;
    void resumeDeferredRequests(ModuleID file);
    void resumeDeferredImports(ModuleID file, uint8_t jobsUntilLoaded);
    void resumeDeferredLoadsWithoutImporter(ModuleID fileOrFace);
    void resumeDeferredLoadsOfFinishedFiles();
    bool hasKey(ModuleID file, const Identifier& key);
    std::optional<ModuleID> fileWithDeferredLoads(const Identifier& key);
    std::optional<ModuleID> m_rootWhoseFilesRanAhead;
    RefPtr<MicrotaskQueue> m_jobsThatWaitForLoader;
    VM::SynchronousModuleQueue* m_stepsThatWaitForLoader { nullptr };
    bool m_isStartingRuntimeModule { false };
    bool m_entryPointIsLinked { false };
};

class RequestToHostScope {
public:
    ALWAYS_INLINE explicit RequestToHostScope(Modules& modules)
        : m_isAskingHost(modules.m_isAskingHost, Modules::RequestToHost::forWhatLiesOutsideImage())
    {
    }

private:
    SetForScope<Modules::RequestToHost> m_isAskingHost;
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

ALWAYS_INLINE void Modules::setStoredStatus(ModuleID module, StoredModuleStatus status)
{
    RELEASE_ASSERT(module.index() < m_storedStatuses.size());
    if (status == StoredModuleStatus::Evaluated && m_filesToLoadAgain.get(module.index())) [[unlikely]]
        return;
    m_storedStatuses[module.index()] = static_cast<uint8_t>(status);
    if (status == StoredModuleStatus::InRecord)
        didChangeStatusInRecord(module);
}

ALWAYS_INLINE ModuleStatus Modules::status(ModuleID module) const
{
    RELEASE_ASSERT(module.index() < m_storedStatuses.size());
    switch (static_cast<StoredModuleStatus>(std::min(m_storedStatuses[module.index()], static_cast<uint8_t>(StoredModuleStatus::Evaluating)))) {
    case StoredModuleStatus::Unlinked: {
        const ModuleData& moduleData = data(module);
        if (!moduleData.needsLink() || (m_entryPointIsLinked && moduleData.isLinkedWithEntryPoint()))
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
    if (requiredBy != RequiredBy::Referrer || !m_modulesWithMockToRun.isEmpty() || !m_modulesWithMockToLetGo.isEmpty() || ((!m_modulesOwedToImportersThatRan.isEmpty() || !m_modulesThatLayMocksAgain.isEmpty()) && (!m_modulesWithNothingOwedBelow.get(target.index()) || hasMockEndedThatWalksPassedOver())) || !m_evictedFiles.isEmpty())
        return { };
    const LoadedModule* loaded = loadedIfExists(target);
    if (!loaded || !loaded->requiredValue || loaded->isBeingRequired || loaded->objectMadeByRequire || storedStatus(target) != StoredModuleStatus::Evaluated || data(target).isOfLibrary())
        return { };
    if (m_importersOfMockThatEnded.get(target.index())) [[unlikely]]
        return { };
    if (loaded->mockInPlace && host().givesOriginalBesideMock) [[unlikely]]
        return { };
    return loaded->requiredValue.get();
}

ALWAYS_INLINE bool Modules::isLoadedModuleWithoutRecord(ModuleID module) const
{
    StoredModuleStatus stored = storedStatus(module);
    if (stored == StoredModuleStatus::Unlinked || stored == StoredModuleStatus::InRecord)
        return false;
    const ModuleData& moduleData = data(module);
    return moduleData.kind == ModuleKind::SourceText && !moduleData.isOfLibrary() && !moduleData.isFaceOfFile();
}

ALWAYS_INLINE bool Modules::isNotLoadedWithoutStatus(ModuleID module, const ModuleData& moduleData)
{
    if (m_entryPointIsLinked && moduleData.isLinkedWithEntryPoint())
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

ALWAYS_INLINE uint8_t Modules::markOfEvaluationThatBegan(ModuleID module) const
{
    uint8_t noted = m_marksOfEvaluationsUnderOthers.isEmpty() ? 0 : m_marksOfEvaluationsUnderOthers[module.index()];
    return noted ? noted : static_cast<uint8_t>(StoredModuleStatus::Evaluating) + 1;
}

ALWAYS_INLINE ModuleRecord* Modules::recordIfExists(ModuleID module) const
{
    return storedStatus(module) == StoredModuleStatus::InRecord ? m_records.get(module) : nullptr;
}

struct MockOperations {
    bool (*runMocksBelow)(Modules&, ModuleID root, Modules::RootOfLoad);
    bool (*runMocksBelowModuleThatLoaderFetches)(Modules&, ModuleID);
    void (*letGoOfMockThatEnded)(Modules&, ModuleID);
    bool (*runMockThatWaits)(Modules&, ModuleID);
    std::optional<Modules::AnswerOfMock> (*mockThatAnswersFor)(const Modules&, ModuleID passingOn, const ModuleExportData& passedOn, Modules::MockOfModule ofPassingOn);
    bool (*isMockedInItsImage)(Modules&, ModuleID, Modules::MocksAskedFor);
};

ALWAYS_INLINE bool Modules::runMocksBelow(ModuleID root, RootOfLoad rootOfLoad)
{
    return m_mockOperations && m_mockOperations->runMocksBelow(*this, root, rootOfLoad);
}

ALWAYS_INLINE bool Modules::runMocksBelowModuleThatLoaderFetches(ModuleID module)
{
    return m_mockOperations && m_mockOperations->runMocksBelowModuleThatLoaderFetches(*this, module);
}

ALWAYS_INLINE void Modules::letGoOfMockThatEnded(ModuleID module)
{
    if (hasMockThatEnded(module)) [[unlikely]]
        m_mockOperations->letGoOfMockThatEnded(*this, module);
}

ALWAYS_INLINE bool Modules::runMockThatWaits(ModuleID module)
{
    return m_mockOperations->runMockThatWaits(*this, module);
}

ALWAYS_INLINE auto Modules::mockThatAnswersFor(ModuleID passingOn, const ModuleExportData& passedOn, MockOfModule ofPassingOn) const -> std::optional<AnswerOfMock>
{
    if (!someMockIsSeen()) [[likely]]
        return std::nullopt;
    return m_mockOperations->mockThatAnswersFor(*this, passingOn, passedOn, ofPassingOn);
}

ALWAYS_INLINE bool Modules::isMockedInItsImage(ModuleID module, MocksAskedFor which)
{
    return m_mockOperations && m_mockOperations->isMockedInItsImage(*this, module, which);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
