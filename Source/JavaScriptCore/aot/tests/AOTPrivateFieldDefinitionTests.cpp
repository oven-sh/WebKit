/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include <initializer_list>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

// class C { #a = <initializer of a>; #b = 2; #m() { <body of m> } read() { return { a: this.#a, b: this.#b } } }; show("read()", new C().read())
static constexpr ASCIILiteral layouts = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="C" constructor=f1 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}, {key=#"a"@L0 declared={type=anything} flags=[IsRequired, IsPrivate]}, {key=#"b"@L0 declared={type=number} flags=[IsRequired, IsPrivate]}]
layout L1 name="Answer" fields=[{key="a" declared={type=anything} flags=[IsRequired]}, {key="b" declared={type=anything} flags=[IsRequired]}]
)IR"_s;

static constexpr ASCIILiteral evaluatedOnce = R"IR(function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = CallDirect function=f5
    Return
}
)IR"_s;

static constexpr ASCIILiteral evaluatedTwice = R"IR(function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = CallDirect function=f5
    v1 = CallDirect function=f5
    Return
}
)IR"_s;

static constexpr ASCIILiteral constructor = R"IR(function f1 name="C" kind=BaseConstructor flags=[HasNewTarget] captures=[{type=classtoken}] results=[{type=anything}] {
  b0(v0: classtoken, v1: anything):
    v2 = GetConstructPrototype v1 layout=L0
    v3 = NewObject v2 layout=L0
    PrivateDefine v3, v0, v0 layout=L0
    v5 = CallDirect v3, v0 function=f2
    Return v3
}
function f2 name="initialize" kind=Initializer flags=[HasThis] thisType={type=L0} parameters=[{type=classtoken}] results=[{type=anything}] {
  b0(v0: object ? L0, v1: classtoken):
)IR"_s;

static constexpr ASCIILiteral definitions = R"IR(    PrivateDefine v0, v1, v2 layout=L0 field=1
    v4 = Integer integer=2
    PrivateDefine v0, v1, v4 layout=L0 field=2
)IR"_s;

static constexpr ASCIILiteral definitionsAfterStore = R"IR(    PrivateDefine v0, v1, v2 layout=L0 field=1
    v5 = Integer integer=2
    PrivateDefine v0, v1, v5 layout=L0 field=2
)IR"_s;

static constexpr ASCIILiteral methods = R"IR(    Return
}
function f3 name="read" kind=Method flags=[HasThis] captures=[{type=classtoken}] thisType={type=L0} results=[{type=anything}] {
  b0(v0: classtoken, v1: object ? L0):
    v2 = PrivateGet v1, v0 layout=L0 field=1
    v3 = PrivateGet v1, v0 layout=L0 field=2
    v4 = NewObject -, v2, v3 layout=L1 fields=[0, 1]
    v5 = FinishObject v4
    Return v5
}
function f4 name="m" kind=Method flags=[HasThis] thisType={type=L0} parameters=[{type=classtoken}] results=[{type=anything}] {
  b0(v0: object ? L0, v1: classtoken):
)IR"_s;

static constexpr ASCIILiteral evaluation = R"IR(}
function f5 name="evaluate" results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="show" [isFreeName]
    v2 = NewClassToken layout=L0
    v3 = NewClosure v2 function=f1
    v4 = NewClass v3 layout=L0
    v5 = NewClosure v2 function=f3
    DefineMethod v4, v5 layout=L0 key="read"
    v7 = FinishClass v4 layout=L0
    v8 = Construct v7
    v9 = PropGet v8 key="read"
    v10 = Call v9, v8 calleeKey="read"
    v11 = String string="read()"
    v12 = Call v1, -, v11, v10
    Return
}
)IR"_s;

static constexpr ASCIILiteral one = "    v2 = Integer integer=1\n"_s;
static constexpr ASCIILiteral callOfMethod = "    v2 = CallDirect v0, v1 function=f4\n"_s;
static constexpr ASCIILiteral methodReturningFive = "    v2 = Integer integer=5\n    Return v2\n"_s;

static constexpr ASCIILiteral script = R"js(
globalThis.show = (name, object) => print(name + ": " + JSON.stringify(object));
)js"_s;

struct PrivateFieldProgram {
    ASCIILiteral main { evaluatedOnce };
    ASCIILiteral initializerOfA { one };
    ASCIILiteral definitions { JSC::AOT::definitions };
    ASCIILiteral bodyOfMethod { methodReturningFive };
};

