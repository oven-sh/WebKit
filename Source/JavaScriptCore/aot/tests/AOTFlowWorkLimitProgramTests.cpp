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
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

static constexpr ASCIILiteral textBeforeRepeatedReads = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
expectedresult x0 type=L6 layouts=[L6]
layout L0 name="Box" fields=[{key="x" declared={type=anything} flags=[IsRequired]}, {key="y" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Parent" fields=[{key="z" declared={type=anything} flags=[IsRequired]}]
layout L2 name="Descriptor" fields=[{key="get" declared={type=anything} flags=[IsRequired]}]
layout L3 name="Source" fields=[{key="x" declared={type=anything} flags=[IsRequired]}]
layout L4 name="Holder" fields=[{key="inner" declared={type=anything} flags=[IsRequired]}]
layout L5 kind=Instance name="Made" constructor=f6 flags=[IsConstructorFunction]
layout L6 name="Parsed" fields=[{key="n" declared={type=number} flags=[IsRequired]}]
layout L7 kind=Environment fields=[{key="n" declared={type=anything}}]
layout L8 kind=Instance name="Class" constructor=f10
variable g0 name="reparented" module=m0 declared={type=anything} initialization=Undefined
variable g1 name="redefined" module=m0 declared={type=anything} initialization=Undefined
variable g2 name="frozen" module=m0 declared={type=anything} initialization=Undefined
variable g3 name="assigned" module=m0 declared={type=anything} initialization=Undefined
variable g4 name="bound" module=m0 declared={type=anything} initialization=Undefined
variable g5 name="list" module=m0 declared={type=anything} initialization=Undefined
variable g6 name="reflected" module=m0 declared={type=anything} initialization=Undefined
variable g7 name="keyed" module=m0 declared={type=anything} initialization=Undefined
variable g8 name="holder" module=m0 declared={type=anything} initialization=Undefined
variable g9 name="deleted" module=m0 declared={type=anything} initialization=Undefined
variable g10 name="symbol" module=m0 declared={type=anything} initialization=Undefined
variable g11 name="parsed" module=m0 declared={type=anything} initialization=Undefined
variable g12 name="created" module=m0 declared={type=anything} initialization=Undefined
variable g13 name="keys" module=m0 declared={type=anything} initialization=Undefined
variable g14 name="class" module=m0 declared={type=anything} initialization=Undefined
variable g15 name="closure" module=m0 declared={type=anything} initialization=Undefined
variable g16 name="stored" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = Integer integer=1
    v3 = Integer integer=2
    v4 = NewObject -, v2, v3 layout=L0 fields=[0, 1]
    PutModuleVariable v4 variable=g0
    v6 = NewObject -, v2, v3 layout=L0 fields=[0, 1]
    PutModuleVariable v6 variable=g1
    v8 = NewObject -, v2, v3 layout=L0 fields=[0, 1]
    PutModuleVariable v8 variable=g2
    v10 = NewObject -, v2, v3 layout=L0 fields=[0, 1]
    PutModuleVariable v10 variable=g3
    v12 = NewObject -, v2, v3 layout=L0 fields=[0, 1]
    PutModuleVariable v12 variable=g4
    v14 = NewArray v2 contents=anything holes=[]
    PutModuleVariable v14 variable=g5
    v16 = NewObject -, v2, v3 layout=L0 fields=[0, 1]
    PutModuleVariable v16 variable=g6
    v18 = NewObject -, v2, v3 layout=L0 fields=[0, 1]
    PutModuleVariable v18 variable=g7
    v20 = NewObject -, v2, v3 layout=L0 fields=[0, 1]
    v21 = NewObject -, v20 layout=L4 fields=[0]
    PutModuleVariable v21 variable=g8
    v23 = NewObject -, v2, v3 layout=L0 fields=[0, 1]
    PutModuleVariable v23 variable=g9
    v25 = String string="main"
    PutModuleVariable v25 variable=g16
    v27 = CallDirect v1, v4 function=f1
    v28 = CallDirect v1 function=f2
    Return
}
function f1 name="busy" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Builtin builtin=ObjectGetPrototypeOf
    v3 = Call v2, -, v1
    v4 = Builtin builtin=ObjectPrototype
    v5 = StrictEq v3, v4
    v6 = Call v0, -, v5
    v7 = NewClosure function=f3
    v8 = Call v7, -, v1
    v9 = Call v0, -, v8
    v10 = PropGet v7 key="bind"
    v11 = Call v10, v7, v1 calleeKey="bind"
    v12 = Call v11, -, v1
    v13 = Call v0, -, v12
    v14 = NewClosure function=f4
    PropSet v1, v14 key="method"
    v16 = PropGet v1 key="method"
    v17 = Call v16, v1 calleeKey="method"
    v18 = Call v0, -, v17
    v19 = NewClosure function=f6
    v20 = Construct v19
    v21 = PropGet v20 key="made"
    v22 = Call v0, -, v21
    v23 = NewClosure function=f5
    v24 = Integer integer=7
    v25 = NewObject -, v24 layout=L1 fields=[0]
    DefineGetter v25, v23 key="got"
    v27 = FinishObject v25
    v28 = PropGet v27 key="got"
    v29 = Call v0, -, v28
    v30 = NewObject -, v24 layout=L1 fields=[0]
    v31 = Builtin builtin=ObjectSetPrototypeOf
    v32 = Call v31, -, v1, v30
    v33 = GetModuleVariable variable=g1
    v34 = NewClosure function=f5
    v35 = NewObject -, v34 layout=L2 fields=[0]
    v36 = Builtin builtin=ObjectDefineProperty
    v37 = String string="x"
    v38 = Call v36, -, v33, v37, v35
    v39 = GetModuleVariable variable=g2
    v40 = Builtin builtin=ObjectFreeze
    v41 = Call v40, -, v39
    v42 = GetModuleVariable variable=g3
    v43 = Integer integer=9
    v44 = NewObject -, v43 layout=L3 fields=[0]
    v45 = Builtin builtin=ObjectAssign
    v46 = Call v45, -, v42, v44
    v47 = GetModuleVariable variable=g4
    v48 = NewClosure function=f7
    v49 = PropGet v48 key="bind"
    v50 = Call v49, v48, v47 calleeKey="bind"
    v51 = Call v50
    v52 = GetModuleVariable variable=g5
    v53 = PropGet v52 key="push"
    v54 = Integer integer=5
    v55 = Call v53, v52, v54 calleeKey="push"
    v56 = GetModuleVariable variable=g6
    v57 = Builtin builtin=ReflectSet
    v58 = Integer integer=13
    v59 = Call v57, -, v56, v37, v58
    v60 = GetModuleVariable variable=g7
    v61 = Builtin builtin=Symbol
    v62 = Call v61, -, v37
    PutModuleVariable v62 variable=g10
    ElemSet v60, v62, v54
    v65 = GetModuleVariable variable=g8
    v66 = PropGet v65 key="inner"
    v67 = Call v40, -, v66
    v68 = GetModuleVariable variable=g9
    v69 = PropDelete v68 key="x"
    v70 = String string="added"
    PropSet v68, v70 key="extra"
    v72 = Builtin builtin=JSONParse
    v73 = String string="{\"n\":4}"
    v74 = Call v72, -, v73 expectedResult=x0
    PutModuleVariable v74 variable=g11
    v76 = Builtin builtin=ObjectCreate
    v77 = Call v76, -, v39
    PutModuleVariable v77 variable=g12
    v79 = Builtin builtin=ObjectKeys
    v80 = Call v79, -, v39
    PutModuleVariable v80 variable=g13
    v82 = NewClosure function=f10
    v83 = NewClass v82 layout=L8
    v84 = NewClosure function=f9
    DefineMethod v83, v84 layout=L8 key="m"
    v86 = FinishClass v83 layout=L8
    PutModuleVariable v86 variable=g14
    v88 = NewObject -, v54 layout=L7 fields=[0]
    v89 = NewClosure v88 function=f8
    PutModuleVariable v89 variable=g15
    v91 = Integer integer=8
    PutEnvironmentField v88, v91 layout=L7 field=0
    v93 = String string="busy"
    PutModuleVariable v93 variable=g16
)IR"_s;

