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
#include <wtf/HashMap.h>
#include <wtf/RefCounted.h>
#include <wtf/RefPtr.h>
#include <wtf/SetForScope.h>
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
// While require() loads a graph the jobs of the program wait for its return in any case, and the steps of the loader are in a queue of their own.
enum class JobsOfImportedFiles : uint8_t { StayQueued, WaitForLoader, StepsOfLoaderWaitForLoader };
enum class ErrorOfImportedFile : uint8_t { IsThrown, IsLeftToLoader };

struct LinkContext {
    ModuleLinkingStack& stack;
    unsigned index { 0 };
    RefPtr<ScriptFetcher> scriptFetcher { };
};

// capturedFields: in place of capturedExports, until somebody asks for all the names. capturedStars: beside them, what the stars that wait on the exports would answer. Set with them, and null where no star waits.
#define FOR_EACH_REFERENCE_OF_LOADED_MODULE(macro) \
    macro(JSObject*, namespaceObject) \
    macro(JSObject*, deferredNamespaceObject) \
    macro(JSObject*, importMeta) \
    macro(JSObject*, requirer) \
    macro(JSObject*, objectMadeByRequire) \
    macro(JSValue, exportsGivenToObjectMadeByRequire) \
    macro(JSObject*, firstRequirerOutsideImage) \
    macro(JSValue, requireFunction) \
    macro(JSValue, requiredValue) \
    macro(JSValue, valueInRequireCache) \
    macro(JSValue, valueInRequireCacheBeforeMock) \
    macro(JSValue, errorOfMockKeptByLoader) \
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
    macro(JSValue, handedOnInPlaceOfNamespace) \
    macro(JSObject*, handedOnByMocks) \
    macro(JSValue, errorKeptByLoader) \
    macro(JSObject*, exportsWhileRequired) \
    macro(JSObject*, exportsShownBeforeBody) \
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

// A set of mocked modules, and what follows from it and the table alone. Whoever began with one set goes on with it.
struct MockedModules {
    static constexpr uint32_t notBelow = std::numeric_limits<uint32_t>::max();

    BitVector modules;
    // The mocked modules and what they reach are numbered. A way from a root to any other module passes no mock, so only a question about one of these takes a walk from the root.
    Vector<uint32_t> positionBelow;
    uint32_t numberBelow { 0 };
    // What no module asks for but in the list of a root, as the file of a module of the library: no walk comes to it.
    BitVector namedByListsAlone;
    // Which of the numbered ones a root reaches without passing a mock.
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
    // Moves on when the entry point is linked and when the exports of a file are captured again: a walk made before may pass over what has work now.
    unsigned linkEpoch() const { return m_linkEpoch; }
    // No link has anything left to do with the module.
    bool isSettledForLinks(ModuleID module) const { return (m_modulesSettledForLinks[module.index() / 64] >> (module.index() % 64) & 1) || checkIsSettledForLinks(module); }
    // A bit for each module that was found to be settled.
    std::span<const uint64_t> modulesSettledForLinks() const { return m_modulesSettledForLinks.span(); }
    const ModuleData& data(ModuleID module) const { return m_table.data(module); }
    const ModuleHost& host() const;
    Identifier identifier(uint32_t number) const;
    ModuleStatus status(ModuleID) const;
    ModuleRecord* recordIfExists(ModuleID) const;
    ModuleRecord* ensureRecord(ModuleID);
    JSValue value(ModuleID) const;
    // IsReadPast: the object that keeps to a module asks, while a mock stands over the module.
    enum class MockOfModule : bool { Answers, IsReadPast };
    JSValue readExport(ModuleID, unsigned index, MockOfModule = MockOfModule::Answers);
    JSValue readExportIfInitialized(ModuleID, unsigned index) const;
    bool isExportAmbiguous(ModuleID, unsigned index) const;
    bool tryReplaceExport(ModuleID, UniquedStringImpl* name, JSValue);
    void fillExportsOfBodyThatNeverRuns(ModuleID);
    // Importers are bound past a module that only hands names on, and a body without code never runs: its status says nothing. Plain Bun has loaded it with the first module that imports it.
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
    // IN A RUN IN WHICH ANY MODULE MAY BE MOCKED A STATEMENT NAMES ITS MODULE TO THE LOADER: the table has the name and who asks (`"./tax"`, External), not the file. What the loader has found under the name is what the
    // statement imports. Whoever walks the requests of the table to learn who imports whom goes by this. A name that nobody has resolved yet: the entry of the table itself. Of a CommonJS file its face, as a statement names it in the table.
    ModuleID moduleLoadedBy(const ModuleRequestData&) const;
    ModuleID moduleLoadedUnder(ModuleID named) const;
    void willMock(ModuleID);
    void didEndMock(ModuleID);
    void didBeginTestFile()
    {
        m_hasBegunTestFile = true;
        forgetEndedMocksOfEarlierTestFiles();
    }
    void markEvaluated(ModuleID module) { setStatus(module, ModuleStatus::Evaluated); }
    bool isMocked(ModuleID module) const { return m_mocks && m_mocks->modules.get(module.index()); }
    // The object of the host that stands in place of a module that was mocked ahead of its load. Of a CommonJS file or of its face.
    JSObject* mockInPlace(ModuleID module) const
    {
        if (!m_numberOfMocksInPlace) [[likely]]
            return nullptr;
        const LoadedModule* loaded = loadedIfExists(module);
        return loaded ? loaded->mockInPlace.get() : nullptr;
    }
    bool someMockStandsInPlace() const { return m_numberOfMocksInPlace; }
    bool setMockInPlace(ModuleID, JSObject* mock, JSValue whatRequireAnswers, bool leavesEarlierMockToItsImporters = false);
    bool setMockInPlaceOfLibraryModule(ModuleID, JSObject* mock, JSValue whatRequireAnswers);
    bool willLeaveModuleAlone(ModuleID, bool onlyIfLoadedInPreload, std::optional<ModuleID> askingAsHostNamesIt);
    bool isGraphRunning() const { return m_innermostEvaluation || m_rootWhoseFilesRun || m_rootWhoseFilesRanAhead || m_instance.vm->synchronousModuleQueue(); }
    bool hasLinkedImporterYetToRun(ModuleID, std::optional<ModuleID> askingAsHostNamesIt) const;
    BitVector modulesWithNothingToRunThatWerePassed() const;
    uint32_t momentOfFirstMockInTestFile(ModuleID);
    bool isLeftAloneByMock(ModuleID fileOrFace) const { return !m_mocksOverModulesThatHaveRun.isEmpty() && m_mocksOverModulesThatHaveRun.contains(m_table.fileBehindFace(fileOrFace).value_or(fileOrFace)); }
    bool setMockOverModuleThatHasRun(ModuleID, JSObject* mock, JSValue whatRequireAnswers);
    void endMockOverModuleThatHasRun(ModuleID);
    bool seesMockOf(ModuleID mocked, ModuleID importer) const;
    // What compiled code reads a name of `mocked` from: the mock that stood when the importer was linked, else the one that stands, else nothing.
    JSObject* mockSeenBy(ModuleID mocked, ModuleID importer) const;
    JSObject* endedMockSeenAt(ModuleID fileOrModule, uint32_t moment) const;
    uint32_t momentLinked(ModuleID importer) const;
    void forgetEndedMocksOfEarlierTestFiles();
    void linkAnewWhoeverReadsEndedMockBelow(ModuleID root);
    // `export * as inner from "./inner"` has no variable. What a mock that is laid over the module gives for the name is handed on in place of the object of "./inner", by every module that
    // hands it on. Whoever imports "./inner" itself keeps its object.
    JSValue handedOnInPlaceOfNamespace(const ModuleExportData&) const;
    bool tryReplaceNamespaceHandedOn(ModuleID handedOn, JSValue);
    void didReplaceNamespaceHandedOn(ModuleID handedOn);
    void letGoOfMockAtNextLoad(ModuleID);
    void letGoOfMockThatEnded(ModuleID);
    void letGoOfMockThatEndedSlow(ModuleID);
    bool letGoOfMockOfLibraryModuleAtNextLoad(ModuleID);
    void letGoOfMocksThatEnded();
    bool hasMockThatEnded(ModuleID module) const { return m_modulesWithMockToLetGo.get(module.index()); }
    bool loadComesToModuleWhoseMockEnded(ModuleID) const;
    bool hasMockOfLibraryModule(ModuleID module) const { return m_libraryModulesWithMock.get(module.index()); }
    bool wasLoadedInThisTestFile(ModuleID) const;
    // `bun test` takes whoever imports a module out of its loader when a mock of the module ends, and when a mock is laid over what an earlier test file loaded. A mock of such an importer
    // comes ahead of its load, though the body has run here.
    enum class ImportersLetGo : bool { All, AllButThoseThatThisTestFileLinkedToModule };
    void letGoOfImportersOf(ModuleID mocked, ImportersLetGo);
    void letGoOfModuleLoadedAsMock(ModuleID module) { m_modulesThatLoaderLetGo.set(module.index()); }
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
    // What an importer holds that does not see the mock over the module: the object from before the mock, with the names and the values of the module itself.
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
    // A mock that comes before the body of an ES module has run stands for the whole module: the loader of `bun run` takes what the factory answers for the module itself.
    // So a name that the factory leaves out is no export: no key of the namespace object. See fillExportsOfBodyThatNeverRuns() for whoever reads the variable.
    void leaveOutWhatMockLacks(ModuleID, JSObject* exportsOfMock);
    void setExportsOfMockAreOfCommonJSModule(ModuleID, bool);
    // And a name that the factory gives and the module lacks is an export: a key of the namespace object, where it is read. The names are settled once the module is loaded.
    void takeWhatMockAdds(ModuleID, bool comesAheadOfLoad);
    bool isAddedByMock(ModuleID, const Identifier&);
    void appendNamesAddedByMock(ModuleID, WTF::Vector<Identifier>&);
    // The table of a module lacks a name where a mock may answer ahead of the file that declares it: in a run in which any module may be mocked, whatever lies behind its second `export *` and those after it.
    // The module has those names all the same. A name may come more than once, `default` among them.
    void appendNamesBehindStarsOf(ModuleID, WTF::Vector<Identifier>&);
    bool hasNameBehindItsStars(ModuleID, UniquedStringImpl*);
    // Such a mock has no `export *`: what stands behind a star of the module gives it no name, and is not loaded for it.
    bool standsForWholeModule(ModuleID) const;
    // Stars lead round in rings and meet again behind a fork. A module that was asked to its end answers whoever comes next as it answered the first;
    // an answer that was made while the search came round in a ring holds for that way alone.
    struct SearchBehindStars {
        Vector<ModuleID, 8> path;
        BitVector isAnswered;
        BitVector isLeftOut;
        unsigned ringsMet { 0 };
    };
    // Every `export *` of the module is followed at run time, and each leads to a module whose table lists what it exports, as do the stars of that module. What mocks leave out and add decides the rest.
    bool namesBehindStarsAreInTables(ModuleID module) const
    {
        SearchBehindStars search;
        return data(module).hasDynamicStarExport() && namesBehindStarsAreInTables(module, search);
    }
    // The build keeps a name in every table that a link searches for it. Of any other name it drops the entries that nothing reads.
    enum class TablesBehindStars : bool { MayLackName, HaveName };
    // Whoever imports the name from the module finds it nowhere: such a mock leaves it out, the link of the module has dropped it, or every `export *` that hands it on leads to a module of which that holds.
    bool isLeftOut(ModuleID module, unsigned exportIndex, TablesBehindStars tables) const
    {
        if (m_table.exports(module)[exportIndex].isDroppedWhileEmpty && isVariableOfExportEmpty(module, exportIndex)) [[unlikely]]
            return true;
        if (m_exportsLeftOut.isEmpty()) [[likely]]
            return false;
        SearchBehindStars search;
        return isLeftOut(module, exportIndex, tables, search);
    }
    // What the graph of the root reaches without going through a mocked module. Nothing if no module is mocked.
    // Asked about the modules of the graph below a root.
    class MocksBelowRoot {
    public:
        MocksBelowRoot(const ModuleTable& table, Box<MockedModules> mocks, ModuleID root)
            : m_table(table)
            , m_mocks(WTF::move(mocks))
            , m_root(root)
        {
        }

