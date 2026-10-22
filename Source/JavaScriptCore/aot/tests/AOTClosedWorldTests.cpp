/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTNativeTest.h"
#include "AOTRealmStartup.h"
#include "AOTShellProgramTestSupport.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> closedWorldTests();

using Flags = OptionSet<ImageFlag>;

static constexpr Flags closed { };
// Text may ask process.getBuiltinModule for a module of the library under a name that nobody knows. No file of the program answers to such a name: closed-world/modules/evaluated-text-finds-no-module-by-name.
static constexpr Flags runsText { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::LoadsModulesByName };
// A module that stays outside the image has require and every module of the library. It sees no module object of the program and replaces no answer of the loader.
static constexpr Flags runsModuleFromOutside { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::LoadsModulesByName, ImageFlag::AdmitsLibraryText };
static constexpr Flags findsModulesByName { ImageFlag::LoadsModulesByName };
static constexpr Flags showsWaysBetweenModuleObjects { ImageFlag::ShowsWaysBetweenModuleObjects };
static constexpr Flags open { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::LoadsModulesByName, ImageFlag::AdmitsLibraryText };

#define AOT_APPLY_EVALUATOR_TEXT R"IR(function f1 name="apply" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = PropGet v1 key="text" [isFreeName]
    Jump -> next b1()
  b1() handler=b3():
    v4 = Call v0, -, v3
    v5 = TypeOf v4
    v6 = Call v2, -, v5
    Jump -> next b2()
  b2():
    Return
  b3() role=CatchHandler:
    v7 = CatchValue
    v8 = PropGet v7 key="name"
    v9 = Call v2, -, v8
    v10 = PropGet v7 key="message"
    v11 = Call v2, -, v10
    Return
}
)IR"

static constexpr ASCIILiteral evaluatorsFromUnknownCode = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="take" [isFreeName]
    v2 = Call v1
    v3 = CallDirect v2 function=f1
    v4 = Call v1
    v5 = CallDirect v4 function=f1
    v6 = Call v1
    v7 = CallDirect v6 function=f1
    v8 = Call v1
    v9 = CallDirect v8 function=f1
    v10 = Call v1
    v11 = CallDirect v10 function=f1
    Return
}
)IR" AOT_APPLY_EVALUATOR_TEXT ""_s;

static constexpr ASCIILiteral evaluatorsFromUnknownCodeAndSeenEval = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="take" [isFreeName]
    v2 = Call v1
    v3 = CallDirect v2 function=f1
    v4 = Call v1
    v5 = CallDirect v4 function=f1
    v6 = Call v1
    v7 = CallDirect v6 function=f1
    v8 = Call v1
    v9 = CallDirect v8 function=f1
    v10 = Call v1
    v11 = CallDirect v10 function=f1
    v12 = PropGet v0 key="eval" [isFreeName]
    v13 = PropGet v0 key="text" [isFreeName]
    v14 = Call v12, -, v13
    v15 = PropGet v0 key="print" [isFreeName]
    v16 = Call v15, -, v14
    Return
}
)IR" AOT_APPLY_EVALUATOR_TEXT ""_s;

static constexpr ASCIILiteral evaluatorUnderUnknownKey = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="holder" [isFreeName]
    v2 = PropGet v0 key="name" [isFreeName]
    v3 = ElemGet v1, v2
    v4 = CallDirect v3 function=f1
    Return
}
)IR" AOT_APPLY_EVALUATOR_TEXT ""_s;

static constexpr ASCIILiteral evaluatorUnderUnknownKeyOfGlobalObject = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="name" [isFreeName]
    v2 = ElemGet v0, v1
    v3 = CallDirect v2 function=f1
    Return
}
)IR" AOT_APPLY_EVALUATOR_TEXT ""_s;

static constexpr ASCIILiteral evaluatorComesBackFromUnknownCode = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="keep" [isFreeName]
    v2 = PropGet v0 key="eval" [isFreeName]
    v3 = Call v1, -, v2
    v4 = PropGet v0 key="kept" [isFreeName]
    v5 = CallDirect v4 function=f1
    Return
}
)IR" AOT_APPLY_EVALUATOR_TEXT ""_s;

