/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltProgramTestSupport.h"
#include "AOTBuiltinTable.h"
#include "AOTNativeTest.h"
#include "AOTRealmStartup.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTTestHarness.h"
#include <algorithm>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> closedWorldTests();

using Flags = OptionSet<ImageFlag>;

static constexpr Flags refusesOriginals { ImageFlag::RefusesOriginalsBesideMocks };
static constexpr Flags closed = refusesOriginals;
static constexpr Flags runsModuleFromOutside = refusesOriginals | Flags { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::LoadsModulesByName, ImageFlag::AllowsLibrarySource, ImageFlag::LetsGlobalObjectGo };
static constexpr Flags findsModulesByName = refusesOriginals | Flags { ImageFlag::LoadsModulesByName };
static constexpr Flags loadsCodeFromOutside = runsModuleFromOutside | Flags { ImageFlag::LoadsCodeOutsideImage };
static constexpr Flags showsWaysBetweenModuleObjects = refusesOriginals | Flags { ImageFlag::ShowsWaysBetweenModuleObjects };
static constexpr Flags open = loadsCodeFromOutside;

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
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="holder" [isFreeName]
    v2 = PropGet v0 key="name" [isFreeName]
    v3 = ElemGet v1, v2
    v4 = CallDirect v3 function=f1
    Return
}
)IR" AOT_APPLY_EVALUATOR_TEXT ""_s;

static constexpr ASCIILiteral evaluatorComesBackFromUnknownCode = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
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
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = PropGet v0 key="one" [isFreeName]
    v3 = Add v2, v2
    v4 = Call v1, -, v3
    Return
}
)IR"_s;

static constexpr ASCIILiteral importsUncompiledModule = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1 completes=true}]
module m1 path="/uncompiled.js" displayPath="uncompiled.js" kind=External referrer=m0
entry module=m0
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
    Return
}
)IR"_s;

static constexpr ASCIILiteral loadsNativeAddon = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=Process
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="module" [isFreeName]
    v3 = Builtin builtin=ProcessDlopen
    v4 = String string="addon.node"
    v5 = Call v3, v0, v2, v4
    Return
}
)IR"_s;

static constexpr ASCIILiteral otherModuleIsNeverLoaded = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/other.js" displayPath="other.js" body=f1
entry module=m0
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
    Return
}
function f1 name="other" module=m1 kind=Module results=[{type=anything}] {
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
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="text" [isFreeName]
    v2 = PropGet v0 key="eval" [isFreeName]
    v3 = Call v2, -, v1
    Return
}
function f1 name="other" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = String string="other ran"
    v3 = Call v1, -, v2
    Return
}
)IR"_s;

static constexpr ASCIILiteral otherModuleIsLoadedByUnknownName = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/other.js" displayPath="other.js" body=f1
entry module=m0
entry module=m1
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="name" [isFreeName]
    v2 = RequireDynamic v1 referrer=m0
    Return
}
function f1 name="other" module=m1 kind=Module results=[{type=anything}] {
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

#define AOT_MAIN_OF_TWO_COMMONJS_MODULES R"IR(module m0 path="/app/main.cjs" displayPath="main.cjs" kind=CommonJs flags=[CachesEvaluationError] body=f0 requests=[{target=m1 kind=Require completes=true}]
module m1 path="/app/other.cjs" displayPath="other.cjs" kind=CommonJs flags=[CachesEvaluationError] body=f1
entry module=m0
function f0 name="main" module=m0 )IR" AOT_COMMONJS_SIGNATURE

#define AOT_OTHER_COMMONJS_MODULE R"IR(function f1 name="other" module=m1 )IR" AOT_COMMONJS_SIGNATURE R"IR(    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = String string="other ran"
    v3 = Call v1, -, v2
    Return
}
)IR"_s

static constexpr ASCIILiteral callsRequireAsValueUnderKnownName = R"IR(module m0 path="/app/main.cjs" displayPath="main.cjs" kind=CommonJs flags=[CachesEvaluationError] body=f0
module m1 path="/app/other.cjs" displayPath="other.cjs" kind=CommonJs flags=[CachesEvaluationError] body=f1
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

