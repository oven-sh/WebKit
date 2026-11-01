/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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

class HeapAnalyzer;
class JSCellButterfly;
class JSModuleLoader;
class ModuleLoadingContext;
class ModuleRegistryEntry;
class ScriptFetchParameters;
class ThrowScope;

namespace AOT {

class ModuleEnvironment;
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

// capturedFields: in place of capturedExports, until somebody asks for all the names.
#define FOR_EACH_REFERENCE_OF_LOADED_MODULE(macro) \
    macro(JSObject*, namespaceObject) \
    macro(JSObject*, deferredNamespaceObject) \
    macro(JSObject*, importMeta) \
    macro(JSObject*, requirer) \
    macro(JSObject*, objectMadeByRequire) \
    macro(JSValue, exportsGivenToObjectMadeByRequire) \
    macro(JSObject*, firstRequirerOfOtherImage) \
    macro(JSValue, requireFunction) \
    macro(JSValue, requiredValue) \
    macro(JSValue, valueInRequireCache) \
    macro(JSValue, valueInRequireCacheBeforeMock) \
    macro(JSObject*, exportsOfMock) \
    macro(JSObject*, exportsBeforeMock) \
    macro(JSObject*, variablesBeforeMock) \
    macro(JSValue, valueBeforeMock) \
    macro(JSObject*, mockInPlace) \
    macro(JSObject*, recordBeforeMockInPlace) \
    macro(JSObject*, namespaceBeforeMockInPlace) \
    macro(JSObject*, capturedExportsBeforeMockInPlace) \
    macro(JSValue, requiredValueBeforeMockInPlace) \
    macro(JSValue, exportsOfOriginalBesideMock) \
    macro(JSValue, answerOfMockWhileOriginalRuns) \
    macro(JSObject*, exportsAddedByMock) \
    macro(JSValue, errorKeptByLoader) \
    macro(JSObject*, exportsWhileRequired) \
    macro(JSObject*, capturedExports) \
    macro(JSObject*, deferredExportsSource) \
    macro(JSCellButterfly*, capturedFields) \
    macro(JSCellButterfly*, capturedStars) \
    macro(JSObject*, sourceOfCapturedFields) \
    macro(JSValue, errorOfMockKeptByLoader) \
    macro(AbstractModuleRecord*, uncompiledRecord)

struct EvaluationContext {
    ModuleEvaluationStack& stack;
    unsigned index { 0 };
    std::optional<int64_t> referrerAsyncOrder { };
    JSPromise* importPromise { nullptr };
};

// A set of mocked modules, and what follows from it and the table alone. Whoever began with one set goes on with it, and the next realm that mocks the same modules takes it over.
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
    JSValue readExport(ModuleID, unsigned index);
    JSValue readExportFromOutside(ModuleID, unsigned index);
    JSValue readExportIfInitialized(ModuleID, unsigned index) const;
    bool isExportAmbiguous(ModuleID, unsigned index) const;
    bool tryReplaceExport(ModuleID, UniquedStringImpl* name, JSValue);
    void fillExportsOfBodyThatNeverRuns(ModuleID);
    // Importers are bound past a module that only hands names on, and a body without code never runs: its status says nothing. Plain Bun has loaded it with the first module that imports it.
    bool wasLoadedWithImporter(ModuleID) const;
    // The factory of a mock that waits runs when the first importer is linked, and that importer has begun to load by then: what was loaded when the mock was made decides.
    bool wasLoadedWhenMockWasMade(ModuleID);
    // A file imports from its own image and from those below. The files of this image that the import statements of an image above lead to, from modules there that have begun, or whose evaluation is under way.
    enum class ImportersAbove : bool { HaveBegun, AreUnderEvaluation };
    Vector<ModuleID, 8> filesImportedByImagesAbove(ImportersAbove) const;
    bool tryReplaceExport(ModuleID, UniquedStringImpl* name, JSValue, BitVector& seenBehindStars);
    bool tryReplaceExportBehindStars(ModuleID, UniquedStringImpl* name, JSValue, BitVector& seenBehindStars);
    void keepExportOfOtherImageBeforeMock(ModuleID external, UniquedStringImpl* name);
    std::optional<bool> tryReplaceExportInImageBelow(ModuleID external, UniquedStringImpl* name, JSValue);
    bool tryReplaceExportOfUncompiledModule(ModuleID external, UniquedStringImpl* name, JSValue);
    bool tryReplaceCapturedExport(ModuleID, UniquedStringImpl* name, JSValue);
    bool tryReplaceExportAddedByMock(ModuleID, UniquedStringImpl* name, JSValue);
    struct SyntheticExportPlace {
        ModuleID module;
        uint32_t name;
    };
    const SyntheticExportPlace* syntheticExportKeptIn(SlotData variable);
    bool mayBeMocked(ModuleID) const;
    ModuleID moduleWithExportsOf(ModuleID) const;
    void willMock(ModuleID);
    void didEndMock(ModuleID);
    void markEvaluated(ModuleID module) { setStatus(module, ModuleStatus::Evaluated); }
    bool isMocked(ModuleID module) const { return m_mocks && m_mocks->modules.get(module.index()); }
    bool hasMockEndedThatWalksPassedOver() const;
    // The object of the host that stands in place of a module that was mocked ahead of its load. Of a CommonJS file or of its face.
    JSObject* mockInPlace(ModuleID module) const
    {
        if (!m_numberOfMocksInPlace) [[likely]]
            return nullptr;
        const LoadedModule* loaded = loadedIfExists(module);
        return loaded ? loaded->mockInPlace.get() : nullptr;
    }
    bool someMockStandsInPlace() const { return m_numberOfMocksInPlace; }
    bool setMockInPlace(ModuleID, JSObject* mock, JSValue whatRequireAnswers);
    void letGoOfMockAtNextLoad(ModuleID);
    void letGoOfMockThatEnded(ModuleID);
    void letGoOfMockThatEndedSlow(ModuleID);
    bool letGoOfMockOfLibraryModuleAtNextLoad(ModuleID);
    void letGoOfMocksThatEnded();
    bool hasMockThatEnded(ModuleID module) const { return m_modulesWithMockToLetGo.get(module.index()); }
    bool loadComesToModuleWhoseMockEnded(ModuleID) const;
    bool setMockInPlaceOfLibraryModule(ModuleID, JSObject* mock, JSValue whatRequireAnswers);
    bool willLeaveModuleAlone(ModuleID, bool onlyIfLoadedInPreload, std::optional<ModuleID> askingAsHostNamesIt);
    bool isGraphRunning() const { return m_innermostEvaluation || m_rootWhoseFilesRun || m_rootWhoseFilesRanAhead || m_instance.vm->synchronousModuleQueue(); }
    bool hasLinkedImporterYetToRun(ModuleID, const String& keyOfFileInImageBelow, std::optional<ModuleID> askingAsHostNamesIt) const;
    uint32_t placeOfMomentOfLoad(ModuleID) const;
    uint32_t momentOfFirstMockInTestFile(ModuleID);
    // The life of the mock of a module: R:launch-doors-stores-and-defines/THE-LIFE-OF-A-MOCK.md. Computed from what the sets and flags below say. NoneOfThem: together they say what no state of the table is.
    enum class MockState : uint8_t { None, Waits, Runs, InPlace, InPlaceWhileAnotherWaits, InPlaceOverModuleThatHasRun, Fills, LaidOver, Failed, Ended, NoneOfThem };
    MockState mockState(ModuleID);
    // For the counts alone: the state, the status of the body, and, for NoneOfThem, what is set.
    String describeMockState(ModuleID);
    void countMockStates(ASCIILiteral door, const String& before, ModuleID);
    bool isLeftAloneByMock(ModuleID fileOrFace) const { return !m_mocksOverModulesThatHaveRun.isEmpty() && m_mocksOverModulesThatHaveRun.contains(m_table.fileBehindFace(fileOrFace).value_or(fileOrFace)); }
    bool setMockOverModuleThatHasRun(ModuleID, JSObject* mock, JSValue whatRequireAnswers);
    void endMockOverModuleThatHasRun(ModuleID);
    bool seesMockOf(ModuleID mocked, ModuleID importer) const;
    JSObject* namespaceOfModuleItself(ModuleID);
    bool isHiddenInModuleItself(ModuleID, unsigned index) const;
    JSValue readExportOfModuleItself(ModuleID, unsigned index);
    // `bun test` takes whoever imports a module out of its loader when a mock of the module ends, and when a mock is laid over what an earlier test file loaded. A mock of such an importer
    // comes ahead of its load, though the body has run here.
    enum class ImportersLetGo : bool { All, AllButThoseThatThisTestFileLinkedToModule };
    void letGoOfImportersOf(ModuleID, ImportersLetGo);
    void letGoOfModuleLoadedAsMock(ModuleID module) { m_modulesThatLoaderLetGo.set(module.index()); }
    bool wasLetGoByLoader(ModuleID module) const { return m_modulesThatLoaderLetGo.get(module.index()); }
    bool hasMockLaidOver(ModuleID) const;
    void takeNamesOfMockInPlace(ModuleID, JSObject* mock);
    void letRecordAnswerByMock(ModuleRecord*, JSObject* mock);
    void letGoOfRecordThatAnswersByMock(ModuleID);
    struct AnswerOfMock {
        JSObject* mock;
        Identifier name;
        ModuleID mocked;
    };
    std::optional<AnswerOfMock> mockThatAnswersFor(ModuleID passingOn, const ModuleExportData& passedOn) const;
    std::optional<AnswerOfMock> mockThatAnswersForSlow(ModuleID passingOn, const ModuleExportData& passedOn) const;
    enum class MocksAskedFor : uint8_t { All, ThoseThatAddNames };
    bool isMockedInItsImage(ModuleID, MocksAskedFor = MocksAskedFor::All);
    bool isMockedInItsImageSlow(ModuleID, MocksAskedFor);
    // A mock that comes before the body of an ES module has run stands for the whole module: the loader of `bun run` takes what the factory answers for the module itself.
    // So a name that the factory leaves out is no export: no key of the namespace object. See fillExportsOfBodyThatNeverRuns() for whoever reads the variable.
    void leaveOutWhatMockLacks(ModuleID, JSObject* exportsOfMock);
    // And a name that the factory gives and the module lacks is an export: a key of the namespace object, where it is read. The names are settled once the module is loaded.
    void takeWhatMockAdds(ModuleID, bool comesAheadOfLoad);
    bool isAddedByMock(ModuleID, const Identifier&);
    // `bun test` loads the original first and lays such a mock over it.
    bool isMockedByFactoryThatTakesOriginal(ModuleID module) { return m_modulesWithMockThatAsksAgain.get(module.index()) && isMocked(module); }
    void appendNamesAddedByMock(ModuleID, WTF::Vector<Identifier>&);
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
    bool runBodyOfLinkedModuleAheadOfMock(ModuleID);
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
    void oweLibraryModuleToImagesAbove(ModuleID);
    void handLibraryModuleToImagesAbove(ModuleID);
    void handFileToImagesAbove(ModuleID file);
    void takeNamesBehindFollowedStarsAnew();
    void keepWhatMockIsLaidOver(ModuleID);
    void didLayMock() { m_moduleWithMockBeingLaid = std::nullopt; }
    void endMockLaidOverFile(ModuleID);
    JSValue exportsBeforeMock(ModuleID);
    void runMockThatWaits(ModuleID);
    void runMockThatWaitsSlow(ModuleID);
    // `build.module("dep", ..)` of a plugin: from now on the loader answers for the name with another module. A file that is linked keeps what it has.
    void forgetWhatNamesResolvedTo();
    void willMockLibraryModule(ModuleID);
    bool letMockStandInPlaceOfLibraryModule(ModuleID);
    enum class RootOfLoad : bool { BeginsLoad, IsImportedByStatement };
    void runMocksBelow(ModuleID root, RootOfLoad = RootOfLoad::BeginsLoad);
    void runMocksBelowSlow(ModuleID root, RootOfLoad);
    static const MockOperations& mockOperations();
    JSPromise* factoryThatWaitsBelow(ModuleID root, FactoryAskedForBy, JSValue& state);
    void runMocksBelowModuleThatLoaderFetches(ModuleID);
    void runMocksBelowModuleThatLoaderFetchesSlow(ModuleID);
    bool mockFailedBehindImportStatement(ModuleID);
    void didFailToFetchRequest(ModuleID requester, const Identifier& key, JSValue error);
    void dropErrorOfLoadEndedByMock(ModuleID);
    void dropErrorsOfLoadsEndedByMockOf(ModuleID mocked);
    void dropErrorOfMockKeptByLoader(ModuleID);
    void dropErrorsOfLoadsThatCameTo(const ScopedLambda<bool(ModuleID)>& isMocked);
    bool standsForFileWithKey(ModuleID, const Identifier& key);
    JSValue errorOfLoadEndedByMock(ModuleID module) const { return m_modulesWithLoadEndedByMock.get(module.index()) ? loadedIfExists(module)->errorKeptByLoader.get() : JSValue(); }
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
    JSObject* moduleNamespaceForImageAbove(ModuleID, ModulePhase);
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
    // Of this image.
    std::optional<ModuleID> firstModuleRequestedByHost() const { return m_firstModuleRequestedByHost; }
    bool isHeldForImport(ModuleID) const;
    // What require() answers for a file of data goes by whether something has imported the file: isHeldForImport().
    void didImportFromUncompiledCode(ModuleID module, bool isRootOfImportCall)
    {
        if (data(module).kind != ModuleKind::Data)
            return;
        if (isRootOfImportCall && !recordIfExists(module)) {
            m_dataFilesAskedForByImportCallAlone.set(module.index());
            return;
        }
        m_dataFilesAskedForByImportCallAlone.clear(module.index());
        m_dataFilesImportedByUncompiledCode.set(module.index());
    }
    // The first file that the loader comes to is the main one, also where the module that imports it is not compiled.
    void didImportFileFromUncompiledCode(ModuleID fileOrFace);
    // The host starts a realm with one module, the entry point, and one image of the realm holds it.
    bool isEntryPointOfRealm(ModuleID module) const { return m_firstModuleRequestedByHost == module && !m_otherImageHoldsEntryPointOfRealm; }
    // Those of the image that holds it. Null before the host has asked for a module.
    Modules* modulesWithEntryPointOfRealm();
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
    StringView askerOfModule() const { return m_askerOfModule ? StringView(*m_askerOfModule) : StringView(); }
    void forEachFileAwaitingAtTopLevel(const WTF::Function<void(int64_t turn, const String& key)>&);
    void evict(ModuleID, bool isUndoneForKeptFiles = false);
    void willRunCodeThatKeepsFiles();
    void keepFilesThatHaveRun();
    void bringBackKeptFiles(const WTF::Function<void(const String&)>&);
    void forgetEvaluation(ModuleID);
    void forgetRunThatThrew(ModuleID);
    void forgetRunOfLibraryModule(ModuleID);
    JSValue executeForLoader(ModuleID);
    void recordRequireFromOtherImage(ModuleID target, JSObject* moduleObjectOfReferrer);
    // The other side of it: a require() of `referrer` has returned a file of another image, which `required` names as the host names it.
    void recordRequireOfOtherImage(ModuleID referrer, ModuleID required);
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
    // `bun test` reads a file when something first loads it. A test may have written it anew since the build read it.
    bool isChangedOnDisk(ModuleID file);
    void takeOutOfLoader(ModuleID);
    void forgetFormAnsweredFirst(ModuleID file);
    JSObject* requireCacheEntry(ModuleID);
    bool isEvicted(ModuleID module) const { return m_evictedFiles.get(fileOf(module).index()); }
    bool hasRun(ModuleID);
    // For questions between which no code runs and no status is stored.
    bool hasRun(ModuleID, bool& importsAreFollowed);
    bool isListedWhileItRuns(ModuleID) const;
    void keepObjectMadeByRequire(ModuleID, JSObject* moduleObject);
    void forgetObjectMadeByRequire(ModuleID);
    bool holdsCodeThatMayBeEvicted();
    BitVector modulesThatHaveRun();
    bool isLeftToHost(ModuleID);
    void didEvictFromOtherImage()
    {
        m_otherImageHasEvictedFiles = true;
        m_filesAboveNoEvictedFile.clearAll();
    }
    // The text of a data: URL that the program imports under a constant name is part of the program.
    bool isTextNamedByProgram(StringView url) const;
    bool refusesLoadsByName() const { return !m_loadsModulesByName && !m_isAskingHost && !m_runsUncompiledCode && !m_hostRunsCopiesFromDisk; }
    bool isAskingHost() const { return m_isAskingHost; }
    // A name that the build did not follow finds an entry point and no other file of the image with code.
    bool refusesRequestFromHost(ModuleID module) const { return !m_admitsInspector && !m_isAskingHost && !data(module).isEntryPoint() && (data(module).kind != ModuleKind::Data || !m_findsDataFilesByName) && m_table.hasFilePath(module) && !data(module).isLibraryCodeWithFilePath() && !m_runsCopyFromDisk; }
    bool refusesOriginalsBesideMocks() const { return m_refusesOriginalsBesideMocks; }
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
    // The modules that compiled code begins to evaluate below the body fail with it.
    JSValue callBodyOfModuleEnteredByLoader(ModuleID module) { return callBodyUnderOwnMark(module, false); }
    void evaluateModuleInRecord(ModuleID);
    // Says whether the start that asks goes on to run the module.
    bool beginModuleInRecord(ModuleID);
    struct ModuleOfRealm {
        Modules* modules;
        ModuleID module;
        // One more than the place, among the ES modules of other images that the image of the root imports, of the one that leads to it. Zero: the image of the root names it itself.
        uint32_t entry { 0 };
        uint32_t levelBelowRoot { 0 };
        uint32_t placeOfImporter { 0 };
    };
    using ModulesOfRealm = Vector<ModuleOfRealm, 8>;
    struct FilesLeftToLoader {
        ModulesOfRealm requests;
        ModulesOfRealm importers;
    };
    struct RequestsMadeOnWay : RefCounted<RequestsMadeOnWay> {
        ModulesOfRealm modules;
    };
    struct LevelOfRealm {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LevelOfRealm);

