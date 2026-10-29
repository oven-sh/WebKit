/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTHostEntriesTestSupport.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCallee.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
#include "AOTModuleTable.h"
#include "AOTNativeTest.h"
#include "AOTObjectModel.h"
#include "CallData.h"
#include "ErrorInstance.h"
#include "JSCInlines.h"
#include "JSPromiseConstructor.h"
#include "JSPromisePrototype.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"
#include <wtf/DataLog.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> nakedConstructorTests();

static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="internal:library" displayPath="internal:library" kind=RuntimeSource flags=[IsLibrary] exportRule=RuntimeModule
entry module=m0
runtimefunction id=PromiseConstructorPromise function=f1
layout L0 kind=Instance name="Widget" constructor=f2
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = NewClosure function=f2
    v2 = NewClass v1 layout=L0
    v3 = FinishClass v2 layout=L0
    PropSet v0, v3 key="Widget"
    v5 = PropGet v0 key="publish" [isFreeName]
    v6 = PropGet v0 key="Promise" [isFreeName]
    v7 = Call v5, -, v6
    Return
}
function f1 name="Promise" module=m1 kind=BaseConstructor role=Library flags=[HasNewTarget, HasExplicitName, IsNakedConstructor] enclosingRuntimeFunction=PromiseConstructorPromise parameters=[{type=anything}] results=[{type=object}] {
    v2 = Builtin builtin=GlobalThis
    Return v2
}
function f2 name="Widget" kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] results=[{type=object}] {
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
)IR"_s;

static constexpr ModuleID mainModule { 0u };

static void checkCallThrowsTypeError(LoadedProgram& program, JSValue callee, ASCIILiteral expectedMessage)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    AOT_TEST_CHECK(callee.isCallable());

    call(program.realm(), callee, jsUndefined(), ArgList(), "the callee is callable"_s);

    AOT_TEST_CHECK(scope.exception());
    auto* error = dynamicDowncast<ErrorInstance>(scope.exception()->value());
    AOT_TEST_CHECK(error && error->errorType() == ErrorType::TypeError);
    scope.clearException();
    String message = error->sanitizedMessageString(program.realm());
    if (message != expectedMessage)
        dataLogLn("the message is ", message, ", expected ", expectedMessage);
    AOT_TEST_CHECK(message == expectedMessage);
}

static void checkCompiledClosureNamesConstructor(LoadedProgram& program)
{
    AOT_TEST_CHECK(!functionImplementing(Builtins::Promise).index());
    const CompiledRuntimeFunctionData* entry = ModuleTable(program.instance().tables()).findRuntimeFunction(0);
    AOT_TEST_CHECK(entry);
    JSCell* closure = program.state().objects->closures.closure(program.vm(), FunctionID(entry->function));

    checkCallThrowsTypeError(program, closure, "Cannot call a constructor Promise without |new|"_s);
}

static void checkBuiltinOfRealmNamesConstructor(LoadedProgram& program)
{
    checkCallThrowsTypeError(program, program.realm()->promiseConstructor(), "Cannot call a constructor Promise without |new|"_s);
}

static void checkClassNamesClassConstructor(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    program.realms().evaluate(realm, "globalThis.publish = function () { }"_s);
    evaluateModuleSynchronously(realm, mainModule);
    AOT_TEST_CHECK(!scope.exception());

    checkCallThrowsTypeError(program, realm->get(realm, Identifier::fromString(vm, "Widget"_s)), "Cannot call a class constructor Widget without |new|"_s);
}

static void checkCompiledClosureIsDeclaredAsFunction(LoadedProgram& program)
{
    const CompiledRuntimeFunctionData* entry = ModuleTable(program.instance().tables()).findRuntimeFunction(0);
    AOT_TEST_CHECK(entry);
    JSCell* closure = program.state().objects->closures.closure(program.vm(), FunctionID(entry->function));

    AOT_TEST_CHECK(declaredAs(closure) == DeclaredAs::Function);
}

static void checkBuiltinOfRealmIsDeclaredAsFunction(LoadedProgram& program)
{
    JSFunction* promiseConstructor = program.realm()->promiseConstructor();
    AOT_TEST_CHECK(promiseConstructor->isHostFunction());

    AOT_TEST_CHECK(declaredAs(promiseConstructor) == DeclaredAs::Function);
}

static void checkClassIsDeclaredAsClass(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    program.realms().evaluate(realm, "globalThis.publish = function () { }"_s);
    evaluateModuleSynchronously(realm, mainModule);
    AOT_TEST_CHECK(!scope.exception());

    AOT_TEST_CHECK(declaredAs(realm->get(realm, Identifier::fromString(vm, "Widget"_s)).asCell()) == DeclaredAs::Class);
}

static void checkHostFunctionsAreNotCompiled(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    JSValue resolve = realm->promiseConstructor()->get(realm, Identifier::fromString(vm, "resolve"_s));
    AOT_TEST_CHECK(resolve.isCallable());

    AOT_TEST_CHECK(declaredAs(resolve.asCell()) == DeclaredAs::NotCompiled);
    AOT_TEST_CHECK(declaredAs(realm->objectConstructor()) == DeclaredAs::NotCompiled);
    AOT_TEST_CHECK(declaredAs(realm->promisePrototype()) == DeclaredAs::NotCompiled);
}

template<LoadedProgramTest body>
static constexpr void (*withConstructors)() = runNativeTestWithLoadedProgram<nakedConstructorTests, programText, body>;

std::span<const NativeTest> nakedConstructorTests()
{
    static constexpr NativeTest tests[] = {
        { "naked-constructor/compiled-closure-names-constructor"_s, withConstructors<checkCompiledClosureNamesConstructor> },
        { "naked-constructor/builtin-of-realm-names-constructor"_s, withConstructors<checkBuiltinOfRealmNamesConstructor> },
        { "naked-constructor/class-names-class-constructor"_s, withConstructors<checkClassNamesClassConstructor> },
        { "naked-constructor/compiled-closure-is-declared-as-function"_s, withConstructors<checkCompiledClosureIsDeclaredAsFunction> },
        { "naked-constructor/builtin-of-realm-is-declared-as-function"_s, withConstructors<checkBuiltinOfRealmIsDeclaredAsFunction> },
        { "naked-constructor/class-is-declared-as-class"_s, withConstructors<checkClassIsDeclaredAsClass> },
        { "naked-constructor/host-functions-are-not-compiled"_s, withConstructors<checkHostFunctionsAreNotCompiled> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