static constexpr ASCIILiteral takesMainModuleAsFrontEndSays = AOT_TAKES_MAIN_MODULE_AS_VALUE("CachesEvaluationError, ReachesModuleObjects");
static constexpr ASCIILiteral takesMainModuleBehindFrontEnd = AOT_TAKES_MAIN_MODULE_AS_VALUE("CachesEvaluationError");

#define AOT_HOLDS_MOCK_OF_FILE(flagsOfVariable) R"IR(variable g0 name="exports" module=m1 declared={type=anything} initialization=Undefined
variable g1 module=m1 declared={type=anything|absent} flags=[)IR" flagsOfVariable R"IR(]
module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/mocked.cjs" displayPath="mocked.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked] requireResult=ModuleExports body=f1 exportsVariable=g0
entry module=m0
entry module=m1
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
    Return
}
function f1 name="mocked" module=m1 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Integer integer=1
    PutModuleVariable v0 variable=g0
    Return
}
)IR"_s

static constexpr ASCIILiteral holdsMockOfFile = AOT_HOLDS_MOCK_OF_FILE("IsReplaceable, HoldsMock");
static constexpr ASCIILiteral holdsMockOfFileWhoseOriginalMayRun = AOT_HOLDS_MOCK_OF_FILE("IsReplaceable, HoldsMock, OriginalMayRun");

#define AOT_LIBRARY_LOADS_BY_NAME(operation) R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/other.js" displayPath="other.js" body=f1
module m2 path="internal:load" displayPath="internal:load" flags=[IsLibrary]
entry module=m0
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
    v0 = CallDirect function=f2
    Return
}
function f1 name="other" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = String string="other ran"
    v3 = Call v1, -, v2
    Return
}
function f2 name="load" module=m2 role=Library results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="name" [isFreeName]
    Jump -> next b1()
  b1() handler=b3():
    v3 = )IR" operation R"IR( v2 referrer=m2
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

static String refusal(unsigned count)
{
    StringBuilder text;
    for (unsigned index = 0; index < count; ++index)
        text.append("EvalError\n"_s, rejectedEvaluationMessage(), '\n');
    return text.toString();
}

static void checkShellOutput(ASCIILiteral programText, const String& expectedOutput, ASCIILiteral statementsBeforeProgram, OptionWords optionWords = { }, std::source_location location = std::source_location::current())
{
    String output = outputOfProgramInShell(programText, optionWords, statementsBeforeProgram);
    checkPrintedLines(location.file_name(), static_cast<int>(location.line()), optionWordsOfReason(optionWords), output, expectedOutput);
}

static void testProgramWithoutEventIsClosed()
{
    checkImageFlags({
        { printsSum, closed },
        { otherModuleIsNeverLoaded, closed },
        { loadsNativeAddon, closed },
    });
}

static void testReachableEvaluatorLeavesTheProgramClosed()
{
    checkImageFlags({
        { evaluatorsFromUnknownCode, closed },
        { evaluatorUnderUnknownKey, closed },
    });
}

static void testUncompiledModuleOpensTheProgram()
{
    checkImageFlags({
        { importsUncompiledModule, runsModuleFromOutside, { "exclude=CodeOutsideImage"_s } },
        { importsUncompiledModule, isHardenedBuild ? runsModuleFromOutside : loadsCodeFromOutside },
    });
}

static void testOptionThatAdmitsCodeOpensTheProgram()
{
    checkImageFlags({
        { printsSum, open, { "admit=Inspector"_s, "exclude=CodeOutsideImage"_s } },
        { printsSum, open, { "admit=Inspector"_s } },
    });
}

static void testOptionThatAdmitsCodeKeepsEveryFeature()
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileForShellOnThisThread(printsSum, { "admit=Inspector"_s });
    });

    AOT_TEST_CHECK(std::ranges::contains(output->records<Wire::UnseenCodeRecord>(Wire::ResultSection::UnseenCode), Wire::UnseenCodeCause::OptionAdmitsCode, &Wire::UnseenCodeRecord::cause));

    const BuiltinTable& builtins = BuiltinTable::singleton();
    std::span<const Wire::BuiltinUseRecord> uses = output->records<Wire::BuiltinUseRecord>(Wire::ResultSection::BuiltinsUsed);
    AOT_TEST_CHECK(uses.size() == builtins.builtinCount());
    static_assert(isFlagWord<Wire::BuiltinUse>);
    constexpr auto everyUse = static_cast<Wire::BuiltinUse>(((1u << numberOfEnumerators<Wire::BuiltinUse>) - 1) & ~static_cast<uint32_t>(Wire::BuiltinUse::NamedAtRunTime));
    AOT_TEST_CHECK(std::ranges::all_of(uses, [](const Wire::BuiltinUseRecord& use) {
        return use.uses == everyUse;
    }));

    std::span<const Wire::FeatureUseRecord> features = output->records<Wire::FeatureUseRecord>(Wire::ResultSection::FeaturesUsed);
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        for (const BuiltinFeatureUse& line : builtins.featureUsesOf(BuiltinID(index))) {
            AOT_TEST_CHECK(std::ranges::any_of(features, [&](const Wire::FeatureUseRecord& use) {
                return use.feature == static_cast<uint32_t>(line.feature) && use.kind == Wire::FeatureUseKind::Named;
            }));
        }
    }

    std::span<const Wire::LocaleUseRecord> locales = output->records<Wire::LocaleUseRecord>(Wire::ResultSection::LocalesUsed);
    AOT_TEST_CHECK(locales.size() == 1 && locales[0].kind == Wire::LocaleUseKind::Unknown);
}

