/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
#include "AOTModules.h"
#include "AOTNativeTest.h"
#include "JSCInlines.h"
#include "TopExceptionScope.h"

namespace JSC { namespace AOT {

std::span<const NativeTest> exportsThatStayInCompiledCodeTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 module=m1 fields=[{key="parse" declared={type=anything} flags=[IsRequired]}] ownFieldCount=1
variable g0 name="exports" module=m1 declared={type=anything} initialization=Undefined
variable g1 name="exports" module=m2 declared={type=anything} initialization=Undefined
module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1 kind=Require completes=true}, {target=m2 kind=Require completes=true}]
module m1 path="/parse.cjs" displayPath="parse.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f1 exportsVariable=g0
module m2 path="/found.cjs" displayPath="found.cjs" kind=CommonJs flags=[CachesEvaluationError, IsFoundByName] exportRule=CommonJs requireResult=ModuleExports body=f3 exportsVariable=g1
entry module=m0

function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
    EvaluateModule module=m1 referrer=m0 [isCallOfRequire]
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="parse"
    v3 = Integer integer=1
    v4 = Call v2, v1, v3 calleeKey="parse" calleeName="parse"
    EvaluateModule module=m2 referrer=m0 [isCallOfRequire]
    Return
}
function f1 name="file" module=m1 kind=CommonJsModule results=[{type=anything}] {
    v0 = NewClosure function=f2
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    PutModuleVariable v2 variable=g0
    Return
}
function f2 name="parse" module=m1 length=1 parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Return v0
}
function f3 name="found" module=m2 kind=CommonJsModule results=[{type=anything}] {
    v0 = Integer integer=7
    PutModuleVariable v0 variable=g1
    Return
}
)IR"_s;

static constexpr ASCIILiteral textOfTwoEntries = R"IR(layout L0 module=m0 fields=[{key="count" declared={type=anything} flags=[IsRequired]}] ownFieldCount=1
layout L1 module=m0 fields=[{key="parse" declared={type=anything} flags=[IsRequired]}] ownFieldCount=1
layout L2 module=m1 fields=[{key="count" declared={type=anything} flags=[IsRequired]}] ownFieldCount=1
variable g0 name="exports" module=m0 declared={type=anything} initialization=Undefined
variable g1 name="exports" module=m1 declared={type=anything} initialization=Undefined
module m0 path="/main.cjs" displayPath="main.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f0 exportsVariable=g0
module m1 path="/worker.cjs" displayPath="worker.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f1 exportsVariable=g1
entry module=m0 hostReads=[{exportName="default" builtin=ObjectKeys namesLookedFor=["parse"]}, {exportName="then"}]
entry module=m1 hostReads=[{exportName="default" builtin=ObjectKeys namesLookedFor=["parse"]}, {exportName="then"}]

function f0 name="main" module=m0 kind=CommonJsModule results=[{type=anything}] {
    v0 = Integer integer=1
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    PutModuleVariable v2 variable=g0
    v4 = NewObject -, v0 layout=L1 fields=[0]
    v5 = FinishObject v4
    Return
}
function f1 name="worker" module=m1 kind=CommonJsModule results=[{type=anything}] {
    v0 = Integer integer=2
    v1 = NewObject -, v0 layout=L2 fields=[0]
    v2 = FinishObject v1
    PutModuleVariable v2 variable=g1
    Return
}
)IR"_s;

static constexpr ModuleID mainEntryPoint { 0u };
static constexpr ModuleID otherEntry { 1u };
static constexpr ASCIILiteral startsRealmTest = "exports-that-stay-in-compiled-code/the-host-starts-the-realm-with-an-entry-and-is-refused-the-other"_s;
static constexpr ASCIILiteral preloadsRunFirstTest = "exports-that-stay-in-compiled-code/the-host-runs-preloads-ahead-of-the-entry-that-starts-the-realm"_s;
static constexpr ASCIILiteral requireOfEntryStopsByNameTest = "exports-that-stay-in-compiled-code/a-require-of-the-host-for-the-entry-stops-by-name"_s;
static constexpr ModuleID fileThatSitesAloneRead { 1u };
static constexpr ModuleID fileFoundByName { 2u };
static constexpr ASCIILiteral refusedInWindowTest = "exports-that-stay-in-compiled-code/the-host-is-refused-the-file-while-the-engine-asks-the-host"_s;
static constexpr ASCIILiteral stopsByNameTest = "exports-that-stay-in-compiled-code/the-module-object-stops-by-name"_s;
static constexpr ASCIILiteral storeStopsByNameTest = "exports-that-stay-in-compiled-code/a-store-through-the-module-object-stops-by-name"_s;
static constexpr ASCIILiteral mockStopsByNameTest = "exports-that-stay-in-compiled-code/a-mock-laid-over-the-file-stops-by-name"_s;
static constexpr ASCIILiteral objectStopsByNameTest = "exports-that-stay-in-compiled-code/making-the-module-object-stops-by-name"_s;
static constexpr ASCIILiteral wordsOfStop = "the exports of /parse.cjs were accessed from outside compiled code"_s;

static JSValue requiredWhileEngineAsksHost(LoadedProgram& program, ModuleID file, bool& threw)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    Modules& modules = modulesOf(program.instance());
    modules.pushHostRequest(true);
    JSValue answer = requireModule(program.realm(), file);
    modules.popHostRequest();
    threw = scope.exception();
    scope.clearException();
    return answer;
}