        // Whether only mocked modules import it there. Such a module never runs.
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
    // Those that a require() of the program has put there, in that order, and behind them those that the loader of `bun run` would hold for a statement.
    Vector<ModuleID> libraryModulesInRequireCache();
    bool isLibraryModuleInRequireCache(ModuleID);
    std::optional<ModuleID> formThatMockFinds(StringView path, std::optional<ModuleID> file) const;
    void deferMock(ModuleID, AfterFailure);
    void forgetMockThatWaits(ModuleID);
    void endMockOfLibraryModule(ModuleID);
    // What ModuleNamespace::DynamicExports holds.
    void createDynamicExports(ModuleID, JSCellButterfly*& names, JSObject*& capturedValues, JSObject*& liveSources);
    void takeNamesBehindFollowedStarsAnew();
    // From here to didLayMock(), what tryReplaceExport() replaces is kept for endMockLaidOverFile().
    void keepWhatMockIsLaidOver(ModuleID);
    void didLayMock() { m_moduleWithMockBeingLaid = std::nullopt; }
    void endMockLaidOverFile(ModuleID);
    JSValue exportsBeforeMock(ModuleID);
    bool runMockThatWaits(ModuleID);
    bool runMockThatWaitsSlow(ModuleID);
    // `build.module("dep", ..)` of a plugin: from now on the loader answers for the name with another module. A file that is linked keeps what it has.
    void forgetWhatNamesResolvedTo();
    void willMockLibraryModule(ModuleID);
    bool letMockStandInPlaceOfLibraryModule(ModuleID);
    enum class RootOfLoad : bool { BeginsLoad, IsImportedByStatement };
    // The factories that one load calls run one at a time. True: the walk has called one that has not settled, and has stopped there. Whoever walks waits for it and walks again.
    bool runMocksBelow(ModuleID root, RootOfLoad = RootOfLoad::BeginsLoad);
    bool runMocksBelowSlow(ModuleID root, RootOfLoad);
    // The factory of a mock that waits runs when the first importer is linked, and that importer has begun to load by then: what was loaded when the mock was made decides.
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
    void takeFailedLoadsOutOfLoader(const ScopedLambda<bool(ModuleID)>& isMocked);
    JSValue errorOfLoadEndedByMock(ModuleID module) const { return m_modulesWithLoadEndedByMock.get(module.index()) ? loadedIfExists(module)->errorKeptByLoader.get() : JSValue(); }
    bool mockFailedBehindImportStatement(ModuleID);
    bool runMocksBelowModuleThatLoaderFetches(ModuleID);
    bool runMocksBelowModuleThatLoaderFetchesSlow(ModuleID);
    bool isFetchedForImportStatement(ModuleID) const;
    JSPromise* factoryThatWaitsBelow(ModuleID root, FactoryAskedForBy, JSValue& state);
    void layMocksAgainAtNextLoad(ModuleID module, bool waits) { m_modulesThatLayMocksAgain.set(module.index(), waits); }
    void layMocksAgainBelow(ModuleID root);
    bool mocksWaitToBeLaidAgain() const;
    enum class AtMock : bool { GoesOn, Stops };
    BitVector reachedByStatements(ModuleID root, AtMock) const;
    bool importsByStatements(ModuleID importer, ModuleID imported) const;
    // The life of the mock of a module, computed from what the sets and flags below say. NoneOfThem: together they say what no state is.
    enum class MockState : uint8_t { None, Waits, Runs, InPlace, InPlaceWhileAnotherWaits, InPlaceOverModuleThatHasRun, Fills, LaidOver, Failed, Ended, EndedWhileAnotherWaits, NoneOfThem };
    MockState mockState(ModuleID);
    // For the counts alone: the state, the status of the body, and, for NoneOfThem, what is set.
    String describeMockState(ModuleID);
    void countMockStates(ASCIILiteral door, const String& before, ModuleID);
    void noteCallOfMock(ModuleID fileOrModule);
    void forgetCallOfMock(ModuleID fileOrModule);
    bool isBeingEvaluated(ModuleID module) const { return status(module) == ModuleStatus::Evaluating || status(module) == ModuleStatus::EvaluatingAsync; }
    static const MockOperations& mockOperations();
    JSObject* capturedExports(ModuleID);
    // Empty if the module has no export of the name.
    JSValue capturedValue(ModuleID, const Identifier&);
    bool hasCapturedExport(ModuleID, const Identifier&);
    // Hands over no value for a deferred export.
    void forEachCapturedExport(ModuleID, const ScopedLambda<void(const Identifier&, JSValue)>&);
    // What a table of exports holds under the name. In the captured exports of a module the table itself stands for a deferred export: capturedValue() reads that one.
    static JSValue storedValue(JSGlobalObject*, JSObject* table, const Identifier& name);
    // As AbstractModuleRecord::getModuleNamespace(): the caller that creates the object decides whether it is extensible.
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
    // In plain Bun a test run that isolates its files links a TypeScript module that exports a name which the other module lacks, for the name may be a type: CyclicModuleRecord::initializeEnvironment(), m_isTypeScript.
    // True: the link of the importer goes on, and the name that the statement exports is no export of it.
    bool dropExportThatFindsNothing(ModuleID importer, const RequiredNameData&);
    void throwExportAmbiguous(const RequiredNameData&);
    String pathOfExporterAsLoaderNamesIt(const RequiredNameData&);
    void didRequestFromHost(ModuleID module)
    {
        if (!m_firstModuleRequestedByHost) [[unlikely]]
            noteFirstRequestFromHost(module);
    }
    std::optional<ModuleID> firstModuleRequestedByHost() const { return m_firstModuleRequestedByHost; }
    bool isHeldForImport(ModuleID) const;
    // What require() answers for a file of data goes by whether something has imported the file: isHeldForImport().
    void didImportFromUncompiledCode(ModuleID module, bool isRootOfImportCall)
    {
        if (data(module).kind != ModuleKind::Data)
            return;
        if (isRootOfImportCall && !recordIfExists(module))
            return;
        m_dataFilesImportedByUncompiledCode.set(module.index());
    }
    // The first file that the loader comes to is the main one, also where the module that imports it is not compiled.
    void didImportFileFromUncompiledCode(ModuleID fileOrFace);
    // The host starts a realm with one module, the entry point.
    bool isEntryPointOfRealm(ModuleID module) const { return m_firstModuleRequestedByHost == module; }
    // `bun run` loads the entry point as the one import of the module bun:main: the graph is loaded one step of the loader behind the entry point. Says so once.
    bool takeStepOfMainModule(ModuleID module) { return isEntryPointOfRealm(module) && !std::exchange(m_didTakeStepOfMainModule, true); }
    // An import() that loads the entry point ahead of the program loads it as any other module, and bun:main finds it loaded.
    void willBeLoadedByImport(ModuleID module) { m_didTakeStepOfMainModule |= isEntryPointOfRealm(module); }
    // Ahead of that step bun:main waits for one promise and whoever else asks for the entry point for another. Says so once.
    bool takeLoadPromiseBelowMainModule(ModuleID module) { return isEntryPointOfRealm(module) && !m_didTakeStepOfMainModule && !std::exchange(m_didTakeLoadPromiseBelowMainModule, true); }
    // The body of bun:main runs where the body of any importer does: behind the last statement of the entry point, or in the job that ends its evaluation.
    JSPromise* evaluateMainModule(ModuleID entryPoint, JSPromise* evaluation);
    bool mainModuleWaitsForItsImport() const { return !!m_capabilityOfMainModule; }
    void runMainModuleThatWaited();
    void failMainModuleThatWaited();
    bool refusesCodeOutsideImage() const { return !m_admitsCodeOutsideImage || m_instance.vm->aotState().askerLoadsCompiledFilesAlone; }
    bool loadsCompiledFilesAlone(ModuleID) const;
    const String* exchangeAskerOfModule(const String* asker) { return std::exchange(m_askerOfModule, asker); }
    // The host names no asker where it starts an entry point, and a module of the image asks through the loader: isAskingHost().
    bool isAskedByCodeOutsideImage() const { return m_askerOfModule && !m_askerOfModule->isEmpty() && !m_table.findByPath(*m_askerOfModule) && !isAskingHost(); }
    void forEachFileAwaitingAtTopLevel(const WTF::Function<void(int64_t turn, const String& key)>&);
    void evict(ModuleID, bool isUndoneForKeptFiles = false);
    void forEachFileThatFailed(const WTF::Function<void(ModuleID)>&) const;
    void willRunCodeThatKeepsFiles();
    void keepFilesThatHaveRun();
    void bringBackKeptFiles(const WTF::Function<void(const String&)>&);
    void forgetEvaluation(ModuleID);
    void forgetRunThatThrew(ModuleID);
    // The object of a tagged template is one for its place in the text as long as the text is evaluated once. `offsetOfCell`: the word of the statics that compiled code keeps it in.
    void didMakeTemplateObject(ModuleID, uint32_t offsetOfCell);
    void forgetRunOfLibraryModule(ModuleID);
    JSValue executeForLoader(ModuleID);
    // Whether evict() would do anything: the file runs or has run.
    bool wouldBeEvicted(ModuleID);
    // Whether the loader of plain Bun would hold a module for the file, which only a deletion takes away.
    bool wasLoadedByImport(ModuleID);
    // Code is compiled for files that load once, but for those that the build saw taken out of require.cache. What holds no code is read again at any time.
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
    // For questions between which no code runs and no status is stored.
    bool hasRun(ModuleID, bool& importsAreFollowed);
    bool isListedBeforeItHasRun(ModuleID);
    // The map of `require` holds it, in front of whatever the loader of ES modules alone holds.
    bool hasModuleObjectInRequireCache(ModuleID) const;
    struct FirstRequire {
        ModuleID requirer;
        ModuleID required;
    };
    // The calls that made a module object, in their order, where the image shows the ways between such objects.
    std::span<const FirstRequire> firstRequires() const LIFETIME_BOUND { return m_firstRequires.span(); }
    // The module object that the loader made for a file that has yet to begin is gone: whoever asks for the file next makes another.
    void forgetRequestByLoader(ModuleID file);
    void keepObjectMadeByRequire(ModuleID, JSObject* moduleObject);
    void forgetObjectMadeByRequire(ModuleID);
    bool holdsCodeThatMayBeEvicted();
    BitVector modulesThatHaveRun();
    bool isLeftToHost(ModuleID);
    bool isChangedOutOfSightOfBuild(StringView path) const;
    // The text of a data: URL that the program imports under a constant name is part of the program.
    bool isTextNamedByProgram(StringView url) const;
    bool refusesLoadsByName() const { return !m_loadsModulesByName && !m_runsUncompiledCode && !m_hostRunsCopiesFromDisk && !isAskingHost(); }
    // A package that the build left out runs its body inside the call that asks the host for it. What that body asks for, code from disk asks for.
    bool isAskingHost() const { return m_isAskingHost && !isInterpretedCodeAsking(); }
    bool isInterpretedCodeAsking() const;
    // A name that the build did not follow finds an entry point and no other file of the image with code.
    bool refusesRequestFromHost(ModuleID module) const { return !m_admitsInspector && !data(module).isEntryPoint() && (data(module).kind != ModuleKind::Data || !m_findsDataFilesByName) && m_table.hasFilePath(module) && !data(module).isLibraryCodeWithFilePath() && !m_runsCopyFromDisk && !isAskingHost(); }
    bool isRootThatCompiledCodeLoads(ModuleID module) const { return m_rootThatCompiledCodeLoads && fileOf(*m_rootThatCompiledCodeLoads) == fileOf(module); }
    bool refusesOriginalsBesideMocks() const { return m_refusesOriginalsBesideMocks; }
    // A mock stands for the module, and the host's own loader would give this importer the module itself beside it. False: the importer gets the mock.
    bool importsOriginalBesideMock(ModuleID mocked, ModuleID importer) const;
    // The body has run, beside the mock or before it came. It does not run again for whoever gets the module itself.
    bool hasRunItself(ModuleID mocked) const;
    // `bun test` loads the original first and lays such a mock over it.
    bool isMockedByFactoryThatTakesOriginal(ModuleID) const;
    bool getsOriginalBesideMock(ModuleID, ModuleID referrer);
    JSValue answerRequireWithOriginal(ModuleID file, ModuleID referrer);
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
    // `onStacks`: modulesThatHaveRunOnStacksOfEvaluations(), found where the first module asks for it.
    bool waitsForItsCycle(ModuleID, std::optional<BitVector>& onStacks) const;
    BitVector modulesThatHaveRunOnStacksOfEvaluations() const;
    // The modules that compiled code begins to evaluate below the body fail with it.
    JSValue callBodyOfModuleEnteredByLoader(ModuleID module) { return callBodyUnderOwnMark(module, false); }
    void evaluateModuleInRecord(ModuleID);
    // Says whether the start that asks goes on to run the module.
    bool beginModuleInRecord(ModuleID);
    // Says whether a file ran.
    bool evaluateImportedCommonJSFiles(ModuleID root, JobsOfImportedFiles = JobsOfImportedFiles::StayQueued, ErrorOfImportedFile = ErrorOfImportedFile::IsThrown, ModuleGraphLoadingState* = nullptr);
    AbstractModuleRecord* recordOfRequestedModule(ModuleID);
    void queueJobsThatWaitForLoader();
    void handGraphsToLoader();
    void didLoadGraph(ModuleID root);
    void didFailToLoadGraph(ModuleID root);
    void resumeImportsBehindLoad(ModuleID root, bool graphRunsInThisJob = false);
    void resumeImportsBehindSettledLoads();
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
    // The files that the module imports are being run: by its start, which is on the stack for that alone, or one after the other by evaluateImportedCommonJSFiles().
    bool isRunningFilesOf(ModuleID root) const
    {
        for (const FilesBeingRun* files = m_innermostFilesBeingRun; files; files = files->outer) {
            if (files->root == root)
                return true;
        }
        return m_rootWhoseFilesRun == root;
    }
    bool stopLoadingBehindFile(ModuleID file);
    // The loader has come to the module: a CommonJS file runs, and the host lists what a native module exports.
    void evaluateLoadedCommonJSFile(ModuleID);
    void generateNativeModuleForLoader(ModuleID);
    // One of the two, and what else stands on the level of a module on the way of a load: PlanOfLoad.
    void evaluateFilesOfLevel(ModuleID moduleOnWay, bool runsWhenLoaded);
    bool isCommonJSFileOrFace(ModuleID module) const { return data(module).kind == ModuleKind::CommonJs || m_table.fileBehindFace(module); }
    // The loader can load a file of the program as it loads code that is not compiled.
    bool isFileOfProgram(ModuleID module) const { return !data(module).isOfLibrary() && (data(module).kind == ModuleKind::SourceText || data(module).kind == ModuleKind::CommonJs); }
    bool isLoadedWithRequests(ModuleID module) const { return isFileOfProgram(module) && data(module).kind == ModuleKind::SourceText; }
    // `bun run` takes the loader's whole round for a data file too.
    bool isImportedByLoader(ModuleID module) const { return isFileOfProgram(m_table.fileBehindFace(module).value_or(module)) || (data(module).kind == ModuleKind::Data && !data(module).isOfLibrary()); }
    bool isBuiltinModuleOfLoader(ModuleID) const;
    bool isBuiltinModuleNewToLoader(ModuleID) const;
    bool needsLoaderForRequire(ModuleID);
    bool canTellHowGraphIsLoaded(ModuleID root);
    void appendRequestsForLoader(ModuleID, ModuleRecord*, ModuleGraphLoadingState&);
    ModuleGraphLoadingState* loadThatWaitsFor(ModuleID requester, const Identifier& key, ModuleGraphLoadingState* loadOfRequester, unsigned& jobsUntilLoadHearsOfFailure);
    std::optional<ModuleFailureID> statementThatEndsLoad(ModuleID) const;
    void appendRequestThatFindsNothing(ModuleFailureID statement, ModuleRecord*);
    void keepFailureOfRequest(ModuleID, ModuleRecord*, const Identifier& name);
    ModuleRecord* appendRequestsOfUncompiledCode(ModuleID, ModuleRecord*);
    struct RequestsOfUncompiledCode;
    void collectRequestsOfUncompiledCode(ModuleID, RequestsOfUncompiledCode&);
    // Null: the statements that name the module say nothing of its type.
    RefPtr<ScriptFetchParameters> fetchParametersOf(ModuleID external) const;
    ModuleRegistryEntry* registryEntry(const Identifier& key, const ScriptFetchParameters*);
    bool comesToCodeThatDoesNotLoad(ModuleID root);
    bool setIsWalkingRequests(bool isWalkingRequests) { return std::exchange(m_isWalkingRequests, isWalkingRequests); }
    // A graph is being loaded until its root is linked.
    unsigned numberOfGraphsBeingLoaded();
    bool comesToLoadInFlight(ModuleID required);
    bool hasNotBegunAsFile(ModuleID) const;
    void markModulesBelowWalkedRoot(ModuleID root);
    // The loader tells when it settles the promise of an import().
    enum class SettlingOfLoad : bool { IsToldByLoader, IsLookedFor };
    void didStartLoad(JSPromise*, ModuleID root, SettlingOfLoad = SettlingOfLoad::IsToldByLoader);
    // IsRequested: the loader has listed the file among what a module imports, and comes to it behind what stands in front of it there.
    enum class LoadOfFile : uint8_t { IsAskedFor, IsRequested, HasComeToFile, IsRootOfImport };
    ModuleRecord* recordOfCommonJSFile(ModuleID, LoadOfFile = LoadOfFile::HasComeToFile);
    void noteLoadOfFile(ModuleID fileOrFace, LoadOfFile = LoadOfFile::HasComeToFile);
    bool asyncLoadMustWaitFor(ModuleID fileOrFace) const;
    bool importsFileBeingEvaluated(ModuleID root) const;
    bool importsFileThatRunsAhead(ModuleID root) const;
    void evaluateRootThatWaitedForFile();
    std::optional<ModuleID> fileToWaitFor(const Identifier& key);
    JSPromise* deferRequest(ModuleID file, ModuleLoadingContext*, bool useImportMap);
    bool hasDeferredLoads() const { return !m_deferredRequests.isEmpty() || !m_deferredImports.isEmpty(); }
    void didRequestModule(const Identifier& key, JSCell* payload);
    void didSettleLoad(const ModuleGraphLoadingState&);
    void didFailToMakeModule(const Identifier& key);
    bool isNotLoaded(ModuleID);
    // Of an ES module of the program that has neither a record nor a status.
    bool isNotLoadedWithoutStatus(ModuleID, const ModuleData&);
    // An ES module of the program whose status stands in the table and says that it is linked, at the least.
    bool isLoadedModuleWithoutRecord(ModuleID) const;
    bool wasLoadedWithEarlierGraph(ModuleID);
    bool wasLoadedWithGraph(ModuleID) const;
    bool setIsAskingHost(bool isAskingHost) { return std::exchange(m_isAskingHost, isAskingHost); }
    // The loader walks the import statements of a module of the image, and asks the host for what one of them names.
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

