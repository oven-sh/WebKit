/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableArrays.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <array>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static constexpr ASCIILiteral recordsOfProgram = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
expectedresult x0 type=L0 layouts=[L0] validation=[{shape=Object{layout=L0 members=[{key="id" node=1 field=0}]} type=L0}, {shape=Primitive{primitive=Number} type=number}]
expectedresult x1 type=L1 layouts=[L1] validation=[{shape=Object{layout=L1 members=[{key="id" node=1 field=0}]} type=L1}, {shape=Primitive{primitive=Number} type=number rules=[DefaultNumber{value=7}]}]
expectedresult x2 type=L2 layouts=[L2]
expectedresult x3 type=L3 layouts=[L3] validation=[{shape=Object{layout=L3 members=[{key="id" node=1 field=0}] undeclared=Rejected} type=L3}, {shape=Primitive{primitive=Number} type=number}]
expectedresult x4 type=L4 layouts=[L4] validation=[{shape=Object{layout=L4 members=[{key="id" node=1 field=0}]} type=L4}, {shape=Primitive{} type=string rules=[Template{head="id-" parts=[{placeholder=Number text="!"}]}]}]
expectedresult x5 type=L5|L6 layouts=[L5, L6] validation=[{shape=Union{arms=[1, 2]} type=L5|L6}, {shape=Object{layout=L5 members=[{key="kind" node=3 field=0}]} type=L5}, {shape=Object{layout=L6 members=[{key="kind" node=4 field=0}]} type=L6}, {shape=StringLiteral{value="a"} type=string}, {shape=StringLiteral{value="b"} type=string}]
expectedresult x6 type=L7|L8 layouts=[L7, L8] validation=[{shape=Union{arms=[1, 2]} type=L7|L8}, {shape=Object{layout=L7 members=[{key="left" node=0 field=0 isOptional=true}]} type=L7}, {shape=Object{layout=L8 members=[{key="right" node=0 field=0 isOptional=true}]} type=L8}]
expectedresult x7 type=L9 layouts=[L7, L8, L9] validation=[{shape=Object{layout=L9 members=[{key="tree" node=1 field=0}, {key="n" node=4 field=1}]} type=L9}, {shape=Union{arms=[2, 3]} type=L7|L8}, {shape=Object{layout=L7 members=[{key="left" node=1 field=0 isOptional=true}]} type=L7}, {shape=Object{layout=L8 members=[{key="right" node=1 field=0 isOptional=true}]} type=L8}, {shape=Primitive{primitive=Number} type=number}]
layout L0 name="User" fields=[{key="id" declared={type=number}}]
layout L1 name="WithDefault" fields=[{key="id" declared={type=number}}]
layout L2 name="Row" fields=[{key="id" declared={type=number}}]
layout L3 name="Strict" fields=[{key="id" declared={type=number}}]
layout L4 name="Tagged" fields=[{key="id" declared={type=string}}]
layout L5 name="KindA" fields=[{key="kind" declared={type=string}}]
layout L6 name="KindB" fields=[{key="kind" declared={type=string}}]
layout L7 name="LeftNode" fields=[{key="left" declared={type=undefined|object}}]
layout L8 name="RightNode" fields=[{key="right" declared={type=undefined|object}}]
layout L9 name="Holder" fields=[{key="tree" declared={type=object}}, {key="n" declared={type=number}}]
)IR"_s;