        ModulesOfRealm requests;
        RefPtr<RequestsMadeOnWay> made;
        ModulesOfRealm importersOfUncompiledCode;
        Modules* modulesOfRoot { nullptr };
    };
    // Says whether a file ran.
    bool evaluateImportedCommonJSFiles(ModuleID root, JobsOfImportedFiles = JobsOfImportedFiles::StayQueued, ErrorOfImportedFile = ErrorOfImportedFile::IsThrown, ModuleGraphLoadingState* = nullptr, FilesLeftToLoader* leftToLoader = nullptr);
    std::pair<Modules*, ModuleID> placeInRealm(ModuleID);
    bool isFileLeftToRun(ModuleID fileOrFace) const;
    ModulesOfRealm filesOfRealmLeftToRunBelow(ModuleID root, ModulesOfRealm& entries, ModulesOfRealm& importers);
    bool layWayThroughRealm(ModuleID root, FilesLeftToLoader&, LevelOfRealm& levelOfRoot);
    void takeUpWayThroughRealm(ModuleID root);
    bool isOnWayBelowRootOfItsImage(ModuleID) const;
    AbstractModuleRecord* recordOfRequestedModule(ModuleID ofSomeImage);
    void queueJobsThatWaitForLoader();
    void handGraphsToLoader();
    void didLoadGraph(ModuleID root);
    void didFailToLoadGraph(ModuleID root);
    void evaluateFilesOfLevel(ModuleID moduleOnWay, bool runsWhenLoaded);
    void captureExportsAgain(ModuleID);
    void exportWhatMockNamesAlone(ModuleID file, bool hasRun);
    bool exportsWhatMockNamesAlone(ModuleID) const;
    void fillNamesThatMockLacks(ModuleID file);
    void answerFirstRequireBehindMock(ModuleID file);
    void noteLoadBehindMock(ModuleID fileOrFace, bool isRequired);
    // The start of the module is on the stack only to run the files that the module imports.
    bool isRunningFilesOf(ModuleID root) const { return m_rootWhoseFilesRun == root; }
    bool stopLoadingBehindFile(ModuleID file);
    void evaluateLoadedCommonJSFile(ModuleID);
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
    std::optional<ModuleFailureID> statementThatEndsLoad(ModuleID) const;
    void appendRequestThatFindsNothing(ModuleFailureID statement, ModuleRecord*);
    ModuleGraphLoadingState* loadThatWaitsFor(ModuleID requester, const Identifier& key, ModuleGraphLoadingState* loadOfRequester, unsigned& jobsUntilLoadHearsOfFailure);
    ModuleRecord* appendRequestsOfUncompiledCode(ModuleID, ModuleRecord*, const ModulesOfRealm& inFront = { }, const LevelOfRealm* = nullptr);
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
    void didStartLoad(JSPromise*, ModuleID root);
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
    bool wasImportedWhileItRan(ModuleID file) const;
    bool loaderHasFileThatThrew(ModuleID file);
    bool isAwaitedByImport(ModuleID file);
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
    JSValue exportsSeenByLoader(ModuleID);
    void setCommonJSExports(ModuleID module, JSValue exports) { setValue(module, exports); }
    void mockWhatRequireAnswered(ModuleID, JSObject* exports);
    void mockWhatRequireCacheHolds(ModuleID, JSObject* exports);
    void letGoOfMockInRequireCache(ModuleID);
    void recordRequire(ModuleID, ModuleID referrer);
    bool wasLoadedSynchronously(ModuleID file);
    // `Module._resolveFilename` or `Module.prototype.require` of the realm is the program's. Every image of the realm is told.
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
    enum class TextOfImport : uint8_t { MayComeLater, HasCome, HasComeWithCallInFront };
    JSPromise* startImport(ModuleID, std::optional<int64_t> referrerAsyncOrder, ImportCaller = ImportCaller::CompiledCode, TextOfImport = TextOfImport::MayComeLater, JSPromise* promiseOfCaller = nullptr, JSValue stateOfWait = JSValue());
    JSPromise* rejectImportWithErrorOfLoad(ModuleID, ImportCaller);
    JSPromise* importDynamic(JSValue specifier, JSValue options, ModuleID referrer);
    // `stateOfWait`: as for startImport().
    JSPromise* importUncompiled(const Identifier& key, ModuleID referrer, RefPtr<ScriptFetchParameters>&&, JSValue stateOfWait = { });
    // `bun run` reads the text of a file that is imported on another thread, and hears of it in a later turn of the event loop: every job that waits runs first, and so does what is left of a graph
    // that has all its texts. Where a file that runs while a graph is loaded calls import(), each text comes in a turn of its own from that call until the last text has come, in the order in which they were asked for.
    // So it is where two calls wait at once. A call outside a load that waits alone is made again in a later turn, and settles the promise that the program has.
    bool holdTextOfRequest(JSPromise* fetched, ModuleRecord* requested, ModuleID);
    enum class AskedBy : bool { Loader, Import };
    bool textOfImportComesInLaterTurn(ModuleID, AskedBy);
    void holdTextOfImportFromOtherImage(JSPromise* fetched, AbstractModuleRecord* root, ModuleID);
    std::optional<uint64_t> placeOfFirstTextThatWaits() const { return m_textsThatWait.isEmpty() ? std::nullopt : std::optional(m_textsThatWait[m_numberOfTextsThatHaveCome].place); }
    // False: nobody is left to hear of it.
    bool deliverFirstTextThatWaits(TextOfImport = TextOfImport::HasCome);
    void continueImport(JSPromise* importPromise, ModuleID, std::optional<int64_t> referrerAsyncOrder);
    void resolveWithNamespace(JSPromise* importPromise, ModuleID);
    void finishImportOfEvaluatedModule(JSPromise* importPromise, ModuleID, JSValue error);
    // Says whether the jobs of that import() are counted.
    bool continueCountedImport(JSPromise* importPromise, JSValue module, JSValue referrerAsyncOrder);
    void handCountedImportsToLoader();
    bool comesToCountedImport(ModuleID required);

    enum class RecordHolder : uint8_t { Anyone, Loader };
    ModuleRecord* exposeRecord(ModuleID, RecordHolder = RecordHolder::Anyone);
    void retireRecordsOfSettledLoads();
    AbstractModuleRecord* uncompiledRecord(ModuleID);
    // The record that stands for the module in ResolveExport() and GetExportedNames().
    AbstractModuleRecord* recordForResolution(ModuleID);
    enum class NamespaceUse : uint8_t { IsShownToProgram, CarriesBindings };
    JSObject* uncompiledNamespace(ModuleID, ModulePhase, NamespaceUse, bool shouldPreventExtensions = true);

    void willAnalyzeHeap(VM&);
    void analyzeVariables(ModuleEnvironment*, uint32_t indexAmongModulesWithNamedVariables, HeapAnalyzer&) const;
    std::span<const NamedVariableData> namedVariablesOf(uint32_t indexAmongModulesWithNamedVariables) const;

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
        // `export * from "./leaf.cjs"`: the values of the mock went over names of these, which keep what was replaced.
        Vector<ModuleID> modulesBehindStarsUnderMock;
        bool capturedFieldsAreOfESModule { false };
        bool capturedFieldsAreOfRuntimeModule { false };
        Identifier resolvedKey;
        bool wasLoadedWhileItRan { false };
        bool wasLoadedSynchronouslyWhileItRan { false };
        bool isAnsweredWithNamespace { false };
        bool bindsEarlierCapture { false };
        bool didDropError { false };
        bool exportsWhatMockNamesAlone { false };
        bool takesNamesThatMockAdds { false };
        bool isYetToBeRequiredBehindMock { false };
        bool wasLoadedBehindMock { false };
        bool wasRequiredBehindMock { false };
        // Of a CommonJS file: didLoseErrorOfModule().
        bool wasReachedBySecondLoad { false };
        bool isBeingLoadedByLoader { false };
        BitVector ambiguousExports;
        std::unique_ptr<SourceOrigin> sourceOrigin;
        // A file of another image stands here under the number that the host has for it.
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
    void keepErrorOfLoad(ModuleID root, JSValue error, const ScopedLambda<bool(ModuleID)>& endedLoad);
    bool isFetchedForImportStatement(ModuleID);
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
    void generateNativeModuleForLoader(ModuleID);
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
    void evaluateModulesOwedBelow(ModuleID root, bool hasImporterAboveThatRan = false);
    void evaluateModulesOwedBelowSlow(ModuleID root, bool hasImporterAboveThatRan);
    void bringBackWhatLoaderLetGoBelow(ModuleID root);
    void letGoOfImportersOf(const BitVector& mocked, BitVector&& gone, bool keepsImportersOfTestFile);
    size_t numberOfJobsThatWouldWait(JobsOfImportedFiles) const;
    void holdJobsBehind(size_t numberOfJobs, ModuleID file, JobsOfImportedFiles = JobsOfImportedFiles::WaitForLoader);
    bool hasQueuedTicks() const { return host().hasQueuedTicks && host().hasQueuedTicks(m_instance.globalObject); }
    struct LevelOfLoad;
    struct PlanOfLoad;
    bool planLevelsOfLoad(ModuleID root, RefPtr<PlanOfLoad>* planWithoutWay = nullptr);
    uint32_t countLevelsBelowRoot(ModuleID root, bool& comesToEarlierCall);
    void countLevelsBelowCallThatWaitedAlone();
    void putCallsThatWaitApartInLine();
    bool isLeftAsLoaded(ModuleID) const;
    bool isModuleWithoutRecord(const ModuleData&, StoredModuleStatus) const;
    bool isNeitherOfProgramNorData(const ModuleData&, StoredModuleStatus) const;
    bool isFoundWithoutRecord(ModuleID, const ModuleData&, StoredModuleStatus);
    bool isStillToBeLoaded(ModuleID);
    bool isStillToBeLoaded(ModuleID, const ModuleData&);
    void layWayOfLoad(PlanOfLoad&, LevelOfLoad* above);
    void layWayOfLoadAgain(LevelOfLoad&);
    void countStepsOfLevel(PlanOfLoad&, size_t depth);
    uint32_t countStepsOfWholeGraph(const PlanOfLoad&, std::span<uint32_t> firstRequestOfPlace, std::span<uint32_t> allRequestedPlaces);
    uint32_t takeMarkOfSearch();
    void takeUpWaysOfLoads();
    void markModulesOfPlan(const PlanOfLoad&);
    JSValue callBody(ModuleID);
    JSValue callBodyUnderOwnMark(ModuleID, bool bodyStoresStatus, bool runsImportedFilesAlone = false);
    Vector<ModuleID, 8> modulesThatEvaluationWillBegin(ModuleID root);
    void evaluateByCallingBody(ModuleID module) { callBodyUnderOwnMark(module, true); }
    void didThrowInBody(ModuleID, JSValue error);
    void failEvaluatedModulesOfUnfinishedCycles(std::span<const ModuleID> failedModules, JSValue error);
    void recordEvaluationIfNothingIsLeftToRun(ModuleID);
    bool isLeftToStartsOfImporters(ModuleID) const;
    bool isPassedOverByEvaluationInProgress(ModuleID) const;

    std::optional<ModuleID> mainCommonJSModule();
    void decideMainCommonJSModule(std::optional<ModuleID>);
    std::optional<ModuleID> firstFileThatLoaderFindsBelow(ModuleID root);
    std::optional<std::pair<Modules*, ModuleID>> firstFileOfRealmThatLoaderFindsBelow(ModuleID root);
    void decideMainCommonJSModuleOfRealm(Modules* owner, ModuleID file);
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
    bool getsOriginalBesideMock(ModuleID, ModuleID referrer);
    JSValue answerRequireWithOriginal(ModuleID file);
    JSValue answerRequireWithOriginalSlow(ModuleID file);
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
    bool isAnythingBeingLoaded() const;
    bool isLoaderInMiddleOfGraph() const;
    bool isImportedByModuleThatRequireAnswered(ModuleID) const;
    bool canCountJobsOfImport(ModuleID root);
    bool hasImportedFileLeftToRun(ModuleID root) const;
    std::optional<int64_t> asyncEvaluationOrder(ModuleID) const;
    ModuleRecord* registerWithLoader(ModuleID, RecordHolder = RecordHolder::Anyone);
    const Identifier& resolvedKey(ModuleID);
    // What resolvedKey() has answered. Null where nobody has asked it.
    const Identifier* resolvedKeyIfKnown(ModuleID) const;

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
    // Compared, never followed: its job holds the promise.
    JSPromise* m_importThatWaitsForQueuedJobs { nullptr };
    const EvaluationInProgress* m_innermostEvaluation { nullptr };
    // Each load of evaluateSynchronously() has a number. The innermost one that has not returned is the active one.
    uint32_t m_synchronousLoadCount { 0 };
    uint32_t m_activeSynchronousLoad { 0 };
    Structure* m_namespaceStructure { nullptr };
    Structure* m_environmentStructure { nullptr };
    Vector<ModuleEnvironment*> m_environments;
    // The table namedVariables as rows, made for the first heap snapshot.
    Vector<NamedVariableData> m_namedVariables;
    Vector<uint32_t> m_firstNamedVariables;
    // A heap snapshot keeps pointers to these until it is written.
    Vector<AtomString> m_namesOfVariables;
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
    // The first CommonJS file below each root that import() was called for in that time.
    Vector<ModuleID, 2> m_firstFilesOfLoads;
    BitVector m_modulesSearchedForFirstFile;
    // `bun run` makes the module object of a file for whoever asks for the file first, and only a require() leaves a parent in it.
    BitVector m_filesRequestedByLoader;
    struct FilesBeingRun {
        ModuleID root;
        // Without one, the start of the root calls them.
        std::optional<ModuleID> fileOrFace;
        FilesBeingRun* outer;
        // The loader of `bun run` has asked for every file of the root by now, in jobs that stand in front of whatever the files queue.
        bool isAheadOfJobsOfLoader { false };
        // In which the loader asks the root for its requests. Without one, the jobs of an import() are counted.
        ModuleGraphLoadingState* load { nullptr };
        size_t numberOfJobsInFrontOfFile { 0 };
        // A require() loads the root, and the loader of `bun run` has taken the steps as far as the one in which the file runs.
        bool isAheadOfStepsOfLoader { false };
        // The loader has gone through the graph as far as the step in which this file runs.
        std::optional<ModuleID> fileHandedToLoader { };
        // By a CommonJS file: how many modules stand between the root and the nearest one that imports it. Made when something asks.
        Vector<uint32_t> distanceOfFiles { };
    };
    void handGraphToLoader(FilesBeingRun&, std::optional<ModuleID> finishedFile = std::nullopt);
    void handSynchronousLoadToLoader(FilesBeingRun&);
    void runSynchronousStepsInFrontOf(ModuleID file);
    struct LoadHandedToLoader {
        ModuleID root;
        Strong<JSPromise> promise;
    };
    Vector<LoadHandedToLoader, 1> m_synchronousLoadsHandedToLoader;
    void captureNoExports(ModuleID file);
    static void runJobsThatAreDue(VM&, MicrotaskQueue&);
    void runStepsOfLoaderInFrontOf(ModuleID file);
    void finishStepInWhichFileRan(ModuleID fileOrFace, JSValue error);
    std::optional<ModuleID> m_fileBeingHandedToLoader;
    FilesBeingRun* m_innermostFilesBeingRun { nullptr };
    void noteRequestByLoader(ModuleID fileOrFace);
    bool wasRequestedByLoader(ModuleID file);
    void noteRequireOfFile(ModuleID file);
    bool hasReachedLevelOf(FilesBeingRun&, ModuleID file);
    void findDistanceOfFiles(FilesBeingRun&);
    BitVector m_modulesWithMarkOfRequire;
    BitVector m_modulesWithExtensibleNamespace;
    mutable BitVector m_namespacesThatOnlyImagesAboveHold;
    BitVector m_modulesWithEarlierNamespaceInValue;
    BitVector m_modulesEvaluatedUnderAnotherEvaluation;
    Box<MockedModules> m_mocks;
    unsigned m_numberOfMockedSets { 0 };
    std::optional<ModuleID> m_moduleWithMockBeingLaid;
    OptionSet<RequireOverride> m_storedLoaderFunctions;
    std::optional<UncheckedKeyHashMap<uint32_t, SyntheticExportPlace, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> m_syntheticExportsByVariable;
    UncheckedKeyHashMap<ModuleID, BitVector> m_exportsLeftOut;
    BitVector m_evictedFiles;
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
    ModuleTable::PathsForSystem m_pathsForSystem;
    // Of the realm. A module of this image may import the file.
    bool m_otherImageHasEvictedFiles { false };
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
    // An import() has queued its first job for them, which has not run.
    BitVector m_modulesOfQueuedImports;
    BitVector m_modulesWithMockToRun;
    // The factory of a mock imports the module that it mocks. Such an import comes to the mock behind the end of that run, and shares what became of it: `bun test` calls the factory once.
    BitVector m_modulesWhoseFactoryRuns;
    Vector<ModuleID, 2> m_importsMadeByOwnFactory;
    // AfterFailure::AsksAgain
    BitVector m_modulesWithMockThatAsksAgain;
    // The factory of a mock failed while they were loaded, and they import the mocked module by statements, one through another. The loader of `bun test` keeps the record of each with that error:
    // whoever loads one, or comes to one by a statement, gets the error, until the module is taken out of require.cache or the module below is mocked anew.
    BitVector m_modulesWithLoadEndedByMock;
    // A statement came to them when the factory failed, so the loader keeps their records too: the next statement gets the error, and the factory is not asked. An import() of such a module drops the
    // record and asks the factory; a require() asks the factory and leaves the record.
    BitVector m_mockedModulesWithErrorKeptByLoader;
    BitVector m_modulesWithMockThatWaited;
    // A mock stood in place of the module while its importers ran, and has ended. They read the names of the module itself now, whose body has yet to run.
    BitVector m_modulesOwedToImportersThatRan;
    // evaluateModulesOwedBelow() has been there since the last module came to be owed.
    BitVector m_modulesWithNothingOwedBelow;
    // It passed over these, which were mocked. What it found holds for as long as they are.
    Vector<ModuleID, 4> m_mockedModulesPassedOverByWalks;
    // The host has ended the mock that stands in place of the module. Whoever was linked against the mock goes on reading it, as under `bun test`, until a load comes to the module.
    BitVector m_modulesWithMockToLetGo;
    BitVector m_modulesLoadedBeforeMockThatWaited;
    // A load that comes by brings them back.
    BitVector m_modulesThatLoaderLetGo;
    BitVector m_importersOfMockThatEnded;
    Vector<Vector<uint32_t, 2>> m_importersByStatement;
    Vector<ModuleID> m_libraryModulesInRequireCache;
    BitVector m_libraryModulesTakenFromLoader;
    BitVector m_libraryModulesThatStatementsReached;
    mutable BitVector m_libraryModulesWithCheckedName;
    mutable BitVector m_libraryModulesKeptInRequireCache;
    BitVector m_libraryModulesWithMock;
    BitVector m_libraryModulesThatMockReplaces;
    BitVector m_libraryModulesOwedToImagesAbove;
    BitVector m_libraryModulesLeftByThrow;
    std::optional<ModuleID> m_firstModuleRequestedByHost;
    BitVector m_dataFilesImportedByUncompiledCode;
    BitVector m_dataFilesAskedForByImportCallAlone;
    bool m_otherImageHoldsEntryPointOfRealm { false };
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
    // No body of a mocked module is called but these: answerRequireWithOriginal(). One may load another.
    BitVector m_filesWhoseOriginalRuns;
    bool m_hasAskedEveryClass { false };
    bool m_isWalkingRequests { false };
    struct LoadInFlight {
        Strong<JSPromise> promise;
        ModuleID root;
        bool isWalked { false };
        bool hasMarksBelowRoot { false };
        bool isLoaded { false };
        RefPtr<PlanOfLoad> planWithoutMarks;
    };
    Vector<LoadInFlight, 2> m_loadsInFlight;
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
    struct CountedImport {
        Strong<JSPromise> promise;
        ModuleID module;
        std::optional<int64_t> referrerAsyncOrder;
        uint32_t serial;
        // Of the jobs in which the loader would load the graph.
        uint32_t jobsRun { 0 };
        // Known from the job in which the loader would ask the root for its requests.
        uint32_t jobOfEvaluation { 0 };
        uint32_t jobOfNextNativeModule { 0 };
        RefPtr<PlanOfLoad> plan { };
        // The loader has the graph in its hands.
        RefPtr<MicrotaskQueue> jobsOfLoader { };
        CountedImport* outerInOwnJob { nullptr };
        // All the jobs in which the loader would load the graph have passed.
        bool isLoaded { false };
        // They passed with those of the call in front.
        bool runsInJobOfCallInFront { false };
    };
    void decideHowCountedImportGoesOn(CountedImport&);
    uint32_t findJobOfNextNativeModule(PlanOfLoad&, uint32_t currentJob) const;
    void forgetCountedImport(const CountedImport&);
    Vector<CountedImport, 2> m_countedImports;
    // Out of the list while their own jobs run.
    CountedImport* m_innermostCountedImportInOwnJob { nullptr };
    void forEachCountedImport(const Invocable<void(CountedImport&)> auto&);
    void noteOrderAndModulesOfOnlyCountedImport();
    // That the loader does not have in its hands.
    uint32_t m_numberOfCountedImports { 0 };
    uint32_t m_serialOfNextCountedImport { 0 };
    // A serial for the call of import() and for each job counted since, of those that are still counted. Kept while there are several.
    Vector<uint32_t, 32> m_orderOfCountedJobs;
    // Their roots, and what their plans have found. Kept while there are several.
    BitVector m_modulesOfCountedImports;
    std::optional<ModuleID> m_rootKnownToBeWatched;
    // What the search below that root found when its jobs were counted. Nothing was loaded since.
    PlanOfLoad* m_planOfRootKnownToBeWatched { nullptr };
    std::optional<ModuleID> m_rootWhoseGraphIsLoaded;
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
    UncheckedKeyHashMap<ModuleID, std::unique_ptr<LevelOfRealm>> m_levelsOfRealm;
    struct WayThroughRealm {
        ModuleID root;
        ModulesOfRealm modulesOnWay;
    };
    Vector<WayThroughRealm, 1> m_waysThroughRealm;
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
    void noteFilesOfImagesBelowLoadedWith(ModuleID root);
    // A bit for each module, read for the heads of groups.
    Vector<uint64_t> m_loadedGroups;
    // The groups are those of a program in which the build has seen whoever names a module. Code from outside the image names what it likes: what is loaded from then on has a bit of its own.
    bool m_buildHasSeenWhoNamesModules;
    const MockOperations* m_mockOperations;
    BitVector m_modulesLoadedBesideGroups;
    // VMState::momentOfLoads when the bit of a head, or of a module beside the groups, was set. Empty outside a test run.
    Vector<uint32_t> m_momentsOfLoads;
    BitVector m_loadedInPreload;
    struct MockOverModuleThatHasRun {
        uint32_t momentLaid;
        uint32_t momentOfTestFile;
        bool hadNamespace;
        bool isInPlace { false };
        std::optional<BitVector> exportsLeftOutBefore { };
        // What the object of the module does not show, as it was when the mock came.
        BitVector exportsHiddenBefore { };
        // When this mock was laid. momentLaid: the first of the test file.
        uint32_t momentThisWasLaid { 0 };
    };
    void setAsideNamespaceUnderMock(ModuleID);
    bool m_makesNamespaceOfModuleItself { false };
    std::optional<ModuleID> m_moduleReadPastItsMock;
    UncheckedKeyHashMap<ModuleID, MockOverModuleThatHasRun> m_mocksOverModulesThatHaveRun;
    struct FirstMockInTestFile {
        uint32_t moment;
        uint32_t momentOfTestFile;
    };
    UncheckedKeyHashMap<ModuleID, FirstMockInTestFile> m_firstMocksInTestFile;
    // An importer that the loader had let go, and that somebody has imported since: `bun test` loads it anew and links it to what stands then.
    UncheckedKeyHashMap<ModuleID, FirstMockInTestFile> m_momentsLoadedAnew;
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
    struct TextThatWaits {
        // The turn in which it was asked for, then whether an import() asked, then the order of the calls. There each text comes with a turn to itself, so a module that came ahead of the file
        // that calls import() has asked for what it imports by then. Here the texts of a level come together, and that module asks a few jobs behind the file.
        uint64_t place;
        // Of the program for an import(), of the loader's fetch for a request of a module.
        Strong<JSPromise> promise;
        ModuleID module;
        // Null for an import() of a file of this image.
        Strong<JSCell> requestedRecord { };
        std::optional<int64_t> referrerAsyncOrder { };
        bool waitsApart { false };
        uint32_t levelsAddedToPlace { 0 };
        ImportCaller caller { ImportCaller::CompiledCode };
        // No other call has waited beside it so far.
        bool levelsAreLeftToCount { false };
        // Both wait for one text, which is the last of either: there both graphs run before either call goes on.
        bool comesWithCallInFront { false };
    };
    // Compared, never read.
    JSPromise* m_promiseOfCallThatOthersComeWith { nullptr };
    void deliverCallsThatComeWith(JSPromise* promiseOfCallInFront);
    struct LastTextBelowCall {
        uint32_t turn;
        uint32_t levelsBelowRoot { 0 };
        // For which it is the last text too: its index among the texts that wait.
        std::optional<size_t> earlierCall { };
        uint32_t levelsBelowRootOfEarlierCall { 0 };
    };
    LastTextBelowCall findLastTextBelowCall(ModuleID root);
    template<typename Visitor> void forEachModuleToLoadBelow(ModuleID root, const Visitor&);
    Vector<TextThatWaits> m_textsThatWait;
    size_t m_numberOfTextsThatHaveCome { 0 };
    BitVector m_modulesBelowCallsThatWait;
    std::span<TextThatWaits> textsThatWait() { return m_textsThatWait.mutableSpan().subspan(m_numberOfTextsThatHaveCome); }
    bool isTextReadOnOtherThread(ModuleID file);
    void holdText(JSPromise*, ModuleID, JSCell* requestedRecord, std::optional<int64_t> referrerAsyncOrder, AskedBy, ImportCaller = ImportCaller::CompiledCode);
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
    std::pair<Modules*, ModuleID> ownerOfRequiredModule(ModuleID);
    bool namesFileEvictedFromOtherImage(ModuleID external);
    void leaveToHost(ModuleID file);
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
// The host holds the numbers of modules of every image of a realm.
ModuleID moduleIDForHost(const Image&, ModuleID);
std::optional<ModuleID> moduleOfImage(const Image&, std::optional<ModuleID> fromHost);

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
// no mock waits to run, no file was evicted, no require() of the module is under way. Empty: it has to take the steps.
// A step that is added to answerRequire() and can do something in that state needs its test here.
inline bool Modules::hasMockEndedThatWalksPassedOver() const
{
    for (ModuleID module : m_mockedModulesPassedOverByWalks) {
        if (!isMocked(module) && !mockInPlace(module))
            return true;
    }
    return false;
}

