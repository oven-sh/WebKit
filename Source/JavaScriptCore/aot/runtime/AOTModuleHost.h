/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTHostFunctionTexts.h"
#include "AOTVector.h"
#include <JavaScriptCore/AOTRecordTypes.h>
#include <JavaScriptCore/ArgList.h>
#include <JavaScriptCore/Identifier.h>
#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/JSExportMacros.h>
#include <JavaScriptCore/ScriptFetcher.h>
#include <limits>
#include <optional>
#include <span>
#include <wtf/IterationStatus.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/text/StringView.h>

namespace JSC {

class AbstractModuleRecord;
class CallFrame;
class JSCell;
class JSGlobalObject;
class JSObject;
class JSPromise;
class JSString;
class ScriptFetchParameters;
class SourceOrigin;
class VM;

namespace AOT {

struct Instance;
struct MockOperations;

struct ModuleObjects {
    JSObject* module { nullptr };
    JSValue require;
};

enum class ModuleObjectTrait : uint8_t {
    IsMain = 1 << 0,
    HasExportsVariable = 1 << 1,
    IsNoCommonJSFile = 1 << 2,
    IsESModule = 1 << 3,
};

enum class ResolvedSpecifierForm : uint8_t { Path, PathForRequire, URL };
enum class LoadByName : uint8_t { Require, Import };
enum class RequireOverride : uint8_t {
    ResolveFilename = 1 << 0,
    RequireFunction = 1 << 1,
    LoaderOfExtension = 1 << 2,
};

enum class FactoryAskedForBy : uint8_t { ImportCall, ImportCallBehindItsText, LoadThatRequireLeft, Loader, Require, RequireThatIsRefused };

enum class AnswerForName : uint8_t { Declined, Answered, Thrown };

struct ModuleHost {
    void (*createModuleObject)(JSGlobalObject*, JSString* path, ModuleID, OptionSet<ModuleObjectTrait>, ModuleObjects&);
    void (*createRequirer)(JSGlobalObject*, JSObject* importMeta, ModuleID, ModuleObjects&);
    JSValue (*requireSpecifier)(JSGlobalObject*, JSObject* requirer, JSValue specifier);
    JSValue (*resolveSpecifier)(JSGlobalObject*, JSObject* requirerOrImportMeta, JSValue specifier, ResolvedSpecifierForm);
    JSObject* (*captureSyntheticExports)(JSGlobalObject*, SyntheticExportRule, JSValue value, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values);
    JSValue (*requireNativeModule)(JSGlobalObject*, JSString* specifier);
    JSObject* (*captureNativeModuleExports)(JSGlobalObject*, uint32_t registryID, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values);
    JSObject* (*requireCache)(JSGlobalObject*);
    JSObject* (*requireExtensions)(JSGlobalObject*);
    JSObject* (*createAsyncModuleRequiredError)(JSGlobalObject*, const String& path);
    JSValue (*createLoadError)(JSGlobalObject*, const String& path);
    void (*showChildren)(JSGlobalObject*, JSObject* module, JSObject* requiredAgain);
    std::optional<ModuleID> (*findModuleWithAttributes)(JSGlobalObject*, const String& path, const ScriptFetchParameters&);
    JSCell* (*resolveFunctionOf)(JSGlobalObject*, JSObject* requirer, JSCell* heldOrNull);
    bool (*replacesNativeModule)(JSGlobalObject*, const String& path);
    bool (*hasQueuedTicks)(JSGlobalObject*);
    bool (*isImportOfMainModule)(JSGlobalObject*, ModuleID);
    void (*runMainModule)(JSGlobalObject*, ModuleID entryPoint);
    JSValue (*createModuleNotFoundError)(JSGlobalObject*, const String& specifier, const String& importer, bool isForRequire);
    JSObject* (*requirePrototype)(JSGlobalObject*);
    bool (*runMock)(JSGlobalObject*, ModuleID);
    bool (*loadsSynchronously)(JSGlobalObject*, ModuleID);
    bool (*loadsFileItself)(JSGlobalObject*, const String& path);
    bool (*hasPluginsThatAnswerForNames)(JSGlobalObject*);
    AnswerForName (*answersForName)(JSGlobalObject*, const String& name, const String& importer, String& foundWithoutPlugins);
    String (*findsWithoutPlugins)(JSGlobalObject*, const String& name, const String& importer);
    void (*askingAboutNames)(JSGlobalObject*, const ScopedLambda<bool(const String& importer)>* leavesWhereAskingEnds);
    const MockOperations* (*mockOperations)();
    uint64_t (*stampOfFile)(std::span<const char> path);
    bool (*givesOriginalBesideMock)(JSGlobalObject*, const String& path, const String& importer, bool isImport);
    bool (*staysLinkedToFileAheadOfMock)(JSGlobalObject*, const String& path, const String& importer);
    bool (*isLeftToItselfByMock)(JSGlobalObject*, const String& path, const String& requirer);
    JSPromise* (*factoryThatWaits)(JSGlobalObject*, const ScopedLambda<bool(ModuleID)>& isAskedAbout, FactoryAskedForBy, JSValue& state);
    std::optional<bool> (*isInPreload)(JSGlobalObject*);
    bool (*layMocksAgain)(JSGlobalObject*, const ScopedLambda<bool(ModuleID)>& isReached);
    void (*callInLaterTurn)(JSGlobalObject*);
    void (*didStartImport)(JSGlobalObject*, JSPromise*, const String& path, const ScopedLambda<bool(ModuleID)>& isReached);
    bool (*holdsTextsOfProgram)(JSGlobalObject*);
    bool (*isCalledFromFactoryThatWaits)(JSGlobalObject*, ModuleID);
    bool (*keepsTexts)(JSGlobalObject*);
    void (*stopWithReport)(std::span<const char> reason);
    OpenCompressedText openCompressedText;
    uint64_t (*callOfFactoryThatHasNotSettled)(JSGlobalObject*, ModuleID, bool& refusesEveryRequire);
    JSPromise* (*evaluateOriginalOfMock)(JSGlobalObject*, ModuleID, bool& hasFactory);
    bool (*loadsBelowVitest)(JSGlobalObject*);
    void (*dropKeptText)(JSGlobalObject*, const String& path);
    bool (*printsDataFileAsJavaScript)(JSGlobalObject*, const String& path);
    void (*didForgetRunOfModuleObject)(JSGlobalObject*, JSObject* module, bool nextRunBegins);
    bool (*isPatchedOnceFactorySettles)(JSGlobalObject*, const String& path);
    uint32_t (*willLoad)(JSGlobalObject*, ModuleID fileThatWroteItIfKnown, ModuleID asked, LoadByName);
};

const ModuleHost& moduleHostOf(JSGlobalObject*);

JS_EXPORT_PRIVATE const MockOperations* mockOperations();

JS_EXPORT_PRIVATE std::optional<ModuleID> findModule(StringView publicPath, unsigned hashOfPath);
inline std::optional<ModuleID> findModule(StringView publicPath) { return findModule(publicPath, publicPath.hash()); }
bool isOriginOfCompiledFile(const Instance&, const SourceOrigin&);
JS_EXPORT_PRIVATE bool holdsFormThatAttributeAsksFor(StringView publicPath);
JS_EXPORT_PRIVATE bool mayHoldPathStartingWith(StringView start);
JS_EXPORT_PRIVATE bool isLeftToCodeOutsideImage(StringView specifier, StringView directoryOfImporter);
JS_EXPORT_PRIVATE String pathBesideFile(StringView file, StringView written);
JS_EXPORT_PRIVATE String modulePath(ModuleID);
String pathOfFileOfModule(ModuleID);
String sourceProviderURL(ModuleID);
JS_EXPORT_PRIVATE bool entryPointIsCommonJS(JSGlobalObject*);
JS_EXPORT_PRIVATE bool isESModule(ModuleID);
JS_EXPORT_PRIVATE bool isESModuleThatWaitsForNothing(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isDataFile(ModuleID);
std::optional<bool> findsBuiltinsReadOnly(ModuleID);
class FetcherOfTextHandedIn : public ScriptFetcher {
public:
    bool isMadeByFileOfProduct() const { return m_isMadeByFileOfProduct; }

protected:
    JS_EXPORT_PRIVATE explicit FetcherOfTextHandedIn(VM&);

private:
    bool m_isMadeByFileOfProduct;
};
enum class OtherNameOfModule : uint8_t { WithoutExtension, UnderAnotherExtension, AsFolder };
JS_EXPORT_PRIVATE bool isReachedUnder(ModuleID, OtherNameOfModule);
JS_EXPORT_PRIVATE bool isHeldForImport(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isNativeModule(ModuleID);
JS_EXPORT_PRIVATE bool isOfLibrary(ModuleID);
JS_EXPORT_PRIVATE bool mayHoldEntryOfProgramInRequireCache(ModuleID);
JS_EXPORT_PRIVATE bool hostAnswersInPlaceOf(JSGlobalObject*, ModuleID, LoadByName);
JS_EXPORT_PRIVATE bool isInRequireCache(JSGlobalObject*, ModuleID);
enum class StartedBy : uint8_t { Code, Host };
JS_EXPORT_PRIVATE bool leavesModuleToHost(JSGlobalObject*, ModuleID, StartedBy = StartedBy::Code);
JS_EXPORT_PRIVATE void willLoadCopyFromDisk(JSGlobalObject*, ModuleID, LoadByName);
JS_EXPORT_PRIVATE void forEachFirstLoad(VM&, const ScopedLambda<void(const String& path, bool wasSynchronous)>&);
JS_EXPORT_PRIVATE void noteFirstLoadInEarlierProcess(VM&, const String& path, bool wasSynchronous);
JS_EXPORT_PRIVATE void forEachModule(const ScopedLambda<IterationStatus(ModuleID)>&);
JS_EXPORT_PRIVATE void forEachCountedFile(VM&, const ScopedLambda<void(const String& description, std::span<const uint64_t> counts)>&);
JS_EXPORT_PRIVATE void forEachFilePathStartingWith(StringView start, const ScopedLambda<IterationStatus(const String&)>&);
JS_EXPORT_PRIVATE bool refusesCodeOutsideImage(JSGlobalObject*, StringView path);
JS_EXPORT_PRIVATE bool isChangedOutOfSightOfBuild(JSGlobalObject*, StringView path);
JS_EXPORT_PRIVATE bool willRunCopyFromDisk(JSGlobalObject*);
JS_EXPORT_PRIVATE bool refusesHostObjectsOutsideImage(JSGlobalObject*);

JS_EXPORT_PRIVATE JSValue moduleExports(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void setModuleExports(JSGlobalObject*, ModuleID, JSValue);
JS_EXPORT_PRIVATE JSObject* mainModuleObject(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willStorePathOfMainModule(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* parentModuleObject(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool mayHandOutModuleObject(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool forEachNewChild(JSGlobalObject*, ModuleID, const ScopedLambda<void(JSObject*)>&);
JS_EXPORT_PRIVATE void dropNewChildren(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willLoadByName(JSGlobalObject*, JSValue specifier, LoadByName = LoadByName::Require);
JS_EXPORT_PRIVATE bool isResolvingImportCall(VM&);
JS_EXPORT_PRIVATE bool isResolvingImportCallWithoutAttributes(VM&);
JS_EXPORT_PRIVATE bool requireRunsMocks(VM&);
JS_EXPORT_PRIVATE bool requireAsksFor(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE String pathOfModuleThatRequires(JSGlobalObject*, ModuleID required);
JS_EXPORT_PRIVATE bool importAsksFor(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE std::optional<ModuleID> loadThatAsks(JSGlobalObject*);
struct ImportThatAsks {
    ModuleID fileThatWroteIt;
    ModuleID fileAskedFor;
    uint32_t momentOfCall { 0 };
};
JS_EXPORT_PRIVATE std::optional<ImportThatAsks> importThatAsks(JSGlobalObject*);
JS_EXPORT_PRIVATE bool callOfImportSeesMockOf(JSGlobalObject*, ModuleID mocked, ModuleID importer);
JS_EXPORT_PRIVATE void letLoadsInFlightKeepRecordOf(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isKeptByLoadsInFlight(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool loadLeadsTo(JSGlobalObject*, ModuleID root, ModuleID);
enum class LoadOf : bool { WhatStandsForModule, Original };
JS_EXPORT_PRIVATE BitVector modulesThatLoadLeadsTo(JSGlobalObject*, ModuleID root, LoadOf = LoadOf::WhatStandsForModule);
JS_EXPORT_PRIVATE bool importThatWaitedAsksFor(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSValue stateOfWaitOfImportThatAsks(JSGlobalObject*);
using MissingExportListener = void (*)(const String& name, const String& exporter, const String& importer);
JS_EXPORT_PRIVATE void setMissingExportListener(MissingExportListener);
JS_EXPORT_PRIVATE void willAnswerWithModule(JSGlobalObject*, ModuleID, LoadByName = LoadByName::Require);
class Modules;
class AskerOfModule {
    WTF_MAKE_NONCOPYABLE(AskerOfModule);
public:
    JS_EXPORT_PRIVATE AskerOfModule(JSGlobalObject*, const String& path);
    JS_EXPORT_PRIVATE ~AskerOfModule();

private:
    Modules* m_modules;
    const String* m_outer;
};
JS_EXPORT_PRIVATE const String& askerThatIsProgram();
JS_EXPORT_PRIVATE bool isAskedByCodeOutsideImage(JSGlobalObject*);
JS_EXPORT_PRIVATE void throwFileOfImageRefused(JSGlobalObject*, ModuleID, LoadByName);

JS_EXPORT_PRIVATE AbstractModuleRecord* moduleRecord(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isCompiledModuleRecord(AbstractModuleRecord*);
JS_EXPORT_PRIVATE std::optional<ModuleID> fileOfCompiledModuleRecord(JSGlobalObject*, AbstractModuleRecord*);
JS_EXPORT_PRIVATE AbstractModuleRecord* moduleRecordOfNamespace(JSGlobalObject*, JSCell* moduleNamespace);
JS_EXPORT_PRIVATE JSPromise* importModule(JSGlobalObject*, ModuleID, std::optional<int64_t> referrerAsyncOrder);
JS_EXPORT_PRIVATE void evaluateModuleSynchronously(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* moduleNamespace(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool mayExport(JSGlobalObject*, ModuleID, UniquedStringImpl* name);
JS_EXPORT_PRIVATE JSValue requireModule(JSGlobalObject*, ModuleID, std::optional<ModuleID> referrer = std::nullopt);
JS_EXPORT_PRIVATE bool refusesToRunFileForDefaultLoader(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSValue runFileForDefaultLoader(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void throwTextRefusedToCompileOfProgram(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didMakeModuleObjectForRequire(JSGlobalObject*, ModuleID, JSObject* moduleObject);
JS_EXPORT_PRIVATE std::optional<OptionSet<ModuleObjectTrait>> traitsOfModuleObjectYetToBeMade(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void takeModuleObject(JSGlobalObject*, ModuleID, const ModuleObjects&);
JS_EXPORT_PRIVATE void giveBackModuleObject(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* moduleObjectOverExportsVariableIfMade(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didLayExportsVariableOnModuleObject(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willRequireModuleFromOutsideImage(JSGlobalObject*, ModuleID, JSObject* moduleObjectOfRequirer);

JS_EXPORT_PRIVATE void willSetRequireCacheEntry(JSGlobalObject*, ModuleID, JSValue entry);
JS_EXPORT_PRIVATE void willSetRequireCacheEntryUnderName(JSGlobalObject*, ModuleID, const String& name);
JS_EXPORT_PRIVATE void didDeleteRequireCacheEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didDeleteEntryOfMockOverFileSetAside(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didDeleteEntryUnderMockThatWaits(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willRunPreloads(JSGlobalObject*);
JS_EXPORT_PRIVATE void didRunPreloads(JSGlobalObject*);
JS_EXPORT_PRIVATE void willRunCodeThatKeepsFiles(JSGlobalObject*);
JS_EXPORT_PRIVATE void keepFilesThatHaveRun(JSGlobalObject*);
JS_EXPORT_PRIVATE void didResetModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isLoadInFlight(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool haveLoadsInFlightTakenAnswerThatStandsOn(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void takeOutFilesThatFailed(JSGlobalObject*);
JS_EXPORT_PRIVATE void bringBackKeptFiles(JSGlobalObject*, const WTF::Function<void(const String&)>&);
JS_EXPORT_PRIVATE bool isEvictedFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didKeepTextInHost(VM&, ModuleID, bool isOfPlugin);
JS_EXPORT_PRIVATE bool refusesToLoadAgain(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willClearRequireCacheEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willReplaceRequireCacheEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool willLoadFileInAnotherWay(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willRunEntryPointAgain(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isEvaluatedFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasMockThatWaitsOrRuns(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE JSObject* requireCacheEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void forEachEvaluatedFile(JSGlobalObject*, const WTF::Function<void(const String&)>&);
JS_EXPORT_PRIVATE void forEachFileWhoseEvaluationAwaits(JSGlobalObject*, const WTF::Function<void(const String&)>&);
JS_EXPORT_PRIVATE void forEachFileBehindEntryOfProgram(JSGlobalObject*, const WTF::Function<void(const String&)>&);
JS_EXPORT_PRIVATE void forEachFileAwaitingAtTopLevel(JSGlobalObject*, const WTF::Function<void(int64_t turn, const String& key)>&);
JS_EXPORT_PRIVATE void forEachLibraryModuleInRequireCache(JSGlobalObject*, const WTF::Function<void(const String&)>&);
JS_EXPORT_PRIVATE std::optional<ModuleID> findLibraryModuleInRequireCache(JSGlobalObject*, const String& name);
JS_EXPORT_PRIVATE void willChangeRequireOverride(JSGlobalObject*, RequireOverride, bool isOverridden);
JS_EXPORT_PRIVATE void willStoreLoaderOfExtension(JSGlobalObject*, StringView extension);

JS_EXPORT_PRIVATE bool isModuleNamespace(JSCell*);
JS_EXPORT_PRIVATE bool tryReplaceExport(JSCell* moduleNamespace, UniquedStringImpl* name, JSValue);
JS_EXPORT_PRIVATE String refusedSpyOnExportMessage(JSCell* moduleNamespace, UniquedStringImpl* name, const String& callerOutsideBuild);
JS_EXPORT_PRIVATE bool mayBeMocked(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE String refusedMockMessage(JSGlobalObject*, const String& path, const String& callerOutsideBuild);
JS_EXPORT_PRIVATE bool hasBegunToLoad(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasOnlyBeenLinked(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasImportBegunToLoad(JSGlobalObject*, ModuleID fileOrFace);
JS_EXPORT_PRIVATE bool wasLoadedByImport(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void forEachModuleWhoseBodyRuns(VM&, CallFrame*, const ScopedLambda<void(ModuleID)>&);
JS_EXPORT_PRIVATE bool isBeingEvaluated(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasPulledInByLoadThatFailed(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isRootOfImportThatFailed(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasLoadedAheadOfThisTestFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didLeaveLoaderInThisTestFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void noteThatKeyIsOfThisTestFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void noteThatMockHasEmptiedKey(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isKeyOfThisTestFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE std::optional<ModuleID> formThatMockFinds(JSGlobalObject*, StringView path, std::optional<ModuleID> file);
JS_EXPORT_PRIVATE Vector<ModuleID, 2> formsThatAttributesAskFor(JSGlobalObject*, StringView path);
enum class AfterFailure : bool { KeepsError, AsksAgain };
JS_EXPORT_PRIVATE void deferMock(JSGlobalObject*, ModuleID, AfterFailure = AfterFailure::KeepsError);
JS_EXPORT_PRIVATE void didFailFactoryThatTakesOriginal(JSGlobalObject*, ModuleID, JSValue error);
JS_EXPORT_PRIVATE bool isSomeImportHeldThatAsksForMockOfItsFile(JSGlobalObject*);
JS_EXPORT_PRIVATE bool isImportHeldThatAsksForMockOf(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasLoadInFlightAskedForOriginalOf(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didFinishTestsOfFile(JSGlobalObject*);
JS_EXPORT_PRIVATE void letMockWaitBehindLateFailure(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void dropErrorsOfLoadsEndedByMocks(JSGlobalObject*);
JS_EXPORT_PRIVATE void letLoadsThatRequiresLeftKeepNoError(JSGlobalObject*);
enum class OriginalBehindEntryOfProgram : uint8_t { GoesWithPatch, Stays };
JS_EXPORT_PRIVATE void didEndMock(JSGlobalObject*, ModuleID, OriginalBehindEntryOfProgram = OriginalBehindEntryOfProgram::GoesWithPatch, bool wasLaidOverMockOfPreload = true, bool bringsBackWhatWasSetAside = true);
JS_EXPORT_PRIVATE void letMockStandForLoadsInFlight(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool standsForLoadsInFlight(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isImportHeld(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void endMocksThatStoodForLoadsInFlight(JSGlobalObject*);
JS_EXPORT_PRIVATE bool importsByStatements(JSGlobalObject*, ModuleID importer, ModuleID imported);
JS_EXPORT_PRIVATE void layMocksAgainAtNextLoad(JSGlobalObject*, ModuleID, bool waits = true);
JS_EXPORT_PRIVATE bool runBodyOfLinkedModuleAheadOfMock(JSGlobalObject*, ModuleID, bool displacesEarlierMock, bool setsModuleAside, bool leavesModuleAlone);
JS_EXPORT_PRIVATE JSPromise* evaluationUnderWay(JSGlobalObject*, ModuleID, bool hasWaitedForTurn);
JS_EXPORT_PRIVATE JSPromise* evaluationUnderWayBelow(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void countMockCall(JSGlobalObject*, std::optional<ModuleID>, ASCIILiteral otherwise, ASCIILiteral call, ASCIILiteral factory);
JS_EXPORT_PRIVATE void runBodyOfRequiredFileAheadOfMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool runBodyOfImportedModuleAheadOfMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* packageItselfBesideMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSPromise* factoryThatWaitsBelow(JSGlobalObject*, ModuleID, JSValue& state);
JS_EXPORT_PRIVATE bool mockFailedBehindImportStatement(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willFetchBehindTurn(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void fetchDidGoOnBehindFactory(JSGlobalObject*);
JS_EXPORT_PRIVATE bool wasLetGoByLoader(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool loadComesToModuleWhoseMockEnded(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void letGoOfMocksThatEnded(JSGlobalObject*);
JS_EXPORT_PRIVATE void deliverTextThatWaits(VM&);
JS_EXPORT_PRIVATE void didRunJobsBehindDelivery(VM&);
JS_EXPORT_PRIVATE bool textOfFetchedModuleComesInLaterTurn(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void holdTextOfFetchedModule(JSGlobalObject*, ModuleID, JSPromise* fetched, AbstractModuleRecord*);
JS_EXPORT_PRIVATE JSValue exportsBeforeMock(JSGlobalObject*, ModuleID, bool evenIfItsBodyThrew, bool* readOfNameThrew = nullptr);
JS_EXPORT_PRIVATE JSValue exportsBehindEntryOfProgram(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE JSValue askForExportsBehindEntryOfProgram(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void keepOriginalOfFileThatLeavesProgram(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void parkRunUnderKeyOfOriginal(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void takeBackRunUnderKeyOfOriginal(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE JSObject* moduleObjectBehindEntryOfProgram(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void forgetOriginalOfFileThatLeftProgram(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE bool liesUnderKeyOfOriginal(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void setLiesUnderKeyOfOriginal(JSGlobalObject*, ModuleID file, bool);
JS_EXPORT_PRIVATE bool didOriginalComeBackUnderOwnKey(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void setOriginalCameBackUnderOwnKey(JSGlobalObject*, ModuleID file, bool);
JS_EXPORT_PRIVATE JSValue exportsThatMockWasLaidOver(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE bool hasMockLaidOver(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasMockLaidOverFileThatHadRun(JSGlobalObject*, ModuleID);
enum class LoadedBy : uint8_t { Nobody, Preload, EarlierTestFile, ThisTestFile };
JS_EXPORT_PRIVATE LoadedBy whoLoaded(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE LoadedBy whoFetched(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didEmptyKeyOfMockInPlace(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE LoadedBy whoLoadedMockInPlaceAnew(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isKeyOfMockInPlaceEmpty(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* namespaceOfFileThatRanAheadOfMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSPromise* evaluationOfModuleThatMockLeftAlone(JSGlobalObject*, ModuleID, std::optional<ModuleID> loadThatCalledFactory);
JS_EXPORT_PRIVATE void didCallImportOfHost(JSGlobalObject*, ModuleID imported, JSPromise*);
JS_EXPORT_PRIVATE JSValue errorThrownByBodyOfMockedModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool runMocksBelow(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didChangePlugins(JSGlobalObject*);
JS_EXPORT_PRIVATE bool setMock(JSGlobalObject*, ModuleID, JSObject* mock, JSValue whatRequireAnswers, bool leavesEarlierMockToItsImporters = false);
JS_EXPORT_PRIVATE bool willLeaveModuleAlone(JSGlobalObject*, ModuleID, bool onlyIfLoadedInPreload, std::optional<ModuleID> asking, bool evenIfItsBodyThrew = false);
JS_EXPORT_PRIVATE bool isLeftAloneByMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didSetAsideModuleLeftAlone(JSGlobalObject*, ModuleID, bool whoeverLoadedIt);
JS_EXPORT_PRIVATE bool isSetAsideByMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE AbstractModuleRecord* recordOfModuleItself(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didBeginTestFile(JSGlobalObject*);
JS_EXPORT_PRIVATE bool hasMockInPlace(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isStandingMockImportedByModuleYetToRun(JSGlobalObject*, ModuleID, std::optional<ModuleID> asking);
JS_EXPORT_PRIVATE bool willLeaveStandingMockAlone(JSGlobalObject*, ModuleID, bool staysTheOriginal, bool keepsWhatThisTestFileLoaded);
struct LoadedUnderStandingMock {
    JSObject* namespaceObject { nullptr };
    JSValue requiredValue;
};
JS_EXPORT_PRIVATE LoadedUnderStandingMock loadedUnderStandingMockThatImportersKeep(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasRequiredUnderMockInPlace(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasRequiredUnderMockLaidInPreload(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE JSObject* mockInPlaceBehind(JSCell* moduleNamespace);
JS_EXPORT_PRIVATE bool hasWordOfMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willMockModule(JSGlobalObject*, ModuleID, JSValue whatRequireAnswers, bool exportsAreOfCommonJSModule = false);
JS_EXPORT_PRIVATE void didMockModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didStopLayingMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void mockExport(JSGlobalObject*, ModuleID, UniquedStringImpl* name, JSValue);
JS_EXPORT_PRIVATE bool hasESModuleMarker(JSCell* moduleNamespace);
JS_EXPORT_PRIVATE void setESModuleMarker(JSCell* moduleNamespace, bool);

JS_EXPORT_PRIVATE void setVerbose(bool);

struct AddressSpaceToStart {
    size_t needed { 0 };
    size_t left { 0 };
    size_t largestStepOfAllocator { std::numeric_limits<size_t>::max() };
    bool isRefused { false };
};
JS_EXPORT_PRIVATE AddressSpaceToStart askForAddressSpaceToStart(size_t sizeOfProgram, std::span<const size_t> stepsOfAllocatorFromLargest);

struct PagesForNewThread {
    void* pages { nullptr };
    bool isRefused { false };
};
JS_EXPORT_PRIVATE PagesForNewThread tryReservePagesForNewThread(bool threadsAreEnding);
JS_EXPORT_PRIVATE PagesForNewThread tryReservePagesForFirstThread();
JS_EXPORT_PRIVATE void releasePagesForNewThread(void* pages);
JS_EXPORT_PRIVATE void adoptPagesForNewThread(void* pages);
JS_EXPORT_PRIVATE void releaseUnusedPagesOfThread();
JS_EXPORT_PRIVATE void didStartThread(VM*);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