    // A whole require(): who required whom is recorded here, whoever asks. Without a referrer it is the evaluation and its answer alone: for the host's own loads, and for a site with operations around it that record.
    // Program: the loader of the host asks for code of the program that this image does not hold.
    enum class RequiredBy : uint8_t { Referrer, Program };
    JSValue require(ModuleID, ModuleID referrer = { }, RequiredBy = RequiredBy::Referrer);
    void evaluateCommonJS(ModuleID);
    JSValue commonJSExports(ModuleID) const;
    // Of a CommonJS file with a module object to hand out: its body has stored no exports.
    bool hasYetToBegin(ModuleID file) const { return data(file).hasValue() && status(file) <= ModuleStatus::Linked && commonJSExports(file).isUndefined(); }
    JSValue exportsShownByModuleObject(ModuleID);
    JSValue exportsSeenByLoader(ModuleID);
    void setCommonJSExports(ModuleID module, JSValue exports) { setValue(module, exports); }
    void mockWhatRequireAnswered(ModuleID, JSObject* exports);
    void mockWhatRequireCacheHolds(ModuleID, JSObject* exports);
    void letGoOfMockInRequireCache(ModuleID);
    void recordRequire(ModuleID, ModuleID referrer);
    void recordRequireFromOutsideImage(ModuleID, JSObject* moduleObjectOfRequirer);
    bool wasLoadedSynchronously(ModuleID file);
    // `Module._resolveFilename` or `Module.prototype.require` of the realm is the program's.
    bool areLoaderFunctionsStored() const { return !m_storedLoaderFunctions.isEmpty(); }
    void noteLoaderFunction(RequireOverride function, bool isStored) { m_storedLoaderFunctions.set(function, isStored); }
    bool wasFirstLoadedSynchronously(ModuleID file);
    bool hostLoadsSynchronously(ModuleID file) const;
    void forgetFirstLoad(ModuleID);
    void willLoadCopyFromDisk(ModuleID, LoadByName);
    JSObject* moduleObject(ModuleID);
    JSObject* mainModuleObject(ModuleID);
    JSObject* parentModuleObject(ModuleID);
    bool forEachNewChild(ModuleID, const ScopedLambda<void(JSObject*)>&);
    void dropNewChildren(ModuleID);
    JSValue requireSpecifier(JSValue specifier, ModuleID referrer, bool isNamedByBuild);
    JSValue resolveDynamic(JSValue specifier, ModuleID referrer, ResolvedSpecifierForm);