static constexpr ASCIILiteral textAfterRepeatedReads = R"IR(    Return
}
function f2 name="idle" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="z"
    v3 = Call v0, -, v2
    v4 = GetModuleVariable variable=g1
    v5 = PropGet v4 key="x"
    v6 = Call v0, -, v5
    v7 = GetModuleVariable variable=g2
    v8 = Integer integer=5
    PropSet v7, v8 key="x" mode=Sloppy
    v10 = PropGet v7 key="x"
    v11 = Call v0, -, v10
    v12 = GetModuleVariable variable=g3
    v13 = PropGet v12 key="x"
    v14 = Call v0, -, v13
    v15 = GetModuleVariable variable=g4
    v16 = PropGet v15 key="x"
    v17 = Call v0, -, v16
    v18 = GetModuleVariable variable=g5
    v19 = PropGet v18 key="length"
    v20 = Call v0, -, v19
    v21 = Integer integer=1
    v22 = ElemGet v18, v21
    v23 = Call v0, -, v22
    v24 = GetModuleVariable variable=g6
    v25 = PropGet v24 key="x"
    v26 = Call v0, -, v25
    v27 = GetModuleVariable variable=g7
    v28 = GetModuleVariable variable=g10
    v29 = ElemGet v27, v28
    v30 = Call v0, -, v29
    v31 = GetModuleVariable variable=g8
    v32 = PropGet v31 key="inner"
    PropSet v32, v8 key="y" mode=Sloppy
    v34 = PropGet v32 key="y"
    v35 = Call v0, -, v34
    v36 = GetModuleVariable variable=g9
    v37 = PropGet v36 key="x"
    v38 = Call v0, -, v37
    v39 = PropGet v36 key="extra"
    v40 = Call v0, -, v39
    v41 = GetModuleVariable variable=g11
    v42 = PropGet v41 key="n"
    v43 = Call v0, -, v42
    v44 = GetModuleVariable variable=g12
    v45 = PropGet v44 key="y"
    v46 = Call v0, -, v45
    v47 = GetModuleVariable variable=g13
    v48 = ElemGet v47, v21
    v49 = Call v0, -, v48
    v50 = GetModuleVariable variable=g14
    v51 = Construct v50
    v52 = PropGet v51 key="m"
    v53 = Call v52, v51 calleeKey="m"
    v54 = Call v0, -, v53
    v55 = InstanceOf v51, v50
    v56 = Call v0, -, v55
    v57 = GetModuleVariable variable=g15
    v58 = Call v57
    v59 = Call v0, -, v58
    v60 = GetModuleVariable variable=g16
    v61 = Call v0, -, v60
    Return
}
function f3 name="closure" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="closure"
    Return v1
}
function f4 name="method" flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="y"
    Return v1
}
function f5 name="getter" kind=Getter flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="getter"
    Return v1
}
function f6 name="Made" flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="made"
    PropSet v0, v1 key="made"
    Return
}
function f7 name="setter" flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=11
    PropSet v0, v1 key="x"
    Return
}
function f8 name="inner" kind=Arrow captures=[{type=env L7}] results=[{type=anything}] {
  b0(v0: env L7):
    v1 = GetEnvironmentField v0 layout=L7 field=0
    Return v1
}
function f9 name="m" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="method of class"
    Return v1
}
function f10 name="Class" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L8
    v2 = NewObject v1 layout=L8 fields=[]
    Return v2
}
)IR"_s;

