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
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> begunImporterTests();

// dep.js is what a mock stands for. single.js imports it. outer.js imports it and then helper.js. root.js imports member.js, which imports dep.js and root.js.
// turns.js imports dep.js, then back.js, which imports turns.js, and then helper.js. The file of a package imports dep.js and then helper.js.
static constexpr ASCIILiteral programText = R"IR(module m0 path="/root.js" displayPath="root.js" body=f0 requests=[{target=m2 completes=true}]
module m1 path="/dep.js" displayPath="dep.js" flags=[MayBeMocked] body=f1 exports=[{name="text" variable=g0}]
module m2 path="/member.js" displayPath="member.js" body=f2 requests=[{target=m1 completes=true}, {target=m0 completes=true}]
module m3 path="/single.js" displayPath="single.js" body=f3 requests=[{target=m1 completes=true}]
module m4 path="/outer.js" displayPath="outer.js" body=f4 requests=[{target=m1 completes=true}, {target=m5 completes=true}]
module m5 path="/helper.js" displayPath="helper.js" body=f5
module m6 path="/turns.js" displayPath="turns.js" body=f6 requests=[{target=m1 completes=true}, {target=m7 completes=true}, {target=m5 completes=true}]
module m7 path="/back.js" displayPath="back.js" body=f7 requests=[{target=m6 completes=true}]
module m8 path="/node_modules/package/index.js" displayPath="package/index.js" body=f8 requests=[{target=m1 completes=true}, {target=m5 completes=true}]
entry module=m0
entry module=m3
entry module=m4
entry module=m6
entry module=m8
variable g0 name="text" module=m1 declared={type=anything} flags=[IsReplaceable]
function f0 name="root" module=m0 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="hook"
    Return
}
function f1 name="dep" module=m1 kind=Module results=[{type=anything}] {
    v0 = Integer integer=1
    PutModuleVariable v0 variable=g0
    Return
}
function f2 name="member" module=m2 kind=Module results=[{type=anything}] {
    Return
}
function f3 name="single" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="hook"
    Return
}
function f4 name="outer" module=m4 kind=Module results=[{type=anything}] {
    Return
}
function f5 name="helper" module=m5 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="hook"
    Return
}
function f6 name="turns" module=m6 kind=Module results=[{type=anything}] {
    Return
}
function f7 name="back" module=m7 kind=Module results=[{type=anything}] {
    Return
}
function f8 name="package" module=m8 kind=Module results=[{type=anything}] {
    Return
}
)IR"_s;

static constexpr ModuleID root { 0u };
static constexpr ModuleID dep { 1u };
static constexpr ModuleID member { 2u };
static constexpr ModuleID single { 3u };
static constexpr ModuleID outer { 4u };
static constexpr ModuleID helper { 5u };
static constexpr ModuleID turns { 6u };
static constexpr ModuleID package { 8u };

// What the first body that reads `hook` learns about the importers of dep.js.
struct AnswersOfHook {
    // In: the module that asks, as the host names the file of the call.
    std::optional<ModuleID> asking;
    bool hasRun { false };
    bool hasImporterByStatus { false };
    bool hasImporterThatHasYetToBegin { false };
    BegunImporters begun;
};

static AnswersOfHook& answersOfHook()
{
    static NeverDestroyed<AnswersOfHook> answers;
    return answers;
}

static JSC_DECLARE_CUSTOM_GETTER(hookOfBody);

JSC_DEFINE_CUSTOM_GETTER(hookOfBody, (JSGlobalObject* realm, EncodedJSValue, PropertyName))
{
    VM& vm = realm->vm();
    AnswersOfHook& answers = answersOfHook();
    if (std::exchange(answers.hasRun, true))
        return JSValue::encode(jsUndefined());
    // dep.js has run by now, and each importer on the way here is linked to it.
    setMock(realm, dep, constructEmptyObject(realm), jsUndefined());
    forEachEvaluatingModuleOnStack(vm, vm.topCallFrame, [&](ModuleID module) {
        answers.begun.bodiesOnStack.append(module);
    });
    answers.hasImporterByStatus = hasUnevaluatedActiveMockImporter(realm, dep, answers.asking);
    answers.hasImporterThatHasYetToBegin = hasUnevaluatedActiveMockImporter(realm, dep, answers.asking, &answers.begun);
    return JSValue::encode(jsUndefined());
}

static const AnswersOfHook& evaluateWithHookThatMocksDep(LoadedProgram& program, ModuleID entry, std::optional<ModuleID> asking = std::nullopt)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    answersOfHook().asking = asking;
    realm->putDirectCustomAccessor(vm, Identifier::fromString(vm, "hook"_s), CustomGetterSetter::create(vm, hookOfBody, nullptr), static_cast<unsigned>(PropertyAttribute::CustomAccessor));

    evaluateModuleSynchronously(realm, entry);
    if (Exception* thrown = scope.exception()) {
        scope.clearException();
        dataLogLn("The entry threw: ", thrown->value().toWTFString(realm));
        AOT_TEST_CHECK(false);
    }
    AOT_TEST_CHECK(answersOfHook().hasRun);
    AOT_TEST_CHECK(hasActiveMock(realm, dep));
    return answersOfHook();
}