static void testLoadByUnknownNameOnlyExposesModules()
{
    checkImageFlags({
        { otherModuleIsLoadedByUnknownName, findsModulesByName, { "exclude=CodeOutsideImage"_s } },
        { otherModuleIsLoadedByUnknownName, isHardenedBuild ? findsModulesByName : loadsCodeFromOutside },
        { otherModuleIsLoadedByUnknownName, open, { "admit=Inspector"_s, "exclude=CodeOutsideImage"_s } },
    });
}

static void testLoaderCalledAsValueLoadsByName()
{
    checkImageFlags({
        { callsRequireAsValueUnderKnownName, findsModulesByName, { "exclude=CodeOutsideImage"_s } },
        { callsRequireAsValueUnderKnownName, isHardenedBuild ? findsModulesByName : loadsCodeFromOutside },
    });
    checkShellOutput(callsRequireAsValueUnderKnownName, "other ran\n"_s, ""_s);
}

static void testLoadByNameInLibraryFunctionLeavesTheProgramClosed()
{
    checkImageFlags({
        { libraryRequiresUnknownName, closed },
        { libraryImportsUnknownName, closed },
    });
}

static void testWhatIsDoneToModuleCacheLeavesTheProgramClosed()
{
    checkImageFlags({
        { requiresOtherModule, closed },
        { readsModuleCache, closed },
        { removesFromModuleCache, closed },
        { storesIntoModuleCache, closed },
    });
}

static void testFrontEndAloneSaysWhetherWaysBetweenModuleObjectsAreShown()
{
    checkImageFlags({
        { takesMainModuleAsFrontEndSays, (isHardenedBuild ? findsModulesByName : loadsCodeFromOutside) | showsWaysBetweenModuleObjects },
        { takesMainModuleBehindFrontEnd, isHardenedBuild ? findsModulesByName : loadsCodeFromOutside },
        { takesMainModuleAsFrontEndSays, open | showsWaysBetweenModuleObjects, { "admit=Inspector"_s, "exclude=CodeOutsideImage"_s } },
    });
}

static void testObfuscationIsAnotherFlag()
{
    checkImageFlags({ { printsSum, closed | Flags { ImageFlag::IsObfuscated }, { "obfuscate"_s } } });
}

static constexpr ASCIILiteral testRunWord = "admit=ReplacedBuiltins"_s;

static void testExecutableRefusesOriginalsUnlessVariableSaysThatOneMayRun()
{
    checkImageFlags({
        { printsSum, refusesOriginals },
        { holdsMockOfFile, refusesOriginals },
        { holdsMockOfFileWhoseOriginalMayRun, Flags { } },
    });
}

static void testTestRunRefusesOriginalsOnlyBesideMockOfFile()
{
    checkImageFlags({
        { printsSum, Flags { }, { testRunWord } },
        { holdsMockOfFile, refusesOriginals, { testRunWord } },
        { holdsMockOfFileWhoseOriginalMayRun, Flags { }, { testRunWord } },
    });
}

