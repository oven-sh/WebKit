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
module m7 path="/exports-absent.js" displayPath="exports-absent.js" body=f7 requests=[{target=m4 completes=true}] requiredNames=[{module=m4 name="absent" exportName="renamed" specifier="./barrel"}, {module=m4 name="alsoAbsent"}]
module m8 path="/exports-ambiguous.js" displayPath="exports-ambiguous.js" body=f8 requests=[{target=m4 completes=true}] requiredNames=[{module=m4 name="single"}, {module=m4 name="shared" exportName="shared" specifier="./barrel.js"}]
module m9 path="/exports-ambiguous-found-unless-mocked.js" displayPath="exports-ambiguous-found-unless-mocked.js" body=f9 requests=[{target=m4 completes=true}] requiredNames=[{module=m4 name="shared" mayBeAbsent=true exportName="shared" specifier="./barrel.js"}]
module m10 path="/stars-ambiguous.js" displayPath="stars-ambiguous.js" body=f10 requests=[{target=m4 completes=true}] requiredNames=[{module=m4 name="shared" mayBeAbsent=true exportName="shared" specifier="./barrel.js" isRequestedByStarExport=true}]
entry module=m0
entry module=m1
entry module=m2
entry module=m3
entry module=m7
entry module=m8
entry module=m9
entry module=m10

function f0 name="absentAllowed" kind=Module results=[{type=anything}] {
    Return
}
function f1 name="absentRequired" module=m1 kind=Module results=[{type=anything}] {
    Return
}
function f2 name="ambiguous" module=m2 kind=Module results=[{type=anything}] {
    Return
}
function f3 name="found" module=m3 kind=Module results=[{type=anything}] {
    Return
}
function f4 name="barrel" module=m4 kind=Module results=[{type=anything}] {
    Return
}
function f5 name="one" module=m5 kind=Module results=[{type=anything}] {
    Return
}
function f6 name="two" module=m6 kind=Module results=[{type=anything}] {
    Return
}
function f7 name="exportsAbsent" module=m7 kind=Module results=[{type=anything}] {
    Return
}
function f8 name="exportsAmbiguous" module=m8 kind=Module results=[{type=anything}] {
    Return
}
function f9 name="exportsAmbiguousFoundUnlessMocked" module=m9 kind=Module results=[{type=anything}] {
    Return
}
function f10 name="starsAmbiguous" module=m10 kind=Module results=[{type=anything}] {
    Return
}
)IR"_s;

static constexpr ModuleID absentAllowedModule { 0u };
static constexpr ModuleID absentRequiredModule { 1u };
static constexpr ModuleID foundModule { 3u };
static constexpr ModuleID exportsAbsentModule { 7u };
static constexpr ModuleID exportsAmbiguousModule { 8u };
static constexpr ModuleID exportsAmbiguousFoundUnlessMockedModule { 9u };
static constexpr ModuleID starsAmbiguousModule { 10u };

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

static void checkAbsentNameOfExportStatementFailsLink(LoadedProgram& program)
{
    checkLinkThrowsSyntaxError(program, exportsAbsentModule, "export 'renamed' not found in './barrel'"_s);
}

static void checkAmbiguousNameOfExportStatementFailsLink(LoadedProgram& program)
{
    checkLinkThrowsSyntaxError(program, exportsAmbiguousModule, "Cannot export 'shared' multiple times in './barrel.js'"_s);
}

static void checkAmbiguousNameOfExportStatementThatMayBeAbsentFailsLink(LoadedProgram& program)
{
    checkLinkThrowsSyntaxError(program, exportsAmbiguousFoundUnlessMockedModule, "Cannot export 'shared' multiple times in './barrel.js'"_s);
}

static void checkAmbiguousNameThatStarAsksForLinks(LoadedProgram& program)
{
    checkModuleLinks(program, starsAmbiguousModule);
}

static void checkFoundNameLinks(LoadedProgram& program)
{
    checkModuleLinks(program, foundModule);
}

