/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTLoadedProgramTestSupport.h"
#include "AOTModules.h"
#include "AOTNativeTest.h"
#include "ErrorInstance.h"
#include "JSCInlines.h"
#include "TopExceptionScope.h"
#include <wtf/DataLog.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> requiredNameTests();

static constexpr ASCIILiteral programText = R"IR(variable g0 name="shared" module=m5 declared={type=anything} initialization=Undefined
variable g1 name="single" module=m5 declared={type=anything} initialization=Undefined
variable g2 name="shared" module=m6 declared={type=anything} initialization=Undefined
module m0 path="/absent-allowed.ts" displayPath="absent-allowed.ts" body=f0 requests=[{target=m4 completes=true}] requiredNames=[{module=m4 name="absent" mayBeAbsent=true}]
module m1 path="/absent-required.js" displayPath="absent-required.js" body=f1 requests=[{target=m4 completes=true}] requiredNames=[{module=m4 name="absent"}]
module m2 path="/ambiguous.ts" displayPath="ambiguous.ts" body=f2 requests=[{target=m4 completes=true}] requiredNames=[{module=m4 name="shared" mayBeAbsent=true}]
module m3 path="/found.ts" displayPath="found.ts" body=f3 requests=[{target=m4 completes=true}] requiredNames=[{module=m4 name="single" mayBeAbsent=true}]
module m4 path="/barrel.js" displayPath="barrel.js" body=f4 requests=[{target=m5 isDynamicStarExport=true completes=true}, {target=m6 isDynamicStarExport=true completes=true}]
module m5 path="/one.js" displayPath="one.js" body=f5 exports=[{name="shared" variable=g0}, {name="single" variable=g1}]
module m6 path="/two.js" displayPath="two.js" body=f6 exports=[{name="shared" variable=g2}]
entry module=m0
entry module=m1
entry module=m2
entry module=m3

function f0 name="absentAllowed" kind=Module results=[{type=anything}] {
  b0():
    Return
}
function f1 name="absentRequired" module=m1 kind=Module results=[{type=anything}] {
  b0():
    Return
}
function f2 name="ambiguous" module=m2 kind=Module results=[{type=anything}] {
  b0():
    Return
}
function f3 name="found" module=m3 kind=Module results=[{type=anything}] {
  b0():
    Return
}
function f4 name="barrel" module=m4 kind=Module results=[{type=anything}] {
  b0():
    Return
}
function f5 name="one" module=m5 kind=Module results=[{type=anything}] {
  b0():
    Return
}
function f6 name="two" module=m6 kind=Module results=[{type=anything}] {
  b0():
    Return
}
)IR"_s;

static constexpr ModuleID absentAllowedModule { 0u };
static constexpr ModuleID absentRequiredModule { 1u };
static constexpr ModuleID ambiguousModule { 2u };
static constexpr ModuleID foundModule { 3u };

static void checkModuleLinks(LoadedProgram& program, ModuleID module)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());

    evaluateModuleSynchronously(program.realm(), module);

    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(modulesOf(program.realm())->status(module) == ModuleStatus::Evaluated);
}

static void checkLinkThrowsSyntaxError(LoadedProgram& program, ModuleID module, ASCIILiteral expectedMessage)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());

    evaluateModuleSynchronously(program.realm(), module);

    AOT_TEST_CHECK(scope.exception());
    auto* error = dynamicDowncast<ErrorInstance>(scope.exception()->value());
    AOT_TEST_CHECK(error && error->errorType() == ErrorType::SyntaxError);
    scope.clearException();
    String message = error->sanitizedMessageString(program.realm());
    if (message != expectedMessage)
        dataLogLn("the message is ", message, ", expected ", expectedMessage);
    AOT_TEST_CHECK(message == expectedMessage);
    AOT_TEST_CHECK(modulesOf(program.realm())->status(module) != ModuleStatus::Evaluated);
}

static void checkAbsentNameThatMayBeAbsentLinks(LoadedProgram& program)
{
    checkModuleLinks(program, absentAllowedModule);
}

static void checkAbsentNameFailsLink(LoadedProgram& program)
{
    checkLinkThrowsSyntaxError(program, absentRequiredModule, "Export named 'absent' not found in module '/barrel.js'."_s);
}

static void checkAmbiguousNameThatMayBeAbsentFailsLink(LoadedProgram& program)
{
    checkLinkThrowsSyntaxError(program, ambiguousModule, "Export named 'shared' cannot be resolved due to ambiguous multiple bindings in module '/barrel.js'."_s);
}

static void checkFoundNameLinks(LoadedProgram& program)
{
    checkModuleLinks(program, foundModule);
}

template<LoadedProgramTest body>
static constexpr void (*withBarrel)() = runNativeTestWithLoadedProgram<requiredNameTests, programText, body>;

std::span<const NativeTest> requiredNameTests()
{
    static constexpr NativeTest tests[] = {
        { "required-names/absent-name-that-may-be-absent-links"_s, withBarrel<checkAbsentNameThatMayBeAbsentLinks> },
        { "required-names/absent-name-fails-link"_s, withBarrel<checkAbsentNameFailsLink> },
        { "required-names/ambiguous-name-that-may-be-absent-fails-link"_s, withBarrel<checkAmbiguousNameThatMayBeAbsentFailsLink> },
        { "required-names/found-name-links"_s, withBarrel<checkFoundNameLinks> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
