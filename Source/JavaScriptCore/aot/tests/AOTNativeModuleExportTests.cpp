/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTHostEntriesTestSupport.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
#include "AOTNativeTest.h"
#include "JSCInlines.h"
#include "TopExceptionScope.h"

namespace JSC { namespace AOT {

std::span<const NativeTest> nativeModuleExportTests();

static constexpr ASCIILiteral programText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1 completes=true}, {target=m2 completes=true}]
module m1 path="bun:jsc" displayPath="bun:jsc" kind=Runtime exportRule=RuntimeModule requireResult=DefaultExport runtimeModule=BunJsc
module m2 path="node:fs" displayPath="node:fs" kind=Runtime exportRule=RuntimeModule requireResult=DefaultExport runtimeModule=NodeFs
entry module=m0

function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = ModuleNamespace module=m1
    PropSet v0, v1 key="nativeModule"
    v3 = ModuleNamespace module=m2
    PropSet v0, v3 key="moduleWithSource"
    Return
}
)IR"_s;

static constexpr ModuleID mainModule { 0u };

static JSObject* namespaceStoredAs(LoadedProgram& program, ASCIILiteral globalName)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    evaluateModuleSynchronously(realm, mainModule);
    AOT_TEST_CHECK(!scope.exception());

    JSValue stored = realm->get(realm, Identifier::fromString(vm, globalName));
    AOT_TEST_CHECK(!scope.exception() && stored.isObject() && isModuleNamespace(stored.asCell()));
    return asObject(stored);
}

static JSValue exportOf(LoadedProgram& program, JSObject* namespaceObject, ASCIILiteral name)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSValue value = namespaceObject->get(program.realm(), Identifier::fromString(program.vm(), name));
    AOT_TEST_CHECK(!scope.exception());
    return value;
}

static void checkNativeModuleExportsWhatHostLists(LoadedProgram& program)
{
    JSObject* namespaceObject = namespaceStoredAs(program, "nativeModule"_s);

    AOT_TEST_CHECK(exportOf(program, namespaceObject, "listedByHost"_s) == jsNumber(2));
    AOT_TEST_CHECK(exportOf(program, namespaceObject, "default"_s) == jsNumber(hostModule("BunJsc"_s).index()));
    AOT_TEST_CHECK(exportOf(program, namespaceObject, "propertyOfModuleObject"_s).isUndefined());
}

static void checkModuleWithSourceExportsPropertiesOfItsObject(LoadedProgram& program)
{
    JSObject* namespaceObject = namespaceStoredAs(program, "moduleWithSource"_s);

    AOT_TEST_CHECK(exportOf(program, namespaceObject, "propertyOfModuleObject"_s) == jsNumber(1));
    AOT_TEST_CHECK(exportOf(program, namespaceObject, "default"_s).isObject());
    AOT_TEST_CHECK(exportOf(program, namespaceObject, "listedByHost"_s).isUndefined());
}

// The host defers `deferredByHost`, and each module that binds it says so.
static constexpr ASCIILiteral deferredExportText = R"IR(variable g0 name="default" module=m3 declared={type=anything}
variable g1 name="deferredByHost" module=m3 declared={type=anything}
variable g2 name="propertyOfModuleObject" module=m3 declared={type=anything}
module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m3 completes=true}]
module m1 path="/first.js" displayPath="first.js" body=f1 requests=[{target=m3 completes=true}] requiredNames=[{module=m3 name="deferredByHost"}]
module m2 path="/second.js" displayPath="second.js" body=f2 requests=[{target=m3 completes=true}] requiredNames=[{module=m3 name="deferredByHost"}]
module m3 path="node:fs" displayPath="node:fs" kind=Runtime exportRule=RuntimeModule requireResult=DefaultExport runtimeModule=NodeFs syntheticExports=[{name="default" variable=g0}, {name="deferredByHost" variable=g1}, {name="propertyOfModuleObject" variable=g2}]
entry module=m0
entry module=m1
entry module=m2

function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = GetModuleVariable variable=g0
    PropSet v0, v1 key="moduleObject"
    v3 = GetModuleVariable variable=g2
    PropSet v0, v3 key="heldFromTheLoad"
    Return
}
function f1 name="first" module=m1 kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = GetModuleVariable variable=g1
    PropSet v0, v1 key="seenByFirst"
    Return
}
function f2 name="second" module=m2 kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = GetModuleVariable variable=g1
    PropSet v0, v1 key="seenBySecond"
    Return
}
)IR"_s;

static JSValue globalNamed(LoadedProgram& program, ASCIILiteral name)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSValue value = program.realm()->get(program.realm(), Identifier::fromString(program.vm(), name));
    AOT_TEST_CHECK(!scope.exception());
    return value;
}

static void evaluateWithoutException(LoadedProgram& program, ModuleID module)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    evaluateModuleSynchronously(program.realm(), module);
    AOT_TEST_CHECK(!scope.exception());
}

static void checkDeferredExportIsReadWhenFirstModuleThatBindsItIsLinked(LoadedProgram& program)
{
    VM& vm = program.vm();
    Identifier deferred = Identifier::fromString(vm, "deferredByHost"_s);
    Identifier held = Identifier::fromString(vm, "propertyOfModuleObject"_s);

    evaluateWithoutException(program, ModuleID(0u));
    JSObject* moduleObject = asObject(globalNamed(program, "moduleObject"_s));
    AOT_TEST_CHECK(globalNamed(program, "heldFromTheLoad"_s) == jsNumber(1));

    // Nobody has read it: the module that is linked next sees what is stored now.
    moduleObject->putDirect(vm, deferred, jsNumber(4));
    moduleObject->putDirect(vm, held, jsNumber(5));
    evaluateWithoutException(program, ModuleID(1u));
    AOT_TEST_CHECK(globalNamed(program, "seenByFirst"_s) == jsNumber(4));

    // It is read once.
    moduleObject->putDirect(vm, deferred, jsNumber(6));
    evaluateWithoutException(program, ModuleID(2u));
    AOT_TEST_CHECK(globalNamed(program, "seenBySecond"_s) == jsNumber(4));
}

template<LoadedProgramTest body>
static constexpr void (*withRuntimeModules)() = runNativeTestWithLoadedProgram<nativeModuleExportTests, programText, body>;

template<LoadedProgramTest body>
static constexpr void (*withDeferredExport)() = runNativeTestWithLoadedProgram<nativeModuleExportTests, deferredExportText, body>;

std::span<const NativeTest> nativeModuleExportTests()
{
    static constexpr NativeTest tests[] = {
        { "native-module-exports/native-module-exports-what-host-lists"_s, withRuntimeModules<checkNativeModuleExportsWhatHostLists> },
        { "native-module-exports/module-with-source-exports-properties-of-its-object"_s, withRuntimeModules<checkModuleWithSourceExportsPropertiesOfItsObject> },
        { "native-module-exports/deferred-export-is-read-when-first-module-that-binds-it-is-linked"_s, withDeferredExport<checkDeferredExportIsReadWhenFirstModuleThatBindsItIsLinked> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
