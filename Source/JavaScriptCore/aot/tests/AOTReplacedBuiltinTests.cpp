/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCompileOptionWords.h"
#include "AOTDriver.h"
#include "AOTIRParser.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTOptionWords.h"
#include "AOTProgramFile.h"
#include "AOTProgramTables.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTTestHarness.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> replacedBuiltinTests();

static constexpr ASCIILiteral patchBeforeFirstUse = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="replacement" [isFreeName]
    v3 = PropGet v0 key="Math" [isFreeName]
    PropSet v3, v2 key="sign"
    v5 = PropGet v3 key="sign"
    v6 = Integer integer=1
    v7 = Integer integer=2
    v8 = Call v5, v3, v6, v7 calleeKey="sign"
    v9 = Call v1, -, v8
    Return
}
)IR"_s;

static constexpr ASCIILiteral patchInModuleImportedFirst = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1 completes=true}]
module m1 path="/polyfill.js" displayPath="polyfill.js" body=f1
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="Math" [isFreeName]
    v3 = PropGet v2 key="sign"
    v4 = Integer integer=1
    v5 = Integer integer=2
    v6 = Call v3, v2, v4, v5 calleeKey="sign"
    v7 = Call v1, -, v6
    Return
}
function f1 name="polyfill" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="replacement" [isFreeName]
    v2 = PropGet v0 key="Math" [isFreeName]
    PropSet v2, v1 key="sign"
    Return
}
)IR"_s;

static constexpr ASCIILiteral patchAfterFirstUse = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="replacement" [isFreeName]
    v3 = PropGet v0 key="Math" [isFreeName]
    v4 = PropGet v3 key="sign"
    v5 = Integer integer=1
    v6 = Integer integer=2
    v7 = Call v4, v3, v5, v6 calleeKey="sign"
    v8 = Call v1, -, v7
    PropSet v3, v2 key="sign"
    v10 = PropGet v3 key="sign"
    v11 = Call v10, v3, v5, v6 calleeKey="sign"
    v12 = Call v1, -, v11
    Return
}
)IR"_s;

static constexpr ASCIILiteral originalKeptThenDeleted = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="Math" [isFreeName]
    v3 = PropGet v2 key="sign"
    v4 = PropDelete v2 key="sign"
    v5 = Integer integer=1
    v6 = Integer integer=2
    v7 = Call v3, v2, v5, v6
    v8 = Call v1, -, v7
    v9 = PropGet v2 key="sign"
    v10 = Call v1, -, v9
    Jump -> next b1()
  b1() handler=b3():
    v11 = PropGet v2 key="sign"
    v12 = Call v11, v2, v5, v6 calleeKey="sign"
    v13 = Call v1, -, v12
    Jump -> next b2()
  b2():
    Return
  b3() role=CatchHandler:
    v14 = CatchValue
    v15 = String string="caught"
    v16 = Call v1, -, v15
    Return
}
)IR"_s;

static constexpr ASCIILiteral wrappedJSONParse = R"IR(variable g0 name="original" module=m0 declared={type=anything} initialization=Undefined
module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="JSON" [isFreeName]
    v3 = PropGet v2 key="parse"
    PutModuleVariable v3 variable=g0
    v5 = NewClosure function=f1
    PropSet v2, v5 key="parse"
    v7 = PropGet v2 key="parse"
    v8 = String string="7"
    v9 = Call v7, v2, v8 calleeKey="parse"
    v10 = Call v1, -, v9
    Return
}
function f1 name="wrapper" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="report" [isFreeName]
    v3 = String string="wrapped"
    v4 = Call v2, -, v3
    v5 = GetModuleVariable variable=g0
    v6 = Call v5, -, v0
    Return v6
}
)IR"_s;

static constexpr ASCIILiteral originalNamesAfterReplacement = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="replacement" [isFreeName]
    v3 = PropGet v0 key="JSON" [isFreeName]
    v4 = PropGet v3 key="parse"
    PropSet v3, v2 key="parse"
    Jump -> next b1()
  b1() handler=b3():
    v6 = Undefined
    v7 = Call v4, v3, v6
    v8 = Call v1, -, v7
    Jump -> next b2()
  b2():
    Return
  b3() role=CatchHandler:
    v9 = CatchValue
    v10 = PropGet v9 key="name"
    v11 = String string="caught-"
    v12 = Add v11, v10
    v13 = Call v1, -, v12
    Return
}
)IR"_s;

static constexpr ASCIILiteral replacedGlobal = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="fakeDate" [isFreeName]
    v3 = PropGet v0 key="Date" [isFreeName]
    PropSet v0, v2 key="Date" [isFreeName]
    v5 = PropGet v0 key="Date" [isFreeName]
    v6 = PropGet v5 key="now"
    v7 = Call v6, v5 calleeKey="now"
    v8 = Call v1, -, v7
    v9 = PropGet v3 key="now"
    v10 = Call v9, v3 calleeKey="now"
    v11 = TypeOf v10
    v12 = Call v1, -, v11
    Return
}
)IR"_s;