static std::unique_ptr<CompileOutput> compileOnThisThread(const PrivateFieldProgram& parts, std::initializer_list<ASCIILiteral> optionWords)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    BuildContext build(options);
    String text = makeString(layouts, parts.main, constructor, parts.initializerOfA, parts.definitions, methods, parts.bodyOfMethod, evaluation);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(WTF::move(text)), Typed::IRForm::AsRead, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(program);

    std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
    if (output->image.isEmpty())
        Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
    AOT_TEST_CHECK(!output->image.isEmpty());
    AOT_TEST_CHECK(output->entryModuleIDs.size() == 1);
    return output;
}

static String runInShell(const PrivateFieldProgram& parts, std::initializer_list<ASCIILiteral> optionWords)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileOnThisThread(parts, optionWords);
    });

    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(script, "runCompiledEntryPoint("_s, output->entryModuleIDs[0], "); drainMicrotasks();"_s),
    };

    // The shell prints what the program throws, and the stack below it.
    ChildProcessResult result = ChildProcessLauncher(shellPath(), { }).run(arguments.span());
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

static void checkUnderEveryOptionSet(const PrivateFieldProgram& parts, ASCIILiteral expectedOutput)
{
    auto check = [&](std::initializer_list<ASCIILiteral> optionWords) {
        String output = runInShell(parts, optionWords);
        if (!output.startsWith(expectedOutput))
            dataLog(output);
        AOT_TEST_CHECK(output.startsWith(expectedOutput));
    };
    check({ });
    check({ "disable=pass:InlineCalls"_s });
    check({ "only-passes=ResolveAccesses"_s });
    check({ "only-passes=AnalyzeFlow"_s });
}

static constexpr ASCIILiteral missingFieldError = "Exception: TypeError: Cannot access invalid private field\n"_s;

static void testDefinitionsInOrder()
{
    checkUnderEveryOptionSet({ }, "read(): {\"a\":1,\"b\":2}\n"_s);
    checkUnderEveryOptionSet({ .initializerOfA = callOfMethod }, "read(): {\"a\":5,\"b\":2}\n"_s);
}

// #a = (this.#b = 1)
static void testStoreBeforeDefinition()
{
    checkUnderEveryOptionSet({ .initializerOfA = "    v2 = Integer integer=1\n    PrivateSet v0, v1, v2 layout=L0 field=2\n"_s, .definitions = definitionsAfterStore }, missingFieldError);
    checkUnderEveryOptionSet({ .initializerOfA = callOfMethod, .bodyOfMethod = "    v2 = Integer integer=5\n    PrivateSet v0, v1, v2 layout=L0 field=2\n    Return v2\n"_s }, missingFieldError);
}

// #a = this.#b
static void testReadBeforeDefinition()
{
    checkUnderEveryOptionSet({ .initializerOfA = "    v2 = PrivateGet v0, v1 layout=L0 field=2\n"_s }, missingFieldError);
    checkUnderEveryOptionSet({ .initializerOfA = callOfMethod, .bodyOfMethod = "    v2 = PrivateGet v0, v1 layout=L0 field=2\n    Return v2\n"_s }, missingFieldError);
}

// #a = (#b in this)
static void testMembershipBeforeDefinition()
{
    checkUnderEveryOptionSet({ .initializerOfA = "    v2 = PrivateHas v0, v1 layout=L0 field=2\n"_s }, "read(): {\"a\":false,\"b\":2}\n"_s);
}

static void testClassEvaluatedTwice()
{
    checkUnderEveryOptionSet({ .main = evaluatedTwice }, "read(): {\"a\":1,\"b\":2}\nread(): {\"a\":1,\"b\":2}\n"_s);
}

std::span<const NativeTest> privateFieldDefinitionTests()
{
    static constexpr NativeTest tests[] = {
        { "private-field-definition/in-order"_s, testDefinitionsInOrder },
        { "private-field-definition/store-before-definition"_s, testStoreBeforeDefinition },
        { "private-field-definition/read-before-definition"_s, testReadBeforeDefinition },
        { "private-field-definition/membership-before-definition"_s, testMembershipBeforeDefinition },
        { "private-field-definition/class-evaluated-twice"_s, testClassEvaluatedTwice },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