static constexpr unsigned firstRepeatedRead = 95;
static constexpr unsigned numberOfRepeatedReads = 300;

static constexpr ASCIILiteral script = R"js(
globalThis.report = value => print(String(value));
)js"_s;

static constexpr ASCIILiteral expectedOutput = R"(true
closure
closure
2
made
getter
7
getter
1
9
11
2
5
13
5
2
undefined
added
4
2
y
method of class
true
8
busy
)"_s;

static String makeProgramText()
{
    StringBuilder text;
    text.append(textBeforeRepeatedReads);
    for (unsigned index = 0; index < numberOfRepeatedReads; ++index)
        text.append("    v"_s, firstRepeatedRead + index, " = PropGet v1 key=\"x\"\n"_s);
    text.append(textAfterRepeatedReads);
    return text.toString();
}

static std::unique_ptr<CompileOutput> compileOnThisThread(std::initializer_list<ASCIILiteral> optionWords)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(makeProgramText()), Typed::IRForm::AsRead, build);
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

static String runInShell(std::initializer_list<ASCIILiteral> optionWords)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileOnThisThread(optionWords);
    });

    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(script, "runCompiledEntryPoint("_s, output->entryModuleIDs[0], ").catch(error => print(\"the program threw \" + error));"_s),
    };

    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