static void testClosedProgramRefusesEveryEvaluator()
{
    checkShellOutput(evaluatorsFromUnknownCode, refusal(5), everyEvaluator);
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

static void testEvaluatorThatComesBackFromUnknownCodeRuns()
{
    checkShellOutput(evaluatorComesBackFromUnknownCode, "number\n"_s, "globalThis.text = '1 + 1'; globalThis.keep = value => { globalThis.kept = value; };"_s);
}

static void testEveryRealmOfClosedProgramRefusesEvaluation()
{
    static constexpr ASCIILiteral evaluateInEveryRealm = "globalThis.one = 1; for (const realm of [globalThis, createGlobalObject()]) { for (const evaluate of [realm.eval, realm.Function, realm.eval('(async function () { }).constructor')]) { try { print(typeof evaluate('1 + 1')); } catch (error) { print(error.name); print(error.message); } } }"_s;
    static constexpr ASCIILiteral evaluateWithoutNestedEvaluation = "globalThis.one = 1; for (const realm of [globalThis, createGlobalObject()]) { for (const evaluate of [realm.eval, realm.Function]) { try { print(typeof evaluate('1 + 1')); } catch (error) { print(error.name); print(error.message); } } }"_s;
    checkShellOutput(printsSum, makeString(refusal(4), "2\n"_s), evaluateWithoutNestedEvaluation);
    checkShellOutput(printsSum, "number\nfunction\nfunction\nnumber\nfunction\nfunction\n2\n"_s, evaluateInEveryRealm, { "admit=Inspector"_s });
}

#define AOT_DECLARATION_REFUSED(name) "SyntaxError: Cannot declare '" name "': compiled code treats the global '" name "' as a constant, so a script can't shadow it with a top-level let, const or class (bun compile differs from bun run here: https://bun.com/docs/compiler/compatibility)\n"

static void testScriptShadowsNoConstantOfCompiledCode()
{
    static constexpr ASCIILiteral declareInScripts = "globalThis.errorOf = script => { try { loadString(script); return null; } catch (error) { return error; } };"
        " globalThis.declare = script => { const error = errorOf(script); if (error) print(error); };"
        " globalThis.text = `declare('let Math = { max: () => \"of the script\" }'); declare('const Math = 1'); declare('class Math { }'); declare('let globalThis = 1');"
        " globalThis.isParseIntDeclared = !errorOf('let parseInt = () => \"of the script\"'); declare('let declaredByScript = \"of the script\"'); declare('print(declaredByScript)');`;"
        " globalThis.reportParsed = value => print(value === (isParseIntDeclared ? 'of the script' : 12) ? 'as declared' : 'not as declared');"_s;
    static constexpr ASCIILiteral expectedOutput = AOT_DECLARATION_REFUSED("Math") AOT_DECLARATION_REFUSED("Math") AOT_DECLARATION_REFUSED("Math") AOT_DECLARATION_REFUSED("globalThis") "of the script\n2\n2\nas declared\nof the script\nundefined\n"_s;
    checkShellOutput(readsGlobalsAfterScripts, expectedOutput, declareInScripts);
    checkShellOutput(readsGlobalsAfterScripts, expectedOutput, declareInScripts, { "admit=Inspector"_s });
}

#define AOT_LOAD_BY_NAME_REFUSED "Cannot load '/other.js': this executable can't load a module from a dynamic path. Pass the file to bun compile as another entry point, or build with --aot-allow-code-from-disk. (bun compile differs from bun run here: https://bun.com/docs/compiler/compatibility)\n"

#define AOT_COMPILED_FILE_REFUSED "Cannot load '/other.js': it was compiled into this executable, and a dynamic path finds a compiled file only if the path is a string somewhere in your code. Pass the file to bun compile as another entry point. (bun compile differs from bun run here: https://bun.com/docs/compiler/compatibility)\n"
#define AOT_LOADER_CHANGE_REFUSED ": imports in this executable were resolved at build time, so the module loader can't be patched. (bun compile differs from bun run here: https://bun.com/docs/compiler/compatibility)\n"

static constexpr ASCIILiteral runOtherModule = "globalThis.name = '/other.js'; try { runCompiledEntryPoint(2); drainMicrotasks(); } catch (error) { print(error.message); }"_s;

static void testClosedProgramFindsNoModuleByName()
{
    checkShellOutput(otherModuleIsNeverLoaded, AOT_LOAD_BY_NAME_REFUSED ""_s, runOtherModule);
}

static void testEvaluatedTextFindsNoModuleByName()
{
    static constexpr ASCIILiteral textThatLoads = "globalThis.text = `for (const change of [() => runCompiledEntryPoint(2), () => overrideRequire(true)]) { try { change(); print('done'); } catch (error) { print(error.message); } }`;"_s;
    checkShellOutput(evaluatesTextBesideOtherModule, isHardenedBuild ? AOT_LOAD_BY_NAME_REFUSED "done\n"_s : AOT_COMPILED_FILE_REFUSED "done\n"_s, textThatLoads);
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

static void testEveryImageTakesHookAndRefusesEntryOfCache()
{
    static constexpr ASCIILiteral refusals = "changed\nCannot replace '/app/other.cjs' in require.cache" AOT_LOADER_CHANGE_REFUSED "other ran\n"_s;
    checkShellOutput(requiresOtherModule, refusals, changeLoader);
    checkShellOutput(otherModuleIsLoadedByUnknownName, "other ran\n"_s, "globalThis.name = '/other.js'; try { overrideRequire(true); } catch (error) { print(error.message); }"_s);
    checkShellOutput(requiresOtherModule, refusals, changeLoader, { "admit=Inspector"_s });
}

std::span<const NativeTest> closedWorldTests()
{
    static constexpr NativeTest tests[] {
        { "closed-world/flags/program-without-event-is-closed"_s, testProgramWithoutEventIsClosed },
        { "closed-world/flags/reachable-evaluator-leaves-the-program-closed"_s, testReachableEvaluatorLeavesTheProgramClosed },
        { "closed-world/flags/uncompiled-module-opens-the-program"_s, testUncompiledModuleOpensTheProgram },
        { "closed-world/flags/option-that-admits-code-opens-the-program"_s, testOptionThatAdmitsCodeOpensTheProgram },
        { "closed-world/flags/load-by-unknown-name-only-exposes-modules"_s, testLoadByUnknownNameOnlyExposesModules },
        { "closed-world/flags/loader-called-as-value-loads-by-name"_s, testLoaderCalledAsValueLoadsByName },
        { "closed-world/flags/load-by-name-in-library-function-leaves-the-program-closed"_s, testLoadByNameInLibraryFunctionLeavesTheProgramClosed },
        { "closed-world/flags/what-is-done-to-module-cache-leaves-the-program-closed"_s, testWhatIsDoneToModuleCacheLeavesTheProgramClosed },
        { "closed-world/flags/front-end-alone-says-whether-ways-between-module-objects-are-shown"_s, testFrontEndAloneSaysWhetherWaysBetweenModuleObjectsAreShown },
        { "closed-world/flags/obfuscation-is-another-flag"_s, testObfuscationIsAnotherFlag },
        { "closed-world/flags/executable-refuses-originals-unless-a-variable-says-that-one-may-run"_s, testExecutableRefusesOriginalsUnlessVariableSaysThatOneMayRun },
        { "closed-world/flags/test-run-refuses-originals-only-beside-a-mock-of-a-file"_s, testTestRunRefusesOriginalsOnlyBesideMockOfFile },
        { "closed-world/features/option-that-admits-code-keeps-every-feature"_s, testOptionThatAdmitsCodeKeepsEveryFeature },
        { "closed-world/evaluation/closed-program-refuses-every-evaluator"_s, testClosedProgramRefusesEveryEvaluator },
        { "closed-world/evaluation/program-that-evaluates-text-keeps-every-evaluator"_s, testProgramThatEvaluatesTextKeepsEveryEvaluator },
        { "closed-world/evaluation/option-keeps-every-evaluator"_s, testOptionKeepsEveryEvaluator },
        { "closed-world/evaluation/evaluator-under-unknown-key-is-refused"_s, testEvaluatorUnderUnknownKeyIsRefused },
        { "closed-world/evaluation/evaluator-that-comes-back-from-unknown-code-runs"_s, testEvaluatorThatComesBackFromUnknownCodeRuns },
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