    JSPromise* import(ModuleID, ModuleID referrer, JSValue options = jsUndefined());
    // The host hands what it gets to import() in code that is not compiled, which gives the program a promise of its own.
    enum class ImportCaller : bool { CompiledCode, Host };
    enum class TextOfImport : bool { MayComeLater, HasCome };
    JSPromise* startImport(ModuleID, std::optional<int64_t> referrerAsyncOrder, ImportCaller = ImportCaller::CompiledCode, TextOfImport = TextOfImport::MayComeLater, JSPromise* promiseOfCaller = nullptr, JSValue stateOfWait = JSValue(), std::optional<bool> answerOfWalkAlreadyMade = std::nullopt);
    JSPromise* rejectImportWithErrorOfLoad(ModuleID, ImportCaller);
    void startImportWhoseTextHasCome(JSPromise* importPromise, ModuleID, std::optional<int64_t> referrerAsyncOrder);
    // `bun run` reads a text on another thread and hears of it a turn of the event loop later. From the call of a file that runs while a graph is loaded until the last text has come,
    // each text comes in a turn of its own, in the order in which they were asked for: also what the loader asks for meanwhile. True: the fetch is fulfilled when the text comes.
    bool holdTextOfRequest(JSPromise* fetched, ModuleRecord* requested, ModuleID requester);
    void deliverFirstTextOnItsWay();
    // `bun test` reads the text of a file that a call of import() names on another thread, and hears of it in a later turn of the event loop: every job that waits runs first, and so do the `describe` callbacks
    // of a test file that has just been loaded. A compiled test run has every text, so a call outside a load waits for a turn and for nothing else: the calls of one turn are made again together, in their order,
    // and settle the promises that their callers have. An executable has every text under plain Bun too: nothing is held for it here.
    enum class AskedBy : bool { Loader, Import };
    bool textOfImportComesInLaterTurn(ModuleID, AskedBy);
    bool hasTextAtHand() const;
    void didRunJobsBehindDelivery() { m_isInJobsBehindDelivery = false; }
    // No text of the program is read.
    bool loadsAsExecutableDoes() const;
    bool isTestRun() const;
    bool isAnythingBeingLoaded() const;
    // `requested`: the root of a graph that a call has asked the loader for by its key, and the promise is that of the fetch. Null: the call is made again, and the promise is the one that its caller has.
    void holdTextOfCall(JSPromise*, ModuleID, ModuleRecord* requested, std::optional<int64_t> referrerAsyncOrder, ImportCaller);
    void willDeliverText() { ++m_turnOfDeliveries; }
    bool hasTextThatWaits() const { return !m_textsThatWait.isEmpty() || !m_textsOnTheirWay.isEmpty(); }
    // Nothing: no call waits for a turn alone.
    std::optional<uint32_t> turnOfFirstTextThatWaits() const { return m_textsThatWait.isEmpty() ? std::nullopt : std::optional(m_textsThatWait[m_numberOfTextsThatHaveCome].turnOfCall); }
    // False: nobody is left to hear of it.
    bool deliverFirstTextThatWaits();
    bool deliverFirstTextInLine();
    void tellHostOfImport(JSPromise*, const String& path);
    JSPromise* importDynamic(JSValue specifier, JSValue options, ModuleID referrer);
    // `stateOfWait`: as for startImport().
    JSPromise* importUncompiled(const Identifier& key, ModuleID referrer, RefPtr<ScriptFetchParameters>&&, JSValue stateOfWait = { });
    void continueImport(JSPromise* importPromise, ModuleID, std::optional<int64_t> referrerAsyncOrder);
    void resolveWithNamespace(JSPromise* importPromise, ModuleID);
    void finishImportOfEvaluatedModule(JSPromise* importPromise, ModuleID, JSValue error);

    enum class RecordHolder : uint8_t { Anyone, Loader };
    ModuleRecord* exposeRecord(ModuleID, RecordHolder = RecordHolder::Anyone);
    void retireRecordsOfSettledLoads();
    AbstractModuleRecord* uncompiledRecord(ModuleID);
    // The record that stands for the module in ResolveExport() and GetExportedNames().
    AbstractModuleRecord* recordForResolution(ModuleID);
    enum class NamespaceUse : uint8_t { IsShownToProgram, CarriesBindings };
    JSObject* uncompiledNamespace(ModuleID, ModulePhase, NamespaceUse, bool shouldPreventExtensions = true);

private:
    friend class RequestToHostScope;

    // The roots are visited at every stop of the mutator, and the loaded modules only after a store.
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
        // From keepWhatMockIsLaidOver() to endMockLaidOverFile(). A file may be taken for loaded while its body has yet to run.
        bool hasMockLaidOver { false };
        bool hadRunWhenMockWasLaid { false };
        Vector<ModuleID> namespacesReplacedByMock;
        // Of the module whose object is handed on: the mocked modules whose mocks give a value in its place, the latest last.
        Vector<ModuleID> mocksThatReplaceNamespace;
        bool capturedFieldsAreOfESModule { false };
        bool capturedFieldsAreOfRuntimeModule { false };
        Identifier resolvedKey;
        // What the name resolved to when the loader was last asked. A plugin that comes or goes makes the next load ask again, and what is linked stays linked.
        Identifier keyOfLatestLoad;
        bool wasLoadedWhileItRan { false };
        bool wasLoadedSynchronouslyWhileItRan { false };
        bool isAnsweredWithNamespace { false };
        bool bindsEarlierCapture { false };
        bool didDropError { false };
        bool exportsWhatMockNamesAlone { false };
        bool takesNamesThatMockAdds { false };
        bool isYetToBeRequiredBehindMock { false };
        bool exportsOfMockAreOfCommonJSModule { false };
        bool wasLoadedBehindMock { false };
        bool wasRequiredBehindMock { false };
        // Of a CommonJS file: didLoseErrorOfModule().
        bool wasReachedBySecondLoad { false };
        // Of a CommonJS file that a require() runs: the first import() of it was called meanwhile.
        bool wasFirstImportedByCallWhileItRan { false };
        bool isBeingLoadedByLoader { false };
        BitVector ambiguousExports;
        std::unique_ptr<SourceOrigin> sourceOrigin;
        Vector<ModuleID> requiredModules;
        unsigned numberOfChildrenShown { 0 };
        std::optional<ModuleID> firstRequirer;
        // Of a CommonJS file whose body the run time called: m_synchronousLoadCount at that call.
        std::optional<uint32_t> synchronousLoadCountAtStart;
    };