static constexpr ASCIILiteral printsSum = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = PropGet v0 key="one" [isFreeName]
    v3 = Add v2, v2
    v4 = Call v1, -, v3
    Return
}
)IR"_s;

static constexpr ASCIILiteral callsUnknownFunction = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="sink" [isFreeName]
    v2 = Call v1, -, v0
    Return
}
)IR"_s;

static constexpr ASCIILiteral evaluatesUnknownText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="text" [isFreeName]
    v2 = PropGet v0 key="eval" [isFreeName]
    v3 = Call v2, -, v1
    Return
}
)IR"_s;

static constexpr ASCIILiteral constructsFunctionFromUnknownText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="text" [isFreeName]
    v2 = Builtin builtin=Function
    v3 = Construct v2, -, v1
    Return
}
)IR"_s;

static constexpr ASCIILiteral importsUncompiledModule = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1 completes=true}]
module m1 path="/uncompiled.js" displayPath="uncompiled.js" kind=External referrer=m0
entry module=m0
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
  b0():
    Return
}
)IR"_s;

static constexpr ASCIILiteral loadsNativeAddon = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=Process
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="module" [isFreeName]
    v3 = Builtin builtin=ProcessDlopen
    v4 = String string="addon.node"
    v5 = Call v3, v0, v2, v4
    Return
}
)IR"_s;

static constexpr ASCIILiteral installsPlugin = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=Bun
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="plugin" [isFreeName]
    v3 = Builtin builtin=BunPlugin
    v4 = Call v3, v0, v2
    Return
}
)IR"_s;

static constexpr ASCIILiteral otherModuleIsNeverLoaded =R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/other.js" displayPath="other.js" body=f1
entry module=m0
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
  b0():
    Return
}
function f1 name="other" module=m1 kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = String string="other ran"
    v3 = Call v1, -, v2
    Return
}
)IR"_s;

static constexpr ASCIILiteral evaluatesTextBesideOtherModule = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/other.js" displayPath="other.js" body=f1
entry module=m0
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="text" [isFreeName]
    v2 = PropGet v0 key="eval" [isFreeName]
    v3 = Call v2, -, v1
    Return
}
function f1 name="other" module=m1 kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = String string="other ran"
    v3 = Call v1, -, v2
    Return
}
)IR"_s;

static constexpr ASCIILiteral unseenCodeMayCallLoader = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/other.js" displayPath="other.js" body=f1
entry module=m0
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="text" [isFreeName]
    v2 = PropGet v0 key="eval" [isFreeName]
    v3 = Call v2, -, v1
    v4 = PropGet v0 key="sink" [isFreeName]
    v5 = Builtin builtin=Process
    v6 = Call v4, -, v5
    v7 = PropGet v0 key="name" [isFreeName]
    v8 = ElemGet v6, v7
    v9 = Call v8, v6, v7
    Return
}
function f1 name="other" module=m1 kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = String string="other ran"
    v3 = Call v1, -, v2
    Return
}
)IR"_s;

// A name that the build did not follow finds the entry points and no other file of the image, so the other module is one.
static constexpr ASCIILiteral otherModuleIsLoadedByUnknownName = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/other.js" displayPath="other.js" body=f1
entry module=m0
entry module=m1
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="name" [isFreeName]
    v2 = RequireDynamic v1 referrer=m0
    Return
}
function f1 name="other" module=m1 kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = String string="other ran"
    v3 = Call v1, -, v2
    Return
}
)IR"_s;

#define AOT_COMMONJS_SIGNATURE R"IR(kind=CommonJsModule thisType={type=anything} results=[{type=anything}] {
  b0():
)IR"

#define AOT_MAIN_OF_TWO_COMMONJS_MODULES R"IR(module m0 path="/app/main.cjs" displayPath="main.cjs" kind=CommonJs flags=[KeepsError] body=f0 requests=[{target=m1 kind=Require completes=true}]
module m1 path="/app/other.cjs" displayPath="other.cjs" kind=CommonJs flags=[KeepsError] body=f1
entry module=m0
function f0 name="main" module=m0 )IR" AOT_COMMONJS_SIGNATURE

