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
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programText = R"IR(layout L0 kind=Instance name="Point" module=m0 constructor=f1 fields=[{key="x" declared={type=anything}}, {key="y" declared={type=anything}}]
layout L1 kind=Instance name="Unlisted" module=m0 constructor=f3
layout L2 kind=Instance name="UnlistedWithInstance" module=m0 constructor=f5
layout L3 name="Pair" module=m0 fields=[{key="a" declared={type=anything} flags=[IsRequired]}, {key="b" declared={type=anything} flags=[IsRequired]}]
module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="registry" [isFreeName]
    v2 = Integer integer=1
    v3 = String string="two"
    v4 = NewClosure function=f1
    v5 = NewClass v4 layout=L0
    v6 = NewClosure function=f2
    DefineMethod v5, v6 layout=L0 key="method"
    v8 = FinishClass v5 layout=L0
    v9 = Construct v8, -, v2, v3
    PropSet v1, v9 key="instance"
    v11 = NewObject -, v2, v3 layout=L3 fields=[0, 1]
    PropSet v1, v11 key="literal"
    v13 = NewClosure function=f3
    v14 = NewClass v13 layout=L1
    v15 = NewClosure function=f4
    DefineMethod v14, v15 layout=L1 key="method"
    v17 = FinishClass v14 layout=L1
    v18 = PropGet v17 key="prototype"
    PropSet v1, v18 key="unlistedPrototype"
    v20 = NewClosure function=f5
    v21 = NewClass v20 layout=L2
    v22 = NewClosure function=f6
    DefineMethod v21, v22 layout=L2 key="method"
    v24 = FinishClass v21 layout=L2
    v25 = Construct v24
    PropSet v1, v25 key="instanceOfUnlistedClass"
    v27 = NewClosure function=f7
    PropSet v1, v27 key="closure"
    v29 = NewClosure function=f8
    v30 = Call v29
    PropSet v1, v30 key="generator"
    Return
}
function f1 name="Point" kind=BaseConstructor flags=[HasNewTarget] length=2 parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    v3 = GetConstructPrototype v0 layout=L0
    v4 = NewObject v3 layout=L0
    PropSet v4, v1 key="x"
    PropSet v4, v2 key="y"
    Return v4
}
function f2 name="method" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    Return
}
function f3 name="Unlisted" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1
    Return v2
}
function f4 name="method" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    Return
}
function f5 name="UnlistedWithInstance" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L2
    v2 = NewObject v1 layout=L2
    Return v2
}
function f6 name="method" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    Return
}
function f7 name="closure" results=[{type=anything}] {
  b0():
    Return
}
function f8 name="generate" flags=[IsGenerator, TakesCallee] results=[{type=anything}] {
  b0(v0: function):
    InitialYield
    Return
}
)IR"_s;

static constexpr ASCIILiteral programNamingFunctionOfShellText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="structuredClone" [isFreeName]
    v2 = PropGet v0 key="registry" [isFreeName]
    PropSet v2, v1 key="clone"
    v4 = Integer integer=5
    v5 = Call v1, -, v4
    PropSet v2, v5 key="cloned"
    Return
}
)IR"_s;

static constexpr ASCIILiteral scriptStart = R"js(
globalThis.registry = { };
function describeClone(value)
{
    let clone;
    try {
        clone = structuredClone(value);
    } catch (error) {
        return "throws " + error;
    }
    let isPlainCopy = clone !== value && Object.getPrototypeOf(clone) === Object.prototype;
    return JSON.stringify(clone) + (isPlainCopy ? ", a plain copy" : ", not a plain copy");
}
)js"_s;

namespace {

struct TestedProgram {
    ASCIILiteral text { programText };
    Wire::AdmittedCode admittedCode { Wire::AdmittedCode::RuntimeCode };
};

} // anonymous namespace

static std::unique_ptr<CompileOutput> compileOnThisThread(const TestedProgram& tested, Wire::ReferenceWidth referenceWidth)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    options.target.referenceWidth = referenceWidth;
    options.admittedCode = tested.admittedCode;
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(tested.text)), Typed::IRForm::AsRead, build);
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

static std::unique_ptr<CompileOutput> compileProgramText(const TestedProgram& tested, Wire::ReferenceWidth referenceWidth)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileOnThisThread(tested, referenceWidth);
    });
    return output;
}

static String runInShell(const TestedProgram& tested, Wire::ReferenceWidth referenceWidth, ASCIILiteral statements)
{
    std::unique_ptr<CompileOutput> output = compileProgramText(tested, referenceWidth);
    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(scriptStart, "runCompiledEntryPoint("_s, output->entryModuleIDs[0], ").then(() => {"_s, statements, "}).catch(error => print(\"the script threw \" + error));"_s),
    };

    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

static void checkShellOutput(ASCIILiteral statements, ASCIILiteral expectedOutput, const TestedProgram& tested = { })
{
    for (Wire::ReferenceWidth referenceWidth : { Wire::ReferenceWidth::Wide, Wire::ReferenceWidth::Narrow }) {
        String output = runInShell(tested, referenceWidth, statements);
        if (output != expectedOutput)
            dataLog(output);
        AOT_TEST_CHECK(output == expectedOutput);
    }
}

static void testCompiledObjects()
{
    checkShellOutput(R"js(
    print("instance: " + describeClone(registry.instance));
    print("literal: " + describeClone(registry.literal));
    print("class prototype: " + describeClone(registry.unlistedPrototype));
    print("its own names afterwards: " + Object.getOwnPropertyNames(registry.unlistedPrototype));
    print("class prototype in a field: " + describeClone({ p: Object.getPrototypeOf(registry.instanceOfUnlistedClass) }));
)js"_s, R"(instance: {"x":1,"y":"two"}, a plain copy
literal: {"a":1,"b":"two"}, a plain copy
class prototype: {}, a plain copy
its own names afterwards: constructor,method
class prototype in a field: {"p":{}}, a plain copy
)"_s);
}

static void testClosuresAndGenerators()
{
    checkShellOutput(R"js(
    print("closure: " + describeClone(registry.closure));
    print("closure in a field: " + describeClone({ f: registry.closure }));
    print("generator: " + describeClone(registry.generator));
)js"_s, R"(closure: throws TypeError: Value could not be cloned.
closure in a field: throws TypeError: Value could not be cloned.
generator: throws TypeError: Value could not be cloned.
)"_s);
}

// The table has structuredClone as a member of the host. The shell has no host and defines a function of its own under the name, after start-up has locked what the program takes for constants.
static void testProgramThatNamesTheFunctionOfTheShell()
{
    for (Wire::AdmittedCode admittedCode : { Wire::AdmittedCode { }, Wire::AdmittedCode::RuntimeCode }) {
        checkShellOutput(R"js(
    print("in the script: " + typeof structuredClone + ", " + describeClone({ a: 1 }));
    print("in the program: " + (registry.clone === structuredClone) + ", " + registry.cloned);
)js"_s, R"(in the script: function, {"a":1}, a plain copy
in the program: true, 5
)"_s, { programNamingFunctionOfShellText, admittedCode });
    }
}

std::span<const NativeTest> structuredCloneTests()
{
    static constexpr NativeTest tests[] = {
        { "structured-clone/compiled-objects"_s, testCompiledObjects },
        { "structured-clone/closures-and-generators"_s, testClosuresAndGenerators },
        { "structured-clone/program-that-names-the-function-of-the-shell"_s, testProgramThatNamesTheFunctionOfTheShell },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