static constexpr ASCIILiteral programOfModulesThatPassLinkOn = R"IR(variable g0 name="single" module=m4 declared={type=anything} initialization=Undefined
module m0 path="/first.js" displayPath="first.js" body=f0 requests=[{target=m1 completes=true}]
module m1 path="/below-first.js" displayPath="below-first.js" requests=[{target=m2 completes=true}]
module m2 path="/names.js" displayPath="names.js" body=f1 requests=[{target=m3 completes=true}] requiredNames=[{module=m3 name="single"}]
module m3 path="/barrel.js" displayPath="barrel.js" body=f2 requests=[{target=m4 isDynamicStarExport=true completes=true}]
module m4 path="/one.js" displayPath="one.js" body=f3 exports=[{name="single" variable=g0}]
module m5 path="/second.js" displayPath="second.js" body=f4 requests=[{target=m6 completes=true}]
module m6 path="/below-two.js" displayPath="below-two.js" requests=[{target=m2 completes=true}]
module m7 path="/third.js" displayPath="third.js" body=f5 requests=[{target=m6 completes=true}]
module m8 path="/alone.js" displayPath="alone.js" body=f6
entry module=m8
entry module=m0
entry module=m5
entry module=m7

function f0 name="first" module=m0 kind=Module results=[{type=anything}] {
    Return
}
function f1 name="names" module=m2 kind=Module results=[{type=anything}] {
    Return
}
function f2 name="barrel" module=m3 kind=Module results=[{type=anything}] {
    Return
}
function f3 name="one" module=m4 kind=Module results=[{type=anything}] {
    Return
}
function f4 name="second" module=m5 kind=Module results=[{type=anything}] {
    Return
}
function f5 name="third" module=m7 kind=Module results=[{type=anything}] {
    Return
}
function f6 name="alone" module=m8 kind=Module results=[{type=anything}] {
    Return
}
)IR"_s;

static void checkModulesThatPassLinkOnAreLinkedWithWhatLeadsToThem(LoadedProgram& program)
{
    constexpr ModuleID first { 0u };
    constexpr ModuleID belowFirst { 1u };
    constexpr ModuleID second { 5u };
    constexpr ModuleID belowTwo { 6u };
    Modules& modules = *modulesOf(program.realm());
    AOT_TEST_CHECK(modules.data(belowFirst).onlyPassesLinkOn() && modules.table().leaderInLink(belowFirst) == first && !modules.data(belowFirst).isLinkedWithEntryPoint());
    AOT_TEST_CHECK(modules.data(belowTwo).needsLink() && !modules.data(belowTwo).onlyPassesLinkOn());
    AOT_TEST_CHECK(modules.status(belowFirst) == ModuleStatus::Unlinked && modules.status(belowTwo) == ModuleStatus::Unlinked);
    checkModuleLinks(program, first);
    AOT_TEST_CHECK(modules.status(belowFirst) == ModuleStatus::Linked && modules.status(belowTwo) == ModuleStatus::Unlinked);
    checkModuleLinks(program, second);
    AOT_TEST_CHECK(modules.status(belowTwo) == ModuleStatus::Linked);
}

template<LoadedProgramTest body>
static constexpr void (*withBarrel)() = runNativeTestWithLoadedProgram<requiredNameTests, programText, body>;

std::span<const NativeTest> requiredNameTests()
{
    static constexpr NativeTest tests[] = {
        { "required-names/absent-name-that-may-be-absent-links"_s, withBarrel<checkAbsentNameThatMayBeAbsentLinks> },
        { "required-names/absent-name-fails-link"_s, withBarrel<checkAbsentNameFailsLink> },
        { "required-names/absent-name-of-export-statement-fails-link"_s, withBarrel<checkAbsentNameOfExportStatementFailsLink> },
        { "required-names/ambiguous-name-of-export-statement-fails-link"_s, withBarrel<checkAmbiguousNameOfExportStatementFailsLink> },
        { "required-names/ambiguous-name-of-export-statement-that-may-be-absent-fails-link"_s, withBarrel<checkAmbiguousNameOfExportStatementThatMayBeAbsentFailsLink> },
        { "required-names/ambiguous-name-that-a-star-asks-for-links"_s, withBarrel<checkAmbiguousNameThatStarAsksForLinks> },
        { "required-names/found-name-links"_s, withBarrel<checkFoundNameLinks> },
        { "required-names/modules-that-pass-the-link-on-are-linked-with-what-leads-to-them"_s, runNativeTestWithLoadedProgram<requiredNameTests, programOfModulesThatPassLinkOn, checkModulesThatPassLinkOnAreLinkedWithWhatLeadsToThem> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