#define AOT_OTHER_COMMONJS_MODULE R"IR(function f1 name="other" module=m1 )IR" AOT_COMMONJS_SIGNATURE R"IR(    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = String string="other ran"
    v3 = Call v1, -, v2
    Return
}
)IR"_s

static constexpr ASCIILiteral callsRequireAsValueUnderKnownName = R"IR(module m0 path="/app/main.cjs" displayPath="main.cjs" kind=CommonJs flags=[KeepsError] body=f0
module m1 path="/app/other.cjs" displayPath="other.cjs" kind=CommonJs flags=[KeepsError] body=f1
entry module=m0
entry module=m1
function f0 name="main" module=m0 )IR" AOT_COMMONJS_SIGNATURE R"IR(    v0 = ModuleBuiltin builtin=ModuleRequire module=m0
    v1 = String string="/app/other.cjs"
    v2 = Call v0, -, v1
    Return
}
)IR" AOT_OTHER_COMMONJS_MODULE;

static constexpr ASCIILiteral requiresOtherModule = AOT_MAIN_OF_TWO_COMMONJS_MODULES R"IR(    EvaluateModule module=m1 referrer=m0 [isBare]
    Return
}
)IR" AOT_OTHER_COMMONJS_MODULE;

static constexpr ASCIILiteral readsModuleCache = AOT_MAIN_OF_TWO_COMMONJS_MODULES R"IR(    EvaluateModule module=m1 referrer=m0 [isBare]
    v1 = ModuleBuiltin builtin=ModuleRequire module=m0
    v2 = PropGet v1 key="cache"
    v3 = String string="/app/other.cjs"
    v4 = ElemGet v2, v3
    Return
}
)IR" AOT_OTHER_COMMONJS_MODULE;

static constexpr ASCIILiteral storesIntoModuleCache = AOT_MAIN_OF_TWO_COMMONJS_MODULES R"IR(    v0 = ModuleBuiltin builtin=ModuleRequire module=m0
    v1 = PropGet v0 key="cache"
    v2 = String string="/app/other.cjs"
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="replacement" [isFreeName]
    ElemSet v1, v2, v4
    EvaluateModule module=m1 referrer=m0 [isBare]
    Return
}
)IR" AOT_OTHER_COMMONJS_MODULE;

static constexpr ASCIILiteral removesFromModuleCache = AOT_MAIN_OF_TWO_COMMONJS_MODULES R"IR(    EvaluateModule module=m1 referrer=m0 [isBare]
    v1 = ModuleBuiltin builtin=ModuleRequire module=m0
    v2 = PropGet v1 key="cache"
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="name" [isFreeName]
    v5 = ElemDelete v2, v4
    EvaluateModule module=m1 referrer=m0 [isBare]
    Return
}
)IR" AOT_OTHER_COMMONJS_MODULE;

#define AOT_TAKES_MAIN_MODULE_AS_VALUE(flagsOfMain) R"IR(module m0 path="/app/main.cjs" displayPath="main.cjs" kind=CommonJs flags=[)IR" flagsOfMain R"IR(] body=f0
entry module=m0
function f0 name="main" module=m0 )IR" AOT_COMMONJS_SIGNATURE R"IR(    v0 = ModuleBuiltin builtin=ModuleRequire module=m0
    v1 = PropGet v0 key="main"
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="print" [isFreeName]
    v4 = Call v3, -, v1
    Return
}
)IR"_s

static constexpr ASCIILiteral takesMainModuleAsFrontEndSays = AOT_TAKES_MAIN_MODULE_AS_VALUE("KeepsError, NamesWayToModuleObjects");
static constexpr ASCIILiteral takesMainModuleBehindFrontEnd = AOT_TAKES_MAIN_MODULE_AS_VALUE("KeepsError");

