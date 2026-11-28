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
#include "CustomGetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"

namespace JSC { namespace AOT {

std::span<const NativeTest> testRunOriginalsTests();

static constexpr ASCIILiteral programText = R"IR(variable g0 name="exports" module=m1 declared={type=anything} initialization=Undefined
variable g1 module=m1 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g2 name="exports" module=m4 declared={type=anything} initialization=Undefined
variable g3 module=m4 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/node_modules/dep/index.js" displayPath="dep.js" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f1 exportsVariable=g0
module m2 path="/node_modules/reads/index.js" displayPath="reads.js" kind=CommonJs flags=[CachesEvaluationError] requireResult=ModuleExports body=f2
module m3 path="/project.cjs" displayPath="project.cjs" kind=CommonJs flags=[CachesEvaluationError] requireResult=ModuleExports body=f3
module m4 path="/node_modules/other/index.js" displayPath="other.js" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f4 exportsVariable=g2
entry module=m0
entry module=m1
entry module=m2
entry module=m3
entry module=m4

function f0 name="main" kind=Module results=[{type=anything}] {
    Return
}
function f1 name="dep" module=m1 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="hook"
    v2 = PropGet v0 key="ranOnce"
    PropSet v0, v2 key="ranTwice"
    v4 = Integer integer=1
    PropSet v0, v4 key="ranOnce"
    PutModuleVariable v4 variable=g0
    Return
}
function f2 name="reads" module=m2 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f3 name="project" module=m3 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f4 name="other" module=m4 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="hook"
    v2 = Integer integer=5
    PutModuleVariable v2 variable=g2
    Return
}
)IR"_s;

static constexpr ModuleID dep { 1u };
static constexpr ModuleID package { 2u };
static constexpr ModuleID project { 3u };
static constexpr ModuleID other { 4u };

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

static void checkPackageGetsOriginalAndBodyRunsOnce(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSValue answerOfMock = jsNumber(2);
    JSValue original = jsNumber(1);

    AOT_TEST_CHECK(setMock(realm, dep, constructEmptyObject(realm), answerOfMock));
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(required(program, project) == answerOfMock);
    AOT_TEST_CHECK(required(program, project) == answerOfMock);
    AOT_TEST_CHECK(globalNamed(program, "ranOnce"_s).isUndefined());

    AOT_TEST_CHECK(required(program, package) == original);
    AOT_TEST_CHECK(globalNamed(program, "ranOnce"_s) == jsNumber(1));
    AOT_TEST_CHECK(required(program, project) == answerOfMock);
    AOT_TEST_CHECK(required(program, package) == original);

    AOT_TEST_CHECK(setMock(realm, dep, nullptr, { }));
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(required(program, project) == original);
    AOT_TEST_CHECK(required(program, package) == original);
    AOT_TEST_CHECK(globalNamed(program, "ranTwice"_s).isUndefined());
}

static bool hookThrows;
static void (*hookDoes)(JSGlobalObject*);
static EncodedJSValue seenByProject;
static EncodedJSValue seenByPackage;
static JSC_DECLARE_CUSTOM_GETTER(hookOfBody);

JSC_DEFINE_CUSTOM_GETTER(hookOfBody, (JSGlobalObject* realm, EncodedJSValue, PropertyName))
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    if (hookThrows)
        throwTypeError(realm, scope, "thrown by the body"_s);
    else if (auto does = std::exchange(hookDoes, nullptr))
        does(realm);
    return JSValue::encode(jsUndefined());
}

static void installHook(LoadedProgram& program)
{
    VM& vm = program.vm();
    program.realm()->putDirectCustomAccessor(vm, Identifier::fromString(vm, "hook"_s), CustomGetterSetter::create(vm, hookOfBody, nullptr), static_cast<unsigned>(PropertyAttribute::CustomAccessor));
}