    struct DynamicExport {
        Identifier name;
        JSValue capturedValue;
        JSObject* liveSource { nullptr };
    };

    StoredModuleStatus storedStatus(ModuleID) const;
    void setStoredStatus(ModuleID, StoredModuleStatus);
    ModuleStatus statusInRecord(ModuleID) const;
    void setStatus(ModuleID, ModuleStatus);
    void setFailed(ModuleID, JSValue error);
    ModuleRecord* ensureRecordWithoutStatus(ModuleID);
    void retireRecord(ModuleRecord*);
    void retireRecordOfModuleThatHasNotBegun(ModuleRecord*);
    bool hasRunToItsEndWithItsCycle(const ModuleRecord&) const;
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
    bool isNativeModule(ModuleID) const;
    JSObject* generateNativeModuleExports(ModuleID, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values);
    JSValue requireNativeModule(ModuleID);
    void bindLatestCapture(ModuleID);
    void captureExportsBehindEvaluation(ModuleID);
    void captureExportsOfLibraryModules(ModuleID root);
    void captureExportsOfFilesBeingEvaluated(ModuleID root);
    JSValue evaluateForCapture(ModuleID);
    bool tryCaptureFields(ModuleID, JSValue exports);
    JSValue capturedField(const LoadedModule&, UniquedStringImpl* name) const;
    void makeTableOfCapturedFields(LoadedModule&);
    void fillCapturedExports(ModuleID);
    void fillDeferredExports(ModuleID importer);
    AbstractModuleRecord::Resolution::Type resolveRequiredName(ModuleID exporter, const Identifier&);
    // Throws for the first name that an unlinked module of the list requires and that its exporter lacks or has from two bindings.
    void checkRequiredNames(std::span<const ModuleID> modules);
    bool isLeftOut(ModuleID, unsigned exportIndex, TablesBehindStars, SearchBehindStars&) const;
    bool namesBehindStarsAreInTables(ModuleID, const SearchBehindStars&) const;
    bool isVariableOfExportEmpty(ModuleID, unsigned exportIndex) const;
    // The text of a CommonJS file always stores the name. The file lacks it only where a mock stands for the whole file.
    bool isFoundInTextOfFile(const RequiredNameData& requiredName) const { return requiredName.mayBeAbsent && isCommonJSFileOrFace(requiredName.exporterModule()); }
    bool isDeferredExport(const LoadedModule&, const Identifier&) const;
    bool findsItsNamesInLibraryAlone(ModuleID) const;
    Vector<DynamicExport> collectDynamicExports(ModuleID);

    void evaluateWithOwnStack(ModuleID root);
    // Modules that somebody asked to have evaluated while they ran.
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
    bool mayHandOutModuleObject(ModuleID module) const { return data(module).kind != ModuleKind::CommonJs || data(module).hasValue(); }
    JSValue answerRequire(ModuleID, ModuleID referrer = { }, RequiredBy = RequiredBy::Referrer);
    JSValue settledAnswerOfRequire(ModuleID, RequiredBy) const;
    void addToRequireCache(ModuleID, JSValue exports);
    void setRequireCacheValue(ModuleID, JSValue exports);
    JSValue requireThroughCache(ModuleID, ModuleID referrer);
    JSValue answerRequireWithMock(ModuleID, ModuleID referrer);
    JSValue answerOfRequireToMock(ModuleID);
    bool isMockedLibraryModule(ModuleID);
    JSValue loadEvictedFile(ModuleID);
    JSValue readExportForRequire(JSObject* namespaceObject, const Identifier& name);
    void willRequireESModule(ModuleID);
    void markNamespaceAsRequired(ModuleID);

    std::optional<ModuleID> findByName(const Identifier& key);
    // The loader has one place for a file without a type and another for it with a type, so they are two modules. The image may hold the first alone.
    enum class FormWithoutAttributes : bool { MayAnswer, IsAnotherModule };
    std::optional<ModuleID> findWithAttributes(const Identifier& key, const ScriptFetchParameters*, FormWithoutAttributes = FormWithoutAttributes::MayAnswer);
    Identifier keyWithoutForm(ModuleID file);
    std::optional<ModuleID> findFormThatStatementLoaded(const Identifier& key, const ScriptFetchParameters&, std::optional<ModuleID> asked) const;
    bool isBelowRootWhoseFilesRun(ModuleID) const;
    void noteFormAnsweredFirst(const Identifier& key, const ScriptFetchParameters&, ModuleID file);
    bool loaderOnlyHandsOverNamespace(ModuleID) const;
    JSPromise* askLoaderForImport(ModuleID, std::optional<int64_t> referrerAsyncOrder, JSPromise* resultPromise = nullptr);
    std::optional<int64_t> asyncEvaluationOrder(ModuleID) const;
    ModuleRecord* registerWithLoader(ModuleID, RecordHolder = RecordHolder::Anyone);
    const Identifier& resolvedKey(ModuleID);

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
    // Whoever asks for one asks for it several times in a row. None is ever removed.
    mutable LoadedModule* m_loadedFoundLast { nullptr };
    mutable ModuleID m_moduleOfLoadedFoundLast;
    RememberedSlotSet m_storedReferences;
    std::unique_ptr<ModuleLinkWalk> takeLinkWalk(ModuleID root);
    // None: no walk of the root is kept, or it no longer holds.
    std::unique_ptr<ModuleLinkWalk> takeKeptLinkWalk(ModuleID root);
    void keepLinkWalk(std::unique_ptr<ModuleLinkWalk>&&);
    Vector<std::unique_ptr<ModuleLinkWalk>, 2> m_linkWalksOfLoads;
    bool checkIsSettledForLinks(ModuleID) const;
    unsigned m_linkEpoch { 0 };
    unsigned m_numberOfMocksInPlace { 0 };
    // The files and the modules of the library that such a mock stands in place of. A module of the library is none of the mocked files.
    BitVector m_modulesWithMockInPlace;
    mutable Vector<uint64_t> m_modulesSettledForLinks;
    const EvaluationInProgress* m_innermostEvaluation { nullptr };
    // Each load of evaluateSynchronously() has a number. The innermost one that has not returned is the active one.
    uint32_t m_synchronousLoadCount { 0 };
    uint32_t m_activeSynchronousLoad { 0 };
    Structure* m_namespaceStructure { nullptr };
    Structure* m_extensibleNamespaceStructure { nullptr };
    Identifier m_moduleExportsName;
    // The pairs of a requiring and a required module that recordRequire() has seen.
    enum class RecordedRequire : uint8_t { Begun, Returned, BegunAgain, BegunAndMadeObjectOfESModule, ReturnedAndNotListed };
    UncheckedKeyHashMap<uint64_t, RecordedRequire> m_recordedRequires;
    std::optional<ModuleID> m_mainCommonJSModule;
    bool m_isMainCommonJSModuleDecided { false };
    bool m_didSearchBelowEntryPoint { false };
    // While no file has the id "." and no require() of a file has returned.
    Vector<ModuleID, 4> m_modulesFirstRequiredByCallsInProgress;
    Vector<FirstRequire> m_firstRequires;
    // The first CommonJS file below each root that import() was called for in that time.
    Vector<ModuleID, 2> m_firstFilesOfLoads;
    // `bun run` makes the module object of a file for whoever asks for the file first, and only a require() leaves a parent in it.
    BitVector m_filesRequestedByLoader;
    struct FilesBeingRun {
        ModuleID root;
        // Without one, the start of the root calls them.
        std::optional<ModuleID> fileOrFace;
        FilesBeingRun* outer;
        // The loader of `bun run` has asked for every file of the root by now, in jobs that stand in front of whatever the files queue.
        bool isAheadOfJobsOfLoader { false };
        // In which the loader asks the root for its requests.
        ModuleGraphLoadingState* load { nullptr };
        size_t numberOfJobsInFrontOfFile { 0 };
        // A require() loads the root, and the loader of `bun run` has taken the steps as far as the one in which the file runs.
        bool isAheadOfStepsOfLoader { false };
        // The loader has gone through the graph as far as the step in which this file runs.
        std::optional<ModuleID> fileHandedToLoader { };
        // What the loader of `bun run` has asked for when the file runs, and the ES modules among it whose requests it has yet to make.
        BitVector requested { };
        Vector<ModuleID> importersOfNextLevel { };
    };
    std::optional<ModuleID> fileOrFaceEvaluatedIn(const FilesBeingRun&) const;
    bool hasLoaderAskedFor(FilesBeingRun&, ModuleID file);
    void handGraphToLoader(FilesBeingRun&, std::optional<ModuleID> finishedFile = std::nullopt);
    void handSynchronousLoadToLoader(FilesBeingRun&);
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
    void forgetTemplateObjects(ModuleID file);
    struct TemplateObjectOfFile {
        ModuleID file;
        uint32_t offsetOfCell;
    };
    // Of the files whose body may run a second time.
    Vector<TemplateObjectOfFile> m_templateObjectsOfFiles;
    void noteRequireOfFile(ModuleID file);
    BitVector m_modulesWithMarkOfRequire;
    BitVector m_modulesWithExtensibleNamespace;
    BitVector m_modulesWithEarlierNamespaceInValue;
    // By module: ModuleWords::evaluatingMark of the evaluation under another one that began it, or that begins it if it comes there. Zero: an evaluation under no other. Empty until one runs under another.
    Vector<uint8_t> m_marksOfEvaluationsUnderOthers;
    Box<MockedModules> m_mocks;
    std::optional<ModuleID> m_moduleWithMockBeingLaid;
    OptionSet<RequireOverride> m_storedLoaderFunctions;
    std::optional<UncheckedKeyHashMap<uint32_t, SyntheticExportPlace, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> m_syntheticExportsByVariable;
    UncheckedKeyHashMap<ModuleID, BitVector> m_exportsLeftOut;
    BitVector m_evictedFiles;
    // isChangedOnDisk(), of files that the build has not marked ModuleData::mayBeEvicted().
    BitVector m_filesChangedOutOfSightOfBuild;
    // A walk has found nothing evicted in what they import, and nothing has been evicted since.
    BitVector m_filesAboveNoEvictedFile;
    BitVector m_filesAboveEvictedFile;
    // See keepFilesThatHaveRun().
    BitVector m_keptFiles;
    BitVector m_modulesThatRanBeforeCodeThatKeepsFiles;
    std::optional<bool> m_holdsCodeThatMayBeEvicted;
    BitVector m_filesToBringBack;
    // They have not run, and are among the evicted files only for what they import: isLeftToHost().
    BitVector m_filesEvictedWithImports;
    bool m_runsCopyFromDisk { false };
    // The host itself names such a copy to its loader: the original beside a mock, in a test run.
    bool m_hostRunsCopiesFromDisk { false };
    BitVector m_filesToLoadAgain;
    // Of modules that are not compiled: the object was asked for, and so far it has only carried bindings: the program has not seen it.
    BitVector m_uncompiledNamespacesAskedFor;
    BitVector m_uncompiledNamespacesNotShownYet;
    // See hasRun(): the answer, and the states that it was made from.
    BitVector m_modulesThatRanWithImporters;
    BitVector m_modulesSeenToHaveRun;
    Vector<uint8_t> m_statusesWhenImportsWereFollowed;
    std::optional<BitVector> m_modulesOnStacksWhenImportsWereFollowed;
    // An import() waits for their text, or has queued its first job for them, which has not run.
    BitVector m_modulesOfQueuedImports;
    BitVector m_modulesWithMockToRun;
    Vector<ModuleID> m_libraryModulesInRequireCache;
    BitVector m_libraryModulesTakenFromLoader;
    BitVector m_libraryModulesThatStatementsReached;
    mutable BitVector m_libraryModulesWithCheckedName;
    mutable BitVector m_libraryModulesKeptInRequireCache;
    BitVector m_libraryModulesWithMock;
    BitVector m_libraryModulesThatMockReplaces;
    BitVector m_libraryModulesLeftByThrow;
    std::optional<ModuleID> m_firstModuleRequestedByHost;
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
    const bool m_findsDataFilesByName;
    const bool m_refusesOriginalsBesideMocks;
    bool m_isAskingHost { false };
    std::optional<ModuleID> m_rootThatCompiledCodeLoads;
    bool m_isStatementAskingHost { false };
    bool m_isWalkingRequests { false };
    struct LoadInFlight {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LoadInFlight);