// single.js mocks while its body runs. Its status says that it has yet to finish, and it has begun.
static void checkImporterWhoseBodyIsOnTheStackIsPassedOver(LoadedProgram& program)
{
    const AnswersOfHook& answers = evaluateWithHookThatMocksDep(program, single);
    AOT_TEST_CHECK(answers.begun.bodiesOnStack.size() == 1 && answers.begun.bodiesOnStack.first() == single);
    AOT_TEST_CHECK(answers.hasImporterByStatus);
    AOT_TEST_CHECK(!answers.hasImporterThatHasYetToBegin);
    AOT_TEST_CHECK(answers.begun.passedOver.size() == 1 && answers.begun.passedOver.first() == single);
}

// single.js is the file of the call. The rule leaves it out with or without the argument, and does not name it.
static void checkImporterThatAsksIsLeftOut(LoadedProgram& program)
{
    const AnswersOfHook& answers = evaluateWithHookThatMocksDep(program, single, single);
    AOT_TEST_CHECK(answers.begun.bodiesOnStack.size() == 1 && answers.begun.bodiesOnStack.first() == single);
    AOT_TEST_CHECK(!answers.hasImporterByStatus);
    AOT_TEST_CHECK(!answers.hasImporterThatHasYetToBegin);
    AOT_TEST_CHECK(answers.begun.passedOver.isEmpty());
}

// helper.js mocks while the file of a package is at its imports. The test host externalizes every package, so that file gets dep.js itself: it does not count, and is not named.
static void checkExternalizedPackageAtItsImportsDoesNotCount(LoadedProgram& program)
{
    const AnswersOfHook& answers = evaluateWithHookThatMocksDep(program, package);
    AOT_TEST_CHECK(answers.begun.bodiesOnStack.size() == 1 && answers.begun.bodiesOnStack.first() == helper);
    AOT_TEST_CHECK(answers.hasImporterByStatus);
    AOT_TEST_CHECK(!answers.hasImporterThatHasYetToBegin);
    AOT_TEST_CHECK(answers.begun.passedOver.isEmpty());
}

// helper.js mocks while outer.js is at its imports. The body of outer.js has yet to begin, so it counts.
static void checkImporterAtItsImportsCounts(LoadedProgram& program)
{
    const AnswersOfHook& answers = evaluateWithHookThatMocksDep(program, outer);
    AOT_TEST_CHECK(answers.begun.bodiesOnStack.size() == 1 && answers.begun.bodiesOnStack.first() == helper);
    AOT_TEST_CHECK(answers.hasImporterByStatus);
    AOT_TEST_CHECK(answers.hasImporterThatHasYetToBegin);
    AOT_TEST_CHECK(answers.begun.passedOver.isEmpty());
}

// helper.js mocks while turns.js is at its imports. turns.js is in an import cycle whose other member has run, and its own body has yet to begin.
static void checkMemberOfCycleAtItsImportsCounts(LoadedProgram& program)
{
    const AnswersOfHook& answers = evaluateWithHookThatMocksDep(program, turns);
    AOT_TEST_CHECK(answers.begun.bodiesOnStack.size() == 1 && answers.begun.bodiesOnStack.first() == helper);
    AOT_TEST_CHECK(answers.hasImporterByStatus);
    AOT_TEST_CHECK(answers.hasImporterThatHasYetToBegin);
    AOT_TEST_CHECK(answers.begun.passedOver.isEmpty());
}

// root.js mocks while its body runs. The body of member.js has returned, and member.js stays in evaluation until root.js is done.
static void checkMemberOfCycleWhoseBodyHasRunIsPassedOver(LoadedProgram& program)
{
    const AnswersOfHook& answers = evaluateWithHookThatMocksDep(program, root);
    AOT_TEST_CHECK(answers.begun.bodiesOnStack.size() == 1 && answers.begun.bodiesOnStack.first() == root);
    AOT_TEST_CHECK(answers.hasImporterByStatus);
    AOT_TEST_CHECK(!answers.hasImporterThatHasYetToBegin);
    AOT_TEST_CHECK(answers.begun.passedOver.size() == 1 && answers.begun.passedOver.first() == member);
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<begunImporterTests, programText, body>;

std::span<const NativeTest> begunImporterTests()
{
    static constexpr NativeTest tests[] = {
        { "begun-importers/importer-whose-body-is-on-the-stack-is-passed-over"_s, withProgram<checkImporterWhoseBodyIsOnTheStackIsPassedOver> },
        { "begun-importers/importer-that-asks-is-left-out"_s, withProgram<checkImporterThatAsksIsLeftOut> },
        { "begun-importers/externalized-package-at-its-imports-does-not-count"_s, withProgram<checkExternalizedPackageAtItsImportsDoesNotCount> },
        { "begun-importers/importer-at-its-imports-counts"_s, withProgram<checkImporterAtItsImportsCounts> },
        { "begun-importers/member-of-cycle-at-its-imports-counts"_s, withProgram<checkMemberOfCycleAtItsImportsCounts> },
        { "begun-importers/member-of-cycle-whose-body-has-run-is-passed-over"_s, withProgram<checkMemberOfCycleWhoseBodyHasRunIsPassedOver> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