static constexpr ASCIILiteral functionsOfProgram = R"IR(function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="register" [isFreeName]
    v2 = NewClosure function=f2
    v3 = Call v1, -, v2
    v4 = NewClosure function=f3
    v5 = Call v1, -, v4
    v6 = NewClosure function=f4
    v7 = Call v1, -, v6
    v8 = NewClosure function=f5
    v9 = Call v1, -, v8
    v10 = NewClosure function=f6
    v11 = Call v1, -, v10
    v12 = NewClosure function=f7
    v13 = Call v1, -, v12
    v14 = NewClosure function=f8
    v15 = Call v1, -, v14
    v16 = NewClosure function=f9
    v17 = Call v1, -, v16
    v18 = NewClosure function=f10
    v19 = Call v1, -, v18
    v20 = NewClosure function=f13
    v21 = Call v1, -, v20
    v22 = NewClosure function=f14
    v23 = Call v1, -, v22
    v24 = NewClosure function=f15
    v25 = Call v1, -, v24
    v26 = NewClosure function=f16
    v27 = Call v1, -, v26
    Return
}
function f1 name="validateByPlan" role=Library parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Builtin builtin=TypeError
    v3 = String string="the value does not match the type"
    v4 = Construct v2, -, v3
    Throw v4
}
function f2 name="resultIsUsed" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BunBundleValidate
    v2 = Call v1, -, v0 expectedResult=x0
    Return v2
}
function f3 name="resultIsNotUsed" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BunBundleValidate
    v2 = Call v1, -, v0 expectedResult=x0
    v3 = String string="did not throw"
    Return v3
}
function f4 name="inRareBlock" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BunBundleValidate
    v2 = Call v1, -, v0 expectedResult=x0
    Throw v2
}
function f5 name="withDefault" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BunBundleValidate
    v2 = Call v1, -, v0 expectedResult=x1
    Return v2
}
function f6 name="validatedParse" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=JSONParse
    v2 = Call v1, -, v0 expectedResult=x0
    Return v2
}
function f7 name="typedParse" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=JSONParse
    v2 = Call v1, -, v0 expectedResult=x2
    Return v2
}
function f8 name="inTailPosition" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BunBundleValidate
    v2 = Call v1, -, v0 expectedResult=x0
    ReturnCall v2
}
function f9 name="withoutUndeclaredMembers" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BunBundleValidate
    v2 = Call v1, -, v0 expectedResult=x3
    Return v2
}
function f10 name="againstNothing" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BunBundleValidate
    v2 = Call v1, -, v0
    ReturnCall v2
}
function f11 name="validate" role=Library parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Return v0
}
function f12 name="holdsRule" role=Library parameters=[{type=anything}, {type=string flags=[IsExpected]}] results=[{type=boolean flags=[IsExpected]}] {
  b0(v0: anything, v1: anything):
    v2 = String string="id-1!"
    v3 = StrictEq v0, v2
    Return v3
}
function f13 name="withRuleTestedByHelper" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BunBundleValidate
    v2 = Call v1, -, v0 expectedResult=x4
    Return v2
}
function f14 name="toldApartByTag" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BunBundleValidate
    v2 = Call v1, -, v0 expectedResult=x5
    Return v2
}
function f15 name="interpretedAtTheRoot" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BunBundleValidate
    v2 = Call v1, -, v0 expectedResult=x6
    Return v2
}
function f16 name="interpretedBelowTheRoot" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BunBundleValidate
    v2 = Call v1, -, v0 expectedResult=x7
    Return v2
}
)IR"_s;

// The general validator of the library throws the error. f1 stands for it: the compiled validators call it with every value that they do not accept.
static String programText()
{
    static_assert(RuntimeFunctions::ValidationHoldsRule.index() < RuntimeFunctions::ValidationValidate.index());
    static_assert(RuntimeFunctions::ValidationValidate.index() < RuntimeFunctions::ValidationValidateByPlan.index());
    return makeString(recordsOfProgram,
        "runtimefunction id="_s, RuntimeFunctions::ValidationHoldsRule.index(), " function=f12\n"_s,
        "runtimefunction id="_s, RuntimeFunctions::ValidationValidate.index(), " function=f11\n"_s,
        "runtimefunction id="_s, RuntimeFunctions::ValidationValidateByPlan.index(), " function=f1\n"_s,
        functionsOfProgram);
}

static constexpr ASCIILiteral scriptStart = R"js(
var functions = { };
globalThis.register = f => { functions[f.name] = f; };
function describe(name, input)
{
    let result;
    try {
        result = functions[name](input);
    } catch (error) {
        return error instanceof Error ? "throws " + error.name + ": " + error.message : "throws the value " + JSON.stringify(error);
    }
    return JSON.stringify(result) + (result === input ? ", the argument itself" : "");
}
)js"_s;

using OptionWords = std::array<ASCIILiteral, 2>;