ALWAYS_INLINE JSValue Modules::settledAnswerOfRequire(ModuleID target, RequiredBy requiredBy) const
{
    if (requiredBy != RequiredBy::Referrer || !m_modulesWithMockToRun.isEmpty() || !m_modulesWithMockToLetGo.isEmpty() || (!m_modulesOwedToImportersThatRan.isEmpty() && (!m_modulesWithNothingOwedBelow.get(target.index()) || hasMockEndedThatWalksPassedOver())) || !m_evictedFiles.isEmpty() || m_otherImageHasEvictedFiles)
        return { };
    const LoadedModule* loaded = loadedIfExists(target);
    if (!loaded || !loaded->requiredValue || loaded->isBeingRequired || loaded->objectMadeByRequire || storedStatus(target) != StoredModuleStatus::Evaluated || data(target).isOfLibrary())
        return { };
    // The answer goes by who asks.
    if (loaded->mockInPlace && host().givesOriginalBesideMock) [[unlikely]]
        return { };
    // `bun test` loads it anew: bringBackWhatLoaderLetGoBelow().
    if (m_importersOfMockThatEnded.get(target.index())) [[unlikely]]
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
    uint32_t head = m_table.headOfLoadGroup(module).index();
    if (m_loadedGroups[head / 64] >> (head % 64) & 1)
        return true;
    return !m_buildHasSeenWhoNamesModules && m_modulesLoadedBesideGroups.get(module.index());
}

