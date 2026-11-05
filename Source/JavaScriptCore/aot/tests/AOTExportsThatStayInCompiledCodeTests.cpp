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
module m1 path="/parse.cjs" displayPath="parse.cjs" kind=CommonJs flags=[KeepsError] exportRule=CommonJs requireResult=ModuleExports body=f1 exportsVariable=g0
module m2 path="/found.cjs" displayPath="found.cjs" kind=CommonJs flags=[KeepsError, IsFoundByName] exportRule=CommonJs requireResult=ModuleExports body=f3 exportsVariable=g1
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

static constexpr ModuleID fileThatSitesAloneRead { 1u };
static constexpr ModuleID fileFoundByName { 2u };
static constexpr ASCIILiteral refusedInWindowTest = "exports-that-stay-in-compiled-code/the-host-is-refused-the-file-while-the-engine-asks-the-host"_s;
static constexpr ASCIILiteral stopsByNameTest = "exports-that-stay-in-compiled-code/the-module-object-stops-by-name"_s;

static JSValue requiredWhileEngineAsksHost(LoadedProgram& program, ModuleID file, bool& threw)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    Modules& modules = modulesOf(program.instance());
    modules.setRequestToHostAside(true);
    JSValue answer = requireModule(program.realm(), file);
    modules.takeUpRequestToHost();
    threw = scope.exception();
    scope.clearException();
    return answer;
}

static void checkHostIsRefusedFileWhileEngineAsksHost(LoadedProgram& program)
{
    Modules& modules = modulesOf(program.instance());
    AOT_TEST_CHECK(modules.data(fileThatSitesAloneRead).exportsStayInCompiledCode());
    AOT_TEST_CHECK(!modules.data(fileFoundByName).exportsStayInCompiledCode());

    bool threw = false;
    AOT_TEST_CHECK(requiredWhileEngineAsksHost(program, fileFoundByName, threw) == jsNumber(7));
    AOT_TEST_CHECK(!threw);
    AOT_TEST_CHECK(!requiredWhileEngineAsksHost(program, fileThatSitesAloneRead, threw));
    AOT_TEST_CHECK(threw);
}

static void askModuleObjectForExports(LoadedProgram& program)
{
    moduleExports(program.realm(), fileThatSitesAloneRead);
}

static void hostIsRefusedFileWhileEngineAsksHost()
{
    runWithLoadedProgram(refusedInWindowTest, programText, checkHostIsRefusedFileWhileEngineAsksHost, { "exclude=CodeOutsideImage"_s });
}

static void moduleObjectStopsByName()
{
    runWithLoadedProgramThatStops(stopsByNameTest, programText, askModuleObjectForExports, "AOT: the exports of /parse.cjs are read or stored from outside compiled code"_s, { "exclude=CodeOutsideImage"_s });
}

std::span<const NativeTest> exportsThatStayInCompiledCodeTests()
{
    static constexpr NativeTest tests[] = {
        { refusedInWindowTest, hostIsRefusedFileWhileEngineAsksHost },
        { stopsByNameTest, moduleObjectStopsByName },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