        Strong<JSPromise> promise;
        ModuleID root;
        bool isWalked { false };
        bool hasMarksBelowRoot { false };
        // LoadsInFlight writes the two, and counts by them.
        bool isLoaded { false };
        bool isKnownToBeSettled { false };
        bool hasText { false };
        // m_stepsOfLoader at the call. The loader takes no step in the job of a call: whoever finds another number calls in a later job.
        uint32_t stepsOfLoaderAtCall { 0 };
        // No other text was on its way at the call. `bun run` reads the texts on other threads: such a text comes ahead of whatever is asked for later, and of those that wait together the last often comes first.
        bool cameFirst { false };
        // What lay below the root and had not begun to run when a later call first asked.
        std::unique_ptr<BitVector> modulesLeftToRun;
        // Of those: a search has gone through them and found no cycle.
        std::unique_ptr<BitVector> modulesAboveNoCycle;
        // How many files the loader had run at the call.
        uint64_t numberOfFilesRunAtCall { 0 };
        // Among all the loads that there have been.
        uint64_t number { 0 };
        RefPtr<PlanOfLoad> planWithoutMarks;
    };
    // The loads in the order of their calls. A program may ask for very many graphs in one job, and every step of the loader asks about the loads:
    // a load is found by its root and by its promise, and what the steps ask of all of them is kept as it changes. No step goes through them.
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
        // The only one of them: whatever is asked for while it is here finds its text on the way.
        LoadInFlight* loadThatCameFirst() const { return m_loadThatCameFirst; }

        bool isBeingLoaded(const LoadInFlight& load) const { return !load.isLoaded && !load.isKnownToBeSettled && load.promise->status() == JSPromise::Status::Pending; }
        unsigned numberBeingLoaded() const
        {
            lookAtPromisesThatNobodyTellsOf();
            return m_numberBeingLoaded;
        }
        // What the file has queued runs ahead of the graph.
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

        LoadInFlight& append(VM& vm, JSPromise* promise, ModuleID root, bool hasText, SettlingOfLoad settling)
        {
            auto owned = makeUnique<LoadInFlight>();
            LoadInFlight& load = *owned;
            load.promise.set(vm, promise);
            load.root = root;
            load.hasText = hasText;
            load.cameFirst = hasText && !m_numberWithText;
            load.numberOfFilesRunAtCall = m_numberOfFilesRun;
            load.number = m_numberAtPlaceZero + m_inOrder.size();
            m_inOrder.append(WTF::move(owned));
            m_byRoot.add(root, Vector<LoadInFlight*, 1> { }).iterator->value.append(&load);
            if (!m_byPromise.add(promise, &load).isNewEntry || settling == SettlingOfLoad::IsLookedFor)
                m_promisesThatNobodyTellsOf.append(&load);
            if (load.cameFirst)
                m_loadThatCameFirst = &load;
            m_numberWithText += hasText;
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

        // The loader says so behind the last step of an import().
        void didSettle(JSPromise* promise)
        {
            LoadInFlight* load = find(promise);
            if (!load)
                return;
            // The module exports a `then`, which has yet to settle it.
            if (promise->status() == JSPromise::Status::Pending) {
                if (!m_promisesThatNobodyTellsOf.contains(load))
                    m_promisesThatNobodyTellsOf.append(load);
                return;
            }
            noteSettled(*load);
        }

        // A settled load stays, and is found by its root, until somebody drops it.
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
            m_loadThatCameFirst = nullptr;
            m_size = 0;
            m_numberWithText = 0;
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
            if (m_loadThatCameFirst == &load)
                m_loadThatCameFirst = nullptr;
            m_numberWithText -= load.hasText;
            --m_size;
            m_inOrder[static_cast<size_t>(load.number - m_numberAtPlaceZero)] = nullptr;
            while (m_firstPlace < m_inOrder.size() && !m_inOrder[m_firstPlace])
                ++m_firstPlace;
            // Each place is moved as often as it takes to halve what stands in front of it.
            if (m_firstPlace * 2 < m_inOrder.size())
                return;
            m_inOrder.removeAt(0, m_firstPlace);
            m_numberAtPlaceZero += m_firstPlace;
            m_firstPlace = 0;
        }

        // Nothing where a load was taken out, and only that in front of the first place.
        Vector<std::unique_ptr<LoadInFlight>, 2> m_inOrder;
        size_t m_firstPlace { 0 };
        uint64_t m_numberAtPlaceZero { 0 };
        UncheckedKeyHashMap<ModuleID, Vector<LoadInFlight*, 1>> m_byRoot;
        UncheckedKeyHashMap<JSPromise*, LoadInFlight*> m_byPromise;
        // The host's own loads, and the import() of a module that exports a `then`.
        mutable Vector<LoadInFlight*, 2> m_promisesThatNobodyTellsOf;
        mutable Vector<LoadInFlight*, 2> m_settled;
        LoadInFlight* m_loadThatCameFirst { nullptr };
        unsigned m_size { 0 };
        unsigned m_numberWithText { 0 };
        // Neither loaded nor settled.
        mutable unsigned m_numberBeingLoaded { 0 };
        mutable unsigned m_numberBeingLoadedThatSawFileRun { 0 };
        uint64_t m_numberOfFilesRun { 0 };
    };
    LoadsInFlight m_loadsInFlight;
    Vector<ModuleID, 4> m_modulesWhoseRecordsLoaderHolds;
    BitVector m_modulesBelowWalkedRoots;
    // `bun run` loads a graph level by level, and the steps of one level follow each other with no other job between them. The steps for an ES module run no code of the program, so of the ES modules
    // the loader need only go down one way to the lowest level. With the module of a level on that way it asks for every CommonJS file of the level that is left to run, in their order:
    // while a file runs the loader has it in its hands as under `bun run`, for whatever the file or another graph asks of the loader, and a file that throws fails alone.
    // What lay below the root and was not loaded when the loader came to the root. A require() may load a part of it before the loader comes there: what is left stands on the level where it stood.
    struct PlanOfLoad : RefCounted<PlanOfLoad> {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PlanOfLoad);

        struct Found {
            ModuleID module;
            // A place in the list.
            uint32_t importer;
            // A request for what another module of the same level has asked for hears of it one step later. The most such requests on a way from the root.
            uint32_t laterRequests : 29;
            uint32_t isFile : 1;
            uint32_t wasLoadedSince : 1;
            // Until the next require() comes between.
            uint32_t isKnownToBeLeft : 1;
        };
        struct Level {
            uint32_t first;
            // The most of a module or file of the level, and where one with so many stands.
            uint32_t laterRequests;
            uint32_t placeOfMostLaterRequests;
            bool hasFiles;
        };
        static constexpr uint32_t jobsOfLevel = 5;

        // The levels stand one behind the other, each in the order in which the loader asks.
        Vector<Found, 64> found;
        Vector<Level, 32> levels;
        Vector<uint32_t> knownToBeLeft;
        // From the step in which the loader asks the root for its requests to the one in which the last request so far is heard of.
        uint32_t steps { 0 };
        // The same for the whole graph, where a module asks for one of its own level or of a level above: the levels alone do not say then who hears of what in which step. Zero: they do.
        uint32_t stepsOfWholeGraph { 0 };
        bool asksAcrossLevels { false };
        // Nothing of it was loaded since.
        bool isWhole { true };
        // Of the root.
        Strong<JSCell> load;

        std::span<Found> level(size_t depth) { return found.mutableSpan().subspan(levels[depth].first, (depth + 1 == levels.size() ? found.size() : levels[depth + 1].first) - levels[depth].first); }
    };
    bool isLeftToLoad(PlanOfLoad&, PlanOfLoad::Found&);
    struct LevelOfLoad {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LevelOfLoad);

        struct FileOfNextLevel {
            ModuleID module;
            // The load of any other importer is not in the hands of the loader: the load of the root waits for the file.
            bool isImportedByModuleOnWay;
        };
        std::optional<ModuleID> next;
        Vector<FileOfNextLevel, 4> filesInFrontOfNext;
        Vector<FileOfNextLevel, 4> filesBehindNext;
        // Of this level, with no code of the program to run.
        Vector<ModuleID, 4> filesWithoutCode;
        RefPtr<PlanOfLoad> plan;
        uint32_t depth { 0 };
        // Of the module on the way, in the plan.
        uint32_t place { 0 };
        bool isAskedFor { false };
    };
    UncheckedKeyHashMap<ModuleID, std::unique_ptr<LevelOfLoad>> m_levelsOfLoads;
    // From where a module on a way lists its requests to where the loader asks for them, in the same step.
    struct FileAskedForOnBehalfOfRoot {
        ModuleID requester;
        Identifier key;
        RefPtr<PlanOfLoad> plan;
        uint32_t depth;
    };
    Vector<FileAskedForOnBehalfOfRoot, 4> m_filesAskedForOnBehalfOfRoot;
    Vector<uint32_t> m_placesSeenByPlans;
    uint32_t m_firstPlaceOfNextPlan { 1 };
    // Their imported files ran without the loader. The set is filled from the list when it is asked.
    Vector<ModuleID, 4> m_rootsLoadedWithoutLoader;
    BitVector m_filesLoadedWithoutLoader;
    BitVector m_filesLoadedByRequire;
    BitVector m_filesWithKnownLoad;
    BitVector m_filesLoadedSynchronously;
    BitVector m_filesWhoseNextLoadIsFirst;
    void noteModulesLoadedWith(ModuleID root);
    const MockOperations* m_mockOperations;
    // What the host's rule said, by (mocked, importer). Emptied where a mock is laid or ends.
    mutable UncheckedKeyHashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_importersAskedAboutOriginal;
    // The keys of m_importersAskedAboutOriginal that were ever answered with yes. The ended mock of a preload outlives the test file, and the answer hangs on what the importer is.
    mutable UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_importersThatGotOriginal;
    // The body ran beside the mock that stands in place of the module, for an importer that gets the module itself. It does not run again.
    BitVector m_modulesThatRanBesideMock;
    // An import() of a package has come for the module itself, and its body has yet to run.
    BitVector m_modulesOwedBesideMock;
    // No body of a mocked module is called but these: answerRequireWithOriginal(). One may load another.
    BitVector m_filesWhoseOriginalRuns;
    bool m_makesNamespaceOfModuleItself { false };
    // The ES modules of the program that a graph has loaded: wasLoadedWithGraph().
    BitVector m_modulesLoadedBesideGroups;
    // VMState::momentOfLoads when the bit of the module was set. Empty outside a test run.
    Vector<uint32_t> m_momentsOfLoads;
    BitVector m_loadedInPreload;
    bool m_hasBegunTestFile { false };
    struct MockOverModuleThatHasRun {
        uint32_t momentLaid;
        uint32_t momentOfTestFile;
        bool isInPlace { false };
        std::optional<BitVector> exportsLeftOutBefore { };
        // What the object of the module does not show, as it was when the mock came.
        BitVector exportsHiddenBefore { };
        // When this mock was laid. momentLaid: the first of the test file.
        uint32_t momentThisWasLaid { 0 };
        // Somebody has asked for the object of the module itself. One that nobody has seen goes with the mock.
        bool namespaceIsHeld { false };
    };
    Vector<std::pair<ModuleID, JSObject*>, 4> m_namespacesBeingMade;
    UncheckedKeyHashMap<ModuleID, MockOverModuleThatHasRun> m_mocksOverModulesThatHaveRun;
    struct FirstMockInTestFile {
        uint32_t moment;
        uint32_t momentOfTestFile;
    };
    UncheckedKeyHashMap<ModuleID, FirstMockInTestFile> m_firstMocksInTestFile;
    // An importer that the loader had let go, and that somebody has imported since: `bun test` loads it anew and links it to what stands then.
    UncheckedKeyHashMap<ModuleID, FirstMockInTestFile> m_momentsLoadedAnew;
    // Whoever holds a module of `bun test` that was linked to a mock keeps that mock. The objects: LoadedModule::endedMocks, by their place in the list.
    struct EndedMock {
        uint32_t momentLaid;
        uint32_t momentEnded;
        uint32_t momentOfTestFile;
        bool wasLaidInPreload;
    };
    UncheckedKeyHashMap<ModuleID, Vector<EndedMock, 2>> m_endedMocks;
    struct MomentMockWasLaid {
        uint32_t moment { 0 };
        uint32_t momentOfTestFile { 0 };
        bool isInPreload { false };
    };
    UncheckedKeyHashMap<ModuleID, MomentMockWasLaid> m_momentsWaitingMocksWereLaid;
    UncheckedKeyHashMap<ModuleID, MomentMockWasLaid> m_momentsStandingMocksWereLaid;
    // VMState::momentOfTestFile when a mock ended that is let go later.
    UncheckedKeyHashMap<ModuleID, uint32_t> m_testFilesInWhichMocksEnded;
    // How many objects LoadedModule::namespacesOfEndedMocks holds.
    UncheckedKeyHashMap<ModuleID, unsigned> m_numbersOfNamespacesOfEndedMocks;
    // Of LoadedModule::mockLaidInPreload.
    UncheckedKeyHashMap<ModuleID, MomentMockWasLaid> m_momentsMocksOfPreloadWereLaid;
    MomentMockWasLaid momentOfNow();
    void keepEndedMock(ModuleID fileOrModule, JSObject* mock, MomentMockWasLaid, uint32_t momentEnded = 0);
    JSValue wordWhereNoMockStands(ModuleID fileOrModule) const;
    void noteMomentMockComesInPlace(ModuleID fileOrModule, JSObject* mock, bool stoodInPlace, bool leavesEarlierMockToItsImporters);
    void keepMockThatStoodInPlace(ModuleID fileOrModule, ModuleID module, JSObject* endedMock);
    void giveBackNamespacesHandedOn(LoadedModule& mocked);
    void setAsideNamespaceUnderMock(ModuleID);
    bool isSomeLoadUnderWay() const;
    BitVector m_namespacesReplacedByMock;
    // A mock stood in place of the module while its importers ran, and has ended. They read the names of the module itself now, whose body has yet to run.
    BitVector m_modulesOwedToImportersThatRan;
    // evaluateModulesOwedBelow() has been there since the last module came to be owed.
    BitVector m_modulesWithNothingOwedBelow;
    // It passed over these, which were mocked. What it found holds for as long as they are.
    Vector<ModuleID, 4> m_mockedModulesPassedOverByWalks;
    // The host has ended the mock that stands in place of the module. Whoever was linked against the mock goes on reading it, as under `bun test`, until a load comes to the module.
    BitVector m_modulesWithMockToLetGo;
    // A load that comes by brings them back.
    BitVector m_modulesThatLoaderLetGo;
    BitVector m_importersOfMockThatEnded;
    Vector<Vector<uint32_t, 2>> m_importersByStatement;
    // A file ran before the loader came to it and queued jobs. They are queued where the loader comes to the file, as under `bun run`.
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
    // The body had run when the mock was made: in an earlier test file, or beside an earlier mock. It does not run again for whoever gets the module itself.
    BitVector m_modulesThatHadRunWhenMocked;
    // The factory of a mock imports the module that it mocks. Such an import comes to the mock behind the end of that run, and shares what became of it: `bun test` calls the factory once.
    BitVector m_modulesWhoseFactoryRuns;
    Vector<ModuleID, 2> m_importsMadeByOwnFactory;
    // AfterFailure::AsksAgain
    BitVector m_modulesWithMockThatAsksAgain;
    // ModuleHost::layMocksAgain
    BitVector m_modulesThatLayMocksAgain;
    // The factory of a mock failed while they were loaded, and they import the mocked module by statements, one through another. The loader of `bun test` keeps the record of each with that error:
    // whoever loads one, or comes to one by a statement, gets the error, until the module is taken out of require.cache or the module below is mocked anew.
    BitVector m_modulesWithLoadEndedByMock;
    // A statement came to them when the factory failed, so the loader keeps their records too: the next statement gets the error, and the factory is not asked. An import() of such a module drops the
    // record and asks the factory; a require() asks the factory and leaves the record.
    BitVector m_mockedModulesWithErrorKeptByLoader;
    bool m_loaderMayHoldLoadsThatFactoryEnded { false };
    std::optional<ModuleID> m_moduleThatProgramImportsNow;
    BitVector m_modulesWithMockThatWaited;
    BitVector m_modulesLoadedBeforeMockThatWaited;
    // runBodyOfRequiredFileAheadOfMock(). The mock may wait on: a require() calls no factory that awaits.
    BitVector m_filesThatRanAheadOfMock;
    // The loader has only the root of a graph in its hands, so it cannot tell that a module belongs to a graph that was asked for earlier.
    struct ImportBehindLoad {
        ModuleID root;
        ModuleID module;
        std::optional<int64_t> referrerAsyncOrder;
        Strong<JSPromise> promise;
    };
    Vector<ImportBehindLoad> m_importsBehindLoads;
    // Their graphs were linked, and ran in that job.
    Vector<ImportBehindLoad> m_importsBehindEvaluations;
    uint32_t m_stepsOfLoader { 0 };
    LoadInFlight* findEarlierLoadAbove(ModuleID);
    bool leadsToModuleLeftToRun(const ModuleRequestData&) const;
    bool leadsTo(ModuleID from, ModuleID to);
    const BitVector& modulesLeftToRunBelow(LoadInFlight&);
    bool isAboveCycleLeftToRun(LoadInFlight&, ModuleID);
    bool asksForLastTextBelow(const LoadInFlight&, ModuleID);
    void makeImportAgain(const ImportBehindLoad&);
    bool hasFileOfGraphBeingLoadedBegun() const;
    bool isTextLeftToRead(ModuleID);
    JSPromise* startImportInLaterTurn(ModuleID, std::optional<int64_t> referrerAsyncOrder);
    struct TextOnItsWay {
        // The turn in which the text of whoever asks has come, then the order of the requests.
        uint64_t place;
        // Of the program for a call, of the fetch for a request of the loader.
        Strong<JSPromise> promise;
        ModuleID module;
        // Null for a call.
        Strong<JSCell> requested;
        std::optional<int64_t> referrerAsyncOrder;
    };
    Vector<TextOnItsWay, 2> m_textsOnTheirWay;
    uint32_t m_turnOfTexts { 0 };
    uint32_t m_numberOfTextsAskedFor { 0 };
    // By file. `bun run` takes every step that a text allows in the turn in which the text comes: a module asks for what it imports, and a file runs and calls import().
    // The steps of an executable that has every text follow one another otherwise: a file calls while modules whose texts came ahead of its own have yet to ask.
    Vector<uint32_t> m_turnsInWhichTextsCame;
    void noteThatTextHasCome(ModuleID);
    // The loader has asked nobody for the root of a graph: it has the text now.
    uint32_t turnInWhichTextCame(ModuleID module) const
    {
        uint32_t turn = m_turnsInWhichTextsCame.isEmpty() ? 0 : m_turnsInWhichTextsCame[fileOf(module).index()];
        return turn ? turn : m_turnOfTexts;
    }
    void holdText(JSPromise*, ModuleID, ModuleRecord* requested, std::optional<int64_t> referrerAsyncOrder, uint32_t turnOfRequest);
    bool doOtherJobsWait() const;
    struct TextThatWaits {
        // As TextOnItsWay::place: where it stands once it is put in line.
        uint64_t place;
        // m_turnOfDeliveries at the call.
        uint32_t turnOfCall;
        // Of the caller for an import(), of the loader's fetch for the root of a graph.
        Strong<JSPromise> promise;
        ModuleID module;
        // Null for an import() that is made again.
        Strong<JSCell> requestedRecord { };
        std::optional<int64_t> referrerAsyncOrder { };
        ImportCaller caller { ImportCaller::CompiledCode };
        // How many jobs behind the first such call of its turn it was made: it is made again as many jobs behind that one.
        uint32_t jobsBehindFirstCall { 0 };
    };
    // One job in each round of the queue, for as long as somebody has to know that a round has passed. That job is nobody who could tell in how many jobs a graph is loaded: doOtherJobsWait().
    Strong<JSPromise> m_promiseOfJobOfRound;
    bool m_isJobOfRoundQueued { false };
    uint32_t m_jobsBehindFirstCall { 0 };
    // What the jobs behind a delivery call for is not delayed a second time: the run lies one turn behind the executable's, once.
    bool m_isInJobsBehindDelivery { false };
    uint32_t m_turnOfDeliveries { 0 };
    void queueJobOfRound(ModuleID moduleOfJob);
    void runJobOfRound(ModuleID moduleOfJob);
    Vector<TextThatWaits, 2> m_callsToMakeAgain;
    bool makeCallAgain(JSPromise*);
    void makeCallAgain(TextThatWaits&);
    // While a text is on its way none waits here: putCallsThatWaitApartInLine().
    Vector<TextThatWaits> m_textsThatWait;
    size_t m_numberOfTextsThatHaveCome { 0 };
    std::span<TextThatWaits> textsThatWait() { return m_textsThatWait.mutableSpan().subspan(m_numberOfTextsThatHaveCome); }
    bool isTextReadOnOtherThread(ModuleID file);
    void putCallsThatWaitApartInLine();
    void forgetTextsThatWait();
    ModuleID fileOf(ModuleID fileOrFace) const { return m_table.fileBehindFace(fileOrFace).value_or(fileOrFace); }
    // Tarjan's algorithm: the files of an import cycle are settled together, when the first of them to be visited is done.
    struct WalkForEvictedFiles {
        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> visitOrder;
        Vector<uint32_t, 32> unsettledFiles;
        bool importsAreFollowed { false };
    };
    bool importsEvictedFile(ModuleID, WalkForEvictedFiles&, uint32_t& earliestVisitReached);
    bool importsEvictedFile(ModuleID);
    void followImportsOfModulesThatHaveRun();
    void followImportsAnewAtNextQuestion();
    void evaluateModulesOwedBelow(ModuleID root, bool hasImporterThatRan = false);
    void leaveToHost(ModuleID file);
    bool isChangedOnDisk(ModuleID file);
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
    // The loader goes through its graph, and comes to files that have run.
    std::optional<ModuleID> m_rootWhoseFilesRanAhead;
    RefPtr<MicrotaskQueue> m_jobsThatWaitForLoader;
    // The queue that evaluateSynchronously() has opened for the files that run ahead. The collector finds it from the VM.
    VM::SynchronousModuleQueue* m_stepsThatWaitForLoader { nullptr };
    bool m_isStartingRuntimeModule { false };
    // The link of the entry point that the image has a plan for stores no status.
    bool m_entryPointIsLinked { false };
};