#define AOT_LIBRARY_LOADS_BY_NAME(operation) R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/other.js" displayPath="other.js" body=f1
entry module=m0
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
  b0():
    v0 = CallDirect function=f2
    Return
}
function f1 name="other" module=m1 kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = String string="other ran"
    v3 = Call v1, -, v2
    Return
}
function f2 name="load" module=m0 role=Library results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="name" [isFreeName]
    Jump -> next b1()
  b1() handler=b3():
    v3 = )IR" operation R"IR( v2 referrer=m0
    v4 = Call v1, -, v3
    Jump -> next b2()
  b2():
    Return
  b3() role=CatchHandler:
    v5 = CatchValue
    v6 = Call v1, -, v5
    Return
}
)IR"_s

static constexpr ASCIILiteral libraryRequiresUnknownName = AOT_LIBRARY_LOADS_BY_NAME("RequireDynamic");
static constexpr ASCIILiteral libraryImportsUnknownName = AOT_LIBRARY_LOADS_BY_NAME("ImportDynamic");

static constexpr ASCIILiteral readsGlobalsAfterScripts = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="eval" [isFreeName]
    v2 = PropGet v0 key="text" [isFreeName]
    v3 = Call v1, -, v2
    v4 = PropGet v0 key="print" [isFreeName]
    v5 = PropGet v0 key="Math" [isFreeName]
    v6 = PropGet v5 key="max"
    v7 = Integer integer=1
    v8 = Integer integer=2
    v9 = Call v6, v5, v7, v8
    v10 = Call v4, -, v9
    v11 = Builtin builtin=Math
    v12 = PropGet v11 key="max"
    v13 = Call v12, v11, v7, v8
    v14 = Call v4, -, v13
    v15 = PropGet v0 key="parseInt" [isFreeName]
    v16 = String string="12"
    v17 = Call v15, -, v16
    v18 = PropGet v0 key="reportParsed" [isFreeName]
    v19 = Call v18, -, v17
    v20 = PropGet v0 key="declaredByScript" [isFreeName]
    v21 = Call v4, -, v20
    v22 = PropGet v0 key="neverDeclared" [isFreeName, isForTypeOf]
    v23 = TypeOf v22
    v24 = Call v4, -, v23
    Return
}
)IR"_s;

static constexpr ASCIILiteral everyEvaluator = "globalThis.text = '1 + 1'; const evaluators = [eval, Function, (async function () { }).constructor, (function* () { }).constructor, (async function* () { }).constructor]; globalThis.take = () => evaluators.shift();"_s;

static Flags flagsOf(ASCIILiteral programText, OptionWords optionWords = { })
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileForShellOnThisThread(programText, optionWords);
    });
    std::optional<Flags> flags = Image::flags(output->image.span());
    AOT_TEST_CHECK(flags);
    return *flags;
}

static String refusal(unsigned count)
{
    StringBuilder text;
    for (unsigned index = 0; index < count; ++index)
        text.append("EvalError\n"_s, evaluationRefusedMessage(), '\n');
    return text.toString();
}

static void checkShellOutput(ASCIILiteral programText, const String& expectedOutput, ASCIILiteral statementsBeforeProgram, OptionWords optionWords = { })
{
    String output = outputOfProgramInShell(programText, optionWords, statementsBeforeProgram);
    if (output != expectedOutput)
        dataLog("the shell printed\n", output, "expected\n", expectedOutput);
    AOT_TEST_CHECK(output == expectedOutput);
}

static void testProgramWithoutEventIsClosed()
{
    AOT_TEST_CHECK(flagsOf(printsSum) == closed);
    AOT_TEST_CHECK(flagsOf(otherModuleIsNeverLoaded) == closed);
    // An addon is native code.
    AOT_TEST_CHECK(flagsOf(loadsNativeAddon) == closed);
}

static void testReachableEvaluatorLeavesTheProgramClosed()
{
    AOT_TEST_CHECK(flagsOf(callsUnknownFunction) == closed);
    AOT_TEST_CHECK(flagsOf(evaluatorsFromUnknownCode) == closed);
    AOT_TEST_CHECK(flagsOf(evaluatorUnderUnknownKey) == closed);
    AOT_TEST_CHECK(flagsOf(evaluatorComesBackFromUnknownCode) == closed);
    AOT_TEST_CHECK(flagsOf(evaluatorUnderUnknownKeyOfGlobalObject) == closed);
}