static void checkOriginalThatThrowsLeavesMockAndRunsAgain(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    installHook(program);

    AOT_TEST_CHECK(setMock(realm, dep, constructEmptyObject(realm), jsNumber(2)));
    AOT_TEST_CHECK(required(program, project) == jsNumber(2));
    hookThrows = true;
    requireModule(realm, dep, package);
    hookThrows = false;
    AOT_TEST_CHECK(scope.exception());
    scope.clearException();
    AOT_TEST_CHECK(globalNamed(program, "ranOnce"_s).isUndefined());
    AOT_TEST_CHECK(hasActiveMock(realm, dep));
    AOT_TEST_CHECK(required(program, project) == jsNumber(2));

    AOT_TEST_CHECK(required(program, package) == jsNumber(1));
    AOT_TEST_CHECK(required(program, project) == jsNumber(2));
    AOT_TEST_CHECK(globalNamed(program, "ranTwice"_s).isUndefined());
}

static void checkProjectThatAsksWhileOriginalRunsGetsMock(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    installHook(program);

    AOT_TEST_CHECK(setMock(realm, dep, constructEmptyObject(realm), jsNumber(2)));
    hookDoes = [](JSGlobalObject* realm) {
        seenByProject = JSValue::encode(requireModule(realm, dep, project));
        seenByPackage = JSValue::encode(requireModule(realm, dep, package));
    };
    AOT_TEST_CHECK(required(program, package) == jsNumber(1));
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(JSValue::decode(seenByProject) == jsNumber(2));
    AOT_TEST_CHECK(JSValue::decode(seenByPackage).isUndefined());
    AOT_TEST_CHECK(required(program, project) == jsNumber(2));
    AOT_TEST_CHECK(required(program, package) == jsNumber(1));
    AOT_TEST_CHECK(globalNamed(program, "ranTwice"_s).isUndefined());
}

static void checkOriginalThatLoadsAnotherOriginalKeepsItsMock(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    installHook(program);

    AOT_TEST_CHECK(setMock(realm, dep, constructEmptyObject(realm), jsNumber(2)));
    AOT_TEST_CHECK(setMock(realm, other, constructEmptyObject(realm), jsNumber(4)));
    hookDoes = [](JSGlobalObject* realm) {
        hookDoes = [](JSGlobalObject* realm) {
            seenByProject = JSValue::encode(requireModule(realm, dep, project));
        };
        seenByPackage = JSValue::encode(requireModule(realm, other, package));
    };
    AOT_TEST_CHECK(required(program, package) == jsNumber(1));
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(JSValue::decode(seenByProject) == jsNumber(2));
    AOT_TEST_CHECK(JSValue::decode(seenByPackage) == jsNumber(5));
    AOT_TEST_CHECK(requireModule(realm, other, project) == jsNumber(4));
    AOT_TEST_CHECK(required(program, project) == jsNumber(2));
    AOT_TEST_CHECK(globalNamed(program, "ranTwice"_s).isUndefined());
}

static void checkMockThatEndsWhileOriginalRunsLeavesOriginal(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    installHook(program);

    AOT_TEST_CHECK(setMock(realm, dep, constructEmptyObject(realm), jsNumber(2)));
    hookDoes = [](JSGlobalObject* realm) {
        setMock(realm, dep, nullptr, { });
    };
    AOT_TEST_CHECK(required(program, package) == jsNumber(1));
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(!hasActiveMock(realm, dep));
    AOT_TEST_CHECK(required(program, project) == jsNumber(1));
    AOT_TEST_CHECK(required(program, package) == jsNumber(1));
    AOT_TEST_CHECK(globalNamed(program, "ranTwice"_s).isUndefined());
}