// The shell has no host, and an image that takes the locations of built-ins for constant asks it to lock members of the host's classes.
static constexpr OptionWords optionSets[] = {
    { "disable=flow:ConstantBuiltinLocations"_s, "references=wide"_s },
    { "disable=flow:ConstantBuiltinLocations"_s, "references=narrow"_s },
    { "disable=flow:ConstantBuiltinLocations,pass:InlineCalls"_s, "references=wide"_s },
    { "disable=flow:ConstantBuiltinLocations"_s, "validate"_s },
    { "disable=flow-results"_s, "only-passes=AnalyzeFlow"_s },
};

static std::unique_ptr<CompileOutput> compileOnThisThread(const OptionWords& optionWords)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(programText()), Typed::IRForm::AsRead, build);
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

static String runInShell(const OptionWords& optionWords, ASCIILiteral statements)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileOnThisThread(optionWords);
    });

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

static void checkShellOutput(ASCIILiteral statements, ASCIILiteral expectedOutput)
{
    for (const OptionWords& optionWords : optionSets) {
        String output = runInShell(optionWords, statements);
        if (output != expectedOutput)
            dataLog(optionWords[0], " ", optionWords[1], ":\n", output);
        AOT_TEST_CHECK(output == expectedOutput);
    }
}

static void testValueInsideTheType()
{
    checkShellOutput(R"js(
    print("member: " + describe("resultIsUsed", { id: 1 }));
    print("undeclared member: " + describe("resultIsUsed", { id: 2, more: true }));
    print("result not used: " + describe("resultIsNotUsed", { id: 1 }));
    print("rare block: " + describe("inRareBlock", { id: 1 }));
    print("tail position: " + describe("inTailPosition", { id: 1 }));
    print("no undeclared member: " + describe("withoutUndeclaredMembers", { id: 1 }));
    print("against nothing: " + describe("againstNothing", { id: "one" }));
    print("rule tested by a helper: " + describe("withRuleTestedByHelper", { id: "id-1!" }));
    print("member with a default: " + describe("withDefault", { id: 1 }));
    print("missing member with a default: " + describe("withDefault", { }));
)js"_s, R"(member: {"id":1}
undeclared member: {"id":2,"more":true}
result not used: "did not throw"
rare block: throws the value {"id":1}
tail position: {"id":1}
no undeclared member: {"id":1}
against nothing: {"id":"one"}, the argument itself
rule tested by a helper: {"id":"id-1!"}
member with a default: {"id":1}
missing member with a default: {"id":7}
)"_s);
}

static void testValueOutsideTheType()
{
    checkShellOutput(R"js(
    let names = ["resultIsUsed", "resultIsNotUsed", "inRareBlock", "inTailPosition", "withDefault", "withoutUndeclaredMembers"];
    for (let index = 0; index < names.length; ++index) {
        let name = names[index];
        print(name + ", member of another type: " + describe(name, { id: "one" }));
        print(name + ", no object: " + describe(name, 5));
    }
    print("missing member: " + describe("resultIsUsed", { }));
    print("missing member, result not used: " + describe("resultIsNotUsed", { }));
    print("undeclared member: " + describe("withoutUndeclaredMembers", { id: 2, more: true }));
    print("rule tested by a helper: " + describe("withRuleTestedByHelper", { id: "id-x!" }));
)js"_s, R"(resultIsUsed, member of another type: throws TypeError: the value does not match the type
resultIsUsed, no object: throws TypeError: the value does not match the type
resultIsNotUsed, member of another type: throws TypeError: the value does not match the type
resultIsNotUsed, no object: throws TypeError: the value does not match the type
inRareBlock, member of another type: throws TypeError: the value does not match the type
inRareBlock, no object: throws TypeError: the value does not match the type
inTailPosition, member of another type: throws TypeError: the value does not match the type
inTailPosition, no object: throws TypeError: the value does not match the type
withDefault, member of another type: throws TypeError: the value does not match the type
withDefault, no object: throws TypeError: the value does not match the type
withoutUndeclaredMembers, member of another type: throws TypeError: the value does not match the type
withoutUndeclaredMembers, no object: throws TypeError: the value does not match the type
missing member: throws TypeError: the value does not match the type
missing member, result not used: throws TypeError: the value does not match the type
undeclared member: throws TypeError: the value does not match the type
rule tested by a helper: throws TypeError: the value does not match the type
)"_s);
}