static void testCallThatStartsUnseenCodeAdmitsNothingElse()
{
    AOT_TEST_CHECK(flagsOf(evaluatesUnknownText) == runsText);
    AOT_TEST_CHECK(flagsOf(constructsFunctionFromUnknownText) == runsText);
    AOT_TEST_CHECK(flagsOf(installsPlugin) == runsText);
    AOT_TEST_CHECK(flagsOf(evaluatesTextBesideOtherModule) == runsText);
    AOT_TEST_CHECK(flagsOf(unseenCodeMayCallLoader) == runsText);
}

static void testUncompiledModuleOpensTheProgram()
{
    AOT_TEST_CHECK(flagsOf(importsUncompiledModule) == runsModuleFromOutside);
}

static void testOptionThatAdmitsCodeOpensTheProgram()
{
    AOT_TEST_CHECK(flagsOf(printsSum, { "admit=Inspector"_s }) == open);
}

static void testLoadByUnknownNameOnlyExposesModules()
{
    AOT_TEST_CHECK(flagsOf(otherModuleIsLoadedByUnknownName) == findsModulesByName);
    AOT_TEST_CHECK(flagsOf(otherModuleIsLoadedByUnknownName, { "admit=Inspector"_s }) == open);
}

static void testLoaderCalledAsValueLoadsByName()
{
    AOT_TEST_CHECK(flagsOf(callsRequireAsValueUnderKnownName) == findsModulesByName);
    checkShellOutput(callsRequireAsValueUnderKnownName, "other ran\n"_s, ""_s);
}

static void testLoadByNameInLibraryFunctionLeavesTheProgramClosed()
{
    AOT_TEST_CHECK(flagsOf(libraryRequiresUnknownName) == closed);
    AOT_TEST_CHECK(flagsOf(libraryImportsUnknownName) == closed);
}

// No module of the image is in the module cache, so nothing that is done to the cache changes what the code counts on.
static void testWhatIsDoneToModuleCacheLeavesTheProgramClosed()
{
    AOT_TEST_CHECK(flagsOf(requiresOtherModule) == closed);
    AOT_TEST_CHECK(flagsOf(readsModuleCache) == closed);
    AOT_TEST_CHECK(flagsOf(removesFromModuleCache) == closed);
    AOT_TEST_CHECK(flagsOf(storesIntoModuleCache) == closed);
}

// What a program can observe never hangs on what flow has reached or lost: the front end says by binding whether a file names a way between module objects, and nobody else is asked.
static void testFrontEndAloneSaysWhetherWaysBetweenModuleObjectsAreShown()
{
    AOT_TEST_CHECK(flagsOf(takesMainModuleAsFrontEndSays) == showsWaysBetweenModuleObjects);
    AOT_TEST_CHECK(flagsOf(takesMainModuleBehindFrontEnd) == closed);
    AOT_TEST_CHECK(flagsOf(takesMainModuleAsFrontEndSays, { "disable=flow-results"_s }) == (open | showsWaysBetweenModuleObjects));
}

static void testExcludedTextLeavesTheProgramClosed()
{
    AOT_TEST_CHECK(flagsOf(evaluatesUnknownText, { "exclude=EvaluatedText"_s }) == closed);
    AOT_TEST_CHECK(flagsOf(installsPlugin, { "exclude=EvaluatedText"_s }) == runsText);
}

static void testLossesOfTheAnalysisAdmitNothing()
{
    AOT_TEST_CHECK(flagsOf(printsSum, { "disable=flow:ReachedFunctions"_s }) == closed);
    AOT_TEST_CHECK(flagsOf(evaluatesUnknownText, { "disable=flow:ReachedFunctions"_s }) == runsText);
}

static void testProgramThatWasNotAnalyzedIsOpen()
{
    AOT_TEST_CHECK(flagsOf(printsSum, { "disable=flow-results"_s }) == open);
}

static void testObfuscationIsAnotherFlag()
{
    AOT_TEST_CHECK(flagsOf(printsSum, { "obfuscate"_s }) == Flags { ImageFlag::IsObfuscated });
}