static constexpr ASCIILiteral replacedDateNow = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="replacement" [isFreeName]
    v3 = PropGet v0 key="Date" [isFreeName]
    v4 = PropGet v3 key="now"
    PropSet v3, v2 key="now"
    v6 = Call v4, v3
    v7 = TypeOf v6
    v8 = Call v1, -, v7
    v9 = PropGet v3 key="now"
    v10 = Call v9, v3 calleeKey="now"
    v11 = Call v1, -, v10
    Return
}
)IR"_s;

static constexpr ASCIILiteral storeUnderUnknownKey = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="replacement" [isFreeName]
    v3 = PropGet v0 key="name" [isFreeName]
    v4 = PropGet v0 key="Math" [isFreeName]
    ElemSet v4, v3, v2
    v6 = PropGet v4 key="sign"
    v7 = Integer integer=1
    v8 = Integer integer=2
    v9 = Call v6, v4, v7, v8 calleeKey="sign"
    v10 = Call v1, -, v9
    v11 = PropGet v4 key="min"
    v12 = Call v11, v4, v7, v8 calleeKey="min"
    v13 = Call v1, -, v12
    Return
}
)IR"_s;

static constexpr ASCIILiteral replacedByBuiltinCall = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Patch" fields=[{key="sign" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="replacement" [isFreeName]
    v3 = PropGet v0 key="Math" [isFreeName]
    v4 = NewObject -, v2 layout=L0 fields=[0]
    v5 = FinishObject v4
    v6 = PropGet v0 key="Object" [isFreeName]
    v7 = PropGet v6 key="assign"
    v8 = Call v7, v6, v3, v5 calleeKey="assign"
    v9 = PropGet v3 key="sign"
    v10 = Integer integer=1
    v11 = Integer integer=2
    v12 = Call v9, v3, v10, v11 calleeKey="sign"
    v13 = Call v1, -, v12
    Return
}
)IR"_s;

static constexpr ASCIILiteral replacedGetter = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Descriptor" fields=[{key="get" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="Intl" [isFreeName]
    v3 = PropGet v2 key="Collator"
    v4 = PropGet v3 key="prototype"
    v5 = Construct v3
    v6 = PropGet v5 key="compare"
    v7 = TypeOf v6
    v8 = Call v1, -, v7
    v9 = NewClosure function=f1
    v10 = NewObject -, v9 layout=L0 fields=[0]
    v11 = FinishObject v10
    v12 = PropGet v0 key="Object" [isFreeName]
    v13 = PropGet v12 key="defineProperty"
    v14 = String string="compare"
    v15 = Call v13, v12, v4, v14, v11 calleeKey="defineProperty"
    v16 = PropGet v5 key="compare"
    v17 = Call v1, -, v16
    Return
}
function f1 name="compare" results=[{type=anything}] {
    v0 = String string="replaced"
    Return v0
}
)IR"_s;

static constexpr ASCIILiteral builtinCalledOnce = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="Math" [isFreeName]
    v3 = PropGet v2 key="sign"
    v4 = Integer integer=1
    v5 = Integer integer=2
    v6 = Call v3, v2, v4, v5 calleeKey="sign"
    v7 = Call v1, -, v6
    Return
}
)IR"_s;

static constexpr ASCIILiteral scriptStart = R"js(
globalThis.report = value => print(String(value));
globalThis.replacement = () => "replaced";
globalThis.fakeDate = { now: () => "replaced-now" };
globalThis.name = "sign";
)js"_s;

static std::unique_ptr<CompileOutput> compile(ASCIILiteral programText, const String& optionWords, ASCIILiteral way)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        for (StringView word : StringView(optionWords).split(' '))
            AOT_TEST_CHECK(!applyOptionWord(options, word));
        BuildContext build(options);
        std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), build);
        if (!program) {
            build.diagnostics().finish(nullptr);
            Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
        }
        AOT_TEST_CHECK(program);
        output = Typed::compileWithMechanisms(WTF::move(program), build, Typed::mechanismsOfWay(way));
        if (!output->hasImage())
            Typed::printDiagnosticsOf(*output, WTF::dataFile());
        AOT_TEST_CHECK(output->hasImage() && output->records<uint32_t>(Wire::ResultSection::EntryModuleIds).size() == 1);
    });
    return output;
}

struct Run {
    ASCIILiteral programText;
    ASCIILiteral optionWords { ""_s };
    ASCIILiteral statementsBeforeProgram { ""_s };
};

static constexpr ASCIILiteral wideReferencesWord = "references=wide"_s;
static constexpr ASCIILiteral narrowReferencesWord = "references=narrow"_s;