static void checkMockLaidWhileOriginalRunsStandsBehindIt(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    installHook(program);

    AOT_TEST_CHECK(setMock(realm, dep, constructEmptyObject(realm), jsNumber(2)));
    AOT_TEST_CHECK(required(program, project) == jsNumber(2));
    hookDoes = [](JSGlobalObject* realm) {
        setMock(realm, dep, constructEmptyObject(realm), jsNumber(3));
        seenByProject = JSValue::encode(requireModule(realm, dep, project));
    };
    AOT_TEST_CHECK(required(program, package) == jsNumber(1));
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(JSValue::decode(seenByProject) == jsNumber(3));
    AOT_TEST_CHECK(hasActiveMock(realm, dep));
    AOT_TEST_CHECK(required(program, project) == jsNumber(3));
    AOT_TEST_CHECK(required(program, package) == jsNumber(1));
    AOT_TEST_CHECK(globalNamed(program, "ranTwice"_s).isUndefined());
}

static constexpr ASCIILiteral programTextWithManyMockedFiles = R"IR(variable g0 name="exports" module=m1 declared={type=anything} initialization=Undefined
variable g1 module=m1 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g2 name="exports" module=m2 declared={type=anything} initialization=Undefined
variable g3 module=m2 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g4 name="exports" module=m3 declared={type=anything} initialization=Undefined
variable g5 module=m3 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g6 name="exports" module=m4 declared={type=anything} initialization=Undefined
variable g7 module=m4 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g8 name="exports" module=m5 declared={type=anything} initialization=Undefined
variable g9 module=m5 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g10 name="exports" module=m6 declared={type=anything} initialization=Undefined
variable g11 module=m6 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g12 name="exports" module=m7 declared={type=anything} initialization=Undefined
variable g13 module=m7 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g14 name="exports" module=m8 declared={type=anything} initialization=Undefined
variable g15 module=m8 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g16 name="exports" module=m9 declared={type=anything} initialization=Undefined
variable g17 module=m9 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g18 name="exports" module=m10 declared={type=anything} initialization=Undefined
variable g19 module=m10 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g20 name="exports" module=m11 declared={type=anything} initialization=Undefined
variable g21 module=m11 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g22 name="exports" module=m12 declared={type=anything} initialization=Undefined
variable g23 module=m12 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g24 name="exports" module=m13 declared={type=anything} initialization=Undefined
variable g25 module=m13 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/mocked1.cjs" displayPath="mocked1.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f1 exportsVariable=g0
module m2 path="/mocked2.cjs" displayPath="mocked2.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f2 exportsVariable=g2
module m3 path="/mocked3.cjs" displayPath="mocked3.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f3 exportsVariable=g4
module m4 path="/mocked4.cjs" displayPath="mocked4.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f4 exportsVariable=g6
module m5 path="/mocked5.cjs" displayPath="mocked5.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f5 exportsVariable=g8
module m6 path="/mocked6.cjs" displayPath="mocked6.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f6 exportsVariable=g10
module m7 path="/mocked7.cjs" displayPath="mocked7.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f7 exportsVariable=g12
module m8 path="/mocked8.cjs" displayPath="mocked8.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f8 exportsVariable=g14
module m9 path="/mocked9.cjs" displayPath="mocked9.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f9 exportsVariable=g16
module m10 path="/mocked10.cjs" displayPath="mocked10.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f10 exportsVariable=g18
module m11 path="/mocked11.cjs" displayPath="mocked11.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f11 exportsVariable=g20
module m12 path="/mocked12.cjs" displayPath="mocked12.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f12 exportsVariable=g22
module m13 path="/mocked13.cjs" displayPath="mocked13.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f13 exportsVariable=g24
entry module=m0
entry module=m1
entry module=m2
entry module=m3
entry module=m4
entry module=m5
entry module=m6
entry module=m7
entry module=m8
entry module=m9
entry module=m10
entry module=m11
entry module=m12
entry module=m13

function f0 name="main" kind=Module results=[{type=anything}] {
    Return
}
function f1 name="mocked1" module=m1 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f2 name="mocked2" module=m2 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f3 name="mocked3" module=m3 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f4 name="mocked4" module=m4 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f5 name="mocked5" module=m5 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f6 name="mocked6" module=m6 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f7 name="mocked7" module=m7 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f8 name="mocked8" module=m8 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f9 name="mocked9" module=m9 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f10 name="mocked10" module=m10 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f11 name="mocked11" module=m11 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f12 name="mocked12" module=m12 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
function f13 name="mocked13" module=m13 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    Return
}
)IR"_s;