ALWAYS_INLINE ModuleRecord* Modules::recordIfExists(ModuleID module) const
{
    return storedStatus(module) == StoredModuleStatus::InRecord ? m_records.get(module) : nullptr;
}

// ModuleHost::mockOperations. Each does nothing in a program where no mock was ever laid.
struct MockOperations {
    void (*runMocksBelow)(Modules&, ModuleID root, Modules::RootOfLoad);
    void (*runMocksBelowModuleThatLoaderFetches)(Modules&, ModuleID);
    void (*evaluateModulesOwedBelow)(Modules&, ModuleID root, bool hasImporterAboveThatRan);
    void (*letGoOfMockThatEnded)(Modules&, ModuleID);
    void (*runMockThatWaits)(Modules&, ModuleID);
    std::optional<Modules::AnswerOfMock> (*mockThatAnswersFor)(const Modules&, ModuleID passingOn, const ModuleExportData& passedOn);
    bool (*isMockedInItsImage)(Modules&, ModuleID, Modules::MocksAskedFor);
    JSValue (*answerRequireWithOriginal)(Modules&, ModuleID file);
};

ALWAYS_INLINE void Modules::runMocksBelow(ModuleID root, RootOfLoad rootOfLoad)
{
    if (m_mockOperations)
        m_mockOperations->runMocksBelow(*this, root, rootOfLoad);
}