// The general validator spreads such a value, once. Here it only throws, so nothing of the value may have run.
static void testValueThatIsNotPlainData()
{
    checkShellOutput(R"js(
    let runs = 0;
    print("getter: " + describe("resultIsUsed", { get id() { runs++; return 1; } }) + ", it ran " + runs);
    let count = name => (...operands) => { runs++; return Reflect[name](...operands); };
    let handler = { ownKeys: count("ownKeys"), getOwnPropertyDescriptor: count("getOwnPropertyDescriptor"), get: count("get"), has: count("has"), getPrototypeOf: count("getPrototypeOf") };
    print("proxy: " + describe("resultIsUsed", new Proxy({ id: 1 }, handler)) + ", traps ran " + runs);
    print("proxy of an array: " + describe("resultIsUsed", new Proxy([1], handler)) + ", traps ran " + runs);
    print("getter of the prototype: " + describe("resultIsUsed", Object.create({ get id() { runs++; return 1; } })) + ", it ran " + runs);
    print("plain data with a tag: " + describe("toldApartByTag", JSON.parse('{"kind":"b"}')));
    print("getter of the tag: " + describe("toldApartByTag", { get kind() { runs++; return "b"; } }) + ", it ran " + runs);
    print("proxy with a tag: " + describe("toldApartByTag", new Proxy({ kind: "b" }, handler)) + ", traps ran " + runs);
    print("getter under a node of the library: " + describe("interpretedAtTheRoot", { get left() { runs++; return undefined; } }) + ", it ran " + runs);
    let holder = JSON.parse('{"tree":null,"n":1}');
    holder.tree = { get left() { runs++; return undefined; } };
    print("the same below plain data: " + describe("interpretedBelowTheRoot", holder) + ", it ran " + runs);
)js"_s, R"(getter: throws TypeError: the value does not match the type, it ran 0
proxy: throws TypeError: the value does not match the type, traps ran 0
proxy of an array: throws TypeError: the value does not match the type, traps ran 0
getter of the prototype: throws TypeError: the value does not match the type, it ran 0
plain data with a tag: {"kind":"b"}
getter of the tag: throws TypeError: the value does not match the type, it ran 0
proxy with a tag: throws TypeError: the value does not match the type, traps ran 0
getter under a node of the library: throws TypeError: the value does not match the type, it ran 0
the same below plain data: throws TypeError: the value does not match the type, it ran 0
)"_s);
}

static void testParsedText()
{
    checkShellOutput(R"js(
    let names = ["validatedParse", "typedParse"];
    for (let index = 0; index < names.length; ++index) {
        let name = names[index];
        print(name + ", text of the type: " + describe(name, '{"id":1}'));
        print(name + ", member of another type: " + describe(name, '{"id":"one"}'));
        print(name + ", an array: " + describe(name, '[1]'));
        print(name + ", no JSON text: " + describe(name, '{"id":1'));
    }
)js"_s, R"(validatedParse, text of the type: {"id":1}
validatedParse, member of another type: throws TypeError: the value does not match the type
validatedParse, an array: throws TypeError: the value does not match the type
validatedParse, no JSON text: throws SyntaxError: JSON Parse error: Expected '}'
typedParse, text of the type: {"id":1}
typedParse, member of another type: {"id":"one"}
typedParse, an array: [1]
typedParse, no JSON text: throws SyntaxError: JSON Parse error: Expected '}'
)"_s);
}

std::span<const NativeTest> validatedValueTests()
{
    static constexpr NativeTest tests[] = {
        { "validated-value/value-inside-the-type"_s, testValueInsideTheType },
        { "validated-value/value-outside-the-type"_s, testValueOutsideTheType },
        { "validated-value/value-that-is-not-plain-data"_s, testValueThatIsNotPlainData },
        { "validated-value/parsed-text"_s, testParsedText },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