static constexpr uint32_t numberOfMockedFiles = 13;

static void checkMockIsKeptWhoseReadersEndOtherMocks(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Modules& modules = *program.state().modules;
    Identifier name = Identifier::fromString(vm, "name"_s);
    modules.startLoadingModulesIndividually();

    JSObject* mockOfPreload = constructEmptyObject(realm);
    mockOfPreload->putDirect(vm, name, jsNumber(1));
    AOT_TEST_CHECK(setMock(realm, dep, mockOfPreload, jsNumber(1)));
    moduleNamespace(realm, dep);
    didBeginTestFile(realm);
    AOT_TEST_CHECK(setMock(realm, dep, nullptr, { }));
    AOT_TEST_CHECK(modules.endedMock(dep, 0) == mockOfPreload);
    AOT_TEST_CHECK(setMock(realm, dep, mockOfPreload, jsNumber(1)));

    JSObject* patch = constructEmptyObject(realm);
    patch->putDirectCustomAccessor(vm, name, CustomGetterSetter::create(vm, hookOfBody, nullptr), static_cast<unsigned>(PropertyAttribute::CustomAccessor));
    AOT_TEST_CHECK(setMock(realm, dep, patch, jsNumber(2)));
    for (uint32_t file = dep.index() + 1; file <= numberOfMockedFiles; ++file)
        AOT_TEST_CHECK(setMock(realm, ModuleID(file), constructEmptyObject(realm), jsNumber(3)));
    static unsigned numberOfMocksEnded;
    numberOfMocksEnded = 0;
    hookDoes = [](JSGlobalObject* realm) {
        for (uint32_t file = dep.index() + 1; file <= numberOfMockedFiles; ++file)
            numberOfMocksEnded += setMock(realm, ModuleID(file), nullptr, { });
    };
    AOT_TEST_CHECK(setMock(realm, dep, nullptr, { }));
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(numberOfMocksEnded == numberOfMockedFiles - 1);

    AOT_TEST_CHECK(modules.endedMock(dep, 0) == mockOfPreload);
    JSObject* keptPatch = modules.endedMock(dep, 1);
    AOT_TEST_CHECK(keptPatch && keptPatch->getDirect(vm, name) && keptPatch->getDirect(vm, name).isUndefined());
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<testRunOriginalsTests, programText, body>;

template<LoadedProgramTest body>
static constexpr void (*withManyMockedFiles)() = runNativeTestWithLoadedProgram<testRunOriginalsTests, programTextWithManyMockedFiles, body>;

std::span<const NativeTest> testRunOriginalsTests()
{
    static constexpr NativeTest tests[] = {
        { "test-run-originals/package-gets-original-and-body-runs-once"_s, withProgram<checkPackageGetsOriginalAndBodyRunsOnce> },
        { "test-run-originals/original-that-throws-leaves-mock-and-runs-again"_s, withProgram<checkOriginalThatThrowsLeavesMockAndRunsAgain> },
        { "test-run-originals/project-that-asks-while-original-runs-gets-mock"_s, withProgram<checkProjectThatAsksWhileOriginalRunsGetsMock> },
        { "test-run-originals/original-that-loads-another-original-keeps-its-mock"_s, withProgram<checkOriginalThatLoadsAnotherOriginalKeepsItsMock> },
        { "test-run-originals/mock-that-ends-while-original-runs-leaves-original"_s, withProgram<checkMockThatEndsWhileOriginalRunsLeavesOriginal> },
        { "test-run-originals/mock-laid-while-original-runs-stands-behind-it"_s, withProgram<checkMockLaidWhileOriginalRunsStandsBehindIt> },
        { "test-run-originals/mock-is-kept-whose-getter-ends-other-mocks"_s, withManyMockedFiles<checkMockIsKeptWhoseReadersEndOtherMocks> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