static void testClosedProgramRefusesEveryEvaluator()
{
    checkShellOutput(evaluatorsFromUnknownCode, refusal(5), everyEvaluator);
    checkShellOutput(evaluatorsFromUnknownCode, refusal(5), everyEvaluator, { "validate"_s });
}

static void testProgramThatEvaluatesTextKeepsEveryEvaluator()
{
    checkShellOutput(evaluatorsFromUnknownCodeAndSeenEval, "number\nfunction\nfunction\nfunction\nfunction\n2\n"_s, everyEvaluator);
}

static void testOptionKeepsEveryEvaluator()
{
    checkShellOutput(evaluatorsFromUnknownCode, "number\nfunction\nfunction\nfunction\nfunction\n"_s, everyEvaluator, { "admit=Inspector"_s });
}

static void testEvaluatorUnderUnknownKeyIsRefused()
{
    checkShellOutput(evaluatorUnderUnknownKey, refusal(1), "globalThis.text = '1 + 1'; globalThis.holder = globalThis; globalThis.name = 'ev' + 'al';"_s);
}

static void testEvaluatorThatComesBackFromUnknownCodeIsRefused()
{
    checkShellOutput(evaluatorComesBackFromUnknownCode, refusal(1), "globalThis.text = '1 + 1'; globalThis.keep = value => { globalThis.kept = value; };"_s);
}

static void testEveryRealmOfClosedProgramRefusesEvaluation()
{
    static constexpr ASCIILiteral evaluateInEveryRealm = "globalThis.one = 1; for (const realm of [globalThis, createGlobalObject()]) { for (const evaluate of [realm.eval, realm.Function, realm.eval('(async function () { }).constructor')]) { try { print(typeof evaluate('1 + 1')); } catch (error) { print(error.name); print(error.message); } } }"_s;
    static constexpr ASCIILiteral evaluateWithoutNestedEvaluation = "globalThis.one = 1; for (const realm of [globalThis, createGlobalObject()]) { for (const evaluate of [realm.eval, realm.Function]) { try { print(typeof evaluate('1 + 1')); } catch (error) { print(error.name); print(error.message); } } }"_s;
    checkShellOutput(printsSum, makeString(refusal(4), "2\n"_s), evaluateWithoutNestedEvaluation);
    checkShellOutput(printsSum, "number\nfunction\nfunction\nnumber\nfunction\nfunction\n2\n"_s, evaluateInEveryRealm, { "admit=Inspector"_s });
    checkShellOutput(printsSum, "number\nfunction\nfunction\nnumber\nfunction\nfunction\n2\n"_s, evaluateInEveryRealm, { "disable=flow-results"_s });
}

#define AOT_DECLARATION_REFUSED(name) "SyntaxError: Cannot declare '" name "': compiled code treats the global '" name "' as a constant, so a script can't shadow it with a top-level let, const or class (bun compile differs from bun run here: https://bun.com/docs/bundler/aot-compatibility)\n"

static void testScriptShadowsNoConstantOfCompiledCode()
{
    // Whether parseInt is a constant is up to the analysis. Either the declaration is refused or compiled code sees the binding.
    static constexpr ASCIILiteral declareInScripts = "globalThis.errorOf = script => { try { loadString(script); return null; } catch (error) { return error; } };"
        " globalThis.declare = script => { const error = errorOf(script); if (error) print(error); };"
        " globalThis.text = `declare('let Math = { max: () => \"of the script\" }'); declare('const Math = 1'); declare('class Math { }'); declare('let globalThis = 1');"
        " globalThis.isParseIntDeclared = !errorOf('let parseInt = () => \"of the script\"'); declare('let declaredByScript = \"of the script\"'); declare('print(declaredByScript)');`;"
        " globalThis.reportParsed = value => print(value === (isParseIntDeclared ? 'of the script' : 12) ? 'as declared' : 'not as declared');"_s;
    static constexpr ASCIILiteral expectedOutput = AOT_DECLARATION_REFUSED("Math") AOT_DECLARATION_REFUSED("Math") AOT_DECLARATION_REFUSED("Math") AOT_DECLARATION_REFUSED("globalThis") "of the script\n2\n2\nas declared\nof the script\nundefined\n"_s;
    checkShellOutput(readsGlobalsAfterScripts, expectedOutput, declareInScripts);
    checkShellOutput(readsGlobalsAfterScripts, expectedOutput, declareInScripts, { "admit=Inspector"_s });
    checkShellOutput(readsGlobalsAfterScripts, expectedOutput, declareInScripts, { "disable=flow-results"_s });
    checkShellOutput(readsGlobalsAfterScripts, expectedOutput, declareInScripts, { "validate"_s });
}