static String runInShell(const Run& run, ASCIILiteral referencesWord, ASCIILiteral way)
{
    std::unique_ptr<CompileOutput> output = compile(run.programText, makeString(run.optionWords, ' ', referencesWord), way);
    TemporaryFile image(".image"_s, output->section(Wire::ResultSection::Image));
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(scriptStart, run.statementsBeforeProgram, "runCompiledEntryPoint("_s, output->records<uint32_t>(Wire::ResultSection::EntryModuleIds)[0], ").catch(error => print(\"the program threw \" + error)); drainMicrotasks();"_s),
    };
    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

static void checkShellOutput(const Run& run, ASCIILiteral expectedOutput)
{
    auto check = [&](ASCIILiteral referencesWord, ASCIILiteral way) {
        String output = runInShell(run, referencesWord, way);
        if (output != expectedOutput)
            dataLog("with ", referencesWord, " ", way, " the shell printed\n", output, "expected\n", expectedOutput);
        AOT_TEST_CHECK(output == expectedOutput);
    };
    check(wideReferencesWord, ""_s);
    for (ASCIILiteral way : waysToCompilePrograms)
        check(narrowReferencesWord, way);
}

static void testPatchBeforeFirstUse()
{
    checkShellOutput({ patchBeforeFirstUse }, "replaced\n"_s);
}

static void testPatchInModuleImportedFirst()
{
    checkShellOutput({ patchInModuleImportedFirst }, "replaced\n"_s);
}

static void testPatchAfterFirstUse()
{
    checkShellOutput({ patchAfterFirstUse }, "1\nreplaced\n"_s);
}

static void testOriginalKeptThenDeleted()
{
    checkShellOutput({ originalKeptThenDeleted }, "1\nundefined\ncaught\n"_s);
}

static void testWrapperCallsOriginal()
{
    checkShellOutput({ wrappedJSONParse }, "wrapped\n7\n"_s);
}

static void testOriginalThrowsItsOwnError()
{
    checkShellOutput({ originalNamesAfterReplacement }, "caught-SyntaxError\n"_s);
}

static void testReplacedGlobal()
{
    checkShellOutput({ replacedGlobal }, "replaced-now\nnumber\n"_s);
}

static void testReplacedMemberOfConstructor()
{
    checkShellOutput({ replacedDateNow }, "number\nreplaced\n"_s);
}

static void testStoreUnderUnknownKey()
{
    checkShellOutput({ storeUnderUnknownKey }, "replaced\n1\n"_s);
}

static void testReplacedByBuiltinCall()
{
    checkShellOutput({ replacedByBuiltinCall }, "replaced\n"_s);
}

static void testReplacedGetter()
{
    checkShellOutput({ replacedGetter }, "function\nreplaced\n"_s);
}

static void testUncompiledPolyfillRunsFirst()
{
    auto check = [](ASCIILiteral statementsBeforeProgram, ASCIILiteral expectedOutput) {
        checkShellOutput({ builtinCalledOnce, "admit=Inspector"_s, statementsBeforeProgram }, expectedOutput);
    };
    check("Math.sign = () => 'polyfill';"_s, "polyfill\n"_s);
    check("delete Math.sign; Math.sign = () => 'polyfill';"_s, "polyfill\n"_s);
    check("try { globalThis.Math = { sign: () => 'polyfill' }; } catch { }"_s, "1\n"_s);
    check(""_s, "1\n"_s);
}

static void testWriteThatFlowDoesNotExpect()
{
    checkShellOutput({ builtinCalledOnce, ""_s, "try { Math.sign = () => 'polyfill'; } catch { }"_s }, "1\n"_s);
}

std::span<const NativeTest> replacedBuiltinTests()
{
    static constexpr NativeTest tests[] = {
        { "replaced-builtins/patch-before-first-use"_s, testPatchBeforeFirstUse },
        { "replaced-builtins/patch-in-module-imported-first"_s, testPatchInModuleImportedFirst },
        { "replaced-builtins/patch-after-first-use"_s, testPatchAfterFirstUse },
        { "replaced-builtins/original-kept-then-deleted"_s, testOriginalKeptThenDeleted },
        { "replaced-builtins/wrapper-calls-original"_s, testWrapperCallsOriginal },
        { "replaced-builtins/original-throws-its-own-error"_s, testOriginalThrowsItsOwnError },
        { "replaced-builtins/replaced-global"_s, testReplacedGlobal },
        { "replaced-builtins/replaced-member-of-constructor"_s, testReplacedMemberOfConstructor },
        { "replaced-builtins/store-under-unknown-key"_s, testStoreUnderUnknownKey },
        { "replaced-builtins/replaced-by-built-in-call"_s, testReplacedByBuiltinCall },
        { "replaced-builtins/replaced-getter"_s, testReplacedGetter },
        { "replaced-builtins/uncompiled-polyfill-runs-first"_s, testUncompiledPolyfillRunsFirst },
        { "replaced-builtins/write-that-flow-does-not-expect"_s, testWriteThatFlowDoesNotExpect },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
