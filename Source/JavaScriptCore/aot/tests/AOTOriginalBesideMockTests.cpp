/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
#include "AOTNativeTest.h"
#include "CustomGetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"

namespace JSC { namespace AOT {

std::span<const NativeTest> originalBesideMockTests();

// The host of the tests gives an importer under node_modules the original: givesOriginalToPackages().
static constexpr ASCIILiteral programText = R"IR(variable g0 name="exports" module=m1 declared={type=anything} initialization=Undefined
variable g1 module=m1 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
variable g2 name="exports" module=m4 declared={type=anything} initialization=Undefined
variable g3 module=m4 declared={type=anything|absent} flags=[IsReplaceable, HoldsMock]
module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/node_modules/dep/index.js" displayPath="dep.js" kind=CommonJs flags=[KeepsError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f1 exportsVariable=g0
module m2 path="/node_modules/reads/index.js" displayPath="reads.js" kind=CommonJs flags=[KeepsError] requireResult=ModuleExports body=f2
module m3 path="/project.cjs" displayPath="project.cjs" kind=CommonJs flags=[KeepsError] requireResult=ModuleExports body=f3
module m4 path="/node_modules/other/index.js" displayPath="other.js" kind=CommonJs flags=[KeepsError, MayBeMocked, MayBeEvicted] requireResult=ModuleExports body=f4 exportsVariable=g2
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
    // Twice: an answer that has settled is given without a look at who asks.
    AOT_TEST_CHECK(required(program, project) == answerOfMock);
    AOT_TEST_CHECK(required(program, project) == answerOfMock);
    AOT_TEST_CHECK(globalNamed(program, "ranOnce"_s).isUndefined());

    AOT_TEST_CHECK(required(program, package) == original);
    AOT_TEST_CHECK(globalNamed(program, "ranOnce"_s) == jsNumber(1));
    AOT_TEST_CHECK(required(program, project) == answerOfMock);
    AOT_TEST_CHECK(required(program, package) == original);

    // What the body exported beside the mock is the file's once the mock has ended.
    AOT_TEST_CHECK(setMock(realm, dep, nullptr, { }));
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(required(program, project) == original);
    AOT_TEST_CHECK(required(program, package) == original);
    AOT_TEST_CHECK(globalNamed(program, "ranTwice"_s).isUndefined());
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

// The file may be evicted, and a CommonJS file of that kind whose body throws is left to the host. Not under a mock: the project goes on getting the mock.
static void checkOriginalThatThrowsLeavesMockAndRunsAgain(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    realm->putDirectCustomAccessor(vm, Identifier::fromString(vm, "hook"_s), CustomGetterSetter::create(vm, hookOfBody, nullptr), static_cast<unsigned>(PropertyAttribute::CustomAccessor));

    AOT_TEST_CHECK(setMock(realm, dep, constructEmptyObject(realm), jsNumber(2)));
    AOT_TEST_CHECK(required(program, project) == jsNumber(2));
    hookThrows = true;
    requireModule(realm, dep, package);
    hookThrows = false;
    AOT_TEST_CHECK(scope.exception());
    scope.clearException();
    AOT_TEST_CHECK(globalNamed(program, "ranOnce"_s).isUndefined());
    AOT_TEST_CHECK(hasMockInPlace(realm, dep));
    AOT_TEST_CHECK(required(program, project) == jsNumber(2));

    AOT_TEST_CHECK(required(program, package) == jsNumber(1));
    AOT_TEST_CHECK(required(program, project) == jsNumber(2));
    AOT_TEST_CHECK(globalNamed(program, "ranTwice"_s).isUndefined());
}

// The hook stands at the head of the body: nothing is exported yet.
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
    AOT_TEST_CHECK(!hasMockInPlace(realm, dep));
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
    AOT_TEST_CHECK(hasMockInPlace(realm, dep));
    AOT_TEST_CHECK(required(program, project) == jsNumber(3));
    AOT_TEST_CHECK(required(program, package) == jsNumber(1));
    AOT_TEST_CHECK(globalNamed(program, "ranTwice"_s).isUndefined());
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<originalBesideMockTests, programText, body>;

std::span<const NativeTest> originalBesideMockTests()
{
    static constexpr NativeTest tests[] = {
        { "original-beside-mock/package-gets-original-and-body-runs-once"_s, withProgram<checkPackageGetsOriginalAndBodyRunsOnce> },
        { "original-beside-mock/mock-that-no-package-asked-behind-leaves-body-to-run"_s, withProgram<checkMockThatNoPackageAskedBehindLeavesBodyToRun> },
        { "original-beside-mock/original-that-throws-leaves-mock-and-runs-again"_s, withProgram<checkOriginalThatThrowsLeavesMockAndRunsAgain> },
        { "original-beside-mock/project-that-asks-while-original-runs-gets-mock"_s, withProgram<checkProjectThatAsksWhileOriginalRunsGetsMock> },
        { "original-beside-mock/original-that-loads-another-original-keeps-its-mock"_s, withProgram<checkOriginalThatLoadsAnotherOriginalKeepsItsMock> },
        { "original-beside-mock/mock-that-ends-while-original-runs-leaves-original"_s, withProgram<checkMockThatEndsWhileOriginalRunsLeavesOriginal> },
        { "original-beside-mock/mock-laid-while-original-runs-stands-behind-it"_s, withProgram<checkMockLaidWhileOriginalRunsStandsBehindIt> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