#define AOT_LOAD_BY_NAME_REFUSED "Cannot load '/other.js': this executable can't load a module from a dynamic path. Pass the file to bun compile as another entry point. (bun compile differs from bun run here: https://bun.com/docs/bundler/aot-compatibility)\n"

#define AOT_LOADER_CHANGE_REFUSED ": imports in this executable were resolved at build time, so the module loader can't be patched. (bun compile differs from bun run here: https://bun.com/docs/bundler/aot-compatibility)\n"

static constexpr ASCIILiteral runOtherModule = "globalThis.name = '/other.js'; try { runCompiledEntryPoint(2); drainMicrotasks(); } catch (error) { print(error.message); }"_s;

static void testClosedProgramFindsNoModuleByName()
{
    checkShellOutput(otherModuleIsNeverLoaded, AOT_LOAD_BY_NAME_REFUSED ""_s, runOtherModule);
}

static void testEvaluatedTextFindsNoModuleByName()
{
    static constexpr ASCIILiteral textThatLoads = "globalThis.text = `for (const change of [() => runCompiledEntryPoint(2), () => overrideRequire(true)]) { try { change(); print('done'); } catch (error) { print(error.message); } }`;"_s;
    checkShellOutput(evaluatesTextBesideOtherModule, AOT_LOAD_BY_NAME_REFUSED "done\n"_s, textThatLoads);
}

static void testProgramThatLoadsByNameFindsEntryPoints()
{
    checkShellOutput(otherModuleIsLoadedByUnknownName, "other ran\n"_s, runOtherModule);
    checkShellOutput(otherModuleIsLoadedByUnknownName, "other ran\n"_s, "globalThis.name = '/other.js';"_s);
    checkShellOutput(otherModuleIsNeverLoaded, "other ran\n"_s, runOtherModule, { "admit=Inspector"_s });
}

static constexpr ASCIILiteral reportLoad = "globalThis.name = '/other.js'; globalThis.report = value => { if (value instanceof Promise) value.then(() => print('imported'), error => print(error.message)); else print(value instanceof Error ? value.message : 'required'); };"_s;

static void testLibraryFunctionOfClosedProgramFindsNoModuleByName()
{
    checkShellOutput(libraryRequiresUnknownName, AOT_LOAD_BY_NAME_REFUSED ""_s, reportLoad);
    checkShellOutput(libraryImportsUnknownName, AOT_LOAD_BY_NAME_REFUSED ""_s, reportLoad);
    checkShellOutput(libraryRequiresUnknownName, "other ran\nrequired\n"_s, reportLoad, { "admit=Inspector"_s });
}

static constexpr ASCIILiteral changeLoader = "for (const change of [() => overrideRequire(true), () => setRequireCacheEntry(2, { })]) { try { change(); print('changed'); } catch (error) { print(error.message); } } overrideRequire(false);"_s;

// The code calls the module that each constant name stood for in the build, whatever else the image admits and whatever function the program has put in the loader's place.
static void testEveryImageTakesHookAndRefusesEntryOfCache()
{
    static constexpr ASCIILiteral refusals = "changed\nCannot replace '/app/other.cjs' in require.cache" AOT_LOADER_CHANGE_REFUSED "other ran\n"_s;
    checkShellOutput(requiresOtherModule, refusals, changeLoader);
    checkShellOutput(requiresOtherModule, refusals, changeLoader, { "admit=Inspector"_s });
    checkShellOutput(otherModuleIsLoadedByUnknownName, "other ran\n"_s, "globalThis.name = '/other.js'; try { overrideRequire(true); } catch (error) { print(error.message); }"_s);
}