ALWAYS_INLINE void Modules::runMocksBelowModuleThatLoaderFetches(ModuleID module)
{
    if (m_mockOperations)
        m_mockOperations->runMocksBelowModuleThatLoaderFetches(*this, module);
}

ALWAYS_INLINE void Modules::evaluateModulesOwedBelow(ModuleID root, bool hasImporterAboveThatRan)
{
    if (m_mockOperations)
        m_mockOperations->evaluateModulesOwedBelow(*this, root, hasImporterAboveThatRan);
}

ALWAYS_INLINE void Modules::letGoOfMockThatEnded(ModuleID module)
{
    if (hasMockThatEnded(module)) [[unlikely]]
        m_mockOperations->letGoOfMockThatEnded(*this, module);
}

// Whoever calls it has seen that a mock waits.
ALWAYS_INLINE void Modules::runMockThatWaits(ModuleID module)
{
    m_mockOperations->runMockThatWaits(*this, module);
}

ALWAYS_INLINE auto Modules::mockThatAnswersFor(ModuleID passingOn, const ModuleExportData& passedOn) const -> std::optional<AnswerOfMock>
{
    if (!m_numberOfMocksInPlace) [[likely]]
        return std::nullopt;
    return m_mockOperations->mockThatAnswersFor(*this, passingOn, passedOn);
}

ALWAYS_INLINE bool Modules::isMockedInItsImage(ModuleID module, MocksAskedFor which)
{
    return m_mockOperations && m_mockOperations->isMockedInItsImage(*this, module, which);
}

// Whoever calls it has seen the mock: getsOriginalBesideMock().
ALWAYS_INLINE JSValue Modules::answerRequireWithOriginal(ModuleID file)
{
    return m_mockOperations->answerRequireWithOriginal(*this, file);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