static void checkAnswers(std::initializer_list<ASCIILiteral> optionWords)
{
    String output = runInShell(optionWords);
    if (output != expectedOutput)
        dataLog(output);
    AOT_TEST_CHECK(output == expectedOutput);
}

static void testWithinLimit()
{
    checkAnswers({ "disable=flow:ConstantBuiltinLocations"_s });
}

static void testLargeSetsAreGivenUp()
{
    checkAnswers({ "disable=flow:ConstantBuiltinLocations"_s, "limit=MaxValueFlowSetMembers:1"_s });
    checkAnswers({ "disable=flow:ConstantBuiltinLocations"_s, "limit=MaxValueFlowSetMembers:1"_s, "validate"_s });
}

static void testEdgeBudgetIsUsedUp()
{
    checkAnswers({ "disable=flow:ConstantBuiltinLocations"_s, "limit=MaxValueFlowEdgesLogged:1"_s });
    checkAnswers({ "disable=flow:ConstantBuiltinLocations"_s, "limit=MaxValueFlowEdgesLogged:200"_s });
    checkAnswers({ "disable=flow:ConstantBuiltinLocations"_s, "limit=MaxValueFlowEdgesLogged:1"_s, "validate"_s });
}

static void testWithoutPassesThatReadStamps()
{
    checkAnswers({ "disable=flow:ConstantBuiltinLocations,pass:ResolveCalls"_s, "limit=MaxValueFlowEdgesLogged:200"_s });
    checkAnswers({ "disable=flow:ConstantBuiltinLocations,pass:PlaceTypeTests"_s, "limit=MaxValueFlowEdgesLogged:200"_s });
    checkAnswers({ "disable=flow:ConstantBuiltinLocations,pass:ResolveAccesses"_s, "limit=MaxValueFlowEdgesLogged:200"_s });
    checkAnswers({ "disable=flow:ConstantBuiltinLocations,pass:ComputeEffectSummaries"_s, "limit=MaxValueFlowEdgesLogged:200"_s });
}

std::span<const NativeTest> flowWorkLimitProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "flow-work-limit-program/within-limit"_s, testWithinLimit },
        { "flow-work-limit-program/large-sets-are-given-up"_s, testLargeSetsAreGivenUp },
        { "flow-work-limit-program/edge-budget-is-used-up"_s, testEdgeBudgetIsUsedUp },
        { "flow-work-limit-program/without-passes-that-read-stamps"_s, testWithoutPassesThatReadStamps },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