std::span<const NativeTest> closedWorldTests()
{
    static constexpr NativeTest tests[] {
        { "closed-world/flags/program-without-event-is-closed"_s, testProgramWithoutEventIsClosed },
        { "closed-world/flags/reachable-evaluator-leaves-the-program-closed"_s, testReachableEvaluatorLeavesTheProgramClosed },
        { "closed-world/flags/call-that-starts-unseen-code-admits-nothing-else"_s, testCallThatStartsUnseenCodeAdmitsNothingElse },
        { "closed-world/flags/uncompiled-module-opens-the-program"_s, testUncompiledModuleOpensTheProgram },
        { "closed-world/flags/option-that-admits-code-opens-the-program"_s, testOptionThatAdmitsCodeOpensTheProgram },
        { "closed-world/flags/load-by-unknown-name-only-exposes-modules"_s, testLoadByUnknownNameOnlyExposesModules },
        { "closed-world/flags/loader-called-as-value-loads-by-name"_s, testLoaderCalledAsValueLoadsByName },
        { "closed-world/flags/load-by-name-in-library-function-leaves-the-program-closed"_s, testLoadByNameInLibraryFunctionLeavesTheProgramClosed },
        { "closed-world/flags/what-is-done-to-module-cache-leaves-the-program-closed"_s, testWhatIsDoneToModuleCacheLeavesTheProgramClosed },
        { "closed-world/flags/front-end-alone-says-whether-ways-between-module-objects-are-shown"_s, testFrontEndAloneSaysWhetherWaysBetweenModuleObjectsAreShown },
        { "closed-world/flags/excluded-text-leaves-the-program-closed"_s, testExcludedTextLeavesTheProgramClosed },
        { "closed-world/flags/losses-of-the-analysis-admit-nothing"_s, testLossesOfTheAnalysisAdmitNothing },
        { "closed-world/flags/program-that-was-not-analyzed-is-open"_s, testProgramThatWasNotAnalyzedIsOpen },
        { "closed-world/flags/obfuscation-is-another-flag"_s, testObfuscationIsAnotherFlag },
        { "closed-world/evaluation/closed-program-refuses-every-evaluator"_s, testClosedProgramRefusesEveryEvaluator },
        { "closed-world/evaluation/program-that-evaluates-text-keeps-every-evaluator"_s, testProgramThatEvaluatesTextKeepsEveryEvaluator },
        { "closed-world/evaluation/option-keeps-every-evaluator"_s, testOptionKeepsEveryEvaluator },
        { "closed-world/evaluation/evaluator-under-unknown-key-is-refused"_s, testEvaluatorUnderUnknownKeyIsRefused },
        { "closed-world/evaluation/evaluator-that-comes-back-from-unknown-code-is-refused"_s, testEvaluatorThatComesBackFromUnknownCodeIsRefused },
        { "closed-world/evaluation/every-realm-of-closed-program-refuses-evaluation"_s, testEveryRealmOfClosedProgramRefusesEvaluation },
        { "closed-world/declarations/script-shadows-no-constant-of-compiled-code"_s, testScriptShadowsNoConstantOfCompiledCode },
        { "closed-world/modules/closed-program-finds-no-module-by-name"_s, testClosedProgramFindsNoModuleByName },
        { "closed-world/modules/evaluated-text-finds-no-module-by-name"_s, testEvaluatedTextFindsNoModuleByName },
        { "closed-world/modules/program-that-loads-by-name-finds-entry-points"_s, testProgramThatLoadsByNameFindsEntryPoints },
        { "closed-world/modules/library-function-of-closed-program-finds-no-module-by-name"_s, testLibraryFunctionOfClosedProgramFindsNoModuleByName },
        { "closed-world/modules/every-image-takes-a-hook-and-refuses-an-entry-of-the-cache"_s, testEveryImageTakesHookAndRefusesEntryOfCache },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
