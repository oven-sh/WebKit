/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
#include "AOTNativeTest.h"
#include "ErrorInstance.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"

namespace JSC { namespace AOT {

std::span<const NativeTest> moduleAdoptionTests();

static constexpr ASCIILiteral programText = R"IR(module m0 path="/app/main.cjs" displayPath="main.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f0
entry module=m0

function f0 name="main" kind=CommonJsModule flags=[HasThis] thisType={type=anything} parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = Integer integer=1
    PropSet v1, v6 key="ran"
    Return
}
)IR"_s;

static constexpr ModuleID mainModule { 0u };

static bool hasRun(VM& vm, JSObject* exports)
{
    return exports->getDirect(vm, Identifier::fromString(vm, "ran"_s)) == jsNumber(1);
}

static JSObject* exportsOf(VM& vm, JSObject* module)
{
    return asObject(module->getDirect(vm, vm.propertyNames->exports));
}

static JSObject* createModuleWithReplacedExports(JSGlobalObject* realm)
{
    VM& vm = realm->vm();
    JSObject* module = createCommonJSModuleOfTestHost(realm);
    module->putDirect(vm, vm.propertyNames->exports, constructEmptyObject(realm));
    return module;
}

static void checkTypeErrorWasThrown(TopExceptionScope& scope)
{
    AOT_TEST_CHECK(scope.exception());
    auto* error = dynamicDowncast<ErrorInstance>(scope.exception()->value());
    AOT_TEST_CHECK(error && error->errorType() == ErrorType::TypeError);
    scope.clearException();
}

static void checkBodyRunsWithModuleAsCreated(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSObject* module = createCommonJSModuleOfTestHost(program.realm());

    evaluateCommonJSBody(program.realm(), mainModule, module);

    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(hasRun(vm, exportsOf(vm, module)));
}

static void checkBodyDoesNotRunWithReplacedExports(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSObject* module = createModuleWithReplacedExports(program.realm());

    evaluateCommonJSBody(program.realm(), mainModule, module);

    checkTypeErrorWasThrown(scope);
    AOT_TEST_CHECK(!hasRun(vm, exportsOf(vm, module)));
}

static void checkBodyDoesNotRunWithPrimitiveExports(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSObject* module = createCommonJSModuleOfTestHost(program.realm());
    module->putDirect(vm, vm.propertyNames->exports, jsNumber(5));

    evaluateCommonJSBody(program.realm(), mainModule, module);

    checkTypeErrorWasThrown(scope);
}

static void checkRejectedRequireLeavesModuleUnevaluated(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSObject* rejected = createModuleWithReplacedExports(realm);

    JSValue result = requireModule(realm, mainModule, rejected);

    checkTypeErrorWasThrown(scope);
    AOT_TEST_CHECK(!result && !hasRun(vm, exportsOf(vm, rejected)) && !isEvaluated(realm, mainModule));

    JSObject* adopted = createCommonJSModuleOfTestHost(realm);
    result = requireModule(realm, mainModule, adopted);

    AOT_TEST_CHECK(!scope.exception() && isEvaluated(realm, mainModule));
    AOT_TEST_CHECK(result == JSValue(exportsOf(vm, adopted)) && hasRun(vm, exportsOf(vm, adopted)));
}

template<LoadedProgramTest body>
static constexpr void (*withCommonJSModule)() = runNativeTestWithLoadedProgram<moduleAdoptionTests, programText, body>;

std::span<const NativeTest> moduleAdoptionTests()
{
    static constexpr NativeTest tests[] = {
        { "module-adoption/body-runs-with-module-as-created"_s, withCommonJSModule<checkBodyRunsWithModuleAsCreated> },
        { "module-adoption/body-does-not-run-with-replaced-exports"_s, withCommonJSModule<checkBodyDoesNotRunWithReplacedExports> },
        { "module-adoption/body-does-not-run-with-primitive-exports"_s, withCommonJSModule<checkBodyDoesNotRunWithPrimitiveExports> },
        { "module-adoption/rejected-require-leaves-module-unevaluated"_s, withCommonJSModule<checkRejectedRequireLeavesModuleUnevaluated> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