static void checkHostIsRefusedFileWhileEngineAsksHost(LoadedProgram& program)
{
    Modules& modules = modulesOf(program.instance());
    AOT_TEST_CHECK(modules.data(fileThatSitesAloneRead).hasUnescapedExports());
    AOT_TEST_CHECK(!modules.data(fileFoundByName).hasUnescapedExports());

    bool threw = false;
    AOT_TEST_CHECK(requiredWhileEngineAsksHost(program, fileFoundByName, threw) == jsNumber(7));
    AOT_TEST_CHECK(!threw);
    AOT_TEST_CHECK(!requiredWhileEngineAsksHost(program, fileThatSitesAloneRead, threw));
    AOT_TEST_CHECK(threw);
}

static void checkHostStartsRealmAndIsRefusedOtherEntry(LoadedProgram& program)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    Modules& modules = modulesOf(program.instance());
    AOT_TEST_CHECK(modules.data(mainEntryPoint).hasUnescapedExports());
    AOT_TEST_CHECK(modules.data(otherEntry).hasUnescapedExports());

    evaluateModuleSynchronously(program.realm(), mainEntryPoint);
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(moduleNamespace(program.realm(), mainEntryPoint));
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(!moduleNamespace(program.realm(), otherEntry));
    AOT_TEST_CHECK(scope.exception());
    scope.clearException();
}

static void checkHostRunsPreloadAheadOfEntryThatStartsRealm(LoadedProgram& program)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    willRunPreloads(program.realm());
    evaluateModuleSynchronously(program.realm(), otherEntry);
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(moduleNamespace(program.realm(), otherEntry));
    AOT_TEST_CHECK(!scope.exception());
    didRunPreloads(program.realm());

    evaluateModuleSynchronously(program.realm(), mainEntryPoint);
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(moduleNamespace(program.realm(), mainEntryPoint));
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(!moduleNamespace(program.realm(), otherEntry));
    AOT_TEST_CHECK(scope.exception());
    scope.clearException();
}

static void requireEntry(LoadedProgram& program)
{
    requireModule(program.realm(), mainEntryPoint);
}

static void askModuleObjectForExports(LoadedProgram& program)
{
    moduleExports(program.realm(), fileThatSitesAloneRead);
}

static void storeExportsThroughModuleObject(LoadedProgram& program)
{
    setModuleExports(program.realm(), fileThatSitesAloneRead, jsNumber(1));
}

static void layMockOverFile(LoadedProgram& program)
{
    modulesOf(program.instance()).saveExportsBeforePatch(fileThatSitesAloneRead);
}

static void makeModuleObject(LoadedProgram& program)
{
    modulesOf(program.instance()).moduleObject(fileThatSitesAloneRead);
}

static void hostIsRefusedFileWhileEngineAsksHost()
{
    runWithLoadedProgram(refusedInWindowTest, programText, checkHostIsRefusedFileWhileEngineAsksHost, { "exclude=CodeOutsideImage"_s });
}

static void hostStartsRealmAndIsRefusedOtherEntry()
{
    runWithLoadedProgram(startsRealmTest, textOfTwoEntries, checkHostStartsRealmAndIsRefusedOtherEntry, { "exclude=CodeOutsideImage"_s });
}

static void hostRunsPreloadAheadOfEntryThatStartsRealm()
{
    runWithLoadedProgram(preloadsRunFirstTest, textOfTwoEntries, checkHostRunsPreloadAheadOfEntryThatStartsRealm, { "exclude=CodeOutsideImage"_s });
}

static void requireOfEntryStopsByName()
{
    runWithLoadedProgramThatStops(requireOfEntryStopsByNameTest, textOfTwoEntries, requireEntry, "the exports of /main.cjs were accessed from outside compiled code"_s, { "exclude=CodeOutsideImage"_s });
}

static void moduleObjectStopsByName()
{
    runWithLoadedProgramThatStops(stopsByNameTest, programText, askModuleObjectForExports, "the exports of /parse.cjs were accessed from outside compiled code"_s, { "exclude=CodeOutsideImage"_s });
}

static void storeThroughModuleObjectStopsByName()
{
    runWithLoadedProgramThatStops(storeStopsByNameTest, programText, storeExportsThroughModuleObject, wordsOfStop, { "exclude=CodeOutsideImage"_s });
}

static void mockLaidOverFileStopsByName()
{
    runWithLoadedProgramThatStops(mockStopsByNameTest, programText, layMockOverFile, wordsOfStop, { "exclude=CodeOutsideImage"_s });
}

static void makingModuleObjectStopsByName()
{
    runWithLoadedProgramThatStops(objectStopsByNameTest, programText, makeModuleObject, wordsOfStop, { "exclude=CodeOutsideImage"_s });
}

std::span<const NativeTest> exportsThatStayInCompiledCodeTests()
{
    static constexpr NativeTest tests[] = {
        { refusedInWindowTest, hostIsRefusedFileWhileEngineAsksHost },
        { startsRealmTest, hostStartsRealmAndIsRefusedOtherEntry },
        { preloadsRunFirstTest, hostRunsPreloadAheadOfEntryThatStartsRealm },
        { requireOfEntryStopsByNameTest, requireOfEntryStopsByName },
        { stopsByNameTest, moduleObjectStopsByName },
        { storeStopsByNameTest, storeThroughModuleObjectStopsByName },
        { mockStopsByNameTest, mockLaidOverFileStopsByName },
        { objectStopsByNameTest, makingModuleObjectStopsByName },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