// What the host asks the engine for while the engine asks it for a module that the image names is no load by a name that the build did not follow.
class RequestToHostScope {
public:
    explicit RequestToHostScope(Modules& modules)
        : m_isAskingHost(modules.m_isAskingHost, true)
    {
    }

private:
    SetForScope<bool> m_isAskingHost;
};

Modules& modulesOf(Instance&);
Modules* modulesOf(JSGlobalObject*);
void forgetUnfinishedLoadsOfRetiredRealms(VM&);

// Counts what a door between the host and the engine does to the state of a mock, where the counts are asked for. There a door that leaves a module in no state says so on stderr.
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
    return static_cast<StoredModuleStatus>(std::min(m_storedStatuses[module.index()], static_cast<uint8_t>(StoredModuleStatus::Evaluating)));
}

ALWAYS_INLINE void Modules::setStoredStatus(ModuleID module, StoredModuleStatus status)
{
    RELEASE_ASSERT(module.index() < m_storedStatuses.size());
    // See evict().
    if (status == StoredModuleStatus::Evaluated && m_filesToLoadAgain.get(module.index())) [[unlikely]]
        return;
    m_storedStatuses[module.index()] = static_cast<uint8_t>(status);
}

ALWAYS_INLINE ModuleStatus Modules::status(ModuleID module) const
{
    switch (storedStatus(module)) {
    case StoredModuleStatus::Unlinked:
        return !data(module).needsLink() || (m_entryPointIsLinked && data(module).isLinkedWithEntryPoint()) ? ModuleStatus::Linked : ModuleStatus::Unlinked;
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

// What answerRequire() comes to at its end for a module of the program that has run and has answered before, where each of its steps up to there does nothing:
// no mock waits to run, no mock that has ended has left a body unrun, no file was evicted, no require() of the module is under way. Empty: it has to take the steps.
// A step that is added to answerRequire() and can do something in that state needs its test here.
ALWAYS_INLINE JSValue Modules::settledAnswerOfRequire(ModuleID target, RequiredBy requiredBy) const
{
    if (requiredBy != RequiredBy::Referrer || !m_modulesWithMockToRun.isEmpty() || !m_modulesWithMockToLetGo.isEmpty() || !m_modulesThatLayMocksAgain.isEmpty() || (!m_modulesOwedToImportersThatRan.isEmpty() && (!m_modulesWithNothingOwedBelow.get(target.index()) || hasMockEndedThatWalksPassedOver())) || !m_evictedFiles.isEmpty())
        return { };
    const LoadedModule* loaded = loadedIfExists(target);
    if (!loaded || !loaded->requiredValue || loaded->isBeingRequired || loaded->objectMadeByRequire || storedStatus(target) != StoredModuleStatus::Evaluated || data(target).isOfLibrary())
        return { };
    // `bun test` loads it anew: bringBackWhatLoaderLetGoBelow().
    if (m_importersOfMockThatEnded.get(target.index())) [[unlikely]]
        return { };
    // The answer goes by who asks.
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

// Of an ES module of the program. A graph is loaded before a module of it begins, and nothing is stored when a module that needs no link is loaded, nor when one with nothing to run is passed over.
ALWAYS_INLINE bool Modules::wasLoadedWithGraph(ModuleID module) const
{
    return m_modulesLoadedBesideGroups.get(module.index());
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

// ModuleHost::mockOperations. Each does nothing in a program where no mock was ever laid.
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

// Whoever calls it has seen that a mock waits.
ALWAYS_INLINE bool Modules::runMockThatWaits(ModuleID module)
{
    return m_mockOperations->runMockThatWaits(*this, module);
}

ALWAYS_INLINE auto Modules::mockThatAnswersFor(ModuleID passingOn, const ModuleExportData& passedOn, MockOfModule ofPassingOn) const -> std::optional<AnswerOfMock>
{
    if (!m_numberOfMocksInPlace) [[likely]]
        return std::nullopt;
    return m_mockOperations->mockThatAnswersFor(*this, passingOn, passedOn, ofPassingOn);
}

ALWAYS_INLINE bool Modules::isMockedInItsImage(ModuleID module, MocksAskedFor which)
{
    return m_mockOperations && m_mockOperations->isMockedInItsImage(*this, module, which);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
