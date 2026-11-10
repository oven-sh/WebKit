/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
#include "AOTNativeTest.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"

namespace JSC { namespace AOT {

std::span<const NativeTest> originalBesideMockTests();

static constexpr ASCIILiteral programText = R"IR(variable g0 name="exports" module=m1 declared={type=anything} initialization=Undefined
variable g1 module=m1 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/node_modules/dep/index.js" displayPath="dep.js" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f1 exportsVariable=g0
module m2 path="/project.cjs" displayPath="project.cjs" kind=CommonJs flags=[CachesEvaluationError] requireResult=ModuleExports body=f2
entry module=m0
entry module=m1
entry module=m2

function f0 name="main" kind=Module results=[{type=anything}] {
    Return
}
function f1 name="dep" module=m1 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="ranOnce"
    PropSet v0, v1 key="ranTwice"
    v3 = Integer integer=1
    PropSet v0, v3 key="ranOnce"
    PutModuleVariable v3 variable=g0
    Return
}
function f2 name="project" module=m2 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
)IR"_s;

static constexpr ModuleID dep { 1u };
static constexpr ModuleID project { 2u };

static JSValue globalNamed(LoadedProgram& program, ASCIILiteral name)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSValue value = program.realm()->get(program.realm(), Identifier::fromString(program.vm(), name));
    AOT_TEST_CHECK(!scope.exception());
    return value;
}

static JSValue required(LoadedProgram& program, ModuleID referrer)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSValue answer = requireModule(program.realm(), dep, referrer);
    if (Exception* thrown = scope.exception()) {
        scope.clearException();
        dataLogLn("require() threw: ", thrown->value().toWTFString(program.realm()));
        AOT_TEST_CHECK(false);
    }
    return answer;
}

static void checkMockThatNoPackageAskedBehindLeavesBodyToRun(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());

    AOT_TEST_CHECK(setMock(realm, dep, constructEmptyObject(realm), jsNumber(2)));
    AOT_TEST_CHECK(required(program, project) == jsNumber(2));
    AOT_TEST_CHECK(setMock(realm, dep, nullptr, { }));
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(globalNamed(program, "ranOnce"_s).isUndefined());
    AOT_TEST_CHECK(required(program, project) == jsNumber(1));
    AOT_TEST_CHECK(globalNamed(program, "ranOnce"_s) == jsNumber(1));
    AOT_TEST_CHECK(globalNamed(program, "ranTwice"_s).isUndefined());
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<originalBesideMockTests, programText, body>;

std::span<const NativeTest> originalBesideMockTests()
{
    static constexpr NativeTest tests[] = {
        { "original-beside-mock/mock-that-no-package-asked-behind-leaves-body-to-run"_s, withProgram<checkMockThatNoPackageAskedBehindLeavesBodyToRun> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
